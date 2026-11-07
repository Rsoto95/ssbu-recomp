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
Score (lobby 1.8, 2026-11-07): the lobby counts the wins and losses of each player (its ladder id, else its name: not the
port, not the seat) in rollback matches, from the first RESULT of each match that has F_STOCKS_FALLS (a newer game) and a
winner (no disconnection, not abandoned). A player's count belongs to the room it plays in (the Battle Arena's key of its
rollback search, 0 outside one): a match in another room starts it again at 0-0, as does SCORE_IDLE s without a match
(a game that loses the lobby for a moment and comes back to the same room keeps it). Both fighters
are told when their match starts and after each counted result; the game shows it over the fight (menu.cpp).
    28 SCORE        lobby:  the other fighter's address u32, wins u16, losses u16 (the one told), wins u16, losses u16 (the other),
                            name[32] (the one told), name[32] (the other): the ladder name, else the lobby name
    29 ROOM_LEFT    nex_server.py on this host (from 127.0.0.1 only): arena gid u32, the player's public address u32: the
                    players at that address whose count is of that arena start again at 0-0 (they left the room: by its
                    menu, a dropped connection, or the arena closed)

Python 3.8+, standard library only. No encryption: run it for people you know."""
import argparse
import json
import os
import secrets
import select
import socket
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

PROTO = 1
LOBBY_VERSION = "1.8"
(JOIN, WELCOME, REJECT, PING, PONG, DATA, LEAVE, LIST, LISTING, LIST_END,
 PEERS, PUNCH, DIRECT, BCAST) = range(1, 15)
RB = 16          # online rollback messages between two games (recomp-study/net_relay.c T_RB, rb_net.c)
ROOMS, ROOM_INFO, ROOMS_END = 17, 18, 19     # O1 room browser (net_relay.c): every room of this lobby code, any build
MATCH, MATCH_STATE = 20, 21                  # O2 matchmaking (net_relay.c): find an opponent of the same build and code
RESULT, RESULT_ACK = 22, 23                  # 1.5 ladder: a rollback match's result (net_relay.c, ladder_report.c)
ARENA, ARENA_STATE = 24, 25                  # 1.6 arena: a queue of players taking turns at 1-on-1 rollback matches
RB_GROUP = 27                                # 1.7 (2026-11-02): a Battle Arena's group (fighters + spectators, key = the arena)
ROOM_LEFT = 29                               # 1.8: from nex_server.py on this host (127.0.0.1 only): a player left a Battle Arena
SCORE = 28                                   # 1.8 (2026-11-07): the session's wins / losses of the two fighters of a rollback match
SCORE_IDLE = 3600.0                          # a player's score starts again at 0-0 after an hour without a match
F_STOCKS_FALLS = 4                           # RESULT flag: stocks left = the rule's stocks less the falls (a newer game: its winner is right)
AR_OUT, AR_WAITING, AR_TURN, AR_PLAYING = range(4)
ARENA_TURN_FOR, ARENA_PLAY_FOR, ARENA_WATCH_FOR, ARENA_LIST = 120.0, 900.0, 3.0, 16
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


class Lobby:
    def __init__(self, sock, verbose, search_for=SEARCH_FOR, match_for=MATCH_FOR, ladder=None, arena_streak=3):
        self.sock, self.verbose, self.ladder = sock, verbose, ladder
        self.arenas = {}          # 1.6: part (build, code) -> Arena
        self.arena_streak = arena_streak
        self.search_for, self.match_for = search_for, match_for
        self.by_at, self.by_ip, self.rooms = {}, {}, {}
        self.next_ip = SUBNET + 2
        self.ladder_said = {}     # match seed -> outcome already logged
        self.score = {}           # 1.8: player key -> [wins, losses, time of its last match, the room (arena key) it counts in]
        self.score_seeds = {}     # 1.8: match seeds already counted (both games send their RESULT)
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
        if c.match is not None:
            other = c.match.other(c)
            c.match = None
            self.match_end(other, MS_PEER_LEFT, "%s left the lobby" % c.name)
        c.search_since = None
        if self.ladder is not None and c.ladder_id is not None and self.ladder.void_sets(c.ladder_id):
            self.say("ladder: the open set of %s is void (%s)" % (c.ladder_why, why))
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
        if kind == ROOM_LEFT and at[0] == "127.0.0.1" and len(p) >= 12:
            return self.score_room_left(struct.unpack_from(">I", p, 4)[0], socket.inet_ntoa(p[8:12]), now)
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
                    self.ladder_open_set(h, g)
                    self.score_send(h, g, now, touch=True)
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
            self.score_result(c, p, now)
            self.arena_result(c, p, now)
            self.on_result(c, p)
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

    def ladder_open_set(self, a, b):
        """a (player 1) and b play a rollback match: their set, if both are ranked"""
        if self.ladder is not None and a.ladder_id is not None and b.ladder_id is not None and a.ladder_id != b.ladder_id:
            if self.ladder.open_set_of(a.ladder_id, b.ladder_id) is None:
                self.ladder.open_set(a.ladder_id, b.ladder_id)
                self.say("ladder: set opened: %s vs %s" % (a.ladder_why, b.ladder_why))

    def on_result(self, c, p):
        """RESULT: cookie u64, match seed u64, the other player's address u32, slot u8 (0: player 1), rule kind u8, stocks of
        the rule u8, flags u8 (1 abandoned, 2 disconnected), stage u32, chara u64 x 2, stocks left u8 x 2, damage (tenths of
        a percent) u16 x 2, frames u32, desyncs u32"""
        seed, peer, slot, rule_kind, stock, flags, stage = struct.unpack_from(">QIBBBBI", p, 12)
        c0, c1, s0, s1, d0, d1, frames, desyncs = struct.unpack_from(">QQBBHHII", p, 32)
        opp = self.by_ip.get(peer)
        if (self.ladder is None or c.ladder_id is None or opp is None or opp is c or opp.ladder_id is None
                or opp.ladder_id == c.ladder_id or opp.part[1] != c.part[1] or slot > 1):
            self.log("ladder: the result of %s is not ranked" % c.name)
            return self.ladder_ack(c, seed, OUT_UNRANKED)
        a, b = (c, opp) if slot == 0 else (opp, c)
        self.ladder_open_set(a, b)
        r = {"seed": seed, "slot": slot, "stage": stage, "chara": (c0, c1), "stocks": (s0, s1), "dmg": (d0, d1), "frames": frames,
             "rule_kind": rule_kind, "stock": stock, "desyncs": desyncs, "flags": flags}
        out = self.ladder.report(c.ladder_id, opp.ladder_id, r)
        if out is None:
            return self.ladder_ack(c, seed, OUT_WAIT, opp)
        code = OUT_OK if out == "ok" else OUT_DRAW if out == "draw" else OUT_DISPUTED
        if seed not in self.ladder_said:
            self.ladder_said[seed] = out
            self.say("ladder: game %016x, %s vs %s: %s" % (seed, a.ladder_why, b.ladder_why, out))
        for x, o in ((c, opp), (opp, c)):
            self.ladder_ack(x, seed, code, o)

    # ---- 1.8 score
    @staticmethod
    def score_key(c):
        return ("ladder", c.ladder_id) if c.ladder_id is not None else ("name", c.name)

    def score_row(self, c, now, room=None):
        """c's count; room (its match's arena key): another room than the count's starts it again"""
        r = self.score.get(self.score_key(c))
        if r is None or now - r[2] > SCORE_IDLE or (room is not None and r[3] != room):
            if r is not None and (r[0] or r[1]):
                self.say("score: %s starts again at 0-0 (it was %d-%d; %s)" % (c.name, r[0], r[1], "another room" if now - r[2] <= SCORE_IDLE else "an hour without a match"))
            r = self.score[self.score_key(c)] = [0, 0, now, room if room is not None else (r[3] if r is not None else 0)]
        return r

    @staticmethod
    def score_name(c):
        return (c.ladder_why if c.ladder_id is not None and c.ladder_why else c.name).encode("utf-8", "replace")[:NAME_LEN].ljust(NAME_LEN, b"\0")

    def score_room_left(self, gid, ip, now):
        for x in list(self.by_at.values()):
            r = self.score.get(self.score_key(x))
            if x.at[0] == ip and r is not None and r[3] == gid:
                if r[0] or r[1]:
                    self.say("score: %s left room %u: starts again at 0-0 (it was %d-%d)" % (x.name, gid, r[0], r[1]))
                r[0] = r[1] = 0

    def score_send(self, a, b, now, touch=False):
        """SCORE to both fighters a and b (b may be None: a alone is told nothing)"""
        if a is None or b is None or a is b:
            return
        ra = self.score_row(a, now, getattr(a, "match_key", 0) if touch else None)
        rb = self.score_row(b, now, getattr(b, "match_key", 0) if touch else None)
        if touch:
            ra[2] = rb[2] = now
        for x, o, rx, ro in ((a, b, ra, rb), (b, a, rb, ra)):
            self.send(hdr(SCORE) + struct.pack(">IHHHH", o.ip, min(rx[0], 65535), min(rx[1], 65535), min(ro[0], 65535), min(ro[1], 65535))
                      + self.score_name(x) + self.score_name(o), x.at)

    def score_result(self, c, p, now):
        """RESULT of a rollback match: the winner's win and the loser's loss, once per match"""
        seed, peer, slot, rule_kind, stock, flags = struct.unpack_from(">QIBBBB", p, 12)
        s0, s1, d0, d1 = struct.unpack_from(">BBHH", p, 48)
        if seed in self.score_seeds or slot > 1 or not flags & F_STOCKS_FALLS or flags & 3:
            return
        win = 0 if s0 > s1 else 1 if s1 > s0 else 0 if d0 < d1 else 1 if d1 < d0 else None
        if win is None:
            return
        self.score_seeds[seed] = now
        if len(self.score_seeds) > 4096:
            self.score_seeds.pop(next(iter(self.score_seeds)))
        opp = self.by_ip.get(peer) or self.by_ip.get(c.rb_peer)
        if opp is c:
            opp = None
        winner, loser = (c, opp) if win == slot else (opp, c)
        for x, k in ((winner, 0), (loser, 1)):
            if x is not None:
                r = self.score_row(x, now)
                r[k] += 1
                r[2] = now
        w, l = self.score_row(winner, now) if winner else None, self.score_row(loser, now) if loser else None
        self.say("score: %s beat %s (session: %s, %s)" % (winner.name if winner else "?", loser.name if loser else "?",
                 "%d-%d" % (w[0], w[1]) if w else "-", "%d-%d" % (l[0], l[1]) if l else "-"))
        self.score_send(c, opp, now)

    def ladder_sweep(self):
        if self.ladder is not None:
            for out in self.ladder.sweep():
                self.say("ladder: a game without its second report: %s" % out)

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
        # (2026-11-05) a Battle Arena's search (a key) is paired by its key alone: the lobby's own room queue (F1, 1.6) must
        # not hold it back. A player left in that queue (joined it earlier, never left) was never paired in a Battle Arena,
        # and both fighters fell back to the game's own netcode (no rollback).
        a = self.arenas.get(c.part) if not key else None
        pair = a.pair if a is not None and op == KIND_ROLLBACK else None
        if pair and c in pair:                      # 1.6 arena: a player taking its turn meets only its opponent of the turn
            waiting = [x for x in waiting if x is (pair[1] if c is pair[0] else pair[0])]
        elif a is not None and c in a.members():   # (in the queue: its turn has not come; the box says it searches)
            waiting = []
        elif a is not None:                         # (a player outside the arena never takes one of the arena's)
            waiting = [x for x in waiting if x not in a.members()]
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
        self.ladder_sweep()
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
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>SSBU ladder</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; --line: #ddddd6; --accent: #c2410c; --win: #15803d; --loss: #b91c1c; }
@media (prefers-color-scheme: dark) { :root { --bg: #161615; --fg: #ecece8; --muted: #9a9a93; --line: #2e2e2b; --accent: #fb923c; --win: #4ade80; --loss: #f87171; } }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.45 system-ui, sans-serif; }
main { max-width: 760px; margin: 0 auto; padding: 24px 16px; }
h1 { font-size: 22px; margin: 0 0 4px; } p.sub { color: var(--muted); margin: 0 0 20px; }
table { width: 100%; border-collapse: collapse; } th, td { padding: 8px 6px; border-bottom: 1px solid var(--line); text-align: left; }
th { color: var(--muted); font-weight: 600; font-size: 13px; } td.n { text-align: right; font-variant-numeric: tabular-nums; }
tr.p { cursor: pointer; } tr.p:hover td { background: color-mix(in srgb, var(--accent) 8%, transparent); }
.rd { color: var(--muted); font-size: 12px; } .w { color: var(--win); } .l { color: var(--loss); }
#hist { margin-top: 28px; } #hist h2 { font-size: 17px; margin: 0 0 8px; }
</style></head><body><main>
<h1>Ladder</h1><p class="sub">Glicko-2, Bo3 sets, 3 stocks. Ordered by rating minus twice the deviation.</p>
<table><thead><tr><th>#</th><th>Player</th><th class="n">Rating</th><th class="n">Sets W-L</th></tr></thead><tbody id="t"></tbody></table>
<section id="hist"></section>
</main><script>
const esc = s => String(s).replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
async function show(name) {
  const r = await fetch("player/" + encodeURIComponent(name) + ".json"); if (!r.ok) return;
  const h = await r.json(), el = document.getElementById("hist");
  el.innerHTML = "<h2>" + esc(h.name) + "</h2><table><thead><tr><th>Opponent</th><th>Result</th><th class=n>Score</th><th class=n>Rating</th></tr></thead><tbody>" +
    h.sets.map(s => "<tr><td>" + esc(s.opponent) + "</td><td class=" + (s.won === true ? "w" : s.won === false ? "l" : "") + ">" +
      (s.state === "done" ? (s.won ? "won" : "lost") : s.state) + "</td><td class=n>" + s.score.join("-") + "</td><td class=n>" +
      (s.rating ? s.rating[0] + " &rarr; " + s.rating[1] : "") + "</td></tr>").join("") + "</tbody></table>";
}
fetch("ladder.json").then(r => r.json()).then(rows => {
  document.getElementById("t").innerHTML = rows.map((p, i) => "<tr class=p data-n=\"" + esc(p.name) + "\"><td>" + (i + 1) + "</td><td>" + esc(p.name) +
    "</td><td class=n>" + p.rating + " <span class=rd>&plusmn;" + p.rd + "</span></td><td class=n>" + p.wins + "-" + p.losses + "</td></tr>").join("");
  document.querySelectorAll("tr.p").forEach(tr => tr.onclick = () => show(tr.dataset.n));
});
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
            self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
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
                return 200, json.dumps(ld.table()).encode(), "application/json"
            if ld is not None and path.startswith("/player/") and path.endswith(".json"):
                h = ld.history(urllib.parse.unquote(path[len("/player/"):-len(".json")]))
                if h is not None:
                    return 200, json.dumps(h).encode(), "application/json"
            return 404, b'{"error":"not found"}', "application/json"

        def do_GET(self):
            code, body, ctype = self.route()
            self.head(code, body, ctype)
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
    elif o.ladder_invite:
        print("--ladder-invite needs --ladder-db", file=sys.stderr)
        return 2
    started = time.monotonic()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if os.name == "nt":   # (no ICMP "port unreachable" errors on receive)
        try:
            sock.ioctl(socket.SIO_UDP_CONNRESET, False)
        except (AttributeError, OSError):
            pass
    sock.bind((o.bind, o.port))
    lobby = Lobby(sock, o.verbose, o.search_secs, o.match_secs, ladder, o.arena_streak)
    lobby.say("lobby on UDP %s:%d (protocol SU %d)" % (o.bind, o.port, PROTO))
    if ladder is not None:
        lobby.say("ladder: %s (%s)" % (o.ladder_db, "open sign-up" if o.ladder_open else "by invitation"))
    if o.status_port:
        srv = start_status_server(lobby, o.status_bind, o.status_port, started)
        lobby.say("status endpoint on http://%s:%d/status%s" % (srv.server_address[:2] + (" (ladder: /ladder)" if ladder is not None else "",)))
    last_sweep = last_stats = time.monotonic()
    while True:
        r, _, _ = select.select([sock], [], [], 0.5)
        now = time.monotonic()
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
