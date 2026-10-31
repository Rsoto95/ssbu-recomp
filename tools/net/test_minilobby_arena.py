"""Test of the room / arena in the lobby that runs inside the game (runtime/net_minilobby.c), with fake players.

The lobby is built on its own (net_minilobby.c compiles alone with -DMINILOBBY_MAIN) and given here:
    clang -DMINILOBBY_MAIN -x c runtime/net_minilobby.c -o minilobby.exe -lws2_32
    python tools/net/test_minilobby_arena.py minilobby.exe

Four players join; one makes the room (its host), the others join the queue; the first two play (their rollback searches
are paired only with each other), the winner stays, the loser goes to the back; a draw sends both back; the host removes
a player; a player in the middle of its turn leaves (the other keeps the front); the host closes the room. Every step
checks the lobby's ARENA_STATE answers. Python 3.8+, standard library only."""
import os
import socket
import struct
import subprocess
import sys
import time

PORT = 39777
JOIN, WELCOME, PING, LEAVE, RB = 1, 2, 4, 7, 16
MATCH, MATCH_STATE, RESULT, ARENA, ARENA_STATE = 20, 21, 22, 24, 25
OUT, WAITING, TURN, PLAYING = range(4)
NAME = 32
fails = 0


def hdr(k):
    return b"SU\x01" + bytes((k,))


def name32(s):
    return s.encode()[:NAME].ljust(NAME, b"\0")


class Player:
    def __init__(self, name):
        self.name = name
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.s.settimeout(0.05)
        self.nonce = os.urandom(8)
        self.ip = self.cookie = None
        self.arena = None
        self.match = None

    def send(self, b):
        self.s.sendto(b, ("127.0.0.1", PORT))

    def join(self):
        p = hdr(JOIN) + self.nonce + name32("public") + name32(self.name) + b"ssbu-test".ljust(40, b"\0") + struct.pack(">IHB", 0x7F000001, 0, 0)
        for _ in range(40):
            self.send(p)
            self.pump()
            if self.cookie:
                return
        raise SystemExit("%s: no WELCOME" % self.name)

    def pump(self):
        while True:
            try:
                b, _ = self.s.recvfrom(65536)
            except socket.timeout:
                return
            except ConnectionResetError:
                continue
            if len(b) < 4 or b[:3] != b"SU\x01":
                continue
            k = b[3]
            if k == WELCOME and b[4:12] == self.nonce:
                self.ip, self.cookie = struct.unpack_from(">IQ", b, 12)
            elif k == ARENA_STATE:
                self.arena = parse_arena(b)
            elif k == MATCH_STATE:
                sid, st, role = struct.unpack_from(">IBB", b, 4)
                self.match = (st, role)
                if len(b) >= 4 + 49 + 7 and b[4 + 49] == ord("A"):      # the opponent's address (the match goes player to player)
                    self.peer_at = struct.unpack_from(">IH", b, 4 + 50)

    def ping(self):
        self.send(hdr(PING) + struct.pack(">Q", self.cookie) + b"\0" * 9)

    def arena_op(self, op, who=None):
        self.send(hdr(ARENA) + struct.pack(">QB", self.cookie, op) + (name32(who) if who else b""))

    def search(self, sid):
        self.send(hdr(MATCH) + struct.pack(">QBI", self.cookie, 2, sid))

    def rb_to(self, other):
        self.send(hdr(RB) + struct.pack(">QI", self.cookie, other.ip) + b"x" * 8)

    def result(self, seed, other, slot, s0, s1, d0=0, d1=0, flags=0):
        self.send(hdr(RESULT) + struct.pack(">QQIBBBBI", self.cookie, seed, other.ip, slot, 1, 3, flags, 0) + b"\0" * 16
                  + struct.pack(">BBHHII", s0, s1, d0, d1, 1000, 0))

    def leave(self):
        self.send(hdr(LEAVE) + struct.pack(">Q", self.cookie))


def parse_arena(b):
    o = 4
    ver, st, place, streak, limit = struct.unpack_from(">IBBBB", b, o); o += 8
    opp = b[o:o + NAME].rstrip(b"\0").decode(); o += NAME + 2
    match = None
    if b[o]:
        match = (b[o + 1:o + 1 + NAME].rstrip(b"\0").decode(), b[o + 1 + NAME:o + 1 + 2 * NAME].rstrip(b"\0").decode())
        o += 1 + 2 * NAME + 3
    else:
        o += 1
    n = b[o]; o += 1
    queue = []
    for _ in range(n):
        queue.append((b[o:o + NAME].rstrip(b"\0").decode(), b[o + NAME])); o += NAME + 1
    room = {"exists": False, "host": "", "me_host": False, "players": 0, "why": 0}
    if o < len(b) and b[o] == ord("H"):
        fl = b[o + 1]
        room = {"exists": bool(fl & 1), "me_host": bool(fl & 2), "host": b[o + 2:o + 2 + NAME].rstrip(b"\0").decode(),
                "players": b[o + 2 + NAME], "why": b[o + 3 + NAME]}
    return {"status": st, "place": place, "streak": streak, "limit": limit, "opp": opp, "match": match, "queue": [q[0] for q in queue], "room": room}


def settle(players, t=0.3):
    end = time.time() + t
    while time.time() < end:
        for p in players:
            p.pump()


def look(players):
    for p in players:
        p.arena_op(2)
    settle(players)


def check(cond, what):
    global fails
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        fails += 1


