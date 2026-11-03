#!/usr/bin/env python3
"""nex_server.py: our own game server for the game's Online menu (phase 1 of docs/ONLINE_MENU_RE.md).

The game (SSBU_ONLINE_MENU=1, runtime/online_menu.inc) reaches g<id>-lp1.s.n.srv.nintendo.net on TCP port 443 with
nn::ssl in the clear (no TLS: both ends are ours), asks for a WebSocket with protocol "NEX" and speaks PRUDP "lite" in
binary messages: one PRUDP packet per message, no checksum, no encryption. Over PRUDP come RMC calls (remote method
calls of the NEX protocols).

Layers (each logs what it does not know, so the next step is read from the log):
    WebSocket   HTTP upgrade, frames (the client's are masked, ours are not)
    PRUDP lite  SYN -> SYN-ACK, CONNECT -> CONNECT-ACK, DATA -> ACK + RMC, PING -> ACK, DISCONNECT -> ACK
    RMC         request {size u32, protocol u8 | 0x80, call id u32, method u32, body}; a known (protocol, method) has a
                handler; any other is answered Core::NotImplemented and its body is logged in full

Formats: the public NEX documentation (Kinnay's NintendoClients wiki); every field this file relies on that the game has
not yet confirmed is marked UNCONFIRMED.

    py -3 tools/net/nex_server.py [--bind 127.0.0.1] [--port 443] [--log nex_server.log]
"""
import argparse
import base64
import datetime
import hashlib
import hmac
import os
import socket
import struct
import threading
import time

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# PRUDP
LITE_MAGIC = 0x80
TYPE_SYN, TYPE_CONNECT, TYPE_DATA, TYPE_DISCONNECT, TYPE_PING = 0, 1, 2, 3, 4
TYPE_NAMES = {0: "SYN", 1: "CONNECT", 2: "DATA", 3: "DISCONNECT", 4: "PING", 8: "KEEPALIVE?"}
FLAG_ACK, FLAG_RELIABLE, FLAG_NEED_ACK, FLAG_HAS_SIZE, FLAG_MULTI_ACK = 1, 2, 4, 8, 0x200
OPT_SUPPORT, OPT_SIGNATURE, OPT_FRAGMENT, OPT_INITIAL_SEQ, OPT_MAX_SUBSTREAM = 0, 1, 2, 3, 4

# RMC
NOT_IMPLEMENTED = 0x80010002            # Core::NotImplemented with the error bit
DATASTORE_NOT_FOUND = 0x80690004        # DataStore::NotFound: the answer to a DataStore method we do not know


def hexdump(b, n=128):
    return b[:n].hex(" ") + (" ..." if len(b) > n else "")


class Log:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8") if path else None
        self.lock = threading.Lock()

    def __call__(self, text):
        line = time.strftime("%H:%M:%S ") + text
        with self.lock:
            print(line, flush=True)
            if self.f:
                self.f.write(line + "\n")
                self.f.flush()


# ------------------------------------------------------------------ PRUDP lite
class Packet:
    """magic 0x80, options length u8, payload length u16, stream types u8 (source << 4 | destination), source port u8,
    destination port u8, fragment id u8, type and flags u16 (flags << 4 | type), sequence id u16, options, payload."""

    def __init__(self, type_, flags=0, seq=0, src_type=10, dst_type=10, src_port=1, dst_port=0, fragment=0,
                 options=None, payload=b""):
        self.type, self.flags, self.seq = type_, flags, seq
        self.src_type, self.dst_type, self.src_port, self.dst_port = src_type, dst_type, src_port, dst_port
        self.fragment, self.options, self.payload = fragment, options or [], payload

    @staticmethod
    def decode(b):
        if len(b) < 12 or b[0] != LITE_MAGIC:
            raise ValueError("not a PRUDP lite packet: %s" % hexdump(b, 16))
        optlen, plen, types, sport, dport, frag, tf, seq = struct.unpack_from("<BHBBBBHH", b, 1)
        if 12 + optlen + plen != len(b):
            raise ValueError("lengths %d + %d do not match %d bytes" % (optlen, plen, len(b)))
        opts, o = [], 12
        while o < 12 + optlen:
            oid, osz = b[o], b[o + 1]
            opts.append((oid, b[o + 2:o + 2 + osz]))
            o += 2 + osz
        return Packet(tf & 0xf, tf >> 4, seq, types >> 4, types & 0xf, sport, dport, frag, opts, b[12 + optlen:])

    def encode(self):
        opts = b"".join(struct.pack("<BB", oid, len(v)) + v for oid, v in self.options)
        return struct.pack("<BBHBBBBHH", LITE_MAGIC, len(opts), len(self.payload), self.src_type << 4 | self.dst_type,
                           self.src_port, self.dst_port, self.fragment, self.flags << 4 | self.type, self.seq) + opts + self.payload

    def option(self, oid):
        for i, v in self.options:
            if i == oid:
                return v
        return None

    def __str__(self):
        fl = "|".join(n for f, n in ((FLAG_ACK, "ACK"), (FLAG_RELIABLE, "RELIABLE"), (FLAG_NEED_ACK, "NEED_ACK"),
                                     (FLAG_HAS_SIZE, "HAS_SIZE"), (FLAG_MULTI_ACK, "MULTI_ACK")) if self.flags & f)
        opts = " ".join("%d=%s" % (i, v.hex()) for i, v in self.options)
        return "%s%s seq %d ports %d->%d streams %d->%d frag %d%s%s" % (
            TYPE_NAMES.get(self.type, self.type), "[" + fl + "]" if fl else "", self.seq, self.src_port, self.dst_port,
            self.src_type, self.dst_type, self.fragment, " opts " + opts if opts else "",
            " payload %d" % len(self.payload) if self.payload else "")


# ------------------------------------------------------------------ RMC
class Rmc:
    def __init__(self, protocol, call, method, body):
        self.protocol, self.call, self.method, self.body = protocol, call, method, body

    @staticmethod
    def decode(b):
        size, = struct.unpack_from("<I", b)
        if size + 4 != len(b):
            raise ValueError("RMC size %d, %d bytes" % (size, len(b)))
        proto, o = b[4], 5
        if not proto & 0x80:
            raise ValueError("RMC response from the client (protocol byte %02x)" % proto)
        proto &= 0x7f
        if proto == 0x7f:
            proto, = struct.unpack_from("<H", b, o)
            o += 2
        call, method = struct.unpack_from("<II", b, o)
        return Rmc(proto, call, method, b[o + 8:])


def rmc_proto_bytes(proto, request):
    flag = 0x80 if request else 0
    return bytes([0x7f | flag]) + struct.pack("<H", proto) if proto >= 0x7f else bytes([proto | flag])


def rmc_request(protocol, call, method, body):
    """A call from the server to the game (notifications)."""
    b = rmc_proto_bytes(protocol, True) + struct.pack("<II", call, method) + body
    return struct.pack("<I", len(b)) + b


def rmc_ok(req, body):
    b = rmc_proto_bytes(req.protocol, False) + b"\x01" + struct.pack("<II", req.call, req.method | 0x8000) + body
    return struct.pack("<I", len(b)) + b


def rmc_error(req, code):
    b = rmc_proto_bytes(req.protocol, False) + b"\x00" + struct.pack("<II", code, req.call)
    return struct.pack("<I", len(b)) + b


