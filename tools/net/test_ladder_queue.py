#!/usr/bin/env python3
"""The ladder queue through ssbu_lobby.py over UDP (2026-11-03): two games with accounts join the queue (TOURNEY op 15),
the lobby pairs them into a ladder match (a one-set tournament: "lk"), the set is played exactly as a tournament set
(stage strikes, ready at the set-up, the rollback search pairs only the two, both RESULTs count a game) and when it is
over both ratings change ("lr", "lq", /ladder.json, /ladder_matches.json). Also: leaving the queue, a player of a
tournament cannot queue, a casual rollback match is never ranked, the tournament list never shows ladder matches.
    python -m unittest tools/net/test_ladder_queue.py        (standard library only)"""
import json
import os
import struct
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ["SSBU_LADDER_GONE_FOR"] = "1"                            # (the lobby under test: a game gone 1 s loses its ladder set)
import ladder as LD  # noqa: E402
import ssbu_lobby as L  # noqa: E402
import test_tournament_lobby as TL  # noqa: E402


def chara(name):
    return (0xc1 << 56) | L.tournament_mod.hash40("ui_chara_" + name)


def result(f, seed, peer, slot, stocks, c0, c1):
    """a RESULT with these fighters (by slot)"""
    f.tx(L.RESULT, struct.pack(">QQIBBBBI", f.cookie, seed, peer, slot, LD.RULE_STOCK, LD.LADDER_STOCKS, 0, 7)
         + struct.pack(">QQBBHHII", c0, c1, stocks[0], stocks[1], 300, 900, 9000, 0))


