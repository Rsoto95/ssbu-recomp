/* net_minilobby.c: a lobby inside the game, so LAN mode and direct-over-internet mode need no server of ours.
 * C port of tools_net/ssbu_lobby.py (same wire protocol "SU" version 1, see the top of net_relay.c); design adapted
 * from SNO2207/WindowSmash, used with the author's permission.
 * #included by net_relay.c: only static functions and data (every name starts with ml_ / ML_ / minilobby_).
 *   minilobby_start / minilobby_stop / minilobby_counts / minilobby_socket / minilobby_open_to (see below).
 * Standalone test: clang -DMINILOBBY_MAIN -x c net_minilobby.c -o minilobby.exe -lws2_32
 *                  minilobby.exe <port> [1 = no PEERS]  */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

enum { ML_PROTO = 1, ML_JOIN = 1, ML_WELCOME, ML_REJECT, ML_PING, ML_PONG, ML_DATA, ML_LEAVE, ML_LIST, ML_LISTING,
       ML_LIST_END, ML_PEERS, ML_PUNCH, ML_DIRECT, ML_BCAST };
enum { ML_ROOMS = 17, ML_ROOM_INFO, ML_ROOMS_END };     /* O1 room browser (net_relay.c head, ssbu_lobby.py on_rooms) */
enum { ML_MATCH = 20, ML_MATCH_STATE };                 /* O2 matchmaking (net_relay.c head, ssbu_lobby.py on_match) */
enum { ML_MS_IDLE, ML_MS_SEARCHING, ML_MS_MATCHED, ML_MS_TIMEOUT, ML_MS_PEER_LEFT, ML_MS_TOGETHER, ML_MS_EXPIRED };
enum { ML_KIND_ROOM = 1, ML_KIND_ROLLBACK = 2 };        /* (R3 net3, lobby 1.4) MATCH op 1 / op 2: a Local Wireless search, a rollback-match search */
enum { ML_RESULT = 22, ML_RESULT_ACK, ML_ARENA, ML_ARENA_STATE };     /* (1.6 arena) the RESULT of a rollback match, the arena (ssbu_lobby.py docstring) */
enum { ML_AR_OUT, ML_AR_WAITING, ML_AR_TURN, ML_AR_PLAYING };
#define ML_ARENA_TURN_FOR 120000                        /* ms: a turn not started in this time (ssbu_lobby.py ARENA_TURN_FOR) */
#define ML_ARENA_PLAY_FOR 900000                        /* ms: a match without a result */
#define ML_ARENA_WATCH_FOR 3000                         /* ms: a game that asked to look gets the changes this long */
#define ML_ARENA_LIST 16                                /* queue entries in ARENA_STATE */
#define ML_ARENAS 8                                     /* arenas (one per build and lobby code) */
#define ML_RESULT_LEN 62
#define ML_SEARCH_FOR 120000                            /* ms; SSBU_LOBBY_SEARCH_SECS / SSBU_LOBBY_MATCH_SECS change them */
#define ML_MATCH_FOR 180000
#define ML_MODS_MAX 32
#define ML_MOD_NAME 31
#define ML_MODS_BYTES (1 + ML_MODS_MAX * (5 + ML_MOD_NAME))
#define ML_ROOMS_MAX 32
enum { ML_M_ADVERTISE = 1, ML_M_JOIN, ML_M_ACCEPT, ML_M_REJECT, ML_M_KEEP, ML_M_LEAVE, ML_M_GONE, ML_M_KICK };
#define ML_NAME_LEN 32
#define ML_BUILD_LEN 40
#define ML_JOIN_LEN (4 + 8 + 2 * ML_NAME_LEN + ML_BUILD_LEN + 4 + 2 + 1)
#define ML_SUBNET 0x0A0D0000u
#define ML_BCAST0 0x0A0DFFFFu
#define ML_BCAST1 0xFFFFFFFFu
#define ML_LDN_PORT 65511
#define ML_NI_SIZE 0x480
#define ML_NI_SESSION 0x10
#define ML_NI_POLICY 0x62
#define ML_NI_NODE_MAX 0x66
#define ML_NI_NODE_COUNT 0x67
#define ML_NI_NODES 0x68
#define ML_NODE_LEN 0x40
#define ML_LIST_MAX 24
#define ML_CLIENT_TIMEOUT 10000
#define ML_ROOM_TIMEOUT 10000
#define ML_LISTED_FOR 4000
#define ML_MAX_DATA 1500
#define ML_MAX_CLIENTS 256
#define ML_MAX_ROOMS 256
#define ML_PEER_LEN 56   /* ip4 name32 pubip4 port2 lanip4 lanport2 secret8 */
#define ML_OPEN_MAX 8

typedef struct ml_room ml_room;
typedef struct ml_client {
    int used;
    struct sockaddr_in at;
    uint8_t nonce[8];
    uint32_t ip;
    char name[ML_NAME_LEN + 1], build[ML_BUILD_LEN + 1], code[ML_NAME_LEN + 1];
    uint32_t lan_ip;
    uint16_t lan_port;
    int direct;
    uint64_t cookie;
    ULONGLONG seen;
    ml_room *room;            /* the room this player hosts */
    int has_claim;
    uint8_t claim[16];        /* session id of the room it asked to join */
    uint32_t peers_version;
    int peers_valid, peers_len;
    uint8_t peers_sent[7 * ML_PEER_LEN];
    int mods_len;             /* O1: its mod-set block (0: an older game that does not send it) */
    uint8_t mods[ML_MODS_BYTES];
    /* O2 matchmaking */
    int has_search, searching, room_said;
    uint32_t search_id;
    ULONGLONG search_since, match_at;
    struct ml_client *mpeer;  /* the player it is matched with (NULL: none) */
    int mrole;                /* 1 it hosts, 2 it joins */
    int has_final;            /* how its last search ended (sent again when asked about that search) */
    uint32_t final_id, final_peer;
    int final_state, final_role;
    char final_name[ML_NAME_LEN + 1];
    int kind;                 /* (1.4) what it searches: ML_KIND_ROOM / ML_KIND_ROLLBACK */
    uint32_t rb_peer;         /* (1.4) the player of its last rollback match (RB to it relayed on the public code) */
    int rb_seen;              /* (1.4, rollback match) 1 = this one's RB to its peer was relayed */
    int arena_streak;         /* (1.6 arena) wins in a row */
    ULONGLONG arena_seen;     /* (1.6 arena) last ARENA from it (0: never): it looks at the arena, send it the changes */
    int arena_why;            /* (room) why it was last taken out: 0 none, 1 removed by the host, 2 the room closed, 3 its turn not started in time */
} ml_client;

struct ml_room {
    int used;
    ml_client *owner;
    uint8_t session[16];
    uint8_t info[ML_NI_SIZE];
    uint32_t nodes[7];        /* station addresses (node 0 is the owner) */
    int nnodes;
    ULONGLONG created, updated;
    struct { uint32_t a, b; uint64_t s; } pairs[64];
    int npairs, pnext;
};

static ml_client ml_clients[ML_MAX_CLIENTS];
static ml_room ml_rooms[ML_MAX_ROOMS];
static uint32_t ml_next_ip = ML_SUBNET + 2;
static unsigned long long ml_relayed, ml_ldn_msgs, ml_dropped;
static CRITICAL_SECTION ml_lock;
static int ml_lock_ready;
static SOCKET ml_sock = INVALID_SOCKET;
static volatile LONG ml_running;
static HANDLE ml_thread;
static int ml_no_peers;
static struct { struct sockaddr_in at; int used; } ml_open[ML_OPEN_MAX];
static uint64_t ml_rng_state;
static ULONGLONG ml_search_for = ML_SEARCH_FOR, ml_match_for = ML_MATCH_FOR;

#define ml_log(...) do { printf("[online] lobby: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/* ---- small helpers */
static uint32_t ml_get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t ml_get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t ml_get64(const uint8_t *p) { return (uint64_t)ml_get32(p) << 32 | ml_get32(p + 4); }
static uint8_t *ml_put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; return p + 4; }
static uint8_t *ml_put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = (uint8_t)v; return p + 2; }
static uint8_t *ml_put64(uint8_t *p, uint64_t v) { return ml_put32(ml_put32(p, (uint32_t)(v >> 32)), (uint32_t)v); }
static uint8_t *ml_hdr(uint8_t *p, int kind) { p[0] = 'S'; p[1] = 'U'; p[2] = ML_PROTO; p[3] = (uint8_t)kind; return p + 4; }

static void ml_text(char *dst, size_t cap, const uint8_t *raw, size_t n) {   /* NUL-terminated string field */
    size_t i = 0;
    while (i < n && i + 1 < cap && raw[i]) { dst[i] = (char)raw[i]; i++; }
    dst[i] = 0;
}

