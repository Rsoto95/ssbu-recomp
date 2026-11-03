#!/usr/bin/env python3
"""Unit tests of tournament.py: brackets of 2 to 64 players played to the end with random winners (double elimination:
nobody out before two set losses, one champion, every player placed, standard places), byes, the grand final reset, the
stage procedure (strikes 1-2-1, bans, the loser's pick), the fighter order (the winner first), DQ, the organizer's
corrections, seeding (rating, random, manual) and the store (a lobby restarted in the middle of a tournament).
    python -m unittest tools/net/test_tournament.py        (standard library only)"""
import os
import random
import sqlite3
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tournament as T  # noqa: E402


def make(n, seed=1, rules=None, ratings=True):
    t = T.Tournament(1, "Test", "org", rules, salt=seed)
    for i in range(n):
        t.sign_up("p%02d" % i, "Player %d" % i, 2000 - i if ratings else None)
    return t


def play_game(t, sid, winner, rnd=None):
    """one game of set sid through the procedure: stage steps by whoever's turn it is, both ready, then the result"""
    p = t.proc(sid)
    a, b = t.view[sid]["p"]
    while p["phase"] in ("strike", "ban", "pick"):
        who = (a, b)[p["turn"]]
        free = [s for s in p["pool"] if s not in p["struck"]]
        t.stage_step(sid, who, (rnd or random).choice(free))
    first = t.first_pick(sid)
    order = [first, 1 - first] if first is not None else [0, 1]
    for side in order:
        t.ready(sid, (a, b)[side], 0xc100000000000000 + side)
    assert t.go(sid)
    return t.game_result(sid, winner)


def run(t, rnd):
    guard = 0
    while t.state == "running":
        live = [i for i, v in enumerate(t.view) if v["state"] in ("ready", "playing")]
        assert live, "running with nothing to play"
        sid = rnd.choice(live)
        play_game(t, sid, rnd.randrange(2), rnd)
        guard += 1
        assert guard < 5000


class BracketTest(unittest.TestCase):
    def test_every_size_to_the_end(self):
        for n in range(2, 65):
            for rep in range(3):
                rnd = random.Random(n * 100 + rep)
                t = make(n, seed=rep)
                t.start()
                run(t, rnd)
                self.assertEqual(t.state, "done", n)
                losses = [0] * n
                for i, v in enumerate(t.view):
                    if v["state"] == "done":
                        losses[v["loser"]] += 1
                champ = t.champion()
                self.assertIsNotNone(champ)
                self.assertLessEqual(losses[champ], 1, n)
                self.assertEqual(sorted(x for i, x in enumerate(losses) if i != champ), sorted([2] * (n - 1)) if True else None, n)
                places = t.standings()
                self.assertEqual(len(places), n, n)
                self.assertEqual(sorted(e for _, e in places), list(range(n)))
                self.assertEqual([p for p, _ in places][:2], [1, 2], n)

    def test_standard_places_of_8(self):
        t = make(8)
        t.start()
        run(t, random.Random(5))
        self.assertEqual(sorted(p for p, _ in t.standings()), [1, 2, 3, 4, 5, 5, 7, 7])

    def test_byes_go_to_the_top_seeds(self):
        t = make(5)
        t.start()
        w1 = [v for s, v in zip(t.sets, t.view) if s["br"] == "W" and s["round"] == 1]
        byes = sorted(next(x for x in v["p"] if x != T.BYE) for v in w1 if T.BYE in v["p"])
        self.assertEqual(byes, [0, 1, 2])             # seeds 1-3 (5 players in a bracket of 8)
        self.assertEqual(sorted(v["p"] for v in w1 if T.BYE not in v["p"]), [[3, 4]])

    def test_first_round_pairs(self):
        t = make(8)
        t.start()
        w1 = [v["p"] for s, v in zip(t.sets, t.view) if s["br"] == "W" and s["round"] == 1]
        self.assertEqual(w1, [[0, 7], [3, 4], [1, 6], [2, 5]])

    def grand_final(self, losers_side_wins):
        t = make(2, rules={"bo": 1, "bo_top8": 1, "bo_finals": 1})
        t.start()
        play_game(t, 0, 0)                            # (winners final: seed 1 wins)
        g1 = len(t.sets) - 2
        self.assertEqual(t.view[g1]["p"], [0, 1])
        play_game(t, g1, 1 if losers_side_wins else 0)
        return t

    def test_grand_final_without_reset(self):
        t = self.grand_final(False)
        self.assertEqual(t.state, "done")
        self.assertEqual(t.view[-1]["state"], "skipped")
        self.assertEqual(t.champion(), 0)

    def test_grand_final_reset(self):
        t = self.grand_final(True)
        self.assertEqual(t.state, "running")
        self.assertEqual(t.view[-1]["state"], "ready")
        play_game(t, len(t.sets) - 1, t.view[-1]["p"].index(1))
        self.assertEqual((t.state, t.champion()), ("done", 1))
        self.assertEqual(t.standings(), [(1, 1), (2, 0)])

    def test_best_of(self):
        t = make(16, rules={"bo": 3, "bo_top8": 5, "bo_finals": 7})
        t.start()
        bo = {(s["br"], s["round"]): v["bo"] for s, v in zip(t.sets, t.view)}
        self.assertEqual(bo[("W", 1)], 3)
        self.assertEqual(bo[("W", 3)], 5)              # (winners semis: 4 players)
        self.assertEqual(bo[("W", 4)], 7)              # winners final
        self.assertEqual(bo[("L", max(r for b, r in bo if b == "L"))], 7)
        self.assertEqual(bo[("G", 1)], 7)

    def test_set_score(self):
        t = make(2, rules={"bo_finals": 3})
        t.start()
        self.assertEqual(play_game(t, 0, 0), "game")
        self.assertEqual(t.view[0]["state"], "playing")
        self.assertEqual(play_game(t, 0, 1), "game")
        self.assertEqual(play_game(t, 0, 0), "set")
        self.assertEqual(t.score(0), [2, 1])
        self.assertEqual(t.view[0]["winner"], 0)