# ------------------------------------------------------------------ NEX data (NEX 4: structures carry a header)
class Reader:
    def __init__(self, b):
        self.b, self.o = b, 0

    def u(self, f):
        v = struct.unpack_from("<" + f, self.b, self.o)[0]
        self.o += struct.calcsize(f)
        return v

    def raw(self, n):
        v = self.b[self.o:self.o + n]
        self.o += n
        return v

    def string(self):
        return self.raw(self.u("H")).rstrip(b"\0").decode("utf-8", "replace")

    def buffer(self):
        return self.raw(self.u("I"))

    def struct_header(self):
        return self.u("B"), self.u("I")          # version, length


class Writer:
    def __init__(self):
        self.parts = []

    def u(self, f, v):
        self.parts.append(struct.pack("<" + f, v))
        return self

    def raw(self, b):
        self.parts.append(b)
        return self

    def string(self, t):
        b = t.encode() + b"\0"
        return self.u("H", len(b)).raw(b)

    def buffer(self, b):
        return self.u("I", len(b)).raw(b)

    def bytes(self):
        return b"".join(self.parts)

    def structure(self, body, version=0):
        return self.u("B", version).u("I", len(body)).raw(body)


def nex_datetime(t=None):
    t = t or datetime.datetime.now(datetime.timezone.utc)
    return t.second | t.minute << 6 | t.hour << 12 | t.day << 17 | t.month << 22 | t.year << 26


def rc4(key, data):
    S, j = list(range(256)), 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 255
        S[i], S[j] = S[j], S[i]
    out, i, j = bytearray(), 0, 0
    for c in data:
        i = (i + 1) & 255
        j = (j + S[i]) & 255
        S[i], S[j] = S[j], S[i]
        out.append(c ^ S[(S[i] + S[j]) & 255])
    return bytes(out)


def kerberos_encrypt(key, data):
    """RC4 with the key, then HMAC-MD5 of the result with the same key (the game accepts the login's ticket so made)."""
    out = rc4(key, data)
    return out + hmac.new(key, out, "md5").digest()


def kerberos_decrypt(key, data):
    """None when the HMAC does not match."""
    body, mac = data[:-16], data[-16:]
    if not hmac.compare_digest(hmac.new(key, body, "md5").digest(), mac):
        return None
    return rc4(key, body)


# ------------------------------------------------------------------ the servers' calls
SERVER_NAME = "ssbu-recomp"
SECURE_PID = 2              # the secure server's pid, the target of the login's ticket
SECURE_STREAM_PORT = 1      # UNCONFIRMED: the secure server on the same PRUDP port as the authentication server


def pid_of(username):
    """The player's NEX pid: the network service account id the game sends (runtime/online_menu.inc om_nsa_id)."""
    try:
        return int(username) & 0xffffffffffffffff
    except ValueError:
        return int.from_bytes(hashlib.sha256(username.encode()).digest()[:8], "little") | 1


def validate_and_request_ticket(client, req):
    """TicketGranting method 6 with ValidateAndRequestTicketParam {platform u32, username, AnyDataHolder
    AuthenticationInfo {token, ngs version u32, token type u8, server version u32}, skip version check bool,
    nex version u32, client version u32}. Answers ValidateAndRequestTicketResult {pid u64, ticket Buffer, secure server
    StationURL, server time DateTime, server name, source key (hex)} (UNCONFIRMED: field list and version 0)."""
    r = Reader(req.body)
    r.struct_header()
    platform, username = r.u("I"), r.string()
    holder = r.string()
    r.u("I"), r.u("I")
    r.struct_header(), r.struct_header()
    token, ngs, token_type, server_version = r.string(), r.u("I"), r.u("B"), r.u("I")
    skip, nex_version, client_version = r.u("B"), r.u("I"), r.u("I")
    pid = pid_of(username)
    client.log("%s     login: platform %d, user %s (pid %d), %s token %d bytes (ngs %d, type %d), NEX %d, client %d" % (
        client.who, platform, username, pid, holder, len(token), ngs, token_type, nex_version, client_version))
    client.pid = pid
    key = client.user_key = os.urandom(32)                   # the source key: the client decrypts its tickets with it
    session_key = os.urandom(32)
    internal = Writer().u("Q", pid).raw(session_key).bytes()     # only our secure server reads it (our own format)
    ticket = kerberos_encrypt(key, Writer().raw(session_key).u("Q", SECURE_PID).buffer(internal).bytes())
    host, port = client.local
    host = PUBLIC_IP[0] or host                              # (a cloud VM: the public address, not its private one)
    url = "prudps:/address=%s;port=%d;CID=1;PID=2;sid=%d;stream=10;type=2" % (host, port, SECURE_STREAM_PORT)
    body = (Writer().u("Q", pid).buffer(ticket).string(url).u("Q", nex_datetime()).string(SERVER_NAME)
            .string(key.hex()).bytes())
    client.log("%s     ticket for pid %d, secure server %s" % (client.who, pid, url))
    return Writer().structure(body).bytes()


RESULT_SUCCESS = 0x00010001
NOTIFY_PARTICIPATION = 3001  # type 3 (participation) * 1000 + 1 (a new participant): param1 gid, param2 pid
CALLS_DIR = "nex_calls"     # the bodies of calls without a handler (in the current directory)


def request_ticket(client, req):
    """TicketGranting method 3 (source pid u64, target pid u64): the ticket for the secure server. Answers Result, the
    ticket Buffer and the key as hex. Encrypted with the login's source key: with a fresh key the game dropped the ticket
    and crashed on a null session key (0x71001cf570, called from 0x71002443d8)."""
    r = Reader(req.body)
    source, target = r.u("Q"), r.u("Q")
    key, session_key = client.user_key or os.urandom(32), os.urandom(32)
    internal = Writer().u("Q", source).raw(session_key).bytes()
    ticket = kerberos_encrypt(key, Writer().raw(session_key).u("Q", target).buffer(internal).bytes())
    client.log("%s     ticket from pid %d for pid %d" % (client.who, source, target))
    return Writer().u("I", RESULT_SUCCESS).buffer(ticket).string(key.hex()).bytes()


_next_cid = [1]
CLIENTS_BY_CID = {}            # connection id (RVCID) -> Client (the secure connection)


def register(client, req):
    """SecureConnection method 1 (List<StationURL>): the game's local addresses (prudp:/address=...;port=...;Pl=3;
    Tpt=2;natf=0;natm=0;pmp=0;sid=30;upnp=0). Answers Result, the connection id u32 and the public StationURL: the
    first local one as seen from here, with type=3, PID and RVCID."""
    r = Reader(req.body)
    urls = [r.string() for _ in range(r.u("I"))]
    cid = client.cid = _next_cid[0]
    _next_cid[0] += 1
    CLIENTS_BY_CID[cid] = client
    first = urls[0] if urls else "prudp:/address=%s;port=0" % client.peer[0]
    scheme, _, params = first.partition(":/")
    fields = dict(kv.split("=", 1) for kv in params.split(";") if "=" in kv)
    fields["address"] = pub_ip(client.peer[0])
    # Smash's Pia (5.19) builds its NAT probe from the public station only with type=11 (behind NAT 1 | public 2 |
    # Switch 8) and Pa=<an address> (public knowledge of the Smash server protocol); type=3 without Pa (the older
    # Pia's shape): the two games probed each other, then "one or more other consoles are not responding" (2618-0502).
    fields.update(type="11", PID=str(client.pid or 0), RVCID=str(cid), Pa=fields["address"])
    public = scheme + ":/" + ";".join("%s=%s" % kv for kv in sorted(fields.items()))
    client.urls = urls + [public]
    client.log("%s     register: %s -> connection id %d, public %s" % (client.who, " | ".join(urls), cid, public))
    return Writer().u("I", RESULT_SUCCESS).u("I", cid).string(public).bytes()


