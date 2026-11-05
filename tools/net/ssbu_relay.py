#!/usr/bin/env python3
"""ssbu_relay.py: a relay for online rollback matches, on a machine near the players (2026-11-05).

The central lobby (ssbu_lobby.py, Oracle VM in Querétaro) pairs the players; a match that cannot go player to player went
through that VM (~80 ms from each player: ~157 ms round trip). This relay, on a machine nearer the players (the owner's
mini PC: ~45 ms from them), carries such a match's messages instead. It never sees anything but those messages.

  - Every 10 s it tells the lobby it is up: RELAY_HB (32) = unix time u64, public port u16, LAN address u32, LAN port u16,
    signed with the shared key (the first 16 bytes of HMAC-SHA256). The lobby takes the relay's public address from where it
    comes, so a home connection whose address changes needs nothing more.
  - The lobby gives both games of a match the relay's address, a match id and its tag (HMAC of the id). A game sends:
        RELAY (33): 'S' 'U' 1 33 | match id u64 | tag u64 | its lobby address u32 | kind u8 | data
    kind 0 (hello): echoed back to the sender as 'S' 'U' 1 33 | 0 u32 | 0 | data (its round trip to the relay);
    kind 1 / 2 (a rollback message / path info): sent to the other game of the same match id as
        'S' 'U' 1 33 | sender's lobby address u32 | kind | data.
    A packet whose tag is wrong is dropped: only the lobby's matches use the relay. At most 2 games per match id, the first
    two seen; a match unheard of for 120 s is forgotten. At most 300 packets per second per match id.

Run: python3 ssbu_relay.py --lobby 160.34.219.118:39500 --secret-file /etc/ssbu-relay.key [--port 39500]
The router must forward UDP --port to this machine. SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import hmac
import select
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse

PROTO, RELAY_HB, RELAY = 1, 32, 33
HB_EVERY, FORGET_AFTER, RATE_MAX = 10.0, 120.0, 300
UPNP_EVERY = 1800.0
UPNP_EXT = [0]            # the external port the router forwards (the one told to the lobby)


def tag_of(secret, match_id):
    return hmac.new(secret, b"m" + struct.pack(">Q", match_id), hashlib.sha256).digest()[:8]


def lan_address(lobby):
    """the address this machine reaches the lobby from (its LAN address behind a router)"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(lobby)
        return s.getsockname()[0]
    except OSError:
        return "0.0.0.0"
    finally:
        s.close()


def upnp_map(port, lan_ip, lease=3600, ext=None):
    """(2026-11-05) asks the router (UPnP IGD: the owner's TP-Link Archer A6 answers) to forward UDP port to this machine, so
    nobody has to open it by hand. Renewed every UPNP_EVERY seconds (a router restart forgets it). Returns a short text."""
    import re
    import urllib.request
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(b"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                 b"ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n", ("239.255.255.250", 1900))
        d, _ = s.recvfrom(4096)
    except OSError:
        return "no UPnP router answered"
    finally:
        s.close()
    m = re.search(rb"(?im)^location:\s*(\S+)", d)
    if not m:
        return "the UPnP answer named no description"
    loc = m.group(1).decode()
    try:
        xml = urllib.request.urlopen(loc, timeout=5).read().decode("utf-8", "replace")
    except OSError as e:
        return "the UPnP description could not be read (%s)" % e
    svc = None
    for st in ("urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2",
               "urn:schemas-upnp-org:service:WANPPPConnection:1"):
        i = xml.find(st)
        if i >= 0:
            c = re.search(r"<controlURL>([^<]+)</controlURL>", xml[i:])
            if c:
                svc = (st, urllib.parse.urljoin(loc, c.group(1)))
                break
    if svc is None:
        return "the router has no WANIPConnection service"
    st, url = svc
    args = ("<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>"
            "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
            "<NewPortMappingDescription>ssbu-relay</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>"
            % (ext or port, port, lan_ip, lease))
    body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:AddPortMapping xmlns:u="%s">%s'
            '</u:AddPortMapping></s:Body></s:Envelope>' % (st, args)).encode()
    req = urllib.request.Request(url, body, {"Content-Type": 'text/xml; charset="utf-8"', "SOAPAction": '"%s#AddPortMapping"' % st})
    try:
        urllib.request.urlopen(req, timeout=5).read()
    except urllib.error.HTTPError as e:
        err = e.read().decode("utf-8", "replace")
        if "<errorCode>718</errorCode>" in err and (ext or port) == port:      # (the owner's TP-Link: 718 for 39500 itself)
            return upnp_map(port, lan_ip, lease, port + 10)
        if lease and "<errorCode>718</errorCode>" not in err:   # (some routers take only permanent mappings: lease 0)
            return upnp_map(port, lan_ip, 0, ext)
        return "the router refused the forward (HTTP %d)" % e.code
    except OSError as e:
        return "the router could not be asked (%s)" % e
    UPNP_EXT[0] = ext or port
    return "UDP %d forwarded to %s:%d by the router (UPnP%s)" % (ext or port, lan_ip, port, ", lease %d s" % lease if lease else ", permanent")


