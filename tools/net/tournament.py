#!/usr/bin/env python3
"""Tournaments of the lobby (ssbu_lobby.py --ladder-db): double elimination brackets that run by themselves.

Players are accounts (ladder.py: the ladder id, hex here). The organizer (the account that made the tournament) opens it,
players sign up, the seeding is proposed by ladder rating (unrated players last, in a random order) and the organizer can
change it by hand (move a player, or the modes rating / random / manual). Start builds the bracket: the winners bracket
(standard seed order, byes for a field that is not a power of two), the losers bracket (losers of each winners round drop
in, in alternating order to put off rematches), grand final and its reset when the losers-bracket player wins it.

The bracket is a fixed graph of sets whose two sides come from an entrant, a bye, the winner or the loser of an earlier
set; every state (who plays whom, byes, forfeits of disqualified players, the champion) is computed again from the
recorded results (resolve), so an organizer's correction or a DQ fixes everything after it by itself, and a result whose
players changed is dropped.

Each set is a best of N (rules: bo, bo_top8, bo_finals). Its games follow the competitive procedure: game 1, the two
players strike the starter stages (1-2-1 for five; who strikes first is drawn); later games, the winner of the last game
bans `bans` stages, the loser picks among the rest; the winner then picks a fighter first and the loser sees it before
confirming its own (ready / go). A game's result (from the two games' RESULT reports, see ssbu_lobby.py) moves the set on.

No network, no game content (stage and fighter names are the game's public internal names, used to address them).
Python 3.8+, standard library only."""
import hashlib
import json
import random
import time
import zlib

STARTERS = ["battle_field", "battle_field_s", "end", "poke_stadium2", "animal_village"]
COUNTERPICKS = ["animal_city", "poke_kalos", "yoshi_story", "fox_lylatcruise", "trail_castle", "ff_cave"]
STAGE_NAMES = {"battle_field": "Battlefield", "battle_field_s": "Small Battlefield", "end": "Final Destination",
               "poke_stadium2": "Pokemon Stadium 2", "animal_village": "Smashville", "animal_city": "Town and City",
               "poke_kalos": "Kalos Pokemon League", "yoshi_story": "Yoshi's Story", "fox_lylatcruise": "Lylat Cruise",
               "trail_castle": "Hollow Bastion", "ff_cave": "Northern Cave", "dracula_castle": "Dracula's Castle",
               "fzero_bigblue": "Big Blue", "zelda_skyward": "Skyloft", "kirby_cave": "The Great Cave Offensive"}
FIGHTERS = {
    "mario": "Mario", "donkey": "Donkey Kong", "link": "Link", "samus": "Samus", "samusd": "Dark Samus", "yoshi": "Yoshi",
    "kirby": "Kirby", "fox": "Fox", "pikachu": "Pikachu", "luigi": "Luigi", "ness": "Ness", "captain": "Captain Falcon",
    "purin": "Jigglypuff", "peach": "Peach", "daisy": "Daisy", "koopa": "Bowser", "ice_climber": "Ice Climbers",
    "sheik": "Sheik", "zelda": "Zelda", "mariod": "Dr. Mario", "pichu": "Pichu", "falco": "Falco", "marth": "Marth",
    "lucina": "Lucina", "younglink": "Young Link", "ganon": "Ganondorf", "mewtwo": "Mewtwo", "roy": "Roy", "chrom": "Chrom",
    "gamewatch": "Mr. Game & Watch", "metaknight": "Meta Knight", "pit": "Pit", "pitb": "Dark Pit",
    "szerosuit": "Zero Suit Samus", "wario": "Wario", "snake": "Snake", "ike": "Ike", "ptrainer": "Pokemon Trainer",
    "pzenigame": "Squirtle", "pfushigisou": "Ivysaur", "plizardon": "Charizard", "diddy": "Diddy Kong", "lucas": "Lucas",
    "sonic": "Sonic", "dedede": "King Dedede", "pikmin": "Olimar", "lucario": "Lucario", "robot": "R.O.B.",
    "toonlink": "Toon Link", "wolf": "Wolf", "murabito": "Villager", "rockman": "Mega Man", "wiifit": "Wii Fit Trainer",
    "rosetta": "Rosalina & Luma", "littlemac": "Little Mac", "gekkouga": "Greninja", "palutena": "Palutena",
    "pacman": "Pac-Man", "reflet": "Robin", "shulk": "Shulk", "koopajr": "Bowser Jr.", "duckhunt": "Duck Hunt",
    "ryu": "Ryu", "ken": "Ken", "cloud": "Cloud", "kamui": "Corrin", "bayonetta": "Bayonetta", "inkling": "Inkling",
    "ridley": "Ridley", "simon": "Simon", "richter": "Richter", "krool": "King K. Rool", "shizue": "Isabelle",
    "gaogaen": "Incineroar", "miifighter": "Mii Brawler", "miiswordsman": "Mii Swordfighter", "miigunner": "Mii Gunner",
    "packun": "Piranha Plant", "jack": "Joker", "brave": "Hero", "buddy": "Banjo & Kazooie", "dolly": "Terry",
    "master": "Byleth", "tantan": "Min Min", "pickel": "Steve", "edge": "Sephiroth", "element": "Pyra / Mythra",
    "demon": "Kazuya", "trail": "Sora", "random": "Random"}


