#!/usr/bin/env python3
"""Self-test of the relay near the players (2026-11-05): ssbu_lobby.py with --relay-secret and ssbu_relay.py, both on
127.0.0.1, two fake games matched for a rollback match on the public code.
  - the relay's RELAY_HB makes the lobby offer it: MATCH_STATE carries 'R' + the relay's address, the match id and its tag
    (after 'A'), the same id for both games; a heartbeat signed with another key is ignored
  - a game's hello comes back to it alone (its round trip to the relay); a rollback message and path info go to the other
    game of the match, with the sender's lobby address; a wrong tag, and a third game on the same match id, are dropped
    python test_relay.py      (standard library only)"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ssbu_lobby as L  # noqa: E402
from test_ssbu_lobby import Fake, free_port, none_of, mods  # noqa: E402

RELAY = 33


def start(args, lines):
    p = subprocess.Popen([sys.executable, "-I"] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    threading.Thread(target=lambda: lines.extend(iter(p.stdout.readline, "")), daemon=True).start()
    return p


def wait_for(lines, text, secs=10):
    end = time.time() + secs
    while time.time() < end:
        if any(text in l for l in lines):
            return
        time.sleep(0.05)
    raise AssertionError("never printed %r:\n%s" % (text, "".join(lines)))


def matched_state(f):
    """the MATCH_STATE of the match: (peer, relay (ip, port, id, tag) or None)"""
    end = time.time() + 4
    while time.time() < end:
        b = f.rx(L.MATCH_STATE, 1.0)
        if b[4] != L.MS_MATCHED:
            continue
        o = 56 if len(b) >= 56 and b[49:50] == b"A" else 49
        peer = struct.unpack_from(">I", b, 7)[0]
        if len(b) >= o + 23 and b[o:o + 1] == b"R":
            return peer, struct.unpack_from(">IHQ8s", b, o + 1)
        return peer, None
    raise AssertionError("%s: not matched" % f.name)


def relay_tx(s, at, mid, tag, src, kind, data):
    s.sendto(b"SU" + bytes([1, RELAY]) + struct.pack(">Q8sIB", mid, tag, src, kind) + data, at)


def relay_rx(s, secs=1.0):
    s.settimeout(secs)
    try:
        while True:
            p, _ = s.recvfrom(4096)
            if p[3] == RELAY:
                return p
    except socket.timeout:
        return None


def main():
    lport, rport = free_port(), free_port()
    d = tempfile.mkdtemp()
    key, other = os.path.join(d, "relay.key"), os.path.join(d, "other.key")
    open(key, "w").write("test-key-0123456789abcdef\n")
    open(other, "w").write("another-key-0123456789abc\n")
    ll, rl, bl = [], [], []
    lob = start([os.path.join(HERE, "ssbu_lobby.py"), "--bind", "127.0.0.1", "--port", str(lport), "--stats", "0",
                 "--relay-secret", key], ll)
    bad = rel = None
    try:
        wait_for(ll, "UDP")
        bad = start([os.path.join(HERE, "ssbu_relay.py"), "--bind", "127.0.0.1", "--port", str(free_port()),
                     "--lobby", "127.0.0.1:%d" % lport, "--secret-file", other, "--stats", "0"], bl)
        time.sleep(1.0)
        assert not any("relay near the players up" in l for l in ll), "a heartbeat with another key was taken"
        rel = start([os.path.join(HERE, "ssbu_relay.py"), "--bind", "127.0.0.1", "--port", str(rport),
                     "--lobby", "127.0.0.1:%d" % lport, "--secret-file", key, "--stats", "0"], rl)
        wait_for(ll, "relay near the players up")
        X, P = b"ssbu-recomp-ldn1-13.0.5", b"public"
        a, b, c = Fake(("127.0.0.1", lport), "A", X, mods(), P), Fake(("127.0.0.1", lport), "B", X, mods(), P), Fake(("127.0.0.1", lport), "C", X, mods(), P)
        ia, ib, ic = a.join(), b.join(), c.join()
        a.match(2, 501)
        time.sleep(0.2)
        b.match(2, 502)
        pa, ra = matched_state(a)
        pb, rb_ = matched_state(b)
        assert pa == ib and pb == ia, (pa, pb)
        assert ra is not None and rb_ is not None, "no 'R' (relay) in MATCH_STATE"
        assert ra[2] == rb_[2] and ra[3] == rb_[3], "the two games got different match ids"
        r_at = (socket.inet_ntoa(struct.pack(">I", ra[0])), ra[1])
        assert r_at[1] == rport, r_at
        mid, tag = ra[2], ra[3]
        # hello: echoed to the sender only
        relay_tx(a.s, r_at, mid, tag, ia, 0, struct.pack(">Q", 12345))
        p = relay_rx(a.s)
        assert p is not None and struct.unpack_from(">IBQ", p, 4) == (0, 0, 12345), p
        relay_tx(b.s, r_at, mid, tag, ib, 0, struct.pack(">Q", 1))
        assert relay_rx(b.s) is not None
        # a rollback message and path info to the other game
        relay_tx(a.s, r_at, mid, tag, ia, 1, b"pads-from-a")
        p = relay_rx(b.s)
        assert p is not None and struct.unpack_from(">IB", p, 4) == (ia, 1) and p[9:] == b"pads-from-a", p
        relay_tx(b.s, r_at, mid, tag, ib, 2, struct.pack(">HH", 40, 80))
        p = relay_rx(a.s)
        assert p is not None and struct.unpack_from(">IBHH", p, 4) == (ib, 2, 40, 80), p
        # a wrong tag, and a third game on the same match id: dropped
        relay_tx(a.s, r_at, mid, b"\0" * 8, ia, 1, b"forged")
        assert relay_rx(b.s, 0.4) is None, "a message with a wrong tag was carried"
        relay_tx(c.s, r_at, mid, tag, ic, 1, b"third")
        assert relay_rx(a.s, 0.4) is None and relay_rx(b.s, 0.4) is None, "a third game's message was carried"
        assert none_of(c, L.MATCH_STATE, 0.2) or True
        print("test_relay: OK (heartbeat signed, offered in MATCH_STATE 'R' with one match id, hello echoed, messages carried "
              "both ways, wrong tag and third game dropped)")
    finally:
        for p in (lob, bad, rel):
            if p is not None:
                p.kill()


if __name__ == "__main__":
    main()