class Relay:
    def __init__(self, sock, secret, verbose=False):
        self.sock, self.secret, self.verbose = sock, secret, verbose
        self.matches = {}         # match id -> {"games": {lobby ip: (addr, port)}, "seen": t, "n": packets this second, "sec": int}
        self.ok_tags = {}         # match id -> tag already checked
        self.stats = {"forwarded": 0, "hellos": 0, "bad tag": 0, "third game": 0, "rate": 0, "short": 0}

    def say(self, *what):
        print(time.strftime("%H:%M:%S"), *what, flush=True)

    def packet(self, p, at, now):
        if len(p) < 25 or p[:2] != b"SU" or p[2] != PROTO or p[3] != RELAY:
            self.stats["short"] += 1
            return
        mid, tag, src, kind = struct.unpack_from(">Q8sIB", p, 4)
        if self.ok_tags.get(mid) != tag:
            if not hmac.compare_digest(tag_of(self.secret, mid), tag):
                self.stats["bad tag"] += 1
                return
            self.ok_tags[mid] = tag
        m = self.matches.get(mid)
        if m is None:
            m = self.matches[mid] = {"games": {}, "seen": now, "n": 0, "sec": int(now)}
            if self.verbose:
                self.say("match %016x: first game %s from %s:%d" % (mid, socket.inet_ntoa(struct.pack(">I", src)), *at))
        m["seen"] = now
        if int(now) != m["sec"]:
            m["sec"], m["n"] = int(now), 0
        m["n"] += 1
        if m["n"] > RATE_MAX:
            self.stats["rate"] += 1
            return
        games = m["games"]
        if src not in games and len(games) >= 2:
            self.stats["third game"] += 1
            return
        games[src] = at
        data = p[25:]
        if kind == 0:
            self.stats["hellos"] += 1
            self.sock.sendto(b"SU" + bytes([PROTO, RELAY]) + struct.pack(">IB", 0, 0) + data[:16], at)
            return
        out = b"SU" + bytes([PROTO, RELAY]) + struct.pack(">IB", src, kind) + data
        for ip, to in games.items():
            if ip != src:
                self.sock.sendto(out, to)
                self.stats["forwarded"] += 1

    def sweep(self, now):
        for mid in [k for k, m in self.matches.items() if now - m["seen"] > FORGET_AFTER]:
            del self.matches[mid]
            self.ok_tags.pop(mid, None)

    def heartbeat(self, lobby, public_port, lan_ip, lan_port):
        body = struct.pack(">QHIH", int(time.time()), public_port, struct.unpack(">I", socket.inet_aton(lan_ip))[0], lan_port)
        self.sock.sendto(b"SU" + bytes([PROTO, RELAY_HB]) + body + hmac.new(self.secret, body, hashlib.sha256).digest()[:16], lobby)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=39500, help="UDP port (the router forwards it here; default 39500)")
    ap.add_argument("--public-port", type=int, default=0, help="the port the router forwards (default: --port)")
    ap.add_argument("--lobby", default="160.34.219.118:39500", help="the central lobby host:port")
    ap.add_argument("--secret-file", required=True, help="the key shared with the lobby (its --relay-secret)")
    ap.add_argument("--stats", type=float, default=60.0, help="seconds between statistics lines (0: none)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--upnp", action="store_true", help="ask the router (UPnP) to forward the port here, every 30 min")
    o = ap.parse_args()
    with open(o.secret_file, "rb") as f:
        secret = f.read().strip()
    if len(secret) < 16:
        sys.exit("the key in %s is too short (at least 16 characters)" % o.secret_file)
    host, _, port = o.lobby.rpartition(":")
    lobby = (socket.gethostbyname(host), int(port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((o.bind, o.port))
    relay = Relay(sock, secret, o.verbose)
    lan_ip = lan_address(lobby)
    relay.say("relay on UDP %s:%d, lobby %s:%d, LAN address %s" % (o.bind, o.port, lobby[0], lobby[1], lan_ip))
    last_hb = last_sweep = last_stats = 0.0
    last_upnp = -UPNP_EVERY
    while True:
        if o.upnp and time.monotonic() - last_upnp >= UPNP_EVERY:
            last_upnp = time.monotonic()
            said = upnp_map(o.public_port or o.port, lan_ip)
            if said != getattr(main, "upnp_said", None):
                main.upnp_said = said
                relay.say("router: " + said)
        r, _, _ = select.select([sock], [], [], 0.5)
        now = time.monotonic()
        if r:
            try:
                p, at = sock.recvfrom(2048)
            except OSError:
                continue
            if at != lobby:
                relay.packet(p, at, now)
        if now - last_hb >= HB_EVERY:
            last_hb = now
            try:
                relay.heartbeat(lobby, o.public_port or UPNP_EXT[0] or o.port, lan_ip, o.port)
            except OSError as e:
                relay.say("heartbeat to the lobby failed: %s" % e)
        if now - last_sweep >= 10:
            last_sweep = now
            relay.sweep(now)
        if o.stats and now - last_stats >= o.stats:
            last_stats = now
            relay.say("stats: %d match(es), %s" % (len(relay.matches), ", ".join("%s %d" % kv for kv in relay.stats.items())))


if __name__ == "__main__":
    main()