def get_integer_settings(client, req):
    """Utility method 7 GetIntegerSettings(index u32) -> Map<u16, s32>: no settings."""
    client.log("%s     integer settings %d: none" % (client.who, Reader(req.body).u("I")))
    return Writer().u("I", 0).bytes()


def datastore_61(client, req):
    """DataStore method 61, no parameters (a Smash method: UNKNOWN). Answered with an empty list (u32 0) to see what
    the game does with it."""
    return Writer().u("I", 0).bytes()


def matchmake_ext_58(client, req):
    """MatchmakeExtension method 58 (u32: 7 when the player searches Battle Arenas): UNKNOWN. Answered with an empty
    list (u32 0) to learn what the game does with it."""
    client.log("%s     MatchmakeExtension 58(%s): an empty list" % (client.who, req.body.hex(" ")))
    return Writer().u("I", 0).bytes()


ARENAS = {}                 # gathering id -> {"owner": pid, "session": MatchmakeSession bytes, "client": Client}
ARENAS_LOCK = threading.Lock()
_next_gid = [1000]


def matchmake_session_span(b, o):
    """The bytes of a MatchmakeSession at b[o:]: the Gathering part (header + body) then the MatchmakeSession part."""
    g = struct.unpack_from("<I", b, o + 1)[0]
    m = struct.unpack_from("<I", b, o + 5 + g + 1)[0]
    return o + 5 + g + 5 + m


def _skip_variant(b, o):
    """The end of a Variant at b[o:] (type u8, then: 1 s64, 2 double, 3 bool, 4 String, 5 DateTime, 6 u64)."""
    t = b[o]
    o += 1
    if t in (1, 2, 5, 6):
        return o + 8
    if t == 3:
        return o + 1
    if t == 4:
        return o + 2 + struct.unpack_from("<H", b, o)[0]
    return o


def finish_session(session, players, key=None):
    """A MatchmakeSession (Gathering + MatchmakeSession, structure headers, NEX 4.x field order) with the fields the server
    owns filled in, as the game expects when it reads an arena (a joiner ignored one without them): participation count,
    a session key of 32 random bytes (Pia's), the start time, and the MatchmakeParam entries @SR = true, @GIR = 3 (the
    values are public knowledge of the Smash server protocol; this code is our own). The fields after StartedTime are kept."""
    b = bytes(session)
    g = struct.unpack_from("<I", b, 1)[0]
    mh = 5 + g                                   # MatchmakeSession header: version u8, length u32
    o = session_participants_offset(b)
    head = b[:o]
    o += 4                                       # participation count
    o += 1                                       # progress score
    o += 4 + struct.unpack_from("<I", b, o)[0]   # session key Buffer
    option = b[o:o + 4]
    o += 4
    po = o                                       # MatchmakeParam: structure (version, length), Map<String, Variant>
    plen = struct.unpack_from("<I", b, po + 1)[0]
    n = struct.unpack_from("<I", b, po + 5)[0]
    q = po + 9
    entries = []
    for _ in range(n):
        kl = struct.unpack_from("<H", b, q)[0]
        name = b[q + 2:q + 2 + kl].rstrip(b"\0").decode("ascii", "replace")
        e = _skip_variant(b, q + 2 + kl)
        entries.append((name, b[q:e]))
        q = e
    o = po + 5 + plen
    o += 8                                       # StartedTime
    tail = b[o:]
    entries = [(k, v) for k, v in entries if k not in ("@SR", "@GIR")]
    entries.append(("@SR", Writer().string("@SR").u("B", 3).u("B", 1).bytes()))
    entries.append(("@GIR", Writer().string("@GIR").u("B", 1).u("q", 3).bytes()))
    pbody = struct.pack("<I", len(entries)) + b"".join(v for _, v in entries)
    key = key or os.urandom(32)
    out = bytearray(head + struct.pack("<I", players) + b"\0" + struct.pack("<I", len(key)) + key + option +
                    b[po:po + 1] + struct.pack("<I", len(pbody)) + pbody + struct.pack("<Q", nex_datetime()) + tail)
    struct.pack_into("<I", out, mh + 1, len(out) - mh - 5)
    return bytes(out), key


def create_matchmake_session_with_param(client, req):
    """MatchmakeExtension method 38, creating a Battle Arena: CreateMatchmakeSessionParam {MatchmakeSession (Gathering
    {gid u32, owner u64, host u64, min u16, max u16, policy u32, policy argument u32, flags u32, state u32,
    description}, game mode u32 (7: an arena), attributes List<u32>, open participation, matchmake system type u32,
    application buffer (413 bytes: the arena's settings and the host's name), ...), then 17 more bytes}. Answers the
    session as it was sent with the gathering id, owner and host filled in."""
    b = req.body
    end = matchmake_session_span(b, 5)
    session = bytearray(b[5:end])
    with ARENAS_LOCK:
        gid = _next_gid[0]
        _next_gid[0] += 1
        struct.pack_into("<IQQ", session, 5, gid, client.pid or 0, client.pid or 0)
        try:
            done, key = finish_session(session, 1)
        except (struct.error, IndexError) as e:
            client.log("%s     arena %d: the session could not be completed (%s): kept as sent" % (client.who, gid, e))
            done, key = bytes(session), None
        session = bytearray(done)
        ARENAS[gid] = {"owner": client.pid, "session": done, "client": client, "key": key}
    r = Reader(bytes(session))
    r.raw(5 + 4 + 16)
    mn, mx = r.u("H"), r.u("H")
    with ARENAS_LOCK:
        if gid in ARENAS:
            ARENAS[gid]["max"] = mx
    client.log("%s     arena %d created by pid %d (%d..%d players, session %d bytes, %d more bytes in the param: %s; "
               "session key %s)" % (client.who, gid, client.pid or 0, mn, mx, len(session), len(b) - end, b[end:].hex(" "),
                                    key.hex() if key else "none"))
    # NexMatchCreateSessionJob::WaitNotification: the game waits ~10 s for a notification after creating, then gives
    # up ("a communication error"). Sent after the answer: participation (3001) of the owner in his own gathering.
    client.after_reply = lambda: client.notify(NOTIFY_PARTICIPATION, gid, client.pid or 0, param3=1)
    return bytes(session)


def unregister_gathering(client, req):
    """MatchMaking method 2 UnregisterGathering(gid u32) -> bool."""
    gid = Reader(req.body).u("I")
    with ARENAS_LOCK:
        ok = ARENAS.get(gid, {}).get("owner") == client.pid
        if ok:
            del ARENAS[gid]
    client.log("%s     unregister gathering %d: %s" % (client.who, gid, "removed" if ok else "not ours / unknown"))
    return Writer().u("B", 1 if ok else 0).bytes()


def matchmake_ext_52(client, req):
    """MatchmakeExtension method 52, the Battle Arenas search: a MatchmakeSessionSearchCriteria-like structure
    (attribute strings "0", "1", "7"..., flags) and at the end a ResultRange {offset 0, size 50}. Answered with an empty
    list of sessions (u32 0) (UNCONFIRMED: the answer type)."""
    with ARENAS_LOCK:
        found = [a["session"] for a in ARENAS.values()]
    client.log("%s     MatchmakeExtension 52 (arena search): %d arena(s)" % (client.who, len(found)))
    w = Writer().u("I", len(found))
    for session in found:
        w.raw(session)
    return w.bytes()


def report_nat_properties(client, req):
    """NATTraversal method 5 ReportNATProperties(natmapping u32, natfiltering u32, rtt u32) -> nothing (sent right after
    the NAT check, when an arena is made)."""
    r = Reader(req.body)
    a = pia_claim(client)
    client.log("%s     NAT properties: mapping %d, filtering %d, rtt %d; Pia socket %s, station id (CID) %s" % (client.who,
               r.u("I"), r.u("I"), r.u("I"), "%s:%d" % a if a else "NOT SEEN (no probe to 33335 in the last 5 s)",
               getattr(client, "pia_cid", None)))
    return b""