class ProcedureTest(unittest.TestCase):
    def setUp(self):
        self.t = make(2)
        self.t.start()

    def test_strikes_1_2_1(self):
        p = self.t.proc(0)
        self.assertEqual((p["phase"], p["counts"]), ("strike", [1, 2, 1]))
        first = p["turn"]
        a = self.t.view[0]["p"]
        with self.assertRaises(T.Error):
            self.t.stage_step(0, a[1 - first], T.STARTERS[0])
        self.t.stage_step(0, a[first], T.STARTERS[0])
        self.assertEqual((p["turn"], p["left"]), (1 - first, 2))
        with self.assertRaises(T.Error):
            self.t.stage_step(0, a[1 - first], T.STARTERS[0])       # (struck already)
        self.t.stage_step(0, a[1 - first], T.STARTERS[1])
        self.t.stage_step(0, a[1 - first], T.STARTERS[2])
        self.assertEqual((p["turn"], p["left"]), (first, 1))
        self.t.stage_step(0, a[first], T.STARTERS[3])
        self.assertEqual((p["phase"], p["stage"]), ("chars", T.STARTERS[4]))
        with self.assertRaises(T.Error):
            self.t.stage_step(0, a[first], T.STARTERS[4])

    def test_first_striker_is_drawn(self):
        firsts = set()
        for salt in range(20):
            t = make(2, seed=salt)
            t.start()
            firsts.add(t.proc(0)["turn"])
        self.assertEqual(firsts, {0, 1})

    def test_bans_then_the_losers_pick_and_fighter_order(self):
        t, (a, b) = self.t, self.t.view[0]["p"]
        play_game(t, 0, 1)                            # (b won game 1)
        p = t.proc(0)
        self.assertEqual((p["phase"], p["turn"], p["left"], p["game"]), ("ban", 1, 2, 2))
        self.assertEqual(len(p["pool"]), len(T.STARTERS) + len(T.COUNTERPICKS))
        t.stage_step(0, b, "end")
        t.stage_step(0, b, "battle_field")
        self.assertEqual((p["phase"], p["turn"]), ("pick", 0))
        with self.assertRaises(T.Error):
            t.stage_step(0, a, "end")                 # (banned)
        t.stage_step(0, a, "poke_kalos")
        self.assertEqual((p["phase"], p["stage"]), ("chars", "poke_kalos"))
        self.assertEqual(t.first_pick(0), 1)
        t.ready(0, a, 0xc1aa)                         # the loser chose before the winner: not yet
        t.ready(0, b, 0xc1bb)
        self.assertFalse(t.go(0))
        t.ready(0, a, 0xc1aa, confirm=True)           # ... until it saw the winner's fighter and confirmed
        self.assertTrue(t.go(0))
        self.assertEqual(t.game_result(0, 0), "game")
        self.assertEqual(t.results["0"]["games"][-1]["stage"], "poke_kalos")

    def test_loser_after_winner_needs_no_confirm(self):
        t, (a, b) = self.t, self.t.view[0]["p"]
        play_game(t, 0, 0)
        p = t.proc(0)
        while p["phase"] != "chars":
            who = (a, b)[p["turn"]]
            t.stage_step(0, who, next(s for s in p["pool"] if s not in p["struck"]))
        t.ready(0, a, 1)
        self.assertFalse(t.go(0))
        t.ready(0, b, 2)
        self.assertTrue(t.go(0))
        t.unready(0, a)                               # (the winner went back to change)
        self.assertFalse(t.go(0))

    def test_playing_ends_ready_until_the_result(self):
        t, (a, b) = self.t, self.t.view[0]["p"]
        p = t.proc(0)
        while p["phase"] != "chars":
            t.stage_step(0, (a, b)[p["turn"]], next(s for s in p["pool"] if s not in p["struck"]))
        t.ready(0, a, 1)
        t.ready(0, b, 2)
        self.assertTrue(t.playing(0))
        self.assertFalse(t.go(0))
        with self.assertRaises(T.Error):
            t.ready(0, a, 1)
        t.game_result(0, 0)
        self.assertEqual(t.proc(0)["phase"], "ban")

    def test_not_your_set(self):
        t = make(4)
        t.start()
        with self.assertRaises(T.Error):
            t.stage_step(0, 1, "end")                 # (set 0 is seed 1 v seed 4)