static uint64_t ml_random64(void) {
    static BOOLEAN (WINAPI *rtlgen)(PVOID, ULONG);
    static int tried;
    uint64_t v = 0;
    if (!tried) {
        HMODULE m = LoadLibraryA("advapi32.dll");
        rtlgen = m ? (BOOLEAN (WINAPI *)(PVOID, ULONG))GetProcAddress(m, "SystemFunction036") : NULL;
        ml_rng_state = GetTickCount64() * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId();
        tried = 1;
    }
    if (rtlgen && rtlgen(&v, sizeof v)) return v;
    ml_rng_state ^= ml_rng_state << 13; ml_rng_state ^= ml_rng_state >> 7; ml_rng_state ^= ml_rng_state << 17;
    return ml_rng_state;
}

static const char *ml_vip(uint32_t ip) {
    static char b[4][24];
    static int i;
    char *s = b[i++ & 3];
    snprintf(s, 24, "10.13.%u.%u", ip >> 8 & 255, ip & 255);
    return s;
}

static int ml_same_at(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static int ml_is_bcast(uint32_t ip) { return ip == ML_BCAST0 || ip == ML_BCAST1; }
static int ml_same_part(const ml_client *a, const ml_client *b) { return !strcmp(a->build, b->build) && !strcmp(a->code, b->code); }

static void ml_send(const void *data, int len, const struct sockaddr_in *at) {
    if (ml_sock != INVALID_SOCKET) sendto(ml_sock, (const char *)data, len, 0, (const struct sockaddr *)at, sizeof *at);
}

/* ---- tables */
static ml_client *ml_by_at(const struct sockaddr_in *at) {
    for (int i = 0; i < ML_MAX_CLIENTS; i++) if (ml_clients[i].used && ml_same_at(&ml_clients[i].at, at)) return &ml_clients[i];
    return NULL;
}
static ml_client *ml_by_ip(uint32_t ip) {
    for (int i = 0; i < ML_MAX_CLIENTS; i++) if (ml_clients[i].used && ml_clients[i].ip == ip) return &ml_clients[i];
    return NULL;
}
static ml_room *ml_find_room(const uint8_t *session) {
    for (int i = 0; i < ML_MAX_ROOMS; i++) if (ml_rooms[i].used && !memcmp(ml_rooms[i].session, session, 16)) return &ml_rooms[i];
    return NULL;
}
static int ml_has_node(const ml_room *r, uint32_t ip) {
    for (int i = 0; i < r->nnodes; i++) if (r->nodes[i] == ip) return 1;
    return 0;
}
static int ml_members(const ml_room *r, ml_client **out) {   /* the owner, then the players that claimed this room */
    int n = 0;
    out[n++] = r->owner;
    for (int i = 0; i < r->nnodes; i++) {
        ml_client *c = ml_by_ip(r->nodes[i]);
        if (c && c->has_claim && !memcmp(c->claim, r->session, 16)) out[n++] = c;
    }
    return n;
}
static ml_room *ml_room_of(ml_client *c) {
    if (c->room) return c->room;
    ml_room *r = c->has_claim ? ml_find_room(c->claim) : NULL;
    return r && ml_has_node(r, c->ip) ? r : NULL;
}
static uint32_t ml_new_ip(void) {
    for (int i = 0; i < 0x10000; i++) {
        uint32_t ip = ml_next_ip;
        ml_next_ip = ip >= ML_SUBNET + 0xFFFE ? ML_SUBNET + 2 : ip + 1;
        if ((ip & 0xFF) != 0 && (ip & 0xFF) != 255 && !ml_by_ip(ip)) return ip;
    }
    return 0;
}

/* ---- direct paths: each member of a room learns the others' addresses and a pair secret */
static void ml_send_peers(ml_client *c) {
    if (ml_no_peers) return;
    ml_room *room = ml_room_of(c);
    uint8_t body[7 * ML_PEER_LEN], pkt[9 + sizeof body];
    int n = 0;
    if (room && c->direct) {
        ml_client *mem[8];
        int cnt = ml_members(room, mem);
        for (int i = 0; i < cnt; i++) {
            ml_client *m = mem[i];
            if (m == c || !m->direct || n >= 7) continue;
            uint32_t a = c->ip < m->ip ? c->ip : m->ip, b = c->ip < m->ip ? m->ip : c->ip;
            uint64_t secret = 0;
            for (int k = 0; k < room->npairs; k++) if (room->pairs[k].a == a && room->pairs[k].b == b) secret = room->pairs[k].s;
            if (!secret) {
                int slot = room->npairs < 64 ? room->npairs++ : room->pnext++ & 63;
                room->pairs[slot].a = a; room->pairs[slot].b = b;
                secret = room->pairs[slot].s = ml_random64() | 1;
            }
            uint8_t *p = body + n * ML_PEER_LEN;
            memset(p, 0, ML_PEER_LEN);
            ml_put32(p, m->ip);
            memcpy(p + 4, m->name, strlen(m->name));
            ml_put32(p + 36, ntohl(m->at.sin_addr.s_addr));
            ml_put16(p + 40, ntohs(m->at.sin_port));
            ml_put32(p + 42, m->lan_ip);
            ml_put16(p + 46, m->lan_port);
            ml_put64(p + 48, secret);
            n++;
        }
    }
    int blen = n * ML_PEER_LEN;
    if (c->peers_valid && c->peers_len == blen && !memcmp(c->peers_sent, body, blen)) return;
    c->peers_valid = 1; c->peers_len = blen;
    memcpy(c->peers_sent, body, blen);
    uint8_t *p = ml_put32(ml_hdr(pkt, ML_PEERS), ++c->peers_version);
    *p++ = (uint8_t)n;
    memcpy(p, body, blen);
    ml_send(pkt, (int)(p + blen - pkt), &c->at);
}

static void ml_close_room(ml_room *room, const char *why) {
    uint8_t gone[4 + 12 + 4 + 16], *p;
    ml_client *mem[8];
    int cnt = ml_members(room, mem);
    p = ml_hdr(gone, ML_DATA);
    p = ml_put32(p, room->owner->ip); p = ml_put32(p, ML_BCAST0); p = ml_put16(p, ML_LDN_PORT); p = ml_put16(p, ML_LDN_PORT);
    *p++ = 'L'; *p++ = ML_M_GONE; *p++ = 0; *p++ = 0;
    memcpy(p, room->session, 16);
    for (int i = 1; i < cnt; i++) {
        ml_send(gone, sizeof gone, &mem[i]->at);
        mem[i]->has_claim = 0;
        ml_send_peers(mem[i]);
    }
    ml_client *owner = room->owner;
    if (owner->room == room) owner->room = NULL;
    room->used = 0;
    if (owner->used) ml_send_peers(owner);
    ml_log("room of %s closed (%s)", owner->name, why);
}

static void ml_match_end(ml_client *c, int state, int role, const ml_client *peer, const char *why, ULONGLONG now);
static void ml_arena_drop(ml_client *c, const char *why, ULONGLONG now);
static void ml_drop(ml_client *c, const char *why) {
    ml_arena_drop(c, why, GetTickCount64());            /* (1.6: its turn goes to the next, or its place in the queue; a host closes its room) */
    if (c->mpeer) {
        ml_client *o = c->mpeer;
        char w[96];
        c->mpeer = NULL;
        snprintf(w, sizeof w, "%s left the lobby", c->name);
        ml_match_end(o, ML_MS_PEER_LEFT, o->mrole, c, w, GetTickCount64());
    }
    c->searching = 0;
    if (c->room) ml_close_room(c->room, "host left");
    if (c->has_claim) {
        ml_room *r = ml_find_room(c->claim);
        if (r && ml_has_node(r, c->ip)) {   /* tell its host at once */
            uint8_t pkt[4 + 12 + 4 + 16], *p = ml_hdr(pkt, ML_DATA);
            p = ml_put32(p, c->ip); p = ml_put32(p, r->owner->ip); p = ml_put16(p, ML_LDN_PORT); p = ml_put16(p, ML_LDN_PORT);
            *p++ = 'L'; *p++ = ML_M_LEAVE; *p++ = 0; *p++ = 0;
            memcpy(p, r->session, 16);
            ml_send(pkt, sizeof pkt, &r->owner->at);
        }
    }
    c->used = 0;
    ml_log("%s (%s) left: %s", c->name, ml_vip(c->ip), why);
}

/* ---- packets */
static void ml_on_join(const uint8_t *p, int len, const struct sockaddr_in *at, ml_client *c, ULONGLONG now) {
    if (len < ML_JOIN_LEN) return;
    const uint8_t *nonce = p + 4;
    char code[ML_NAME_LEN + 1], name[ML_NAME_LEN + 1], build[ML_BUILD_LEN + 1];
    ml_text(code, sizeof code, p + 12, 32);
    ml_text(name, sizeof name, p + 44, 32);
    if (!name[0]) snprintf(name, sizeof name, "Player");
    ml_text(build, sizeof build, p + 76, 40);
    uint32_t lan_ip = ml_get32(p + 116);
    uint16_t lan_port = ml_get16(p + 120);
    int direct = p[122] & 1;
    if (c && memcmp(c->nonce, nonce, 8)) { ml_drop(c, "restarted"); c = NULL; }
    if (!c) {   /* the same game from a new port (its router remapped it)? */
        for (int i = 0; i < ML_MAX_CLIENTS; i++) {
            ml_client *o = &ml_clients[i];
            if (o->used && !memcmp(o->nonce, nonce, 8) && !strcmp(o->build, build) && !strcmp(o->code, code)) { o->at = *at; c = o; break; }
        }
    }
    if (!c) {
        uint32_t ip = ml_new_ip();
        for (int i = 0; ip && i < ML_MAX_CLIENTS && !c; i++) if (!ml_clients[i].used) c = &ml_clients[i];
        if (!c) {
            uint8_t rej[4 + 8 + 64], *r = ml_hdr(rej, ML_REJECT);
            memset(r, 0, 8 + 64);
            memcpy(r, nonce, 8);
            memcpy(r + 8, "the lobby is full", 17);
            ml_send(rej, sizeof rej, at);
            return;
        }
        memset(c, 0, sizeof *c);
        c->used = 1; c->at = *at; c->ip = ip; c->cookie = ml_random64();
        memcpy(c->nonce, nonce, 8);
        snprintf(c->name, sizeof c->name, "%s", name); snprintf(c->build, sizeof c->build, "%s", build); snprintf(c->code, sizeof c->code, "%s", code);
        ml_log("%s joined (code '%s', build %s) as %s from %u.%u.%u.%u:%d, LAN %u.%u.%u.%u:%u, direct %s", name, code, build, ml_vip(ip),
               at->sin_addr.S_un.S_un_b.s_b1, at->sin_addr.S_un.S_un_b.s_b2, at->sin_addr.S_un.S_un_b.s_b3, at->sin_addr.S_un.S_un_b.s_b4, ntohs(at->sin_port), lan_ip >> 24, lan_ip >> 16 & 255, lan_ip >> 8 & 255, lan_ip & 255,
               lan_port, direct ? "yes" : "no");
    }
    c->seen = now; c->lan_ip = lan_ip; c->lan_port = lan_port; c->direct = direct;
    if (len > ML_JOIN_LEN + 2 && p[ML_JOIN_LEN] == 'M' && p[ML_JOIN_LEN + 1] == 'S') {      /* O1: the game's mods */
        const uint8_t *m = p + ML_JOIN_LEN + 2;
        int n = len - ML_JOIN_LEN - 2, o = 1, ok = n >= 1 && m[0] <= ML_MODS_MAX;
        for (int i = 0; ok && i < m[0]; i++) {
            if (o + 5 > n || m[o + 4] > ML_MOD_NAME || o + 5 + m[o + 4] > n) ok = 0;
            else o += 5 + m[o + 4];
        }
        c->mods_len = ok ? o : 0;
        if (ok) memcpy(c->mods, m, (size_t)o);
    }
    uint8_t w[4 + 8 + 4 + 8], *q = ml_hdr(w, ML_WELCOME);
    memcpy(q, nonce, 8);
    q = ml_put64(ml_put32(q + 8, c->ip), c->cookie);
    ml_send(w, sizeof w, at);
}

static int ml_room_rank(const ml_room *r) {   /* open rooms first, then fuller ones */
    int open = r->info[ML_NI_POLICY] != 1 && r->info[ML_NI_NODE_COUNT] < r->info[ML_NI_NODE_MAX];
    return (open ? 0 : 1000) - r->info[ML_NI_NODE_COUNT];
}

/* the rooms c may see, best first: other = 0 its own build only (the game's list of an older game), 1 every build of its
 * lobby code (O1: a newer game's list, and the room browser), its own build's rooms first */
static int ml_select(ml_client *c, ULONGLONG now, int other, int with_own, ml_room **sel, int max) {
    int n = 0;
    const ml_client *mhost = c->mpeer && c->mrole == 2 ? c->mpeer : NULL;     /* O2: the matched host's room first */
    for (int i = 0; i < ML_MAX_ROOMS; i++) {
        ml_room *r = &ml_rooms[i];
        if (!r->used || (r->owner == c && !with_own) || now - r->updated > ML_LISTED_FOR) continue;
        if (other ? strcmp(r->owner->code, c->code) != 0 : !ml_same_part(r->owner, c)) continue;
        int k = n < max ? n++ : max;   /* insertion sort keeping the best max */
        sel[k] = r;
        while (k > 0) {
            int ra = ml_room_rank(sel[k]) + (ml_same_part(sel[k]->owner, c) ? 0 : 10000) - (sel[k]->owner == mhost ? 100000 : 0),
                rb = ml_room_rank(sel[k - 1]) + (ml_same_part(sel[k - 1]->owner, c) ? 0 : 10000) - (sel[k - 1]->owner == mhost ? 100000 : 0);
            if (!(ra < rb || (ra == rb && sel[k]->created < sel[k - 1]->created))) break;
            ml_room *t = sel[k]; sel[k] = sel[k - 1]; sel[k - 1] = t; k--;
        }
    }
    return n;
}

static void ml_on_list(ml_client *c, const uint8_t *seq, ULONGLONG now, int other) {
    ml_room *sel[ML_LIST_MAX + 1];
    int n = ml_select(c, now, other, 0, sel, ML_LIST_MAX);
    for (int i = 0; i < n; i++) {
        uint8_t pkt[4 + 4 + 4 + ML_NI_SIZE + ML_BUILD_LEN], *p = ml_hdr(pkt, ML_LISTING);
        memcpy(p, seq, 4);
        ml_put32(p + 4, sel[i]->owner->ip);
        memcpy(p + 8, sel[i]->info, ML_NI_SIZE);
        memset(p + 8 + ML_NI_SIZE, 0, ML_BUILD_LEN);       /* O1: the host's build (only asked with flag 1) */
        memcpy(p + 8 + ML_NI_SIZE, sel[i]->owner->build, strlen(sel[i]->owner->build));
        ml_send(pkt, other ? (int)sizeof pkt : (int)sizeof pkt - ML_BUILD_LEN, &c->at);
    }
    uint8_t end[4 + 4 + 1], *p = ml_hdr(end, ML_LIST_END);
    memcpy(p, seq, 4);
    p[4] = (uint8_t)n;
    ml_send(end, sizeof end, &c->at);
    if (n) ml_log("%s scanned: %d room(s)", c->name, n);
}

static void ml_on_rooms(ml_client *c, const uint8_t *seq, ULONGLONG now) {     /* O1: the room browser */
    ml_room *sel[ML_ROOMS_MAX + 1];
    int n = ml_select(c, now, 1, 1, sel, ML_ROOMS_MAX);
    for (int i = 0; i < n; i++) {
        ml_room *r = sel[i];
        ml_client *o = r->owner;
        uint8_t pkt[4 + 4 + 4 + 3 + ML_NAME_LEN + ML_BUILD_LEN + ML_MODS_BYTES], *p = ml_hdr(pkt, ML_ROOM_INFO);
        int open = r->info[ML_NI_POLICY] != 1 && r->info[ML_NI_NODE_COUNT] < r->info[ML_NI_NODE_MAX];
        memcpy(p, seq, 4);
        ml_put32(p + 4, o->ip);
        p[8] = r->info[ML_NI_NODE_COUNT]; p[9] = r->info[ML_NI_NODE_MAX];
        p[10] = (uint8_t)((open ? 1 : 0) | (o == c ? 2 : 0) | (o->mods_len ? 4 : 0));
        memset(p + 11, 0, ML_NAME_LEN + ML_BUILD_LEN);
        memcpy(p + 11, o->name, strlen(o->name));
        memcpy(p + 11 + ML_NAME_LEN, o->build, strlen(o->build));
        uint8_t *m = p + 11 + ML_NAME_LEN + ML_BUILD_LEN;
        if (o->mods_len) { memcpy(m, o->mods, (size_t)o->mods_len); m += o->mods_len; } else *m++ = 0;
        ml_send(pkt, (int)(m - pkt), &c->at);
    }
    uint8_t end[4 + 4 + 1], *p = ml_hdr(end, ML_ROOMS_END);
    memcpy(p, seq, 4);
    p[4] = (uint8_t)n;
    ml_send(end, sizeof end, &c->at);
}

/* ---- O2 matchmaking (ssbu_lobby.py match_send / match_end / on_match / match_room) */
static int ml_room_open(const ml_room *r, ULONGLONG now) {
    return r && r->info[ML_NI_POLICY] != 1 && r->info[ML_NI_NODE_COUNT] < r->info[ML_NI_NODE_MAX] && now - r->updated <= ML_LISTED_FOR;
}
/* ---- (1.6) the arena: C port of ssbu_lobby.py (class Arena, on_arena .. arena_result), same messages, so a group playing
 * in LAN mode (this lobby inside the first player's game) can take turns too (2026-10-30: 4-5 friends over Radmin). The
 * players of one build and lobby code who joined wait in a queue; the first two play a 1-on-1 rollback match (Smash versus:
 * their MATCH op 2 searches are paired only with each other, the one ahead is player 1); the winner stays (unless
 * SSBU_ARENA_STREAK wins in a row; default 0: no limit), the loser goes to the back. */
typedef struct {
    int used;
    char build[ML_BUILD_LEN + 1], code[ML_NAME_LEN + 1];
    ml_client *queue[ML_MAX_CLIENTS]; int nq;           /* waiting, the next first */
    ml_client *pair[2];                                 /* taking their turn (pair[0] NULL: none) */
    int playing;                                        /* the pair's games exchange rollback messages */
    ml_client *host;                                    /* (room) the player who made it: it controls it; the arena exists while it does */
    ULONGLONG at;                                       /* since when the pair is in its state */
    uint32_t version;
    uint64_t last_seed;                                 /* the last match whose result was taken (both games send it) */
} ml_arena;
static ml_arena ml_arenas[ML_ARENAS];
static int ml_arena_streak;

static ml_arena *ml_arena_of(const ml_client *c, int make) {
    ml_arena *fr = NULL;
    for (int i = 0; i < ML_ARENAS; i++) {
        ml_arena *a = &ml_arenas[i];
        if (a->used && !strcmp(a->build, c->build) && !strcmp(a->code, c->code)) return a;
        if (!a->used && !fr) fr = a;
    }
    if (!make || !fr) return NULL;
    memset(fr, 0, sizeof *fr);
    fr->used = 1;
    snprintf(fr->build, sizeof fr->build, "%s", c->build); snprintf(fr->code, sizeof fr->code, "%s", c->code);
    return fr;
}
static int ml_arena_in_pair(const ml_arena *a, const ml_client *c) { return a && a->pair[0] && (a->pair[0] == c || a->pair[1] == c); }
static int ml_arena_qpos(const ml_arena *a, const ml_client *c) { for (int i = 0; a && i < a->nq; i++) if (a->queue[i] == c) return i; return -1; }
static int ml_arena_member(const ml_arena *a, const ml_client *c) { return ml_arena_in_pair(a, c) || ml_arena_qpos(a, c) >= 0; }
static void ml_arena_qdel(ml_arena *a, int i) { memmove(a->queue + i, a->queue + i + 1, (size_t)(a->nq - i - 1) * sizeof *a->queue); a->nq--; }
static void ml_arena_qback(ml_arena *a, ml_client *c) { if (a->nq < ML_MAX_CLIENTS) a->queue[a->nq++] = c; }
static void ml_arena_qfront(ml_arena *a, ml_client *c) {
    if (a->nq >= ML_MAX_CLIENTS) return;
    memmove(a->queue + 1, a->queue, (size_t)a->nq * sizeof *a->queue); a->queue[0] = c; a->nq++;
}
static ml_client *ml_arena_other(const ml_arena *a, const ml_client *c) { return a->pair[0] == c ? a->pair[1] : a->pair[0]; }
static int ml_arena_status(const ml_arena *a, const ml_client *c) {
    if (ml_arena_in_pair(a, c)) return a->playing ? ML_AR_PLAYING : ML_AR_TURN;
    return ml_arena_qpos(a, c) >= 0 ? ML_AR_WAITING : ML_AR_OUT;
}
static uint8_t *ml_arena_name(uint8_t *p, const ml_client *c) { memset(p, 0, ML_NAME_LEN); memcpy(p, c->name, strlen(c->name)); return p + ML_NAME_LEN; }
/* ARENA_STATE: version u32, status u8, place u8, streak u8, streak limit u8, opponent name[32], seconds u16, match count u8
 * (+ name[32] x 2, streak u8 x 2, state u8), queue count u8 + count x (name[32], streak u8) */
static void ml_arena_send(ml_client *c, ULONGLONG now) {
    uint8_t pkt[4 + 8 + ML_NAME_LEN + 2 + 1 + 2 * ML_NAME_LEN + 3 + 1 + ML_ARENA_LIST * (ML_NAME_LEN + 1) + 4 + ML_NAME_LEN], *p = ml_hdr(pkt, ML_ARENA_STATE);
    ml_arena *a = ml_arena_of(c, 0);
    int st = ml_arena_status(a, c), place = 0;
    ULONGLONG since = now;
    const ml_client *opp = NULL;
    if (st == ML_AR_WAITING) place = ml_arena_qpos(a, c) + 1;
    else if (st == ML_AR_TURN || st == ML_AR_PLAYING) { opp = ml_arena_other(a, c); since = a->at; }
    p = ml_put32(p, a ? a->version : 0);
    *p++ = (uint8_t)st; *p++ = (uint8_t)(place > 255 ? 255 : place);
    *p++ = (uint8_t)(c->arena_streak > 255 ? 255 : c->arena_streak); *p++ = (uint8_t)(ml_arena_streak > 255 ? 255 : ml_arena_streak);
    memset(p, 0, ML_NAME_LEN); if (opp) memcpy(p, opp->name, strlen(opp->name)); p += ML_NAME_LEN;
    { ULONGLONG s = (now - since) / 1000; p = ml_put16(p, s > 65535 ? 65535u : (unsigned)s); }
    if (a && a->pair[0]) {
        *p++ = 1;
        p = ml_arena_name(p, a->pair[0]); p = ml_arena_name(p, a->pair[1]);
        *p++ = (uint8_t)(a->pair[0]->arena_streak > 255 ? 255 : a->pair[0]->arena_streak);
        *p++ = (uint8_t)(a->pair[1]->arena_streak > 255 ? 255 : a->pair[1]->arena_streak);
        *p++ = (uint8_t)(a->playing ? ML_AR_PLAYING : ML_AR_TURN);
    } else *p++ = 0;
    int n = a ? (a->nq < ML_ARENA_LIST ? a->nq : ML_ARENA_LIST) : 0;
    *p++ = (uint8_t)n;
    for (int i = 0; i < n; i++) { p = ml_arena_name(p, a->queue[i]); *p++ = (uint8_t)(a->queue[i]->arena_streak > 255 ? 255 : a->queue[i]->arena_streak); }
    /* (room) 'H', flags u8 (1 a room exists, 2 this player is its host), the host's name[32], players in the room u8, why this
     * player was last taken out u8 (0 none, 1 removed by the host, 2 the room closed, 3 its turn not started in time) */
    *p++ = 'H';
    *p++ = (uint8_t)((a && a->host ? 1 : 0) | (a && a->host == c ? 2 : 0));
    if (a && a->host) p = ml_arena_name(p, a->host); else { memset(p, 0, ML_NAME_LEN); p += ML_NAME_LEN; }
    *p++ = (uint8_t)(a ? a->nq + (a->pair[0] ? 2 : 0) : 0);
    *p++ = (uint8_t)c->arena_why;
    ml_send(pkt, (int)(p - pkt), &c->at);
}
static void ml_arena_changed(ml_arena *a, ULONGLONG now) {      /* to its players, and whoever looks at it */
    a->version++;
    for (int i = 0; i < ML_MAX_CLIENTS; i++) {
        ml_client *x = &ml_clients[i];
        if (x->used && ml_arena_of(x, 0) == a && (ml_arena_member(a, x) || (x->arena_seen && now - x->arena_seen <= ML_ARENA_WATCH_FOR))) ml_arena_send(x, now);
    }
}
static void ml_arena_tell_all(const ml_client *c, ULONGLONG now) {     /* (room made / closed) every player of its build and code */
    for (int i = 0; i < ML_MAX_CLIENTS; i++) if (ml_clients[i].used && ml_same_part(&ml_clients[i], c)) ml_arena_send(&ml_clients[i], now);
}
static void ml_arena_next(ml_arena *a, ULONGLONG now) {          /* no match on: the first two of the queue take their turn */
    if (a->pair[0] || a->nq < 2) return;
    a->pair[0] = a->queue[0]; a->pair[1] = a->queue[1];
    ml_arena_qdel(a, 0); ml_arena_qdel(a, 0);
    a->playing = 0; a->at = now;
    ml_log("arena: %s's turn against %s (%d waiting)", a->pair[0]->name, a->pair[1]->name, a->nq);
}
static void ml_arena_leave(ml_client *c, const char *why, ULONGLONG now) {
    ml_arena *a = ml_arena_of(c, 0);
    if (!a || !ml_arena_member(a, c)) return;
    if (ml_arena_in_pair(a, c)) {
        ml_client *o = ml_arena_other(a, c);
        a->pair[0] = a->pair[1] = NULL; a->playing = 0;
        ml_arena_qfront(a, o);                                   /* (the other player keeps its place at the front, and its streak) */
        ml_log("arena: %s left its turn (%s): %s is next again", c->name, why, o->name);
    } else {
        ml_arena_qdel(a, ml_arena_qpos(a, c));
        ml_log("arena: %s left the queue (%s)", c->name, why);
    }
    c->arena_streak = 0;
    ml_arena_next(a, now);
    ml_arena_changed(a, now);
    if (c->used) ml_arena_send(c, now);
}
static void ml_arena_close(ml_arena *a, const char *why, ULONGLONG now) {    /* (room) the host closed it or left: everyone out */
    ml_client *host = a->host;
    for (int i = 0; i < ML_MAX_CLIENTS; i++) {
        ml_client *x = &ml_clients[i];
        if (x->used && ml_arena_member(a, x)) { x->arena_why = 2; x->arena_streak = 0; }
    }
    ml_log("arena: the room of %s is closed (%s)", host ? host->name : "?", why);
    a->used = 0;                                         /* (nobody is in an arena that is not used: their states say "out") */
    if (host && host->used) ml_arena_tell_all(host, now);
    else for (int i = 0; i < ML_MAX_CLIENTS; i++) if (ml_clients[i].used && ml_clients[i].arena_why == 2) ml_arena_send(&ml_clients[i], now);
}
static void ml_arena_join(ml_arena *a, ml_client *c, ULONGLONG now) {
    if (ml_arena_member(a, c)) return;
    ml_arena_qback(a, c);
    c->arena_streak = 0; c->arena_why = 0;
    ml_log("arena: %s joined the queue of %s's room (%d waiting)", c->name, a->host->name, a->nq);
    ml_arena_next(a, now);
    ml_arena_changed(a, now);
}
/* ARENA: cookie u64, op u8 (0 leave the queue, 1 join (or stay in), 2 only look, 3 make a room (and join), 4 close it (its
 * host), 5 remove a player (its host; + name[32])) */
static void ml_on_arena(ml_client *c, const uint8_t *p, int len, ULONGLONG now) {
    int op = p[12];
    ml_arena *a = ml_arena_of(c, 0);
    if (op == 3) {
        if (!a) {
            a = ml_arena_of(c, 1);
            if (a) { a->host = c; ml_log("arena: %s made a room", c->name); ml_arena_join(a, c, now); ml_arena_tell_all(c, now); }
        } else if (a->host == c) ml_arena_join(a, c, now);
        else ml_log("arena: %s asked to make a room, but %s's room is open", c->name, a->host ? a->host->name : "?");
    } else if (op == 1 && a && a->host) ml_arena_join(a, c, now);
    else if (op == 0) ml_arena_leave(c, "left the queue", now);
    else if (op == 4 && a && a->host == c) ml_arena_close(a, "closed by its host", now);
    else if (op == 5 && a && a->host == c && len >= 13 + ML_NAME_LEN) {
        char nm[ML_NAME_LEN + 1]; ml_text(nm, sizeof nm, p + 13, ML_NAME_LEN);
        for (int i = 0; i < ML_MAX_CLIENTS; i++) {
            ml_client *x = &ml_clients[i];
            if (x->used && x != c && ml_arena_member(a, x) && !strcmp(x->name, nm)) { x->arena_why = 1; ml_arena_leave(x, "removed by the host", now); break; }
        }
    }
    c->arena_seen = now;
    ml_arena_send(c, now);
}
static void ml_arena_drop(ml_client *c, const char *why, ULONGLONG now) {     /* (ml_drop) a player gone from the lobby */
    ml_arena *a = ml_arena_of(c, 0);
    if (a && a->host == c) { char w[96]; snprintf(w, sizeof w, "its host left: %s", why); ml_arena_close(a, w, now); }
    else ml_arena_leave(c, why, now);
}
static void ml_arena_playing(ml_client *h, ml_client *g, ULONGLONG now) {     /* the pair's rollback messages went both ways */
    ml_arena *a = ml_arena_of(h, 0);
    if (!a || !ml_arena_in_pair(a, h) || !ml_arena_in_pair(a, g) || a->playing) return;
    a->playing = 1; a->at = now;
    ml_log("arena: %s and %s are playing", h->name, g->name);
    ml_arena_changed(a, now);
}
static void ml_arena_end(ml_arena *a, ml_client *winner, const char *why, ULONGLONG now) {     /* winner NULL: no winner */
    ml_client *p1 = a->pair[0], *p2 = a->pair[1];
    a->pair[0] = a->pair[1] = NULL; a->playing = 0;
    if (!winner) {
        p1->arena_streak = p2->arena_streak = 0;
        ml_arena_qback(a, p1); ml_arena_qback(a, p2);
        ml_log("arena: %s: %s and %s go to the back of the queue", why, p1->name, p2->name);
    } else {
        ml_client *loser = winner == p1 ? p2 : p1;
        loser->arena_streak = 0;
        ml_arena_qback(a, loser);
        winner->arena_streak++;
        if (ml_arena_streak && winner->arena_streak >= ml_arena_streak) {
            ml_log("arena: %s beat %s: %d wins in a row, the limit: both to the back", winner->name, loser->name, winner->arena_streak);
            winner->arena_streak = 0;
            ml_arena_qback(a, winner);
        } else {
            ml_log("arena: %s beat %s (%d in a row) and stays", winner->name, loser->name, winner->arena_streak);
            ml_arena_qfront(a, winner);
        }
    }
    ml_arena_next(a, now);
    ml_arena_changed(a, now);
}
/* RESULT (from net_relay.c net_ladder_result): cookie u64 at 4, seed u64 at 12, the other player's address u32 at 20, slot u8
 * at 24, rule u8, stocks of the rule u8, flags u8 at 27 (2 disconnected), stage u32, chara u64 x 2, stocks left u8 x 2 at 48,
 * damage u16 x 2 at 50 (tenths of a percent), frames, desyncs. Both games send it: the first one of a seed counts. */
static void ml_arena_result(ml_client *c, const uint8_t *p, ULONGLONG now) {
    ml_arena *a = ml_arena_of(c, 0);
    if (!ml_arena_in_pair(a, c)) return;
    uint64_t seed = ml_get64(p + 12);
    uint32_t peer = ml_get32(p + 20);
    int slot = p[24], flags = p[27], s0 = p[48], s1 = p[49];
    unsigned d0 = ml_get16(p + 50), d1 = ml_get16(p + 52);
    ml_client *other = ml_arena_other(a, c);
    if (seed == a->last_seed || peer != other->ip || slot > 1) return;
    a->last_seed = seed;
    int win = s0 > s1 ? 0 : s1 > s0 ? 1 : d0 < d1 ? 0 : d1 < d0 ? 1 : -1;
    if ((flags & 2) || win < 0) { ml_arena_end(a, NULL, flags & 2 ? "no winner (disconnected)" : "no winner (a draw)", now); return; }
    ml_arena_end(a, win == slot ? c : other, "result", now);
}
static void ml_arena_sweep(ULONGLONG now) {
    for (int i = 0; i < ML_ARENAS; i++) {
        ml_arena *a = &ml_arenas[i];
        if (!a->used || !a->pair[0]) continue;
        if (!a->playing && now - a->at > ML_ARENA_TURN_FOR) {
            /* (a turn not started in time: whoever did not even search is away and leaves the arena; the other keeps its place) */
            ml_client *away[2]; int na = 0;
            for (int k = 0; k < 2; k++) if (!a->pair[k]->searching && !a->pair[k]->mpeer) away[na++] = a->pair[k];
            char w[96]; snprintf(w, sizeof w, "did not start its turn in %d s", ML_ARENA_TURN_FOR / 1000);
            if (na) { for (int k = 0; k < na; k++) { away[k]->arena_why = 3; ml_arena_leave(away[k], w, now); } }     /* (two away: the next pair forms by itself; ssbu_lobby.py then ended that new pair too) */
            else { char e[160]; snprintf(e, sizeof e, "the turn of %s and %s was not started in %d s", a->pair[0]->name, a->pair[1]->name, ML_ARENA_TURN_FOR / 1000); ml_arena_end(a, NULL, e, now); }
        } else if (a->playing && now - a->at > ML_ARENA_PLAY_FOR) {
            char e[160]; snprintf(e, sizeof e, "no result of the match of %s and %s in %d s", a->pair[0]->name, a->pair[1]->name, ML_ARENA_PLAY_FOR / 1000);
            ml_arena_end(a, NULL, e, now);
        }
    }
}

/* MATCH_STATE: search id u32, state u8, role u8, flags u8 (1 the host's room is open), peer u32, peer name[32],
 * searching u16, players u16 (of c's build and code), seconds waited u16 */
static void ml_match_send(ml_client *c, ULONGLONG now) {
    uint8_t pkt[4 + 49 + 7], *p = ml_hdr(pkt, ML_MATCH_STATE);
    uint32_t sid = c->search_id, peer = 0;
    int state, role = 0, flags = 0, searching = 0, players = 0;
    const char *name = "";
    ULONGLONG since = 0;
    if (c->mpeer) {
        state = ML_MS_MATCHED; role = c->mrole; peer = c->mpeer->ip; name = c->mpeer->name; since = c->match_at;
        ml_client *host = c->mrole == 1 ? c : c->mpeer;
        if (ml_room_open(host->room, now)) flags |= 1;
    } else if (c->searching) { state = ML_MS_SEARCHING; since = c->search_since; }
    else if (c->has_final) { sid = c->final_id; state = c->final_state; role = c->final_role; peer = c->final_peer; name = c->final_name; }
    else state = ML_MS_IDLE;
    for (int i = 0; i < ML_MAX_CLIENTS; i++)
        if (ml_clients[i].used && ml_same_part(&ml_clients[i], c)) { players++; searching += ml_clients[i].searching; }
    ULONGLONG waited = since ? (now - since) / 1000 : 0;
    p = ml_put32(p, sid);
    *p++ = (uint8_t)state; *p++ = (uint8_t)role; *p++ = (uint8_t)flags;
    p = ml_put32(p, peer);
    memset(p, 0, ML_NAME_LEN); memcpy(p, name, strlen(name)); p += ML_NAME_LEN;
    p = ml_put16(p, (unsigned)searching); p = ml_put16(p, (unsigned)players); p = ml_put16(p, waited > 65535 ? 65535u : (unsigned)waited);
    /* (2026-10-30, rooms: the match goes player to player, the host only runs the room) a rollback match: 'A' + the address
     * this lobby sees the opponent at (u32, u16); 0.0.0.0 = the opponent is the game of this lobby's PC (its own loopback):
     * the player reaches it at the address it reaches the lobby at. net_relay.c then sends the match's messages straight there. */
    if (c->mpeer && c->kind == ML_KIND_ROLLBACK) {
        uint32_t a = ntohl(c->mpeer->at.sin_addr.s_addr);
        *p++ = 'A';
        p = ml_put32(p, (a >> 24) == 127 ? 0 : a);
        p = ml_put16(p, ntohs(c->mpeer->at.sin_port));
    }
    ml_send(pkt, (int)(p - pkt), &c->at);
}
static void ml_match_end(ml_client *c, int state, int role, const ml_client *peer, const char *why, ULONGLONG now) {
    c->has_final = 1; c->final_id = c->search_id; c->final_state = state; c->final_role = role;
    c->final_peer = peer ? peer->ip : 0;
    snprintf(c->final_name, sizeof c->final_name, "%s", peer ? peer->name : "");
    c->searching = 0; c->mpeer = NULL;
    ml_match_send(c, now);
    ml_log("match: %s: %s", c->name, why);
}
static void ml_match_end_pair(ml_client *a, int state, const char *wa, const char *wb, ULONGLONG now) {  /* a and its peer */
    ml_client *b = a->mpeer;
    if (!b) return;
    ml_match_end(a, state, a->mrole, b, wa, now);
    ml_match_end(b, state, b->mrole, a, wb, now);
}
static void ml_match_room(ml_client *host, ml_room *room, ULONGLONG now) {
    ml_client *g = host->mpeer;
    if (!g || host->mrole != 1) return;
    if (ml_has_node(room, g->ip) && g->has_claim && !memcmp(g->claim, room->session, 16)) {
        char wa[96], wb[96];
        snprintf(wa, sizeof wa, "in a room with %s", g->name);
        snprintf(wb, sizeof wb, "in %s's room", host->name);
        ml_match_end_pair(host, ML_MS_TOGETHER, wa, wb, now);
        return;
    }
    if (ml_room_open(room, now) && !host->room_said) { host->room_said = 1; ml_match_send(g, now); ml_match_send(host, now); }
}
static void ml_on_match(ml_client *c, int op, uint32_t sid, ULONGLONG now) {
    char w[96];
    if (op == 0) {
        if (c->mpeer) {
            ml_client *o = c->mpeer;
            c->mpeer = NULL;
            snprintf(w, sizeof w, "%s cancelled the match", c->name);
            ml_match_end(o, ML_MS_PEER_LEFT, o->mrole, c, w, now);
        }
        if (c->searching) ml_log("match: %s stopped searching", c->name);
        c->searching = 0; c->has_search = 1; c->search_id = sid;
        c->has_final = 1; c->final_id = sid; c->final_state = ML_MS_IDLE; c->final_role = 0; c->final_peer = 0; c->final_name[0] = 0;
        ml_match_send(c, now);
        return;
    }
    if (op != ML_KIND_ROOM && op != ML_KIND_ROLLBACK) return;
    if (c->has_search && c->search_id == sid && (c->mpeer || c->searching || c->has_final)) { ml_match_send(c, now); return; }
    if (c->mpeer) {
        ml_client *o = c->mpeer;
        c->mpeer = NULL;
        snprintf(w, sizeof w, "%s searches again", c->name);
        ml_match_end(o, ML_MS_PEER_LEFT, o->mrole, c, w, now);
    }
    c->has_search = 1; c->search_id = sid; c->searching = 1; c->search_since = now; c->has_final = 0; c->kind = op;
    ml_log("match: %s searches an opponent%s (build %s)", c->name, op == ML_KIND_ROLLBACK ? " for a rollback match" : "", c->build);
    ml_arena *ar = op == ML_KIND_ROLLBACK ? ml_arena_of(c, 0) : NULL;
    int c_turn = ml_arena_in_pair(ar, c);
    if (ar && !c_turn && ml_arena_member(ar, c)) { ml_match_send(c, now); return; }      /* (1.6 arena: in the queue, its turn has not come) */
    ml_client *o = NULL;
    for (int i = 0; i < ML_MAX_CLIENTS; i++) {
        ml_client *x = &ml_clients[i];
        if (!(x->used && x != c && x->searching && !x->mpeer && x->kind == op && ml_same_part(x, c))) continue;
        if (c_turn ? x != ml_arena_other(ar, c) : ar && ml_arena_member(ar, x)) continue;   /* (a turn meets only its opponent; outsiders never take an arena player) */
        if (!o || x->search_since < o->search_since) o = x;
    }
    if (!o) { ml_match_send(c, now); return; }
    int c_open = op == ML_KIND_ROOM && ml_room_open(c->room, now), o_open = op == ML_KIND_ROOM && ml_room_open(o->room, now);
    ml_client *host = c_open && !o_open ? c : o, *guest = host == c ? o : c;     /* an open room hosts, else the one that waited longer */
    if (c_turn) { host = ar->pair[0]; guest = ar->pair[1]; }                     /* (the arena: the one ahead in the queue, or the winner who stays, is player 1) */
    host->mpeer = guest; guest->mpeer = host;
    host->mrole = 1; guest->mrole = 2;
    host->match_at = guest->match_at = now;
    host->searching = guest->searching = 0;
    host->room_said = 0;
    host->rb_seen = guest->rb_seen = 0;
    if (op == ML_KIND_ROLLBACK) { host->rb_peer = guest->ip; guest->rb_peer = host->ip; }
    ml_log("match: %s hosts, %s joins%s", host->name, guest->name, op == ML_KIND_ROLLBACK ? " (rollback match)" : "");
    ml_match_send(host, now);
    ml_match_send(guest, now);
    if (host->room && op == ML_KIND_ROOM) ml_match_room(host, host->room, now);
}

/* NetworkInfo's station addresses (node 0 = owner) into out[]; -1 if it is not one a game makes */
static int ml_room_nodes(const uint8_t *info, uint32_t owner, uint32_t *out) {
    int count = info[ML_NI_NODE_COUNT], most = info[ML_NI_NODE_MAX], n = 0;
    uint32_t ips[8];
    if (!(1 <= count && count <= most && most <= 8)) return -1;
    for (int i = 0; i < 8; i++) {
        const uint8_t *o = info + ML_NI_NODES + ML_NODE_LEN * i;
        if (!o[0xB]) { if (i == 0) return -1; continue; }
        uint32_t ip = (uint32_t)o[0] | o[1] << 8 | o[2] << 16 | (uint32_t)o[3] << 24;   /* little-endian */
        if ((ip & 0xFFFF0000u) != ML_SUBNET || (i == 0 && ip != owner)) return -1;
        for (int k = 0; k < n; k++) if (ips[k] == ip) return -1;
        ips[n++] = ip;
    }
    for (int i = 1; i < n; i++) out[i - 1] = ips[i];
    return n - 1;
}

static void ml_on_advertise(ml_client *c, const uint8_t *p, int len, const uint8_t *info, ULONGLONG now) {
    uint32_t nodes[8];
    int nn = ml_room_nodes(info, c->ip, nodes);
    if (nn < 0) { ml_dropped++; return; }
    const uint8_t *session = info + ML_NI_SESSION;
    ml_room *room = ml_find_room(session);
    if (room && room->owner != c) { ml_dropped++; return; }
    if (!room) {
        if (c->room) ml_close_room(c->room, "replaced by a new room");
        c->has_claim = 0;
        for (int i = 0; i < ML_MAX_ROOMS && !room; i++) if (!ml_rooms[i].used) room = &ml_rooms[i];
        if (!room) { ml_dropped++; return; }
        memset(room, 0, sizeof *room);
        room->used = 1; room->owner = c; room->created = now;
        memcpy(room->session, session, 16);
        c->room = room;
        ml_log("%s opened a room (%d players at most)", c->name, info[ML_NI_NODE_MAX]);
    }
    ml_client *before[8], *after[8];
    int nb = ml_members(room, before);
    int changed = nn != room->nnodes;
    for (int i = 0; i < nn && !changed; i++) changed = !ml_has_node(room, nodes[i]);
    memcpy(room->info, info, ML_NI_SIZE);
    memcpy(room->nodes, nodes, nn * sizeof nodes[0]);
    room->nnodes = nn; room->updated = now;
    int na = ml_members(room, after);
    if (changed) {
        char names[8 * (ML_NAME_LEN + 3)] = "";
        for (int i = 0; i < 8; i++) {
            const uint8_t *o = info + ML_NI_NODES + ML_NODE_LEN * i;
            if (!o[0xB]) continue;
            char nm[ML_NAME_LEN + 1];
            ml_text(nm, sizeof nm, o + 0xC, ML_NAME_LEN);
            if (names[0]) strcat(names, ", ");
            strcat(names, nm[0] ? nm : "?");
        }
        ml_log("room of %s: %d player(s): %s", c->name, info[ML_NI_NODE_COUNT], names);
        for (int i = 0; i < na; i++) ml_send_peers(after[i]);
        for (int i = 0; i < nb; i++) {
            int stays = 0;
            for (int k = 0; k < na; k++) stays |= before[i] == after[k];
            if (!stays) ml_send_peers(before[i]);
        }
    }
    for (int i = 0; i < nb + na; i++) {   /* before and after, each once */
        ml_client *m = i < nb ? before[i] : after[i - nb];
        int dup = 0;
        for (int k = 0; k < i; k++) dup |= m == (k < nb ? before[k] : after[k - nb]);
        if (m != c && !dup) ml_send(p, len, &m->at);
    }
    if (c->mpeer && c->mrole == 1 && c->kind == ML_KIND_ROOM) ml_match_room(c, room, now);
}

static void ml_on_ldn(ml_client *c, const uint8_t *p, int len, uint32_t dst, unsigned sport, unsigned dport, ULONGLONG now) {
    if (sport != ML_LDN_PORT || dport != ML_LDN_PORT || len < 20 || p[16] != 'L') { ml_dropped++; return; }
    ml_ldn_msgs++;
    int kind = p[17], blen = len - 20;
    const uint8_t *body = p + 20;
    if (ml_is_bcast(dst)) {
        if (kind == ML_M_ADVERTISE && blen >= ML_NI_SIZE) ml_on_advertise(c, p, len, body, now);
        else if (kind == ML_M_GONE && c->room && blen >= 16 && !memcmp(body, c->room->session, 16)) ml_close_room(c->room, "closed by its host");
        return;
    }
    ml_client *b = ml_by_ip(dst);
    if (!b || b == c || !ml_same_part(b, c)) { ml_dropped++; return; }
    if (kind == ML_M_JOIN) {
        if (!b->room || blen < 16 || memcmp(body, b->room->session, 16)) { ml_dropped++; return; }
        if (c->room) ml_close_room(c->room, "its host joins another room");
        c->has_claim = 1;
        memcpy(c->claim, b->room->session, 16);
        ml_log("%s asks to join %s's room", c->name, b->name);
    } else if (kind == ML_M_LEAVE) {
        if (c->has_claim && b->room && !memcmp(b->room->session, c->claim, 16)) {
            c->has_claim = 0;
            ml_send(p, len, &b->at);
            ml_send_peers(c);
            return;
        }
    }
    ml_send(p, len, &b->at);
}

static void ml_on_data(ml_client *c, const uint8_t *p, int len, const uint32_t *skip, int nskip, ULONGLONG now) {
    if (len - 16 > ML_MAX_DATA + ML_NI_SIZE) { ml_dropped++; return; }
    uint32_t src = ml_get32(p + 4), dst = ml_get32(p + 8);
    unsigned sport = ml_get16(p + 12), dport = ml_get16(p + 14);
    if (src != c->ip) { ml_dropped++; return; }
    if (sport == ML_LDN_PORT || dport == ML_LDN_PORT) { ml_on_ldn(c, p, len, dst, sport, dport, now); return; }
    ml_room *room = ml_room_of(c);
    if (!room) { ml_dropped++; return; }
    ml_client *mem[8];
    int cnt = ml_members(room, mem), sent = 0;
    for (int i = 0; i < cnt; i++) {
        int skipped = 0;
        for (int k = 0; k < nskip; k++) skipped |= skip[k] == mem[i]->ip;
        if (mem[i] == c || skipped) continue;
        if (ml_is_bcast(dst) || mem[i]->ip == dst) { ml_send(p, len, &mem[i]->at); sent++; }
    }
    ml_relayed += sent;
    if (!sent && !ml_is_bcast(dst)) ml_dropped++;
}

static void ml_packet(const uint8_t *p, int len, const struct sockaddr_in *at, ULONGLONG now) {
    if (len < 4 || p[0] != 'S' || p[1] != 'U' || p[2] != ML_PROTO) return;
    int kind = p[3];
    ml_client *c = ml_by_at(at);
    if (kind == ML_JOIN) {
        for (int i = 0; i < ML_OPEN_MAX; i++) if (ml_open[i].used && ml_same_at(&ml_open[i].at, at)) ml_open[i].used = 0;   /* NAT is open */
        ml_on_join(p, len, at, c, now);
        return;
    }
    if (!c) return;
    c->seen = now;
    int cookie_ok = len >= 12 && ml_get64(p + 4) == c->cookie;
    if (kind == ML_PING && len >= 21 && cookie_ok) {
        int players = 0, rooms = 0;
        for (int i = 0; i < ML_MAX_CLIENTS; i++) players += ml_clients[i].used && ml_same_part(&ml_clients[i], c);
        for (int i = 0; i < ML_MAX_ROOMS; i++) rooms += ml_rooms[i].used && ml_same_part(ml_rooms[i].owner, c);
        int searching = 0, code = 0;       /* O2: players searching (this build and code), players of this code (any build) */
        for (int i = 0; i < ML_MAX_CLIENTS; i++) {
            searching += ml_clients[i].used && ml_clients[i].searching && ml_same_part(&ml_clients[i], c);
            code += ml_clients[i].used && !strcmp(ml_clients[i].code, c->code);
        }
        uint8_t pong[4 + 8 + 8], *q = ml_hdr(pong, ML_PONG);
        memcpy(q, p + 12, 8);
        ml_put16(ml_put16(ml_put16(ml_put16(q + 8, players), rooms), searching), code);
        ml_send(pong, sizeof pong, at);
    } else if (kind == ML_LEAVE && len >= 12 && cookie_ok) {
        ml_drop(c, "left");
    } else if (kind == ML_LIST && len >= 16 && cookie_ok) {
        ml_on_list(c, p + 12, now, len >= 17 && (p[16] & 1));
    } else if (kind == ML_ROOMS && len >= 16 && cookie_ok) {
        ml_on_rooms(c, p + 12, now);
    } else if (kind == ML_MATCH && len >= 17 && cookie_ok) {
        ml_on_match(c, p[12], ml_get32(p + 13), now);
    } else if (kind == ML_ARENA && len >= 13 && cookie_ok) {        /* (1.6) */
        ml_on_arena(c, p, len, now);
    } else if (kind == ML_RESULT && len >= ML_RESULT_LEN && cookie_ok) {
        ml_arena_result(c, p, now);
    } else if (kind == 16 && len > 16 && len <= 1500 && cookie_ok) {      /* R3: a rollback message (net_relay.c T_RB) to the other players of its lobby code */
        uint32_t dst = ml_get32(p + 12);
        static uint8_t out[1600];   /* (only used under ml_lock) */
        int on = len - 16 + 8, sent = 0;
        /* (no "public code" restriction here, unlike ssbu_lobby.py: this lobby runs inside a host's game (HOST / JOIN / LAN modes), its
         * players connected on purpose, and the joiner of a room code sends to destination 0 = the host: R3 net3 run pd3) */
        memcpy(out, "SU\1\x10", 4); ml_put32(out + 4, c->ip); memcpy(out + 8, p + 16, len - 16);
        for (int i = 0; i < ML_MAX_CLIENTS; i++) {
            ml_client *o = &ml_clients[i];
            if (!o->used || o == c || !ml_same_part(o, c) || (dst && o->ip != dst)) continue;
            ml_send(out, on, &o->at); sent++;
        }
        ml_relayed += sent;
        if (!sent) ml_dropped++;
        if (dst && c->mpeer && c->kind == ML_KIND_ROLLBACK && c->mpeer->ip == dst) {   /* (1.4) RB both ways between a rollback pair: together */
            c->rb_seen = 1;
            if (c->mpeer->rb_seen) {
                char wa[96], wb[96]; ml_client *h = c->mrole == 1 ? c : c->mpeer;
                ml_arena_playing(h, h->mpeer, now);               /* (1.6) */
                snprintf(wa, sizeof wa, "playing a rollback match with %s", h->mpeer->name);
                snprintf(wb, sizeof wb, "playing a rollback match with %s", h->name);
                ml_match_end_pair(h, ML_MS_TOGETHER, wa, wb, now);
            }
        }
    } else if (kind == ML_DATA && len >= 16) {
        ml_on_data(c, p, len, NULL, 0, now);
    } else if (kind == ML_BCAST && len >= 5) {
        int n = p[4], body = 5 + 4 * n;
        uint32_t skip[255];
        if (len < body) n = 0, body = 5 + 4 * p[4];   /* (no usable skip list) */
        for (int i = 0; i < n; i++) skip[i] = ml_get32(p + 5 + 4 * i);
        if (len >= body + 12) {
            static uint8_t buf[65536];   /* (only used under ml_lock) */
            memcpy(buf, "SU\1\x06", 4);
            memcpy(buf + 4, p + body, len - body);
            ml_on_data(c, buf, 4 + len - body, skip, n, now);
        }
    }
}

static void ml_sweep(ULONGLONG now) {
    for (int i = 0; i < ML_MAX_CLIENTS; i++)
        if (ml_clients[i].used && now - ml_clients[i].seen > ML_CLIENT_TIMEOUT) ml_drop(&ml_clients[i], "silent for 10 s");
    for (int i = 0; i < ML_MAX_ROOMS; i++)
        if (ml_rooms[i].used && now - ml_rooms[i].updated > ML_ROOM_TIMEOUT) ml_close_room(&ml_rooms[i], "no advertisement for 10 s");
    for (int i = 0; i < ML_MAX_CLIENTS; i++) {        /* O2: searches without an opponent, matches nobody followed */
        ml_client *c = &ml_clients[i];
        char w[96], wb[96];
        if (!c->used) continue;
        if (c->searching && !c->mpeer && now - c->search_since > ml_search_for) {
            snprintf(w, sizeof w, "no opponent in %u s", (unsigned)(ml_search_for / 1000));
            ml_match_end(c, ML_MS_TIMEOUT, 0, NULL, w, now);
        }
        if (c->mpeer && c->mrole == 1 && now - c->match_at > ml_match_for) {
            snprintf(w, sizeof w, "the match with %s expired", c->mpeer->name);
            snprintf(wb, sizeof wb, "the match with %s expired", c->name);
            ml_match_end_pair(c, ML_MS_EXPIRED, w, wb, now);
        }
    }
    ml_arena_sweep(now);                                /* (1.6) */
}

static DWORD WINAPI ml_thread_main(LPVOID arg) {
    static uint8_t buf[65536];
    ULONGLONG last_sweep, last_stats, last_open = 0;
    (void)arg;
    last_sweep = last_stats = GetTickCount64();
    while (ml_running) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(ml_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        ULONGLONG now = GetTickCount64();
        EnterCriticalSection(&ml_lock);
        if (n > 0 && ml_running) ml_packet(buf, n, &from, now);
        if (now - last_sweep >= 1000) { last_sweep = now; ml_sweep(now); }
        if (now - last_stats >= 60000) {
            int players = 0, rooms = 0;
            last_stats = now;
            for (int i = 0; i < ML_MAX_CLIENTS; i++) players += ml_clients[i].used;
            for (int i = 0; i < ML_MAX_ROOMS; i++) rooms += ml_rooms[i].used;
            if (players) ml_log("stats: %d player(s), %d room(s), relayed game datagrams %llu, ldn messages %llu, dropped %llu",
                                players, rooms, ml_relayed, ml_ldn_msgs, ml_dropped);
        }
        if (now - last_open >= 500) {   /* keep opening the host's NAT for joiners who gave their code */
            last_open = now;
            for (int i = 0; i < ML_OPEN_MAX; i++) if (ml_open[i].used) ml_send("SUo", 4, &ml_open[i].at);
        }
        LeaveCriticalSection(&ml_lock);
        if (n < 0 && WSAGetLastError() != WSAETIMEDOUT && WSAGetLastError() != WSAECONNRESET && ml_running) Sleep(10);
    }
    return 0;
}

/* ---- API */
static uint16_t minilobby_start(uint32_t bind_ip_host_order, uint16_t port, int no_peers) {
    WSADATA wsa;
    struct sockaddr_in a;
    int al = sizeof a;
    DWORD timeout = 50;
    BOOL off = FALSE;
    DWORD bytes = 0;
    if (ml_running) return 0;
    if (!ml_lock_ready) { InitializeCriticalSection(&ml_lock); ml_lock_ready = 1; }
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);   /* (no ICMP errors on receive) */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(bind_ip_host_order);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || getsockname(s, (struct sockaddr *)&a, &al)) { closesocket(s); return 0; }
    EnterCriticalSection(&ml_lock);
    memset(ml_clients, 0, sizeof ml_clients);
    memset(ml_rooms, 0, sizeof ml_rooms);
    memset(ml_open, 0, sizeof ml_open);
    memset(ml_arenas, 0, sizeof ml_arenas);
    {   const char *e = getenv("SSBU_ARENA_STREAK"); ml_arena_streak = e ? atoi(e) : 0; if (ml_arena_streak < 0) ml_arena_streak = 0; }   /* (1.6: 0 = the winner stays without a limit) */
    ml_next_ip = ML_SUBNET + 2;
    ml_relayed = ml_ldn_msgs = ml_dropped = 0;
    ml_no_peers = no_peers;
    {   const char *e = getenv("SSBU_LOBBY_SEARCH_SECS"), *m = getenv("SSBU_LOBBY_MATCH_SECS");     /* (tests: shorter) */
        if (e && atof(e) > 0) ml_search_for = (ULONGLONG)(atof(e) * 1000);
        if (m && atof(m) > 0) ml_match_for = (ULONGLONG)(atof(m) * 1000); }
    ml_sock = s;
    ml_running = 1;
    LeaveCriticalSection(&ml_lock);
    ml_thread = CreateThread(NULL, 0, ml_thread_main, NULL, 0, NULL);
    if (!ml_thread) { ml_running = 0; ml_sock = INVALID_SOCKET; closesocket(s); return 0; }
    ml_log("lobby on UDP port %u (protocol SU %d%s)", ntohs(a.sin_port), ML_PROTO, no_peers ? ", everyone relayed" : "");
    return ntohs(a.sin_port);
}