ARENA_CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXY0123456789"     # (no I / O / Z: easy to read out) UNCONFIRMED: the game's own set


def matchmake_ext_55(client, req):
    """MatchmakeExtension method 55 (Smash's own): makes the arena's code (the 5 characters shown to share it) for the
    gathering id u32. Answer: a structure {String code, DateTime} (version 0) (the shape: public knowledge of the
    Smash server protocol; this code is our own)."""
    gid = Reader(req.body).u("I")
    with ARENAS_LOCK:
        a = ARENAS.get(gid)
        code = a.get("code") if a else None
        while not code or any(x.get("code") == code for x in ARENAS.values()):
            code = "".join(ARENA_CODE_ALPHABET[b % len(ARENA_CODE_ALPHABET)] for b in os.urandom(5))
        if a is not None:
            a["code"] = code
    client.log("%s     MatchmakeExtension 55: arena %d gets the code %s%s" % (client.who, gid, code, "" if a else " (gid unknown)"))
    return Writer().structure(Writer().string(code).u("Q", nex_datetime()).bytes()).bytes()


def matchmake_ext_56(client, req):
    """MatchmakeExtension method 56 (Smash's own): the arena whose code a player typed. Request: a structure {String
    code, u64} (2026-11-02: "Ebh2v" for the code EBH2V: compared without case). Answer: the gathering id u32 (0: no such
    arena; the game then says "arena not found")."""
    r = Reader(req.body)
    r.raw(5)                                  # (the structure's version u8 and length u32)
    n = r.u("H")
    code = r.raw(n).rstrip(b"\0").decode("ascii", "replace").strip().upper()
    with ARENAS_LOCK:
        gid = next((g for g, a in ARENAS.items() if a.get("code", "").upper() == code), 0)
    client.log("%s     MatchmakeExtension 56: code %s -> %s" % (client.who, code, ("arena %d" % gid) if gid else "no arena"))
    return Writer().u("I", gid).bytes()


def find_by_gathering_ids(client, req):
    """MatchmakeExtension method 48 FindMatchmakeSessionByGatheringID(List<u32> gids) -> List<MatchmakeSession>: asked
    right after an arena code was resolved (method 56), before joining it. The sessions as they were made (the same
    bytes the search, method 52, answers with)."""
    r = Reader(req.body)
    gids = [r.u("I") for _ in range(r.u("I"))]
    with ARENAS_LOCK:
        found = [ARENAS[g]["session"] for g in gids if g in ARENAS]
    client.log("%s     FindMatchmakeSessionByGatheringID %s: %d found" % (client.who, gids, len(found)))
    w = Writer().u("I", len(found))
    for session in found:
        w.raw(session)
    return w.bytes()


def session_participants_offset(session):
    """Where the participation count u32 sits in a stored MatchmakeSession (the bytes of create, method 38): after the
    Gathering, the MatchmakeSession header, game mode u32, attributes List<u32>, open participation u8, matchmake system
    type u32 and the application buffer (NEX 4.x field order; UNCONFIRMED)."""
    g = struct.unpack_from("<I", session, 1)[0]
    o = 5 + g + 5
    o += 4
    o += 4 + 4 * struct.unpack_from("<I", session, o)[0]
    o += 1 + 4
    o += 4 + struct.unpack_from("<I", session, o)[0]
    return o


def join_matchmake_session_with_param(client, req):
    """MatchmakeExtension method 39 JoinMatchmakeSessionWithParam(JoinMatchmakeSessionParam {gid u32, ...}) ->
    MatchmakeSession: a player enters an arena (after method 56 / 48 or the search). The session is answered with the
    participation count now; every player already in it gets a participation notification (3001) for the newcomer, and
    the newcomer one for itself."""
    r = Reader(req.body)
    r.raw(5)
    gid = r.u("I")
    with ARENAS_LOCK:
        a = ARENAS.get(gid)
        if a is None:
            client.log("%s     JoinMatchmakeSessionWithParam: arena %d does not exist" % (client.who, gid))
            return 0x8068000c          # RendezVous::SessionVoid (UNCONFIRMED: the game's message for it)
        parts = a.setdefault("players", [a["owner"]])
        if client.pid not in parts:
            parts.append(client.pid)
        session = bytearray(a["session"])
        try:
            struct.pack_into("<I", session, session_participants_offset(session), len(parts))
        except struct.error:
            pass
        a["session"] = bytes(session)
        conns = a.setdefault("clients", {a["owner"]: a["client"]})
        conns[client.pid] = client
        others = [c for p, c in conns.items() if c is not client and p in parts]
    client.log("%s     JoinMatchmakeSessionWithParam: pid %d into arena %d (%d players: %s)" % (client.who, client.pid or 0, gid, len(parts), parts))
    joiner = client.pid or 0
    count = len(parts)
    existing = [p for p in parts if p != joiner]

    def tell():
        # (public knowledge of the Smash server protocol; this code is our own) every player in the arena, the newcomer
        # too, is told "joiner joined" with the player count in param3; then the newcomer is told about each player
        # already in it, one notification each. Without the second part the newcomer never learnt the host was there:
        # the two games probed each other, then "one or more other consoles are not responding" (2618-0502).
        client.notify(NOTIFY_PARTICIPATION, gid, joiner, source=joiner, param3=count)
        for c in others:
            c.notify(NOTIFY_PARTICIPATION, gid, joiner, source=joiner, param3=count)
        for p in existing:
            client.notify(NOTIFY_PARTICIPATION, gid, p, source=joiner, param3=count)
    client.after_reply = tell
    return bytes(session)


def request_probe_initiation_ext(client, req):
    """NATTraversal method 3 RequestProbeInitiationExt(targets List<String>, station String) -> nothing: a player who
    joined asks the server to tell the others (their station URLs: the RVCID is their connection id) to probe it at the
    same time (the hole punch). Each target gets InitiateProbe (NATTraversal method 2, station String) from the server."""
    r = Reader(req.body)
    targets = [r.string() for _ in range(r.u("I"))]
    station = r.string()
    told = []
    for t in targets:
        fields = dict(kv.split("=", 1) for kv in t.partition(":/")[2].split(";") if "=" in kv)
        c = CLIENTS_BY_CID.get(int(fields.get("RVCID", "0") or 0))
        if c is None or c is client:
            continue
        c.call(3, 2, Writer().string(station).bytes(), "InitiateProbe(%s)" % station)
        told.append(c.pid)
    client.log("%s     RequestProbeInitiationExt: probe %s; told %s (targets %s)" % (client.who, station, told, targets))
    return b""


def nat_result(client, req):
    """NATTraversal method 4 ReportNATTraversalResult / 7 ReportNATTraversalResultDetail: how a probe went (logged)."""
    client.log("%s     NAT traversal result (method %d): %s" % (client.who, req.method, hexdump(req.body, 96)))
    return b""


def nat_ack(client, req):
    """NATTraversal methods 1 / 2 (RequestProbeInitiation, InitiateProbe) sent to the server: nothing to do."""
    client.log("%s     NATTraversal %d: acknowledged" % (client.who, req.method))
    return b""


def relay_signature_key(client, req):
    """NATTraversal method 6 GetRelaySignatureKey -> relay mode s32 0, current time DateTime, relay address String "",
    relay port u16 0, relay address type s32 0, game server id u32 0: no relay (direct connections only)."""
    client.log("%s     GetRelaySignatureKey: no relay" % client.who)
    return Writer().u("i", 0).u("Q", nex_datetime()).string("").u("H", 0).u("i", 0).u("I", 0).bytes()


