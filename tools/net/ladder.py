#!/usr/bin/env python3
"""Ranked ladder of the lobby (ssbu_lobby.py --ladder-db): Glicko-2 ratings over Bo3 sets of online rollback matches.

A closed group: a player is ranked only once the lobby knows its ladder id (a game's random 16-byte id, with a 32-byte
key that proves it, see the 'L''D' block of JOIN). An id is admitted by a one-use invitation code (--ladder-invite) or,
with --ladder-open, the first time it is seen; its key is then kept (trust on first use) and its name is bound to it.
That id is the player's ACCOUNT (the game shows it, and an "account code" moves it to another PC). With --ladder-open the
name follows the game's: a JOIN with another valid name renames the account when no other account has it (a name already
taken at the sign-up gets a free variant, "Name 2"); the game's own placeholder "Player-xxxx" never renames one. The
admin renames, bans and deletes accounts (ssbu_lobby.py --account-*).

A set opens when two ranked players are paired for a rollback match; each game of it is counted only when BOTH games
reported the same result for the same match seed (the handshake's) without a desync, under the ladder's rules (stock,
LADDER_STOCKS stocks, a legal stage). The first to win LADDER_TO games closes the set and both ratings change: one
Glicko-2 rating period per set (Glickman, "Example of the Glicko-2 system", 2012). A game reported differently, by one
game only (REPORT_WAIT seconds) or with a desync is disputed and does not count; a set left half-played is void.

Python 3.8+, standard library only (sqlite3). No game content."""
import hashlib
import hmac
import math
import re
import secrets
import sqlite3
import time

GLICKO_SCALE = 173.7178
RATING0, RD0, VOL0, TAU = 1500.0, 350.0, 0.06, 0.5
RULE_STOCK = 1                    # handshake rule_kind of a stock match (debug_mode.c)
LADDER_STOCKS, LADDER_TO = 3, 2   # 3 stocks, best of 3
REPORT_WAIT = 60.0                # seconds the second report of a game is waited for
ID_LEN, KEY_LEN = 16, 32
NAME_MIN, NAME_MAX = 2, 24        # an account name: printable, no spaces at its ends
PLACEHOLDER = re.compile(r"^Player-[0-9a-fA-F]{4}$")   # (net_relay.c: the name of a game without one, new at each start)
F_ABANDON, F_DISCONNECT = 1, 2    # RESULT flags


# ---- Glicko-2
def glicko2(rating, rd, vol, results, tau=TAU):
    """One rating period of a player (rating, rd, vol) against results [(opp rating, opp rd, score 1 / 0.5 / 0)].
    Returns the new (rating, rd, vol). No results: only the RD grows."""
    mu, phi = (rating - RATING0) / GLICKO_SCALE, rd / GLICKO_SCALE
    if not results:
        return rating, min(math.sqrt(phi * phi + vol * vol) * GLICKO_SCALE, RD0), vol
    g = lambda p: 1.0 / math.sqrt(1.0 + 3.0 * p * p / (math.pi * math.pi))
    v_inv = delta_sum = 0.0
    for r, d, s in results:
        mu_j, phi_j = (r - RATING0) / GLICKO_SCALE, d / GLICKO_SCALE
        e = 1.0 / (1.0 + math.exp(-g(phi_j) * (mu - mu_j)))
        v_inv += g(phi_j) ** 2 * e * (1.0 - e)
        delta_sum += g(phi_j) * (s - e)
    v = 1.0 / v_inv
    delta = v * delta_sum
    a = math.log(vol * vol)

    def f(x):
        ex = math.exp(x)
        return ex * (delta * delta - phi * phi - v - ex) / (2.0 * (phi * phi + v + ex) ** 2) - (x - a) / (tau * tau)
    big_a = a
    if delta * delta > phi * phi + v:
        big_b = math.log(delta * delta - phi * phi - v)
    else:
        k = 1
        while f(a - k * tau) < 0:
            k += 1
        big_b = a - k * tau
    fa, fb = f(big_a), f(big_b)
    while abs(big_b - big_a) > 1e-6:
        c = big_a + (big_a - big_b) * fa / (fb - fa)
        fc = f(c)
        if fc * fb <= 0:
            big_a, fa = big_b, fb
        else:
            fa /= 2.0
        big_b, fb = c, fc
    vol2 = math.exp(big_a / 2.0)
    phi_star = math.sqrt(phi * phi + vol2 * vol2)
    phi2 = 1.0 / math.sqrt(1.0 / (phi_star * phi_star) + 1.0 / v)
    mu2 = mu + phi2 * phi2 * delta_sum
    return mu2 * GLICKO_SCALE + RATING0, phi2 * GLICKO_SCALE, vol2