class LadderQueueTest(TL.TournamentLobbyTest):
    test_a_whole_tournament = test_no_account_no_tournament = test_rules_checked = test_others_not_taken_by_a_tournament_player = None

    def play_set(self, fs, winner_name):
        """play the running ladder set of fs (two fakes) through the protocol; winner_name wins every game"""
        searches, guard = 900, 0
        while guard < 200:
            guard += 1
            vs = [f.view(0, wait=0.05)[1] for f in fs]
            if all("lk" not in v for v in vs):
                return
            acted = False
            for f, v in zip(fs, vs):
                me = v.get("me")
                if not me or me[1] == "-1":
                    continue
                phase, turn = v["phase"][0], v["phase"][1]
                if phase in ("strike", "ban", "pick") and turn == "1":
                    free = [s for s in v["pool"] if s not in v.get("struck", [])]
                    f.view(0, L.T_STAGE, struct.pack(">HB", int(me[1]), len(free[0])) + free[0].encode())
                    acted = True
                elif phase == "chars" and v["ready"][0] == "0":
                    first = v["first"][0]
                    if first == "2" and v["ready"][1] == "0":
                        continue
                    f.view(0, L.T_READY, struct.pack(">QB", chara("mario" if f.name == winner_name else "fox"), 1 if first == "2" else 0))
                    acted = True
            if acted:
                continue
            if all(v.get("go") == ["1"] for v in vs):
                (h, hv), (g, gv) = sorted(zip(fs, vs), key=lambda x: x[1]["me"][4] != "1")
                searches += 1
                h.match(L.KIND_ROLLBACK, searches)
                g.match(L.KIND_ROLLBACK, searches)
                h.until(L.MS_MATCHED)
                g.until(L.MS_MATCHED)
                h.tx(L.RB, struct.pack(">QI", h.cookie, g.ip) + b"x")
                g.tx(L.RB, struct.pack(">QI", g.cookie, h.ip) + b"x")
                time.sleep(0.1)
                stocks = (2, 0) if h.name == winner_name else (0, 1)
                c0, c1 = (chara("mario"), chara("fox")) if h.name == winner_name else (chara("fox"), chara("mario"))
                result(h, 0x7000 + searches, g.ip, 0, stocks, c0, c1)
                result(g, 0x7000 + searches, h.ip, 1, stocks, c0, c1)
                for x in (h, g):
                    x.match(0, 0)
                time.sleep(0.1)
        self.fail("the ladder set did not end: " + "".join(self.lines[-30:]))

    def test_a_ladder_set(self):
        a, b = self.fake("Rober"), self.fake("napky")
        _, v = a.view(0, L.T_LADDER_JOIN)
        self.assertNotIn("err", v)
        self.assertEqual(v["lq"][0], "1")                           # (in the queue, alone)
        self.assertNotIn("lk", v)
        _, v = b.view(0, L.T_LADDER_JOIN)
        time.sleep(0.6)                                             # (the lobby pairs at its next sweep)
        _, va = a.view(0)
        _, vb = b.view(0)
        self.assertIn("lk", va, "".join(self.lines[-20:]))
        self.assertIn("lk", vb)
        self.assertEqual((va["lq"][0], vb["lq"][0]), ("0", "0"))    # (out of the queue: matched)
        self.assertEqual(va["opp"], ["napky"])
        self.assertEqual(va["round"], ["W", "1", "3"])              # (one set, best of 3)
        self.assertEqual(va["t"], [])                               # (a ladder match is not in the tournament list)
        self.play_set([a, b], "Rober")
        _, va = a.view(0)
        _, vb = b.view(0)
        self.assertNotIn("lk", va)
        self.assertEqual(va["lr"][1:4], ["1", "2", "0"])            # (won 2 - 0)
        self.assertEqual(vb["lr"][1:4], ["0", "0", "2"])
        self.assertGreater(int(va["lr"][5]), int(va["lr"][4]))      # (rating up)
        self.assertLess(int(vb["lr"][5]), int(vb["lr"][4]))         # (rating down)
        self.assertEqual(va["lq"][4:6], ["1", "0"])                 # (sets won - lost)
        table = json.loads(self.get("/ladder.json"))
        self.assertEqual([r["name"] for r in table], ["Rober", "napky"])
        self.assertEqual(table[0]["fighters"][0]["key"], "mario")
        ms = json.loads(self.get("/ladder_matches.json"))
        self.assertEqual((ms[0]["players"], ms[0]["score"], ms[0]["winner"]), (["Rober", "napky"], [2, 0], 1))
        self.assertEqual(json.loads(self.get("/tournaments.json")), [])     # (ladder matches are not tournaments)
        t = json.loads(self.get("/tournament/%d.json" % ms[0]["tid"]))
        self.assertEqual(t["kind"], "ladder")
        self.assertEqual(t["sets"][0]["games"][0]["chars"], ["Mario", "Fox"])
        h = json.loads(self.get("/player/Rober.json"))
        self.assertEqual((h["sets"][0]["opponent"], h["sets"][0]["won"], h["sets"][0]["tid"]), ("napky", True, ms[0]["tid"]))
        self.assertIn(b"<title>SSBU ladder</title>", self.get("/ladder"))

    def test_leave_the_queue_and_tournament_players(self):
        a, b, c = self.fake("Rober"), self.fake("napky"), self.fake("Fredy")
        a.view(0, L.T_LADDER_JOIN)
        _, v = a.view(0, L.T_LADDER_LEAVE)
        self.assertEqual(v["lq"][0], "0")
        _, v = c.view(0, L.T_CREATE, b"T".ljust(32, b"\0") + bytes((1, 1, 1, 0, 3)))
        tid = int(v["t"][0][0])
        c.view(tid, L.T_SIGNUP)
        _, v = c.view(0, L.T_LADDER_JOIN)                           # (signed up for a tournament: no ladder meanwhile)
        self.assertIn("tournament", v["err"][0])
        b.view(0, L.T_LADDER_JOIN)
        time.sleep(0.6)
        _, v = b.view(0)
        self.assertEqual(v["lq"][0], "1")                           # (still waiting: nobody else in the queue)
        self.assertNotIn("lk", v)

    def test_a_player_leaves(self):
        a, b = self.fake("Rober"), self.fake("napky")
        a.view(0, L.T_LADDER_JOIN)
        b.view(0, L.T_LADDER_JOIN)
        time.sleep(0.6)
        _, v = a.view(0)
        self.assertIn("lk", v)
        b.tx(L.LEAVE, struct.pack(">Q", b.cookie))                  # (napky closes the game)
        time.sleep(2.0)
        _, v = a.view(0)
        self.assertNotIn("lk", v, "".join(self.lines[-20:]))
        self.assertEqual(v["lr"][1], "1")                           # (Rober wins the set, and it counts)
        self.assertGreater(int(v["lr"][5]), int(v["lr"][4]))

    def to_fight(self, a, b):
        """two queued fakes through the stage strikes to their paired match: (host, guest)"""
        guard = 0
        while guard < 60:
            guard += 1
            vs = [f.view(0, wait=0.05)[1] for f in (a, b)]
            if all(v.get("go") == ["1"] for v in vs):
                break
            for f, v in zip((a, b), vs):
                phase, turn = v["phase"][0], v["phase"][1]
                if phase in ("strike", "ban", "pick") and turn == "1":
                    free = [x for x in v["pool"] if x not in v.get("struck", [])]
                    f.view(0, L.T_STAGE, struct.pack(">HB", int(v["me"][1]), len(free[0])) + free[0].encode())
                elif phase == "chars" and v["ready"][0] == "0":
                    f.view(0, L.T_READY, struct.pack(">QB", chara("mario"), 0))
        (h, hv), (g, gv) = sorted(zip((a, b), vs), key=lambda x: x[1]["me"][4] != "1")
        h.match(L.KIND_ROLLBACK, 1)
        g.match(L.KIND_ROLLBACK, 1)
        h.until(L.MS_MATCHED)
        g.until(L.MS_MATCHED)
        h.tx(L.RB, struct.pack(">QI", h.cookie, g.ip) + b"x")
        g.tx(L.RB, struct.pack(">QI", g.cookie, h.ip) + b"x")
        time.sleep(0.1)
        return h, g

    def test_link_cut_both_on_the_lobby_nobody_blamed(self):
        a, b = self.fake("Rober"), self.fake("napky")
        a.view(0, L.T_LADDER_JOIN)
        b.view(0, L.T_LADDER_JOIN)
        time.sleep(0.6)
        h, g = self.to_fight(a, b)
        for f, o, slot in ((h, g, 0), (g, h, 1)):                    # (both games saw the other go silent, 3 min in)
            f.tx(L.RESULT, struct.pack(">QQIBBBBI", f.cookie, 0x8888, o.ip, slot, LD.RULE_STOCK, LD.LADDER_STOCKS, 2, 7)
                 + struct.pack(">QQBBHHII", chara("mario"), chara("fox"), 2, 1, 300, 900, 3 * 3600, 0))
        time.sleep(0.5)
        _, v = a.view(0)
        self.assertIn("lk", v)                                      # (the set goes on: the game is played again)
        self.assertIn("not counted", v["err"][0])
        self.assertEqual(v["score"][:2], ["0", "0"])

    def test_disconnect_after_two_minutes_loses(self):
        a, b = self.fake("Rober"), self.fake("napky")
        a.view(0, L.T_LADDER_JOIN)
        self.assertEqual(json.loads(self.get("/ladder_queue.json"))["searching"], 1)
        b.view(0, L.T_LADDER_JOIN)
        time.sleep(0.6)
        self.assertEqual(json.loads(self.get("/ladder_queue.json")), {"searching": 0, "playing": 1})
        guard = 0
        while guard < 60:                                           # (to the fight: strikes, both ready, paired)
            guard += 1
            vs = [f.view(0, wait=0.05)[1] for f in (a, b)]
            if all(v.get("go") == ["1"] for v in vs):
                break
            for f, v in zip((a, b), vs):
                phase, turn = v["phase"][0], v["phase"][1]
                if phase in ("strike", "ban", "pick") and turn == "1":
                    free = [s for s in v["pool"] if s not in v.get("struck", [])]
                    f.view(0, L.T_STAGE, struct.pack(">HB", int(v["me"][1]), len(free[0])) + free[0].encode())
                elif phase == "chars" and v["ready"][0] == "0":
                    f.view(0, L.T_READY, struct.pack(">QB", chara("mario"), 0))
        (h, hv), (g, gv) = sorted(zip((a, b), vs), key=lambda x: x[1]["me"][4] != "1")
        h.match(L.KIND_ROLLBACK, 1)
        g.match(L.KIND_ROLLBACK, 1)
        h.until(L.MS_MATCHED)
        g.until(L.MS_MATCHED)
        h.tx(L.RB, struct.pack(">QI", h.cookie, g.ip) + b"x")
        g.tx(L.RB, struct.pack(">QI", g.cookie, h.ip) + b"x")
        time.sleep(0.1)
        # napky (losing) pulls the cable after 3 minutes of fight: its game leaves the lobby, Rober's game reports the cut
        nap, other = b, a
        slot = 0 if other is h else 1
        other.tx(L.RESULT, struct.pack(">QQIBBBBI", other.cookie, 0x7777, nap.ip, slot, LD.RULE_STOCK, LD.LADDER_STOCKS, 2, 7)
                 + struct.pack(">QQBBHHII", chara("mario"), chara("fox"), 2, 1, 300, 900, 3 * 3600, 0))
        time.sleep(0.2)
        nap.tx(L.LEAVE, struct.pack(">Q", nap.cookie))              # (then napky's game drops off the lobby too)
        time.sleep(2.5)
        _, v = a.view(0)
        self.assertNotIn("lk", v, "".join(self.lines[-20:]))
        self.assertEqual(v["lr"][1], "1")                           # (Rober wins the set: napky lost the connection)
        self.assertGreater(int(v["lr"][5]), int(v["lr"][4]))
        self.assertTrue(any("lost the connection" in l for l in self.lines))

    def test_ladder_game_acks_waiting_then_counted(self):
        """(2026-11-08) a ladder set's game is acked "waiting" by its first RESULT, and both games learn "counted" once the two
        RESULTs agree (before, every RESULT was acked "not ranked", so the game showed a ranked match as unranked)"""
        a, b = self.fake("Rober"), self.fake("napky")
        a.view(0, L.T_LADDER_JOIN)
        b.view(0, L.T_LADDER_JOIN)
        time.sleep(0.6)
        h, g = self.to_fight(a, b)
        seed = 0x6666
        result(h, seed, g.ip, 0, (2, 1), chara("mario"), chara("fox"))
        self.assertEqual(h.ack(seed)[0], L.OUT_WAIT)
        result(g, seed, h.ip, 1, (2, 1), chara("mario"), chara("fox"))
        self.assertEqual(g.ack(seed)[0], L.OUT_WAIT)
        self.assertEqual(h.ack(seed)[0], L.OUT_OK)
        self.assertEqual(g.ack(seed)[0], L.OUT_OK)

    def test_casual_match_not_ranked(self):
        a, b = self.fake("Rober"), self.fake("napky")
        a.match(L.KIND_ROLLBACK, 1)
        b.match(L.KIND_ROLLBACK, 1)
        a.until(L.MS_MATCHED)
        b.until(L.MS_MATCHED)
        for _ in range(2):
            a.result(0x9000, b.ip, 0, (2, 0))
            b.result(0x9000, a.ip, 1, (2, 0))
        time.sleep(0.2)
        table = json.loads(self.get("/ladder.json"))
        self.assertEqual([(r["wins"], r["losses"]) for r in table], [(0, 0), (0, 0)])

    def test_casual_match_counts_the_score(self):
        """(2026-11-08) the win counter (1.8 score) is the casual match's too: both games send the same RESULT, the
        stocks left are the rule's stocks less the falls (flag 4), and the lobby says who beat whom"""
        a, b = self.fake("Rober"), self.fake("napky")
        a.match(L.KIND_ROLLBACK, 1)
        b.match(L.KIND_ROLLBACK, 1)
        a.until(L.MS_MATCHED)
        b.until(L.MS_MATCHED)
        flags = L.F_STOCKS_FALLS
        for f, o, slot in ((a, b, 0), (b, a, 1)):
            f.tx(L.RESULT, struct.pack(">QQIBBBBI", f.cookie, 0x9100, o.ip, slot, LD.RULE_STOCK, 3, flags, 7)
                 + struct.pack(">QQBBHHII", chara("mario"), chara("fox"), 3, 1, 0, 0, 9000, 0))
        time.sleep(0.3)
        self.assertTrue(any("score: Rober beat napky" in l for l in self.lines), "".join(self.lines[-10:]))

    def test_abandoned_finish_still_counts_the_winner(self):
        """(2026-11-08) a match that ends with both still holding stocks (time-up, or a leaver) counts for the one with more"""
        a, b = self.fake("Rober"), self.fake("napky")
        a.match(L.KIND_ROLLBACK, 1)
        b.match(L.KIND_ROLLBACK, 1)
        a.until(L.MS_MATCHED)
        b.until(L.MS_MATCHED)
        flags = L.F_STOCKS_FALLS | 1                                # 1 = abandoned, as the game marks it when both still hold stocks
        for f, o, slot in ((a, b, 0), (b, a, 1)):
            f.tx(L.RESULT, struct.pack(">QQIBBBBI", f.cookie, 0x9200, o.ip, slot, LD.RULE_STOCK, 3, flags, 7)
                 + struct.pack(">QQBBHHII", chara("mario"), chara("fox"), 2, 1, 0, 0, 9000, 0))
        time.sleep(0.3)
        self.assertTrue(any("score: Rober beat napky" in l for l in self.lines), "".join(self.lines[-10:]))


if __name__ == "__main__":
    unittest.main()