def stations_for(c):
    """The host's two stations for a joiner: its local one (as registered) and its public one, both with the address and
    port of its Pia socket."""
    out = []
    pia = getattr(c, "pia", None)
    for u in (getattr(c, "urls", None) or [])[:1] + (getattr(c, "urls", None) or [])[-1:]:
        scheme, _, params = u.partition(":/")
        fields = dict(kv.split("=", 1) for kv in params.split(";") if "=" in kv)
        if pia:
            fields["address"], fields["port"] = pub_ip(pia[0], pia[1]), str(pia[1])
            if "Pa" in fields:
                fields["Pa"] = pub_ip(pia[0], pia[1])
        if getattr(c, "pia_cid", None):
            fields["CID"] = str(c.pia_cid)
        fields.setdefault("PID", str(c.pid or 0))
        fields.setdefault("RVCID", str(getattr(c, "cid", 0)))
        out.append(scheme + ":/" + ";".join("%s=%s" % kv for kv in sorted(fields.items())))
    return out


def station_for(c):
    """The station URL other players reach this game at: its public one (register) with the address and port of its
    Pia socket (seen by the reachability port). The port the game registers is not its Pia port (it registers before
    Pia binds; public knowledge of the Smash server protocol: the server puts the NAT check's port in)."""
    public = c.urls[-1] if getattr(c, "urls", None) else "prudp:/address=%s;port=0" % c.peer[0]
    scheme, _, params = public.partition(":/")
    fields = dict(kv.split("=", 1) for kv in params.split(";") if "=" in kv)
    pia = getattr(c, "pia", None)
    if pia:
        fields["address"], fields["port"] = pub_ip(pia[0], pia[1]), str(pia[1])
    return scheme + ":/" + ";".join("%s=%s" % kv for kv in sorted(fields.items()))


def get_session_urls(client, req):
    """MatchMaking method 41 GetSessionURLs(gid u32) -> List<StationURL>: a player who joined an arena asks where its
    host is (then Pia connects to it)."""
    gid = Reader(req.body).u("I")
    with ARENAS_LOCK:
        a = ARENAS.get(gid)
        host = a and a.get("client")
    urls = stations_for(host) if host else []
    client.log("%s     GetSessionURLs arena %d: %s" % (client.who, gid, urls or "no host"))
    w = Writer().u("I", len(urls))
    for u in urls:
        w.string(u)
    return w.bytes()


_next_round = [1]


def leave_arenas(pid, only_gid=None):
    """(2026-11-02) a member out of its arenas' member lists (a departed member kept there made the fighters wait for a
    spectator's HELLO that never came, and the F1 counts were wrong). Call with ARENAS_LOCK held."""
    for gid, a in ARENAS.items():
        if only_gid is not None and gid != only_gid:
            continue
        parts = a.get("players")
        if parts and pid in parts and pid != a["owner"]:
            parts.remove(pid)
            a.get("clients", {}).pop(pid, None)


def referee_1(client, req):
    """MatchmakeReferee (50 here) method 1: 7 bytes (the arena's gid u32, then 01 00 00) right after a player joined.
    UNCONFIRMED what it is for Smash (StartRound in other games: answered with a round id u64): answered with a new
    number u64."""
    n = _next_round[0]
    _next_round[0] += 1
    try:
        gid = struct.unpack_from("<I", req.body, 0)[0]
        with ARENAS_LOCK:
            leave_arenas(client.pid, gid)              # (MatchMakingExt EndParticipation(gid, message): the member leaves)
    except struct.error:
        pass
    client.log("%s     MatchmakeReferee 1 (%s): answered %d (UNCONFIRMED)" % (client.who, hexdump(req.body), n))
    return Writer().u("B", 1).bytes()             # (MatchMakingExt EndParticipation(gid u32, message String) -> bool)


def update_application_buffer(client, req):
    """MatchmakeExtension method 11 UpdateApplicationBuffer(gid u32, buffer Buffer) -> nothing: inside the arena the
    host rewrites its application buffer (the arena's settings, the players' names: 414 bytes on 2026-11-02). It replaces
    the one inside the stored session: a joiner reads the arena from it (methods 48 / 52) and ignored the one of the
    creation (2026-11-02: the code resolved, the arena was sent, the game said "not found")."""
    r = Reader(req.body)
    gid = r.u("I")
    n = r.u("I")
    buf = r.raw(n)
    how = " (gid unknown)"
    with ARENAS_LOCK:
        a = ARENAS.get(gid)
        if a is not None:
            a["app_buffer"] = buf
            b = a["session"]
            try:
                g = struct.unpack_from("<I", b, 1)[0]
                q = 5 + g + 5 + 4
                q += 4 + 4 * struct.unpack_from("<I", b, q)[0]
                q += 1 + 4                                    # q: the application buffer (u32 length + bytes)
                old = struct.unpack_from("<I", b, q)[0]
                out = bytearray(b[:q] + struct.pack("<I", n) + buf + b[q + 4 + old:])
                mh = 5 + g
                struct.pack_into("<I", out, mh + 1, len(out) - mh - 5)
                a["session"] = bytes(out)
                how = " (the arena's buffer replaced: %d -> %d bytes)" % (old, n)
            except (struct.error, IndexError) as e:
                how = " (kept beside the session: %s)" % e
    client.log("%s     UpdateApplicationBuffer: arena %d, %d bytes%s" % (client.who, gid, n, how))
    return b""


PIA_SEEN = []                  # (time, (ip, port), cid) of the sockets that sent a Pia packet to 33335, not given to a client yet
PIA_LOCK = threading.Lock()


def pia_claim(client):
    """The Pia socket of this client: the latest one that probed 33335 in the last 5 s and no one has claimed yet (the
    game probes it just before ReportNATProperties). Several games on one IP (two copies on one PC) are told apart this
    way: by the order of the probe and the report, not by the address."""
    now = time.time()
    with PIA_LOCK:
        PIA_SEEN[:] = [e for e in PIA_SEEN if now - e[0] < 5]
        if not PIA_SEEN:
            return None
        t, a, cid = PIA_SEEN.pop()
    client.pia = a
    client.pia_cid = cid
    return a


ARENA_QUERY_PORT = 39599