# ---- one game's result
def game_winner(stocks, dmg):
    """0 / 1: the slot that won (more stocks; equal stocks, time out: less damage), None: a draw (it does not count)."""
    if stocks[0] != stocks[1]:
        return 0 if stocks[0] > stocks[1] else 1
    if dmg[0] != dmg[1]:
        return 0 if dmg[0] < dmg[1] else 1
    return None


def clean_name(name):
    """name as an account name, or "" when it cannot be one (too short or long, control characters)"""
    name = " ".join(str(name or "").split())
    if not NAME_MIN <= len(name) <= NAME_MAX or not name.isprintable():
        return ""
    return name


def join_proof(key, nonce):
    """The 'L''D' proof of a JOIN: HMAC-SHA256(key, the JOIN's nonce)."""
    return hmac.new(key, nonce, hashlib.sha256).digest()


SCHEMA = """
CREATE TABLE IF NOT EXISTS seasons (id INTEGER PRIMARY KEY, name TEXT NOT NULL, started REAL NOT NULL, ended REAL);
CREATE TABLE IF NOT EXISTS players (
    id BLOB PRIMARY KEY, key BLOB NOT NULL, name TEXT NOT NULL UNIQUE COLLATE NOCASE,
    rating REAL NOT NULL, rd REAL NOT NULL, vol REAL NOT NULL,
    wins INTEGER NOT NULL DEFAULT 0, losses INTEGER NOT NULL DEFAULT 0, created REAL NOT NULL, banned INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS invites (code TEXT PRIMARY KEY, name TEXT NOT NULL, created REAL NOT NULL, used_by BLOB);
CREATE TABLE IF NOT EXISTS sets (
    id INTEGER PRIMARY KEY, season INTEGER NOT NULL, p1 BLOB NOT NULL, p2 BLOB NOT NULL,
    opened REAL NOT NULL, closed REAL, state TEXT NOT NULL DEFAULT 'open',
    winner INTEGER, w1 INTEGER NOT NULL DEFAULT 0, w2 INTEGER NOT NULL DEFAULT 0,
    r1_before REAL, r1_after REAL, r2_before REAL, r2_after REAL);
CREATE TABLE IF NOT EXISTS games (
    id INTEGER PRIMARY KEY, set_id INTEGER, seed INTEGER NOT NULL, at REAL NOT NULL, state TEXT NOT NULL,
    why TEXT, stage INTEGER, chara1 INTEGER, chara2 INTEGER, stocks1 INTEGER, stocks2 INTEGER,
    dmg1 INTEGER, dmg2 INTEGER, frames INTEGER, winner INTEGER);
"""


