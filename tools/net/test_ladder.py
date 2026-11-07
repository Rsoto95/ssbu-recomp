#!/usr/bin/env python3
"""Unit tests of ladder.py: Glicko-2 against Glickman's worked example, identity (invitation, open sign-up, key proof,
names), a Bo3 with matching reports (both ratings change), disputed games (reports differ, a desync, one report only,
not the ladder's rules), draws, a set left half-played, and the web documents (no ids or keys).
LobbyLadderTest: the same through ssbu_lobby.py over UDP (JOIN 'L''D' sign-up by invitation, then the key proof only;
a JOIN without 'L''D' still joins, unranked; a Bo3 of RESULTs with RESULT_ACKs; /ladder.json and /player/<name>.json).
    python -m unittest tools/net/test_ladder.py        (standard library only)"""
import hashlib
import hmac
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
import ladder as LD  # noqa: E402
import ssbu_lobby as L  # noqa: E402
from test_ssbu_lobby import Fake, free_port, free_tcp_port  # noqa: E402


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def report(seed, slot, stocks=(2, 0), dmg=(30, 80), **kw):
    r = {"seed": seed, "slot": slot, "stage": 7, "chara": (0x11, 0x22), "stocks": stocks, "dmg": dmg, "frames": 9000,
         "rule_kind": LD.RULE_STOCK, "stock": LD.LADDER_STOCKS, "desyncs": 0, "flags": 0}
    r.update(kw)
    return r


class Glicko2Test(unittest.TestCase):
    def test_paper_example(self):
        r, rd, vol = LD.glicko2(1500, 200, 0.06, [(1400, 30, 1), (1550, 100, 0), (1700, 300, 0)])
        self.assertAlmostEqual(r, 1464.06, delta=0.01)   # (the paper rounds its intermediate steps: 1464.0507 exact)
        self.assertAlmostEqual(rd, 151.52, places=2)
        self.assertAlmostEqual(vol, 0.05999, delta=1e-5)

    def test_no_games_grows_rd(self):
        r, rd, vol = LD.glicko2(1500, 200, 0.06, [])
        self.assertEqual(r, 1500)
        self.assertGreater(rd, 200)

    def test_winner(self):
        self.assertEqual(LD.game_winner((1, 0), (200, 10)), 0)
        self.assertEqual(LD.game_winner((2, 2), (90, 40)), 1)
        self.assertIsNone(LD.game_winner((1, 1), (50, 50)))


class LadderTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.clock = Clock()
        self.ld = LD.Ladder(os.path.join(self.dir.name, "ladder.db"), now=self.clock)
        self.a, self.ka = os.urandom(16), os.urandom(32)
        self.b, self.kb = os.urandom(16), os.urandom(32)
        self.assertEqual(self.ld.identify(self.a, "x", key=self.ka, invite=self.ld.invite("Roberto")), (self.a, "Roberto"))
        self.assertEqual(self.ld.identify(self.b, "y", key=self.kb, invite=self.ld.invite("FreDy")), (self.b, "FreDy"))

    def tearDown(self):
        self.ld.db.close()
        self.dir.cleanup()

    def play(self, seed, winner_slot=0, stocks=None, **kw):
        stocks = stocks or ((2, 0) if winner_slot == 0 else (0, 1))
        self.assertIsNone(self.ld.report(self.a, self.b, report(seed, 0, stocks, **kw)))
        return self.ld.report(self.b, self.a, report(seed, 1, stocks, **kw))

    # identity
    def test_identity(self):
        nonce = os.urandom(8)
        self.assertEqual(self.ld.identify(self.a, "?", nonce, LD.join_proof(self.ka, nonce)), (self.a, "Roberto"))
        self.assertEqual(self.ld.identify(self.a, "?", nonce, LD.join_proof(self.kb, nonce))[1], "wrong key")
        c = os.urandom(16)
        self.assertEqual(self.ld.identify(c, "Mallory", key=os.urandom(32))[1], "not invited")
        self.ld.open_signup = True
        self.assertEqual(self.ld.identify(c, "roberto", key=os.urandom(32)), (c, "roberto 2"))      # (a free variant)
        d = os.urandom(16)
        self.assertEqual(self.ld.identify(d, "Mallory", key=os.urandom(32)), (d, "Mallory"))

    # accounts
    def join(self, pid, key, name):
        nonce = os.urandom(8)
        return self.ld.identify(pid, name, nonce, LD.join_proof(key, nonce))

    def test_rename_follows_the_game_with_open_signup(self):
        self.assertEqual(self.join(self.a, self.ka, "Robert"), (self.a, "Roberto"))      # (invitation only: names are the admin's)
        self.ld.open_signup = True
        self.assertEqual(self.join(self.a, self.ka, "Robert"), (self.a, "Robert"))
        self.assertEqual(self.join(self.a, self.ka, "fredy"), (self.a, "Robert"))        # (another account's name)
        self.assertEqual(self.join(self.a, self.ka, "Player-1c83"), (self.a, "Robert"))  # (the game's placeholder)
        self.assertEqual(self.join(self.a, self.ka, " x"), (self.a, "Robert"))            # (too short)
        self.assertEqual(self.join(self.a, self.ka, "a\x07b"), (self.a, "Robert"))        # (a control character)
        self.assertEqual(self.join(self.a, self.ka, "  Rob   Soto "), (self.a, "Rob Soto"))
        self.assertEqual(self.ld.player(self.a)[0], "Rob Soto")

    def test_signup_names(self):
        self.ld.open_signup = True
        self.assertEqual(self.ld.identify(os.urandom(16), "Player-0a1b", key=os.urandom(32))[1], "Player-0a1b")
        self.assertEqual(self.ld.identify(os.urandom(16), "", key=os.urandom(32))[1], "Player")
        self.assertEqual(self.ld.identify(os.urandom(16), "x" * 40, key=os.urandom(32))[1], "Player 2")
        long_name = "N" * LD.NAME_MAX
        self.assertEqual(self.ld.identify(os.urandom(16), long_name, key=os.urandom(32))[1], long_name)
        self.assertEqual(self.ld.identify(os.urandom(16), long_name, key=os.urandom(32))[1], "N" * (LD.NAME_MAX - 2) + " 2")

    def test_admin_rename_ban_delete(self):
        self.assertIsNone(self.ld.rename("Roberto", "Beto"))
        self.assertIn("another account", self.ld.rename("Beto", "fredy"))
        self.assertIn("no account", self.ld.rename("Nadie", "X1"))
        self.assertIn("not a valid name", self.ld.rename("Beto", "x"))
        self.assertEqual(self.join(self.a, self.ka, "?"), (self.a, "Beto"))
        self.assertIsNone(self.ld.ban("Beto"))
        self.assertEqual(self.join(self.a, self.ka, "?"), (None, "banned"))
        self.assertIsNone(self.ld.ban("Beto", False))
        self.assertEqual(self.join(self.a, self.ka, "?"), (self.a, "Beto"))
        self.assertEqual([r[0] for r in self.ld.accounts()], ["Beto", "FreDy"])
        self.assertIsNone(self.ld.delete("Beto"))
        self.assertIn("no account", self.ld.delete("Beto"))
        self.assertEqual(self.join(self.a, self.ka, "?")[1], "unknown id")               # (the game signs up again)

    def test_older_database_gets_the_banned_column(self):
        path = os.path.join(self.dir.name, "old.db")
        db = LD.sqlite3.connect(path)
        db.executescript(LD.SCHEMA.replace(", banned INTEGER NOT NULL DEFAULT 0", ""))
        db.close()
        old = LD.Ladder(path, now=self.clock)
        self.assertIn("no account", old.ban("nobody"))
        self.assertIn("banned", [r[1] for r in old.db.execute("PRAGMA table_info(players)")])
        old.db.close()

    def test_invite_is_single_use(self):
        code = self.ld.invite("Third")
        self.assertEqual(self.ld.identify(os.urandom(16), "", key=os.urandom(32), invite=code)[1], "Third")
        self.assertEqual(self.ld.identify(os.urandom(16), "", key=os.urandom(32), invite=code)[1], "invalid invitation")

    # sets
    def test_bo3_changes_ratings(self):
        self.ld.open_set(self.a, self.b)
        self.assertEqual(self.play(1, 0), "ok")
        self.assertEqual(self.play(2, 1), "ok")
        self.assertIsNotNone(self.ld.open_set_of(self.a, self.b))
        self.assertEqual(self.play(3, 0), "ok")
        self.assertIsNone(self.ld.open_set_of(self.a, self.b))
        ra, rb = self.ld.player(self.a), self.ld.player(self.b)
        self.assertGreater(ra[1], 1500)
        self.assertLess(rb[1], 1500)
        self.assertEqual((ra[4], ra[5], rb[4], rb[5]), (1, 0, 0, 1))

    def test_2_0_closes_set(self):
        self.ld.open_set(self.a, self.b)
        self.play(1, 1)
        self.play(2, 1)
        self.assertGreater(self.ld.player(self.b)[1], 1500)

    def test_disputed(self):
        self.ld.open_set(self.a, self.b)
        self.ld.report(self.a, self.b, report(1, 0, (2, 0)))
        self.assertEqual(self.ld.report(self.b, self.a, report(1, 1, (0, 1))), "disputed: reports differ")
        self.assertEqual(self.play(2, desyncs=3), "disputed: desync")
        self.assertEqual(self.play(3, stock=2), "disputed: not ladder rules")
        self.assertEqual(self.play(4, flags=LD.F_DISCONNECT), "disputed: abandoned")
        self.ld.report(self.a, self.b, report(5, 0))
        self.clock.t += LD.REPORT_WAIT + 1
        self.assertEqual(self.ld.sweep(), ["disputed: one report only"])
        self.assertEqual(self.ld.open_set_of(self.a, self.b)[3:], (0, 0))
        self.assertEqual(self.ld.player(self.a)[1], 1500)

    def test_resent_report_after_settle(self):
        self.ld.open_set(self.a, self.b)
        self.assertEqual(self.play(1, 0), "ok")
        self.assertEqual(self.ld.report(self.a, self.b, report(1, 0)), "ok")
        self.clock.t += LD.REPORT_WAIT + 1
        self.assertEqual(self.ld.sweep(), [])
        self.assertEqual(self.ld.open_set_of(self.a, self.b)[3:], (1, 0))
        self.assertEqual((self.ld.rank(self.a), self.ld.rank(os.urandom(16))), (1, 0))

    def test_restart_voids_open_sets(self):
        self.ld.open_set(self.a, self.b)
        self.ld.db.close()
        self.ld = LD.Ladder(self.ld.path, now=self.clock)
        self.assertIsNone(self.ld.open_set_of(self.a, self.b))

    def test_report_from_outsider_ignored(self):
        self.ld.open_set(self.a, self.b)
        self.ld.report(self.a, self.b, report(1, 0))
        self.assertIsNone(self.ld.report(os.urandom(16), self.a, report(1, 1)))

    def test_draw_and_stage(self):
        self.ld.open_set(self.a, self.b)
        self.assertEqual(self.play(1, stocks=(1, 1), dmg=(50, 50)), "draw")
        self.ld.legal_stages = {3}
        self.assertEqual(self.play(2), "disputed: stage not legal")

    def test_void_on_leave(self):
        self.ld.open_set(self.a, self.b)
        self.play(1, 0)
        self.assertEqual(self.ld.void_sets(self.b), 1)
        self.assertIsNone(self.ld.open_set_of(self.a, self.b))
        self.assertEqual(self.ld.player(self.a)[1], 1500)

    # web
    def test_web_documents(self):
        self.ld.open_set(self.a, self.b)
        self.play(1, 0)
        self.play(2, 0)
        t = self.ld.table()
        self.assertEqual([p["name"] for p in t], ["Roberto", "FreDy"])
        self.assertEqual(t[0]["main"], "%016x" % 0x11)
        h = self.ld.history("fredy")
        self.assertEqual(h["sets"][0]["opponent"], "Roberto")
        self.assertEqual(h["sets"][0]["score"], [0, 2])
        self.assertIs(h["sets"][0]["won"], False)
        doc = json.dumps([t, h])
        for secret in (self.a, self.b, self.ka, self.kb):
            self.assertNotIn(secret.hex(), doc)
        self.assertIsNone(self.ld.history("nobody"))