def arena_query_listener(host, log, socks=None):
    """(2026-11-02) The game asks which Battle Arena its player is in, to look for its opponent of a rollback match in the
    lobby with that arena as the key (ssbu_lobby.py MATCH op 2 with a key; runtime net_session.c). UDP, little-endian:
    "ARENA?" + pid u64 -> "ARENA!" + gid u32 (0: in none) + n u8 + n x pid u64 (the arena's players, its maker first)."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        u.bind((host, ARENA_QUERY_PORT))
    except OSError as e:
        log("arena query port %s:%d: %s" % (host, ARENA_QUERY_PORT, e))
        return
    log("arena queries on UDP %s:%d" % (host, ARENA_QUERY_PORT))
    if socks is not None:
        socks[("nncs2", 10025)] = u                         # (the virtual nncs2's port 10025: NNCS2_PORTS)
    while True:
        try:
            data, addr = u.recvfrom(256)
        except OSError:
            continue
        if data[:7] == b"STATUS?":
            # (2026-11-02, F1 > Experimental) "STATUS!" + players online u16 + arenas u8 + per arena: code[5] (ASCII, NUL
            # padded) + members u8 + most u8 (little-endian; at most 32 arenas)
            with ARENAS_LOCK:
                online = len(CLIENTS_BY_CID)
                rows = [(a.get("code", "") or "", len(a.get("players", [a["owner"]])), a.get("max", 0)) for a in ARENAS.values()][:32]
            out = b"STATUS!" + struct.pack("<HB", online, len(rows))
            for code, members, most in rows:
                out += code.encode("ascii", "replace")[:5].ljust(5, b"\0") + struct.pack("<BB", min(members, 255), min(most, 255))
            u.sendto(out, addr)
            continue
        if len(data) == 16 and socks is not None and PUBLIC_IP[0]:   # a NAT check sent to the virtual nncs2:10025
            out, via = nat_answer(data, addr, ("nncs2", 10025), socks)
            if out is not None and via is not None:
                via.sendto(out, addr)
            continue
        if len(data) < 14 or data[:6] != b"ARENA?":
            continue
        pid = struct.unpack_from("<Q", data, 6)[0]
        with ARENAS_LOCK:
            found = [(g, a.get("players", [a["owner"]])) for g, a in ARENAS.items() if pid in a.get("players", [a["owner"]])]
        gid, players = found[-1] if found else (0, [])
        u.sendto(b"ARENA!" + struct.pack("<IB", gid, len(players)) + b"".join(struct.pack("<Q", x) for x in players), addr)
        log("arena query from %s:%d: pid %d -> arena %d, players %s" % (addr[0], addr[1], pid, gid, players))


def sinkhole(host, port, log):
    """Pia's reachability check sends to the NAT check server on UDP 33334 / 33335 and expects NO answer: the ports
    only have to be open (on Windows a datagram to a closed loopback port comes back as an error on the game's socket:
    with nothing there, creating an arena ended in 2618-0006)."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        u.bind((host, port))
    except OSError as e:
        log("reachability port %s:%d: %s" % (host, port, e))
        return
    seen = set()
    while True:
        try:
            data, addr = u.recvfrom(2048)
        except OSError:
            continue
        if port == 33335 and data[:4] == b"\x32\xab\x98\x64" and not data[4] & 0x80 and len(data) >= 76:
            # the first probe is in the clear; its last 4 bytes are the game's Pia station id (its variable id, the
            # CID of its station URLs: a joiner's connection request names the host by it; without CID in the host's
            # URLs the host denied it, result 3, and the joiner showed 2618-0502)
            cid = struct.unpack(">I", data[72:76])[0]
            with PIA_LOCK:
                if all(e[1] != addr for e in PIA_SEEN):
                    PIA_SEEN.append((time.time(), addr, cid))
        if addr not in seen:
            seen.add(addr)
            log("reachability %s:%d <- %s:%d %d bytes (not answered, as it should be): %s" % (host, port, addr[0], addr[1], len(data), hexdump(data[:48])))


HANDLERS = {
    (3, 5): report_nat_properties,
    (3, 3): request_probe_initiation_ext,
    (3, 4): nat_result,
    (3, 7): nat_result,
    (3, 1): nat_ack,
    (3, 2): nat_ack,
    (3, 6): relay_signature_key,
    (109, 55): matchmake_ext_55,
    (109, 11): update_application_buffer,
    (109, 56): matchmake_ext_56,
    (109, 48): find_by_gathering_ids,
    (109, 39): join_matchmake_session_with_param,
    (21, 41): get_session_urls,
    (50, 1): referee_1,
    (21, 2): unregister_gathering,
    (109, 38): create_matchmake_session_with_param,
    (109, 52): matchmake_ext_52,
    (109, 58): matchmake_ext_58,
    (110, 7): get_integer_settings,
    (115, 61): datastore_61,
    (11, 1): register,
    (10, 3): request_ticket,
    (10, 6): validate_and_request_ticket,
}

# protocol id -> (name, {method id: name}); names only for the log
PROTOCOLS = {
    10: ("TicketGranting", {1: "Login", 2: "LoginEx", 3: "RequestTicket", 4: "GetPID", 5: "GetName",
                            6: "ValidateAndRequestTicketWithParam"}),
    11: ("SecureConnection", {1: "Register", 2: "RequestConnectionData", 3: "RequestUrls",
                              4: "RegisterEx", 5: "TestConnectivity", 6: "UpdateURLs", 7: "ReplaceURL",
                              8: "SendReport"}),
    14: ("Notification", {}),
    21: ("MatchMaking", {}),
    50: ("MatchmakeReferee", {}),
    109: ("MatchmakeExtension", {}),
    110: ("Utility", {1: "AcquireNexUniqueId", 7: "GetIntegerSettings", 8: "GetStringSettings"}),
    112: ("Ranking2", {}),
    115: ("DataStore", {}),
}


def rmc_name(req):
    pname, methods = PROTOCOLS.get(req.protocol, ("protocol %d" % req.protocol, {}))
    return "%s.%s" % (pname, methods.get(req.method, "method %d" % req.method))


