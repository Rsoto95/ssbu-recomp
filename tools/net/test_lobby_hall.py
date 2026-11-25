"""Test of the lobby's halls (ssbu_lobby.py 1.9): a room where several pairs play at the same time.

The lobby is started here and fake players talk to it over UDP, as games do: a hall is made and entered by its code; two
players sit at a station and are paired only with each other; two more are paired by a challenge at another station AT
THE SAME TIME; a result counts for its station; standing up, declining, leaving and a lost player are handled.

    python tools/net/test_lobby_hall.py

Python 3.8+, standard library only."""
import os
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ssbu_lobby as L            # noqa: E402
import test_ssbu_lobby as T       # noqa: E402

fails = 0


def check(what, ok, got=None):
    global fails
    if not ok:
        fails += 1
    print("%s %s%s" % ("ok  " if ok else "FAIL", what, "" if ok or got is None else ": %r" % (got,)))


class P(T.Fake):
    """A fake game that knows the hall's messages."""
    def hall(self, op, arg=0, code=b""):
        self.tx(L.HALL, struct.pack(">QBI", self.cookie, op, arg) + code.ljust(5, b"\0"))

    def hstate(self, secs=1.5):
        """the LATEST HALL_STATE (older ones dropped), parsed"""
        b = self.rx(L.HALL_STATE, secs)
        while True:
            try:
                b = self.rx(L.HALL_STATE, 0.25)
            except AssertionError:
                break
        ver, status = struct.unpack_from(">IB", b)
        d = {"version": ver, "status": status, "code": b[5:10].rstrip(b"\0"), "state": b[10], "station": b[11],
             "opp": struct.unpack_from(">I", b, 12)[0], "opp_name": b[16:48].rstrip(b"\0"),
             "from": struct.unpack_from(">I", b, 50)[0], "from_name": b[54:86].rstrip(b"\0"), "to": struct.unpack_from(">I", b, 86)[0]}
        o, d["members"], d["stations"] = 91, {}, {}
        for _ in range(b[90]):
            ip = struct.unpack_from(">I", b, o)[0]
            d["members"][b[o + 4:o + 36].rstrip(b"\0").decode()] = (b[o + 36], b[o + 37])
            o += 38
        n = b[o]
        o += 1
        for _ in range(n):
            num, playing, p1, p2, w1, w2 = struct.unpack_from(">BBIIHH", b, o)
            d["stations"][num] = (playing, p1, p2, w1, w2)
            o += 14
        assert o == len(b), (o, len(b))
        return d

    def ask(self, op=None, arg=0, code=b""):
        """an op (default: still here), then the latest state"""
        self.hall(L.H_STAY if op is None else op, arg, code)
        return self.hstate()

    def result(self, seed, other, slot, s0, s1, flags=L.F_STOCKS_FALLS):
        self.tx(L.RESULT, struct.pack(">QQIBBBBIQQBBHHII", self.cookie, seed, other, slot, 1, 3, flags, 0, 0, 0, s0, s1, 0, 0, 3600, 0))


def play(a, b, sid):
    """a and b search a rollback match and exchange messages both ways: (a's state, b's state) at MATCHED"""
    a.match(2, sid)
    time.sleep(0.15)
    b.match(2, sid + 1)
    sa, sb = a.until(L.MS_MATCHED), b.until(L.MS_MATCHED)
    T.rb(a, b.ip, b"Hhello")
    T.rb(b, a.ip, b"Ihello")
    a.until(L.MS_TOGETHER), b.until(L.MS_TOGETHER)
    return sa, sb