def main():
    exe = sys.argv[1]
    lobby = subprocess.Popen([exe, str(PORT)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.5)
    try:
        a, b, c, d = (Player(n) for n in ("Ana", "Beto", "Caro", "Dani"))
        everyone = [a, b, c, d]
        for p in everyone:
            p.join()
        look(everyone)
        check(all(p.arena["status"] == OUT and not p.arena["room"]["exists"] for p in everyone), "no room before anyone makes one")

        b.arena_op(1)                                    # joining without a room does nothing
        settle(everyone)
        check(b.arena["status"] == OUT, "joining when there is no room: still out")

        a.arena_op(3)                                    # Ana makes the room
        settle(everyone)
        check(a.arena["room"]["exists"] and a.arena["room"]["me_host"] and a.arena["status"] == WAITING, "Ana makes a room: its host, in the queue")
        check(all(p.arena and p.arena["room"]["exists"] and p.arena["room"]["host"] == "Ana" for p in (b, c, d)), "everyone is told: Ana's room")

        b.arena_op(3)                                    # a second room: refused
        settle(everyone)
        check(b.arena["room"]["host"] == "Ana" and not b.arena["room"]["me_host"], "a second room while Ana's is open: refused")

        for p in (b, c, d):
            p.arena_op(1)
            settle(everyone, 0.15)
        look(everyone)
        check(a.arena["status"] == TURN and b.arena["status"] == TURN and a.arena["opp"] == "Beto", "the first two (Ana, Beto) take their turn")
        check(c.arena["place"] == 1 and d.arena["place"] == 2 and a.arena["room"]["players"] == 4, "Caro next, Dani second; 4 players in the room")

        c.search(1)                                      # Caro searches early: not paired (not its turn)
        a.search(1); b.search(1)
        settle(everyone)
        check(a.match and b.match and a.match[0] == 2 and b.match[0] == 2 and a.match[1] == 1, "Ana and Beto paired, Ana is player 1")
        check(not c.match or c.match[0] != 2, "Caro (waiting) is not paired")
        check(getattr(a, "peer_at", None) == (0, b.s.getsockname()[1]) and getattr(b, "peer_at", None) == (0, a.s.getsockname()[1]),
              "each gets the other's address for a direct connection (0.0.0.0 = the lobby's PC, here loopback)")

        a.rb_to(b); b.rb_to(a)
        look(everyone)
        check(a.arena["status"] == PLAYING and c.arena["match"] == ("Ana", "Beto"), "rollback messages both ways: playing")

        a.result(0x1111, b, 0, 2, 0)                     # Ana (player 1) wins with 2 stocks left
        b.result(0x1111, a, 1, 2, 0)                     # (the second report of the same match counts no more)
        look(everyone)
        check(a.arena["status"] == TURN and a.arena["opp"] == "Caro" and a.arena["streak"] == 1, "Ana wins: she stays (1 in a row) against Caro")
        check(b.arena["status"] == WAITING and b.arena["place"] == 2, "Beto lost: to the back (after Dani)")

        a.search(2)                                      # (Caro's early search is still on: paired now that it is her turn)
        settle(everyone)
        check(c.match and c.match[0] == 2, "Caro's search from before her turn is paired when her turn comes")
        a.rb_to(c); c.rb_to(a)
        settle(everyone)
        a.result(0x2222, c, 0, 1, 1, 500, 500)           # a draw
        look(everyone)
        check(d.arena["status"] == TURN and b.arena["status"] == TURN and a.arena["status"] == WAITING and c.arena["status"] == WAITING,
              "a draw: both to the back, Dani and Beto take their turn")
        check(a.arena["streak"] == 0, "a draw ends Ana's streak")

        a.arena_op(5, "Dani")                            # the host removes Dani (in its turn): Beto keeps the front
        look(everyone)
        check(d.arena["status"] == OUT and d.arena["room"]["why"] == 1, "the host removed Dani: out, told why")
        check(b.arena["status"] == TURN and b.arena["opp"] in ("Ana", "Caro"), "Beto keeps the front and plays the next one")

        c.arena_op(5, "Beto")                            # not the host: nothing happens
        look(everyone)
        check(b.arena["status"] in (TURN, WAITING), "a player who is not the host cannot remove anyone")

        partner = a if b.arena["opp"] == "Ana" else c    # Beto (not the host) leaves the lobby in his turn: the other keeps the front
        b.leave()
        everyone.remove(b)
        look(everyone)
        check(partner.arena["status"] == TURN and partner.arena["opp"] != "Beto", "a player leaving the lobby in his turn: the other keeps the front and plays the next")
        check(all("Beto" not in p.arena["queue"] for p in everyone), "...and he is gone from the queue")

        a.arena_op(4)                                    # the host closes the room
        look(everyone)
        check(all(p.arena["status"] == OUT and not p.arena["room"]["exists"] for p in everyone), "the host closes the room: everyone out")
        check(c.arena["room"]["why"] == 2, "...and told the room closed")

        a.arena_op(3); c.arena_op(1); d.arena_op(1)      # a new room, and its host leaves the lobby
        look(everyone)
        a.leave()
        everyone.remove(a)
        look(everyone)
        check(all(p.arena["status"] == OUT and p.arena["room"]["why"] == 2 and not p.arena["room"]["exists"] for p in everyone), "the host left the lobby: the room closed for everyone")
    finally:
        lobby.terminate()
        try:
            out = lobby.communicate(timeout=3)[0]
        except Exception:
            out = ""
        print("---- the lobby's log:")
        print("\n".join(l for l in out.splitlines() if "arena" in l or "match" in l))
    print("FAILED: %d" % fails if fails else "PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