# ------------------------------------------------------------------ one client (one WebSocket)
class Client:
    def __init__(self, s, addr, log):
        self.s, self.log, self.who = s, log, "%s:%d" % addr
        self.send_lock = threading.Lock()
        self.seq_out = 1                 # our next DATA sequence id (UNCONFIRMED: 1 unless the client gave option 3)
        self.fragments = b""
        self.handlers = HANDLERS         # (protocol, method) -> fn(client, Rmc) -> body bytes or int error
        self.local = s.getsockname()     # the address the game reached us on (the secure server's address)
        self.peer = addr                 # the game's address as seen from here
        self.cid, self.urls = 0, []
        self.pid = None
        self.after_reply = None          # something to send right after the current call's answer (a notification)
        self.user_key = None             # the login's source key
        self.last_ports = (1, 0x1f)

    # WebSocket
    def recv_exact(self, n):
        out = b""
        while len(out) < n:
            k = self.s.recv(n - len(out))
            if not k:
                raise ConnectionError("closed")
            out += k
        return out

    def ws_read(self):
        h = self.recv_exact(2)
        op, masked, ln = h[0] & 15, h[1] >> 7, h[1] & 127
        if ln == 126:
            ln = int.from_bytes(self.recv_exact(2), "big")
        elif ln == 127:
            ln = int.from_bytes(self.recv_exact(8), "big")
        mask = self.recv_exact(4) if masked else b"\0\0\0\0"
        return op, bytes(b ^ mask[i & 3] for i, b in enumerate(self.recv_exact(ln)))

    def ws_send(self, data, op=2):
        n = len(data)
        h = bytes([0x80 | op]) + (bytes([n]) if n < 126 else bytes([126]) + n.to_bytes(2, "big") if n < 65536
                                  else bytes([127]) + n.to_bytes(8, "big"))
        with self.send_lock:
            self.s.sendall(h + data)

    def handshake(self):
        req = b""
        while b"\r\n\r\n" not in req:
            k = self.s.recv(4096)
            if not k:
                raise ConnectionError("closed before the upgrade")
            req += k
        lines = req.partition(b"\r\n\r\n")[0].decode("latin-1").split("\r\n")
        hdr = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:])}
        self.log("%s %s, Host %s, protocol %s" % (self.who, lines[0], hdr.get("host"), hdr.get("sec-websocket-protocol")))
        if hdr.get("upgrade", "").lower() != "websocket":
            self.s.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
            raise ConnectionError("not a WebSocket upgrade")
        accept = base64.b64encode(hashlib.sha1(hdr.get("sec-websocket-key", "").encode() + WS_GUID).digest())
        reply = b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n"
        if "sec-websocket-protocol" in hdr:
            reply += b"Sec-WebSocket-Protocol: " + hdr["sec-websocket-protocol"].split(",")[0].strip().encode() + b"\r\n"
        self.s.sendall(reply + b"\r\n")

    # PRUDP
    def send_packet(self, p):
        self.log("%s  <- %s" % (self.who, p))
        self.ws_send(p.encode())

    def reply_to(self, req, type_, flags, seq=None, options=None, payload=b""):
        return Packet(type_, flags, req.seq if seq is None else seq, req.dst_type, req.src_type, req.dst_port,
                      req.src_port, 0, options, payload)

    def on_packet(self, p):
        self.log("%s  -> %s" % (self.who, p))
        if p.flags & (FLAG_ACK | FLAG_MULTI_ACK):
            return                                            # the client's ACK of our packet: nothing to resend over TCP
        if p.type == TYPE_SYN:
            sig = os.urandom(16)                              # UNCONFIRMED: the lite SYN-ACK carries a 16-byte signature
            opts = [(OPT_SUPPORT, p.option(OPT_SUPPORT) or struct.pack("<I", 0))] + [(OPT_SIGNATURE, sig)]
            self.send_packet(self.reply_to(p, TYPE_SYN, FLAG_ACK, options=opts))
        elif p.type == TYPE_CONNECT:
            init = p.option(OPT_INITIAL_SEQ)
            if init:
                self.seq_out = struct.unpack("<H", init)[0]
            opts = [(i, v) for i, v in p.options if i in (OPT_SUPPORT, OPT_INITIAL_SEQ, OPT_MAX_SUBSTREAM)]
            self.send_packet(self.reply_to(p, TYPE_CONNECT, FLAG_ACK, options=opts, payload=self.secure_connect(p.payload)))
        elif p.type == TYPE_DATA:
            if p.flags & FLAG_NEED_ACK:
                self.send_packet(self.reply_to(p, TYPE_DATA, FLAG_ACK))
            self.fragments += p.payload
            if p.fragment == 0:                               # the last (or only) fragment
                data, self.fragments = self.fragments, b""
                self.on_rmc(data)
        elif p.type == TYPE_PING:
            if p.flags & FLAG_NEED_ACK:
                self.send_packet(self.reply_to(p, TYPE_PING, FLAG_ACK))
        elif p.type == TYPE_DISCONNECT:
            self.send_packet(self.reply_to(p, TYPE_DISCONNECT, FLAG_ACK))
            raise ConnectionError("DISCONNECT")
        elif p.flags & FLAG_NEED_ACK:
            # type 8: the game's keep-alive on the secure connection, every 3 s with its own sequence; without an ACK
            # the game drops the connection after about 50 s (2306-0332)
            self.send_packet(self.reply_to(p, p.type, FLAG_ACK))

    def secure_connect(self, payload):
        """The secure server's CONNECT: Buffer internal ticket (ours: pid u64, session key) + Buffer check data
        (kerberos with the session key: pid u64, connection id u32, check u32). The answer is Buffer(check + 1) in the clear (the game disconnected when it was encrypted, with or without a length)."""
        if not payload:
            return b""                                        # the authentication server: no ticket
        r = Reader(payload)
        internal, check = r.buffer(), r.buffer()
        pid, session_key = struct.unpack_from("<Q", internal)[0], internal[8:]
        plain = kerberos_decrypt(session_key, check)
        if plain is None or len(plain) < 16:
            self.log("%s     secure CONNECT: the check data does not decrypt with the ticket's session key (%s)" % (self.who, hexdump(check)))
            return b""
        cpid, cid, value = struct.unpack_from("<QII", plain)
        self.pid, self.session_key = pid, session_key
        self.log("%s     secure CONNECT: pid %d (check data pid %d), connection id %d, check %08x" % (self.who, pid, cpid, cid, value))
        return Writer().buffer(struct.pack("<I", (value + 1) & 0xffffffff)).bytes()

    def notify(self, type_, param1, param2, text="", param3=0, source=None):
        """NotificationEvents (protocol 14) method 1 ProcessNotificationEvent(NotificationEvent {source pid u64, type u32,
        param1 u64, param2 u64, string, param3 u64}) (UNCONFIRMED: the NEX 4.6 field list, structure version 0)."""
        event = Writer().u("Q", (self.pid if source is None else source) or 0).u("I", type_).u("Q", param1).u("Q", param2).string(text).u("Q", param3).bytes()
        self.notify_call = getattr(self, "notify_call", 0x10000) + 1
        self.log("%s     notification %d to pid %d: param1 %d param2 %d" % (self.who, type_, self.pid or 0, param1, param2))
        self.send_data(rmc_request(14, self.notify_call, 1, Writer().structure(event).bytes()), self.last_ports[0],
                       self.last_ports[1])

    def call(self, protocol, method, body, what=""):
        """A remote call from the server to this game (an RMC request; its answer is only logged)."""
        self.notify_call = getattr(self, "notify_call", 0x10000) + 1
        self.log("%s     call to pid %d: protocol %d method %d %s" % (self.who, self.pid or 0, protocol, method, what))
        self.send_data(rmc_request(protocol, self.notify_call, method, body), self.last_ports[0], self.last_ports[1])

    def send_data(self, payload, src_port=1, dst_port=0x1f):
        p = Packet(TYPE_DATA, FLAG_RELIABLE | FLAG_NEED_ACK | FLAG_HAS_SIZE, self.seq_out, 10, 10, src_port, dst_port,
                   0, None, payload)
        self.seq_out = (self.seq_out + 1) & 0xffff
        self.send_packet(p)

    # RMC
    def on_rmc(self, data):
        if len(data) > 4 and not data[4] & 0x80:
            self.log("%s     the game's answer to our call: %s" % (self.who, hexdump(data, 32)))
            return
        try:
            req = Rmc.decode(data)
        except (ValueError, struct.error) as e:
            self.log("%s     not an RMC request (%s): %s" % (self.who, e, hexdump(data)))
            return
        fn = self.handlers.get((req.protocol, req.method))
        if not fn:                                            # the whole body, for reading it later (the log cuts it)
            os.makedirs(CALLS_DIR, exist_ok=True)
            with open(os.path.join(CALLS_DIR, "%d_%d_%d.bin" % (req.protocol, req.method, req.call)), "wb") as f:
                f.write(req.body)
        self.log("%s     RMC call %d %s, %d bytes%s" % (self.who, req.call, rmc_name(req), len(req.body),
                                                         "" if fn else ": NOT IMPLEMENTED (%s), body %s" % (
            "answered DataStore::NotFound" if req.protocol == 115 else "answered Core::NotImplemented", hexdump(req.body, 512))))
        try:
            out = fn(self, req) if fn else DATASTORE_NOT_FOUND if req.protocol == 115 else NOT_IMPLEMENTED
        except (ValueError, struct.error, IndexError) as e:
            self.log("%s     %s: could not read the call (%s), body %s" % (self.who, rmc_name(req), e, hexdump(req.body, 512)))
            out = NOT_IMPLEMENTED
        self.send_data(rmc_error(req, out) if isinstance(out, int) else rmc_ok(req, out), self.last_ports[0],
                       self.last_ports[1])
        after, self.after_reply = self.after_reply, None
        if after:
            after()

    def run(self):
        self.log("%s connected" % self.who)
        try:
            self.s.settimeout(300)
            self.handshake()
            while True:
                op, data = self.ws_read()
                if op == 8:
                    self.log("%s WebSocket close" % self.who)
                    return
                if op in (9, 10):
                    if op == 9:
                        self.ws_send(data, 10)
                    continue
                try:
                    p = Packet.decode(data)
                except ValueError as e:
                    self.log("%s  ?? %s" % (self.who, e))
                    continue
                self.last_ports = (p.dst_port, p.src_port)
                self.on_packet(p)
        except (ConnectionError, OSError) as e:
            self.log("%s %s" % (self.who, e))
        finally:
            with ARENAS_LOCK:
                for gid in [g for g, a in ARENAS.items() if a["client"] is self]:
                    del ARENAS[gid]
                    self.log("%s arena %d closed (its host left)" % (self.who, gid))
                if getattr(self, "cid", None) and CLIENTS_BY_CID.get(self.cid) is self:
                    del CLIENTS_BY_CID[self.cid]
                    if self.pid:
                        leave_arenas(self.pid)
            self.s.close()
            self.log("%s gone" % self.who)


