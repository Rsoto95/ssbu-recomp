#!/usr/bin/env python3
"""Tournaments through ssbu_lobby.py over UDP (lobby 1.7): four games with accounts make a tournament, sign up, the
organizer moves a seed and starts it, and the whole double elimination is played through the protocol: stage strikes,
bans and picks by whoever's turn it is, ready at the set-up (the loser of a game confirms after the winner's fighter),
the rollback search pairs each set's two players only (the side-0 player hosts), both games' RESULTs count a game; a
disputed game is played again. Also: the bracket text in chunks (TOURNEY_DOC), only the organizer may start, a game
without an account is refused, /tournaments.json and /tournament/<id>.json (names only).
    python -m unittest tools/net/test_tournament_lobby.py        (standard library only)"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ssbu_lobby as L  # noqa: E402
from test_ladder import RankedFake  # noqa: E402
from test_ssbu_lobby import Fake, free_port, free_tcp_port  # noqa: E402


class TFake(RankedFake):
    seq = 0

    def __init__(self, lobby, name):
        super().__init__(lobby, name)
        self.s.settimeout(0.02)                                     # (many short looks: do not wait 0.2 s on each empty read)

    def t(self, op, tid=0, args=b"", have=0):
        """a TOURNEY op (a new seq for anything but a look)"""
        if op != L.T_LOOK:
            self.seq += 1
        self.tx(L.TOURNEY, struct.pack(">QHBII", self.cookie, self.seq, op, tid, have) + args)

    def view(self, tid=0, op=L.T_LOOK, args=b"", wait=0.25):
        """send op, then the newest TOURNEY_STATE: (acknowledged seq, {key: [rest of the line]} with 't' a list of lines)"""
        self.t(op, tid, args)
        last, end = None, time.time() + wait
        while time.time() < end or last is None:
            try:
                last = self.rx(L.TOURNEY_STATE, max(0.05, end - time.time()) if last is None else 0.05)
            except AssertionError:
                if last is None and time.time() > end + 2:
                    raise
        ack = struct.unpack_from(">H", last)[0]
        out = {"t": []}
        for line in last[2:].decode().split("\n"):
            k, _, rest = line.partition(" ")
            if k == "t":
                out["t"].append(rest.split(" ", 5))
            elif k:
                out[k] = rest.split(" ") if k not in ("err", "opp") else [rest]
        return ack, out

    def doc(self, tid, secs=1.0):
        """the newest bracket text, from its TOURNEY_DOC chunks (older versions may still be queued: the newest complete one wins)"""
        self.t(L.T_LOOK, tid)
        vers, end = {}, time.time() + secs
        while time.time() < end:
            try:
                b = self.rx(L.TOURNEY_DOC, 0.1)
            except AssertionError:
                continue
            t, ver, i, cnt = struct.unpack_from(">IIBB", b)
            if t == tid:
                vers.setdefault(ver, [cnt, {}])[1][i] = b[10:]
        done = [v for v, (cnt, parts) in vers.items() if len(parts) == cnt]
        assert done, "no complete bracket"
        cnt, parts = vers[max(done)]
        return b"".join(parts[i] for i in range(cnt)).decode()


class TournamentLobbyTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.db = os.path.join(self.dir.name, "ladder.db")
        self.port, self.sport = free_port(), free_tcp_port()
        self.proc = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "ssbu_lobby.py"), "--ladder-db", self.db, "--ladder-open",
                                      "--bind", "127.0.0.1", "--port", str(self.port), "--stats", "0", "--status-port", str(self.sport)],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.lines = []
        self.reader = threading.Thread(target=lambda: self.lines.extend(iter(self.proc.stdout.readline, "")), daemon=True)
        self.reader.start()
        deadline = time.time() + 10
        while time.time() < deadline and not any("status endpoint" in l for l in self.lines):
            time.sleep(0.05)
        self.lob = ("127.0.0.1", self.port)

    def tearDown(self):
        self.proc.terminate()
        self.proc.wait(5)
        self.reader.join(2)
        self.proc.stdout.close()
        if os.environ.get("LOBBY_LOG"):
            print("".join(self.lines))
        self.dir.cleanup()

    def fake(self, name):
        f = TFake(self.lob, name)
        self.addCleanup(f.s.close)
        st = f.join_ld()
        self.assertEqual((st[0], st[-1]), (L.LD_RANKED, name))
        return f

    def get(self, path):
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (self.sport, path), timeout=3) as r:
            return r.read()

    def test_a_whole_tournament(self):
        names = ["Rober", "napky", "Fredy", "Luis"]
        fs = [self.fake(n) for n in names]
        org = fs[0]
        create = b"Viernes".ljust(32, b"\0") + bytes((3, 3, 3, 2, 3))
        ack, v = org.view(0, L.T_CREATE, create)
        self.assertEqual(ack, org.seq)
        self.assertNotIn("err", v)
        tid = int(v["t"][0][0])
        self.assertEqual(v["t"][0][5], "Viernes")
        for f in fs:
            _, v = f.view(tid, L.T_SIGNUP)
            self.assertNotIn("err", v, f.name)
        _, v = fs[1].view(tid, L.T_START)
        self.assertIn("only the organizer", v["err"][0])
        doc = org.doc(tid)                                      # (no ratings yet: a random order)
        luis = next(int(l.split(" ")[1]) for l in doc.split("\n") if l.startswith("E ") and l.endswith(" Luis"))
        _, v = org.view(tid, L.T_MOVE, bytes((luis, 0)))        # (Luis to seed 1)
        doc = org.doc(tid)
        self.assertIn("E 0 0 - Luis", doc)
        _, v = org.view(tid, L.T_START)
        self.assertNotIn("err", v)
        self.assertEqual(v["t"][0][1], "running")
        doc = org.doc(tid)
        self.assertEqual(sum(1 for l in doc.split("\n") if l.startswith("S ")), 3 + 2 + 2)   # (4 players: W 2+1, L 1+1, G 2)
        played, disputed, guard, searches = 0, False, 0, 100
        while guard < 400:
            guard += 1
            vs = [f.view(tid, wait=0.05)[1] for f in fs]
            if all(v["t"][0][1] == "done" for v in vs if v["t"]):
                break
            acted = False
            for f, v in zip(fs, vs):
                me = v.get("me")
                if not me or me[1] == "-1":
                    continue
                phase, turn = v["phase"][0], v["phase"][1]
                if phase in ("strike", "ban", "pick") and turn == "1":
                    free = [s for s in v["pool"] if s not in v.get("struck", [])]
                    sid = int(me[1])
                    f.view(tid, L.T_STAGE, struct.pack(">HB", sid, len(free[0])) + free[0].encode())
                    acted = True
                elif phase == "chars" and v["ready"][0] == "0":
                    first = v["first"][0]
                    if first == "2" and v["ready"][1] == "0":
                        continue                                    # (the winner of the last game picks first)
                    f.view(tid, L.T_READY, struct.pack(">QB", 0xc100000000000001, 1 if first == "2" else 0))
                    acted = True
            if acted:
                continue
            ready = [(f, v) for f, v in zip(fs, vs) if v.get("go") == ["1"]]
            if len(ready) >= 2:
                f0, v0 = ready[0]
                sid = v0["me"][1]
                pair = [(f, v) for f, v in ready if v["me"][1] == sid]
                self.assertEqual(len(pair), 2)
                (h, hv), (g, gv) = sorted(pair, key=lambda x: x[1]["me"][4] != "1")      # (the host first: "me <entrant> <set> set <side> <host>")
                searches += 1                                       # (a new id for every search, as the game does)
                h.match(L.KIND_ROLLBACK, searches)
                g.match(L.KIND_ROLLBACK, searches)
                st_h, st_g = h.until(L.MS_MATCHED), g.until(L.MS_MATCHED)
                self.assertEqual((st_h[2], st_h[4], st_g[2], st_g[4]), (L.ROLE_HOST, g.ip, L.ROLE_JOIN, h.ip))
                h.tx(L.RB, struct.pack(">QI", h.cookie, g.ip) + b"x")         # (the two games start the match: the lobby relays it)
                g.tx(L.RB, struct.pack(">QI", g.cookie, h.ip) + b"x")
                time.sleep(0.1)
                _, v = h.view(tid)
                self.assertEqual((v["phase"][0], v["go"]), ("playing", ["0"]))   # (a game pressing start again is not paired now)
                seed = 0x5000 + searches                            # (every match has its own seed)
                win = played % 2                                    # (alternate winners: sets go to game 3)
                stocks = (2, 0) if win == 0 else (0, 1)
                if not disputed:                                    # one game whose two reports disagree: not counted
                    disputed = True
                    h.result(seed, g.ip, 0, stocks)
                    g.result(seed, h.ip, 1, (stocks[1], stocks[0]))
                    time.sleep(0.2)
                    _, v = h.view(tid)
                    self.assertIn("not counted", v["err"][0])
                    self.assertEqual(v["score"][:2], ["0", "0"])
                    continue
                h.result(seed, g.ip, 0, stocks)
                g.result(seed, h.ip, 1, stocks)
                played += 1
                for x in (h, g):
                    x.match(0, 0)                                   # (the search is over)
                time.sleep(0.1)
        _, v = org.view(tid)
        self.assertEqual(v["t"][0][1], "done", "".join(self.lines[-30:]))
        self.assertTrue(disputed)
        self.assertGreaterEqual(played, 2 * 6)                      # (6 sets at least, each 2 or 3 games)
        places = {}
        for f in fs:
            _, v = f.view(tid)
            self.assertEqual(v["me"][2], "done")
            places[f.name] = int(v["me"][3])
        self.assertEqual(sorted(places.values()), [1, 2, 3, 4])
        lst = json.loads(self.get("/tournaments.json"))
        self.assertEqual([(t["id"], t["state"]) for t in lst], [(tid, "done")])
        t = json.loads(self.get("/tournament/%d.json" % tid))
        self.assertEqual(sorted(s["name"] for s in t["standings"]), sorted(names))
        self.assertEqual(t["entrants"][0]["name"], "Luis")
        body = self.get("/tournament/%d.json" % tid).decode()
        for f in fs:
            self.assertNotIn(f.pid.hex(), body)
        self.assertIn(b"<title>SSBU torneos</title>", self.get("/tournament/%d" % tid))

    def test_no_account_no_tournament(self):
        f = TFake(self.lob, "Guest")
        self.addCleanup(f.s.close)
        f.join()                                                    # (no 'L''D' block: no account)
        _, v = f.view(0, L.T_CREATE, b"X".ljust(32, b"\0") + bytes((3, 3, 3, 2, 3)))
        self.assertIn("account", v["err"][0])
        self.assertEqual(v["acct"], ["0", "0"])

    def test_rules_checked(self):
        f = self.fake("Rober")
        _, v = f.view(0, L.T_CREATE, b"X".ljust(32, b"\0") + bytes((2, 3, 3, 2, 3)))
        self.assertIn("best of", v["err"][0])

    def test_others_not_taken_by_a_tournament_player(self):
        a, b, c = self.fake("Rober"), self.fake("napky"), self.fake("Fredy")
        _, v = a.view(0, L.T_CREATE, b"T".ljust(32, b"\0") + bytes((1, 1, 1, 0, 3)))
        tid = int(v["t"][0][0])
        a.view(tid, L.T_SIGNUP)
        b.view(tid, L.T_SIGNUP)
        a.view(tid, L.T_START)
        c.match(L.KIND_ROLLBACK, 7)                                 # (a stranger searching meets neither of them)
        a.match(L.KIND_ROLLBACK, 8)
        st = a.state()
        self.assertEqual(st[1], L.MS_SEARCHING)
        self.assertEqual(c.state()[1], L.MS_SEARCHING)


if __name__ == "__main__":
    unittest.main()
