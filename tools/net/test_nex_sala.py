"""Test of the salas of the arenas server (nex_server.py, 2026-11-21): rooms with several stations, one Battle Arena each.

No game and no network: the handlers are called as the server calls them, with fake clients, and the two datagrams of the
arena query port (SALA?, STATUS? version 2) are given to their functions.

    python tools/net/test_nex_sala.py

Python 3.8+, standard library only."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nex_server as nx      # noqa: E402

fails = 0


def check(what, ok, got=None):
    global fails
    if not ok:
        fails += 1
    print("%s %s%s" % ("ok  " if ok else "FAIL", what, "" if ok or got is None else ": %r" % (got,)))


class Req:
    def __init__(self, body, method=0):
        self.body, self.method = body, method


class Fake:
    """What a handler uses of a Client."""
    def __init__(self, pid, ip="10.0.0.%d"):
        self.pid, self.who, self.told, self.after_reply = pid, (ip % (pid % 250 + 1)) + ":5000", [], None

    def log(self, text):
        pass

    def notify(self, *a, **k):
        self.told.append((a, k))

    def done(self):
        if self.after_reply:
            f, self.after_reply = self.after_reply, None
            f()


def session(most=4):
    """A MatchmakeSession as a game sends it at the creation (the fields the server reads)."""
    gathering = struct.pack("<IQQHHIIII", 0, 0, 0, 2, most, 0, 0, 0, 0) + struct.pack("<H", 1) + b"\0"
    param = struct.pack("<I", 0)
    mm = (struct.pack("<I", 7) + struct.pack("<I", 0) + b"\1" + struct.pack("<I", 0) + struct.pack("<I", 0) +
          struct.pack("<I", 0) + b"\0" + struct.pack("<I", 0) + struct.pack("<I", 0) +
          b"\0" + struct.pack("<I", len(param)) + param + struct.pack("<Q", 0))
    return b"\0" + struct.pack("<I", len(gathering)) + gathering + b"\0" + struct.pack("<I", len(mm)) + mm


def create(c, most=4):
    out = nx.create_matchmake_session_with_param(c, Req(b"\0" * 5 + session(most) + b"\0" * 17))
    c.done()
    return struct.unpack_from("<I", out, 5)[0]


def code_of(c, gid):
    out = nx.matchmake_ext_55(c, Req(struct.pack("<I", gid)))
    n = struct.unpack_from("<H", out, 5)[0]
    return out[7:7 + n].rstrip(b"\0").decode()


def typed(c, code):
    return struct.unpack("<I", nx.matchmake_ext_56(c, Req(b"\0" * 5 + struct.pack("<H", len(code) + 1) + code.encode() + b"\0" + b"\0" * 8)))[0]


def join(c, gid):
    out = nx.join_matchmake_session_with_param(c, Req(b"\0" * 5 + struct.pack("<I", gid)))
    c.done()
    return out


def listed(c):
    out = nx.matchmake_ext_52(c, Req(b""))
    n, o, gids = struct.unpack_from("<I", out, 0)[0], 4, []
    for _ in range(n):
        gids.append(struct.unpack_from("<I", out, o + 5)[0])
        o = nx.matchmake_session_span(out, o)
    return gids


def sala(op, pid, code="", name=""):
    nm = name.encode()
    out = nx.sala_datagram(b"SALA?" + bytes((1, op)) + struct.pack("<Q", pid) + code.encode().ljust(5, b"\0") + bytes((len(nm),)) + nm)
    return out[6], out[7:12].rstrip(b"\0").decode(), out[12], out[13]


def status(pid):
    out = nx.status_v2(pid)
    assert out[:7] == b"SALAST!"
    code, k, o, rows = out[9:14].rstrip(b"\0").decode(), out[14], 15, []
    for _ in range(k):
        gid, n = struct.unpack_from("<IB", out, o)
        members, most, mask, people = struct.unpack_from("<BBBB", out, o + 10)
        o += 14
        names = []
        for _ in range(people):
            names.append((out[o], out[o + 1:o + 11].rstrip(b"\0").decode()))
            o += 11
        rows.append((n, gid, members, most, names))
    return code, rows


def where(pid):
    """The arena the arena query would answer for this player."""
    found = [(a.get("joined", {}).get(pid, 0.0), g) for g, a in nx.ARENAS.items() if pid in nx.arena_members(a)]
    return max(found)[1] if found else 0


def reset():
    nx.ARENAS.clear(), nx.SALAS.clear(), nx.MEMBER.clear(), nx.NAMES.clear()


A, B, C, D, E, F = (Fake(p) for p in (101, 102, 103, 104, 105, 106))

# ---- without a sala nothing changes
reset()
g1 = create(A)
c1 = code_of(A, g1)
check("an arena of no sala: a code of 5 characters", len(c1) == 5, c1)
check("asked twice: the same code", code_of(A, g1) == c1)
check("its code leads to it", typed(B, c1) == g1)
join(B, g1)
check("listed for everyone", listed(C) == [g1], listed(C))
check("no sala was made", not nx.SALAS and not nx.MEMBER)

# ---- a sala: made in F1, its stations made in the game
reset()
st, S, n_st, n_mem = sala(3, A.pid, name="Ana")
check("SALA? op 3 makes a sala", st == 0 and len(S) == 5 and n_st == 0, (st, S))
check("its maker's list is empty (no station yet)", listed(A) == [])
s1 = create(A)
check("the maker's arena is station 1", nx.ARENAS[s1].get("sala") == S and nx.ARENAS[s1].get("station") == 1)
check("the station shows the sala's code", code_of(A, s1) == S)
check("a stranger's list does not show it", listed(B) == [])
pub = create(F)
pc = code_of(F, pub)
check("an arena of no sala beside it: its own code", pc != S and len(pc) == 5)
check("a stranger sees the public arena only", listed(B) == [pub], listed(B))
check("the member sees its sala's station only", listed(A) == [s1], listed(A))

# ---- typing the sala's code: a member, sent to the station with one player
check("the code sends B to station 1 (one player waits)", typed(B, S) == s1)
join(B, s1)
check("B is a member (implicit)", nx.MEMBER.get(B.pid, {}).get("sala") == S and not nx.MEMBER[B.pid]["explicit"])
check("B's list: the sala's stations", listed(B) == [s1])
check("C typed it: station 1 again (the only one)", typed(C, S) == s1)
s2 = create(C)
check("C made an arena: station 2 of the sala", nx.ARENAS[s2].get("sala") == S and nx.ARENAS[s2].get("station") == 2)
check("station 2 shows the same code", code_of(C, s2) == S)
check("the list: both stations in order", listed(B) == [s1, s2], listed(B))
check("D typed it: station 2 (one player waits)", typed(D, S) == s2)
join(D, s2)
check("E typed it: the fewest members (station 1 has 2, station 2 has 2: the lower number)", typed(E, S) == s1)
join(E, s1)
check("the next: station 2 (2 members against 3)", typed(Fake(107), S) == s2)

# ---- one arena per player
check("B is in station 1", where(B.pid) == s1)
join(B, s2)
check("B changed station: out of station 1", B.pid not in nx.arena_members(nx.ARENAS[s1]) and where(B.pid) == s2)
check("station 1 counts 2 (A, E)", len(nx.arena_members(nx.ARENAS[s1])) == 2)
join(B, s1)
check("and back: the arena query answers station 1", where(B.pid) == s1 and B.pid not in nx.arena_members(nx.ARENAS[s2]))

# ---- a full station is the last choice
reset()
_, S, _, _ = sala(3, A.pid)
s1 = create(A, most=2)
join(B, s1)
check("a full station and nothing else: it is answered (the game says full)", typed(C, S) == s1)
sala(1, C.pid, S)
s2 = create(C, most=4)
join(D, s2)
join(E, s2)
check("station 1 full (2 of 2), station 2 has 3 of 4: station 2", typed(F, S) == s2)

# ---- a station's host leaves: the sala stays
reset()
_, S, _, _ = sala(3, A.pid, name="Ana")
s1 = create(A)
join(B, s1)
sala(0, B.pid, name="Beto")
code, rows = status(B.pid)
check("STATUS? 2 for a member: the sala and its station with names", code == S and rows == [(1, s1, 2, 4, [(0, "Ana"), (1, "Beto")])], (code, rows))
with nx.ARENAS_LOCK:
    nx.close_arena(s1, "its host left")
check("the station is gone, the sala is not", s1 not in nx.ARENAS and S in nx.SALAS and nx.SALAS[S]["stations"] == {})
check("B is still a member: an empty list", listed(B) == [])
check("the code answers no arena now", typed(B, S) == 0)
s3 = create(B)
check("B opens a station: number 1 again, the same code", nx.ARENAS[s3].get("station") == 1 and code_of(B, s3) == S)

# ---- the host of a station enters another: its own is closed
s4 = create(A)
check("A (a member) made station 2", nx.ARENAS[s4].get("station") == 2)
join(A, s3)
check("A entered station 1: the station it had made is closed", s4 not in nx.ARENAS and nx.SALAS[S]["stations"] == {1: s3})

# ---- leaving, and the time limits
check("SALA? op 2: out", sala(2, B.pid)[0] == 2 and B.pid not in nx.MEMBER)
check("SALA? op 1 with a wrong code: no such sala", sala(1, D.pid, "ZZZZZ")[0] == 1 and D.pid not in nx.MEMBER)
check("SALA? op 1: a member", sala(1, D.pid, S)[:2] == (0, S) and nx.MEMBER[D.pid]["explicit"])
check("SALA? op 0 without a code: the sala the server has it in", sala(0, D.pid)[:2] == (0, S))
check("SALA? of a player in no sala: status 2", sala(0, 999)[0] == 2)
import time as _t      # noqa: E402
now = _t.monotonic()
with nx.ARENAS_LOCK:
    nx.sala_sweep(now + nx.SALA_EXPLICIT_FOR + 1)
check("an explicit member that stopped saying so is dropped (D), one at a station is kept (A)", D.pid not in nx.MEMBER and A.pid in nx.MEMBER)
reset()
_, S, _, _ = sala(3, A.pid)
with nx.ARENAS_LOCK:
    nx.sala_sweep(now + nx.SALA_EXPLICIT_FOR + 1)
    nx.sala_sweep(now + nx.SALA_EXPLICIT_FOR + 2)
    kept = S in nx.SALAS
    nx.sala_sweep(now + nx.SALA_EXPLICIT_FOR + nx.SALA_EMPTY_FOR + 10)
check("an empty sala is kept for a while, then dropped", kept and S not in nx.SALAS)

# ---- the datagrams
reset()
g = create(A)
code_of(A, g)
code, rows = status(B.pid)
check("STATUS? 2 for a player in no sala: the public arenas, station number 0", code == "" and [r[:2] for r in rows] == [(0, g)], (code, rows))
check("SALA? is never 16 bytes (the NAT check's length)", len(b"SALA?" + bytes(2) + bytes(8) + bytes(5) + bytes(1)) == 21)
check("STATUS? 2 is 16 bytes and begins with STATUS?", len(b"STATUS?" + bytes((2,)) + bytes(8)) == 16)
for i in range(60):                                  # many arenas: the answer stays under its size
    c = Fake(1000 + i)
    create(c)
check("STATUS? 2 with 61 arenas: at most 1200 bytes", len(nx.status_v2(B.pid)) <= 1200, len(nx.status_v2(B.pid)))
reset()
for i in range(nx.SALA_MOST):
    sala(3, 2000 + i)
check("no more salas than SALA_MOST", sala(3, 5000)[0] == 3 and len(nx.SALAS) == nx.SALA_MOST)

print("%d failed" % fails if fails else "all passed")
sys.exit(1 if fails else 0)