class OrganizerTest(unittest.TestCase):
    def test_dq_forfeits_the_rest(self):
        t = make(4)
        t.start()
        t.dq(3)                                       # seed 4 (in set 0 against seed 1)
        self.assertEqual(t.view[0]["state"], "forfeit")
        self.assertEqual(t.view[0]["winner"], 0)
        run(t, random.Random(2))
        self.assertEqual(t.state, "done")
        self.assertNotEqual(t.champion(), 3)

    def test_override_and_correction(self):
        t = make(4, rules={"bo": 1, "bo_top8": 1, "bo_finals": 1})
        t.start()
        t.override(0, 1)                              # (seed 4 beat seed 1, says the organizer)
        self.assertEqual(t.view[0]["winner"], 3)
        w2 = next(i for i, s in enumerate(t.sets) if s["br"] == "W" and s["round"] == 2)
        play_game(t, 1, 0)
        self.assertEqual(sorted(t.view[w2]["p"]), [1, 3])         # (set 1: seed 2 v seed 3, seed 2 won)
        play_game(t, w2, t.view[w2]["p"].index(3))
        t.override(0, 0)                              # a correction: seed 1 won after all
        self.assertEqual(sorted(t.view[w2]["p"]), [0, 1])
        self.assertNotIn(str(w2), t.results)          # (the later set's result had other players: dropped)
        t.override(0, None)
        self.assertEqual(t.view[0]["state"], "ready")

    def test_withdraw(self):
        t = make(3)
        t.withdraw("p01")
        self.assertEqual([e["pid"] for e in t.entrants], ["p00", "p02"])
        t.start()
        t.withdraw("p00")
        self.assertTrue(t.entrants[0]["dq"])
        with self.assertRaises(T.Error):
            t.sign_up("p09", "Late")


class SeedingTest(unittest.TestCase):
    def test_rating_order_unrated_last(self):
        t = T.Tournament(1, "S", "org", salt=3)
        t.sign_up("a", "A", None)
        t.sign_up("b", "B", 1500)
        t.sign_up("c", "C", 1800)
        t.sign_up("d", "D", None)
        self.assertEqual([e["pid"] for e in t.entrants][:2], ["c", "b"])
        self.assertEqual(sorted(e["pid"] for e in t.entrants[2:]), ["a", "d"])

    def test_manual(self):
        t = make(4)
        t.move(3, 0)
        self.assertEqual((t.seed_mode, [e["pid"] for e in t.entrants]), ("manual", ["p03", "p00", "p01", "p02"]))
        t.sign_up("p09", "Late", 9999)                # (manual: a new player goes last, nothing moves)
        self.assertEqual(t.entrants[-1]["pid"], "p09")
        self.assertEqual(t.entrants[0]["pid"], "p03")
        t.start()
        self.assertEqual(t.view[0]["p"][0], 0)        # (seed 1 = p03)
        self.assertEqual(t.entrants[0]["pid"], "p03")
        with self.assertRaises(T.Error):
            t.move(0, 1)

    def test_modes(self):
        t = make(6)
        t.set_seed_mode("random")
        self.assertEqual(sorted(e["pid"] for e in t.entrants), ["p0%d" % i for i in range(6)])
        t.set_seed_mode("rating")
        self.assertEqual([e["pid"] for e in t.entrants], ["p0%d" % i for i in range(6)])
        with self.assertRaises(T.Error):
            t.set_seed_mode("best")

    def test_too_few(self):
        with self.assertRaises(T.Error):
            make(1).start()