class Ladder:
    """The ladder database. Single-threaded writer (the lobby's loop); the HTTP thread reads through its own connection
    (snapshot())."""

    def __init__(self, path, open_signup=False, legal_stages=None, now=time.time):
        self.path, self.open_signup, self.legal_stages, self.now = path, open_signup, legal_stages, now
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute("PRAGMA journal_mode=WAL")      # (the web thread reads while the lobby writes)
        self.db.executescript(SCHEMA)
        if "banned" not in [r[1] for r in self.db.execute("PRAGMA table_info(players)")]:       # (a database of lobby 1.5 / 1.6)
            self.db.execute("ALTER TABLE players ADD COLUMN banned INTEGER NOT NULL DEFAULT 0")
        if "tid" not in [r[1] for r in self.db.execute("PRAGMA table_info(sets)")]:            # (2026-11-03) the set's match page
            self.db.execute("ALTER TABLE sets ADD COLUMN tid INTEGER")
        self.db.execute("UPDATE sets SET state = 'void', closed = ? WHERE state = 'open'", (self.now(),))   # (a restarted lobby: those sets' games are lost)
        if self.db.execute("SELECT COUNT(*) FROM seasons WHERE ended IS NULL").fetchone()[0] == 0:
            self.db.execute("INSERT INTO seasons (name, started) VALUES (?, ?)", ("Season 1", self.now()))
        self.db.commit()
        self.pending = {}         # match seed -> {"at": time, "reports": {player id: report}, "pair": the two ids}
        self.settled = {}         # match seed -> outcome (a game sends its RESULT again until it is acknowledged)

    def season(self):
        return self.db.execute("SELECT id FROM seasons WHERE ended IS NULL ORDER BY id DESC").fetchone()[0]

    # ---- identity
    def invite(self, name):
        """A one-use invitation code for a player to be called name."""
        code = secrets.token_urlsafe(6)
        self.db.execute("INSERT INTO invites (code, name, created) VALUES (?, ?, ?)", (code, name, self.now()))
        self.db.commit()
        return code

    def identify(self, pid, name, nonce=None, proof=None, key=None, invite=None):
        """The ladder id of a joining player: (id, name) if it is ranked, else (None, why). A known id must prove its key
        (proof = join_proof(key, nonce)); an unknown one is admitted with its key by an invitation code or --ladder-open."""
        if len(pid) != ID_LEN:
            return None, "bad id"
        row = self.db.execute("SELECT key, name, banned FROM players WHERE id = ?", (pid,)).fetchone()
        if row is not None:
            if proof is None or nonce is None or not hmac.compare_digest(join_proof(row[0], nonce), proof):
                return None, "wrong key"
            if row[2]:
                return None, "banned"
            want = clean_name(name)
            if self.open_signup and want and want != row[1] and not PLACEHOLDER.match(want) and self.name_free(want, pid):
                self.db.execute("UPDATE players SET name = ? WHERE id = ?", (want, pid))
                self.db.commit()
                return pid, want
            return pid, row[1]
        if key is None or len(key) != KEY_LEN:
            return None, "unknown id"
        inv = None
        if invite:
            inv = self.db.execute("SELECT name FROM invites WHERE code = ? AND used_by IS NULL", (invite,)).fetchone()
            if inv is None:
                return None, "invalid invitation"
            name = inv[0]
        elif not self.open_signup:
            return None, "not invited"
        else:
            name = self.free_variant(clean_name(name) or "Player")
        if self.db.execute("SELECT 1 FROM players WHERE name = ?", (name,)).fetchone():
            return None, "name taken"
        self.db.execute("INSERT INTO players (id, key, name, rating, rd, vol, created) VALUES (?, ?, ?, ?, ?, ?, ?)",
                        (pid, key, name, RATING0, RD0, VOL0, self.now()))
        if inv is not None:
            self.db.execute("UPDATE invites SET used_by = ? WHERE code = ?", (pid, invite))
        self.db.commit()
        return pid, name

    def name_free(self, name, pid=None):
        row = self.db.execute("SELECT id FROM players WHERE name = ?", (name,)).fetchone()
        return row is None or row[0] == pid

    def free_variant(self, name):
        """name, or "name 2", "name 3"... : the first no account has"""
        n, cand = 1, name
        while not self.name_free(cand):
            n += 1
            suffix = " %d" % n
            cand = name[:NAME_MAX - len(suffix)] + suffix
        return cand

    # ---- accounts (the admin: ssbu_lobby.py --account-*)
    def accounts(self):
        """[(name, rating, wins, losses, banned, created)] by name"""
        return self.db.execute("SELECT name, rating, wins, losses, banned, created FROM players ORDER BY name").fetchall()

    def rename(self, old, new):
        """-> None, or why not"""
        want = clean_name(new)
        if not want:
            return "not a valid name (%d to %d characters, printable)" % (NAME_MIN, NAME_MAX)
        row = self.db.execute("SELECT id FROM players WHERE name = ?", (old,)).fetchone()
        if row is None:
            return "no account called %s" % old
        if not self.name_free(want, row[0]):
            return "another account is called %s" % want
        self.db.execute("UPDATE players SET name = ? WHERE id = ?", (want, row[0]))
        self.db.commit()
        return None

    def ban(self, name, on=True):
        """-> None, or why not. A banned account is not ranked (its game plays on, unranked) until unbanned."""
        cur = self.db.execute("UPDATE players SET banned = ? WHERE name = ?", (1 if on else 0, name))
        self.db.commit()
        return None if cur.rowcount else "no account called %s" % name

    def delete(self, name):
        """-> None, or why not. Its games and sets stay (without a name); the game signs up again as a new account."""
        cur = self.db.execute("DELETE FROM players WHERE name = ?", (name,))
        self.db.commit()
        return None if cur.rowcount else "no account called %s" % name

    def rank(self, pid):
        """pid's place in the table (1 = first), 0 = not ranked"""
        row = self.db.execute("SELECT rating - 2 * rd FROM players WHERE id = ?", (pid,)).fetchone()
        if row is None:
            return 0
        return 1 + self.db.execute("SELECT COUNT(*) FROM players WHERE rating - 2 * rd > ?", row).fetchone()[0]

    def player(self, pid):
        return self.db.execute("SELECT name, rating, rd, vol, wins, losses FROM players WHERE id = ?", (pid,)).fetchone()

    # ---- sets
    def open_set(self, p1, p2):
        """p1 / p2 (ladder ids, p1 = the rollback match's player 1) start playing: their open set, or a new one."""
        row = self.db.execute("SELECT id, p1 FROM sets WHERE state = 'open' AND ((p1 = ? AND p2 = ?) OR (p1 = ? AND p2 = ?))",
                              (p1, p2, p2, p1)).fetchone()
        if row is not None:
            return row[0]
        for p in (p1, p2):        # any other open set of either is left half-played
            self.void_sets(p, "a new set started")
        cur = self.db.execute("INSERT INTO sets (season, p1, p2, opened) VALUES (?, ?, ?, ?)", (self.season(), p1, p2, self.now()))
        self.db.commit()
        return cur.lastrowid

    def void_sets(self, pid, why="left"):
        n = self.db.execute("UPDATE sets SET state = 'void', closed = ? WHERE state = 'open' AND (p1 = ? OR p2 = ?)",
                            (self.now(), pid, pid)).rowcount
        self.db.commit()
        return n

    def open_set_of(self, a, b):
        return self.db.execute("SELECT id, p1, p2, w1, w2 FROM sets WHERE state = 'open' AND ((p1 = ? AND p2 = ?) OR (p1 = ? AND p2 = ?))",
                               (a, b, b, a)).fetchone()

    # ---- results
    def report(self, pid, opp, r):
        """A RESULT from player pid (whose rollback opponent is opp). r: dict seed, slot (0 / 1: pid's own slot), stage,
        chara (2), stocks (2), dmg (2), frames, rule_kind, stock, desyncs, flags. Returns the outcome of the game once
        both reports are in ("ok" / "draw" / "disputed: why"), or None while one is waited for."""
        if r["seed"] in self.settled:
            return self.settled[r["seed"]]
        p = self.pending.setdefault(r["seed"], {"at": self.now(), "reports": {}, "pair": frozenset((pid, opp))})
        if pid not in p["pair"]:
            return None
        p["reports"][pid] = dict(r)
        if len(p["reports"]) < 2:
            return None
        del self.pending[r["seed"]]
        return self.settle(r["seed"], p["reports"])

    def sweep(self):
        """Games whose second report did not come in time are disputed."""
        out = []
        for seed, p in list(self.pending.items()):
            if self.now() - p["at"] > REPORT_WAIT:
                del self.pending[seed]
                out.append(self.settle(seed, p["reports"]))
        return out

    def check(self, reports):
        """(ok, why, players by slot, the agreed report)"""
        if len(reports) != 2:
            return False, "one report only", None, next(iter(reports.values()))
        (a, ra), (b, rb) = reports.items()
        if {ra["slot"], rb["slot"]} != {0, 1}:
            return False, "slots differ", None, ra
        by_slot = (a, b) if ra["slot"] == 0 else (b, a)
        keys = ("stage", "chara", "stocks", "dmg", "rule_kind", "stock")
        if any(list(ra[k]) != list(rb[k]) if isinstance(ra[k], (list, tuple)) else ra[k] != rb[k] for k in keys):
            return False, "reports differ", by_slot, ra
        if ra["desyncs"] or rb["desyncs"]:
            return False, "desync", by_slot, ra
        if (ra["flags"] | rb["flags"]) & (F_ABANDON | F_DISCONNECT):
            return False, "abandoned", by_slot, ra
        if ra["rule_kind"] != RULE_STOCK or ra["stock"] != LADDER_STOCKS:
            return False, "not ladder rules", by_slot, ra
        if self.legal_stages is not None and ra["stage"] not in self.legal_stages:
            return False, "stage not legal", by_slot, ra
        return True, None, by_slot, ra

    def settle(self, seed, reports):
        ok, why, by_slot, r = self.check(reports)
        winner = game_winner(r["stocks"], r["dmg"]) if ok else None
        state = "ok" if ok and winner is not None else "draw" if ok else "disputed"
        st = self.open_set_of(*by_slot) if by_slot else None
        self.db.execute("INSERT INTO games (set_id, seed, at, state, why, stage, chara1, chara2, stocks1, stocks2, dmg1, dmg2, frames, winner)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        (st[0] if st else None, seed & 0x7FFFFFFFFFFFFFFF, self.now(), state, why, r["stage"], r["chara"][0], r["chara"][1],
                         r["stocks"][0], r["stocks"][1], r["dmg"][0], r["dmg"][1], r["frames"], winner))
        if state == "ok" and st is not None:
            set_id, p1, _, w1, w2 = st
            won = by_slot[winner]
            w1, w2 = w1 + (won == p1), w2 + (won != p1)
            self.db.execute("UPDATE sets SET w1 = ?, w2 = ? WHERE id = ?", (w1, w2, set_id))
            if max(w1, w2) >= LADDER_TO:
                self.close_set(set_id, 1 if w1 > w2 else 2)
        self.db.commit()
        out = self.settled[seed] = state if state != "disputed" else "disputed: " + why
        if len(self.settled) > 4096:
            self.settled.pop(next(iter(self.settled)))
        return out

    def close_set(self, set_id, winner):
        _, p1, p2 = self.db.execute("SELECT id, p1, p2 FROM sets WHERE id = ?", (set_id,)).fetchone()
        a, b = self.player(p1), self.player(p2)
        s = 1.0 if winner == 1 else 0.0
        na = glicko2(a[1], a[2], a[3], [(b[1], b[2], s)])
        nb = glicko2(b[1], b[2], b[3], [(a[1], a[2], 1.0 - s)])
        for pid, n, won in ((p1, na, winner == 1), (p2, nb, winner == 2)):
            self.db.execute("UPDATE players SET rating = ?, rd = ?, vol = ?, wins = wins + ?, losses = losses + ? WHERE id = ?",
                            (n[0], n[1], n[2], int(won), int(not won), pid))
        self.db.execute("UPDATE sets SET state = 'done', closed = ?, winner = ?, r1_before = ?, r1_after = ?, r2_before = ?, r2_after = ?"
                        " WHERE id = ?", (self.now(), winner, a[1], na[0], b[1], nb[0], set_id))

    # ---- (2026-11-03) a whole set at once: the ladder queue's matches and rated tournament sets (tournament.py)
    def record_set(self, p1, p2, winner, w1, w2, tid=None, games=()):
        """p1 / p2 (ladder ids) played a set that winner (1 / 2) won w1 - w2; games: [(winner slot 0 / 1, chara1, chara2)].
        Both ratings change (one Glicko-2 period). Returns ((p1 rating before, after), (p2 rating before, after)), or
        None when one of the two has no account."""
        a, b = self.player(p1), self.player(p2)
        if a is None or b is None or p1 == p2:
            return None
        cur = self.db.execute("INSERT INTO sets (season, p1, p2, opened, w1, w2, tid) VALUES (?, ?, ?, ?, ?, ?, ?)",
                              (self.season(), p1, p2, self.now(), w1, w2, tid))
        set_id = cur.lastrowid
        for k, (gw, c1, c2) in enumerate(games):
            self.db.execute("INSERT INTO games (set_id, seed, at, state, chara1, chara2, winner) VALUES (?, ?, ?, 'ok', ?, ?, ?)",
                            (set_id, (tid or 0) * 100 + k, self.now(), c1 & 0x7FFFFFFFFFFFFFFF if c1 else None,
                             c2 & 0x7FFFFFFFFFFFFFFF if c2 else None, gw))
        self.close_set(set_id, winner)
        self.db.commit()
        na, nb = self.player(p1), self.player(p2)
        return (a[1], na[1]), (b[1], nb[1])

    def matches(self, n=30):
        """the latest sets with a match page (the ladder queue's): names, score, winner, ratings (the web page)"""
        db = sqlite3.connect(self.path)
        try:
            rows = db.execute("SELECT s.tid, a.name, b.name, s.w1, s.w2, s.winner, s.closed, s.r1_before, s.r1_after, s.r2_before, s.r2_after"
                              " FROM sets s JOIN players a ON a.id = s.p1 JOIN players b ON b.id = s.p2"
                              " WHERE s.state = 'done' AND s.tid IS NOT NULL ORDER BY s.id DESC LIMIT ?", (n,)).fetchall()
            return [{"tid": tid, "players": [n1, n2], "score": [w1, w2], "winner": winner, "closed": closed,
                     "rating": [[round(r1b), round(r1a)], [round(r2b), round(r2a)]] if r1b is not None else None}
                    for tid, n1, n2, w1, w2, winner, closed, r1b, r1a, r2b, r2a in rows]
        finally:
            db.close()

    # ---- web (never ids, keys or addresses)
    def table(self):
        db = sqlite3.connect(self.path)
        try:
            rows = db.execute("SELECT id, name, rating, rd, wins, losses FROM players ORDER BY rating - 2 * rd DESC, name").fetchall()
            out = []
            for pid, name, rating, rd, w, l in rows:
                mains = db.execute("SELECT c, COUNT(*) n FROM (SELECT g.chara1 c FROM games g JOIN sets s ON g.set_id = s.id WHERE g.state = 'ok' AND s.p1 = ?"
                                   " UNION ALL SELECT g.chara2 FROM games g JOIN sets s ON g.set_id = s.id WHERE g.state = 'ok' AND s.p2 = ?)"
                                   " WHERE c IS NOT NULL GROUP BY c ORDER BY n DESC LIMIT 3", (pid, pid)).fetchall()
                out.append({"name": name, "rating": round(rating), "rd": round(rd), "wins": w, "losses": l,
                            "main": "%016x" % mains[0][0] if mains else None, "mains": [m[0] for m in mains]})
            return out
        finally:
            db.close()

    def history(self, name):
        db = sqlite3.connect(self.path)
        try:
            row = db.execute("SELECT id, name FROM players WHERE name = ?", (name,)).fetchone()
            if row is None:
                return None
            pid = row[0]
            sets = db.execute("SELECT s.id, s.p1, a.name, b.name, s.state, s.winner, s.w1, s.w2, s.closed, s.r1_before, s.r1_after, s.r2_before, s.r2_after, s.tid"
                              " FROM sets s JOIN players a ON a.id = s.p1 JOIN players b ON b.id = s.p2 WHERE s.p1 = ? OR s.p2 = ?"
                              " ORDER BY s.id DESC LIMIT 100", (pid, pid)).fetchall()
            out = []
            for sid, p1, n1, n2, state, winner, w1, w2, closed, r1b, r1a, r2b, r2a, tid in sets:
                me1 = p1 == pid
                out.append({"opponent": n2 if me1 else n1, "state": state, "score": [w1, w2] if me1 else [w2, w1], "tid": tid,
                            "won": None if winner is None else (winner == 1) == me1, "closed": closed,
                            "rating": [round(r1b), round(r1a)] if me1 and r1b is not None else [round(r2b), round(r2a)] if r2b is not None else None})
            return {"name": row[1], "sets": out}
        finally:
            db.close()