NAT_PORTS = (10025, 10125)  # Pia's NAT check servers (nncs1 / nncs2): primary and secondary port (the game sends to 10025)
NAT_HOSTS = ["127.0.0.1", "127.0.0.2"]   # nncs1, nncs2 (runtime/online_menu.inc resolves them to these); --nat-ip

# The NAT check (Kinnay's NintendoClients wiki, "NAT Check Server"): UDP, big-endian, 16 bytes both ways:
#   +0 type u32 (the game's), +4 external port u32, +8 external IP u32, +0xC the answering server's own IP u32 (the server's).
# Where the answer comes from tells the game how its NAT filters: 2 = from the other IP and the other port, 3 / 102 (0x66)
# = from the same IP and the other port, every other type (1, 4, 5, 101, 103) = from the socket it was sent to.
# (2026-11-02: the game sends 101 and 102 to nncs1:10025 and 103 to nncs2:10025; unanswered, creating an arena ended
# in 2618-0201 after ~17 s.)
FAKE_IP = [""]                 # --fake-ip: the address games on THIS PC are told they have (runtime SSBU_ONLINE_MENU_FAKEIP)


def pub_ip(ip, port=None):
    """The address other players reach a game's socket at: a loopback one (a game on this PC) becomes 10.200.<port / 256>.
    <port % 256> with --fake-ip (Pia drops 127.x from station URLs; one shared fake address made the host take the
    joiner for itself and deny it, result 3). The same rule as online_menu.inc's RecvFrom; without a port: --fake-ip."""
    if not (FAKE_IP[0] and ip.startswith("127.")):
        return ip
    return "10.200.%d.%d" % (port >> 8 & 255, port & 255) if port is not None else FAKE_IP[0]


PUBLIC_IP = [""]               # --public-ip: this server's address as the players reach it (a cloud VM: its public IP, not
                               # the private one of its network card); empty = the address each connection came in on


# (2026-11-02) One address (--public-ip): Pia needs nncs2 to be another address (nncs2 = nncs1: 2618-0006 at once). The
# runtime tells the game nncs2 is NNCS2_VIRTUAL and sends what goes there to this server: its port 10025 -> 39599 (the
# arena query port, already open), 10125 -> 10126 (only answered FROM: no inbound rule needed). runtime online_menu.inc.
NNCS2_VIRTUAL = "198.18.0.2"
NNCS2_PORTS = {10025: 39599, 10125: 10126}
NAT_OTHER_PORT = {2, 3, 0x66}
NAT_OTHER_IP = {2}


def nat_answer(data, addr, here, socks):
    if len(data) < 4:
        return None, None
    kind = struct.unpack(">I", data[:4])[0]
    host, port = here
    if kind in NAT_OTHER_IP and len(NAT_HOSTS) > 1:       # (one address: nncs1 and nncs2 are the same server; the game asks 101-103 only)
        host = NAT_HOSTS[1] if host == NAT_HOSTS[0] else NAT_HOSTS[0]
    elif kind in NAT_OTHER_IP and PUBLIC_IP[0]:            # (one address: the virtual nncs2 <-> nncs1)
        host = "nncs2" if host != "nncs2" else NAT_HOSTS[0]
    if kind in NAT_OTHER_PORT:
        port = NAT_PORTS[1] if port == NAT_PORTS[0] else NAT_PORTS[0]
    out = struct.pack(">IIII", kind, addr[1], struct.unpack(">I", socket.inet_aton(pub_ip(addr[0], addr[1])))[0],
                      struct.unpack(">I", socket.inet_aton(NNCS2_VIRTUAL if host == "nncs2" else (PUBLIC_IP[0] or host)))[0])
    return out, socks.get((host, port))


def nat_listener(here, socks, log):
    """Answers Pia's NAT check sent to nncs1 (127.0.0.1) / nncs2 (127.0.0.2), ports 10025 / 10125."""
    u = socks[here]
    seen = set()
    while True:
        try:
            data, addr = u.recvfrom(2048)
        except OSError:
            continue            # (Windows: an ICMP port unreachable of an earlier answer)
        out, via = nat_answer(data, addr, here, socks)
        key = (addr, data[:4])
        if out is None or via is None:
            log("NAT check %s:%d <- %s:%d %d bytes: %s: NOT ANSWERED" % (here[0], here[1], addr[0], addr[1], len(data), hexdump(data)))
            continue
        via.sendto(out, addr)
        if key not in seen:     # (each type is resent ~5 times: logged once per client socket)
            seen.add(key)
            src = via.getsockname()
            log("NAT check %s:%d <- %s:%d type %s: answered from %s:%d with %s" % (here[0], here[1], addr[0], addr[1],
                hexdump(data[:4]), src[0], src[1], hexdump(out)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=443)
    ap.add_argument("--log", default="")
    ap.add_argument("--public-ip", default="", help="this server's address as the players reach it (a cloud VM behind 1:1 NAT: "
                    "its public IP; the NAT check and the login's secure server address say it); with it the NAT check "
                    "listens on every interface (0.0.0.0)")
    ap.add_argument("--nat-ip", default="127.0.0.1,127.0.0.2",
                    help="the address(es) of the NAT check (nncs1[,nncs2]) as the players reach this PC: one address for a "
                         "server the players reach over a network (Radmin, a VPS: the game is told nncs2 = nncs1); two "
                         "loopback ones for copies on this PC (default)")
    ap.add_argument("--fake-ip", default="", help="the address games on this PC are told they have (two copies on one PC; "
                    "the same as their SSBU_ONLINE_MENU_FAKEIP, default there 10.200.0.1)")
    a = ap.parse_args()
    log = Log(a.log)
    FAKE_IP[0] = a.fake_ip
    NAT_HOSTS[:] = [h.strip() for h in a.nat_ip.split(",") if h.strip()][:2]
    PUBLIC_IP[0] = a.public_ip
    if a.public_ip:
        NAT_HOSTS[:] = ["0.0.0.0"]                           # (one server, every interface; the answers name --public-ip)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.bind, a.port))
    srv.listen(8)
    log("nex server on TCP %s:%d (WebSocket, PRUDP lite)" % (a.bind, a.port))
    socks = {}
    for host in NAT_HOSTS:
        for port in NAT_PORTS:
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                u.bind((host, port))
            except OSError as e:
                log("NAT check listener %s:%d: %s" % (host, port, e))
                continue
            socks[(host, port)] = u
    for here in list(socks):
        threading.Thread(target=nat_listener, args=(here, socks, log), daemon=True).start()
    if a.public_ip:                                          # the virtual nncs2's port 10125 (answers only)
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.bind(("0.0.0.0", NNCS2_PORTS[10125]))
        socks[("nncs2", 10125)] = u
    threading.Thread(target=arena_query_listener, args=(a.bind, log, socks if a.public_ip else None), daemon=True).start()
    for host in NAT_HOSTS:
        for port in (33334, 33335):
            threading.Thread(target=sinkhole, args=(host, port, log), daemon=True).start()
    while True:
        s, addr = srv.accept()
        threading.Thread(target=Client(s, addr, log).run, daemon=True).start()


if __name__ == "__main__":
    main()