class RankedFake(Fake):
    def __init__(self, lobby, name, invite=None):
        super().__init__(lobby, name, mods=b"\0")
        self.pid, self.key, self.invite = os.urandom(16), os.urandom(32), invite

    def join_ld(self, signup=True, ld=True):
        """JOIN with the ladder block; returns WELCOME's 'L''D' tail as (status, rating, rd, wins, losses, place, text)"""
        port = self.s.getsockname()[1]
        block = b""
        if ld:
            block = b"LD\1" + self.pid + hmac.new(self.key, self.nonce, hashlib.sha256).digest()
            if signup:
                inv = (self.invite or "").encode()
                block += b"\1" + self.key + bytes((len(inv),)) + inv
            else:
                block += b"\0"
        self.tx(L.JOIN, self.nonce + self.code.ljust(32, b"\0") + self.name.encode().ljust(32, b"\0") + self.build.ljust(40, b"\0")
                + struct.pack(">IHB", 0x7F000001, port, 1) + b"MS" + self.mods + block)
        b = self.rx(L.WELCOME)
        self.ip, self.cookie = struct.unpack_from(">IQ", b, 8)
        if len(b) == 20:
            return None
        assert b[20:22] == b"LD", b[20:]
        return (b[22],) + struct.unpack_from(">HHHHH", b, 23) + (b[33:65].rstrip(b"\0").decode(),)

    def result(self, seed, peer, slot, stocks, dmg=(300, 900), desyncs=0):
        self.tx(L.RESULT, struct.pack(">QQIBBBBI", self.cookie, seed, peer, slot, LD.RULE_STOCK, LD.LADDER_STOCKS, 0, 7)
                + struct.pack(">QQBBHHII", 0x11, 0x22, stocks[0], stocks[1], dmg[0], dmg[1], 9000, desyncs))

    def ack(self, seed):
        """the next RESULT_ACK for seed: (outcome, rating, rd, wins, losses, place, my set score, the other's)"""
        while True:
            b = self.rx(L.RESULT_ACK)
            if struct.unpack_from(">Q", b)[0] == seed:
                return (b[8],) + struct.unpack_from(">HHHHH", b, 9) + (b[19], b[20])


class LobbyLadderTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.db = os.path.join(self.dir.name, "ladder.db")
        lobby = [sys.executable, "-I", os.path.join(HERE, "ssbu_lobby.py"), "--ladder-db", self.db]
        self.inv = [subprocess.check_output(lobby + ["--ladder-invite", n], text=True).strip() for n in ("Roberto", "FreDy")]
        self.port, self.sport = free_port(), free_tcp_port()
        self.proc = subprocess.Popen(lobby + ["--bind", "127.0.0.1", "--port", str(self.port), "--stats", "0", "--status-port", str(self.sport)],
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

    def fake(self, name, invite=None):
        f = RankedFake(self.lob, name, invite)
        self.addCleanup(f.s.close)
        return f

    def get(self, path):
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (self.sport, path), timeout=3) as r:
            return r.read().decode()

    def test_accounts_through_the_lobby(self):
        a, b = self.fake("Rob", self.inv[0]), self.fake("Fre", self.inv[1])
        self.assertEqual(a.join_ld()[0::6], (L.LD_RANKED, "Roberto"))
        self.assertEqual(b.join_ld()[0::6], (L.LD_RANKED, "FreDy"))
        a2 = self.fake("Rob")              # the same game again (new nonce): the key proof only
        a2.pid, a2.key = a.pid, a.key
        self.assertEqual(a2.join_ld(signup=False)[0::6], (L.LD_RANKED, "Roberto"))
        a2.key = os.urandom(32)
        a2.nonce = os.urandom(8)
        self.assertEqual(a2.join_ld(signup=False)[0::6], (L.LD_UNRANKED, "wrong key"))
        stranger = self.fake("Who")
        self.assertEqual(stranger.join_ld(signup=False)[0], L.LD_UNKNOWN)
        self.assertIsNone(self.fake("Old").join_ld(ld=False))       # an older game: no ladder, still joined
        # (2026-11-03) a casual rollback match is never ranked: ratings change only with a whole set of the ladder queue
        # (test_ladder_queue.py) or of a rated tournament; every RESULT is still acknowledged (the game stops resending)
        for seed, stocks in ((101, (2, 0)), (102, (0, 1)), (103, (1, 0))):
            a.result(seed, b.ip, 0, stocks)
            self.assertEqual(a.ack(seed)[0], L.OUT_UNRANKED)
            b.result(seed, a.ip, 1, stocks)
            self.assertEqual(b.ack(seed)[0], L.OUT_UNRANKED)
        stranger.result(105, a.ip, 0, (2, 0))
        self.assertEqual(stranger.ack(105)[0], L.OUT_UNRANKED)
        table = json.loads(self.get("/ladder.json"))
        self.assertEqual(sorted((p["name"], p["wins"], p["losses"]) for p in table), [("FreDy", 0, 0), ("Roberto", 0, 0)])
        self.assertEqual(json.loads(self.get("/player/FreDy.json"))["sets"], [])
        self.assertIn("<title>SSBU ladder</title>", self.get("/ladder"))
        doc = self.get("/ladder.json") + self.get("/player/Roberto.json")
        for leak in ("127.0.0.1", "10.13.", a.pid.hex(), a.key.hex(), b.pid.hex(), self.inv[0]):
            self.assertNotIn(leak, doc)


if __name__ == "__main__":
    unittest.main()