static void minilobby_stop(void) {
    if (!ml_running) return;
    ml_running = 0;
    WaitForSingleObject(ml_thread, 2000);
    CloseHandle(ml_thread);
    ml_thread = NULL;
    EnterCriticalSection(&ml_lock);
    closesocket(ml_sock);
    ml_sock = INVALID_SOCKET;
    LeaveCriticalSection(&ml_lock);
}

static void minilobby_counts(int *players, int *rooms) {
    int p = 0, r = 0;
    if (ml_lock_ready) {
        EnterCriticalSection(&ml_lock);
        for (int i = 0; i < ML_MAX_CLIENTS; i++) p += ml_clients[i].used;
        for (int i = 0; i < ML_MAX_ROOMS; i++) r += ml_rooms[i].used;
        LeaveCriticalSection(&ml_lock);
    }
    if (players) *players = p;
    if (rooms) *rooms = r;
}

static SOCKET minilobby_socket(void) { return ml_running ? ml_sock : INVALID_SOCKET; }

static void minilobby_open_to(const struct sockaddr_in *at) {
    int slot = -1;
    if (!at || !ml_lock_ready) return;
    EnterCriticalSection(&ml_lock);
    for (int i = 0; i < ML_OPEN_MAX; i++) {
        if (ml_open[i].used && ml_same_at(&ml_open[i].at, at)) { slot = -2; break; }
        if (!ml_open[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) { ml_open[slot].at = *at; ml_open[slot].used = 1; }
    LeaveCriticalSection(&ml_lock);
}

#ifdef MINILOBBY_MAIN
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: minilobby <port> [1 = no PEERS]\n"); return 2; }
    if (!minilobby_start(0x7F000001u, (uint16_t)atoi(argv[1]), argc > 2 && argv[2][0] == '1')) { fprintf(stderr, "cannot start\n"); return 1; }
    for (;;) Sleep(1000);
}
#endif
