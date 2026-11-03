#!/usr/bin/env python3
"""Online lobby for the SSBU native port: the game's own Local Wireless mode carried over the internet.

Every player's game (SSBU_ONLINE=<this host>:<port>, see recomp-study/net_relay.c) joins this lobby at start and gets an
address of a virtual network 10.13.0.0/16. A room a player creates in Local Wireless is listed to every other player
with the same game build and lobby code, who joins it from the game's own Rooms List. The lobby relays each room's
datagrams between that room's players only, and introduces the players of a room to each other so that their games
can send to each other directly (UDP hole punching); the relay is the fallback.

Design adapted from SNO2207/WindowSmash (tools/ssbu_relay.py), used with the author's permission. Our own code and our
own wire protocol ("SU" version 1, described at the top of recomp-study/net_relay.c); not compatible with theirs.

    python ssbu_lobby.py [--bind 127.0.0.1] [--port 39500] [--verbose] [--status-port 8080 [--status-bind 127.0.0.1]]

--status-port starts a small HTTP endpoint, GET /status, that answers with counts only (players, rooms, searching,
version, uptime_s, up) and Access-Control-Allow-Origin: * so a web page (the project site) can show the lobby's state. It never
returns addresses, names or codes. It listens on 127.0.0.1 by default, to sit behind a TLS reverse proxy (Caddy, see
deploy_lobby.sh); --status-bind 0.0.0.0 serves it directly over plain HTTP.

Room browser (O1, lobby 1.2): every room stays listed to every player of the same lobby code whatever its game version
or mod set (ROOMS, and LIST with flag 1); joining stays within one build (the build id carries the game version and a
hash of the mods). An older game keeps its own partition exactly as before.

Matchmaking (O2, lobby 1.3): "Find an opponent" (F1 > Online). A game sends MATCH; the lobby pairs two searching players
of the same build and lobby code (the same partition as the rooms), picks the host (the one that already hosts an open
room, else the one that waited longer) and tells both (MATCH_STATE); the host creates the Local Wireless room, the other
finds it first in its Rooms List. A search ends after --search-secs without an opponent (TIMEOUT); a match ends when
both are in the same room (TOGETHER), when one cancels or leaves (PEER_LEFT), or after --match-secs (EXPIRED). PONG ends
with the number of players searching and the players of every build of this lobby code. An older game never sends
MATCH: it is never matched.

Rollback matchmaking (R3 net3, lobby 1.4): MATCH op 2 is a search for a ROLLBACK match (Smash versus with online
rollback, rb_net.c): such searches are paired only with each other (never with a Local Wireless search, op 1), the one
that waited longer is player 1 (ROLE_HOST); there is no room: both games are held at their match set-up and agree on the
match over RB messages (16) addressed to each other. The lobby counts the pair TOGETHER once it relayed RB both ways
between them. On the public lobby code an RB message to one player is relayed only to the player matched with the
sender (or a player of its Local Wireless room). An older lobby ignores op 2 (the game then says "older lobby").

Ranked ladder (lobby 1.5, --ladder-db, ladder.py): Glicko-2 over Bo3 sets of rollback matches between invited players.
JOIN may end (after the 'M''S' block) with 'L''D' + version u8 (1) + ladder id[16] + HMAC-SHA256(key, JOIN nonce)[32] +
flags u8 (1: key[32] + invitation length u8 + invitation follow: the sign-up, sent until the lobby says "ranked"). WELCOME
then ends with 'L''D' + status u8 (1 ranked, 0 not, 2 unknown id: sign up again) + rating u16 + RD u16 + wins u16 +
losses u16 + place u16 + text[32] (the ladder name, or why not ranked). After each rollback match both games send RESULT
(22) until RESULT_ACK (23); a game counts only when both sent the same result for the same match seed (ladder.py).
    python ssbu_lobby.py --ladder-db ladder.db [--ladder-open]        the lobby with the ladder (web: /ladder on --status-port)
    python ssbu_lobby.py --ladder-db ladder.db --ladder-invite <name>  prints a one-use invitation code and exits
Tournaments (lobby 1.7, tournament.py, with the ladder: players are its accounts): double elimination brackets run by
the lobby. TOURNEY (29) from a game: cookie u64 + seq u16 + op u8 + tournament id u32 + bracket version held u32 + the op's
arguments. Op 0 only looks; any other op is done once per new seq (the game sends it again until TOURNEY_STATE acknowledges
that seq): 1 sign up, 2 withdraw, 3 create (name[32], bo, bo of the top 8, bo of the finals, bans, stocks: u8 each),
4 start, 5 cancel, 6 seeding mode (u8: 0 rating, 1 random, 2 manual), 7 move a seed (from u8, to u8), 8 disqualify
(entrant u8), 9 a set's result (set u16, winner i8: 0 / 1 side, -1 clear), 10 a stage step (set u16, length u8, stage
name), 11 ready at the set-up (fighter u64, confirm u8), 12 not ready, 13 check in, 14 open the check-in (organizer).
Ops 10-12 always act on the player's own set (the tournament id is not read). TOURNEY_STATE (30): acknowledged seq u16 + text
lines (the player's own view: the list, its set, the stage procedure; see t_state). TOURNEY_DOC (31): tournament id u32 +
version u32 + chunk index u8 + chunks u8 + a piece of the bracket's text (t_doc), sent when the game's version is older.
A tournament player who searches a rollback match (MATCH op 2) is paired only with the opponent of its set, once both are
ready (the stage chosen, the fighters in the right order); the two games' RESULTs of that match count the game for the
set when they agree (a draw, a desync, a disconnection or two different results: the game is played again).
Accounts (the ladder ids, ladder.py): with --ladder-open the account's name follows the game's name. The admin's commands
(they print the outcome and exit): --account-list, --account-rename OLD NEW, --account-ban NAME, --account-unban NAME,
--account-delete NAME.

Arena (lobby 1.6, F1 > Arena): the players of one build and lobby code who joined the arena wait in a queue; the first two
take their turn at a 1-on-1 rollback match (Smash versus: their MATCH op 2 searches are paired only with each other, the
one ahead in the queue is player 1) while the others wait. When the match is over (RESULT of either game: the winner by
stocks, then damage) the winner stays and plays the next in the queue, the loser goes to the back; after --arena-streak
wins in a row (default 3, 0 = no limit) the winner goes to the back too. No result (a draw, a disconnection), a turn not
started in ARENA_TURN_FOR s or a match longer than ARENA_PLAY_FOR s: both go to the back.
    24 ARENA        client: cookie u64, op u8 (0 leave, 1 join / stay in, 2 look only)
    25 ARENA_STATE  lobby:  version u32, status u8 (0 out, 1 waiting, 2 your turn, 3 playing), place u8 (waiting: 1 = next),
                            streak u8, streak limit u8, opponent name[32], seconds in this status u16, then the match: count
                            u8 (0 / 1) + (name[32] x 2, streak u8 x 2, state u8: 2 turn, 3 playing), then the queue: count u8
                            + count x (name[32], streak u8)
A game polls (op 1 or 2) every second while it is in the arena or shows it; the lobby also sends ARENA_STATE at each change.
Spectators of a Battle Arena (lobby 1.7, 2026-11-02; the game's own Online menu on our NEX server, net_session.c): the
members of one arena (its gathering id = the key) form a group; a message to the group goes to every other member of it.
    27 RB_GROUP     client: cookie u64, key u64 (0 = leave the group), message      (a non-zero key also joins that group)
                    lobby:  source u32, message
An older lobby drops type 27: nobody hears the spectators and every member's game plays the match with its own netcode.

Python 3.8+, standard library only. No encryption: run it for people you know."""
import argparse
import json
import os
import secrets
import queue
import re
import hashlib
import hmac
import select
import socket
import sqlite3
import struct
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:                         # ladder.py from this file's own directory (python -I leaves that directory out of sys.path)
    import importlib.util
    _spec = importlib.util.spec_from_file_location("ladder", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ladder.py"))
    ladder_mod = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(ladder_mod)
except (ImportError, OSError):   # (ladder.py not installed next to it: no ladder)
    ladder_mod = None
try:                         # tournament.py, the same way (1.7)
    _spec = importlib.util.spec_from_file_location("tournament", os.path.join(os.path.dirname(os.path.abspath(__file__)), "tournament.py"))
    tournament_mod = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(tournament_mod)
except (ImportError, OSError):
    tournament_mod = None

PROTO = 1
LOBBY_VERSION = "1.7"
(JOIN, WELCOME, REJECT, PING, PONG, DATA, LEAVE, LIST, LISTING, LIST_END,
 PEERS, PUNCH, DIRECT, BCAST) = range(1, 15)
RB = 16          # online rollback messages between two games (recomp-study/net_relay.c T_RB, rb_net.c)
ROOMS, ROOM_INFO, ROOMS_END = 17, 18, 19     # O1 room browser (net_relay.c): every room of this lobby code, any build
MATCH, MATCH_STATE = 20, 21                  # O2 matchmaking (net_relay.c): find an opponent of the same build and code
RESULT, RESULT_ACK = 22, 23                  # 1.5 ladder: a rollback match's result (net_relay.c, ladder_report.c)
ARENA, ARENA_STATE = 24, 25                  # 1.6 arena: a queue of players taking turns at 1-on-1 rollback matches
RB_GROUP = 27                                # 1.7 (2026-11-02): a Battle Arena's group (fighters + spectators, key = the arena)
AR_OUT, AR_WAITING, AR_TURN, AR_PLAYING = range(4)
ARENA_TURN_FOR, ARENA_PLAY_FOR, ARENA_WATCH_FOR, ARENA_LIST = 120.0, 900.0, 3.0, 16
TOURNEY, TOURNEY_STATE, TOURNEY_DOC = 29, 30, 31   # 1.7 tournaments (tournament.py; 26 is the games' own RB_DIRECT, net_relay.c)
(T_LOOK, T_SIGNUP, T_WITHDRAW, T_CREATE, T_START, T_CANCEL, T_SEEDMODE, T_MOVE, T_DQ, T_RESULT, T_STAGE, T_READY,
 T_UNREADY, T_CHECKIN, T_OPEN_CHECKIN, T_LADDER_JOIN, T_LADDER_LEAVE) = range(17)
# (2026-11-03) the ladder queue: a player waits for an opponent; two waiting players of the same build are paired (the
# closest ratings first) into a ladder match, a one-set tournament (tournament.py kind "ladder") played exactly as a
# tournament set (stage strikes, fighters, the match starting by itself); when the set is over both ratings change.
LADDER_RULES = {"kind": "ladder", "bo": 3, "bo_top8": 3, "bo_finals": 3, "bans": 2, "stocks": 3, "checkin": False,
                "noshow": 300, "turn_for": 60, "organizer_name": "Ladder"}
LADDER_RESULT_FOR = 180.0     # s: the game shows the last ladder set's result (rating before / after) this long
LADDER_GONE_FOR = float(os.environ.get("SSBU_LADDER_GONE_FOR", "180"))   # s: a ladder player's game gone this long: the other wins the set (both gone: no set, no rating)
T_PLAYER_OPS = (T_STAGE, T_READY, T_UNREADY)        # (they act on the player's own set, whatever tournament it looks at)
T_CHUNK, T_LIST, T_REPORT_WAIT, T_ERR_FOR = 1000, 8, 60.0, 8.0
LD_UNRANKED, LD_RANKED, LD_UNKNOWN = 0, 1, 2
OUT_WAIT, OUT_OK, OUT_DRAW, OUT_DISPUTED, OUT_UNRANKED = range(5)
RESULT_LEN = 4 + 8 + 8 + 4 + 4 + 4 + 16 + 2 + 4 + 4 + 4
(MS_IDLE, MS_SEARCHING, MS_MATCHED, MS_TIMEOUT, MS_PEER_LEFT, MS_TOGETHER, MS_EXPIRED) = range(7)
ROLE_HOST, ROLE_JOIN = 1, 2
KIND_ROOM, KIND_ROLLBACK = 1, 2              # MATCH op 1 / op 2: a Local Wireless search, a rollback-match search (1.4)
SEARCH_FOR, MATCH_FOR = 120.0, 180.0         # seconds (--search-secs, --match-secs)
MODS_MAX, MOD_NAME, ROOMS_MAX = 32, 31, 32
NAME_LEN, BUILD_LEN = 32, 40
JOIN_LEN = 4 + 8 + 2 * NAME_LEN + BUILD_LEN + 4 + 2 + 1
SUBNET, BROADCASTS = 0x0A0D0000, (0x0A0DFFFF, 0xFFFFFFFF)
LDN_PORT = 65511
M_ADVERTISE, M_JOIN, M_ACCEPT, M_REJECT, M_KEEP, M_LEAVE, M_GONE, M_KICK = range(1, 9)
NI_SIZE, NI_SESSION, NI_POLICY, NI_NODE_MAX, NI_NODE_COUNT, NI_NODES, NODE_LEN = 0x480, 0x10, 0x62, 0x66, 0x67, 0x68, 0x40
LIST_MAX = 24
CLIENT_TIMEOUT, ROOM_TIMEOUT, LISTED_FOR = 10.0, 10.0, 4.0
MAX_DATA = 1500


def hdr(kind):
    return b"SU" + bytes((PROTO, kind))


def text(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def vip_text(ip):
    return "10.13.%d.%d" % (ip >> 8 & 255, ip & 255)


def node_name(info, i):
    o = NI_NODES + NODE_LEN * i + 0xC
    return text(info[o:o + NAME_LEN])


def mods_block(raw):
    """A game's mod-set block (count u8, count x (hash u32, length u8, name)) checked and cut to its own length, or None."""
    if not raw or raw[0] > MODS_MAX:
        return None
    o = 1
    for _ in range(raw[0]):
        if o + 5 > len(raw) or raw[o + 4] > MOD_NAME or o + 5 + raw[o + 4] > len(raw):
            return None
        o += 5 + raw[o + 4]
    return bytes(raw[:o])


def room_nodes(info, owner):
    """The station addresses of a NetworkInfo whose node 0 is owner, or None if it is not one a game makes."""
    count, most = info[NI_NODE_COUNT], info[NI_NODE_MAX]
    if not 1 <= count <= most <= 8:
        return None
    ips = []
    for i in range(8):
        o = NI_NODES + NODE_LEN * i
        if not info[o + 0xB]:
            if i == 0:
                return None
            continue
        ip = struct.unpack_from("<I", info, o)[0]
        if ip & 0xFFFF0000 != SUBNET or (i == 0 and ip != owner) or ip in ips:
            return None
        ips.append(ip)
    return frozenset(ips[1:])


class Client:
    def __init__(self, at, nonce, ip, name, part, lan, direct, now):
        self.at, self.nonce, self.ip, self.name, self.part = at, nonce, ip, name, part
        self.lan, self.direct = lan, direct
        self.cookie = secrets.randbits(64)
        self.seen = now
        self.room = None          # the Room this player hosts
        self.claim = None         # the session id of the room it asked to join
        self.peers_version = 0
        self.peers_sent = None
        self.mods = None          # its mod-set block (None: an older game that does not send it)
        self.search_id = None     # O2: the id of its current search (MATCH), None = never searched
        self.search_since = None  # searching since (None: not searching)
        self.match = None         # the Match it is in
        self.final = None         # (search id, state, role, peer ip, peer name): how its last search ended (sent again if asked)
        self.kind = KIND_ROOM     # 1.4: what it searches (KIND_ROOM / KIND_ROLLBACK)
        self.rb_peer = 0          # 1.4: the player of its last rollback match (RB to it is relayed on the public code)
        self.ladder_id = None     # 1.5: its ladder id when it is ranked
        self.ladder_why = ""      # (its ladder name, or why it is not ranked)
        self.arena_streak = 0     # 1.6: wins in a row in the arena
        self.arena_seen = None    # 1.6: when it last asked for the arena's state (it is shown the changes for ARENA_WATCH_FOR s)
        self.rb_group = None      # 1.7: the Battle Arena group it is in (RB_GROUP key), None = none
        self.t_seq = None         # 1.7: the last TOURNEY op done (its seq), its outcome, the tournament it looks at
        self.t_err = ""
        self.t_err_at = 0.0
        self.t_focus = 0
        self.t_have = 0


class Arena:
    """1.6: the arena of one build and lobby code: the queue and the match being played (or about to be)."""
    def __init__(self):
        self.queue = []           # Clients waiting, the next first
        self.pair = None          # [player 1, player 2] taking their turn, or None
        self.playing = False      # the pair's games exchange rollback messages
        self.at = 0.0             # since when the pair is in its state
        self.version = 0
        self.last_seed = None     # the last match whose result was taken (both games send it)

    def members(self):
        return list(self.queue) + (list(self.pair) if self.pair else [])


class Match:
    def __init__(self, host, guest, now):
        self.host, self.guest, self.at = host, guest, now
        self.room_said = False    # the guest was told the host's room is open
        self.rb_seen = set()      # (rollback matches) the players whose RB to the other one the lobby relayed

    def other(self, c):
        return self.guest if c is self.host else self.host


class Room:
    def __init__(self, owner, session, now):
        self.owner, self.session, self.created = owner, session, now
        self.info, self.nodes, self.updated = b"", frozenset(), now
        self.secrets = {}         # frozenset({a, b}) -> pair secret

    def is_open(self):
        return self.info[NI_POLICY] != 1 and self.info[NI_NODE_COUNT] < self.info[NI_NODE_MAX]


# ---- 1.7 tournaments
ACC_B32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def account_code_bytes(code):
    """the 50 bytes (id[16] + key[32] + check[2]) of an account code ("SSBU-XXXXXXXX-..." , net_relay.c net_account_code),
    or None when it is not one"""
    t = str(code or "").strip().upper()
    if t.startswith("SSBU"):
        t = t[4:]
    acc = bits = 0
    out = bytearray()
    for ch in t:
        if ch in "- \t\r\n":
            continue
        ch = "0" if ch == "O" else "1" if ch in "IL" else ch
        v = ACC_B32.find(ch)
        if v < 0:
            return None
        acc, bits = (acc << 5) | v, bits + 5
        if bits >= 8:
            out.append((acc >> (bits - 8)) & 0xff)
            bits -= 8
            acc &= (1 << bits) - 1
    if len(out) != 50 or bits:
        return None
    if hashlib.sha256(bytes(out[:48])).digest()[:2] != bytes(out[48:]):
        return None
    return bytes(out)


def t_bytes(x):
    return x.encode("utf-8", "replace")


def t_text(raw):
    return " ".join(raw.split(b"\0", 1)[0].decode("utf-8", "replace").split())


class TourneyMixin:
    """Lobby's tournament part (1.7): ops, the players' views, pairing and results (tournament.py does the bracket)."""

    def t_pid(self, c):
        return c.ladder_id.hex() if c.ladder_id is not None else None

    # ---- (2026-11-03) the ladder queue
    def lq_ladder_match(self, c):
        """c's ladder match being played (running), or None"""
        pid = self.t_pid(c)
        if pid is None:
            return None
        return next((t for t in self.t_list(c.part) if t.is_ladder() and t.state == "running" and t.find(pid) >= 0), None)

    def lq_in_tournament(self, c):
        """c is still in a tournament (signed up, or running and not knocked out): no ladder for it meanwhile"""
        pid = self.t_pid(c)
        for t in self.t_list(c.part):
            e = t.find(pid) if pid else -1
            if t.is_ladder() or e < 0 or t.state in ("done", "cancelled"):
                continue
            if t.state in ("signup", "checkin"):
                return t
            if not t.entrants[e]["dq"] and all(x != e for _, x in t.standings()):
                return t
        return None

    def lq_join(self, c, now):
        if self.ladder is None or self.tourneys is None:
            return "this server has no ladder"
        if self.lq_ladder_match(c) is not None:
            return "you are playing a ladder set now"
        t = self.lq_in_tournament(c)
        if t is not None:
            return "you are in the tournament %s: the ladder opens again when you are out of it" % t.name
        if c not in self.lq:
            self.lq[c] = now
            self.say("ladder: %s waits for an opponent (%d in the queue)" % (c.ladder_why, sum(1 for x in self.lq if x.part == c.part)))
        return None

    def lq_leave(self, c, why):
        if self.lq.pop(c, None) is not None:
            self.say("ladder: %s left the queue (%s)" % (c.ladder_why, why))

    def lq_guard(self, now):
        """running ladder matches whose players' games are gone: one gone LADDER_GONE_FOR s, the other wins the set (it
        counts); both gone that long, the match is cancelled (no rating)"""
        if self.tourneys is None:
            return
        for t in [x for x in self.tourneys.all.values() if x.is_ladder() and x.state == "running"]:
            gone = []
            for e in (0, 1):
                key = (t.id, e)
                if self.t_client(t, e) is None:
                    self.lq_gone.setdefault(key, now)
                    if now - self.lq_gone[key] >= LADDER_GONE_FOR:
                        gone.append(e)
                else:
                    self.lq_gone.pop(key, None)
            if not gone:
                continue
            if len(gone) == 2:
                t.cancel()
                self.say("ladder: match %d cancelled: both games left" % t.id)
            else:
                v = t.view[0]
                t.override(0, 1 - v["p"].index(gone[0]), by="left")
                self.say("ladder: match %d: %s left, %s wins the set" % (t.id, t.entrants[gone[0]]["name"], t.entrants[1 - gone[0]]["name"]))
            for e in (0, 1):
                self.lq_gone.pop((t.id, e), None)
            self.t_changed(t, now)

    def lq_sweep(self, now):
        """pair the waiting players of each build: the one waiting longest with the closest rating among the others"""
        self.lq_guard(now)
        if not self.lq:
            return
        alive = set(self.by_at.values())
        for c in list(self.lq):
            if c not in alive or c.ladder_id is None:
                self.lq_leave(c, "left the lobby")
            elif self.t_active(c) is not None:
                self.lq_leave(c, "a tournament set is ready")
        parts = {}
        for c, since in sorted(self.lq.items(), key=lambda kv: kv[1]):
            parts.setdefault(c.part, []).append(c)
        for part, waiting in parts.items():
            while len(waiting) >= 2:
                a = waiting.pop(0)
                ra = (self.ladder.player(a.ladder_id) or (None, 1500.0))[1]
                b = min(waiting, key=lambda x: abs((self.ladder.player(x.ladder_id) or (None, 1500.0))[1] - ra))
                waiting.remove(b)
                self.lq_match(a, b, now)

    def lq_match(self, a, b, now):
        self.lq.pop(a, None)
        self.lq.pop(b, None)
        rules = dict(LADDER_RULES, part=[a.part[0].decode("ascii", "replace"), a.part[1]])
        t = self.tourneys.create("Ladder: %s vs %s" % (a.ladder_why, b.ladder_why), "", rules)
        for x in (a, b):
            row = self.ladder.player(x.ladder_id)
            t.sign_up(x.ladder_id.hex(), x.ladder_why, row[1] if row else None)
        t.start()
        self.say("ladder: match %d: %s vs %s" % (t.id, a.ladder_why, b.ladder_why))
        self.t_changed(t, now)

    def t_rate(self, t):
        """(2026-11-03) the ratings of finished sets: a ladder match's set, and every set of a rated tournament won by
        playing it (not a bye, a forfeit or the organizer's result). Each set once (rules["rated_sets"])."""
        if self.ladder is None or not t.sets or not (t.is_ladder() or t.rules.get("rated")):
            return
        done = t.rules.setdefault("rated_sets", [])
        for i, v in enumerate(t.view):
            r = t.results.get(str(i)) or {}
            ok = v["state"] == "done" and (r.get("by") == "games" or (t.is_ladder() and r.get("by") in ("noshow", "left")))
            if not ok or i in done or None in v["p"] or tournament_mod.BYE in v["p"]:
                continue
            done.append(i)
            ea, eb = v["p"]
            pa, pb = t.entrants[ea]["pid"], t.entrants[eb]["pid"]
            sc = t.score(i)
            games = []
            for g in r.get("games", []):
                ch = list(g.get("chara") or [None, None]) + [None, None]
                games.append((g.get("winner"), ch[0] if isinstance(ch[0], int) else None, ch[1] if isinstance(ch[1], int) else None))
            try:
                out = self.ladder.record_set(bytes.fromhex(pa), bytes.fromhex(pb), 1 if v["winner"] == ea else 2, sc[0], sc[1],
                                             t.id if t.is_ladder() else None, games)
            except ValueError:
                out = None
            if out is None:
                continue
            names = [t.entrants[ea]["name"], t.entrants[eb]["name"]]
            self.say("ladder: %s %d - %d %s: ratings %d -> %d, %d -> %d" % (names[0], sc[0], sc[1], names[1],
                                                                         out[0][0], out[0][1], out[1][0], out[1][1]))
            if t.is_ladder():
                for k, (pid, e) in enumerate(((pa, ea), (pb, eb))):
                    self.lq_last[pid] = {"tid": t.id, "won": v["winner"] == e, "mine": sc[k], "theirs": sc[1 - k],
                                         "before": out[k][0], "after": out[k][1], "opp": names[1 - k], "at": time.monotonic()}
            t.changed()

    def t_list(self, part):
        if self.tourneys is None:
            return []
        key = [part[0].decode("ascii", "replace"), part[1]]
        return [t for t in sorted(self.tourneys.all.values(), key=lambda t: t.id) if t.rules.get("part") == key]

    def t_client(self, t, e):
        pid = t.entrants[e]["pid"]
        for x in self.by_at.values():
            if x.ladder_id is not None and x.ladder_id.hex() == pid and t.rules.get("part") == [x.part[0].decode("ascii", "replace"), x.part[1]]:
                return x
        return None

    def t_active(self, c):
        """(tournament, entrant, set id) of c's set being played now, or None"""
        pid = self.t_pid(c)
        if pid is None:
            return None
        for t in self.t_list(c.part):
            if t.state != "running":
                continue
            e = t.find(pid)
            if e < 0 or t.entrants[e]["dq"]:
                continue
            sid = t.set_of(e)
            if sid is not None:
                return t, e, sid
        return None

    def t_busy(self, part):
        """the clients playing a tournament set now (other searches never take them)"""
        out = set()
        for t in self.t_list(part):
            if t.state == "running":
                for v in t.view:
                    if v["state"] in ("ready", "playing"):
                        for e in v["p"]:
                            x = self.t_client(t, e)
                            if x is not None:
                                out.add(x)
        return out

    def t_pairing(self, c):
        """None: c plays no tournament set; else (the opponent's client or None while they are not both ready, side of c)"""
        a = self.t_active(c)
        if a is None:
            return None
        t, e, sid = a
        side = t.view[sid]["p"].index(e)
        if not t.go(sid):
            return None, side
        return self.t_client(t, t.view[sid]["p"][1 - side]), side

    def on_tourney(self, c, p, now):
        seq, op, tid, have = struct.unpack_from(">HBII", p, 12)
        args = p[23:]
        if tid and op not in T_PLAYER_OPS:
            c.t_focus = tid                         # (before the op: a new tournament becomes the one looked at)
        if op != T_LOOK and seq != c.t_seq:
            c.t_seq = seq
            try:
                c.t_err = self.t_do(c, op, tid, args, now) or ""
            except tournament_mod.Error as e:
                c.t_err = str(e)
            except (struct.error, IndexError, ValueError):
                c.t_err = "a malformed request"
            c.t_err_at = now
        c.t_have = have
        self.t_send(c, now)

    def t_do(self, c, op, tid, args, now):
        if self.tourneys is None:
            return "this lobby has no tournaments (it needs the ladder: --ladder-db)"
        pid = self.t_pid(c)
        if pid is None:
            return "you need an account on this lobby first (F1 > Online > Your account)"
        if op == T_LADDER_JOIN:                       # (2026-11-03) the ladder queue
            return self.lq_join(c, now)
        if op == T_LADDER_LEAVE:
            self.lq_leave(c, "left by the player")
            return None
        if op == T_CREATE:
            if self.tourney_admins is not None and c.ladder_why.lower() not in self.tourney_admins:
                return "only the lobby's organizers can make tournaments here"
            name = t_text(args[:32]) or "%s's tournament" % c.ladder_why
            bo, b8, bf, bans, stocks = args[32:37]
            for x in (bo, b8, bf):
                if x not in (1, 3, 5, 7):
                    return "a set is a best of 1, 3, 5 or 7"
            if bans > 4 or not 1 <= stocks <= 5:
                return "0 to 4 bans, 1 to 5 stocks"
            noshow = args[37] * 60 if len(args) > 37 else 600     # (2026-11-03) the game's no-show minutes (an older game: 10)
            t = self.tourneys.create(name, pid, {"bo": bo, "bo_top8": b8, "bo_finals": bf, "bans": bans, "stocks": stocks, "noshow": min(noshow, 7200),
                                                 "part": [c.part[0].decode("ascii", "replace"), c.part[1]], "organizer_name": c.ladder_why})
            c.t_focus = t.id
            self.say("tournament %d \"%s\" made by %s" % (t.id, name, c.ladder_why))
            return self.t_changed(t, now)
        if op in T_PLAYER_OPS:
            a = self.t_active(c)
            if a is None:
                return "you have no set to play now"
            t, e, sid = a
            if op == T_STAGE:
                sid2, n = struct.unpack_from(">HB", args, 0)
                if sid2 != sid:
                    return "that set is over"
                t.stage_step(sid, e, args[3:3 + n].decode("ascii", "replace"))
            elif op == T_READY:
                chara, confirm = struct.unpack_from(">QB", args, 0)
                t.seen_at_setup(sid, e)
                t.ready(sid, e, chara, bool(confirm))
            else:
                t.unready(sid, e)
            return self.t_changed(t, now)
        t = self.tourneys.get(tid)
        if t is None or t.rules.get("part") != [c.part[0].decode("ascii", "replace"), c.part[1]]:
            return "no such tournament (or it is for another version of the game)"
        org = pid == t.organizer
        e = t.find(pid)
        if op == T_SIGNUP:
            row = self.ladder.player(c.ladder_id)
            rated = row is not None and row[4] + row[5] > 0
            t.sign_up(pid, c.ladder_why, row[1] if rated else None)
            self.say("tournament %d: %s signed up (%d players)" % (t.id, c.ladder_why, len(t.entrants)))
        elif op == T_WITHDRAW:
            t.withdraw(pid)
            self.say("tournament %d: %s withdrew" % (t.id, c.ladder_why))
        elif op == T_CHECKIN:
            t.check_in(pid)
            self.say("tournament %d: %s checked in" % (t.id, c.ladder_why))
        elif op in (T_START, T_CANCEL, T_SEEDMODE, T_MOVE, T_DQ, T_RESULT, T_OPEN_CHECKIN):
            if not org:
                return "only the organizer (%s) can do that" % t.rules.get("organizer_name", "?")
            if op == T_START:
                t.start()
                self.say("tournament %d: started with %d players" % (t.id, len(t.entrants)))
            elif op == T_CANCEL:
                t.cancel()
                self.say("tournament %d: cancelled" % t.id)
            elif op == T_OPEN_CHECKIN:
                t.open_checkin()
                self.say("tournament %d: check-in open" % t.id)
            elif op == T_SEEDMODE:
                t.set_seed_mode(("rating", "random", "manual")[args[0]] if args[0] < 3 else "?")
            elif op == T_MOVE:
                t.move(args[0], args[1])
            elif op == T_DQ:
                t.dq(args[0])
            elif op == T_RESULT:
                sid, w = struct.unpack_from(">Hb", args, 0)
                t.override(sid, None if w < 0 else w)
        else:
            return "unknown request"
        return self.t_changed(t, now)

    def t_changed(self, t, now):
        """save t, and send the new state to its players (and to whoever looks at it)"""
        self.t_rate(t)
        self.tourneys.save(t)
        for x in list(self.by_at.values()):
            if x.t_focus == t.id or (x.ladder_id is not None and t.find(x.ladder_id.hex()) >= 0):
                self.t_send(x, now)
        return None

    def t_doc(self, t):
        """the bracket as text lines (the game parses them, net_relay.c tourney_doc)"""
        r = t.rules
        lines = ["T %d %s %s %d %d %d %d %d %s" % (t.id, t.state, t.seed_mode, r["bo"], r["bo_top8"], r["bo_finals"], r["bans"], r["stocks"],
                                                   r.get("organizer_name", "?")),
                 "N %s" % t.name,
                 "P s %s" % " ".join(r["starters"]), "P c %s" % " ".join(r["counterpicks"])]
        for i, en in enumerate(t.entrants):
            lines.append("E %d %d %s %s" % (i, 1 if en["dq"] else 0, "-" if en["rating"] is None else "%d" % en["rating"], en["name"]))
        for i, st in enumerate(t.sets):
            v = t.view[i]
            sc = t.score(i)
            lines.append("S %d %s %d %d %d %s %d %d %d %d" % (i, st["br"], st["round"], -2 if v["p"][0] is None else v["p"][0],
                                                            -2 if v["p"][1] is None else v["p"][1], v["state"],
                                                            -1 if v["winner"] in (None, tournament_mod.BYE) else v["winner"], sc[0], sc[1], v["bo"]))
        if t.sets:
            for place, e in t.standings():
                lines.append("R %d %d" % (place, e))
        return t_bytes("\n".join(lines) + "\n")

    def t_state(self, c):
        """the player's own view (TOURNEY_STATE text): the list, the tournament it looks at, its set and what it must do"""
        pid = self.t_pid(c)
        out = ["acct %d %d" % (1 if pid else 0, 1 if (pid and (self.tourney_admins is None or c.ladder_why.lower() in self.tourney_admins)) else 0)]
        if self.tourneys is None:
            out.append("off")
        if c.t_err and time.monotonic() - c.t_err_at < T_ERR_FOR:     # (an error is shown a few seconds, then it is gone)
            out.append("err %s" % c.t_err)
        ts = [t for t in self.t_list(c.part) if not t.is_ladder()]
        mine = [t for t in ts if pid and t.find(pid) >= 0]
        focus = self.tourneys.get(c.t_focus) if self.tourneys is not None and c.t_focus else None
        if focus not in ts:
            focus = mine[-1] if mine else None
        shown = (mine + [t for t in reversed(ts) if t not in mine])[:T_LIST]
        for t in sorted(shown, key=lambda t: t.id):
            out.append("t %d %s %d %d %d %s" % (t.id, t.state, len(t.entrants), 1 if pid and t.find(pid) >= 0 else 0,
                                                 1 if pid == t.organizer else 0, t.name))
        if focus is not None:
            out.append("f %d %d %d" % (focus.id, focus.version, (len(self.t_doc(focus)) + T_CHUNK - 1) // T_CHUNK))
        # (2026-11-02) the player's own tournament (its set, check-in, place), whatever tournament the list shows:
        # "mt <id> <state> <checked in 0/1> <name>" then the "me" lines of it
        own = self.lq_ladder_match(c) or next((t for t in mine if t.state == "running"), None) or next((t for t in mine if t.state == "checkin"), None) \
            or next((t for t in reversed(mine) if t.state == "done"), None) or next((t for t in mine if t.state == "signup"), None)
        # (2026-11-03) the ladder: "lq <in the queue 0/1> <seconds waited> <players waiting> <rating> <wins> <losses> <place>",
        # "lk" (the own tournament is a ladder match), "lr <match id> <won 0/1> <mine> <theirs> <rating before> <after> <opponent>"
        if self.ladder is not None and pid:
            row = self.ladder.player(c.ladder_id)
            out.append("lq %d %d %d %d %d %d %d" % (1 if c in self.lq else 0, int(time.monotonic() - self.lq[c]) if c in self.lq else 0,
                                                   sum(1 for x in self.lq if x.part == c.part), round(row[1]) if row else 0,
                                                   row[4] if row else 0, row[5] if row else 0, self.ladder.rank(c.ladder_id) if row else 0))
            last = self.lq_last.get(pid)
            if last is not None and time.monotonic() - last["at"] < LADDER_RESULT_FOR:
                out.append("lr %d %d %d %d %d %d %s" % (last["tid"], 1 if last["won"] else 0, last["mine"], last["theirs"],
                                                        round(last["before"]), round(last["after"]), last["opp"]))
        if own is not None and own.is_ladder():
            out.append("lk")
        if own is not None:
            e = own.find(pid)
            out.append("mt %d %s %d %s" % (own.id, own.state, 1 if own.entrants[e].get("in") else 0, own.name))
            if own.state == "running":
                sid = own.set_of(e)
                if sid is None:
                    gone = own.entrants[e]["dq"] or any(x == e for _, x in own.standings())
                    place = next((pl for pl, x in own.standings() if x == e), 0)
                    out.append("me %d -1 %s %d" % (e, "dq" if own.entrants[e]["dq"] else "out" if gone else "wait", place))
                    if not gone:
                        out.append(self.t_next_line(own, e))
                else:
                    out += self.t_set_lines(own, e, sid)
            elif own.state == "done":
                place = next((pl for pl, x in own.standings() if x == e), 0)
                out.append("me %d -1 done %d" % (e, place))
        return "\n".join(out)

    def t_next_line(self, t, e):
        """"next <br> <round> <who>": the set entrant e plays next, and who it meets there (a name, or "A / B": the
        players of the set whose winner or loser comes)"""
        for i, v in enumerate(t.view):
            if v["state"] == "waiting" and e in v["p"]:
                st = t.sets[i]
                k = 1 - v["p"].index(e)
                src = st["src"][k]
                if v["p"][k] not in (None, tournament_mod.BYE):
                    who = t.entrants[v["p"][k]]["name"]
                elif src[0] in ("W", "L"):
                    names = [t.entrants[x]["name"] if x not in (None, tournament_mod.BYE) else "?" for x in t.view[src[1]]["p"]]
                    who = "%s of %s / %s" % ("winner" if src[0] == "W" else "loser", names[0], names[1])
                else:
                    who = "?"
                return "next %s %d %s" % (st["br"], st["round"], who)
        return "next - 0 -"

    def t_set_lines(self, t, e, sid):
        v = t.view[sid]
        side = v["p"].index(e)
        opp = v["p"][1 - side]
        p = t.proc(sid)
        sc = t.score(sid)
        st = t.sets[sid]
        lines = ["me %d %d set %d %d" % (e, sid, side, 1 if side == 0 else 0),
                 "round %s %d %d" % (st["br"], st["round"], v["bo"]),
                 "opp %s" % t.entrants[opp]["name"],
                 "score %d %d %d %d" % (sc[side], sc[1 - side], t.needed(sid), p["game"]),
                 "rules %d" % t.rules["stocks"]]
        turn = 1 if p["phase"] in ("strike", "ban", "pick") and p["turn"] == side else 0
        tl = t.turn_left(sid)
        lines.append("phase %s %d %d %d" % (p["phase"], turn, p.get("left", 0), -1 if tl is None else tl))
        lines.append("stage %s" % (p["stage"] or "-"))
        lines.append("pool %s" % " ".join(p["pool"]))
        lines.append("struck %s" % " ".join(p["struck"]))
        first = t.first_pick(sid)
        lines.append("first %d" % (0 if first is None else 1 if first == side else 2))
        mine, theirs = p["ready"][side], p["ready"][1 - side]
        reveal = theirs is not None and first is not None and first != side      # (game 2 on: the winner's fighter, to the loser)
        lines.append("ready %d %d %s %s" % (1 if mine else 0, 1 if theirs else 0, "%016x" % theirs["chara"] if reveal else "-",
                                            tournament_mod.chara_name(theirs["chara"]) if reveal else ""))
        lines.append("go %d" % (1 if t.go(sid) else 0))
        if t.featured(sid):
            lines.append("feature %s" % t.featured(sid))
        return lines

    def t_send(self, c, now):
        body = t_bytes(self.t_state(c))[:1400]
        self.send(hdr(TOURNEY_STATE) + struct.pack(">H", c.t_seq or 0) + body, c.at)
        t = self.tourneys.get(c.t_focus) if self.tourneys is not None and c.t_focus else None
        if t is None and self.tourneys is not None:
            a = self.t_active(c)
            pid = self.t_pid(c)
            t = a[0] if a else next((x for x in self.t_list(c.part) if pid and x.find(pid) >= 0 and x.state != "done"), None)
        if t is not None and c.t_have != t.version:
            doc = self.t_doc(t)
            n = (len(doc) + T_CHUNK - 1) // T_CHUNK
            for i in range(n):
                self.send(hdr(TOURNEY_DOC) + struct.pack(">IIBB", t.id, t.version, i, n) + doc[i * T_CHUNK:(i + 1) * T_CHUNK], c.at)

    def t_playing(self, h, g, now):
        """the two games of a tournament set exchange rollback messages: that game is on (tournament.py playing)"""
        a = self.t_active(h)
        if a is not None:
            t, e, sid = a
            other = t.view[sid]["p"][1 - t.view[sid]["p"].index(e)]
            if self.t_client(t, other) is g and t.playing(sid):
                self.say("tournament %d: set %d: game %d on" % (t.id, sid, t.proc(sid)["game"]))
                self.t_changed(t, now)

    def t_result(self, c, p, now):
        """RESULT of a tournament game: counted once both games' reports agree (see the top of this file)"""
        a = self.t_active(c)
        if a is None:
            return
        t, e, sid = a
        seed, peer, slot, rule_kind, stock, flags = struct.unpack_from(">QIBBBB", p, 12)
        c0, c1, s0, s1, d0, d1, frames, desyncs = struct.unpack_from(">QQBBHHII", p, 32)
        if seed in self.t_counted or slot > 1:
            return
        side = t.view[sid]["p"].index(e)
        opp = self.by_ip.get(peer)                 # (2026-11-02) a fight against someone else (a casual arena game): not this set's
        if opp is None or opp is not self.t_client(t, t.view[sid]["p"][1 - side]):
            return
        rep = self.t_reports.setdefault(seed, {"at": now, "tid": t.id, "sid": sid, "by": {}})
        if rep["tid"] != t.id or rep["sid"] != sid:
            return
        win_slot = ladder_mod.game_winner((s0, s1), (d0, d1)) if ladder_mod is not None else None
        # (2026-11-02) the reporter is match slot `slot` and set side `side`: the winning slot is this side or the other one
        # (before: "slot = side" was assumed, the host being side 0)
        win = None if win_slot is None else side if win_slot == slot else 1 - side
        why = "a desync" if desyncs else "a disconnection" if flags & 2 else "a player left the match" if flags & 1 else ""
        mine_c = (c0, c1) if slot == 0 else (c1, c0)        # (fighters by side: this side's first)
        rep["by"][side] = {"win": win, "bad": bool(why), "chara": mine_c if side == 0 else mine_c[::-1], "why": why}
        if len(rep["by"]) == 2:
            self.t_settle(seed, now)

    def t_settle(self, seed, now):
        rep = self.t_reports.pop(seed)
        self.t_counted.add(seed)
        t = self.tourneys.get(rep["tid"])
        if t is None or t.view[rep["sid"]]["state"] not in ("ready", "playing"):
            return
        sid, by = rep["sid"], list(rep["by"].values())
        wins = {r["win"] for r in by}
        why = rep.get("why") or next((r["why"] for r in by if r["bad"]), "") or ("a draw" if None in wins else "the two games disagree" if len(wins) > 1 else "")
        if why:
            p = t.proc(sid)
            if p is not None:
                p["ready"], p["phase"] = [None, None], "chars"
            for e in t.view[sid]["p"]:
                x = self.t_client(t, e)
                if x is not None:
                    x.t_err, x.t_err_at = "the last game was not counted (%s): play it again" % why, now
            self.say("tournament %d: set %d: a game was not counted (%s)" % (t.id, sid, why))
            t.changed()
        else:
            w = wins.pop()
            out = t.game_result(sid, w, chara=by[0]["chara"])
            sc = t.score(sid)
            names = [t.entrants[x]["name"] for x in t.view[sid]["p"]]
            self.say("tournament %d: set %d: %s won a game (%s %d - %d %s)%s" % (t.id, sid, names[w], names[0], sc[0], sc[1], names[1],
                                                                             ": the set is over" if out == "set" else ""))
            if t.state == "done":
                ch = t.champion()
                self.say("tournament %d: over, %s won it" % (t.id, t.entrants[ch]["name"] if ch is not None and ch >= 0 else "?"))
        self.t_changed(t, now)

    def t_sweep(self, now):
        """the tournaments' clocks (stage steps, no-shows: tournament.py sweep); a game with one report only after
        T_REPORT_WAIT s: it counts when the other player's game is gone (it closed), else the game is played again"""
        if self.tourneys is None:
            return
        for t in list(self.tourneys.all.values()):
            done = t.sweep()
            for sid, what in done:
                self.say("tournament %d: set %d: %s" % (t.id, sid, what))
            if done:
                self.t_changed(t, now)
        for seed, rep in list(self.t_reports.items()):
            if now - rep["at"] > T_REPORT_WAIT:
                t = self.tourneys.get(rep["tid"])
                if t is not None and len(rep["by"]) == 1:
                    missing = 1 - next(iter(rep["by"]))
                    e = t.view[rep["sid"]]["p"][missing]
                    if e not in (None, tournament_mod.BYE) and self.t_client(t, e) is not None:
                        rep["why"] = "only one of the two games sent its result"
                self.t_settle(seed, now)

    # ---- (2026-11-02) the organizer's web page: log in with the account code, then the same actions as F1 > Tournament
    WEB_LOGIN_FOR = 12 * 3600.0

    def web_call(self, fn, timeout=5.0):
        """from an HTTP thread: run fn(now) on the lobby's thread (the main loop drains web_q), and return its result"""
        box, ev = {}, threading.Event()
        self.web_q.put((fn, ev, box))
        if not ev.wait(timeout):
            raise tournament_mod.Error("the lobby is busy: try again")
        if "e" in box:
            raise box["e"]
        return box.get("r")

    def web_drain(self, now):
        while True:
            try:
                fn, ev, box = self.web_q.get_nowait()
            except queue.Empty:
                return
            try:
                box["r"] = fn(now)
            except Exception as e:
                box["e"] = e if isinstance(e, tournament_mod.Error) else tournament_mod.Error("error: %r" % (e,))
            ev.set()

    def web_nonce(self, now):
        """a one-use challenge for the login (valid 2 minutes)"""
        self.web_nonces = {n: t for n, t in getattr(self, "web_nonces", {}).items() if t > now}
        n = secrets.token_hex(16)
        self.web_nonces[n] = now + 120.0
        return {"nonce": n}

    def web_login(self, a, now):
        """the page decodes the account code (F1 > Online > Account code) itself and sends only the account id and
        HMAC-SHA256(key, nonce): the code never travels (the page may be plain HTTP). -> a login token, or Error"""
        nonce = str(a.get("nonce", ""))
        if getattr(self, "web_nonces", {}).pop(nonce, 0) < now:
            raise tournament_mod.Error("the login took too long: try again")
        try:
            pid, proof = bytes.fromhex(str(a.get("id", ""))), bytes.fromhex(str(a.get("proof", "")))
        except ValueError:
            pid = proof = b""
        if len(pid) != 16 or len(proof) != 32:
            raise tournament_mod.Error("that is not an account code (F1 > Online > Account code > Copy)")
        if self.ladder is None:
            raise tournament_mod.Error("this lobby has no accounts")
        row = self.ladder.db.execute("SELECT key, name, banned FROM players WHERE id = ?", (pid,)).fetchone()
        if row is None or not hmac.compare_digest(hmac.new(bytes(row[0]), nonce.encode(), hashlib.sha256).digest(), proof):
            raise tournament_mod.Error("no account with that code on this lobby")
        if row[2]:
            raise tournament_mod.Error("that account is banned")
        for tok, v in list(self.web_tokens.items()):
            if v[2] < now:
                del self.web_tokens[tok]
        tok = secrets.token_hex(16)
        self.web_tokens[tok] = (pid.hex(), row[1], now + self.WEB_LOGIN_FOR)
        self.say("web: %s logged in as organizer" % row[1])
        return {"token": tok, "name": row[1], "admin": self.tourney_admins is None or row[1].lower() in self.tourney_admins}

    def web_who(self, token, now):
        v = self.web_tokens.get(token or "")
        if v is None or v[2] < now:
            raise tournament_mod.Error("log in again (paste your account code)")
        return v

    def web_action(self, token, tid, action, a, now):
        """an organizer's action from the web page; returns {"ok": True} or raises Error (its text is shown)"""
        if self.tourneys is None:
            raise tournament_mod.Error("this lobby has no tournaments")
        pid, name, _ = self.web_who(token, now)
        if action == "me":
            return {"name": name, "admin": self.tourney_admins is None or name.lower() in self.tourney_admins}
        if action == "create":
            if self.tourney_admins is not None and name.lower() not in self.tourney_admins:
                raise tournament_mod.Error("only the lobby's organizers can make tournaments here")
            game = next((x for x in self.by_at.values() if x.ladder_id is not None and x.ladder_id.hex() == pid), None)
            if game is None:
                raise tournament_mod.Error("open your game (connected to this server) first: the tournament is made for its version")
            rules = {"bo": int(a.get("bo", 3)), "bo_top8": int(a.get("bo_top8", 3)), "bo_finals": int(a.get("bo_finals", 5)),
                     "bans": int(a.get("bans", 2)), "stocks": int(a.get("stocks", 3)), "checkin": bool(a.get("checkin", True)),
                     "rated": bool(a.get("rated", False)), "noshow": max(0, int(a.get("noshow", 600))),
                     "turn_for": max(0, int(a.get("turn_for", 60))),
                     "part": [game.part[0].decode("ascii", "replace"), game.part[1]], "organizer_name": name}
            if any(rules[k] not in (1, 3, 5, 7) for k in ("bo", "bo_top8", "bo_finals")):
                raise tournament_mod.Error("a set is a best of 1, 3, 5 or 7")
            if not 0 <= rules["bans"] <= 4 or not 1 <= rules["stocks"] <= 5:
                raise tournament_mod.Error("0 to 4 bans, 1 to 5 stocks")
            tname = " ".join(str(a.get("name", "")).split())[:40] or "%s's tournament" % name
            t = self.tourneys.create(tname, pid, rules)
            self.say("tournament %d \"%s\" made by %s (web)" % (t.id, tname, name))
            self.t_changed(t, now)
            return {"ok": True, "id": t.id}
        t = self.tourneys.get(int(tid))
        if t is None:
            raise tournament_mod.Error("no such tournament (or it is over)")
        if pid != t.organizer:
            raise tournament_mod.Error("only the organizer (%s) can do that" % t.rules.get("organizer_name", "?"))
        if action == "open_checkin":
            t.open_checkin()
        elif action == "start":
            t.start()
            self.say("tournament %d: started with %d players (web)" % (t.id, len(t.entrants)))
        elif action == "cancel":
            t.cancel()
        elif action == "seed_mode":
            t.set_seed_mode(str(a.get("mode", "")))
        elif action == "move":
            t.move(int(a["i"]), int(a["j"]))
        elif action == "dq":
            t.dq(int(a["i"]))
        elif action == "check_in":                # (the organizer checks a player in by hand)
            i = int(a["i"])
            if not 0 <= i < len(t.entrants):
                raise tournament_mod.Error("no such player")
            t.check_in(t.entrants[i]["pid"])
        elif action == "result":
            w = int(a["winner"])
            t.override(int(a["set"]), None if w < 0 else w)
        elif action == "noshow":
            t.set_noshow(int(a.get("minutes", 10)))
        elif action == "feature":
            t.feature(int(a["set"]), str(a.get("code", "")))
        else:
            raise tournament_mod.Error("unknown action")
        self.t_changed(t, now)
        return {"ok": True}


class Lobby(TourneyMixin):
    def __init__(self, sock, verbose, search_for=SEARCH_FOR, match_for=MATCH_FOR, ladder=None, arena_streak=3, tourney_admins=None):
        self.sock, self.verbose, self.ladder = sock, verbose, ladder
        self.tourneys = tournament_mod.Store(ladder.db) if ladder is not None and tournament_mod is not None else None
        self.icons_dir = ""           # (2026-11-03) the tournament pages' fighter icons (main: --icons-dir)
        self.tourney_admins = {x.lower() for x in tourney_admins} if tourney_admins else None   # (None: any account may organize)
        self.t_reports = {}       # match seed -> {"at", "tid", "sid", "by": {side: report}}
        self.t_counted = set()    # match seeds already counted (both games send their RESULT)
        self.lq = {}              # (2026-11-03) the ladder queue: client -> when it began waiting (monotonic)
        self.lq_last = {}         # account id hex -> its last ladder set's result (the game shows it a few minutes)
        self.lq_gone = {}         # (ladder match id, entrant) -> since when its game is gone (lq_guard)
        self.web_q = queue.Queue()   # (2026-11-02) the web page's organizer actions, done on the lobby's own thread
        self.web_tokens = {}      # token -> (account id hex, name, until): an organizer logged in on the web page
        self.arenas = {}          # 1.6: part (build, code) -> Arena
        self.arena_streak = arena_streak
        self.search_for, self.match_for = search_for, match_for
        self.by_at, self.by_ip, self.rooms = {}, {}, {}
        self.next_ip = SUBNET + 2
        self.stats = {"relayed game datagrams": 0, "ldn messages": 0, "dropped": 0}

    def say(self, *what):
        print(time.strftime("%H:%M:%S"), *what, flush=True)

    def log(self, *what):
        if self.verbose:
            self.say(*what)

    def send(self, data, at):
        try:
            self.sock.sendto(data, at)
        except OSError:
            pass

    # ---- who is where
    def members(self, room):
        out = [room.owner]
        for ip in room.nodes:
            c = self.by_ip.get(ip)
            if c is not None and c.claim == room.session:
                out.append(c)
        return out

    def room_of(self, c):
        if c.room is not None:
            return c.room
        r = self.rooms.get(c.claim) if c.claim else None
        return r if r is not None and c.ip in r.nodes else None

    def new_ip(self):
        for _ in range(0x10000):
            ip = self.next_ip
            self.next_ip = SUBNET + 2 if ip >= SUBNET + 0xFFFE else ip + 1
            if ip & 0xFF not in (0, 255) and ip not in self.by_ip:
                return ip
        return None

    def close_room(self, room, why):
        gone = hdr(DATA) + struct.pack(">IIHH", room.owner.ip, BROADCASTS[0], LDN_PORT, LDN_PORT) + bytes((ord("L"), M_GONE, 0, 0)) + room.session
        for m in self.members(room):
            if m is not room.owner:
                self.send(gone, m.at)
                m.claim = None
                self.send_peers(m)
        self.rooms.pop(room.session, None)
        if room.owner.room is room:
            room.owner.room = None
            self.send_peers(room.owner)
        self.say("room of %s closed (%s)" % (room.owner.name, why))

    def drop(self, c, why):
        self.arena_leave(c, why)
        self.lq_leave(c, why)
        if c.match is not None:
            other = c.match.other(c)
            c.match = None
            self.match_end(other, MS_PEER_LEFT, "%s left the lobby" % c.name)
        c.search_since = None
        if c.room is not None:
            self.close_room(c.room, "host left")
        if c.claim:
            r = self.rooms.get(c.claim)
            if r is not None and c.ip in r.nodes:   # tell its host at once
                self.send(hdr(DATA) + struct.pack(">IIHH", c.ip, r.owner.ip, LDN_PORT, LDN_PORT) + bytes((ord("L"), M_LEAVE, 0, 0)) + r.session, r.owner.at)
        self.by_at.pop(c.at, None)
        self.by_ip.pop(c.ip, None)
        self.say("%s (%s) left: %s" % (c.name, vip_text(c.ip), why))

    # ---- direct paths: each member of a room learns the others' addresses and a pair secret
    def send_peers(self, c):
        room = self.room_of(c)
        entries = []
        if room is not None and c.direct:
            for m in self.members(room):
                if m is c or not m.direct:
                    continue
                pair = frozenset((c.ip, m.ip))
                secret = room.secrets.setdefault(pair, secrets.randbits(64) | 1)
                pub_ip = struct.unpack(">I", socket.inet_aton(m.at[0]))[0]
                entries.append(struct.pack(">I32sIHIHQ", m.ip, m.name.encode()[:NAME_LEN], pub_ip, m.at[1], m.lan[0], m.lan[1], secret))
        body = b"".join(entries)
        if body == c.peers_sent:
            return
        c.peers_sent = body
        c.peers_version += 1
        self.send(hdr(PEERS) + struct.pack(">IB", c.peers_version, len(entries)) + body, c.at)
        if entries:
            self.log("%s introduced to %d player(s) of its room" % (c.name, len(entries)))

    def room_changed(self, room):
        for m in self.members(room):
            self.send_peers(m)

    # ---- packets
    def packet(self, p, at, now):
        if len(p) < 4 or p[:2] != b"SU" or p[2] != PROTO:
            return
        kind = p[3]
        c = self.by_at.get(at)
        if kind == JOIN:
            return self.on_join(p, at, c, now)
        if c is None:
            return
        c.seen = now
        if kind == PING and len(p) >= 21 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            same = [x for x in self.by_at.values() if x.part == c.part]
            rooms = [r for r in self.rooms.values() if r.owner.part == c.part]
            searching = sum(1 for x in same if x.search_since is not None)
            code = sum(1 for x in self.by_at.values() if x.part[1] == c.part[1])
            self.send(hdr(PONG) + p[12:20] + struct.pack(">HHHH", len(same), len(rooms), searching, code), at)   # (O2: the last two)
        elif kind == LEAVE and len(p) >= 12 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.drop(c, "left")
        elif kind == LIST and len(p) >= 16 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.on_list(c, p[12:16], now, len(p) >= 17 and p[16] & 1)
        elif kind == ROOMS and len(p) >= 16 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.on_rooms(c, p[12:16], now)
        elif kind == MATCH and len(p) >= 17 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.on_match(c, p[12], struct.unpack_from(">I", p, 13)[0], now,
                          struct.unpack_from(">Q", p, 17)[0] if len(p) >= 25 else 0)
        elif kind == RB and 16 < len(p) <= 1500 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            # online rollback message: to one player of the same lobby code (destination 0 = all others, only with a private code)
            # (1.4) on the public code only to its matched rollback opponent or a player of its Local Wireless room
            dst = struct.unpack_from(">I", p, 12)[0]
            if dst == 0 and c.part[1] == "public":
                return
            if dst and c.part[1] == "public" and dst != c.rb_peer:
                room = self.room_of(c)
                if room is None or dst not in [m.ip for m in self.members(room)]:
                    self.stats["dropped"] += 1
                    return
            out = hdr(RB) + struct.pack(">I", c.ip) + p[16:]
            for x in list(self.by_at.values()):
                if x is not c and x.part == c.part and (dst == 0 or x.ip == dst):
                    self.send(out, x.at)
            m = c.match
            if dst and m is not None and c.kind == KIND_ROLLBACK and m.other(c).ip == dst:
                m.rb_seen.add(c.ip)
                if len(m.rb_seen) == 2:
                    h, g = m.host, m.guest
                    self.arena_playing(h, g, now)
                    self.t_playing(h, g, now)
                    self.match_end(h, MS_TOGETHER, "playing a rollback match with %s" % g.name)
                    self.match_end(g, MS_TOGETHER, "playing a rollback match with %s" % h.name)
        elif kind == RB_GROUP and 20 <= len(p) <= 1500 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            # (1.7, 2026-11-02) a Battle Arena's spectators: the sender joins the group of the key (0 = leaves it) and the message
            # goes to every other player of the same build and lobby code in the same group (fighters <-> spectators)
            key = struct.unpack_from(">Q", p, 12)[0]
            if (key or None) != c.rb_group:
                self.say("arena group: %s %s" % (c.name, "joins the group of arena %u" % key if key else "leaves its arena group"))
            c.rb_group = key or None
            if key and len(p) > 20:
                out = hdr(RB_GROUP) + struct.pack(">I", c.ip) + p[20:]
                for x in list(self.by_at.values()):
                    if x is not c and x.part == c.part and x.rb_group == key:
                        self.send(out, x.at)
        elif kind == RESULT and len(p) >= RESULT_LEN and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            # (2026-11-03) ratings change only with a whole set of the ladder queue (or of a rated tournament: t_rate);
            # a casual rollback match (Smash, a Battle Arena) is never ranked. The ack ends the game's resending.
            self.arena_result(c, p, now)
            self.t_result(c, p, now)
            self.ladder_ack(c, struct.unpack_from(">Q", p, 12)[0], OUT_UNRANKED)
        elif kind == TOURNEY and len(p) >= 23 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.on_tourney(c, p, now)
        elif kind == ARENA and len(p) >= 13 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.on_arena(c, p[12], now)
        elif kind == DATA and len(p) >= 16:
            self.on_data(c, p, 4, now, ())
        elif kind == BCAST and len(p) >= 5:
            n = p[4]
            skip = set(struct.unpack_from(">%dI" % n, p, 5)) if len(p) >= 5 + 4 * n else set()
            body = 5 + 4 * n
            if len(p) >= body + 12:
                self.on_data(c, hdr(DATA) + p[body:], 4, now, skip)

    def on_join(self, p, at, c, now):
        if len(p) < JOIN_LEN:
            return
        nonce = p[4:12]
        code, name = text(p[12:44]), text(p[44:76]) or "Player"
        build = p[76:116].rstrip(b"\0")
        lan = struct.unpack_from(">IH", p, 116)
        direct = bool(p[122] & 1)
        part = (build, code)
        if c is not None and c.nonce != nonce:
            self.drop(c, "restarted")
            c = None
        if c is None:
            for other in list(self.by_at.values()):   # the same game from a new port (its router remapped it)?
                if other.nonce == nonce and other.part == part:
                    del self.by_at[other.at]
                    other.at = at
                    self.by_at[at] = c = other
                    break
        if c is None:
            ip = self.new_ip()
            if ip is None:
                self.send(hdr(REJECT) + nonce + b"the lobby is full".ljust(64, b"\0"), at)
                return
            c = Client(at, nonce, ip, name, part, lan, direct, now)
            self.by_at[at], self.by_ip[ip] = c, c
            self.say("%s joined (code %r, build %s) as %s from %s:%d, LAN %s:%d, direct %s" % (
                name, code, build.decode("ascii", "replace"), vip_text(ip), at[0], at[1], socket.inet_ntoa(struct.pack(">I", lan[0])), lan[1],
                "yes" if direct else "no"))
        c.seen, c.lan, c.direct = now, lan, direct
        ld = None
        if len(p) > JOIN_LEN + 2 and p[JOIN_LEN:JOIN_LEN + 2] == b"MS":      # O1: the game's mods (an older game sends none)
            c.mods = mods_block(p[JOIN_LEN + 2:])
            o = JOIN_LEN + 2 + len(c.mods) if c.mods is not None else len(p)
            if p[o:o + 3] == b"LD\1":                                       # 1.5: the game's ladder id
                ld = p[o + 3:]
        tail = self.ladder_join(c, nonce, ld) if ld is not None else b""
        self.send(hdr(WELCOME) + nonce + struct.pack(">IQ", c.ip, c.cookie) + tail, at)

    # ---- 1.5 ladder
    def ladder_join(self, c, nonce, ld):
        """The 'L''D' block of a JOIN: who c is on the ladder. Returns WELCOME's 'L''D' tail."""
        status, why = LD_UNRANKED, "no ladder on this lobby"
        if self.ladder is not None and len(ld) >= 16 + 32 + 1:
            pid, proof, flags = bytes(ld[:16]), bytes(ld[16:48]), ld[48]
            key = invite = None
            if flags & 1 and len(ld) >= 49 + 32 + 1:
                key, n = bytes(ld[49:81]), ld[81]
                invite = text(ld[82:82 + n]) or None
            got, why = self.ladder.identify(pid, c.name, nonce, proof, key, invite)
            if got is None and why == "unknown id":
                status = LD_UNKNOWN
            elif got is not None:
                status = LD_RANKED
                if c.ladder_id != got:
                    self.say("ladder: %s is ranked as %s" % (c.name, why))
            elif c.ladder_why != why:
                self.say("ladder: %s is not ranked: %s" % (c.name, why))
            c.ladder_id = got
        c.ladder_why = why
        return b"LD" + bytes((status,)) + self.ladder_numbers(c) + why.encode("utf-8", "replace")[:NAME_LEN].ljust(NAME_LEN, b"\0")

    def ladder_numbers(self, c):
        """rating u16, RD u16, wins u16, losses u16, place u16 of a ranked player (zeros otherwise)"""
        row = self.ladder.player(c.ladder_id) if self.ladder is not None and c.ladder_id is not None else None
        if row is None:
            return bytes(10)
        return struct.pack(">HHHHH", max(0, min(int(round(row[1])), 65535)), min(int(round(row[2])), 65535), min(row[4], 65535),
                           min(row[5], 65535), min(self.ladder.rank(c.ladder_id), 65535))

    def ladder_ack(self, c, seed, outcome, opp=None):
        """RESULT_ACK: seed u64, outcome u8, rating u16, RD u16, wins u16, losses u16, place u16, set score (mine u8, the other's u8)"""
        mine = theirs = 0
        if self.ladder is not None and c.ladder_id is not None and opp is not None and opp.ladder_id is not None:
            st = self.ladder.open_set_of(c.ladder_id, opp.ladder_id)
            if st is not None:
                mine, theirs = (st[3], st[4]) if st[1] == c.ladder_id else (st[4], st[3])
        self.send(hdr(RESULT_ACK) + struct.pack(">QB", seed, outcome) + self.ladder_numbers(c) + bytes((mine, theirs)), c.at)

    def on_list(self, c, seq, now, other=False):
        """The game's Rooms List. other (flag 1, a newer game): the rooms of every build of this code, each LISTING ending
        with its host's build[40] (that game refuses to join a room of another build and says why)."""
        def mine(r):
            return r.owner.part == c.part if not other else r.owner.part[1] == c.part[1]
        mhost = c.match.host if c.match is not None and c.match.guest is c else None     # O2: the matched host's room first
        rooms = sorted((r for r in self.rooms.values() if mine(r) and r.owner is not c and now - r.updated <= LISTED_FOR),
                       key=lambda r: (r.owner is not mhost, r.owner.part != c.part, not r.is_open(), -r.info[NI_NODE_COUNT], r.created))[:LIST_MAX]
        for r in rooms:
            self.send(hdr(LISTING) + seq + struct.pack(">I", r.owner.ip) + r.info + (r.owner.part[0][:BUILD_LEN].ljust(BUILD_LEN, b"\0") if other else b""), c.at)
        self.send(hdr(LIST_END) + seq + bytes((len(rooms),)), c.at)
        if rooms:
            self.log("%s scanned: %d room(s)" % (c.name, len(rooms)))

    def on_rooms(self, c, seq, now):
        """The room browser (F1 > Online rooms): every room of this lobby code, any build, joinable ones first."""
        rooms = sorted((r for r in self.rooms.values() if r.owner.part[1] == c.part[1] and now - r.updated <= LISTED_FOR),
                       key=lambda r: (r.owner.part != c.part, not r.is_open(), -r.info[NI_NODE_COUNT], r.created))[:ROOMS_MAX]
        for r in rooms:
            o = r.owner
            flags = (1 if r.is_open() else 0) | (2 if o is c else 0) | (4 if o.mods is not None else 0)
            self.send(hdr(ROOM_INFO) + seq + struct.pack(">IBBB", o.ip, r.info[NI_NODE_COUNT], r.info[NI_NODE_MAX], flags)
                      + o.name.encode("utf-8", "replace")[:NAME_LEN].ljust(NAME_LEN, b"\0") + o.part[0][:BUILD_LEN].ljust(BUILD_LEN, b"\0")
                      + (o.mods or b"\0"), c.at)
        self.send(hdr(ROOMS_END) + seq + bytes((len(rooms),)), c.at)

    def on_data(self, c, p, off, now, skip):
        if len(p) - off - 12 > MAX_DATA + NI_SIZE:
            self.stats["dropped"] += 1
            return
        src, dst, sport, dport = struct.unpack_from(">IIHH", p, off)
        if src != c.ip:
            self.stats["dropped"] += 1
            return
        if sport == LDN_PORT or dport == LDN_PORT:
            return self.on_ldn(c, p, dst, sport, dport, now)
        room = self.room_of(c)
        if room is None:
            self.stats["dropped"] += 1
            return
        members = self.members(room)
        sent = 0
        for m in members:
            if m is c or m.ip in skip:
                continue
            if dst in BROADCASTS or m.ip == dst:
                self.send(p, m.at)
                sent += 1
        self.stats["relayed game datagrams"] += sent
        if not sent and dst not in BROADCASTS:
            self.stats["dropped"] += 1

    def on_ldn(self, c, p, dst, sport, dport, now):
        if sport != LDN_PORT or dport != LDN_PORT or len(p) < 20 or p[16] != ord("L"):
            self.stats["dropped"] += 1
            return
        self.stats["ldn messages"] += 1
        kind, body = p[17], p[20:]
        if dst in BROADCASTS:
            if kind == M_ADVERTISE and len(body) >= NI_SIZE:
                self.on_advertise(c, p, bytes(body[:NI_SIZE]), now)
            elif kind == M_GONE and c.room is not None and body[:16] == c.room.session:
                self.close_room(c.room, "closed by its host")
            return
        b = self.by_ip.get(dst)
        if b is None or b is c or b.part != c.part:
            self.stats["dropped"] += 1
            return
        if kind == M_JOIN:
            if b.room is None or body[:16] != b.room.session:
                self.stats["dropped"] += 1
                return
            if c.room is not None:
                self.close_room(c.room, "its host joins another room")
            c.claim = b.room.session
            self.say("%s asks to join %s's room" % (c.name, b.name))
        elif kind == M_LEAVE:
            if c.claim and b.room is not None and b.room.session == c.claim:
                c.claim = None
                self.send(p, b.at)
                self.send_peers(c)
                return
        self.send(p, b.at)

    def on_advertise(self, c, p, info, now):
        nodes = room_nodes(info, c.ip)
        if nodes is None:
            self.stats["dropped"] += 1
            return
        session = info[NI_SESSION:NI_SESSION + 16]
        room = self.rooms.get(session)
        if room is not None and room.owner is not c:
            self.stats["dropped"] += 1
            return
        if room is None:
            if c.room is not None:
                self.close_room(c.room, "replaced by a new room")
            c.claim = None
            room = c.room = self.rooms[session] = Room(c, session, now)
            self.say("%s opened a room (%d players at most)" % (c.name, info[NI_NODE_MAX]))
        before = self.members(room)
        changed = nodes != room.nodes
        room.info, room.nodes, room.updated = info, nodes, now
        after = self.members(room)
        if changed:
            names = ", ".join(node_name(info, i) or "?" for i in range(8) if info[NI_NODES + NODE_LEN * i + 0xB])
            self.say("room of %s: %d player(s): %s" % (c.name, info[NI_NODE_COUNT], names))
            self.room_changed(room)
            for m in before:
                if m not in after:
                    self.send_peers(m)
        for m in set(before) | set(after):
            if m is not c:
                self.send(p, m.at)
        if c.match is not None and c.match.host is c and c.kind == KIND_ROOM:
            self.match_room(c.match, room, now)

    # ---- O2 matchmaking
    def match_send(self, c, now, state=None):
        """MATCH_STATE to c: search id u32, state u8, role u8, flags u8 (1: the host's room is open), peer address u32,
        peer name[32], players searching u16, players u16 (both of c's build and code), seconds waited u16."""
        m, role, flags, peer, name, sid = c.match, 0, 0, 0, b"", c.search_id or 0
        if state is None and m is None and c.search_since is None and c.final is not None:
            sid, state, role, peer, name = c.final
        elif state is None:
            state = MS_MATCHED if m is not None else MS_SEARCHING if c.search_since is not None else MS_IDLE
        if m is not None:
            o = m.other(c)
            role, peer, name = (ROLE_HOST if m.host is c else ROLE_JOIN), o.ip, o.name.encode("utf-8", "replace")
            r = m.host.room
            if r is not None and r.is_open() and now - r.updated <= LISTED_FOR:
                flags |= 1
        since = m.at if m is not None else c.search_since
        same = [x for x in self.by_at.values() if x.part == c.part]
        out = (hdr(MATCH_STATE) + struct.pack(">IBBBI", sid & 0xFFFFFFFF, state, role, flags, peer) + name[:NAME_LEN].ljust(NAME_LEN, b"\0")
               + struct.pack(">HHH", sum(1 for x in same if x.search_since is not None), len(same),
                             min(int(now - since), 65535) if since is not None else 0))
        if m is not None and c.kind == KIND_ROLLBACK and c.direct and o.direct:
            # (2026-11-03) a rollback match goes player to player, as in the rooms of net_minilobby.c: 'A' + the address the
            # opponent is reached at (u32, u16): where this lobby sees it, or its LAN address when both come from the same
            # public address (the same house). Without it every message of the match went through this server (a friend
            # and the user, both in Mexico: ~156 ms round trip through the VM instead of the direct path).
            if o.at[0] == c.at[0] and o.lan[0]:
                a_ip, a_port = o.lan
            else:
                a_ip, a_port = struct.unpack(">I", socket.inet_aton(o.at[0]))[0], o.at[1]
            out += b"A" + struct.pack(">IH", a_ip, a_port)
        self.send(out, c.at)

    def match_end(self, c, state, why):
        """c's search or match is over (state: MS_TIMEOUT, MS_PEER_LEFT, MS_TOGETHER, MS_EXPIRED)."""
        m = c.match
        role, peer, name = 0, 0, b""
        if m is not None:
            o = m.other(c)
            role, peer, name = (ROLE_HOST if m.host is c else ROLE_JOIN), o.ip, o.name.encode("utf-8", "replace")
        c.match, c.search_since = None, None
        c.final = (c.search_id or 0, state, role, peer, name)
        self.match_send(c, time.monotonic())
        self.say("match: %s: %s" % (c.name, why))

    def on_match(self, c, op, sid, now, key=0):
        """MATCH from a game: op 1 = search (sent again every 2 s while it waits: the answer says how it stands), 0 = cancel,
        2 = (1.4) search for a rollback match (paired only with another op 2 search; no room: the one that waited longer is
        player 1, ROLE_HOST)."""
        if op == 0:
            if c.match is not None:
                other = c.match.other(c)
                c.match = None
                self.match_end(other, MS_PEER_LEFT, "%s cancelled the match" % c.name)
            if c.search_since is not None:
                self.say("match: %s stopped searching" % c.name)
            c.search_since, c.search_id, c.final = None, sid, (sid, MS_IDLE, 0, 0, b"")
            return self.match_send(c, now)
        if op not in (KIND_ROOM, KIND_ROLLBACK):
            return
        if c.search_id == sid and (c.match is not None or c.search_since is not None or c.final is not None):
            return self.match_send(c, now)          # the same search: how it stands (or how it ended)
        if c.match is not None:                     # a new search while matched: the old match is over
            other = c.match.other(c)
            c.match = None
            self.match_end(other, MS_PEER_LEFT, "%s searches again" % c.name)
        c.search_id, c.search_since, c.final, c.kind = sid, now, None, op
        c.match_key = key                           # (2026-11-02) a Battle Arena's match: paired only with the same key
        self.say("match: %s searches an opponent%s (build %s)" % (c.name, " for a rollback match" if op == KIND_ROLLBACK else "", c.part[0].decode("ascii", "replace")))
        waiting = [x for x in self.by_at.values() if x is not c and x.part == c.part and x.search_since is not None and x.match is None and x.kind == op
                   and getattr(x, "match_key", 0) == key]   # (a key: the game's own Online menu, one Battle Arena: its two players only)
        a = self.arenas.get(c.part)
        pair = a.pair if a is not None and op == KIND_ROLLBACK else None
        tp = self.t_pairing(c) if op == KIND_ROLLBACK and not key else None      # (a Battle Arena's match: the arena pairs it; a featured set's result still counts)
        busy = self.t_busy(c.part)
        if tp is not None:                          # 1.7 tournament: only the opponent of its set, once both are ready
            waiting = [x for x in waiting if x is tp[0]] if tp[0] is not None else []
            pair = [tp[0], c] if tp[1] == 1 else [c, tp[0]]
        elif pair and c in pair:                      # 1.6 arena: a player taking its turn meets only its opponent of the turn
            waiting = [x for x in waiting if x is (pair[1] if c is pair[0] else pair[0])]
        elif a is not None and c in a.members():   # (in the queue: its turn has not come; the box says it searches)
            waiting = []
        elif a is not None:                         # (a player outside the arena never takes one of the arena's)
            waiting = [x for x in waiting if x not in a.members()]
        if tp is None:                              # (nor one of a tournament's)
            waiting = [x for x in waiting if x not in busy]
        if not waiting:
            return self.match_send(c, now)
        o = min(waiting, key=lambda x: x.search_since)

        def hosts_open(x):
            return op == KIND_ROOM and x.room is not None and x.room.is_open()
        host = c if hosts_open(c) and not hosts_open(o) else o   # the one with an open room hosts, else the one that waited longer
        if pair and c in pair:
            host = pair[0]                          # (the arena: the one ahead in the queue, or the winner who stays, is player 1)
        m = Match(host, o if host is c else c, now)
        for x in (c, o):
            x.match, x.search_since = m, None
        if op == KIND_ROLLBACK:
            c.rb_peer, o.rb_peer = o.ip, c.ip
        self.say("match: %s hosts, %s joins%s" % (m.host.name, m.guest.name, " (rollback match)" if op == KIND_ROLLBACK else ""))
        self.match_send(m.host, now)
        self.match_send(m.guest, now)
        if m.host.room is not None and op == KIND_ROOM:
            self.match_room(m, m.host.room, now)

    def match_room(self, m, room, now):
        """(the host advertised its room) both in it: done; the room open: the guest is told once (it polls as well)."""
        if m.guest.ip in room.nodes and m.guest.claim == room.session:
            h, g = m.host, m.guest
            self.match_end(h, MS_TOGETHER, "in a room with %s" % g.name)
            self.match_end(g, MS_TOGETHER, "in %s's room" % h.name)
            return
        if room.is_open() and not m.room_said:
            m.room_said = True
            self.match_send(m.guest, now)
            self.match_send(m.host, now)

    def sweep(self, now):
        for c in list(self.by_at.values()):
            if now - c.seen > CLIENT_TIMEOUT:
                self.drop(c, "silent for %d s" % CLIENT_TIMEOUT)
        for c in list(self.by_at.values()):
            if c.search_since is not None and c.match is None and now - c.search_since > self.search_for:
                self.match_end(c, MS_TIMEOUT, "no opponent in %d s" % self.search_for)
            m = c.match
            if m is not None and m.host is c and now - m.at > self.match_for:
                g = m.guest
                self.match_end(c, MS_EXPIRED, "the match with %s expired" % g.name)
                self.match_end(g, MS_EXPIRED, "the match with %s expired" % c.name)
        for r in list(self.rooms.values()):
            if now - r.updated > ROOM_TIMEOUT:
                self.close_room(r, "no advertisement for %d s" % ROOM_TIMEOUT)
        self.t_sweep(now)
        self.lq_sweep(now)
        for a in list(self.arenas.values()):
            if a.pair and not a.playing and now - a.at > ARENA_TURN_FOR:
                # (a turn not started in time: whoever did not even search is away and leaves the arena; the other keeps its place)
                away = [x for x in a.pair if x.search_since is None and x.match is None]
                if not away or len(away) == 2:
                    for x in away:
                        self.arena_leave(x, "did not start its turn in %d s" % ARENA_TURN_FOR, now)
                    if a.pair:
                        self.arena_end(a, None, "the turn of %s and %s was not started in %d s" % (a.pair[0].name, a.pair[1].name, ARENA_TURN_FOR), now)
                else:
                    self.arena_leave(away[0], "did not start its turn in %d s" % ARENA_TURN_FOR, now)
            elif a.pair and a.playing and now - a.at > ARENA_PLAY_FOR:
                self.arena_end(a, None, "no result of the match of %s and %s in %d s" % (a.pair[0].name, a.pair[1].name, ARENA_PLAY_FOR), now)

    # ---- 1.6 arena
    def arena_status(self, a, c):
        if a is not None and a.pair and c in a.pair:
            return AR_PLAYING if a.playing else AR_TURN
        return AR_WAITING if a is not None and c in a.queue else AR_OUT

    def arena_send(self, c, now):
        """ARENA_STATE to c (see the top of this file)."""
        a = self.arenas.get(c.part)
        st = self.arena_status(a, c)
        place, opp, since = 0, b"", now
        if st == AR_WAITING:
            place = a.queue.index(c) + 1
        elif st in (AR_TURN, AR_PLAYING):
            opp = (a.pair[1] if c is a.pair[0] else a.pair[0]).name.encode("utf-8", "replace")
            since = a.at

        def nm(x):
            return x.name.encode("utf-8", "replace")[:NAME_LEN].ljust(NAME_LEN, b"\0")
        out = hdr(ARENA_STATE) + struct.pack(">IBBBB", a.version if a else 0, st, min(place, 255), min(c.arena_streak, 255), min(self.arena_streak, 255))
        out += opp[:NAME_LEN].ljust(NAME_LEN, b"\0") + struct.pack(">H", max(0, min(int(now - since), 65535)))
        if a is not None and a.pair:
            out += bytes((1,)) + nm(a.pair[0]) + nm(a.pair[1]) + bytes((min(a.pair[0].arena_streak, 255), min(a.pair[1].arena_streak, 255),
                                                                     AR_PLAYING if a.playing else AR_TURN))
        else:
            out += bytes((0,))
        q = a.queue[:ARENA_LIST] if a is not None else []
        out += bytes((len(q),)) + b"".join(nm(x) + bytes((min(x.arena_streak, 255),)) for x in q)
        self.send(out, c.at)

    def arena_changed(self, a, now):
        a.version += 1
        for x in self.by_at.values():               # its players, and whoever looks at it
            if self.arenas.get(x.part) is a and (x in a.members() or (x.arena_seen is not None and now - x.arena_seen <= ARENA_WATCH_FOR)):
                self.arena_send(x, now)

    def on_arena(self, c, op, now):
        """ARENA from a game: op 0 leave, 1 join (or stay in), 2 only look."""
        a = self.arenas.get(c.part)
        if op == 1 and (a is None or c not in a.members()):
            if a is None:
                a = self.arenas[c.part] = Arena()
            a.queue.append(c)
            c.arena_streak = 0
            self.say("arena: %s joined the queue (%d waiting)" % (c.name, len(a.queue)))
            self.arena_next(a, now)
            self.arena_changed(a, now)
        elif op == 0:
            self.arena_leave(c, "left the arena", now)
        c.arena_seen = now
        self.arena_send(c, now)

    def arena_leave(self, c, why, now=None):
        a = self.arenas.get(c.part)
        if a is None or c not in a.members():
            return
        now = time.monotonic() if now is None else now
        if a.pair and c in a.pair:
            other = a.pair[1] if c is a.pair[0] else a.pair[0]
            a.pair, a.playing = None, False
            a.queue.insert(0, other)                # (the other player keeps its place at the front, and its streak)
            self.say("arena: %s left its turn (%s): %s is next again" % (c.name, why, other.name))
        else:
            a.queue.remove(c)
            self.say("arena: %s left the queue (%s)" % (c.name, why))
        c.arena_streak = 0
        self.arena_next(a, now)
        self.arena_changed(a, now)
        self.arena_send(c, now)

    def arena_next(self, a, now):
        """No match on: the first two of the queue take their turn."""
        if a.pair is None and len(a.queue) >= 2:
            a.pair, a.playing, a.at = [a.queue.pop(0), a.queue.pop(0)], False, now
            self.say("arena: %s's turn against %s (%d waiting)" % (a.pair[0].name, a.pair[1].name, len(a.queue)))

    def arena_playing(self, h, g, now):
        a = self.arenas.get(h.part)
        if a is not None and a.pair and h in a.pair and g in a.pair and not a.playing:
            a.playing, a.at = True, now
            self.say("arena: %s and %s are playing" % (h.name, g.name))
            self.arena_changed(a, now)

    def arena_end(self, a, winner, why, now):
        """The pair's match is over: the winner (None: no winner) stays, unless at the streak limit."""
        p1, p2 = a.pair
        a.pair, a.playing = None, False
        if winner is None:
            for x in (p1, p2):
                x.arena_streak = 0
                a.queue.append(x)
            self.say("arena: %s: %s and %s go to the back of the queue" % (why, p1.name, p2.name))
        else:
            loser = p2 if winner is p1 else p1
            loser.arena_streak = 0
            a.queue.append(loser)
            winner.arena_streak += 1
            if self.arena_streak and winner.arena_streak >= self.arena_streak:
                self.say("arena: %s beat %s: %d wins in a row, the limit: both to the back" % (winner.name, loser.name, winner.arena_streak))
                winner.arena_streak = 0
                a.queue.append(winner)
            else:
                self.say("arena: %s beat %s (%d in a row) and stays" % (winner.name, loser.name, winner.arena_streak))
                a.queue.insert(0, winner)
        self.arena_next(a, now)
        self.arena_changed(a, now)

    def arena_result(self, c, p, now):
        """RESULT of a game of the pair: who won (stocks left, then damage). Both games send it: the first one counts."""
        a = self.arenas.get(c.part)
        if a is None or not a.pair or c not in a.pair:
            return
        seed, peer, slot, rule_kind, stock, flags = struct.unpack_from(">QIBBBB", p, 12)
        s0, s1, d0, d1 = struct.unpack_from(">BBHH", p, 48)
        other = a.pair[1] if c is a.pair[0] else a.pair[0]
        if seed == a.last_seed or peer != other.ip or slot > 1:
            return
        a.last_seed = seed
        win = 0 if s0 > s1 else 1 if s1 > s0 else 0 if d0 < d1 else 1 if d1 < d0 else None
        if flags & 2 or win is None:                 # (a disconnection, or a draw: no winner)
            return self.arena_end(a, None, "no winner (%s)" % ("disconnected" if flags & 2 else "a draw"), now)
        self.arena_end(a, c if win == slot else other, "result", now)


def status_json(lobby, started, now):
    """The /status document: counts only. Never addresses, names, lobby codes or builds."""
    return json.dumps({"up": True, "players": len(lobby.by_at), "rooms": len(lobby.rooms),
                       "searching": sum(1 for c in lobby.by_at.values() if c.search_since is not None),
                       "version": "%s (protocol SU %d)" % (LOBBY_VERSION, PROTO), "uptime_s": int(now - started)})


LADDER_PAGE = """<!doctype html>
<html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>SSBU ladder</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; --line: #ddddd6; --card: #ffffff; --accent: #c2410c; --win: #15803d; --loss: #b91c1c; }
@media (prefers-color-scheme: dark) { :root { --bg: #161615; --fg: #ecece8; --muted: #9a9a93; --line: #2e2e2b; --card: #1f1f1d; --accent: #fb923c; --win: #4ade80; --loss: #f87171; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.45 system-ui, sans-serif; }
main { max-width: 860px; margin: 0 auto; padding: 24px 16px; }
h1 { font-size: 22px; margin: 0 0 4px; } h2 { font-size: 17px; margin: 26px 0 8px; } p.sub { color: var(--muted); margin: 0 0 18px; }
a { color: var(--accent); }
.wrap { overflow-x: auto; } table { width: 100%; border-collapse: collapse; } th, td { padding: 7px 6px; border-bottom: 1px solid var(--line); text-align: left; }
th { color: var(--muted); font-weight: 600; font-size: 13px; } td.n, th.n { text-align: right; font-variant-numeric: tabular-nums; }
tr.p { cursor: pointer; } tr.p:hover td { background: color-mix(in srgb, var(--accent) 8%, transparent); }
.rd { color: var(--muted); font-size: 12px; } .w { color: var(--win); } .l { color: var(--loss); }
.fi { display: inline-flex; align-items: center; justify-content: center; width: 20px; height: 20px; border-radius: 50%; color: #fff;
      font-size: 9px; font-weight: 700; vertical-align: middle; }
img.fi { background: transparent; border-radius: 0; object-fit: contain; }
.fis { display: inline-flex; gap: 3px; margin-left: 7px; vertical-align: middle; }
.how { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: 10px 14px; margin: 0 0 8px; }
.how ol { margin: 6px 0; padding-left: 20px; }
</style></head><body><main>
<h1>Ladder</h1><p class="sub">Rating Glicko-2. Cada set (mejor de 3, 3 stocks) suma puntos al ganador y le quita al perdedor.
Ordenado por rating menos dos veces la desviación (quien jugó poco sube despacio).</p>
<div class="how"><b>Cómo jugar</b><ol>
<li>En el juego presiona <b>F3</b> (o mantén <b>&minus;</b> un segundo) y elige <b>Ladder: buscar rival</b>.</li>
<li>Cuando alguien más busca, el servidor los junta (el rating más parecido primero). La pantalla se abre sola.</li>
<li>Tachen escenarios, vayan a <b>Smash</b>, elijan personaje y presionen <b>start</b>: la pelea empieza sola.</li>
<li>Al terminar el set cambia el rating de los dos.</li></ol></div>
<h2>Tabla</h2>
<div class="wrap"><table><thead><tr><th>#</th><th>Jugador</th><th class="n">Rating</th><th class="n">Sets G-P</th></tr></thead><tbody id="t"></tbody></table></div>
<section id="hist"></section>
<h2>Últimos sets</h2>
<div class="wrap"><table><thead><tr><th>Set</th><th class="n">Marcador</th><th class="n">Rating</th></tr></thead><tbody id="ms"></tbody></table></div>
</main><script>
const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
let ICONS = new Set();
function fi(c) {                                     // c: {key, name}
  if (!c || !c.name) return "";
  if (c.key && ICONS.has(c.key)) return "<img class=fi src='icons/" + c.key + ".png' alt='" + esc(c.name) + "' title='" + esc(c.name) + "'>";
  const w = c.name.replace(/[^A-Za-z0-9 ]/g, " ").trim().split(/\\s+/).filter(Boolean);
  const ini = w.length > 1 ? w[0][0] + w[1][0] : (w[0] || "?").slice(0, 2);
  let h = 0; for (const ch of c.name) h = (h * 31 + ch.charCodeAt(0)) % 360;
  return "<span class=fi style='background:hsl(" + h + " 55% 42%)' title='" + esc(c.name) + "'>" + esc(ini) + "</span>";
}
const when = t => t ? new Date(t * 1000).toLocaleString("es", {day: "numeric", month: "short", hour: "2-digit", minute: "2-digit"}) : "";
const delta = r => { const d = r[1] - r[0]; return "<span class=" + (d >= 0 ? "w" : "l") + ">" + (d >= 0 ? "+" : "") + d + "</span>"; };
async function show(name) {
  const r = await fetch("player/" + encodeURIComponent(name) + ".json"); if (!r.ok) return;
  const h = await r.json(), el = document.getElementById("hist");
  el.innerHTML = "<h2>" + esc(h.name) + "</h2><div class=wrap><table><thead><tr><th>Rival</th><th>Resultado</th><th class=n>Marcador</th><th class=n>Rating</th></tr></thead><tbody>" +
    (h.sets.map(s => "<tr><td>" + (s.tid ? "<a href='tournament/" + s.tid + "'>" + esc(s.opponent) + "</a>" : esc(s.opponent)) + "</td><td class=" +
      (s.won === true ? "w" : s.won === false ? "l" : "") + ">" + (s.state === "done" ? (s.won ? "ganó" : "perdió") : esc(s.state)) + "</td><td class=n>" +
      s.score.join(" - ") + "</td><td class=n>" + (s.rating ? s.rating[0] + " &rarr; " + s.rating[1] + " (" + delta(s.rating) + ")" : "") + "</td></tr>").join("") ||
     "<tr><td colspan=4 class=rd>Todavía no jugó sets de ladder.</td></tr>") + "</tbody></table></div>";
  el.scrollIntoView({behavior: "smooth"});
}
(async () => {
  try { ICONS = new Set(await (await fetch("icons.json")).json()); } catch (e) {}
  const rows = await (await fetch("ladder.json")).json();
  document.getElementById("t").innerHTML = rows.map((p, i) => "<tr class=p data-n=\\"" + esc(p.name) + "\\"><td>" + (i + 1) + "</td><td>" + esc(p.name) +
    "<span class=fis>" + (p.fighters || []).map(fi).join("") + "</span></td><td class=n>" + p.rating + " <span class=rd>&plusmn;" + p.rd + "</span></td><td class=n>" +
    p.wins + " - " + p.losses + "</td></tr>").join("") || "<tr><td colspan=4 class=rd>Nadie todavía.</td></tr>";
  document.querySelectorAll("tr.p").forEach(tr => tr.onclick = () => show(tr.dataset.n));
  const ms = await (await fetch("ladder_matches.json")).json();
  document.getElementById("ms").innerHTML = ms.map(m => {
    const w = m.winner === 1 ? 0 : 1;
    return "<tr><td><a href='tournament/" + m.tid + "'><span class=" + (w === 0 ? "w" : "") + ">" + esc(m.players[0]) + "</span> vs <span class=" + (w === 1 ? "w" : "") + ">" +
      esc(m.players[1]) + "</span></a> <span class=rd>" + when(m.closed) + "</span></td><td class=n>" + m.score.join(" - ") + "</td><td class=n>" +
      (m.rating ? delta(m.rating[0]) + " / " + delta(m.rating[1]) : "") + "</td></tr>";
  }).join("") || "<tr><td colspan=3 class=rd>Todavía no hay sets.</td></tr>";
})();
</script></body></html>
"""


def tourney_list_json(path):
    """/tournaments.json: the latest tournaments (names and states only; no ids of players)"""
    db = sqlite3.connect(path)
    try:
        return [{"id": tid, "name": name, "state": state} for tid, name, state in
                db.execute("SELECT id, name, state FROM tournaments WHERE data NOT LIKE '%\"kind\": \"ladder\"%' ORDER BY id DESC LIMIT 30")]
    except sqlite3.Error:
        return []
    finally:
        db.close()


def game_charas(g):
    """a recorded game's two fighters (ui_chara ids by side; None when not known)"""
    c = g.get("chara")
    return (list(c) + [None, None])[:2] if isinstance(c, (list, tuple)) else [None, None]


# (2026-11-03) fighter icons for the tournament pages: PNG files the server's admin puts in the icons folder
# (--icons-dir; default: "icons" beside the ladder's database), named by the fighter's internal name (mario.png,
# captain.png, ...: tournament.FIGHTERS). Nothing of the game ships with this file; a missing icon shows the initials.
ICON_NAME_RE = re.compile(r"^[a-z0-9_]{1,24}$")


def icon_list(folder):
    try:
        return sorted(f[:-4] for f in os.listdir(folder) if f.endswith(".png") and ICON_NAME_RE.match(f[:-4]))
    except OSError:
        return []


def tourney_json(path, tid):
    """/tournament/<id>.json: the bracket for the web page (names, never account ids)"""
    db = sqlite3.connect(path)
    try:
        row = db.execute("SELECT data FROM tournaments WHERE id = ?", (tid,)).fetchone()
    except sqlite3.Error:
        row = None
    finally:
        db.close()
    if row is None:
        return None
    t = tournament_mod.Tournament.from_dict(json.loads(row[0]))
    names = [e["name"] for e in t.entrants]
    sets = []
    for i, st in enumerate(t.sets):
        v = t.view[i]
        r = t.results.get(str(i)) or {}
        sets.append({"id": i, "bracket": st["br"], "round": st["round"], "state": v["state"], "bo": v["bo"], "score": t.score(i),
                     "by": r.get("by", ""), "featured": t.featured(i),
                     "players": [None if x is None else "(bye)" if x == tournament_mod.BYE else names[x] for x in v["p"]],
                     "winner": None if v["winner"] in (None, tournament_mod.BYE) else names[v["winner"]],
                     # (2026-11-03) game by game: the stage and the two fighters (by side), for the set's window on the page
                     "games": [{"winner": g.get("winner"),
                                "stage": tournament_mod.STAGE_NAMES.get(g.get("stage") or "", g.get("stage") or ""),
                                "chars": [tournament_mod.chara_name(x) if isinstance(x, int) else "" for x in game_charas(g)],
                                "keys": [tournament_mod.chara_key(x) if isinstance(x, int) else "" for x in game_charas(g)]}
                               for g in r.get("games", [])]})
    return {"id": t.id, "name": t.name, "state": t.state, "organizer": t.rules.get("organizer_name", ""), "kind": t.rules.get("kind", ""),
            "seed_mode": t.seed_mode,
            "rules": {k: t.rules.get(k) for k in ("bo", "bo_top8", "bo_finals", "bans", "stocks", "checkin", "rated", "noshow", "turn_for")},
            "stages": {"starters": [tournament_mod.STAGE_NAMES.get(x, x) for x in t.rules["starters"]],
                       "counterpicks": [tournament_mod.STAGE_NAMES.get(x, x) for x in t.rules["counterpicks"]]},
            "entrants": [{"seed": i + 1, "name": e["name"], "dq": e["dq"], "in": bool(e.get("in")),
                          "rating": None if e["rating"] is None else int(e["rating"])} for i, e in enumerate(t.entrants)],
            "sets": sets, "standings": [{"place": pl, "name": names[e]} for pl, e in t.standings()] if t.sets else []}


TOURNEY_PAGE = """<!doctype html>
<html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>SSBU torneos</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; --line: #ddddd6; --card: #ffffff; --accent: #c2410c; --win: #15803d; --bad: #b91c1c; }
@media (prefers-color-scheme: dark) { :root { --bg: #161615; --fg: #ecece8; --muted: #9a9a93; --line: #2e2e2b; --card: #1f1f1d; --accent: #fb923c; --win: #4ade80; --bad: #f87171; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.45 system-ui, sans-serif; }
main { max-width: 1200px; margin: 0 auto; padding: 24px 16px; }
h1 { font-size: 22px; margin: 0 0 4px; } h2 { font-size: 17px; margin: 22px 0 8px; } p.sub { color: var(--muted); margin: 0 0 16px; }
a { color: var(--accent); } .row { display: flex; gap: 14px; overflow-x: auto; padding-bottom: 8px; }
.col { min-width: 190px; display: flex; flex-direction: column; justify-content: space-around; gap: 10px; }
.col h3 { font-size: 12px; color: var(--muted); margin: 0; font-weight: 600; text-transform: uppercase; letter-spacing: .04em; }
.set { background: var(--card); border: 1px solid var(--line); border-radius: 8px; overflow: hidden; }
.set div.p { display: flex; justify-content: space-between; padding: 5px 9px; gap: 8px; } .set div.p + div.p { border-top: 1px solid var(--line); }
.set .w { color: var(--win); font-weight: 600; } .set .tbd { color: var(--muted); } .set.live { border-color: var(--accent); }
.set .org { display: flex; gap: 4px; padding: 4px 6px; border-top: 1px dashed var(--line); flex-wrap: wrap; }
.n { font-variant-numeric: tabular-nums; } ol { padding-left: 22px; } li.dq { color: var(--muted); text-decoration: line-through; }
button, select, input { font: inherit; color: inherit; background: var(--card); border: 1px solid var(--line); border-radius: 6px; padding: 4px 9px; }
button { cursor: pointer; } button:hover { border-color: var(--accent); } button.main { background: var(--accent); color: #fff; border-color: var(--accent); }
button.small { font-size: 12px; padding: 2px 6px; }
.panel { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: 12px 14px; margin: 14px 0; }
.panel h2 { margin-top: 0; } .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(170px, 1fr)); gap: 8px 14px; }
.grid label { display: flex; flex-direction: column; gap: 3px; font-size: 13px; color: var(--muted); }
.msg { margin: 8px 0 0; } .msg.err { color: var(--bad); } .msg.ok { color: var(--win); }
table.ent { border-collapse: collapse; width: 100%; } table.ent td, table.ent th { padding: 5px 6px; border-bottom: 1px solid var(--line); text-align: left; }
table.ent th { font-size: 12px; color: var(--muted); } .tag { font-size: 12px; padding: 1px 6px; border-radius: 999px; border: 1px solid var(--line); }
.tag.in { color: var(--win); border-color: var(--win); } .tag.out { color: var(--muted); }
.set.open { cursor: pointer; } .set.open:hover { border-color: var(--accent); }
.set .more { font-size: 11px; color: var(--muted); padding: 2px 9px 4px; border-top: 1px solid var(--line); }
dialog#gd { border: 1px solid var(--line); border-radius: 12px; background: var(--card); color: var(--fg); padding: 0; width: min(560px, calc(100vw - 32px)); }
dialog#gd::backdrop { background: rgba(0, 0, 0, .55); }
#gd .hd { display: flex; justify-content: space-between; align-items: flex-start; gap: 10px; padding: 14px 16px 10px; border-bottom: 1px solid var(--line); }
#gd .hd h3 { margin: 0; font-size: 17px; } #gd .hd p { margin: 2px 0 0; color: var(--muted); font-size: 13px; }
#gd .bd { padding: 10px 16px 16px; max-height: 70vh; overflow-y: auto; }
#gd .g { display: grid; grid-template-columns: 56px 1fr; gap: 4px 12px; padding: 10px 0; border-bottom: 1px solid var(--line); }
#gd .g:last-child { border-bottom: 0; } #gd .gn { font-size: 12px; color: var(--muted); font-weight: 600; text-transform: uppercase; }
#gd .stage { font-weight: 600; } #gd .fx { display: flex; justify-content: space-between; gap: 8px; }
#gd .fx span { flex: 1; } #gd .fx span:last-child { text-align: right; } #gd .fx .w { color: var(--win); font-weight: 600; }
#gd .fx small { display: block; color: var(--muted); font-weight: 400; } #gd .fx .w small:after { content: " \\2713"; color: var(--win); }
#gd .none { color: var(--muted); }
.fi { display: inline-flex; align-items: center; justify-content: center; flex: none; width: 22px; height: 22px; border-radius: 50%;
      color: #fff; font-size: 10px; font-weight: 700; letter-spacing: -.02em; margin-right: 6px; vertical-align: middle;
      box-shadow: inset 0 0 0 1px rgba(255, 255, 255, .25); }
.set div.p > span:first-child { display: inline-flex; align-items: center; min-width: 0; }
#gd .fx > span { display: flex; align-items: center; gap: 0; } #gd .fx > span:last-child { flex-direction: row-reverse; text-align: right; }
#gd .fx > span:last-child .fi { margin: 0 0 0 6px; } #gd .fx .fi { width: 30px; height: 30px; font-size: 12px; }
img.fi { background: transparent; box-shadow: none; border-radius: 0; object-fit: contain; }
.fis { display: inline-flex; gap: 3px; margin-left: 7px; vertical-align: middle; } .fis .fi { margin: 0; width: 20px; height: 20px; font-size: 9px; }
li { margin: 3px 0; }
</style></head><body><main><div id="login"></div><div id="m"><p class="sub">Cargando...</p></div></main>
<dialog id="gd"><div class="hd"><div><h3 id="gd_t"></h3><p id="gd_s"></p></div><button id="gd_x" aria-label="Cerrar">&times;</button></div><div class="bd" id="gd_b"></div></dialog><script>
const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const m = document.getElementById("m"), lg = document.getElementById("login");
const parts = location.pathname.split("/").filter(Boolean), last = parts[parts.length - 1], id = /^[0-9]+$/.test(last) ? last : null;
const base = id ? "../" : "";
const B32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
function codeBytes(code) {
  let t = String(code || "").trim().toUpperCase(); if (t.startsWith("SSBU")) t = t.slice(4);
  let acc = 0, bits = 0; const out = [];
  for (let ch of t) {
    if (ch === "-" || ch.charCodeAt(0) <= 32) continue;
    ch = ch === "O" ? "0" : (ch === "I" || ch === "L") ? "1" : ch;
    const v = B32.indexOf(ch); if (v < 0) return null;
    acc = ((acc << 5) | v) & 0xffff; bits += 5;
    if (bits >= 8) { out.push((acc >> (bits - 8)) & 255); bits -= 8; }
  }
  if (out.length !== 50 || bits) return null;
  const h = sha256(out.slice(0, 48)); if (h[0] !== out[48] || h[1] !== out[49]) return null;
  return out;
}
function sha256(bytes) {
  const K = [0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,
    0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,
    0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,
    0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
  const H = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
  const m = bytes.slice(); const n = m.length; m.push(0x80); while (m.length % 64 !== 56) m.push(0);
  const hi = Math.floor(n / 0x20000000), lo = (n * 8) >>> 0;
  m.push((hi >>> 24) & 255, (hi >>> 16) & 255, (hi >>> 8) & 255, hi & 255, (lo >>> 24) & 255, (lo >>> 16) & 255, (lo >>> 8) & 255, lo & 255);
  const r = (x, k) => (x >>> k) | (x << (32 - k)), w = new Array(64);
  for (let o = 0; o < m.length; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = (m[o + 4 * i] << 24) | (m[o + 4 * i + 1] << 16) | (m[o + 4 * i + 2] << 8) | m[o + 4 * i + 3];
    for (let i = 16; i < 64; i++) {
      const s0 = r(w[i - 15], 7) ^ r(w[i - 15], 18) ^ (w[i - 15] >>> 3), s1 = r(w[i - 2], 17) ^ r(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let [a, b, c, d, e, f, g, h] = H;
    for (let i = 0; i < 64; i++) {
      const t1 = (h + (r(e, 6) ^ r(e, 11) ^ r(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0;
      const t2 = ((r(a, 2) ^ r(a, 13) ^ r(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    H[0] = (H[0] + a) | 0; H[1] = (H[1] + b) | 0; H[2] = (H[2] + c) | 0; H[3] = (H[3] + d) | 0;
    H[4] = (H[4] + e) | 0; H[5] = (H[5] + f) | 0; H[6] = (H[6] + g) | 0; H[7] = (H[7] + h) | 0;
  }
  const out = []; H.forEach(x => out.push((x >>> 24) & 255, (x >>> 16) & 255, (x >>> 8) & 255, x & 255)); return out;
}
function hmac256(key, msg) {
  let k = key.slice(); if (k.length > 64) k = sha256(k); while (k.length < 64) k.push(0);
  const ip = k.map(x => x ^ 0x36), op = k.map(x => x ^ 0x5c);
  return sha256(op.concat(sha256(ip.concat(msg))));
}
const hex = b => b.map(x => x.toString(16).padStart(2, "0")).join("");
let tok = "", me = null, msg = "", msgOk = false, timer = null;
try { tok = localStorage.getItem("ssbu_org_token") || ""; } catch (e) {}
const ST = {signup: "inscripciones abiertas", checkin: "check-in abierto", running: "en juego", done: "terminado", cancelled: "cancelado"};
async function api(path, args) {
  const r = await fetch(base + "api/" + path, {method: "POST", headers: {"Content-Type": "application/json", "Authorization": "Bearer " + tok},
                                              body: JSON.stringify(args || {})});
  return r.json();
}
async function act(action, args) {
  const r = await api("tournament/" + (action === "create" ? "new" : id) + "/" + action, args);
  msg = r.error || "Hecho."; msgOk = !r.error;
  if (r.error && /log in again/.test(r.error)) { logout(); }
  await load();
  return r;
}
function logout() { tok = ""; me = null; try { localStorage.removeItem("ssbu_org_token"); } catch (e) {} showLogin(); }
async function login() {
  const b = codeBytes(document.getElementById("code").value);
  if (!b) { document.getElementById("lmsg").textContent = "Ese no es un código de cuenta (F1 > Online > Account code > Copy)."; return; }
  const n = await api("nonce", {}); if (n.error) { document.getElementById("lmsg").textContent = n.error; return; }
  const enc = Array.from(new TextEncoder().encode(n.nonce));
  const r = await api("login", {id: hex(b.slice(0, 16)), nonce: n.nonce, proof: hex(hmac256(b.slice(16, 48), enc))});
  if (r.error) { document.getElementById("lmsg").textContent = r.error; return; }
  tok = r.token; me = r; try { localStorage.setItem("ssbu_org_token", tok); } catch (e) {}
  showLogin(); load();
}
function showLogin() {
  if (me) { lg.innerHTML = "<p class=sub>Organizador: <b>" + esc(me.name) + "</b> &middot; <a href='#' id=lo>salir</a></p>";
            document.getElementById("lo").onclick = e => { e.preventDefault(); logout(); load(); }; return; }
  lg.innerHTML = "<details class=panel><summary><b>Entrar como organizador</b></summary><p class=sub>Pega tu código de cuenta (en el juego: F1 &gt; Online &gt; Account code &gt; Copy). " +
    "El código no se envía: el navegador solo firma una prueba con él.</p><input id=code style='width:100%' placeholder='SSBU-XXXXXXXX-...'> <p><button class=main id=lb>Entrar</button></p><p class='msg err' id=lmsg></p></details>";
  document.getElementById("lb").onclick = login;
}
function msgHtml() { return msg ? "<p class='msg " + (msgOk ? "ok" : "err") + "'>" + esc(msg) + "</p>" : ""; }
// (2026-11-03) the fighter's badge: its initials on a colour of its own (no game art on this public page)
let ICONS = new Set(), NKEY = {};                  // the icons the server has (icons.json), fighter name -> internal name
function fi(name) {
  if (!name) return "";
  const k = NKEY[name];
  if (k && ICONS.has(k)) return "<img class='fi img' src='" + base + "icons/" + k + ".png' alt='" + esc(name) + "' title='" + esc(name) + "'>";
  const w = name.replace(/[^A-Za-z0-9 ]/g, " ").trim().split(/\\s+/).filter(Boolean);
  const ini = w.length > 1 ? w[0][0] + w[1][0] : (w[0] || "?").slice(0, 2);
  let h = 0; for (const c of name) h = (h * 31 + c.charCodeAt(0)) % 360;
  return "<span class=fi style='background:hsl(" + h + " 55% 42%)' title='" + esc(name) + "'>" + esc(ini.charAt(0).toUpperCase() + ini.slice(1)) + "</span>";
}
function setChars(s, i) {                            // every fighter side i used in set s, in the order first used
  const out = []; (s.games || []).forEach(g => { const c = g.chars && g.chars[i]; if (c && !out.includes(c)) out.push(c); });
  return out.map(fi).join("");
}
function mains(t) {                                   // every fighter each player used in this tournament, the most played first
  const n = {};
  t.sets.forEach(s => (s.games || []).forEach(g => [0, 1].forEach(i => {
    const p = s.players[i], c = g.chars && g.chars[i]; if (!p || !c) return;
    (n[p] = n[p] || {})[c] = (n[p][c] || 0) + 1; })));
  const out = {}; for (const p in n) out[p] = Object.keys(n[p]).sort((a, b) => n[p][b] - n[p][a]);
  return out;
}
function card(s, t, org) {
  if (s.state === "skipped") return "";
  const row = i => { const p = s.players[i]; return "<div class='p " + (p == null ? "tbd" : s.winner && s.winner === p ? "w" : "") + "'><span>" +
    esc(p == null ? "-" : p) + "<span class=fis>" + setChars(s, i) + "</span></span><span class=n>" + (s.state === "done" || s.state === "playing" ? s.score[i] : "") + "</span></div>"; };
  let tools = "";
  if (org && t.state === "running" && s.players[0] != null && s.players[1] != null && s.players[0] !== "(bye)" && s.players[1] !== "(bye)" &&
      ["ready", "playing", "done", "forfeit"].includes(s.state)) {
    tools = "<div class=org><button class=small data-set=" + s.id + " data-w=0>Ganó " + esc(s.players[0]) + "</button><button class=small data-set=" + s.id +
            " data-w=1>Ganó " + esc(s.players[1]) + "</button>" + (s.state === "done" ? "<button class=small data-set=" + s.id + " data-w=-1>Jugar de nuevo</button>" : "") +
            (s.state !== "done" && s.state !== "forfeit" ? "<button class=small data-feat=" + s.id + ">" + (s.featured ? "Quitar del stream" : "En stream") + "</button>" : "") + "</div>";
  }
  const by = (s.by === "noshow" ? " (no se presentó)" : s.by === "organizer" ? " (organizador)" : "") + (s.featured ? " - en stream, arena " + s.featured : "");
  const n = (s.games || []).length;
  const more = n ? "<div class=more>" + n + (n === 1 ? " game" : " games") + " &middot; ver escenarios y personajes</div>" : "";
  return "<div class='set" + (s.state === "playing" || s.state === "ready" ? " live" : "") + (n ? " open' data-gd=" + s.id : "'") +
         " title='mejor de " + s.bo + by + "'>" + row(0) + row(1) + more + tools + "</div>";
}
function bracket(t, br, title, org) {
  const sets = t.sets.filter(s => s.bracket === br && s.players[0] !== "(bye)" && s.players[1] !== "(bye)");
  if (!sets.length) return "";
  const rounds = [...new Set(sets.map(s => s.round))].sort((a, b) => a - b);
  return "<h2>" + title + "</h2><div class=row>" + rounds.map(r => "<div class=col><h3>" + roundTitle(t, br, r) +
    "</h3>" + sets.filter(s => s.round === r).map(s => card(s, t, org)).join("") + "</div>").join("") + "</div>";
}
function createForm() {
  if (!me) return "";
  const bo = (n, v) => "<label>" + n + "<select id=" + v[0] + ">" + [1, 3, 5, 7].map(x => "<option" + (x === v[1] ? " selected" : "") + ">" + x + "</option>").join("") + "</select></label>";
  return "<details class=panel><summary><b>Crear un torneo</b></summary><p class=sub>Ten el juego abierto y conectado a este servidor: el torneo se crea para tu versión del juego.</p><div class=grid>" +
    "<label>Nombre<input id=f_name maxlength=40></label>" + bo("Sets (mejor de)", ["f_bo", 3]) + bo("Top 8 (mejor de)", ["f_b8", 3]) + bo("Finales (mejor de)", ["f_bf", 5]) +
    "<label>Bans desde el game 2<select id=f_bans>" + [0, 1, 2, 3, 4].map(x => "<option" + (x === 2 ? " selected" : "") + ">" + x + "</option>").join("") + "</select></label>" +
    "<label>Stocks<select id=f_stocks>" + [1, 2, 3, 4, 5].map(x => "<option" + (x === 3 ? " selected" : "") + ">" + x + "</option>").join("") + "</select></label>" +
    "<label>Check-in<select id=f_ci><option value=1 selected>Sí</option><option value=0>No</option></select></label>" +
    "<label>Cuenta para el ladder<select id=f_rated><option value=0 selected>No</option><option value=1>Sí</option></select></label>" +
    "<label>No se presentó (minutos)<input id=f_ns type=number min=0 value=10></label>" +
    "<label>Tiempo para tachar (segundos)<input id=f_tf type=number min=0 value=60></label>" +
    "</div><p><button class=main id=f_go>Crear</button></p></details>";
}
function wireCreate() {
  const b = document.getElementById("f_go"); if (!b) return;
  const v = x => document.getElementById(x).value;
  b.onclick = async () => {
    const r = await act("create", {name: v("f_name"), bo: +v("f_bo"), bo_top8: +v("f_b8"), bo_finals: +v("f_bf"), bans: +v("f_bans"), stocks: +v("f_stocks"),
                                   checkin: v("f_ci") === "1", rated: v("f_rated") === "1", noshow: Math.round(+v("f_ns") * 60), turn_for: +v("f_tf")});
    if (r.id) location.href = base + "tournament/" + r.id;
  };
}
function orgPanel(t) {
  const btn = (a, label, cls) => "<button class='" + (cls || "") + "' data-a=" + a + ">" + label + "</button> ";
  let h = "<div class=panel><h2>Organizar</h2>";
  if (t.state === "signup") {
    h += "<p class=sub>Los jugadores se inscriben desde el juego (aparece un aviso al abrirlo). " + (t.rules.checkin ?
         "Abre el check-in unos 30 minutos antes: quien no confirme queda fuera al empezar." : "Sin check-in: juegan todos los inscritos.") + "</p>";
    if (t.rules.checkin) h += btn("open_checkin", "Abrir check-in", "main");
    h += btn("start", "Empezar el torneo" + (t.rules.checkin ? " (sin check-in)" : ""), t.rules.checkin ? "" : "main");
  } else if (t.state === "checkin") {
    const n = t.entrants.filter(e => e.in).length;
    h += "<p class=sub>Check-in abierto: " + n + " de " + t.entrants.length + " confirmaron. Al empezar, quien no confirmó queda fuera.</p>" +
         btn("start", "Empezar el torneo con " + n + " jugadores", "main");
  } else if (t.state === "running") {
    h += "<p class=sub>Los sets se juegan solos. En el bracket puedes marcar un resultado o pedir que un set se juegue de nuevo; en Jugadores, descalificar (DQ).</p>";
  }
  if (t.state === "signup" || t.state === "checkin") {
    h += " Seeding: <select id=sm>" + [["rating", "por rating"], ["random", "al azar"], ["manual", "manual"]].map(x =>
         "<option value=" + x[0] + (t.seed_mode === x[0] ? " selected" : "") + ">" + x[1] + "</option>").join("") + "</select> ";
  }
  if (t.state !== "done" && t.state !== "cancelled") {
    h += "<p class=sub style='margin-top:10px'>Si tu rival no llega a Smash en <input id=ns type=number min=0 max=120 style='width:70px' value=" +
         Math.round((t.rules.noshow || 0) / 60) + "> minutos, gana el que llegó (0: nunca, lo decides tú). <button class=small id=nsb>Guardar</button></p>";
    h += btn("cancel", "Cancelar el torneo");
  }
  return h + msgHtml() + "</div>";
}
function players(t, org) {
  const open = t.state === "signup" || t.state === "checkin", mn = mains(t);
  let h ="<h2>Jugadores (" + t.entrants.length + ")</h2><table class=ent><tr><th>Seed</th><th>Jugador</th><th>Rating</th>" +
          (t.state === "checkin" ? "<th>Check-in</th>" : "") + (org ? "<th></th>" : "") + "</tr>";
  t.entrants.forEach((e, i) => {
    h += "<tr><td class=n>" + e.seed + "</td><td" + (e.dq ? " style='text-decoration:line-through;color:var(--muted)'" : "") + ">" + esc(e.name) +
         "<span class=fis>" + (mn[e.name] || []).map(fi).join("") + "</span></td><td class=n>" + (e.rating == null ? "-" : e.rating) + "</td>" +
         (t.state === "checkin" ? "<td>" + (e.in ? "<span class='tag in'>listo</span>" : "<span class='tag out'>falta</span>") + "</td>" : "");
    if (org) {
      h += "<td>";
      if (open) h += "<button class=small data-mv=" + i + " data-to=" + (i - 1) + (i === 0 ? " disabled" : "") + ">&uarr;</button> " +
                    "<button class=small data-mv=" + i + " data-to=" + (i + 1) + (i === t.entrants.length - 1 ? " disabled" : "") + ">&darr;</button> ";
      if (t.state === "checkin" && !e.in) h += "<button class=small data-ci=" + i + ">Marcar listo</button> ";
      if ((open || t.state === "running") && !e.dq) h += "<button class=small data-dq=" + i + ">" + (open ? "Quitar" : "DQ") + "</button>";
      h += "</td>";
    }
    h += "</tr>";
  });
  return h + "</table>";
}
// (2026-11-03) the rounds' usual names: the last three of each bracket are its Final, Semifinal and Quarterfinal
function roundTitle(t, br, r) {
  if (t.kind === "ladder") return "Set de ladder";
  if (br === "G") return r === 1 ? "Grand Final" : "Grand Final Reset";
  const last = Math.max(...t.sets.filter(x => x.bracket === br).map(x => x.round)), side = br === "W" ? "Winners " : "Losers ";
  return side + (r === last ? "Final" : r === last - 1 ? "Semifinal" : r === last - 2 ? "Quarterfinal" : "Round " + r);
}
function roundName(t, s) { return roundTitle(t, s.bracket, s.round); }
function showGames(t, s) {
  const p = [s.players[0] || "-", s.players[1] || "-"];
  document.getElementById("gd_t").textContent = p[0] + " " + s.score[0] + " - " + s.score[1] + " " + p[1];
  document.getElementById("gd_s").textContent = roundName(t, s) + " · mejor de " + s.bo + (s.by === "organizer" ? " · resultado puesto por el organizador" : "");
  const side = (g, i) => "<span class='" + (g.winner === i ? "w" : "") + "'>" + fi(g.chars[i]) + "<span>" +
                         (g.chars[i] ? esc(g.chars[i]) : "<span class=none>personaje sin dato</span>") + "<small>" + esc(p[i]) + "</small></span></span>";
  document.getElementById("gd_b").innerHTML = (s.games || []).map((g, k) =>
    "<div class=g><div class=gn>Game " + (k + 1) + "</div><div><div class=stage>" + (g.stage ? esc(g.stage) : "<span class=none>escenario sin dato</span>") +
    "</div><div class=fx>" + side(g, 0) + side(g, 1) + "</div></div></div>").join("") || "<p class=none>Sin games registrados.</p>";
  const d = document.getElementById("gd");
  if (d.showModal) { if (!d.open) d.showModal(); } else d.setAttribute("open", "");
}
(() => {
  const d = document.getElementById("gd"), close = () => d.close ? d.close() : d.removeAttribute("open");
  document.getElementById("gd_x").onclick = close;
  d.addEventListener("click", e => { if (e.target === d) close(); });       // a click on the backdrop closes it
})();
function wire(t) {
  document.querySelectorAll("[data-gd]").forEach(el => el.onclick = e => {
    if (e.target.closest("button")) return;                                   // (the organizer's buttons on the card)
    showGames(t, t.sets.find(x => x.id === +el.dataset.gd));
  });
  document.querySelectorAll("[data-a]").forEach(b => b.onclick = () => {
    if (b.dataset.a === "cancel" && !confirm("¿Cancelar el torneo para todos?")) return;
    act(b.dataset.a, {});
  });
  const sm = document.getElementById("sm"); if (sm) sm.onchange = () => act("seed_mode", {mode: sm.value});
  const nsb = document.getElementById("nsb"); if (nsb) nsb.onclick = () => act("noshow", {minutes: +document.getElementById("ns").value});
  document.querySelectorAll("[data-mv]").forEach(b => b.onclick = () => act("move", {i: +b.dataset.mv, j: +b.dataset.to}));
  document.querySelectorAll("[data-ci]").forEach(b => b.onclick = () => act("check_in", {i: +b.dataset.ci}));
  document.querySelectorAll("[data-dq]").forEach(b => b.onclick = () => {
    if (t.state === "running" && !confirm("¿Descalificar a " + t.entrants[+b.dataset.dq].name + "? Pierde el resto de sus sets.")) return;
    act("dq", {i: +b.dataset.dq});
  });
  document.querySelectorAll("[data-set]").forEach(b => b.onclick = () => act("result", {set: +b.dataset.set, winner: +b.dataset.w}));
  document.querySelectorAll("[data-feat]").forEach(b => b.onclick = () => {
    const s = t.sets.find(x => x.id === +b.dataset.feat);
    if (s.featured) { act("feature", {set: s.id, code: ""}); return; }
    const code = prompt("Código de la Battle Arena donde se juega este set (5 letras, lo ves en el juego al crear la arena):");
    if (code) act("feature", {set: s.id, code});
  });
}
async function load() {
  clearTimeout(timer);
  if (!id) {
    const l = await (await fetch(base + "tournaments.json")).json();
    m.innerHTML = "<h1>Torneos</h1><p class=sub>Se juegan dentro del juego: inscríbete desde el aviso que sale al abrirlo.</p>" + createForm() + msgHtml() + "<ul>" +
      l.map(t => "<li><a href='tournament/" + t.id + "'>" + esc(t.name) + "</a> - " + esc(ST[t.state] || t.state) + "</li>").join("") + "</ul>";
    wireCreate();
    return;
  }
  const r = await fetch(id + ".json"); if (!r.ok) { m.innerHTML = "<p>Ese torneo no existe.</p>"; return; }
  const t = await r.json();
  t.sets.forEach(s => (s.games || []).forEach(g => [0, 1].forEach(i => { if (g.chars && g.chars[i] && g.keys && g.keys[i]) NKEY[g.chars[i]] = g.keys[i]; })));
  const org = !!(me && me.name.toLowerCase() === String(t.organizer).toLowerCase());
  const mn = mains(t);
  if (t.kind === "ladder") {                         // (2026-11-03) a ladder match: no organizer, no bracket
    m.innerHTML = "<p><a href='../ladder'>&larr; Ladder</a></p><h1>" + esc(t.name) + "</h1><p class=sub>" + esc(ST[t.state] || t.state) +
      " &middot; ladder &middot; mejor de " + t.rules.bo + " &middot; " + t.rules.stocks + " stocks &middot; " + t.rules.bans + " bans</p>" +
      (t.standings.length ? "<h2>Resultado</h2><ol>" + t.standings.map(s => "<li value=" + s.place + ">" + esc(s.name) + "<span class=fis>" + (mn[s.name] || []).map(fi).join("") + "</span></li>").join("") + "</ol>" : "") +
      bracket(t, "W", "Set", false);
    wire(t);
    if (t.state === "running") timer = setTimeout(autoLoad, 5000);
    return;
  }
  m.innerHTML = "<p><a href='../tournaments'>&larr; Torneos</a></p><h1>" + esc(t.name) + "</h1><p class=sub>" + esc(ST[t.state] || t.state) + " &middot; organiza " + esc(t.organizer) +
    " &middot; mejor de " + t.rules.bo + " (top 8: " + t.rules.bo_top8 + ", finales: " + t.rules.bo_finals + ") &middot; " + t.rules.stocks + " stocks &middot; " + t.rules.bans + " bans &middot; " +
    (t.rules.noshow ? "no se presentó: " + Math.round(t.rules.noshow / 60) + " min" : "no se presentó: lo decide el organizador") + "</p>" +
    (org ? orgPanel(t) : "") +
    (t.standings.length ? "<h2>Posiciones</h2><ol>" + t.standings.map(s => "<li value=" + s.place + ">" + esc(s.name) + "<span class=fis>" + (mn[s.name] || []).map(fi).join("") + "</span></li>").join("") + "</ol>" : "") +
    bracket(t, "W", "Winners", org) + bracket(t, "L", "Losers", org) + bracket(t, "G", "Gran final", org) + players(t, org) +
    "<h2>Escenarios</h2><p>Para empezar: " + t.stages.starters.map(esc).join(", ") + "<br>Counterpicks: " + t.stages.counterpicks.map(esc).join(", ") + "</p>";
  wire(t);
  if (["running", "signup", "checkin"].includes(t.state)) timer = setTimeout(autoLoad, 5000);
}
function autoLoad() {                                 // (not while the organizer types in a field of the page, nor with a set's window open)
  const a = document.activeElement, d = document.getElementById("gd");
  if ((a && m.contains(a) && /^(INPUT|SELECT)$/.test(a.tagName)) || (d && d.open)) { timer = setTimeout(autoLoad, 2000); return; }
  load();
}
(async () => {
  if (tok) { const r = await api("me", {}); if (!r.error) me = r; else { tok = ""; try { localStorage.removeItem("ssbu_org_token"); } catch (e) {} } }
  try { ICONS = new Set(await (await fetch(base + "icons.json")).json()); } catch (e) {}
  showLogin(); load();
})();
</script></body></html>
"""


def start_status_server(lobby, bind, port, started):
    """Serve GET /status (and, with a ladder, /ladder, /ladder.json, /player/<name>.json) on a daemon thread. Returns the
    server (its .server_address has the real port)."""
    class Handler(BaseHTTPRequestHandler):
        server_version = "ssbu-lobby-status"
        sys_version = ""

        def head(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()

        def route(self):
            """(code, body, content type) of a GET. The ladder documents never hold ids, keys or addresses."""
            path = self.path.split("?", 1)[0].rstrip("/")
            ld = lobby.ladder
            if path == "/status":
                return 200, status_json(lobby, started, time.monotonic()).encode(), "application/json"
            if ld is not None and path == "/ladder":
                return 200, LADDER_PAGE.encode(), "text/html; charset=utf-8"
            if ld is not None and path == "/ladder.json":
                rows = ld.table()
                for row in rows:                    # (2026-11-03) the fighters each player used most, for the icons
                    row["fighters"] = [{"key": tournament_mod.chara_key(x), "name": tournament_mod.chara_name(x)}
                                       for x in row.pop("mains", []) if isinstance(x, int) and tournament_mod is not None]
                return 200, json.dumps(rows).encode(), "application/json"
            if ld is not None and path == "/ladder_matches.json":
                return 200, json.dumps(ld.matches()).encode(), "application/json"
            if lobby.tourneys is not None and path == "/tournaments":
                return 200, TOURNEY_PAGE.encode(), "text/html; charset=utf-8"
            if lobby.tourneys is not None and path == "/icons.json":
                return 200, json.dumps(icon_list(lobby.icons_dir)).encode(), "application/json"
            if lobby.tourneys is not None and path.startswith("/icons/") and path.endswith(".png") and lobby.icons_dir:
                name = path[len("/icons/"):-len(".png")]
                if ICON_NAME_RE.match(name):
                    try:
                        with open(os.path.join(lobby.icons_dir, name + ".png"), "rb") as f:
                            data = f.read(512 * 1024 + 1)
                        if len(data) <= 512 * 1024 and data[:8] == b"\x89PNG\r\n\x1a\n":
                            return 200, data, "image/png"
                    except OSError:
                        pass
            if lobby.tourneys is not None and path == "/tournaments.json":
                return 200, json.dumps(tourney_list_json(ld.path)).encode(), "application/json"
            if lobby.tourneys is not None and path.startswith("/tournament/"):
                rest = path[len("/tournament/"):]
                tid = rest[:-len(".json")] if rest.endswith(".json") else rest
                if tid.isdigit() and not rest.endswith(".json"):
                    return 200, TOURNEY_PAGE.encode(), "text/html; charset=utf-8"
                doc = tourney_json(ld.path, int(tid)) if tid.isdigit() else None
                if doc is not None:
                    return 200, json.dumps(doc).encode(), "application/json"
            if ld is not None and path.startswith("/player/") and path.endswith(".json"):
                h = ld.history(urllib.parse.unquote(path[len("/player/"):-len(".json")]))
                if h is not None:
                    return 200, json.dumps(h).encode(), "application/json"
            return 404, b'{"error":"not found"}', "application/json"

        def do_GET(self):
            code, body, ctype = self.route()
            self.head(code, body, ctype)
            self.wfile.write(body)

        def do_POST(self):
            """(2026-11-02) the organizer's web page: POST /api/nonce -> {"nonce"}; POST /api/login {"id", "nonce",
            "proof": HMAC-SHA256(account key, nonce)} -> {"token", "name", "admin"};
            POST /api/tournament/<id|new>/<action> {args} with "Authorization: Bearer <token>" -> {"ok"} or {"error"}"""
            path = self.path.split("?", 1)[0].rstrip("/")
            try:
                n = int(self.headers.get("Content-Length") or 0)
                if n > 8192:
                    raise ValueError("too big")
                a = json.loads(self.rfile.read(n) or b"{}") if n else {}
                if not isinstance(a, dict):
                    raise ValueError("not an object")
            except (ValueError, OSError):
                body = b'{"error":"a malformed request"}'
                self.head(400, body, "application/json")
                self.wfile.write(body)
                return
            tok = (self.headers.get("Authorization") or "").replace("Bearer", "").strip()
            code, out = 404, {"error": "not found"}
            if lobby.tourneys is not None and tournament_mod is not None:
                try:
                    if path == "/api/nonce":
                        out = lobby.web_call(lambda now: lobby.web_nonce(now))
                        code = 200
                    elif path == "/api/login":
                        out = lobby.web_call(lambda now: lobby.web_login(a, now))
                        code = 200
                    elif path == "/api/me":
                        out = lobby.web_call(lambda now: lobby.web_action(tok, 0, "me", a, now))
                        code = 200
                    elif path.startswith("/api/tournament/"):
                        parts = path[len("/api/tournament/"):].split("/")
                        if len(parts) == 2 and (parts[0].isdigit() or parts[0] == "new"):
                            tid = int(parts[0]) if parts[0].isdigit() else 0
                            act = "create" if parts[0] == "new" else parts[1]
                            out = lobby.web_call(lambda now: lobby.web_action(tok, tid, act, a, now))
                            code = 200
                except tournament_mod.Error as e:
                    code, out = 200, {"error": str(e)}
                except (KeyError, TypeError, ValueError):
                    code, out = 200, {"error": "a malformed request"}
            body = json.dumps(out).encode()
            self.head(code, body, "application/json")
            self.wfile.write(body)

        def do_HEAD(self):
            code, _, ctype = self.route()
            self.head(code, b"", ctype)

        def do_OPTIONS(self):
            self.head(204, b"", "text/plain")

        def log_message(self, *args):   # no request log: it would hold the callers' addresses
            pass

    srv = ThreadingHTTPServer((bind, port), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, name="status-http", daemon=True).start()
    return srv


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=39500)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--stats", type=float, default=10.0, help="seconds between statistics lines (0: none)")
    ap.add_argument("--status-port", type=int, default=0, help="TCP port of the HTTP status endpoint GET /status (0: off)")
    ap.add_argument("--status-bind", default="127.0.0.1", help="address of the status endpoint (default 127.0.0.1, behind a proxy)")
    ap.add_argument("--search-secs", type=float, default=SEARCH_FOR, help="matchmaking: a search without an opponent ends after this (default 120)")
    ap.add_argument("--match-secs", type=float, default=MATCH_FOR, help="matchmaking: matched players not in one room by then are told it expired (default 180)")
    ap.add_argument("--ladder-db", default="", help="the ranked ladder's sqlite file (ladder.py; empty: no ladder)")
    ap.add_argument("--ladder-open", action="store_true", help="ladder: any new player is ranked the first time it is seen (default: by invitation only)")
    ap.add_argument("--ladder-invite", metavar="NAME", help="ladder: print a one-use invitation code for a player called NAME, and exit")
    ap.add_argument("--account-list", action="store_true", help="accounts: list them (name, rating, sets won / lost, banned), and exit")
    ap.add_argument("--account-rename", nargs=2, metavar=("OLD", "NEW"), help="accounts: rename one, and exit")
    ap.add_argument("--account-ban", metavar="NAME", help="accounts: ban one (it plays on, unranked), and exit")
    ap.add_argument("--account-unban", metavar="NAME", help="accounts: lift a ban, and exit")
    ap.add_argument("--account-delete", metavar="NAME", help="accounts: delete one (its game signs up again as a new account), and exit")
    ap.add_argument("--tournament-admins", default="", help="tournaments: the account names that may make them, comma-separated (empty: any account)")
    ap.add_argument("--icons-dir", default="", help="tournament pages: folder of fighter icons <name>.png (default: icons beside --ladder-db)")
    ap.add_argument("--arena-streak", type=int, default=3, help="arena: wins in a row after which the winner goes to the back of the queue too (default 3, 0: no limit)")
    ap.add_argument("--ladder-stages", default="", help="ladder: the legal stages, comma-separated stage ids (match block + 0x420, hex or decimal; empty: any)")
    o = ap.parse_args()
    ladder = None
    if o.ladder_db:
        if ladder_mod is None:
            print("--ladder-db: ladder.py is not next to ssbu_lobby.py", file=sys.stderr)
            return 2
        stages = {int(x, 0) for x in o.ladder_stages.split(",") if x.strip()} or None
        ladder = ladder_mod.Ladder(o.ladder_db, open_signup=o.ladder_open, legal_stages=stages)
        if o.ladder_invite:
            print(ladder.invite(o.ladder_invite))
            return 0
        if o.account_list:
            for name, rating, wins, losses, banned, _ in ladder.accounts():
                print("%-24s %6.0f  sets %d-%d%s" % (name, rating, wins, losses, "  BANNED" if banned else ""))
            return 0
        act = (ladder.rename(*o.account_rename) if o.account_rename else ladder.ban(o.account_ban) if o.account_ban else
               ladder.ban(o.account_unban, False) if o.account_unban else ladder.delete(o.account_delete) if o.account_delete else 0)
        if act != 0:
            print(act or "done")
            return 1 if act else 0
    elif o.ladder_invite or o.account_list or o.account_rename or o.account_ban or o.account_unban or o.account_delete:
        print("--ladder-invite and --account-* need --ladder-db", file=sys.stderr)
        return 2
    started = time.monotonic()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if os.name == "nt":   # (no ICMP "port unreachable" errors on receive)
        try:
            sock.ioctl(socket.SIO_UDP_CONNRESET, False)
        except (AttributeError, OSError):
            pass
    sock.bind((o.bind, o.port))
    admins = [x.strip() for x in o.tournament_admins.split(",") if x.strip()] or None
    lobby = Lobby(sock, o.verbose, o.search_secs, o.match_secs, ladder, o.arena_streak, admins)
    lobby.icons_dir = o.icons_dir or (os.path.join(os.path.dirname(os.path.abspath(o.ladder_db)), "icons") if o.ladder_db else "")
    lobby.say("lobby on UDP %s:%d (protocol SU %d)" % (o.bind, o.port, PROTO))
    if ladder is not None:
        lobby.say("ladder: %s (%s)" % (o.ladder_db, "open sign-up" if o.ladder_open else "by invitation"))
        lobby.say("tournaments: %s" % ("off (tournament.py is not next to ssbu_lobby.py)" if lobby.tourneys is None else
                                       "made by %s" % (", ".join(admins) if admins else "any account")))
    if o.status_port:
        srv = start_status_server(lobby, o.status_bind, o.status_port, started)
        lobby.say("status endpoint on http://%s:%d/status%s" % (srv.server_address[:2] + (" (ladder: /ladder)" if ladder is not None else "",)))
    last_sweep = last_stats = time.monotonic()
    while True:
        r, _, _ = select.select([sock], [], [], 0.2)
        now = time.monotonic()
        lobby.web_drain(now)
        if r:
            try:
                p, at = sock.recvfrom(65536)
            except OSError:
                continue
            try:
                lobby.packet(p, at, now)
            except Exception as e:   # one bad packet never stops the lobby
                lobby.say("error on a packet from %s: %r" % (at[0], e))
        if now - last_sweep >= 1.0:
            last_sweep = now
            lobby.sweep(now)
        if o.stats and now - last_stats >= o.stats:
            last_stats = now
            if lobby.by_at:
                lobby.say("stats: %d player(s), %d room(s), %s" % (len(lobby.by_at), len(lobby.rooms),
                                                                   ", ".join("%s %d" % kv for kv in lobby.stats.items())))


if __name__ == "__main__":
    sys.exit(main())