def hash40(s):
    """the game's hash of a name: crc32 | length << 32"""
    b = s.encode()
    return (len(b) << 32) | zlib.crc32(b)


_CHARA_BY_HASH = {hash40("ui_chara_" + k): v for k, v in FIGHTERS.items()}
_KEY_BY_HASH = {hash40("ui_chara_" + k): k for k in FIGHTERS}


def chara_key(chara):
    """the fighter's internal name ("mario") from a ui_chara id, or "" (the web page's icon file: icons/<key>.png)"""
    return _KEY_BY_HASH.get(chara & 0xffffffffff, "")


def chara_name(chara):
    """the fighter's name from a ui_chara id (0xc1 << 56 | hash40("ui_chara_<name>"); the game's own ids may carry more
    bits above the hash), or "" when it is not one of the fighters"""
    return _CHARA_BY_HASH.get(chara & 0xffffffffff, "")


RULES = {"bo": 3, "bo_top8": 3, "bo_finals": 5, "bans": 2, "stocks": 3,
         "starters": STARTERS, "counterpicks": COUNTERPICKS,
         "checkin": True,       # (2026-11-02) the organizer opens a check-in; who does not check in is left out at the start
         "rated": False,        # the games also count for the ladder (rating)
         "noshow": 600,         # s: a set ready this long with one player at the set-up and the other never seen: the first wins it (0: never)
         "turn_for": 60}        # s: a stage strike / ban / pick not made by then is made by the lobby (the first stage left)
MAX_ENTRANTS = 256
BYE = -1                          # a side with no player


def seed_order(size):
    """[1-based seeds in bracket order] for a power of two: 1 v size, size/2 v size/2+1, ... (1 and 2 meet last)"""
    order = [1, 2]
    while len(order) < size:
        n = len(order) * 2
        order = [x for s in order for x in (s, n + 1 - s)]
    return order


def strike_counts(n):
    """strikes per turn to leave one of n stages: [1, 2, 2, ..., rest] (five: 1-2-1)"""
    out, left = [], n - 1
    if left > 0:
        out.append(1)
        left -= 1
    while left > 0:
        c = min(2, left)
        out.append(c)
        left -= c
    return out


class Error(Exception):
    """a refused action (the text says why, for the player)"""