def run(lob):
    X, C = b"ssbu-recomp-ldn1-13.0.5", b"public"
    a, b, c, d, e, out = (P(lob, n, X, T.mods(), C) for n in ("Ana", "Beto", "Caro", "Dani", "Eva", "Otto"))
    for x in (a, b, c, d, e, out):
        x.join()

    s = a.ask()
    check("a player in no hall: status out", s["status"] == L.HS_OUT, s)
    s = a.ask(L.H_STAY, 0, b"ZZZZZ")
    check("a wrong code: no such hall", s["status"] == L.HS_NONE and s["code"] == b"ZZZZZ", s)
    s = a.ask(L.H_MAKE)
    code = s["code"]
    check("a hall is made: its code, its maker in it, free", s["status"] == L.HS_IN and len(code) == 5 and s["members"] == {"Ana": (L.HP_FREE, 0)}, s)
    for x in (b, c, d, e):
        s = x.ask(L.H_STAY, 0, code)
    check("four more came in by the code", s["status"] == L.HS_IN and sorted(s["members"]) == ["Ana", "Beto", "Caro", "Dani", "Eva"], s)
    check("everyone is told (Ana sees five)", len(a.ask()["members"]) == 5)

    # ---- a station: two sit, they are paired with each other only
    s = a.ask(L.H_SIT, 0)
    check("Ana sits: station 1, alone", s["state"] == L.HP_SEATED and s["station"] == 1 and s["stations"] == {1: (0, a.ip, 0, 0, 0)}, s)
    a.match(2, 100)
    check("alone at a station: a search finds nobody", a.state()[:2] == (100, L.MS_SEARCHING))
    out.match(2, 900)                                 # a player outside the hall searches too
    check("...not even a player outside the hall", out.state()[:2] == (900, L.MS_SEARCHING) and T.none_of(a, L.MATCH_STATE, 0.4))
    a.match(0, 100)
    out.match(0, 900)
    a.until(L.MS_IDLE), out.until(L.MS_IDLE)
    s = b.ask(L.H_SIT, 1)
    check("Beto sits at station 1: both matched", s["state"] == L.HP_MATCHED and s["opp"] == a.ip and s["opp_name"] == b"Ana", s)
    check("Ana is told: matched against Beto", a.hstate()["opp"] == b.ip)
    check("Caro cannot sit at the full station", c.ask(L.H_SIT, 1)["state"] == L.HP_FREE)

    # ---- a challenge: another pair, at the same time
    out.ping()
    s = c.ask(L.H_CHALLENGE, d.ip)
    check("Caro challenges Dani", s["to"] == d.ip, s)
    s = d.hstate()
    check("Dani is told who challenges", s["from"] == c.ip and s["from_name"] == b"Caro", s)
    check("a challenge to a player of a full station is refused", e.ask(L.H_CHALLENGE, a.ip)["to"] == 0)
    s = d.ask(L.H_ACCEPT, c.ip)
    check("Dani accepts: both at station 2, matched", s["state"] == L.HP_MATCHED and s["station"] == 2 and s["opp"] == c.ip, s)
    s = c.hstate()
    check("the challenger is player 1 of the station", s["stations"].get(2, (0, 0, 0))[1:3] == (c.ip, d.ip) and s["to"] == 0, s)

    # ---- both pairs search at once: each one meets its own rival
    a.match(2, 200), c.match(2, 300)
    time.sleep(0.2)
    d.match(2, 301), b.match(2, 201)
    sa, sb, sc, sd = a.until(L.MS_MATCHED), b.until(L.MS_MATCHED), c.until(L.MS_MATCHED), d.until(L.MS_MATCHED)
    check("station 1: Ana (player 1) against Beto", sa[2] == L.ROLE_HOST and sa[4] == b.ip and sb[2] == L.ROLE_JOIN and sb[4] == a.ip, (sa, sb))
    check("station 2: Caro (player 1) against Dani", sc[2] == L.ROLE_HOST and sc[4] == d.ip and sd[2] == L.ROLE_JOIN and sd[4] == c.ip, (sc, sd))
    for x, y in ((a, b), (c, d)):
        T.rb(x, y.ip, b"Hhello")
        T.rb(y, x.ip, b"Ihello")
        x.until(L.MS_TOGETHER), y.until(L.MS_TOGETHER)
    s = e.ask()
    check("two stations playing at the same time", s["stations"].get(1, (0,))[0] == 1 and s["stations"].get(2, (0,))[0] == 1, s["stations"])
    check("the members' states: four playing, Eva free", [s["members"][n][0] for n in ("Ana", "Beto", "Caro", "Dani", "Eva")] == [L.HP_PLAYING] * 4 + [L.HP_FREE], s["members"])

    # ---- a result: the station's count, the pair stays for the next match
    a.result(7001, b.ip, 0, 2, 0)
    b.result(7001, a.ip, 1, 2, 0)                     # (both games send it: counted once)
    time.sleep(0.3)
    s = a.ask()
    check("Ana beat Beto: 1-0 at station 1, both still seated, matched", s["stations"][1] == (0, a.ip, b.ip, 1, 0) and s["state"] == L.HP_MATCHED, s)
    check("station 2 goes on", s["stations"][2][0] == 1)
    play(a, b, 210)
    b.result(7002, a.ip, 1, 0, 1)
    time.sleep(0.3)
    check("the rematch: 1-1", a.ask()["stations"][1][3:] == (1, 1))

    # ---- standing up, declining, taking back
    s = b.ask(L.H_STAND)
    check("Beto stands up: free; Ana stays seated, alone, the count gone", s["state"] == L.HP_FREE and s["stations"][1] == (0, a.ip, 0, 0, 0), s)
    s = e.ask(L.H_SIT, 0)
    check("Eva sits (any station): the one where Ana waits", s["station"] == 1 and s["opp"] == a.ip, s)
    e.ask(L.H_STAND)
    b.ask(L.H_CHALLENGE, e.ip)
    check("Eva declines Beto", e.ask(L.H_DECLINE, b.ip)["from"] == 0 and b.hstate()["to"] == 0)
    b.ask(L.H_CHALLENGE, e.ip)
    check("Beto takes his challenge back", b.ask(L.H_CANCEL)["to"] == 0 and e.hstate()["from"] == 0)
    b.ask(L.H_CHALLENGE, e.ip)
    s = e.ask(L.H_CHALLENGE, b.ip)
    check("each challenges the other: that is a yes (station 3)", s["state"] == L.HP_MATCHED and s["opp"] == b.ip and s["station"] == 3, s)

    # ---- leaving and a lost player
    s = d.ask(L.H_LEAVE)
    check("Dani leaves the hall", s["status"] == L.HS_OUT, s)
    s = c.hstate()
    check("Caro is alone at station 2, Dani is not a member", s["stations"][2] == (0, c.ip, 0, 0, 0) and "Dani" not in s["members"], s)
    e.tx(L.LEAVE, struct.pack(">Q", e.cookie))        # Eva's game closes
    time.sleep(0.3)
    s = b.ask()
    check("Eva's game left the lobby: Beto alone at station 3", s["stations"].get(3) == (0, b.ip, 0, 0, 0) and "Eva" not in s["members"], s)

    # ---- the hall and the lobby's other queues
    out = P(lob, "Otto2", X, T.mods(), C)             # (the first one was silent for too long: the lobby dropped it)
    out.join()
    out.match(2, 950)
    searching = out.state()[:2] == (950, L.MS_SEARCHING)
    c.match(2, 951)
    check("a hall member alone at a station is not given to a stranger's search", searching and T.none_of(out, L.MATCH_STATE, 0.5))
    out.match(0, 950), c.match(0, 951)
    f, g = P(lob, "Fede", X, T.mods(), C), P(lob, "Gabi", X, T.mods(), C)
    f.join(), g.join()
    f.match(2, 960)
    time.sleep(0.15)
    g.match(2, 961)
    check("players in no hall are paired as before", f.until(L.MS_MATCHED)[4] == g.ip)
    for x in (a, b, c, out, f, g):
        x.hall(L.H_LEAVE)
        x.tx(L.LEAVE, struct.pack(">Q", x.cookie))
    time.sleep(0.2)


def main():
    port = T.free_port()
    proc = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "ssbu_lobby.py"), "--bind", "127.0.0.1", "--port", str(port), "--stats", "0",
                             "--status-port", str(T.free_tcp_port()), "--search-secs", "30", "--match-secs", "60"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = []
    threading.Thread(target=lambda: lines.extend(iter(proc.stdout.readline, "")), daemon=True).start()
    try:
        deadline = time.time() + 10
        while time.time() < deadline and not any("UDP" in l for l in lines) and proc.poll() is None:
            time.sleep(0.05)
        run(("127.0.0.1", port))
    except Exception:
        global fails
        fails += 1
        import traceback
        traceback.print_exc()
    finally:
        proc.terminate()
        proc.wait(5)
    bad = [l for l in lines if "Traceback" in l or "Error" in l]
    if bad or fails:
        print("--- the lobby's output (last lines)")
        print("".join(lines[-40:]))
        fails_total = fails + len(bad)
    else:
        fails_total = 0
    print("%d failed" % fails_total if fails_total else "all passed")
    sys.exit(1 if fails_total else 0)


if __name__ == "__main__":
    main()