class StoreTest(unittest.TestCase):
    def test_restart_in_the_middle(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "ladder.db")
            db = sqlite3.connect(path)
            st = T.Store(db)
            t = st.create("Weekly", "org", {"bo": 1})
            for i in range(6):
                t.sign_up("p%d" % i, "P%d" % i, 1500 + i)
            t.start()
            live = [i for i, x in enumerate(t.view) if x["state"] == "ready"]
            play_game(t, live[0], 1)
            p = t.proc(live[1])
            a = t.view[live[1]]["p"][p["turn"]]
            t.stage_step(live[1], a, p["pool"][0])
            st.save(t)
            v = (t.version, [x["p"] for x in t.view], t.proc(live[1])["struck"])
            db.close()
            db = sqlite3.connect(path)
            st2 = T.Store(db)
            t2 = st2.get(t.id)
            self.assertEqual((t2.version, [x["p"] for x in t2.view], t2.proc(live[1])["struck"]), v)
            run(t2, random.Random(1))
            st2.save(t2)
            self.assertEqual(st2.get(t.id).state, "done")         # (finished: still shown, for a while)
            self.assertEqual(st2.load(t.id).state, "done")
            self.assertEqual(st2.recent(), [t.id])
            db.close()


class ClockTest(unittest.TestCase):
    """(2026-11-02) check-in, the stage step's time limit, no-shows, featured sets"""

    def clock(self):
        self.t_now = [1000.0]
        return lambda: self.t_now[0]

    def test_checkin_leaves_out_who_did_not(self):
        t = make(4)
        t.open_checkin()
        for pid in ("p00", "p01", "p02"):
            t.check_in(pid)
        t.sign_up("late", "Late", None)               # (a sign-up during the check-in is checked in)
        t.start()
        self.assertEqual(sorted(e["pid"] for e in t.entrants), ["late", "p00", "p01", "p02"])

    def test_checkin_needs_it_open(self):
        t = make(2)
        with self.assertRaises(T.Error):
            t.check_in("p00")

    def test_stage_turn_times_out(self):
        now = self.clock()
        t = T.Tournament(1, "T", "org", {"turn_for": 60, "noshow": 0}, salt=1, now=now)
        for i in range(2):
            t.sign_up("p%d" % i, "P%d" % i, None)
        t.start()
        sid = next(i for i, v in enumerate(t.view) if v["state"] == "ready")
        p = t.proc(sid)
        self.assertEqual(p["phase"], "strike")
        self.t_now[0] += 61
        done = t.sweep()
        self.assertTrue(done and "timed out" in done[0][1])
        self.assertEqual(len(t.proc(sid)["struck"]), 1)

    def test_noshow(self):
        now = self.clock()
        t = T.Tournament(1, "T", "org", {"noshow": 600, "turn_for": 0}, salt=1, now=now)
        for i in range(2):
            t.sign_up("p%d" % i, "P%d" % i, None)
        t.start()
        sid = next(i for i, v in enumerate(t.view) if v["state"] == "ready")
        a, b = t.view[sid]["p"]
        p = t.proc(sid)
        while p["phase"] in ("strike", "ban", "pick"):
            t.stage_step(sid, (a, b)[p["turn"]], [x for x in p["pool"] if x not in p["struck"]][0])
        t.ready(sid, a, 0xc100000000000001)          # (only a came to the set-up)
        self.t_now[0] += 300
        self.assertEqual(t.sweep(), [])
        self.t_now[0] += 400
        done = t.sweep()
        self.assertTrue(done and "no-show" in done[0][1])
        self.assertEqual(t.view[sid]["winner"], a)
        t.override(sid, None)                         # (the organizer can undo it)
        self.assertIn(t.view[sid]["state"], ("ready", "playing"))

    def test_feature(self):
        t = make(2)
        t.start()
        t.feature(0, "ab-12c")
        self.assertEqual(t.featured(0), "AB12C")
        t.feature(0, "")
        self.assertEqual(t.featured(0), "")


if __name__ == "__main__":
    unittest.main()