class Tournament:
    def __init__(self, tid, name, organizer, rules=None, salt=None, now=time.time):
        self.id, self.name, self.organizer, self.now = tid, name, organizer, now
        self.rules = dict(RULES)
        self.rules.update(rules or {})
        self.state = "signup"         # signup -> (checkin ->) running -> done (or cancelled)
        self.entrants = []            # [{"pid", "name", "rating" (None: unrated), "dq", "in" (checked in)}] in seed order
        self.seed_mode = "rating"
        self.salt = salt if salt is not None else random.getrandbits(32)
        self.sets = []                # the bracket's graph (start): {"id", "br" W / L / G, "round", "src": [src, src]}
        self.results = {}             # set id (str) -> {"players": [a, b], "games": [...], "winner": side, "by": "games" / "organizer"}
        self.procs = {}               # set id (str) -> the procedure of its next game (stages, fighters)
        self.version = 1              # every change (the lobby resends the bracket when it changes)
        self.created = now()
        self.view = []                # (resolve) per set: {"p": [a, b], "state", "winner", "loser"}
        self.ready_at = {}            # set id (str) -> when it became ready (the no-show rule; not saved: a restart starts it again)

    # ---- sign-up and seeding
    def changed(self):
        self.version += 1

    def find(self, pid):
        for i, e in enumerate(self.entrants):
            if e["pid"] == pid:
                return i
        return -1

    def sign_up(self, pid, name, rating=None):
        if self.state not in ("signup", "checkin"):
            raise Error("sign-ups are closed")
        i = self.find(pid)
        if i >= 0:
            self.entrants[i]["name"] = name
            return
        if len(self.entrants) >= MAX_ENTRANTS:
            raise Error("the tournament is full (%d players)" % MAX_ENTRANTS)
        self.entrants.append({"pid": pid, "name": name, "rating": rating, "dq": False, "in": self.state == "checkin"})
        self.reseed()
        self.changed()

    def withdraw(self, pid):
        i = self.find(pid)
        if i < 0:
            raise Error("not signed up")
        if self.state in ("signup", "checkin"):
            del self.entrants[i]
            self.changed()
        else:
            self.dq(i)

    def open_checkin(self):
        """the organizer opens the check-in: signed-up players confirm they are here (late sign-ups are checked in)"""
        if self.state != "signup":
            raise Error("the check-in is open already" if self.state == "checkin" else "the bracket is made already")
        self.state = "checkin"
        self.changed()

    def check_in(self, pid):
        i = self.find(pid)
        if i < 0:
            raise Error("not signed up")
        if self.state != "checkin":
            raise Error("the check-in is not open")
        if not self.entrants[i].get("in"):
            self.entrants[i]["in"] = True
            self.changed()

    def reseed(self):
        if self.seed_mode == "rating":
            rnd = random.Random(self.salt)
            unrated = [e for e in self.entrants if e["rating"] is None]
            rnd.shuffle(unrated)
            self.entrants = sorted((e for e in self.entrants if e["rating"] is not None), key=lambda e: -e["rating"]) + unrated
        elif self.seed_mode == "random":
            rnd = random.Random(self.salt)
            self.entrants.sort(key=lambda e: e["pid"])
            rnd.shuffle(self.entrants)

    def set_seed_mode(self, mode):
        if self.state not in ("signup", "checkin"):
            raise Error("the bracket is made already")
        if mode not in ("rating", "random", "manual"):
            raise Error("unknown seeding mode")
        self.seed_mode = mode
        if mode == "random":
            self.salt = random.getrandbits(32)
        self.reseed()
        self.changed()

    def move(self, i, j):
        """entrant i to seed position j (0 = seed 1): the seeding becomes manual"""
        if self.state not in ("signup", "checkin"):
            raise Error("the bracket is made already")
        if not (0 <= i < len(self.entrants) and 0 <= j < len(self.entrants)):
            raise Error("no such seed")
        self.entrants.insert(j, self.entrants.pop(i))
        self.seed_mode = "manual"
        self.changed()

    # ---- the bracket
    def start(self):
        if self.state not in ("signup", "checkin"):
            raise Error("already started")
        if self.state == "checkin":                   # (the ones who did not check in are left out)
            self.entrants = [e for e in self.entrants if e.get("in")]
        if len(self.entrants) < 2:
            raise Error("at least 2 players are needed")
        if self.is_ladder():                          # (2026-11-03) a ladder match: one set between its two players, no bracket
            if len(self.entrants) != 2:
                raise Error("a ladder match has two players")
            self.sets, self.state = [{"id": 0, "br": "W", "round": 1, "src": [["E", 0], ["E", 1]]}], "running"
            self.results, self.procs = {}, {}
            self.resolve()
            self.changed()
            return
        n = len(self.entrants)
        size = 2
        while size < n:
            size *= 2
        k = size.bit_length() - 1
        sets = []

        def add(br, rnd, a, b):
            sets.append({"id": len(sets), "br": br, "round": rnd, "src": [a, b]})
            return len(sets) - 1

        order = seed_order(size)
        slot = [["E", s - 1] if s <= n else ["BYE"] for s in order]
        w = [[add("W", 1, slot[2 * i], slot[2 * i + 1]) for i in range(size // 2)]]
        for r in range(2, k + 1):
            prev = w[-1]
            w.append([add("W", r, ["W", prev[2 * i]], ["W", prev[2 * i + 1]]) for i in range(len(prev) // 2)])
        if k >= 2:
            lr = 1
            lprev = [add("L", lr, ["L", w[0][2 * i]], ["L", w[0][2 * i + 1]]) for i in range(len(w[0]) // 2)]
            for r in range(2, k + 1):
                drop = w[r - 1] if r % 2 else list(reversed(w[r - 1]))     # (every other round reversed: fewer rematches)
                lr += 1
                lprev = [add("L", lr, ["W", lprev[i]], ["L", drop[i]]) for i in range(len(lprev))]
                if r < k:
                    lr += 1
                    lprev = [add("L", lr, ["W", lprev[2 * i]], ["W", lprev[2 * i + 1]]) for i in range(len(lprev) // 2)]
            lchamp = ["W", lprev[0]]
        else:
            lchamp = ["L", w[0][0]]
        g1 = add("G", 1, ["W", w[-1][0]], lchamp)
        add("G", 2, ["W", g1], ["L", g1])             # (the reset: played only when the losers-bracket player won g1)
        self.sets, self.state = sets, "running"
        self.results, self.procs = {}, {}
        self.resolve()
        self.changed()

    def best_of(self, s):
        if s["br"] == "G":
            return self.rules["bo_finals"]
        same = [x for x in self.sets if x["br"] == s["br"] and x["round"] == s["round"]]
        last = max(x["round"] for x in self.sets if x["br"] == s["br"])
        if s["round"] == last:
            return self.rules["bo_finals"]               # winners final, losers final
        return self.rules["bo_top8"] if len(same) <= 2 else self.rules["bo"]

    def resolve(self):
        """who plays whom now, from the results (and drop the results whose players changed)"""
        view = []

        def side(src):
            if src[0] == "E":
                return src[1]
            if src[0] == "BYE":
                return BYE
            v = view[src[1]]
            return v["winner"] if src[0] == "W" else v["loser"]

        for s in self.sets:
            sid = str(s["id"])
            p = [side(s["src"][0]), side(s["src"][1])]
            v = {"p": p, "state": "waiting", "winner": None, "loser": None, "bo": self.best_of(s)}
            if s["br"] == "G" and s["round"] == 2:
                g1 = view[s["id"] - 1]
                over = g1["state"] in ("done", "forfeit", "bye")
                if not (g1["state"] == "done" and g1["winner"] == g1["p"][1]):     # (reset only when the losers' side won it)
                    v["state"] = "skipped" if over else "waiting"
                    v["p"] = [None, None]
                    self.results.pop(sid, None)
                    self.procs.pop(sid, None)
                    view.append(v)
                    continue
            r = self.results.get(sid)
            if r is not None and r["players"] != p:
                del self.results[sid]
                self.procs.pop(sid, None)
                r = None
            if None in p:
                pass
            elif BYE in p or any(self.entrants[x]["dq"] for x in p if x != BYE):
                live = [x for x in p if x != BYE and not self.entrants[x]["dq"]]
                if r is not None and r.get("by") == "organizer":
                    v["state"], v["winner"], v["loser"] = "done", p[r["winner"]], p[1 - r["winner"]]
                else:
                    v["state"] = "bye" if BYE in p else "forfeit"
                    v["winner"] = live[0] if live else BYE
                    v["loser"] = next((x for x in p if x != v["winner"]), BYE)
                    self.procs.pop(sid, None)
            elif r is not None and r.get("winner") is not None:
                v["state"], v["winner"], v["loser"] = "done", p[r["winner"]], p[1 - r["winner"]]
            else:
                v["state"] = "playing" if r and r["games"] else "ready"
            view.append(v)
        self.view = view
        for i, v in enumerate(view):
            k = str(i)
            if v["state"] in ("ready", "playing"):
                self.ready_at.setdefault(k, self.now())
            else:
                self.ready_at.pop(k, None)
        if self.sets and self.state in ("running", "done"):
            self.state = "done" if view[-1]["state"] in ("done", "skipped", "forfeit", "bye") else "running"

    def is_ladder(self):
        """(2026-11-03) a ladder match (the lobby's ladder queue paired its two players): one set, no bracket"""
        return self.rules.get("kind") == "ladder"

    def champion(self):
        """the winner (entrant), once the tournament is over"""
        if self.state != "done":
            return None
        if len(self.view) == 1:                       # (a ladder match)
            return self.view[0]["winner"]
        g1, g2 = self.view[-2], self.view[-1]
        return g2["winner"] if g2["state"] != "skipped" else g1["winner"]

    def standings(self):
        """[(place, entrant)] of the players placed so far: the champion, then by how late they were knocked out (a
        loss in the losers bracket or the grand final); players knocked out in the same round share a place"""
        if len(self.sets) == 1:                       # (a ladder match: the winner, then the loser)
            v = self.view[0]
            return [(1, v["winner"]), (2, v["loser"])] if v["state"] in ("done", "forfeit") and v["loser"] not in (None, BYE) else []
        depth = {}
        for i, s in enumerate(self.sets):
            v = self.view[i]
            if s["br"] == "W" or v["state"] not in ("done", "forfeit", "bye") or v["loser"] in (None, BYE):
                continue
            if s["br"] == "G" and s["round"] == 1 and v["winner"] == v["p"][1] and v["state"] == "done":
                continue                              # (the winners' side lost g1: the reset decides)
            depth[v["loser"]] = (1 if s["br"] == "L" else 2, s["round"])
        champ = self.champion()
        out = [(1, champ)] if champ not in (None, BYE) else []
        alive = sum(1 for i in range(len(self.entrants)) if i not in depth and i != champ)
        for e, d in sorted(depth.items(), key=lambda kv: kv[1], reverse=True):
            better = sum(1 for x in depth.values() if x > d)
            out.append((1 + len(out[:1] if champ not in (None, BYE) else []) + alive + better, e))
        return out

    def set_of(self, entrant):
        """the set entrant plays now (ready / playing), or None"""
        for i, v in enumerate(self.view):
            if v["state"] in ("ready", "playing") and entrant in v["p"]:
                return self.sets[i]["id"]
        return None

    def dq(self, i):
        if not 0 <= i < len(self.entrants):
            raise Error("no such player")
        if self.state in ("signup", "checkin"):
            del self.entrants[i]
        else:
            self.entrants[i]["dq"] = True
            self.resolve()
        self.changed()

    def override(self, sid, winner, by="organizer"):
        """the organizer's result for set sid: winner 0 / 1 (side), or None to clear it (the set is played again).
        by: "organizer", or (the lobby's ladder: the other player's game left) "left\""""
        if not 0 <= sid < len(self.sets):
            raise Error("no such set")
        v = self.view[sid]
        if None in v["p"] or v["state"] == "skipped":
            raise Error("that set has no players yet")
        if winner is None:
            self.results.pop(str(sid), None)
        else:
            r = self.results.setdefault(str(sid), {"players": v["p"], "games": [], "winner": None})
            r["winner"], r["by"], r["players"] = winner, by, v["p"]
        self.procs.pop(str(sid), None)
        self.resolve()
        self.changed()

    def cancel(self):
        self.state = "cancelled"
        self.changed()

    def set_noshow(self, minutes):
        """(2026-11-03) the organizer changes the no-show time (minutes, 0: never) at any time before the end; a set
        already waiting is judged with the new time (its clock started when it became ready)"""
        if self.state in ("done", "cancelled"):
            raise Error("the tournament is over")
        minutes = int(minutes)
        if not 0 <= minutes <= 120:
            raise Error("0 to 120 minutes")
        self.rules["noshow"] = minutes * 60
        self.changed()

    def feature(self, sid, code):
        """(2026-11-02) the organizer puts set sid on stream: its players play it in the Battle Arena whose code is `code`
        (the game's Online menu; spectators watch it there). "" takes it off."""
        if not 0 <= sid < len(self.sets):
            raise Error("no such set")
        code = "".join(ch for ch in str(code or "").upper() if ch.isalnum())[:8]
        f = self.rules.setdefault("featured", {})
        if code:
            f[str(sid)] = code
        else:
            f.pop(str(sid), None)
        self.changed()

    def featured(self, sid):
        return self.rules.get("featured", {}).get(str(sid), "")

    # ---- the games of a set: stages, fighters, results
    def needed(self, sid):
        return self.view[sid]["bo"] // 2 + 1

    def score(self, sid):
        r = self.results.get(str(sid))
        games = r["games"] if r else []
        return [sum(1 for g in games if g["winner"] == 0), sum(1 for g in games if g["winner"] == 1)]

    def proc(self, sid):
        """the procedure of set sid's next game (made when first asked)"""
        key = str(sid)
        if self.view[sid]["state"] not in ("ready", "playing"):
            return None
        p = self.procs.get(key)
        if p is not None:
            return p
        r = self.results.get(key)
        games = r["games"] if r else []
        n = len(games) + 1
        starters, cps = list(self.rules["starters"]), list(self.rules["counterpicks"])
        if n == 1:
            first = int(hashlib.sha256(b"%d:%d" % (self.salt, sid)).hexdigest(), 16) & 1
            counts = strike_counts(len(starters))
            p = {"game": n, "phase": "strike" if counts else "chars", "pool": starters, "struck": [], "counts": counts, "step": 0,
                 "turn": first, "left": counts[0] if counts else 0, "stage": None if counts else starters[0]}
        else:
            w = games[-1]["winner"]
            pool = starters + [c for c in cps if c not in starters]
            bans = min(self.rules["bans"], len(pool) - 1)
            p = {"game": n, "phase": "ban" if bans else "pick", "pool": pool, "struck": [], "counts": [], "step": 0,
                 "turn": w if bans else 1 - w, "left": bans, "stage": None, "last_winner": w}
        p.update({"ready": [None, None], "seq": 0, "turn_at": self.now()})
        self.procs[key] = p
        return p

    def side_of(self, sid, entrant):
        p = self.view[sid]["p"]
        if entrant not in p:
            raise Error("not your set")
        return p.index(entrant)

    def stage_step(self, sid, entrant, stage):
        """a strike, ban or pick of stage by entrant in set sid"""
        side = self.side_of(sid, entrant)
        p = self.proc(sid)
        if p is None or p["phase"] not in ("strike", "ban", "pick"):
            raise Error("no stage to choose now")
        if p["turn"] != side:
            raise Error("it is the other player's turn")
        if stage not in p["pool"] or stage in p["struck"]:
            raise Error("that stage cannot be chosen")
        if p["phase"] == "pick":
            p["stage"], p["phase"] = stage, "chars"
        else:
            p["struck"].append(stage)
            p["left"] -= 1
            p["turn_at"] = self.now()
            if p["phase"] == "strike":
                rest = [s for s in p["pool"] if s not in p["struck"]]
                if len(rest) == 1:
                    p["stage"], p["phase"] = rest[0], "chars"
                elif p["left"] == 0:
                    p["step"] += 1
                    p["turn"], p["left"] = 1 - p["turn"], p["counts"][p["step"]]
            elif p["left"] == 0:                      # (bans done: the loser of the last game picks)
                p["phase"], p["turn"] = "pick", 1 - p["last_winner"]
        p["turn_at"] = self.now()
        self.changed()

    def turn_left(self, sid):
        """seconds left for the stage step of set sid (None: no stage step now, or no limit)"""
        p = self.procs.get(str(sid))
        if p is None or p["phase"] not in ("strike", "ban", "pick") or not self.rules.get("turn_for"):
            return None
        return max(0, int(self.rules["turn_for"] - (self.now() - p.get("turn_at", self.now()))))

    def sweep(self):
        """the clock: a stage step not made in time is made for the player (the first stage left); a set ready for
        rules["noshow"] s with one player seen at the set-up and the other never: the first wins it (by "noshow"; the
        organizer can clear it). Returns [(sid, what)] of what it did (the lobby logs it and sends the new state)."""
        out = []
        if self.state != "running":
            return out
        for i, v in enumerate(self.view):
            if v["state"] not in ("ready", "playing"):
                continue
            key = str(i)
            p = self.procs.get(key)
            if self.turn_left(i) == 0:
                rest = [x for x in p["pool"] if x not in p["struck"]]
                if rest:
                    self.stage_step(i, v["p"][p["turn"]], rest[0])
                    out.append((i, "a stage step timed out: %s" % rest[0]))
            ns = self.rules.get("noshow") or 0
            r = self.results.get(key)
            if not ns or v["state"] != "ready" or (r and r["games"]) or p is None:
                continue
            seen = p.setdefault("seen", [False, False])
            for k in (0, 1):
                if p["ready"][k] is not None:
                    seen[k] = True
            if self.now() - self.ready_at.get(key, self.now()) > ns and seen.count(True) == 1:
                w = seen.index(True)
                r = self.results.setdefault(key, {"players": v["p"], "games": [], "winner": None})
                r["winner"], r["by"], r["players"] = w, "noshow", v["p"]
                self.procs.pop(key, None)
                self.resolve()
                self.changed()
                out.append((i, "no-show: %s wins the set" % self.entrants[v["p"][w]]["name"]))
        return out

    def seen_at_setup(self, sid, entrant):
        """entrant came to the set-up of set sid (its game asked to play): it is not a no-show"""
        p = self.proc(sid)
        if p is not None:
            p.setdefault("seen", [False, False])[self.side_of(sid, entrant)] = True

    def ready(self, sid, entrant, chara, confirm=False):
        """entrant is at the set-up with fighter chara (ui_chara id): ready to play the next game. confirm: it saw the
        other player's fighter (the loser of the last game, after the winner chose)"""
        side = self.side_of(sid, entrant)
        p = self.proc(sid)
        if p is not None and p["phase"] == "playing":
            raise Error("this game is being played")
        if p is None or p["phase"] not in ("chars", "go"):
            raise Error("the stage is not chosen yet")
        old = p["ready"][side]
        if old is None or old["chara"] != chara or (confirm and not old["confirm"]):
            p["seq"] += 1
            p["ready"][side] = {"chara": chara, "at": p["seq"], "confirm": bool(confirm)}
            p["phase"] = "chars"
            p["phase"] = "go" if self.go(sid) else "chars"
            self.changed()

    def unready(self, sid, entrant):
        """entrant left the set-up (went back to change its fighter)"""
        side = self.side_of(sid, entrant)
        p = self.proc(sid)
        if p is not None and p["phase"] in ("chars", "go") and p["ready"][side] is not None:
            p["ready"][side] = None
            p["phase"] = "chars"
            self.changed()

    def playing(self, sid):
        """the two games of set sid started the match (the lobby saw them exchange rollback messages): until its result,
        the set is not "ready" any more (a game pressing start again at once is not paired for a game nobody chose)"""
        p = self.proc(sid)
        if p is not None and p["phase"] == "go":
            p["phase"] = "playing"
            self.changed()
            return True
        return False

    def go(self, sid):
        """both players are ready (and the order of the fighter choice is kept): the game can start"""
        p = self.proc(sid)
        if p is None or p["phase"] not in ("chars", "go") or None in p["ready"]:
            return False
        if p["game"] == 1:
            return True
        w = p["last_winner"]
        loser = p["ready"][1 - w]
        return loser["at"] > p["ready"][w]["at"] or loser["confirm"]

    def first_pick(self, sid):
        """the side that must choose its fighter first (the winner of the last game), or None (game 1: both blind)"""
        p = self.proc(sid)
        return None if p is None or p["game"] == 1 else p["last_winner"]

    def game_result(self, sid, winner, stage=None, chara=None):
        """a game of set sid won by side winner (0 / 1). Returns 'game' (the set goes on) or 'set' (it is over)."""
        v = self.view[sid]
        if v["state"] not in ("ready", "playing"):
            raise Error("that set is not being played")
        key = str(sid)
        r = self.results.setdefault(key, {"players": v["p"], "games": [], "winner": None, "by": "games"})
        p = self.procs.get(key) or {}
        r["games"].append({"winner": winner, "stage": stage or p.get("stage"), "chara": chara, "at": self.now()})
        self.procs.pop(key, None)
        sc = self.score(sid)
        out = "game"
        if max(sc) >= self.needed(sid):
            r["winner"] = 0 if sc[0] > sc[1] else 1
            out = "set"
        self.resolve()
        self.changed()
        return out

    # ---- storage
    def to_dict(self):
        return {"id": self.id, "name": self.name, "organizer": self.organizer, "rules": self.rules, "state": self.state,
                "entrants": self.entrants, "seed_mode": self.seed_mode, "salt": self.salt, "sets": self.sets,
                "results": self.results, "procs": self.procs, "version": self.version, "created": self.created}

    @classmethod
    def from_dict(cls, d, now=time.time):
        t = cls(d["id"], d["name"], d["organizer"], d["rules"], d["salt"], now)
        for k in ("state", "entrants", "seed_mode", "sets", "results", "procs", "version", "created"):
            setattr(t, k, d[k])
        for e in t.entrants:
            e.setdefault("in", False)
        if t.sets:
            t.resolve()
        return t


SCHEMA = """
CREATE TABLE IF NOT EXISTS tournaments (id INTEGER PRIMARY KEY, name TEXT NOT NULL, state TEXT NOT NULL,
    created REAL NOT NULL, updated REAL NOT NULL, data TEXT NOT NULL);
"""


KEEP_SHOWN = 2 * 86400.0          # a finished tournament stays in the games' list this long (its web page for ever)


class Store:
    """the tournaments of a lobby, kept in its ladder database (one row each, the state as JSON). `all`: the ones the
    lobby works with: those not finished, and those finished less than KEEP_SHOWN ago (players see their place)."""

    def __init__(self, db, now=time.time):
        self.db, self.now = db, now
        self.db.executescript(SCHEMA)
        self.db.commit()
        self.all = {}
        for tid, data in self.db.execute("SELECT id, data FROM tournaments WHERE state IN ('signup', 'checkin', 'running') OR updated > ?",
                                         (self.now() - KEEP_SHOWN,)):
            self.all[tid] = Tournament.from_dict(json.loads(data), now)

    def create(self, name, organizer, rules=None):
        cur = self.db.execute("INSERT INTO tournaments (name, state, created, updated, data) VALUES (?, 'signup', ?, ?, '{}')",
                              (name, self.now(), self.now()))
        t = Tournament(cur.lastrowid, name, organizer, rules, now=self.now)
        self.all[t.id] = t
        self.save(t)
        return t

    def save(self, t):
        self.db.execute("UPDATE tournaments SET state = ?, updated = ?, data = ? WHERE id = ?",
                        (t.state, self.now(), json.dumps(t.to_dict()), t.id))
        self.db.commit()

    def get(self, tid):
        return self.all.get(tid)

    def load(self, tid):
        """any tournament, finished ones too (the web pages)"""
        if tid in self.all:
            return self.all[tid]
        row = self.db.execute("SELECT data FROM tournaments WHERE id = ?", (tid,)).fetchone()
        return Tournament.from_dict(json.loads(row[0]), self.now) if row else None

    def recent(self, n=20):
        return [r[0] for r in self.db.execute("SELECT id FROM tournaments ORDER BY id DESC LIMIT ?", (n,))]
