/* Online play: the client side of the lobby protocol, and direct player-to-player paths (see net_relay.h).
 * Adapted from SNO2207/WindowSmash (runtime/net_relay.c: lobby client, UDP hole punching, relay fallback), used with
 * the author's permission. Our own code and our own wire protocol; no UPnP and no in-game relay (yet).
 *
 * Wire protocol "SU" version 1, UDP, every field big-endian; each packet starts 'S' 'U' 1 <type>:
 *   1 JOIN     client: nonce u64, code[32], name[32], build[40], lan address u32, lan port u16, flags u8 (1: direct ok)
 *   2 WELCOME  lobby:  nonce u64, virtual address u32, cookie u64
 *   3 REJECT   lobby:  nonce u64, text[64]
 *   4 PING     client: cookie u64, time u64, direct paths u8                   (every second: also keeps NAT mappings open)
 *   5 PONG     lobby:  time u64, players u16, rooms u16
 *   6 DATA     both:   source u32, destination u32, source port u16, destination port u16, payload
 *   7 LEAVE    client: cookie u64
 *   8 LIST     client: cookie u64, seq u32                                      (nn::ldn::Scan: the lobby's rooms)
 *   9 LISTING  lobby:  seq u32, host address u32, NetworkInfo[0x480]            (up to 24, best first)
 *  10 LIST_END lobby:  seq u32, count u8
 *  11 PEERS    lobby:  version u32, count u8, count x (address u32, name[32], public ip u32, port u16, lan ip u32,
 *                      port u16, secret u64)           (the other players of this player's room that allow direct paths)
 *  12 PUNCH    player: secret u64, from u32, to u32, ack u8, nonce u64          (straight to another player)
 *  13 DIRECT   player: secret u64, then a DATA body                             (the same)
 *  14 BCAST    client: count u8, count x address u32, then a DATA body          (a broadcast; the lobby leaves out the
 *                      players listed: this player reached them directly)
 * O1 (2026-10-20, plan\phases\O1_room_browser.md): additions an older lobby or game simply ignores.
 *   JOIN may end with 'M' 'S' + a mod-set block: count u8, count x (hash u32, length u8, name[length])  (this game's mods)
 *   LIST may end with flags u8: 1 = also the rooms of OTHER builds of this lobby code; each LISTING then ends with the
 *        room host's build[40] (the game shows those rooms too; joining one is refused here, net_room_refusal)
 *  17 ROOMS     client: cookie u64, seq u32                                      (the room browser: every room of this code)
 *  18 ROOM_INFO lobby:  seq u32, host address u32, players u8, most u8, flags u8 (1 open, 2 yours, 4 mods known),
 *                       host name[32], build[40], mod-set block                  (up to 32, joinable first)
 *  19 ROOMS_END lobby:  seq u32, count u8
 * O2 (2026-10-21, plan\phases\O2_matchmaking.md): matchmaking, "Find an opponent" (F1 > Online). An older lobby ignores
 * MATCH (the game then says "older lobby"); an older game never sends it and is never matched.
 *  20 MATCH       client: cookie u64, op u8 (1 search: sent again every 2 s while searching or matched; 0 cancel), search id u32
 *  21 MATCH_STATE lobby:  search id u32, state u8 (0 idle, 1 searching, 2 matched, 3 timeout, 4 the other cancelled or left,
 *                         5 both in one room, 6 expired), role u8 (1 you host, 2 you join), flags u8 (1 the host's room is
 *                         open), the other player's address u32, its name[32], players searching u16, players u16 (both of
 *                         this build and code), seconds waited u16
 *   PONG may end with: players searching u16 (this build and code), players u16 (every build of this code)
 * Ladder (lobby 1.5, tools/net/ladder.py): JOIN may end, after the 'M' 'S' block, with 'L' 'D' + the ladder identity, and WELCOME
 * with 'L' 'D' + this player's standing (see ladder_block / ladder_welcome); RESULT / RESULT_ACK (22 / 23) carry a rollback
 * match's result (see net_ladder_result). An older lobby ignores all of it.
 * R3 net3 (lobby 1.4): MATCH op 2 = a search for a ROLLBACK match (rb_net.c: Smash versus with rollback on, no room code, no
 * Local Wireless room): paired only with another op 2 search, the one that waited longer is player 1 (role 1); the lobby
 * says TOGETHER once it relayed rollback messages both ways between the two. An older lobby ignores op 2 (= older lobby).
 * The build id is "ssbu-recomp-ldn1-<game version>" plus "+m<8 hex>" when mods are loaded (the hash of the mod set):
 * the lobby keeps parting players by build and code, so different versions or mod sets never share a room.
 * The lobby gives every player its own virtual address and relays a Local Wireless room's datagrams between that room's
 * players only. nnldn.c's own messages (port 65511) always go through the lobby: it is the judge of who is in which room.
 *
 * Configuration (environment first, then graphics.ini keys; nothing is written):
 *   SSBU_ONLINE=1 | <host:port>      online_server = <host:port>      the lobby (default 127.0.0.1:39500); 0/off = off
 *   SSBU_ONLINE_NAME                 online_name                      name in the lobby's log (default Player-xxxx)
 *   SSBU_ONLINE_CODE                 online_code                      lobby code: only players with the same code meet
 *   SSBU_ONLINE_DIRECT=0             online_direct = off              no direct paths: everything through the lobby
 *   SSBU_ONLINE_PORT=<n>             local UDP port (default: any)
 *   SSBU_ONLINE_LOG=0..3             log level (default 1)
 *   SSBU_ONLINE_BUILD                build id override (players of different builds never meet) */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "net_relay.h"
#include "net_stats.h"
#include "gfx_options.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

enum { T_JOIN = 1, T_WELCOME, T_REJECT, T_PING, T_PONG, T_DATA, T_LEAVE, T_LIST, T_LISTING, T_LIST_END,
       T_PEERS, T_PUNCH, T_DIRECT, T_BCAST };
enum { T_ROOMS = 17, T_ROOM_INFO, T_ROOMS_END };      /* O1 room browser */
enum { T_MATCH = 20, T_MATCH_STATE };                 /* O2 matchmaking */
#define PROTO 1
#define NAME_LEN 32
#define BUILD_LEN 40
#define NI_SIZE 0x480
#define MAX_PEERS 7
#define PEER_LEN (4 + NAME_LEN + 4 + 2 + 4 + 2 + 8)
#define LOBBY_TIMEOUT_MS 10000
#define GONE_AFTER_MS 5000
#define PUNCH_FOR_MS 5000          /* hellos every 200 ms this long; no answer -> relayed (hello every 5 s in case it opens) */
#define FRESH_MS 1200              /* a direct path is used while its last answer is younger than this */
#define NET_BUILD_DEFAULT "ssbu-recomp-ldn1"
#define NET_PUBLIC_LOBBY "opensmash-lobby.duckdns.org:39500"     /* the project's public lobby (menu.cpp ONLINE_DEFAULT_LOBBY): the default without online_mode / online_server */

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) << 32 | g32(p + 4); }
static void s16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void s32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void s64(uint8_t *p, uint64_t v) { s32(p, (uint32_t)(v >> 32)); s32(p + 4, (uint32_t)v); }
static void hdr(uint8_t *p, int type) { p[0] = 'S'; p[1] = 'U'; p[2] = PROTO; p[3] = (uint8_t)type; }
static uint64_t ms_now(void) { return GetTickCount64(); }
static uint64_t rnd64(void) {
    uint64_t v = 0;
    HCRYPTPROV h;
    if (CryptAcquireContextA(&h, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) { CryptGenRandom(h, sizeof v, (BYTE *)&v); CryptReleaseContext(h, 0); }
    if (!v) v = ((uint64_t)GetTickCount64() << 32) ^ GetCurrentProcessId() ^ (uint64_t)(uintptr_t)&v;
    return v;
}

/* ---------------------------------------------------------------- configuration */
static int g_cfg = -1, g_log = 1, g_direct = 1;
extern int g_game_build;             /* hostrt.c: 0 = 13.0.4, 1 = 13.0.5 (hostrt.h GAME_13_0_*), -1 before main.elf is read */
static char g_server[256], g_name[NAME_LEN + 1], g_code[NAME_LEN + 1] = "public", g_build[BUILD_LEN + 1] = NET_BUILD_DEFAULT;
static uint16_t g_local_port;

static int ini_value(const char *key, char *out, size_t n) {     /* "key = value" in graphics.ini (read only) */
    const GfxOptions *o = gfx_options();
    FILE *f = o && o->path[0] ? fopen(o->path, "r") : NULL;
    char line[512];
    int found = 0;
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t kl = strlen(key);
        if (_strnicmp(s, key, kl) || (s[kl] != ' ' && s[kl] != '\t' && s[kl] != '=')) continue;
        s += kl;
        while (*s == ' ' || *s == '\t') s++;
        if (*s++ != '=') continue;
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strcspn(s, ";#\r\n");
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
        snprintf(out, n, "%s", s);
        found = 1;
    }
    fclose(f);
    return found;
}
static int is_off(const char *v) { return !*v || !strcmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "no") || !_stricmp(v, "false"); }

/* ---------------------------------------------------------------- ladder (lobby 1.5, tools/net/ladder.py): who this player is
 * graphics.ini (written once, the first time online play is on): ladder_id = 32 hex (random), ladder_key = 64 hex (random,
 * never shown); ladder_registered = 1 once a lobby said "ranked" (the key is not sent any more); ladder_invite = the code
 * the lobby's admin gave (ssbu_lobby.py --ladder-invite; SSBU_LADDER_INVITE wins). SSBU_LADDER=0: no ladder block. */
#define LD_ID 16
#define LD_KEY 32
#define LD_INVITE 40
typedef struct { uint32_t h[8]; uint8_t b[64]; uint64_t len; int n; } Sha256;
static const uint32_t k_sha256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR32(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha256_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ w[i - 2] >> 10);
    memcpy(v, s->h, sizeof v);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = v[7] + (ROR32(v[4], 6) ^ ROR32(v[4], 11) ^ ROR32(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k_sha256[i] + w[i];
        uint32_t t2 = (ROR32(v[0], 2) ^ ROR32(v[0], 13) ^ ROR32(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof *v);
        v[4] += t1; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += v[i];
}
static void sha256_init(Sha256 *s) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0); s->len = 0; s->n = 0;
}
static void sha256_add(Sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    while (n--) { s->b[s->n++] = *p++; if (s->n == 64) { sha256_block(s, s->b); s->n = 0; } }
}
static void sha256_end(Sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t one = 0x80, zero = 0, l[8];
    sha256_add(s, &one, 1);
    while (s->n != 56) sha256_add(s, &zero, 1);
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_add(s, l, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}
static void hmac_sha256(const uint8_t *key, size_t kn, const uint8_t *msg, size_t n, uint8_t out[32]) {     /* (kn <= 64) */
    uint8_t pad[64], inner[32];
    Sha256 s;
    for (int pass = 0; pass < 2; pass++) {
        memset(pad, 0, sizeof pad); memcpy(pad, key, kn);
        for (int i = 0; i < 64; i++) pad[i] ^= pass ? 0x5c : 0x36;
        sha256_init(&s); sha256_add(&s, pad, 64);
        if (pass) sha256_add(&s, inner, 32); else sha256_add(&s, msg, n);
        sha256_end(&s, pass ? out : inner);
    }
}
static int g_ld_on;                                     /* the 'L''D' block goes with JOIN */
static uint8_t g_ld_id[LD_ID], g_ld_key[LD_KEY];
static volatile LONG g_ld_registered;
static char g_ld_invite[LD_INVITE + 1];
static SRWLOCK g_ld_lock = SRWLOCK_INIT;
static NetLadder g_ld;                                  /* (g_ld_lock) what the lobby said: WELCOME, RESULT_ACK */
static int hex_bytes(const char *t, uint8_t *out, int n) {
    if ((int)strlen(t) != 2 * n) return 0;
    for (int i = 0; i < n; i++) { unsigned v; if (!isxdigit((unsigned char)t[2 * i]) || !isxdigit((unsigned char)t[2 * i + 1]) || sscanf(t + 2 * i, "%2x", &v) != 1) return 0; out[i] = (uint8_t)v; }
    return 1;
}
static void hex_text(const uint8_t *b, int n, char *out) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
static void ladder_config(void) {
    char v[160], t[2 * LD_KEY + 1];
    const char *e = getenv("SSBU_LADDER");
    g_ld.state = NET_LADDER_OFF; g_ld.last_outcome = -1;
    if (e && is_off(e)) return;
    if (!(ini_value("ladder_id", v, sizeof v) && hex_bytes(v, g_ld_id, LD_ID)) || !(ini_value("ladder_key", v, sizeof v) && hex_bytes(v, g_ld_key, LD_KEY))) {
        for (int i = 0; i < LD_ID; i += 8) { uint64_t r = rnd64(); memcpy(g_ld_id + i, &r, 8); }
        for (int i = 0; i < LD_KEY; i += 8) { uint64_t r = rnd64(); memcpy(g_ld_key + i, &r, 8); }
        hex_text(g_ld_id, LD_ID, t);
        int ok = gfx_ini_set("ladder_id", t);
        hex_text(g_ld_key, LD_KEY, t);
        ok &= gfx_ini_set("ladder_key", t);
        gfx_ini_set("ladder_registered", "0");
        if (!ok) { printf("[online] ladder: graphics.ini cannot be written: no ladder identity\n"); fflush(stdout); return; }
        printf("[online] ladder: a new ladder identity was made (graphics.ini ladder_id / ladder_key; keep them to keep your rating)\n");
    }
    g_ld_registered = ini_value("ladder_registered", v, sizeof v) && !is_off(v);
    if ((e = getenv("SSBU_LADDER_INVITE")) && *e) snprintf(g_ld_invite, sizeof g_ld_invite, "%s", e);
    else if (ini_value("ladder_invite", v, sizeof v)) snprintf(g_ld_invite, sizeof g_ld_invite, "%s", v);
    g_ld_on = 1;
    g_ld.state = NET_LADDER_UNKNOWN;
}
/* JOIN's 'L''D' block (after the 'M''S' block): version 1, id[16], HMAC-SHA256(key, nonce as sent)[32], flags u8 (1: the
 * sign-up follows: key[32], invitation length u8, invitation) */
static int ladder_block(uint8_t *p, const uint8_t *nonce) {
    if (!g_ld_on) return 0;
    int n = 0, il = (int)strlen(g_ld_invite);
    p[n++] = 'L'; p[n++] = 'D'; p[n++] = 1;
    memcpy(p + n, g_ld_id, LD_ID); n += LD_ID;
    hmac_sha256(g_ld_key, LD_KEY, nonce, 8, p + n); n += 32;
    if (g_ld_registered) { p[n++] = 0; return n; }
    p[n++] = 1;
    memcpy(p + n, g_ld_key, LD_KEY); n += LD_KEY;
    p[n++] = (uint8_t)il;
    memcpy(p + n, g_ld_invite, (size_t)il);
    return n + il;
}
static void ladder_numbers(const uint8_t *p) {          /* rating, RD, wins, losses, place (u16 each); g_ld_lock held */
    g_ld.rating = g16(p); g_ld.rd = g16(p + 2); g_ld.wins = g16(p + 4); g_ld.losses = g16(p + 6); g_ld.place = g16(p + 8);
}
static void ladder_welcome(const uint8_t *p, int n) {   /* WELCOME's 'L''D' tail: status u8, numbers, text[32] */
    if (!g_ld_on || n < 3 + 10 + NAME_LEN || p[0] != 'L' || p[1] != 'D') return;
    int st = p[2];
    char text[NAME_LEN + 1];
    for (int k = 0; k < NAME_LEN; k++) text[k] = p[13 + k] && p[13 + k] < 0x20 ? '?' : (char)p[13 + k];
    text[NAME_LEN] = 0;
    AcquireSRWLockExclusive(&g_ld_lock);
    int was = g_ld.state;
    g_ld.state = st == 1 ? NET_LADDER_RANKED : NET_LADDER_UNRANKED;
    ladder_numbers(p + 3);
    memcpy(g_ld.text, text, sizeof g_ld.text);
    ReleaseSRWLockExclusive(&g_ld_lock);
    if (st == 1 && !g_ld_registered) { InterlockedExchange(&g_ld_registered, 1); gfx_ini_set("ladder_registered", "1"); if (g_ld_invite[0]) gfx_ini_set("ladder_invite", ""); g_ld_invite[0] = 0; }
    if (st == 2 && g_ld_registered) { InterlockedExchange(&g_ld_registered, 0); gfx_ini_set("ladder_registered", "0"); }   /* (a new ladder: sign up again) */
    if (was != g_ld.state || st == 2) { printf("[online] ladder: %s%s%s\n", st == 1 ? "ranked as " : "not ranked: ", text, st == 1 ? "" : st == 2 ? " (signing up)" : ""); fflush(stdout); }
}

/* ---------------------------------------------------------------- O1: the mod set this game loads
 * What the runtime loads (read, never changed; only while online play is on):
 *   - the characters folder (SSBU_CHARACTERS; or <exe>\..\characters with SSBU_LOOSE_FILES=1, loose_files.c): every
 *     sub-folder is one mod, a new fighter (fighter.json: its display_name, else name) or a plain file mod (folder name)
 *   - SSBU_MODS (fighter_sdk\hook_host.c): every *.dll of that folder is one mod (file name). Left out when it is the
 *     registry's own staging folder registry_bin (newmodule_host.c sets it with SSBU_CHARACTERS: the fighters' DLLs).
 * Each mod's hash: the sum, over its files, of FNV-1a 64 of (relative path in lower case, size): the same copy of a mod
 * gives the same hash on every PC whatever the order Windows lists the files in (dates are left out: a copy changes
 * them). The set's hash (FNV-1a over the sorted names and hashes) goes into the build id as "+m<8 hex>". */
#define MODS_MAX 32
#define MOD_NAME 31
typedef struct { char name[MOD_NAME + 1]; uint32_t hash; } ModEntry;
static ModEntry g_mods[MODS_MAX];
static int g_nmods;
static uint32_t g_mods_hash;
static uint8_t g_mods_wire[1 + MODS_MAX * (5 + MOD_NAME)];
static int g_mods_wire_len = 1;                        /* (count 0) */
static char g_mods_text[512] = "none";
#define FNV0 0xcbf29ce484222325ull
static uint64_t fnv64(uint64_t h, const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; while (n--) { h ^= *b++; h *= 0x100000001b3ull; } return h; }
static uint64_t tree_hash(const char *dir, const char *rel, int depth, int *files) {
    char pat[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    uint64_t sum = 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        char sub[MAX_PATH * 2], r[MAX_PATH * 2];
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 16) sum += tree_hash(sub, r, depth + 1, files); continue; }
        for (char *s = r; *s; s++) *s = (char)tolower((unsigned char)*s);
        uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, x = fnv64(FNV0, r, strlen(r));
        sum += fnv64(x, &size, sizeof size);
        (*files)++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return sum;
}
static int json_str(const char *js, const char *key, char *out, size_t n) {     /* "key": "value" (flat manifest) */
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
    if (*p++ != '"') return 0;
    size_t k = 0;
    while (*p && *p != '"' && k + 1 < n) out[k++] = *p++;
    out[k] = 0;
    return k > 0;
}
static void mod_add(const char *name, uint64_t h64) {
    if (g_nmods >= MODS_MAX) return;
    ModEntry *m = &g_mods[g_nmods++];
    size_t k = 0;
    for (const char *s = name; *s && k < MOD_NAME; s++) m->name[k++] = (unsigned char)*s >= 0x20 && *s != ',' ? *s : ' ';
    m->name[k] = 0;
    m->hash = (uint32_t)(h64 ^ h64 >> 32);
}
static int mod_cmp(const void *a, const void *b) { return _stricmp(((const ModEntry *)a)->name, ((const ModEntry *)b)->name); }
static void mods_scan(void) {
    char root[MAX_PATH * 2] = "";
    const char *ch = getenv("SSBU_CHARACTERS"), *lf = getenv("SSBU_LOOSE_FILES"), *md = getenv("SSBU_MODS");
    if (ch && ch[0] && !(lf && lf[0] == '0')) snprintf(root, sizeof root, "%s", ch);
    else if (lf && lf[0] == '1') {
        DWORD n = GetModuleFileNameA(NULL, root, MAX_PATH);
        while (n && root[n - 1] != '\\' && root[n - 1] != '/') n--;
        snprintf(root + n, sizeof root - n, "..\\characters");
    }
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH * 2];
    if (root[0]) {
        snprintf(pat, sizeof pat, "%s\\*", root);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                char sub[MAX_PATH * 2], mf[MAX_PATH * 2], js[4096] = "", name[64];
                int files = 0;
                snprintf(sub, sizeof sub, "%s\\%s", root, fd.cFileName);
                snprintf(mf, sizeof mf, "%s\\fighter.json", sub);
                FILE *f = fopen(mf, "rb");
                if (f) { size_t n = fread(js, 1, sizeof js - 1, f); fclose(f); js[n] = 0; }
                if (!json_str(js, "display_name", name, sizeof name) && !json_str(js, "name", name, sizeof name)) snprintf(name, sizeof name, "%s", fd.cFileName);
                uint64_t h64 = tree_hash(sub, "", 0, &files);
                if (files) mod_add(name, h64);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if (md && md[0] && strcmp(md, "0") && !(ch && ch[0] && !_stricmp(md, "registry_bin"))) {
        snprintf(pat, sizeof pat, "%s\\*.dll", md);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char name[MAX_PATH], low[MAX_PATH];
                snprintf(name, sizeof name, "%s", fd.cFileName);
                char *dot = strrchr(name, '.'); if (dot) *dot = 0;
                snprintf(low, sizeof low, "%s", fd.cFileName);
                for (char *s = low; *s; s++) *s = (char)tolower((unsigned char)*s);
                uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
                mod_add(name, fnv64(fnv64(FNV0, low, strlen(low)), &size, sizeof size));
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    qsort(g_mods, (size_t)g_nmods, sizeof g_mods[0], mod_cmp);
    uint64_t set = FNV0;
    uint8_t *w = g_mods_wire;
    *w++ = (uint8_t)g_nmods;
    g_mods_text[0] = 0;
    for (int i = 0; i < g_nmods; i++) {
        size_t l = strlen(g_mods[i].name);
        char hx[16];
        set = fnv64(set, g_mods[i].name, l + 1);
        set = fnv64(set, &g_mods[i].hash, 4);
        s32(w, g_mods[i].hash); w[4] = (uint8_t)l; memcpy(w + 5, g_mods[i].name, l); w += 5 + l;
        snprintf(hx, sizeof hx, " (%08x)", g_mods[i].hash);
        size_t t = strlen(g_mods_text);
        snprintf(g_mods_text + t, sizeof g_mods_text - t, "%s%s%s", i ? ", " : "", g_mods[i].name, hx);
    }
    g_mods_wire_len = (int)(w - g_mods_wire);
    g_mods_hash = g_nmods ? (uint32_t)(set ^ set >> 32) : 0;
    if (!g_nmods) snprintf(g_mods_text, sizeof g_mods_text, "none");
}

/* Modes (net_relay.h): LOBBY = our lobby server; LAN = no server: one game of the local network runs the lobby inside
 * itself and announces it with a beacon (multicast 239.255.13.13 + broadcast, UDP port 39501), the others find it;
 * HOST = no server, over the internet: this game runs the lobby inside itself and shows a room code (its public
 * address, learnt by STUN and/or a UPnP port mapping); JOIN = no server: the lobby is the host's game named by the code.
 *   SSBU_ONLINE = 0|off  1|on|auto  lan  host[:<port>]  join:<code or ip:port>  <host:port> (= lobby)
 *   graphics.ini: online_mode = off|auto|lobby|lan|host|join, online_server, online_join, online_peer, online_upnp,
 *   online_stun, online_port. auto: online_server given -> LOBBY (no answer for 5 s -> LAN), otherwise LAN. */
static int g_mode, g_auto, g_upnp = 1, g_list_other = 1;     /* g_list_other: online_list_other (O1) */
static uint16_t g_host_port;                    /* HOST mode: the in-game lobby's UDP port (default 39500) */
static char g_join[64], g_peer[256], g_stun[128] = "stun.l.google.com:19302";

static void configure(void) {
    if (g_cfg >= 0) return;
    char v[256] = "", mode[64] = "";
    const char *e = getenv("SSBU_ONLINE");
    g_cfg = 0;
    if (e) {
        if (is_off(e)) return;
        snprintf(mode, sizeof mode, "%s", e);
    } else if (ini_value("online_mode", v, sizeof v)) {
        if (is_off(v)) return;
        snprintf(mode, sizeof mode, "%s", v);
    } else {                                        /* no online_mode: ON through a lobby, the file's or the public one (2026-10-20) */
        if (ini_value("online_server", v, sizeof v)) { if (is_off(v)) return; }
        else {
            /* scripted test runs (key script, debug match, deterministic mode) stay offline unless they ask */
            if (getenv("SSBU_SEQ") || getenv("SSBU_DEBUG_MATCH") || getenv("SSBU_DETERMINISTIC") || getenv("SSBU_SDK_SELFTEST") ||
                getenv("SSBU_PAD_SELFTEST") || getenv("SSBU_CXXRT_SELFTEST")) return;     /* (self-tests: their socket checks hung on the lobby) */
            snprintf(g_server, sizeof g_server, "%s", NET_PUBLIC_LOBBY);
        }
        snprintf(mode, sizeof mode, "lobby");
    }
    if (!_stricmp(mode, "1") || !_stricmp(mode, "on") || !_stricmp(mode, "auto")) g_auto = 1;
    else if (!_stricmp(mode, "lan")) g_mode = NET_MODE_LAN;
    else if (!_strnicmp(mode, "host", 4)) { g_mode = NET_MODE_HOST; if (mode[4] == ':') g_host_port = (uint16_t)atoi(mode + 5); }
    else if (!_strnicmp(mode, "join", 4)) { g_mode = NET_MODE_JOIN; if (mode[4] == ':') snprintf(g_join, sizeof g_join, "%s", mode + 5); }
    else if (!_stricmp(mode, "lobby")) g_mode = NET_MODE_LOBBY;
    else { g_mode = NET_MODE_LOBBY; snprintf(g_server, sizeof g_server, "%s", mode); }
    if (!g_server[0]) {
        if ((e = getenv("SSBU_ONLINE_SERVER")) && *e) snprintf(g_server, sizeof g_server, "%s", e);
        else if (ini_value("online_server", v, sizeof v) && !is_off(v)) snprintf(g_server, sizeof g_server, "%s", v);
    }
    if (g_auto) g_mode = g_server[0] ? NET_MODE_LOBBY : NET_MODE_LAN;
    if (g_mode == NET_MODE_LOBBY && !g_server[0]) snprintf(g_server, sizeof g_server, "127.0.0.1:%d", VNET_DEFAULT_PORT);
    if (!g_join[0]) { if ((e = getenv("SSBU_ONLINE_JOIN")) && *e) snprintf(g_join, sizeof g_join, "%s", e); else ini_value("online_join", g_join, sizeof g_join); }
    if ((e = getenv("SSBU_ONLINE_PEER")) && *e) snprintf(g_peer, sizeof g_peer, "%s", e); else ini_value("online_peer", g_peer, sizeof g_peer);
    if ((e = getenv("SSBU_ONLINE_UPNP"))) g_upnp = !is_off(e); else if (ini_value("online_upnp", v, sizeof v)) g_upnp = !is_off(v);
    if ((e = getenv("SSBU_ONLINE_STUN"))) snprintf(g_stun, sizeof g_stun, "%s", is_off(e) ? "" : e);
    else if (ini_value("online_stun", v, sizeof v)) snprintf(g_stun, sizeof g_stun, "%s", is_off(v) ? "" : v);
    if (!g_host_port && ini_value("online_port", v, sizeof v) && atoi(v) > 0 && atoi(v) < 65536) g_host_port = (uint16_t)atoi(v);
    if (!g_host_port) g_host_port = VNET_DEFAULT_PORT;
    if (g_mode == NET_MODE_JOIN && !g_join[0]) { printf("[online] join mode without a room code (SSBU_ONLINE=join:<code> or online_join): online play off\n"); fflush(stdout); return; }
    if ((e = getenv("SSBU_ONLINE_NAME")) && *e) snprintf(g_name, sizeof g_name, "%s", e);
    else if (ini_value("online_name", v, sizeof v) && *v) snprintf(g_name, sizeof g_name, "%s", v);
    else if (ini_value("player_name", v, sizeof v) && *v) snprintf(g_name, sizeof g_name, "%s", v);    /* F1 > Experimental > Your name */
    else snprintf(g_name, sizeof g_name, "Player-%04x", (unsigned)(rnd64() & 0xffff));
    if ((e = getenv("SSBU_ONLINE_CODE")) && *e) snprintf(g_code, sizeof g_code, "%s", e);
    else if (ini_value("online_code", v, sizeof v) && *v) snprintf(g_code, sizeof g_code, "%s", v);
    if ((e = getenv("SSBU_ONLINE_DIRECT"))) g_direct = !is_off(e);
    else if (ini_value("online_direct", v, sizeof v)) g_direct = !is_off(v);
    if ((e = getenv("SSBU_ONLINE_PORT")) && atoi(e) > 0 && atoi(e) < 65536) g_local_port = (uint16_t)atoi(e);
    if ((e = getenv("SSBU_ONLINE_LOG"))) g_log = atoi(e);
    mods_scan();                       /* O1: the mods this game loads (their names and hashes go to the lobby) */
    if ((e = getenv("SSBU_ONLINE_BUILD")) && *e) snprintf(g_build, sizeof g_build, "%s", e);
    else {
        size_t l = strlen(g_build);
        if (g_game_build >= 0)         /* players of 13.0.4 and 13.0.5 never share a room: the lobby and the LAN lobby part rooms by build */
            snprintf(g_build + l, sizeof g_build - l, "-%s", g_game_build == 1 ? "13.0.5" : "13.0.4");
        l = strlen(g_build);
        if (g_nmods) snprintf(g_build + l, sizeof g_build - l, "+m%08x", g_mods_hash);      /* nor do different mod sets */
    }
    if ((e = getenv("SSBU_ONLINE_LIST_OTHER"))) g_list_other = !is_off(e);
    else if (ini_value("online_list_other", v, sizeof v)) g_list_other = !is_off(v);
    ladder_config();
    g_cfg = 1;
    static const char *mode_names[] = {"lobby", "lan (no server)", "host (no server)", "join (no server)"};
    printf("[online] on: mode %s%s, lobby %s, code %s, name %s, direct paths %s, build %s\n", mode_names[g_mode], g_auto ? " (auto)" : "",
           g_mode == NET_MODE_LOBBY ? g_server : g_mode == NET_MODE_JOIN ? g_join : "inside this game", g_code, g_name, g_direct ? "allowed" : "off", g_build);
    printf("[online] mods: %s; rooms of other builds %s\n", g_mods_text, g_list_other ? "shown in the game's list (joining them is refused, with the reason)" : "only in F1 > Online");
    fflush(stdout);
}
int net_online(void) { configure(); return g_cfg; }
int net_log_level(void) { configure(); return g_cfg ? g_log : 0; }

static void say(const char *fmt, ...) {
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    printf("[online] %s\n", buf);
    fflush(stdout);
}

/* ---------------------------------------------------------------- state */
static SOCKET g_sock = INVALID_SOCKET;
static struct sockaddr_in g_lobby;
static volatile LONG g_joined, g_players, g_rooms, g_ping = -1, g_searching = -1, g_code_players = -1;
static volatile uint32_t g_ip;
static uint64_t g_nonce, g_cookie, g_last_pong, g_unjoined_since;
static uint32_t g_lan_ip;
static uint16_t g_lan_port;
static int g_loopback;
static char g_reject[80];
static volatile LONG64 g_out_relay, g_out_direct, g_in_relay, g_in_direct;

enum { PS_PUNCHING, PS_DIRECT, PS_RELAYED };
typedef struct {
    struct sockaddr_in at;
    uint64_t hello, hello_at, prev, prev_at;          /* our last two hellos there */
} Cand;
typedef struct {
    uint32_t ip;
    char name[NAME_LEN + 1];
    uint64_t secret, since, last_ack, next_hello, acks_window;
    int state, via, ncand, acks, rtt;
    Cand cand[2];                                     /* through its router (as the lobby sees it), then on its own LAN */
} Peer;
static SRWLOCK g_peers_lock = SRWLOCK_INIT;
static Peer g_peers[MAX_PEERS];
static int g_npeers;
static uint32_t g_peers_version;

static int same_at(const struct sockaddr_in *a, const struct sockaddr_in *b) { return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port; }
static const char *at_text(const struct sockaddr_in *a, char *buf) {          /* buf: 24 bytes */
    char ip[16];
    inet_ntop(AF_INET, (void *)&a->sin_addr, ip, sizeof ip);
    snprintf(buf, 24, "%s:%u", ip, ntohs(a->sin_port));
    return buf;
}
static int fresh(const Peer *q, uint64_t t) { return q->state == PS_DIRECT && t - q->last_ack < FRESH_MS; }

uint32_t net_my_ip(void) { return g_joined ? g_ip : 0; }
int net_lobby_gone(void) { return g_cfg > 0 && !g_joined && ms_now() - g_unjoined_since > GONE_AFTER_MS; }

static void tx_now(const void *p, int n, const struct sockaddr_in *to) { sendto(g_sock, (const char *)p, n, 0, (const struct sockaddr *)to, sizeof *to); }

/* TEST SWITCH SSBU_NET_SIM=<ms>[,<loss %>[,<jitter ms>]] (R3, default off): every datagram this game sends is held <ms>
 * (+ 0..<jitter> ms, order kept) and <loss> % of them are dropped, so two games on one PC see a long-distance link:
 * both with SSBU_NET_SIM=75 = 150 ms round trip between them. Only this game's client socket (not the in-game lobby). */
typedef struct SimPkt { struct SimPkt *next; double due; int n; struct sockaddr_in to; uint8_t b[]; } SimPkt;
static int g_sim = -1; static double g_sim_ms, g_sim_loss, g_sim_jit, g_sim_last_due; static uint32_t g_sim_rng = 0x2545F491u;
static SimPkt *g_sim_head, *g_sim_tail; static SRWLOCK g_sim_lock = SRWLOCK_INIT; static CONDITION_VARIABLE g_sim_cv = CONDITION_VARIABLE_INIT;
static volatile LONG64 g_sim_sent, g_sim_dropped;
static double sim_now(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / f.QuadPart; }
static DWORD WINAPI sim_thread(LPVOID u) {
    (void)u;
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, 0x2 /* high resolution */, TIMER_ALL_ACCESS);
    if (!tm) tm = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    for (;;) {
        AcquireSRWLockExclusive(&g_sim_lock);
        while (!g_sim_head) SleepConditionVariableSRW(&g_sim_cv, &g_sim_lock, INFINITE, 0);
        SimPkt *q = g_sim_head; double due = q->due, now = sim_now();
        if (now >= due) { g_sim_head = q->next; if (!g_sim_head) g_sim_tail = NULL; }
        ReleaseSRWLockExclusive(&g_sim_lock);
        if (now < due) {
            double left = due - now;
            if (left > 1.2 && tm) { LARGE_INTEGER d; d.QuadPart = -(LONGLONG)((left - 0.8) * 10000.0); SetWaitableTimer(tm, &d, 0, NULL, NULL, FALSE); WaitForSingleObject(tm, 1000); }
            else Sleep(0);
            continue;
        }
        tx_now(q->b, q->n, &q->to); InterlockedIncrement64(&g_sim_sent);
        free(q);
    }
    return 0;
}
static void sim_init(void) {                           /* (net_start, before the client thread) */
    const char *e = getenv("SSBU_NET_SIM");
    g_sim = 0;
    if (e && *e && atof(e) > 0) {
        g_sim = 1; sscanf(e, "%lf,%lf,%lf", &g_sim_ms, &g_sim_loss, &g_sim_jit);
        HANDLE th = CreateThread(NULL, 0, sim_thread, NULL, 0, NULL); if (th) CloseHandle(th); else g_sim = 0;
        printf("[online] TEST: link simulation on: +%.1f ms per datagram sent (+0..%.1f jitter), %.1f %% lost\n", g_sim_ms, g_sim_jit, g_sim_loss); fflush(stdout);
    }
}
static void tx(const void *p, int n, const struct sockaddr_in *to) {
    if (g_sim <= 0) { tx_now(p, n, to); return; }
    g_sim_rng ^= g_sim_rng << 13; g_sim_rng ^= g_sim_rng >> 17; g_sim_rng ^= g_sim_rng << 5;
    if (g_sim_loss > 0 && (g_sim_rng % 100000) < (uint32_t)(g_sim_loss * 1000)) { InterlockedIncrement64(&g_sim_dropped); return; }
    SimPkt *q = malloc(sizeof *q + (size_t)n);
    if (!q) return;
    q->next = NULL; q->n = n; q->to = *to; memcpy(q->b, p, (size_t)n);
    double jit = g_sim_jit > 0 ? g_sim_jit * ((g_sim_rng >> 8) % 1000) / 1000.0 : 0;
    AcquireSRWLockExclusive(&g_sim_lock);
    q->due = sim_now() + g_sim_ms + jit;
    if (q->due < g_sim_last_due) q->due = g_sim_last_due;      /* (order kept) */
    g_sim_last_due = q->due;
    if (g_sim_tail) g_sim_tail->next = q; else g_sim_head = q;
    g_sim_tail = q;
    ReleaseSRWLockExclusive(&g_sim_lock);
    WakeConditionVariable(&g_sim_cv);
}

/* R3 online rollback (rb_net.c): its messages, packet type 16 through the lobby (here: the host's game).
 *   client -> lobby: 'S' 'U' 1 16 | cookie u64 | destination u32 (0 = the other players of this lobby code) | message
 *   lobby -> client: 'S' 'U' 1 16 | source u32 | message */
#define T_RB 16
void rbn_rx(const uint8_t *p, int n);                  /* rb_net.c */
static volatile uint32_t g_rb_peer;
int net_rb_ready(void) { return g_cfg > 0 && g_joined && g_sock != INVALID_SOCKET; }
/* (R3 pub1, testers 2026-10-20: "messages sent 0, received 0" for 120 s with the other player in the room) the destination was
 * only learnt from a message received (g_rb_peer), so the joiner's first messages went to "0 = every other player of this lobby
 * code", which the PUBLIC lobby drops (tools_net/ssbu_lobby.py: destination 0 only with a private code): neither game ever
 * heard the other (the host answers only a CHOICE). Now the destination is the other player of this Local Wireless room
 * (nnldn.c), else the address learnt; 0 on the public code of a lobby SERVER = nothing can arrive (the in-game lobbies of HOST / JOIN / LAN pass 0 on): not sent (counted, the notice says so). */
uint32_t nnldn_other_ip(void);
static const char *ip_text(uint32_t ip, char *b);
static volatile uint32_t g_rb_match_peer;              /* (R3 net3) the opponent of the last rollback-match search (lobby 1.4), 0 = none */
static volatile LONG64 g_rb_rx_other;                  /* (R3 net3) rollback messages from another player than that opponent: dropped */
static volatile LONG g_rb_nodst; static volatile uint32_t g_rb_dst_said;
int net_rb_no_destination(void) { return g_rb_nodst; }
/* (2026-10-30, rooms: "the connection should be p2p, the host will be just for the room") the messages of a rollback match the
 * lobby paired (MATCH_STATE 'A': the opponent's address) go STRAIGHT to the opponent: 26 RB_DIRECT = hdr, sender's lobby
 * address u32, the message. Until a direct message has come back they also go through the lobby (as before), and again
 * whenever none came for RBD_STALE_MS (a firewall, Radmin down): never worse than relayed. The lobby only carries the room. */
#define T_RB_DIRECT 26
#define RBD_STALE_MS 1500
static SRWLOCK g_rbd_lock = SRWLOCK_INIT;
static struct sockaddr_in g_rbd_at;                     /* (g_rbd_lock) the opponent's address; sin_port 0 = none */
static uint32_t g_rbd_peer;                             /* (g_rbd_lock) its lobby address (10.13.x.x) */
static volatile LONG64 g_rbd_last_rx;                   /* when a direct message of it last came */
static volatile LONG64 g_rbd_tx, g_rbd_rx;
static volatile LONG g_rbd_said;
static void rbd_set(uint32_t peer, uint32_t ip, uint16_t port) {   /* (client thread) MATCH_STATE 'A' */
    AcquireSRWLockExclusive(&g_rbd_lock);
    int same = g_rbd_peer == peer && g_rbd_at.sin_port == htons(port);
    memset(&g_rbd_at, 0, sizeof g_rbd_at);
    g_rbd_at.sin_family = AF_INET;
    g_rbd_at.sin_addr.s_addr = ip ? htonl(ip) : g_lobby.sin_addr.s_addr;      /* (0: the game on the lobby's PC) */
    g_rbd_at.sin_port = htons(port);
    g_rbd_peer = peer;
    ReleaseSRWLockExclusive(&g_rbd_lock);
    if (!same) { char a[16]; InterlockedExchange64(&g_rbd_last_rx, 0); InterlockedExchange(&g_rbd_said, 0);
        say("match: the opponent's address for a direct connection: %s:%u", ip_text(ntohl(g_rbd_at.sin_addr.s_addr), a), port); }
}
static void rbd_rx(const uint8_t *b, int n, const struct sockaddr_in *from) {      /* (client thread) RB_DIRECT */
    if (n <= 8 || !g_joined) return;
    AcquireSRWLockShared(&g_rbd_lock);
    int ok = g_rbd_at.sin_port && from->sin_addr.s_addr == g_rbd_at.sin_addr.s_addr && g32(b + 4) == g_rbd_peer && g_rbd_peer == g_rb_match_peer;
    ReleaseSRWLockShared(&g_rbd_lock);
    if (!ok) return;
    InterlockedExchange64(&g_rbd_last_rx, (LONG64)ms_now()); InterlockedIncrement64(&g_rbd_rx);
    if (!g_rbd_said) { InterlockedExchange(&g_rbd_said, 1); char a[16]; say("match: DIRECT connection with the opponent (%s): the match no longer goes through the lobby", ip_text(ntohl(from->sin_addr.s_addr), a)); }
    g_rb_peer = g32(b + 4);
    rbn_rx(b + 8, n - 8);
}
int net_rb_direct(void) { return g_rbd_last_rx && ms_now() - (uint64_t)g_rbd_last_rx < RBD_STALE_MS; }    /* 1 = the match goes player to player now */
int net_rb_send(const void *p, int n) {
    if (!net_rb_ready() || n <= 0 || n > 1400) return 0;
    uint32_t dst = nnldn_other_ip(), room = dst;
    if (!dst) dst = g_rb_match_peer;                    /* (R3 net3) the opponent the lobby found for this rollback match */
    if (!dst) dst = g_rb_peer;
    if (!dst && g_mode == NET_MODE_LOBBY && !_stricmp(g_code, "public")) { InterlockedIncrement(&g_rb_nodst); return 0; }
    if (dst && dst != g_rb_dst_said) { char a[16]; g_rb_dst_said = dst; printf("[rbnet] rollback messages go to %s (%s)\n", ip_text(dst, a), room ? "the other player of this Local Wireless room" : dst == g_rb_match_peer ? "the opponent the lobby found" : "the game that wrote to this one"); fflush(stdout); }
    uint8_t b[1500];
    if (dst && !room && dst == g_rb_match_peer) {       /* (direct) straight to the opponent */
        struct sockaddr_in at; uint32_t peer;
        AcquireSRWLockShared(&g_rbd_lock); at = g_rbd_at; peer = g_rbd_peer; ReleaseSRWLockShared(&g_rbd_lock);
        if (at.sin_port && peer == dst) {
            hdr(b, T_RB_DIRECT); s32(b + 4, g_ip); memcpy(b + 8, p, (size_t)n);
            tx(b, 8 + n, &at); InterlockedIncrement64(&g_rbd_tx);
            static unsigned keep;                       /* (the other way works too: the lobby is not needed; 1 in 30 still goes through it, so the */
            if (net_rb_direct() && ++keep % 30) return 1;   /* room sees the match being played: net_minilobby.c rb_seen / ml_arena_playing) */
        }
    }
    hdr(b, T_RB); s64(b + 4, g_cookie); s32(b + 12, dst);
    memcpy(b + 16, p, (size_t)n);
    tx(b, 16 + n, &g_lobby);
    return 1;
}

/* ladder (lobby 1.5): the result of a rollback match (ladder_report.c), sent every 2 s until the lobby acknowledges it
 * (RESULT_ACK) or LD_TRIES times (an older lobby, or the in-game lobbies of HOST / LAN / JOIN: no ladder there).
 *   22 RESULT     client: cookie u64, match seed u64, the other player's address u32, slot u8, rule kind u8, stocks of the
 *                         rule u8, flags u8, stage u32, chara u64 x 2, stocks left u8 x 2, damage (tenths) u16 x 2, frames u32,
 *                         desyncs u32
 *   23 RESULT_ACK lobby:  match seed u64, outcome u8 (0 waiting for the other game, 1 counted, 2 draw, 3 disputed, 4 not
 *                         ranked), rating u16, RD u16, wins u16, losses u16, place u16, set score (mine u8, the other's u8) */
#define T_RESULT 22
#define T_RESULT_ACK 23
#define LD_TRIES 30
static uint8_t g_ld_res[62];
static volatile LONG g_ld_res_left;                     /* sends left (0: nothing to send) */
static uint64_t g_ld_res_seed, g_ld_res_tx;
void net_ladder_result(const NetLadderResult *r) {
    uint32_t dst = nnldn_other_ip();
    if (!dst) dst = g_rb_match_peer;
    if (!dst) dst = g_rb_peer;
    if (!g_ld_on || !net_rb_ready() || !dst || !r->seed) return;
    uint8_t *p = g_ld_res;
    InterlockedExchange(&g_ld_res_left, 0);
    hdr(p, T_RESULT); s64(p + 4, g_cookie); s64(p + 12, r->seed); s32(p + 20, dst);
    p[24] = (uint8_t)r->slot; p[25] = (uint8_t)r->rule_kind; p[26] = (uint8_t)r->stock; p[27] = (uint8_t)r->flags;
    s32(p + 28, r->stage); s64(p + 32, r->chara[0]); s64(p + 40, r->chara[1]);
    p[48] = (uint8_t)r->stocks[0]; p[49] = (uint8_t)r->stocks[1]; s16(p + 50, (uint16_t)r->dmg10[0]); s16(p + 52, (uint16_t)r->dmg10[1]);
    s32(p + 54, r->frames); s32(p + 58, r->desyncs);
    g_ld_res_seed = r->seed; g_ld_res_tx = 0;
    AcquireSRWLockExclusive(&g_ld_lock); g_ld.last_outcome = -1; g_ld.set_mine = g_ld.set_theirs = 0; ReleaseSRWLockExclusive(&g_ld_lock);
    InterlockedExchange(&g_ld_res_left, LD_TRIES);
    say("ladder: result of match %016llx sent: player %d, stocks %d / %d, damage %.1f / %.1f%%, %u frames, %u desyncs, flags %d", (unsigned long long)r->seed, r->slot + 1,
        r->stocks[0], r->stocks[1], r->dmg10[0] / 10.0, r->dmg10[1] / 10.0, r->frames, r->desyncs, r->flags);
}
static void ladder_tick(uint64_t t) {                   /* (client thread, joined) */
    if (g_ld_res_left <= 0 || t - g_ld_res_tx < 2000) return;
    s64(g_ld_res + 4, g_cookie);                        /* (joined again since: the new cookie) */
    tx(g_ld_res, sizeof g_ld_res, &g_lobby);
    g_ld_res_tx = t;
    if (InterlockedDecrement(&g_ld_res_left) == 0) say("ladder: no answer to the result (an older lobby, or no ladder there)");
}
static void ladder_ack(const uint8_t *p, int n) {       /* (client thread) a RESULT_ACK body */
    if (n < 9 + 10 + 2 || g64(p) != g_ld_res_seed) return;
    static const char *const out[] = {"waiting for the other game's result", "counted", "a draw (not counted)", "disputed (not counted)", "not ranked"};
    int o = p[8] <= 4 ? p[8] : 4;
    InterlockedExchange(&g_ld_res_left, 0);
    AcquireSRWLockExclusive(&g_ld_lock);
    int was = g_ld.last_outcome;
    g_ld.last_outcome = o;
    if (g_ld.state == NET_LADDER_RANKED) ladder_numbers(p + 9);
    g_ld.set_mine = p[19]; g_ld.set_theirs = p[20];
    ReleaseSRWLockExclusive(&g_ld_lock);
    if (was != o) say("ladder: match %016llx: %s; set %d-%d", (unsigned long long)g64(p), out[o], p[19], p[20]);
}
void net_ladder_get(NetLadder *o) {
    AcquireSRWLockShared(&g_ld_lock);
    *o = g_ld;
    ReleaseSRWLockShared(&g_ld_lock);
    if (g_cfg <= 0) o->state = NET_LADDER_OFF;
}

/* ---------------------------------------------------------------- direct paths */
static void punch(Peer *q, int c, int ack, uint64_t word) {     /* (peers lock held) */
    uint8_t p[4 + 8 + 4 + 4 + 1 + 8];
    hdr(p, T_PUNCH);
    s64(p + 4, q->secret);
    s32(p + 12, g_ip);
    s32(p + 16, q->ip);
    p[20] = (uint8_t)ack;
    s64(p + 21, word);
    tx(p, sizeof p, &q->cand[c].at);
}
static void hello(Peer *q, int c, uint64_t t) {
    Cand *k = &q->cand[c];
    k->prev = k->hello; k->prev_at = k->hello_at;
    k->hello = rnd64() | 1; k->hello_at = t;
    punch(q, c, 0, k->hello);
}

static void read_peers(const uint8_t *p, int n) {               /* PEERS body */
    if (n < 5) return;
    uint32_t version = g32(p);
    int count = p[4];
    if ((int32_t)(version - g_peers_version) <= 0 || count > MAX_PEERS || 5 + count * PEER_LEN > n) return;
    uint64_t t = ms_now();
    Peer next[MAX_PEERS];
    char lines[MAX_PEERS][120];
    int nl = 0;
    memset(next, 0, sizeof next);
    AcquireSRWLockExclusive(&g_peers_lock);
    for (int i = 0; i < count; i++) {
        const uint8_t *e = p + 5 + i * PEER_LEN, *a = e + 4 + NAME_LEN;
        Peer *q = &next[i];
        q->ip = g32(e);
        for (int k = 0; k < NAME_LEN; k++) q->name[k] = e[4 + k] && e[4 + k] < 0x20 ? '?' : (char)e[4 + k];
        q->secret = g64(a + 12);
        q->ncand = 1;
        q->cand[0].at.sin_family = AF_INET;
        q->cand[0].at.sin_addr.s_addr = htonl(g32(a));
        q->cand[0].at.sin_port = htons(g16(a + 4));
        if (g32(a + 6) && g16(a + 10) && (g32(a + 6) != g32(a) || g16(a + 10) != g16(a + 4))) {
            q->cand[1].at.sin_family = AF_INET;
            q->cand[1].at.sin_addr.s_addr = htonl(g32(a + 6));
            q->cand[1].at.sin_port = htons(g16(a + 10));
            q->ncand = 2;
        }
        const Peer *old = NULL;
        for (int j = 0; j < g_npeers && !old; j++) if (g_peers[j].ip == q->ip && g_peers[j].secret == q->secret) old = &g_peers[j];
        int same = old && old->ncand == q->ncand;
        for (int c = 0; same && c < q->ncand; c++) same = same_at(&old->cand[c].at, &q->cand[c].at);
        if (same) {
            q->state = old->state; q->via = old->via; q->since = old->since; q->last_ack = old->last_ack; q->rtt = old->rtt;
            q->next_hello = old->next_hello; q->acks = old->acks; q->acks_window = old->acks_window;
            memcpy(q->cand, old->cand, sizeof q->cand);
        } else {
            char at[24];
            q->state = PS_PUNCHING;
            q->since = q->next_hello = t;
            if (nl < MAX_PEERS) snprintf(lines[nl++], sizeof lines[0], "%s (10.13.%u.%u) introduced at %s%s: punching", q->name,
                                         q->ip >> 8 & 255, q->ip & 255, at_text(&q->cand[0].at, at), q->ncand > 1 ? " (or its LAN address)" : "");
        }
    }
    memcpy(g_peers, next, sizeof next);
    g_npeers = count;
    g_peers_version = version;
    ReleaseSRWLockExclusive(&g_peers_lock);
    for (int i = 0; i < nl; i++) say("%s", lines[i]);
}

static void peers_tick(uint64_t t) {
    static uint64_t last_report;
    char lines[2 * MAX_PEERS][160];
    int nl = 0;
    AcquireSRWLockExclusive(&g_peers_lock);
    for (int i = 0; i < g_npeers; i++) {
        Peer *q = &g_peers[i];
        if (q->state == PS_DIRECT && t - q->last_ack >= FRESH_MS) {
            q->state = PS_RELAYED; q->since = t;
            snprintf(lines[nl++], sizeof lines[0], "direct path to %s went quiet: through the lobby", q->name);
        } else if (q->state == PS_PUNCHING && t - q->since >= PUNCH_FOR_MS) {
            q->state = PS_RELAYED;
            snprintf(lines[nl++], sizeof lines[0], "no direct path to %s (router or firewall): through the lobby", q->name);
        }
        if (t >= q->next_hello) {
            q->next_hello = t + (q->state == PS_PUNCHING ? 200 : q->state == PS_DIRECT || t - q->since < PUNCH_FOR_MS ? 400 : 5000);
            if (q->state == PS_DIRECT) hello(q, q->via, t);
            else for (int c = 0; c < q->ncand; c++) hello(q, c, t);
        }
    }
    if (g_log >= 1 && t - last_report >= 10000 && (g_out_relay || g_out_direct || g_in_relay || g_in_direct)) {
        last_report = t;
        snprintf(lines[nl++], sizeof lines[0], "traffic: out %lld relayed + %lld direct, in %lld relayed + %lld direct; %d peer(s)%s",
                 (long long)g_out_relay, (long long)g_out_direct, (long long)g_in_relay, (long long)g_in_direct, g_npeers,
                 g_npeers ? (fresh(&g_peers[0], t) ? ", first direct" : ", first relayed") : "");
    }
    ReleaseSRWLockExclusive(&g_peers_lock);
    for (int i = 0; i < nl; i++) say("%s", lines[i]);
}

/* (peers lock held) the introduced player whose secret this is, if it came from one of its addresses */
static Peer *peer_at(uint64_t secret, const struct sockaddr_in *from, int *cand) {
    for (int i = 0; i < g_npeers; i++) {
        if (g_peers[i].secret != secret) continue;
        for (int c = 0; c < g_peers[i].ncand; c++) if (same_at(&g_peers[i].cand[c].at, from)) { *cand = c; return &g_peers[i]; }
        return NULL;
    }
    return NULL;
}

static void from_player(const uint8_t *p, int n, const struct sockaddr_in *from) {
    uint64_t t = ms_now();
    int c = 0;
    if (n < 12) return;
    uint64_t secret = g64(p + 4);
    if (p[3] == T_DIRECT && n >= 12 + 12) {
        const uint8_t *b = p + 12;
        uint32_t src = g32(b), dst = g32(b + 4);
        uint16_t sport = g16(b + 8), dport = g16(b + 10);
        int ok;
        AcquireSRWLockShared(&g_peers_lock);
        Peer *q = peer_at(secret, from, &c);
        ok = q && q->ip == src && (dst == g_ip || dst == VNET_BROADCAST || dst == 0xFFFFFFFFu) && sport != VNET_LDN_PORT && dport != VNET_LDN_PORT;
        ReleaseSRWLockShared(&g_peers_lock);
        if (ok && nnldn_has_node(src)) {
            InterlockedIncrement64(&g_in_direct);
            nnldn_deliver(src, sport, dst, dport, b + 12, (uint32_t)(n - 24));
        }
        return;
    }
    if (p[3] != T_PUNCH || n < 29) return;
    uint32_t from_ip = g32(p + 12), to_ip = g32(p + 16);
    int ack = p[20];
    uint64_t word = g64(p + 21);
    char line[160] = "";
    AcquireSRWLockExclusive(&g_peers_lock);
    Peer *q = peer_at(secret, from, &c);
    if (q && q->ip == from_ip && to_ip == g_ip) {
        Cand *k = &q->cand[c];
        if (ack) {
            uint64_t asked = word == k->hello ? k->hello_at : word == k->prev ? k->prev_at : 0;
            if (word && asked && t - asked < 2000) {
                if (word == k->hello) k->hello = 0; else k->prev = 0;
                q->rtt = (int)(t - asked);
                q->last_ack = t;
                if (q->state != PS_DIRECT || (c == 1 && q->via == 0)) {
                    char at[24];
                    q->state = PS_DIRECT;
                    q->via = c;
                    snprintf(line, sizeof line, "%s: direct path via %s %s (%d ms)", q->name, c ? "LAN" : "public", at_text(&k->at, at), q->rtt);
                }
            }
        } else {
            if (t - q->acks_window >= 1000) { q->acks_window = t; q->acks = 0; }
            if (q->acks++ < 10) {
                punch(q, c, 1, word);
                if (q->state != PS_DIRECT) hello(q, c, t);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_peers_lock);
    if (line[0]) say("%s", line);
}

/* ---------------------------------------------------------------- sending */
void net_send(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, uint32_t len) {
    if (!g_joined || g_sock == INVALID_SOCKET || len > 65000) return;
    enum { HEAD_ROOM = 4 + 1 + 4 * MAX_PEERS };
    uint8_t stack[2048], *buf = HEAD_ROOM + 12 + len <= sizeof stack ? stack : malloc(HEAD_ROOM + 12 + len);
    if (!buf) return;
    uint8_t *body = buf + HEAD_ROOM, *h;
    s32(body, src); s32(body + 4, dst); s16(body + 8, sport); s16(body + 10, dport);
    memcpy(body + 12, data, len);
    int n = 12 + (int)len, bcast = dst == VNET_BROADCAST || dst == 0xFFFFFFFFu, nd = 0;
    struct { struct sockaddr_in at; uint64_t secret; uint32_t ip; } direct[MAX_PEERS];
    if (sport != VNET_LDN_PORT && dport != VNET_LDN_PORT) {
        uint64_t t = ms_now();
        AcquireSRWLockShared(&g_peers_lock);
        for (int i = 0; i < g_npeers; i++) {
            Peer *q = &g_peers[i];
            if (!bcast && q->ip != dst) continue;
            if (fresh(q, t)) { direct[nd].at = q->cand[q->via].at; direct[nd].secret = q->secret; direct[nd++].ip = q->ip; }
        }
        ReleaseSRWLockShared(&g_peers_lock);
    }
    for (int i = 0; i < nd; i++) {
        h = body - 12;
        hdr(h, T_DIRECT);
        s64(h + 4, direct[i].secret);
        tx(h, n + 12, &direct[i].at);
        InterlockedIncrement64(&g_out_direct);
    }
    if (!nd || bcast) {                         /* a broadcast also reaches the room's players not reached directly: the lobby sends it on */
        if (nd) {
            h = body - (5 + 4 * nd);
            hdr(h, T_BCAST);
            h[4] = (uint8_t)nd;
            for (int i = 0; i < nd; i++) s32(h + 5 + 4 * i, direct[i].ip);
        } else {
            h = body - 4;
            hdr(h, T_DATA);
        }
        tx(h, (int)(body - h) + n, &g_lobby);
        InterlockedIncrement64(&g_out_relay);
    }
    if (g_log >= 3) printf("[online] send %u bytes 10.13.%u.%u:%u -> %u.%u.%u.%u:%u (%d direct)\n", len, src >> 8 & 255, src & 255, sport,
                           dst >> 24, dst >> 16 & 255, dst >> 8 & 255, dst & 255, dport, nd);
    if (buf != stack) free(buf);
}

static volatile LONG64 g_rooms_wanted;                /* O1: when the room browser or the game's Rooms List last asked */
void net_list_rooms(uint32_t seq) {
    if (!g_joined) return;
    uint8_t p[17];
    hdr(p, T_LIST);
    s64(p + 4, g_cookie);
    s32(p + 12, seq);
    p[16] = (uint8_t)(g_list_other ? 1 : 0);        /* O1: also the rooms of other builds (an older lobby ignores the byte) */
    tx(p, sizeof p, &g_lobby);
    InterlockedExchange64(&g_rooms_wanted, (LONG64)ms_now());
}

/* ---------------------------------------------------------------- O1: the room browser (every room of this lobby code) */
void menu_popup(const char *title, const char *text);   /* menu.cpp (menu_stub.c: the log only) */
#define ROOMS_MAX 32
static SRWLOCK g_rooms_lock = SRWLOCK_INIT;
static NetRoom g_room_list[ROOMS_MAX], g_room_next[ROOMS_MAX];
static int g_nroom_list, g_nroom_next, g_rooms_answered;
static uint32_t g_rooms_seq;
static uint64_t g_rooms_sent, g_rooms_at;
static struct { uint32_t ip; char build[BUILD_LEN + 1]; } g_other[24];   /* the last game list's rooms of other builds */
static int g_nother;

static const char *build_version(const char *b, char *out, size_t n) {   /* "ssbu-recomp-ldn1-13.0.5+m..." -> "13.0.5" */
    const char *plus = strchr(b, '+'), *end = plus ? plus : b + strlen(b), *dash = end;
    while (dash > b && dash[-1] != '-') dash--;
    snprintf(out, n, "%.*s", (int)(end - dash), dash);
    return out;
}
static int build_base_len(const char *b) { const char *plus = strchr(b, '+'); return (int)(plus ? plus - b : (ptrdiff_t)strlen(b)); }
/* a mod-set block (count u8, count x (hash u32, length u8, name)) into entries; -1 if it is not one */
static int mods_read(const uint8_t *p, int n, ModEntry *out, int max) {
    if (n < 1 || p[0] > MODS_MAX) return -1;
    int count = p[0], k = 0, o = 1;
    for (int i = 0; i < count; i++) {
        if (o + 5 > n || p[o + 4] > MOD_NAME || o + 5 + p[o + 4] > n) return -1;
        if (k < max) {
            out[k].hash = g32(p + o);
            int l = p[o + 4];
            for (int c = 0; c < l; c++) out[k].name[c] = p[o + 5 + c] >= 0x20 && p[o + 5 + c] < 0x7f ? (char)p[o + 5 + c] : '?';
            out[k].name[l] = 0;
            k++;
        }
        o += 5 + p[o + 4];
    }
    return k;
}
static void list_join(char *out, size_t n, const char *what) {
    size_t t = strlen(out);
    snprintf(out + t, n - t, "%s%s", t ? ", " : "", what);
}
/* why this game cannot join a room of build b with mods m (known: the room's game named its mods); 0 = it can */
static int room_why(const char *b, int known, const ModEntry *m, int nm, char *out, size_t n) {
    out[0] = 0;
    if (!strcmp(b, g_build)) return 0;
    char theirs[24], mine[24];
    if (build_base_len(b) != build_base_len(g_build) || strncmp(b, g_build, (size_t)build_base_len(b))) {
        build_version(b, theirs, sizeof theirs); build_version(g_build, mine, sizeof mine);
        if (theirs[0] && mine[0] && strcmp(theirs, mine) && !strncmp(b, g_build, (size_t)(build_base_len(b) - (int)strlen(theirs))))
            snprintf(out, n, "Different version: this room is %s, you have %s", theirs, mine);
        else snprintf(out, n, "Different game build: this room is %.*s, you have %.*s", build_base_len(b), b, build_base_len(g_build), g_build);
        return 1;
    }
    if (!strchr(b, '+')) { known = 1; nm = 0; }      /* the room's game loads no mods */
    if (known < 0) { snprintf(out, n, "Different mods: this room uses another set of mods (F1 > Online names them)"); return 1; }
    if (!known) { snprintf(out, n, "Different mods: this room's game uses other mods (it is an older version that does not name them)"); return 1; }
    char missing[256] = "", other[256] = "", extra[256] = "";
    for (int i = 0; i < nm; i++) {
        int found = -1;
        for (int k = 0; k < g_nmods && found < 0; k++) if (!_stricmp(m[i].name, g_mods[k].name)) found = k;
        if (found < 0) list_join(missing, sizeof missing, m[i].name);
        else if (g_mods[found].hash != m[i].hash) list_join(other, sizeof other, m[i].name);
    }
    for (int k = 0; k < g_nmods; k++) {
        int found = 0;
        for (int i = 0; i < nm && !found; i++) found = !_stricmp(m[i].name, g_mods[k].name);
        if (!found) list_join(extra, sizeof extra, g_mods[k].name);
    }
    size_t t = 0;
    if (missing[0]) t += (size_t)snprintf(out + t, n - t, "MISSING: %s", missing);
    if (other[0] && t < n) t += (size_t)snprintf(out + t, n - t, "%sANOTHER COPY: %s (yours differs from the room's)", t ? "\n" : "", other);
    if (extra[0] && t < n) t += (size_t)snprintf(out + t, n - t, "%sNOT IN THIS ROOM: %s (turn them off to join)", t ? "\n" : "", extra);
    if (!t) snprintf(out, n, "Different mods (the same names, another set)");
    return 1;
}
static void rooms_ask(uint64_t t) {                 /* (client thread) */
    uint8_t p[16];
    hdr(p, T_ROOMS);
    s64(p + 4, g_cookie);
    s32(p + 12, ++g_rooms_seq);
    g_nroom_next = 0;
    tx(p, sizeof p, &g_lobby);
    g_rooms_sent = t;
}
static void rooms_info(const uint8_t *p, int n) {   /* (client thread) ROOM_INFO body */
    if (n < 4 + 4 + 3 + NAME_LEN + BUILD_LEN + 1 || g32(p) != g_rooms_seq || g_nroom_next >= ROOMS_MAX) return;
    NetRoom *r = &g_room_next[g_nroom_next];
    ModEntry m[MODS_MAX];
    memset(r, 0, sizeof *r);
    r->host = g32(p + 4);
    r->players = p[8]; r->most = p[9];
    r->open = p[10] & 1; r->mine = (p[10] & 2) != 0;
    int known = (p[10] & 4) != 0;
    for (int k = 0; k < NAME_LEN; k++) r->name[k] = p[11 + k] && p[11 + k] < 0x20 ? '?' : (char)p[11 + k];
    memcpy(r->build, p + 11 + NAME_LEN, BUILD_LEN);
    build_version(r->build, r->version, sizeof r->version);
    int nm = known ? mods_read(p + 11 + NAME_LEN + BUILD_LEN, n - (11 + NAME_LEN + BUILD_LEN), m, MODS_MAX) : 0;
    if (nm < 0) { nm = 0; known = 0; }
    r->mods_known = known;
    r->nmods = nm;
    for (int i = 0; i < nm; i++) list_join(r->mods, sizeof r->mods, m[i].name);
    if (!known && strchr(r->build, '+')) snprintf(r->mods, sizeof r->mods, "(not named: an older game)");
    r->joinable = !room_why(r->build, known, m, nm, r->why, sizeof r->why);
    g_nroom_next++;
}
static void rooms_end(const uint8_t *p, int n) {    /* (client thread) ROOMS_END body */
    if (n < 5 || g32(p) != g_rooms_seq) return;
    AcquireSRWLockExclusive(&g_rooms_lock);
    memcpy(g_room_list, g_room_next, sizeof g_room_list[0] * (size_t)g_nroom_next);
    g_nroom_list = g_nroom_next;
    g_rooms_answered = 1;
    g_rooms_at = ms_now();
    ReleaseSRWLockExclusive(&g_rooms_lock);
    static int said = -1;
    if (g_nroom_list != said && g_log >= 1) {
        int other = 0;
        for (int i = 0; i < g_nroom_list; i++) other += !g_room_list[i].joinable && !g_room_list[i].mine;
        say("rooms of this lobby code: %d (%d of another version or mod set)", g_nroom_list, other);
        said = g_nroom_list;
    }
}
static void listing_build(uint32_t host, const char *b) {     /* (client thread) the build of a room of the game's list */
    AcquireSRWLockExclusive(&g_rooms_lock);
    int i = 0;
    while (i < g_nother && g_other[i].ip != host) i++;
    if (strcmp(b, g_build)) {
        if (i == g_nother && g_nother < 24) g_nother++;
        if (i < g_nother) { g_other[i].ip = host; snprintf(g_other[i].build, sizeof g_other[i].build, "%s", b); }
    } else if (i < g_nother) g_other[i] = g_other[--g_nother];
    ReleaseSRWLockExclusive(&g_rooms_lock);
}
int net_rooms_get(NetRoom *out, int max, int *state) {
    InterlockedExchange64(&g_rooms_wanted, (LONG64)ms_now());
    int n = 0;
    if (state) *state = g_cfg <= 0 ? 0 : !g_joined ? 1 : g_rooms_answered ? 3 : g_rooms_sent && ms_now() - g_rooms_sent > 3000 ? 2 : 1;
    if (g_cfg <= 0) return 0;
    AcquireSRWLockShared(&g_rooms_lock);
    n = g_nroom_list < max ? g_nroom_list : max;
    memcpy(out, g_room_list, sizeof *out * (size_t)n);
    ReleaseSRWLockShared(&g_rooms_lock);
    return n;
}
const char *net_build_id(void) { configure(); return g_build; }
const char *net_mods_text(void) { configure(); return g_mods_text; }
/* nnldn.c join(): 1 = this game must not join the room of host (another build): why says the cause, the popup shows it */
int net_room_refusal(uint32_t host, char *why, size_t n) {
    char b[BUILD_LEN + 1] = "", name[NAME_LEN + 1] = "";
    int found = 0;
    if (!n || g_cfg <= 0) return 0;
    why[0] = 0;
    AcquireSRWLockShared(&g_rooms_lock);
    for (int i = 0; i < g_nroom_list && !found; i++)
        if (g_room_list[i].host == host && !g_room_list[i].mine) {
            found = 1;
            if (!g_room_list[i].joinable) { snprintf(why, n, "%s", g_room_list[i].why); snprintf(name, sizeof name, "%s", g_room_list[i].name); snprintf(b, sizeof b, "%s", g_room_list[i].build); }
        }
    for (int i = 0; i < g_nother && !found; i++) if (g_other[i].ip == host) snprintf(b, sizeof b, "%s", g_other[i].build);
    ReleaseSRWLockShared(&g_rooms_lock);
    if (!b[0]) return 0;
    if (!found || !why[0]) room_why(b, -1, NULL, 0, why, n);
    if (!why[0]) return 0;
    char title[80];
    snprintf(title, sizeof title, "Cannot join %s's room", name[0] ? name : "this");
    say("refused to join the room of 10.13.%u.%u (build %s): %s", host >> 8 & 255, host & 255, b, why);
    menu_popup(title, why);
    return 1;
}

/* ---------------------------------------------------------------- O2: matchmaking ("Find an opponent", F1 > Online)
 * net_match_find() starts a search: the client thread sends MATCH (op 1, a new search id) every 2 s while it searches or
 * is matched; the lobby answers each with MATCH_STATE (and sends one by itself when something changes). The lobby picks
 * the host. Nothing is driven in the game's menus: the host is told to create a room (Local Wireless > Create Room), the
 * other to open Local Wireless > Rooms List, where nnldn.c puts the host's room first (net_match_host). A popup says
 * each step over the game; F1 > Online shows the state. TEST SWITCH SSBU_MATCH_TEST=1 (default off): the search starts
 * by itself once the lobby answered (log "[online] match: ..."; input marks @match:host, @match:join, @match:room-open,
 * @match:together for an input sequence). */
void input_mark(const char *what);                      /* nnhid.c */
static SRWLOCK g_mm_lock = SRWLOCK_INIT;
static NetMatch g_mm = {MM_IDLE};
static volatile LONG g_mm_want, g_mm_cancel;            /* searching / matched (MATCH op 1 every 2 s); cancels left to send */
static uint32_t g_mm_id;
static volatile LONG g_mm_kind = 1;                     /* (R3 net3) MATCH op of the search: 1 a Local Wireless search, 2 a rollback-match search */
static uint64_t g_mm_tx, g_mm_asked;                    /* (client thread) last MATCH sent; when the search was asked (no answer yet) */
static int g_mm_room_said;
static const char *mm_name(int st) {
    static const char *n[] = {"idle", "searching", "matched", "timeout", "the other player left", "together", "expired", "asking", "older lobby", "off"};
    return st >= 0 && st <= MM_OFFLINE ? n[st] : "?";
}
static void mm_find(int kind) {
    if (!net_online()) return;
    AcquireSRWLockExclusive(&g_mm_lock);
    g_mm_id = (uint32_t)rnd64() | 1;
    memset(&g_mm, 0, sizeof g_mm);
    g_mm.state = MM_ASKING;
    g_mm.kind = kind;
    g_mm_room_said = 0;
    InterlockedExchange(&g_mm_kind, kind);
    g_rb_match_peer = 0;                                /* (a new search: the last opponent's messages are not taken any more) */
    ReleaseSRWLockExclusive(&g_mm_lock);
    InterlockedExchange(&g_mm_cancel, 0);
    InterlockedExchange(&g_mm_want, 1);
    g_mm_tx = 0;
    say("match: searching for an opponent%s (build %s, code %s)", kind == 2 ? " for a rollback match (Smash versus)" : "", g_build, g_code);
}
void net_match_find(void) { mm_find(1); }
void net_match_find_rb(void) { mm_find(2); }
void net_match_cancel(void) {
    AcquireSRWLockExclusive(&g_mm_lock);
    int was = g_mm.state;
    if (was == MM_SEARCHING || was == MM_MATCHED || was == MM_ASKING) g_mm.state = MM_IDLE;
    ReleaseSRWLockExclusive(&g_mm_lock);
    if (InterlockedExchange(&g_mm_want, 0)) { InterlockedExchange(&g_mm_cancel, 3); g_mm_tx = 0; say("match: cancelled (%s)", mm_name(was)); }
}
uint32_t net_match_host(void) {
    AcquireSRWLockShared(&g_mm_lock);
    uint32_t h = g_mm.state == MM_MATCHED && g_mm.role == MM_ROLE_JOIN ? g_mm.peer : 0;
    ReleaseSRWLockShared(&g_mm_lock);
    return h;
}
void net_match_get(NetMatch *o) {
    if (g_cfg <= 0) { memset(o, 0, sizeof *o); o->state = MM_OFFLINE; o->players = o->searching = o->code_players = -1; return; }
    AcquireSRWLockShared(&g_mm_lock);
    *o = g_mm;
    ReleaseSRWLockShared(&g_mm_lock);
    if (!g_joined) { o->players = o->searching = o->code_players = -1; if (o->state == MM_IDLE) o->state = MM_OFFLINE; return; }
    if (o->state != MM_SEARCHING && o->state != MM_MATCHED) {   /* (the lobby's own counts between searches: PONG) */
        o->players = (int)g_players; o->searching = (int)g_searching; o->code_players = (int)g_code_players;
    } else o->code_players = (int)g_code_players;
}
static void mm_send(int op) {                           /* (client thread) */
    uint8_t p[4 + 8 + 1 + 4];
    hdr(p, T_MATCH);
    s64(p + 4, g_cookie);
    p[12] = (uint8_t)op;
    s32(p + 13, g_mm_id);
    tx(p, sizeof p, &g_lobby);
}
static void mm_tick(uint64_t t) {                       /* (client thread, joined) */
    static int test = -1;
    if (test < 0) { const char *e = getenv("SSBU_MATCH_TEST"); test = e && e[0] == '1'; if (test) { say("match: TEST: searching by itself (SSBU_MATCH_TEST=1)"); net_match_find(); } }
    if (g_mm_cancel > 0) {
        if (t - g_mm_tx >= 300) { mm_send(0); g_mm_tx = t; InterlockedDecrement(&g_mm_cancel); }
        return;
    }
    if (!g_mm_want || t - g_mm_tx < 2000) return;
    if (!g_mm_tx) g_mm_asked = t;
    mm_send((int)g_mm_kind);
    g_mm_tx = t;
    AcquireSRWLockExclusive(&g_mm_lock);
    int old = g_mm.state == MM_ASKING && t - g_mm_asked > 6500;
    if (old) g_mm.state = MM_OLD_LOBBY;
    ReleaseSRWLockExclusive(&g_mm_lock);
    if (old) {
        InterlockedExchange(&g_mm_want, 0);
        say("match: the lobby does not answer MATCH%s (an older lobby server): no matchmaking there", g_mm_kind == 2 ? " for a rollback match" : "");
        if (g_mm_kind != 2) menu_popup("Find an opponent", "This lobby server is an older version: it cannot find opponents.\nCreate a room yourself (Local Wireless > Create Room) or look at F1 > Online.");
    }
}
/* ---------------------------------------------------------------- Arena (lobby 1.6, F1 > Arena; tools/net/ssbu_lobby.py)
 * The players who joined the arena wait in a queue at the lobby; the first two take their turn at a rollback match (Smash
 * versus: rbn_handshake searches the lobby by itself during the turn, MATCH op 2, and the lobby pairs only the two), the
 * winner stays. The client thread sends ARENA (op 1 = in the queue, 2 = only looking) every second while this player is
 * in the arena or F1 > Arena is shown; the lobby answers each with ARENA_STATE and sends one by itself at each change.
 *   24 ARENA        client: cookie u64, op u8 (0 leave, 1 join / stay in, 2 look only)
 *   25 ARENA_STATE  lobby:  version u32, status u8, place u8, streak u8, streak limit u8, opponent name[32], seconds u16,
 *                           match count u8 (+ name[32] x 2, streak u8 x 2, state u8), queue count u8 (+ (name[32], streak u8) each) */
#define T_ARENA 24
#define T_ARENA_STATE 25
static SRWLOCK g_ar_lock = SRWLOCK_INIT;
static NetArena g_ar;                                   /* (g_ar_lock) the lobby's last ARENA_STATE */
static volatile LONG g_ar_in, g_ar_leave;               /* in the queue (op 1 every second); leaves left to send */
static volatile LONG64 g_ar_watch;                      /* when F1 > Arena last asked (op 2 while it is shown) */
static uint64_t g_ar_tx;                                /* (client thread) */
/* (room, 2026-10-30) ARENA op 3 make a room, 4 close it, 5 remove a player (+ name[32]): sent a few times (UDP) by the client
 * thread. Making a room in LAN mode first makes this game the network's lobby, with priority: its beacon carries 'R' and
 * every other game (and the lobby before it) moves to it, so the host's PC carries everyone's connection. */
static volatile LONG g_ar_op, g_ar_op_left;             /* the command to send, sends left */
static char g_ar_kick[NAME_LEN + 1];
static volatile LONG g_lan_room_want;                   /* make this game the network's lobby for a room (client thread) */
static volatile LONG g_lan_room_prio;                   /* this game's lobby has (or is getting) a room: its beacon has priority */
static volatile LONG g_ar_making;                       /* a room being made here (until the lobby says it is open) */
static volatile LONG64 g_lan_lobby_id;                  /* (below: our own in-game lobby, 0 none) */
static void ar_command(int op, int sends) { InterlockedExchange(&g_ar_op, op); InterlockedExchange(&g_ar_op_left, sends); g_ar_tx = 0; }
void net_arena_join(int in) {
    if (!net_online()) return;
    InterlockedExchange(&g_ar_in, in ? 1 : 0);
    InterlockedExchange(&g_ar_leave, in ? 0 : 3);
    g_ar_tx = 0;
    say("arena: %s", in ? "joining the queue" : "leaving the arena");
}
void net_arena_get(NetArena *o) {
    InterlockedExchange64(&g_ar_watch, (LONG64)ms_now());
    AcquireSRWLockShared(&g_ar_lock);
    *o = g_ar;
    ReleaseSRWLockShared(&g_ar_lock);
    o->in = (int)g_ar_in;
    o->making = (int)g_ar_making;
    if (!g_joined) o->known = 0;
}
int net_arena_turn(void) {
    AcquireSRWLockShared(&g_ar_lock);
    int t = g_ar_in && g_ar.known && (g_ar.status == NET_ARENA_TURN || g_ar.status == NET_ARENA_PLAYING);
    ReleaseSRWLockShared(&g_ar_lock);
    return t;
}
int net_arena_queued(void) {
    AcquireSRWLockShared(&g_ar_lock);
    int q = g_ar_in && g_ar.known && g_ar.status != NET_ARENA_OUT;
    ReleaseSRWLockShared(&g_ar_lock);
    return q;
}
void net_arena_create(void) {
    if (!net_online()) return;
    InterlockedExchange(&g_ar_in, 1); InterlockedExchange(&g_ar_leave, 0);
    InterlockedExchange(&g_ar_making, 1);
    if (g_mode == NET_MODE_LAN && !g_lan_lobby_id) InterlockedExchange(&g_lan_room_want, 1);   /* (the client thread becomes the lobby first) */
    else InterlockedExchange(&g_lan_room_prio, 1);
    ar_command(3, 5);
    say("arena: making a room");
    menu_popup("Room", "Making your room...\nYour PC now carries the connection of everyone in it (a few seconds).");
}
void net_arena_close(void) { if (net_online()) { ar_command(4, 3); say("arena: closing the room"); } }
void net_arena_kick(const char *name) {
    if (!net_online() || !name || !name[0]) return;
    snprintf(g_ar_kick, sizeof g_ar_kick, "%s", name);
    ar_command(5, 3);
    say("arena: removing %s from the room", name);
}
static void arena_send(int op) {                        /* (client thread) */
    uint8_t p[4 + 8 + 1 + NAME_LEN];
    int n = 13;
    hdr(p, T_ARENA);
    s64(p + 4, g_cookie);
    p[12] = (uint8_t)op;
    if (op == 5) { memset(p + 13, 0, NAME_LEN); memcpy(p + 13, g_ar_kick, strlen(g_ar_kick)); n += NAME_LEN; }
    tx(p, n, &g_lobby);
}
static void arena_tick(uint64_t t) {                    /* (client thread, joined) */
    static int test = -1;                               /* TEST SWITCH SSBU_ARENA_TEST=1 (default off): into the queue by itself */
    if (test < 0) { const char *e = getenv("SSBU_ARENA_TEST"); test = e && e[0] == '1'; if (test) { say("arena: TEST: joining the queue by itself (SSBU_ARENA_TEST=1)"); net_arena_join(1); } }
    {   static uint64_t made_at;                       /* TEST SWITCH SSBU_ROOM_TEST=<seconds> (default off): make a room that long after joining */
        static double after = -2; if (after == -2) { const char *e = getenv("SSBU_ROOM_TEST"); after = e && *e ? atof(e) : -1; }
        if (after >= 0 && !made_at) made_at = t + (uint64_t)(after * 1000);
        if (after >= 0 && made_at > 1 && t >= made_at) { made_at = 1; say("arena: TEST: making a room (SSBU_ROOM_TEST)"); net_arena_create(); } }
    {   static int told;                                /* (room) once a session, 3+ players on this network and no room: how to take turns */
        if (!told && g_mode == NET_MODE_LAN && g_players >= 3 && !g_ar_in && !g_ar_making) {
            AcquireSRWLockShared(&g_ar_lock); int known = g_ar.known, open = g_ar.has_room; char host[NAME_LEN + 1]; memcpy(host, g_ar.host, sizeof host); ReleaseSRWLockShared(&g_ar_lock);
            if (!known) InterlockedExchange64(&g_ar_watch, (LONG64)t);       /* (ask the lobby once: is a room open?) */
            else {
                char m[300]; told = 1;
                if (open) snprintf(m, sizeof m, "%s has a room open to take turns (the winner stays).\nTo play: F1 > Arena > Join the queue.", host);
                else snprintf(m, sizeof m, "3 or more players online: you can take turns (the winner stays).\nOne player opens F1 > Arena > Create a room; the others: F1 > Arena > Join the queue.");
                menu_popup("Room", m);
            }
        } }
    if (g_ar_op_left > 0 && !g_lan_room_want) {        /* (a command: a few times, 300 ms apart) */
        if (t - g_ar_tx >= 300) { arena_send((int)g_ar_op); g_ar_tx = t; InterlockedDecrement(&g_ar_op_left); }
        return;
    }
    if (g_ar_leave > 0) {
        if (t - g_ar_tx >= 300) { arena_send(0); g_ar_tx = t; InterlockedDecrement(&g_ar_leave); }
        return;
    }
    int watched = t - (uint64_t)g_ar_watch < 2000;
    if ((!g_ar_in && !watched) || t - g_ar_tx < 1000) return;
    arena_send(g_ar_in ? 1 : 2);
    g_ar_tx = t;
}
static void arena_name(const uint8_t *p, char *out) {
    for (int k = 0; k < NAME_LEN; k++) out[k] = p[k] && p[k] < 0x20 ? '?' : (char)p[k];
    out[NAME_LEN] = 0;
}
static void arena_state(const uint8_t *p, int n) {      /* (client thread) an ARENA_STATE body */
    if (n < 4 + 4 + NAME_LEN + 2 + 1) return;
    NetArena a; memset(&a, 0, sizeof a);
    a.known = 1;
    a.status = p[4]; a.place = p[5]; a.streak = p[6]; a.streak_limit = p[7];
    arena_name(p + 8, a.opponent);
    a.secs = g16(p + 8 + NAME_LEN);
    int o = 8 + NAME_LEN + 2;
    if (p[o] && o + 1 + 2 * NAME_LEN + 3 <= n) {
        a.has_match = 1;
        arena_name(p + o + 1, a.match[0].name); arena_name(p + o + 1 + NAME_LEN, a.match[1].name);
        a.match[0].streak = p[o + 1 + 2 * NAME_LEN]; a.match[1].streak = p[o + 2 + 2 * NAME_LEN]; a.match_state = p[o + 3 + 2 * NAME_LEN];
        o += 1 + 2 * NAME_LEN + 3;
    } else o += 1;
    if (o < n) {
        int q = p[o++];
        for (int i = 0; i < q && i < 16 && o + NAME_LEN + 1 <= n; i++, o += NAME_LEN + 1) { arena_name(p + o, a.queue[i].name); a.queue[i].streak = p[o + NAME_LEN]; a.nqueue = i + 1; }
    }
    if (o + 4 + NAME_LEN <= n && p[o] == 'H') {           /* (room) the host's block (net_minilobby.c ml_arena_send) */
        a.has_room = p[o + 1] & 1; a.is_host = (p[o + 1] & 2) != 0;
        arena_name(p + o + 2, a.host);
        a.room_players = p[o + 2 + NAME_LEN]; a.why = p[o + 3 + NAME_LEN];
    }
    AcquireSRWLockExclusive(&g_ar_lock);
    NetArena old = g_ar;
    int was = g_ar.known ? g_ar.status : NET_ARENA_OUT;
    g_ar = a;
    ReleaseSRWLockExclusive(&g_ar_lock);
    char t[400];
    /* (room) the instructions the players see, at each change (2026-10-30, owner: "the users should be able to do it without help") */
    if (a.has_room && a.is_host && g_ar_making) {
        InterlockedExchange(&g_ar_making, 0);
        say("arena: our room is open");
        menu_popup("Your room is open", "Your friends: F1 > Arena > Join the queue.\nYou are in the queue too: close F1, go to Smash, choose your fighter and press start.\nYour game waits for your turn and starts the match by itself.");
    } else if (a.has_room && !a.is_host && g_ar_making) {                  /* (someone else's room was open already) */
        InterlockedExchange(&g_ar_making, 0); InterlockedExchange(&g_lan_room_prio, 0);
        snprintf(t, sizeof t, "%s already has a room open (only one at a time).\nF1 > Arena > Join the queue to play in it.", a.host);
        menu_popup("Room", t);
    } else if (a.has_room && !old.has_room && !a.is_host && !g_ar_in) {
        snprintf(t, sizeof t, "%s opened a room to take turns (winner stays).\nTo play: F1 > Arena > Join the queue.", a.host);
        menu_popup("Room", t);
    }
    if (!a.has_room && old.has_room && old.is_host) InterlockedExchange(&g_lan_room_prio, 0);     /* (our room is closed: a normal lobby again) */
    if (!g_ar_in || a.status == was) return;
    if (a.status == NET_ARENA_TURN && was == NET_ARENA_PLAYING) {        /* (won: the winner stays) */
        snprintf(t, sizeof t, "You won (%d in a row)! You stay.\nNext: against %s. At the character select press start again.", a.streak, a.opponent);
        say("arena: won, next against %s", a.opponent);
        menu_popup("Room", t);
    } else if (a.status == NET_ARENA_TURN) {
        snprintf(t, sizeof t, "YOUR TURN against %s!\nIf you are not waiting at the start already: go to Smash, choose your fighter and press start (you have 2 minutes).", a.opponent);
        say("arena: our turn against %s", a.opponent);
        menu_popup("Room", t);
    } else if (a.status == NET_ARENA_PLAYING) say("arena: playing against %s", a.opponent);
    else if (a.status == NET_ARENA_WAITING && was == NET_ARENA_PLAYING) {
        snprintf(t, sizeof t, "You lost: you go to the back of the queue (place %d).\nAt the character select press start again: your game waits for your next turn.", a.place);
        say("arena: lost, place %d", a.place);
        menu_popup("Room", t);
    } else if (a.status == NET_ARENA_WAITING && was == NET_ARENA_OUT) {
        snprintf(t, sizeof t, "You are in %s's room (place %d in the queue).\nNow close F1, go to Smash, choose your fighter and press start.\nYour game waits for your turn and starts the match by itself.", a.host, a.place);
        say("arena: in the queue, place %d", a.place);
        menu_popup("Room", t);
    } else if (a.status == NET_ARENA_WAITING) say("arena: in the queue, place %d", a.place);
    else if (a.status == NET_ARENA_OUT && was != NET_ARENA_OUT) {
        InterlockedExchange(&g_ar_in, 0);
        say("arena: taken out of the room (%d)", a.why);
        if (a.why == 1) snprintf(t, sizeof t, "The host (%s) removed you from the room.\nF1 > Arena > Join the queue to come back.", old.host);
        else if (a.why == 2 && old.is_host) snprintf(t, sizeof t, "You closed your room.");
        else if (a.why == 2) snprintf(t, sizeof t, "%s closed the room.\nAnyone can open a new one: F1 > Arena > Create a room.", old.host[0] ? old.host : "The host");
        else if (a.why == 3) snprintf(t, sizeof t, "You are out of the room: your turn was not started within 2 minutes.\nF1 > Arena > Join the queue to come back.");
        else snprintf(t, sizeof t, "The room is gone: %s closed the game (the room lived on that PC).\nAnyone can open a new one: F1 > Arena > Create a room.", old.host[0] ? old.host : "its host");
        menu_popup("Room", t);
    }
}

static void mm_state(const uint8_t *p, int n, uint64_t t) {    /* (client thread) a MATCH_STATE body (n bytes) */
    uint32_t id = g32(p);
    int st = p[4], role = p[5], flags = p[6];
    if (id != g_mm_id || st > MM_EXPIRED) return;
    char name[NAME_LEN + 1];
    for (int k = 0; k < NAME_LEN; k++) name[k] = p[11 + k] && p[11 + k] < 0x20 ? '?' : (char)p[11 + k];
    name[NAME_LEN] = 0;
    if (!name[0]) snprintf(name, sizeof name, "the other player");
    AcquireSRWLockExclusive(&g_mm_lock);
    int was = g_mm.state, want = g_mm_want;
    if (!want) {                                        /* cancelled (or over): the lobby's IDLE ends the cancel; anything else is late */
        if (st == MM_IDLE) InterlockedExchange(&g_mm_cancel, 0);
        ReleaseSRWLockExclusive(&g_mm_lock);
        return;
    }
    int open_before = g_mm.room_open;
    g_mm.state = st;
    g_mm.role = role;
    g_mm.room_open = flags & 1;
    g_mm.peer = g32(p + 7);
    memcpy(g_mm.peer_name, name, sizeof g_mm.peer_name);
    g_mm.searching = g16(p + 43);
    g_mm.players = g16(p + 45);
    g_mm.waited_s = g16(p + 47);
    int room_new = st == MM_MATCHED && role == MM_ROLE_JOIN && g_mm.room_open && (!open_before || was != MM_MATCHED) && !g_mm_room_said;
    if (room_new) g_mm_room_said = 1;
    int rbk = g_mm.kind == 2;
    if (rbk && (st == MM_MATCHED || st == MM_TOGETHER) && g_mm.peer) g_rb_match_peer = g_mm.peer;
    int direct_at = rbk && st == MM_MATCHED && g_mm.peer && n >= 49 + 7 && p[49] == 'A';
    ReleaseSRWLockExclusive(&g_mm_lock);
    if (direct_at) rbd_set(g32(p + 7), g32(p + 50), g16(p + 54));     /* (the opponent's address: the match goes player to player) */
    if (st >= MM_TIMEOUT) InterlockedExchange(&g_mm_want, 0);
    (void)t;
    if (rbk) {                                          /* (R3 net3) a rollback-match search: rb_net.c's box shows every step; log lines + a mark only */
        if (st == MM_MATCHED && was != MM_MATCHED) { uint32_t ip = g32(p + 7);
            say("match: matched with %s (10.13.%u.%u) for a rollback match: this game is player %d", name, ip >> 8 & 255, ip & 255, role == MM_ROLE_HOST ? 1 : 2); input_mark("match:rb"); }
        else if (st != was && st >= MM_TIMEOUT) {
            static const char *line[] = {"", "", "", "no opponent found in time", "the other player cancelled or left", "both games exchange rollback messages", "the match expired"};
            say("match (rollback): %s%s%s", line[st], st == MM_TIMEOUT ? "" : ": ", st == MM_TIMEOUT ? "" : name);
        } else if (st == MM_SEARCHING && was == MM_ASKING) say("match: the lobby is searching for a rollback match (%d of %d players of this build searching)", g16(p + 43), g16(p + 45));
        return;
    }
    char text[256], title[96];
    if (st == MM_MATCHED && was != MM_MATCHED) {
        uint32_t ip = g32(p + 7);
        say("match: matched with %s (10.13.%u.%u): %s", name, ip >> 8 & 255, ip & 255, role == MM_ROLE_HOST ? "this game hosts" : "this game joins their room");
        snprintf(title, sizeof title, "Matched with %s", name);
        if (role == MM_ROLE_HOST) {
            snprintf(text, sizeof text, "You host: Local Wireless > Create Room (or keep your open room).\nAccept %s's request when it appears.", name);
            input_mark("match:host");
        } else if (flags & 1) {
            snprintf(text, sizeof text, "%s hosts and their room is open:\nLocal Wireless > Rooms List, their room is first. Join it.", name);
            input_mark("match:join");
        } else {
            snprintf(text, sizeof text, "%s hosts: open Local Wireless > Rooms List,\ntheir room will be first as soon as they create it.", name);
            input_mark("match:join");
        }
        menu_popup(title, text);
        if (room_new) { say("match: %s's room is open", name); input_mark("match:room-open"); }
    } else if (room_new) {
        say("match: %s's room is open", name);
        snprintf(title, sizeof title, "%s's room is open", name);
        menu_popup(title, "Local Wireless > Rooms List: their room is first. Join it.");
        input_mark("match:room-open");
    } else if (st != was && st >= MM_TIMEOUT) {
        static const char *line[] = {"", "", "", "no opponent found in time", "the other player cancelled or left", "both in the same room", "the match expired (no room together in time)"};
        say("match: %s%s%s", line[st], st == MM_TIMEOUT ? "" : ": ", st == MM_TIMEOUT ? "" : name);
        if (st == MM_TIMEOUT) menu_popup("No opponent found", "Nobody with your version and mods searched in time.\nTry again later (F1 > Online > Find an opponent), or create a room.");
        else if (st == MM_PEER_LEFT) { snprintf(title, sizeof title, "Match with %s cancelled", name); menu_popup(title, "They cancelled or left the lobby. F1 > Online > Find an opponent searches again."); }
        else if (st == MM_TOGETHER) { snprintf(title, sizeof title, "In a room with %s", name); menu_popup(title, "Have a good match!"); input_mark("match:together"); }
        else { snprintf(title, sizeof title, "Match with %s expired", name); menu_popup(title, "You did not meet in a room in time. F1 > Online > Find an opponent searches again."); }
    } else if (st == MM_SEARCHING && was == MM_ASKING) say("match: the lobby is searching (%d of %d players of this build searching)", g16(p + 43), g16(p + 45));
}

/* ---------------------------------------------------------------- the client thread */
static uint32_t outbound_ip(void) {                 /* the address this PC goes out on (no packet is sent) */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    int len = sizeof a;
    uint32_t ip = 0;
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);  /* (TEST-NET-1: only the route is looked up) */
    if (s != INVALID_SOCKET && !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len)) ip = ntohl(a.sin_addr.s_addr);
    if (s != INVALID_SOCKET) closesocket(s);
    return ip;
}

static int lookup(const char *text, uint16_t default_port, struct sockaddr_in *out) {     /* "host[:port]" */
    char host[256];
    snprintf(host, sizeof host, "%s", text);
    uint16_t port = default_port;
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; if (atoi(colon + 1) > 0) port = (uint16_t)atoi(colon + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return 0;
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return 1;
}
static int test_loopback(void) {                /* SSBU_ONLINE_LOOPBACK=1 (test runs): every socket on 127.0.0.1 only, so the many
                                                 * test exes of one PC do not each raise the Windows firewall prompt */
    static int v = -1; if (v < 0) { const char *e = getenv("SSBU_ONLINE_LOOPBACK"); v = e && e[0] == '1'; } return v;
}
static void bind_client(int loopback_only) {    /* (client thread) binds g_sock; loopback_only: the lobby is on this PC */
    if (test_loopback()) loopback_only = 1;
    g_loopback = loopback_only;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(g_local_port);
    a.sin_addr.s_addr = loopback_only ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
    if (bind(g_sock, (struct sockaddr *)&a, sizeof a)) say("cannot bind UDP port %u (error %d)", g_local_port, WSAGetLastError());
    int len = sizeof a;
    if (!getsockname(g_sock, (struct sockaddr *)&a, &len)) g_lan_port = ntohs(a.sin_port);
    g_lan_ip = loopback_only ? INADDR_LOOPBACK : outbound_ip();
}
static int resolve(void) {
    if (!lookup(g_server, VNET_DEFAULT_PORT, &g_lobby)) return 0;
    bind_client((ntohl(g_lobby.sin_addr.s_addr) >> 24) == 127);      /* (lobby on this PC: loopback only) */
    return 1;
}

/* ---------------------------------------------------------------- no server: room codes, STUN, the in-game lobby */
#include "net_minilobby.c"                      /* the lobby of tools_net/ssbu_lobby.py in C: runs inside the host's game */
#include "net_upnp.c"                           /* UPnP-IGD port mapping (HOST mode, online_upnp) */

/* A room code is an IPv4 address and a UDP port (6 bytes) in Crockford base32: 10 characters, shown as XXXXX-XXXXX.
 * Reading it ignores '-', spaces and case; O reads as 0, I and L as 1. "a.b.c.d:port" is accepted as well. */
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static void code_of(uint32_t ip, uint16_t port, char *out) {   /* out: 12 bytes */
    uint64_t v = (uint64_t)ip << 16 | port;
    char t[10];
    for (int i = 9; i >= 0; i--) { t[i] = B32[v & 31]; v >>= 5; }
    memcpy(out, t, 5); out[5] = '-'; memcpy(out + 6, t + 5, 5); out[11] = 0;
}
static int code_read(const char *s, struct sockaddr_in *out) {
    if (strchr(s, '.')) return lookup(s, VNET_DEFAULT_PORT, out);
    uint64_t v = 0;
    int n = 0;
    for (; *s; s++) {
        int c = toupper((unsigned char)*s), d;
        if (c == '-' || c == ' ') continue;
        if (c == 'O') c = '0'; else if (c == 'I' || c == 'L') c = '1';
        const char *k = strchr(B32, c);
        if (!k || !c) return 0;
        d = (int)(k - B32);
        v = v << 5 | (uint64_t)d;
        n++;
    }
    if (n != 10 || !(v & 0xffff)) return 0;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl((uint32_t)(v >> 16));
    out->sin_port = htons((uint16_t)v);
    return 1;
}
static char g_room_code[64];
static char g_lan_code[16], g_upnp_text[192];           /* U1 (the Online page of the settings menu): HOST mode's code for the same network, the UPnP result */
const char *net_room_code(void) { return g_room_code; }
const char *net_lan_code(void) { return g_lan_code; }
const char *net_upnp_result(void) { return g_upnp_text; }
int net_mode(void) { configure(); return g_mode; }

/* STUN (RFC 5389) Binding request from socket s: the public address the router gives that socket's port. 0 = no answer. */
static int stun_query(SOCKET s, uint32_t *ip, uint16_t *port) {
    struct sockaddr_in srv;
    if (!g_stun[0] || !lookup(g_stun, 3478, &srv)) return 0;
    uint8_t q[20] = {0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42}, r[512];
    for (int i = 8; i < 20; i++) q[i] = (uint8_t)rnd64();
    for (int tries = 0; tries < 3; tries++) {
        sendto(s, (const char *)q, sizeof q, 0, (struct sockaddr *)&srv, sizeof srv);
        uint64_t until = ms_now() + 700;
        while (ms_now() < until) {
            struct sockaddr_in from; int fl = sizeof from;
            int n = recvfrom(s, (char *)r, sizeof r, 0, (struct sockaddr *)&from, &fl);
            if (n < 20 || r[0] != 1 || r[1] != 1 || memcmp(r + 4, q + 4, 16)) continue;
            for (int o = 20; o + 4 <= n;) {                 /* attributes: 0x20 XOR-MAPPED-ADDRESS, 0x01 MAPPED-ADDRESS */
                int type = g16(r + o), len = g16(r + o + 2);
                if (o + 4 + len > n) break;
                if ((type == 0x20 || type == 0x01) && len >= 8 && r[o + 5] == 1) {
                    *port = g16(r + o + 6); *ip = g32(r + o + 8);
                    if (type == 0x20) { *port ^= 0x2112; *ip ^= 0x2112a442u; }
                    return 1;
                }
                o += 4 + ((len + 3) & ~3);
            }
        }
    }
    return 0;
}
static SOCKET udp_socket(uint32_t ip, uint16_t port, int timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip); a.sin_port = htons(port);
    DWORD to = (DWORD)timeout_ms, bytes = 0; BOOL off = FALSE;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return INVALID_SOCKET; }
    return s;
}
static const char *ip_text(uint32_t ip, char *b) { snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); return b; }

static uint16_t g_upnp_mapped;
static void upnp_cleanup(void) { if (g_upnp_mapped) upnp_unmap_udp(g_upnp_mapped); }
static int start_host(void) {                   /* HOST mode: the lobby in this game, its code, the NAT opened if we can */
    uint16_t port = g_host_port;
    uint32_t lan = outbound_ip(), pub = 0;
    uint16_t pub_port = 0;
    char a[16], b[16], why[128] = "", ext[32] = "";
    SOCKET probe = udp_socket(test_loopback() ? INADDR_LOOPBACK : 0, port, 300);                  /* STUN from the lobby's own port, before the lobby takes it */
    if (probe == INVALID_SOCKET) { say("host: UDP port %u is in use: set another with online_port", port); return 0; }
    int stun_ok = stun_query(probe, &pub, &pub_port);
    closesocket(probe);
    if (!minilobby_start(test_loopback() ? INADDR_LOOPBACK : 0, port, 1)) { say("host: cannot start the lobby on UDP port %u", port); return 0; }
    int mapped = 0;
    if (g_upnp && lan) {
        mapped = upnp_map_udp(port, lan, "SSBU online room", ext, sizeof ext, why, sizeof why);
        if (mapped) { g_upnp_mapped = port; atexit(upnp_cleanup); say("host: UPnP: the router forwards UDP port %u to this PC%s%s", port, ext[0] ? ", public address " : "", ext);
                      snprintf(g_upnp_text, sizeof g_upnp_text, "UPnP: the router forwards UDP port %u to this PC%s%s", port, ext[0] ? ", public address " : "", ext); }
        else { say("host: UPnP: %s", why); snprintf(g_upnp_text, sizeof g_upnp_text, "UPnP: %s", why); }
        if (mapped && !pub && ext[0] && strcmp(ext, "0.0.0.0")) { struct in_addr x; if (inet_pton(AF_INET, ext, &x) == 1) { pub = ntohl(x.s_addr); pub_port = port; } }
    }
    if (!(g_upnp && lan)) snprintf(g_upnp_text, sizeof g_upnp_text, "UPnP: %s", g_upnp ? "no local address" : "off (online_upnp = off)");
    if (stun_ok) say("host: STUN %s: public address %s:%u", g_stun, ip_text(pub, a), pub_port);
    else if (g_stun[0]) say("host: STUN %s: no answer", g_stun);
    char code[16], lan_code[16];
    code_of(lan ? lan : 0x7f000001u, port, lan_code);
    snprintf(g_lan_code, sizeof g_lan_code, "%s", lan_code);
    if (pub) {
        code_of(pub, mapped ? port : pub_port, code);
        snprintf(g_room_code, sizeof g_room_code, "%s", code);
        say("host: ROOM CODE %s (internet, %s:%u); on the same network: %s (%s:%u)", code, ip_text(pub, a), mapped ? port : pub_port, lan_code, ip_text(lan, b), port);
        if (!mapped && pub != lan)
            say("host: no port mapping: if the other player cannot join, forward UDP port %u to %s on the router, or set online_peer = <the joiner's code>, or use lobby mode", port, ip_text(lan, b));
    } else {
        snprintf(g_room_code, sizeof g_room_code, "%s", lan_code);
        say("host: ROOM CODE %s (this network only, %s:%u: the public address is not known)", lan_code, ip_text(lan ? lan : 0x7f000001u, b), port);
    }
    for (char *s = g_peer; *s;) {                             /* online_peer = code[,code...]: open our NAT towards them */
        char one[64]; size_t k = strcspn(s, ", ;");
        snprintf(one, sizeof one, "%.*s", (int)k, s);
        struct sockaddr_in at;
        if (one[0] && code_read(one, &at)) { minilobby_open_to(&at); say("host: sending NAT-opening datagrams to the joiner %s", one); }
        s += k; while (*s == ',' || *s == ' ' || *s == ';') s++;
    }
    g_lobby.sin_family = AF_INET; g_lobby.sin_addr.s_addr = htonl(INADDR_LOOPBACK); g_lobby.sin_port = htons(port);
    bind_client(1);
    return 1;
}

/* LAN mode: beacon "SU" 1 15: lobby id u64, lobby port u16, code[32], build[40] (multicast + broadcast, every second). */
#define T_BEACON 15
#define BEACON_GROUP "239.255.13.13"
static volatile LONG64 g_lan_lobby_id;          /* our own in-game lobby (0: none) */
static uint16_t g_lan_lobby_port;
static struct { SRWLOCK lock; struct sockaddr_in at; uint64_t id, cur; int pending, prio, cur_prio; } g_switch = {SRWLOCK_INIT};   /* cur: the id of the lobby this game (not a lobby itself) is in or going to */
static SOCKET g_beacon = INVALID_SOCKET;
static SOCKET beacon_socket(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    BOOL yes = TRUE; DWORD to = 200, bytes = 0, off = FALSE; int ttl = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(VNET_BEACON_PORT); a.sin_addr.s_addr = test_loopback() ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return INVALID_SOCKET; }
    struct ip_mreq m = {0};
    inet_pton(AF_INET, BEACON_GROUP, &m.imr_multiaddr);
    m.imr_interface.s_addr = INADDR_ANY;
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&m, sizeof m);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof ttl);
    yes = TRUE; setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&yes, sizeof yes);
    return s;
}
/* After build[40] the beacon may carry 'V' + the lobby game's runtime version[24] (boot.c ssbu_version; an older game sends
 * none and reads only the part before). Different versions still play together; the player is told which one is older. */
#define BEACON_BASE (4 + 8 + 2 + NAME_LEN + BUILD_LEN)
#define VER_LEN 24
static void version_check(const char *theirs) {      /* (beacon thread) the version of the network's lobby game */
    const char *ssbu_version(void), *mine = ssbu_version();
    static char last[VER_LEN + 1];
    static uint64_t next;
    if (!theirs[0] || !strcmp(theirs, "dev") || !strcmp(mine, "dev") || !strncmp(theirs, mine, VER_LEN)) { last[0] = 0; return; }     /* (the beacon carries VER_LEN characters) */
    uint64_t t = ms_now();
    int older_here = strcmp(theirs, mine) > 0;
    /* (tester 2026-10-31: two friends playing on one PC got it every 30 s from another friend's older lobby on Radmin) the
     * lobby's game older: told once (nothing this player can do about it); this game older: again every 10 minutes */
    if (!strcmp(theirs, last) && (!older_here || t < next)) return;
    snprintf(last, sizeof last, "%s", theirs); next = t + 600000;
    char text[400];
    if (older_here)
        snprintf(text, sizeof text, "Your game is OLDER than the lobby's (the player who opened first).\nYours: %s\nLobby: %s\n"
                 "Close the game and open Play SSBU.bat: it updates by itself.", mine, theirs);
    else
        snprintf(text, sizeof text, "The lobby's game (the player who opened first) is OLDER than yours.\nYours: %s\nLobby: %s\n"
                 "That player should close the game and open Play SSBU.bat: it updates by itself.", mine, theirs);
    say("version: the lobby's game is %s, this one %s", theirs, mine);
    menu_popup("Different version", text);
}
/* (room, 2026-10-30) after the 'V' block: 'R' + u8 1 = this lobby has a player's room (or is getting one): the games of its
 * build move to it at once, joined or not, and a lobby without a room hands over to it (the host's PC carries the room) */
static int beacon_read(const uint8_t *p, int n, uint64_t *id, uint16_t *port, char *ver, int *prio) {   /* 1: a lobby of our code and build */
    ver[0] = 0; *prio = 0;
    if (n < BEACON_BASE || p[0] != 'S' || p[1] != 'U' || p[2] != PROTO || p[3] != T_BEACON) return 0;
    char code[NAME_LEN + 1] = "", build[BUILD_LEN + 1] = "";
    memcpy(code, p + 14, NAME_LEN); memcpy(build, p + 14 + NAME_LEN, BUILD_LEN);
    if (strcmp(code, g_code)) return 0;     /* O1: any build (it used to be ours only): its lobby lists the rooms of every build, the
                                               * joins stay within one build (the lobby parts players by build and code) */
    (void)build;
    if (n >= BEACON_BASE + 1 + VER_LEN && p[BEACON_BASE] == 'V') {
        int k = 0;
        for (; k < VER_LEN && p[BEACON_BASE + 1 + k]; k++) ver[k] = p[BEACON_BASE + 1 + k] >= 0x21 && p[BEACON_BASE + 1 + k] < 0x7f ? (char)p[BEACON_BASE + 1 + k] : '?';
        ver[k] = 0;
    }
    if (n >= BEACON_BASE + 1 + VER_LEN + 2 && p[BEACON_BASE + 1 + VER_LEN] == 'R' && !strncmp(build, g_build, BUILD_LEN)) *prio = p[BEACON_BASE + 2 + VER_LEN] == 1;
    *id = g64(p + 4); *port = g16(p + 12);
    return *id != 0 && *port != 0;
}
static DWORD WINAPI beacon_thread(LPVOID unused) {    /* sends our lobby's beacon; hears the others' (one lobby per network) */
    (void)unused;
    const char *ssbu_version(void);
    uint8_t p[BEACON_BASE + 1 + VER_LEN + 2] = {0}, r[256];
    int prio;
    char ver[VER_LEN + 1];
    struct sockaddr_in grp = {0}, bc = {0};
    grp.sin_family = bc.sin_family = AF_INET;
    grp.sin_port = bc.sin_port = htons(VNET_BEACON_PORT);
    inet_pton(AF_INET, BEACON_GROUP, &grp.sin_addr);
    bc.sin_addr.s_addr = INADDR_BROADCAST;
    uint64_t last = 0;
    for (;;) {
        uint64_t t = ms_now(), mine = (uint64_t)g_lan_lobby_id;
        if (mine && t - last >= 1000) {
            hdr(p, T_BEACON); s64(p + 4, mine); s16(p + 12, g_lan_lobby_port);
            memcpy(p + 14, g_code, strlen(g_code)); memcpy(p + 14 + NAME_LEN, g_build, strlen(g_build));
            p[BEACON_BASE] = 'V'; strncpy((char *)p + BEACON_BASE + 1, ssbu_version(), VER_LEN);
            p[BEACON_BASE + 1 + VER_LEN] = 'R'; p[BEACON_BASE + 2 + VER_LEN] = (uint8_t)(g_lan_room_prio ? 1 : 0);
            sendto(g_beacon, (const char *)p, sizeof p, 0, (struct sockaddr *)&grp, sizeof grp);
            sendto(g_beacon, (const char *)p, sizeof p, 0, (struct sockaddr *)&bc, sizeof bc);
            last = t;
        }
        struct sockaddr_in from; int fl = sizeof from; uint64_t id; uint16_t port;
        int n = recvfrom(g_beacon, (char *)r, sizeof r, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0 || !beacon_read(r, n, &id, &port, ver, &prio) || id == mine) continue;
        if (!mine && (!g_switch.cur || id == g_switch.cur)) version_check(ver);      /* the lobby this game is in (or going to) */
        /* another lobby on this network: the one with the smaller id stays; ours goes if nobody but us is in it and we are
         * in no Local Wireless room (otherwise both stay: their rooms are listed by each lobby to its own players) */
        int players = 0, rooms = 0;
        if (mine) minilobby_counts(&players, &rooms);
        /* (room) a lobby with a player's room wins over one without, whatever the ids and the players: they all move to it */
        int mine_prio = mine && g_lan_room_prio;
        if (mine && prio != mine_prio) { if (mine_prio) continue; }
        else if (mine && (id > mine || players > 1 || nnldn_players())) continue;
        /* (2026-10-29: three players, two lobbies on the network) a game that is not a lobby took every beacon it heard as the
         * lobby to join: with two lobbies it went from one to the other at each beacon and was disconnected each time. Now it
         * stays in the lobby it is in while it is joined; not joined, it goes to a lobby of a smaller id (the one that stays,
         * as above) or, when its lobby has not let it in for GONE_AFTER_MS (a firewall in front of that lobby), to another. */
        if (!mine && g_switch.cur && id != g_switch.cur && !(prio && !g_switch.cur_prio) && (g_joined || (id > g_switch.cur && ms_now() - g_unjoined_since < GONE_AFTER_MS))) continue;
        if (!mine && g_switch.cur && id != g_switch.cur && g_switch.cur_prio && !prio) continue;      /* (in a room's lobby: never back to one without) */
        AcquireSRWLockExclusive(&g_switch.lock);
        if (g_switch.id != id) { g_switch.at = from; g_switch.at.sin_port = htons(port); g_switch.id = id; g_switch.prio = prio; g_switch.pending = 1; }
        else if (id == g_switch.cur) g_switch.cur_prio = prio;      /* (the room of our lobby opened or closed) */
        ReleaseSRWLockExclusive(&g_switch.lock);
    }
}
static int lan_become_lobby(void) {             /* (client thread) this game runs the network's lobby and announces it */
    uint16_t port = minilobby_start(test_loopback() ? INADDR_LOOPBACK : 0, 0, 1);
    if (!port) { say("lan: cannot start a lobby"); return 0; }
    g_lan_lobby_port = port;
    InterlockedExchange64(&g_lan_lobby_id, (LONG64)(rnd64() >> 1 | 1));
    g_lobby.sin_family = AF_INET; g_lobby.sin_addr.s_addr = htonl(INADDR_LOOPBACK); g_lobby.sin_port = htons(port);
    say("lan: no lobby on this network: this game is the lobby (UDP port %u, beacon on port %u)", port, VNET_BEACON_PORT);
    return 1;
}
static int start_lan(void) {                    /* LAN mode: find the network's lobby for 1.5 s; none -> run it here */
    char a[16];
    g_beacon = beacon_socket();
    if (g_beacon == INVALID_SOCKET) { say("lan: cannot open the beacon port %u", VNET_BEACON_PORT); return 0; }
    HANDLE th = CreateThread(NULL, 0, beacon_thread, NULL, 0, NULL);
    if (th) { SetThreadDescription(th, L"online lan beacon"); CloseHandle(th); }
    Sleep(1500 + (DWORD)(rnd64() % 500));
    AcquireSRWLockExclusive(&g_switch.lock);
    int found = g_switch.pending;
    if (found) { g_lobby = g_switch.at; g_switch.cur = g_switch.id; g_switch.pending = 0; }
    ReleaseSRWLockExclusive(&g_switch.lock);
    if (found) { bind_client(0); say("lan: found the lobby of this network at %s:%u", ip_text(ntohl(g_lobby.sin_addr.s_addr), a), ntohs(g_lobby.sin_port)); return 1; }
    bind_client(0);
    return lan_become_lobby();
}
static void lan_switch_if_asked(void) {         /* (client thread) another network lobby won: join it */
    if (g_mode != NET_MODE_LAN) return;
    AcquireSRWLockExclusive(&g_switch.lock);
    int go = g_switch.pending;
    struct sockaddr_in at = g_switch.at;
    g_switch.pending = 0;
    if (go) { g_switch.cur = g_switch.id; g_switch.cur_prio = g_switch.prio; }
    int prio = g_switch.prio;
    ReleaseSRWLockExclusive(&g_switch.lock);
    if (!go || same_at(&at, &g_lobby)) return;
    char a[16];
    if (g_lan_lobby_id) { InterlockedExchange64(&g_lan_lobby_id, 0); minilobby_stop(); }
    if (g_lan_room_prio) { InterlockedExchange(&g_lan_room_prio, 0); InterlockedExchange(&g_ar_making, 0); }     /* (another room's lobby won: ours is gone) */
    if (prio) say("lan: a player opened a room: its game is the network's lobby now: moving to it");
    InterlockedExchange(&g_joined, 0);
    g_lobby = at;
    g_unjoined_since = ms_now();
    say("lan: another lobby on this network (%s:%u): joining it", ip_text(ntohl(at.sin_addr.s_addr), a), ntohs(at.sin_port));
}
static void lobby_fallback_to_lan(void) {       /* (client thread) auto mode, the lobby server does not answer */
    say("no answer from the lobby %s: looking for players on this network instead (LAN mode)", g_server);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    DWORD timeout = 50, bytes = 0; BOOL off = FALSE;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
    SOCKET old = g_sock;
    g_sock = s;                                 /* (net_send sends nothing until joined) */
    closesocket(old);
    g_mode = NET_MODE_LAN;
    if (!start_lan()) say("lan: online play unavailable");
    g_unjoined_since = ms_now();
}
static int setup(void) {                        /* (client thread) where the lobby is, for this mode */
    char a[16];
    switch (g_mode) {
    case NET_MODE_HOST: return start_host();
    case NET_MODE_LAN: return start_lan();
    case NET_MODE_JOIN:
        if (!code_read(g_join, &g_lobby)) { snprintf(g_reject, sizeof g_reject, "not a room code: %s", g_join); say("%s", g_reject); return 0; }
        bind_client((ntohl(g_lobby.sin_addr.s_addr) >> 24) == 127);
        say("join: the host's game at %s:%u (room code %s)", ip_text(ntohl(g_lobby.sin_addr.s_addr), a), ntohs(g_lobby.sin_port), g_join);
        if (!g_loopback) {                       /* our own code: the host can open its NAT towards it (online_peer) */
            uint32_t pub; uint16_t pp;
            if (stun_query(g_sock, &pub, &pp)) { code_of(pub, pp, g_room_code); say("join: your code (only needed if the host's router blocks you: the host sets online_peer = %s)", g_room_code); }
        }
        return 1;
    default:
        if (resolve()) return 1;
        snprintf(g_reject, sizeof g_reject, "cannot find the lobby %s", g_server);
        return 0;
    }
}

static void send_join(void) {
    enum { BASE = 4 + 8 + 2 * NAME_LEN + BUILD_LEN + 4 + 2 + 1 };
    uint8_t p[BASE + 2 + sizeof g_mods_wire + 3 + LD_ID + 32 + 1 + LD_KEY + 1 + LD_INVITE] = {0};
    hdr(p, T_JOIN);
    s64(p + 4, g_nonce);
    memcpy(p + 12, g_code, strlen(g_code));
    memcpy(p + 12 + NAME_LEN, g_name, strlen(g_name));
    memcpy(p + 12 + 2 * NAME_LEN, g_build, strlen(g_build));
    s32(p + 12 + 2 * NAME_LEN + BUILD_LEN, g_lan_ip);
    s16(p + 16 + 2 * NAME_LEN + BUILD_LEN, g_lan_port);
    p[18 + 2 * NAME_LEN + BUILD_LEN] = (uint8_t)g_direct;
    p[BASE] = 'M'; p[BASE + 1] = 'S';                  /* O1: this game's mod set (an older lobby reads only BASE bytes) */
    memcpy(p + BASE + 2, g_mods_wire, (size_t)g_mods_wire_len);
    int n = BASE + 2 + g_mods_wire_len;
    n += ladder_block(p + n, p + 4);                   /* ladder (lobby 1.5): who this player is (an older lobby ignores it) */
    tx(p, n, &g_lobby);
}

static DWORD WINAPI client_thread(LPVOID unused) {
    (void)unused;
    static uint8_t buf[65536];
    while (!setup()) {
        if (g_mode == NET_MODE_LOBBY && g_auto && ms_now() - g_unjoined_since > GONE_AFTER_MS) { lobby_fallback_to_lan(); break; }
        Sleep(3000);
    }
    g_reject[0] = 0;
    { char a[16]; say("UDP port %u, lobby %s:%u", g_lan_port, ip_text(ntohl(g_lobby.sin_addr.s_addr), a), ntohs(g_lobby.sin_port)); }
    uint64_t last_join = 0, last_ping = 0, last_tick = 0, said_blocked = 0;
    for (;;) {
        uint64_t t = ms_now();
        if (g_lan_room_want && g_mode == NET_MODE_LAN) {   /* (room) this player makes a room: its game becomes the network's lobby, with priority */
            InterlockedExchange(&g_lan_room_want, 0);
            if (!g_lan_lobby_id) {
                InterlockedExchange(&g_lan_room_prio, 1);
                if (lan_become_lobby()) {
                    InterlockedExchange(&g_joined, 0); g_unjoined_since = t;
                    AcquireSRWLockExclusive(&g_switch.lock); g_switch.cur = (uint64_t)g_lan_lobby_id; g_switch.cur_prio = 1; g_switch.pending = 0; ReleaseSRWLockExclusive(&g_switch.lock);
                    say("lan: this game is the network's lobby now, for its room: the others move to it");
                } else { InterlockedExchange(&g_lan_room_prio, 0); InterlockedExchange(&g_ar_making, 0); menu_popup("Room", "Your room could not be made (this game could not open its lobby). Try again."); }
            }
        }
        lan_switch_if_asked();
        if (!g_joined && g_auto && g_mode == NET_MODE_LOBBY && t - g_unjoined_since > GONE_AFTER_MS) lobby_fallback_to_lan();
        if (!g_joined && g_mode == NET_MODE_JOIN && !said_blocked && t - g_unjoined_since > LOBBY_TIMEOUT_MS) {
            said_blocked = 1;
            say("join: no answer from the host's game: its router or firewall blocks it. The host can forward its UDP port, turn on online_upnp, "
                "or set online_peer = %s; otherwise use lobby mode", g_room_code[0] ? g_room_code : "<your code>");
            snprintf(g_reject, sizeof g_reject, "no answer from the host (router?)");
        }
        if (!g_joined && t - last_join >= 500) { send_join(); last_join = t; }
        if (g_joined && t - last_ping >= 1000) {
            uint8_t p[4 + 8 + 8 + 1];
            int nd = 0;
            hdr(p, T_PING);
            s64(p + 4, g_cookie);
            s64(p + 12, t);
            AcquireSRWLockShared(&g_peers_lock);
            for (int i = 0; i < g_npeers; i++) nd += fresh(&g_peers[i], t);
            ReleaseSRWLockShared(&g_peers_lock);
            p[20] = (uint8_t)nd;
            tx(p, sizeof p, &g_lobby);
            last_ping = t;
        }
        if (g_joined && t - g_last_pong > LOBBY_TIMEOUT_MS) {
            g_unjoined_since = t;
            InterlockedExchange(&g_joined, 0);
            InterlockedExchange(&g_ping, -1);
            AcquireSRWLockExclusive(&g_peers_lock); g_npeers = 0; g_peers_version = 0; ReleaseSRWLockExclusive(&g_peers_lock);
            if (g_mode == NET_MODE_LAN && !g_lan_lobby_id) lan_become_lobby();          /* the network's lobby went away: be it */
            else say("lost the lobby %s: joining again", g_server);
        }
        if (g_joined && t - last_tick >= 50) { peers_tick(t); last_tick = t; }
        if (g_joined && t - (uint64_t)g_rooms_wanted < 5000 && t - g_rooms_sent >= 2000) rooms_ask(t);     /* O1: while someone looks */
        if (g_joined) mm_tick(t);                                                                         /* O2 */
        if (g_joined) ladder_tick(t);                                                                     /* ladder: a match result until acknowledged */
        if (g_joined) arena_tick(t);                                                                      /* arena (lobby 1.6) */
        struct sockaddr_in from;
        int flen = sizeof from;
        int n = recvfrom(g_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n < 4 || buf[0] != 'S' || buf[1] != 'U' || buf[2] != PROTO) continue;
        if (!same_at(&from, &g_lobby)) { if (buf[3] == T_RB_DIRECT) rbd_rx(buf, n, &from); else if (g_joined && g_direct) from_player(buf, n, &from); continue; }
        switch (buf[3]) {
        case T_DATA:
            if (n >= 16 && g_joined) {
                InterlockedIncrement64(&g_in_relay);
                nnldn_deliver(g32(buf + 4), g16(buf + 12), g32(buf + 8), g16(buf + 14), buf + 16, (uint32_t)(n - 16));
            }
            break;
        case T_WELCOME:
            if (n < 24 || g64(buf + 4) != g_nonce) break;
            g_ip = g32(buf + 12);
            g_cookie = g64(buf + 16);
            g_last_pong = ms_now();
            last_ping = 0;
            if (!g_joined) { char a[16]; say("joined the lobby %s:%u as 10.13.%u.%u (code %s)", ip_text(ntohl(g_lobby.sin_addr.s_addr), a), ntohs(g_lobby.sin_port), g_ip >> 8 & 255, g_ip & 255, g_code); }
            {   static int shown; const char *ssbu_version(void);      /* the version on screen once, to compare with the others' */
                if (!g_joined && !shown) { char t[96]; shown = 1; snprintf(t, sizeof t, "Version %s", ssbu_version()); menu_popup("Online: connected", t); } }
            InterlockedExchange(&g_joined, 1);
            if (n > 24) { int reg = g_ld_registered; ladder_welcome(buf + 24, n - 24); if (reg && !g_ld_registered) send_join(); }   /* (ladder: an unknown id signs up at once) */
            break;
        case T_RESULT_ACK:
            if (g_joined) ladder_ack(buf + 4, n - 4);
            break;
        case T_REJECT:
            if (n >= 12 && g64(buf + 4) == g_nonce) {
                snprintf(g_reject, sizeof g_reject, "%.64s", (const char *)buf + 12);
                say("the lobby refused: %s", g_reject);
                Sleep(2000);
            }
            break;
        case T_PONG:
            if (n >= 16) {
                g_last_pong = ms_now();
                InterlockedExchange(&g_ping, (LONG)(g_last_pong - g64(buf + 4)));
                InterlockedExchange(&g_players, g16(buf + 12));
                InterlockedExchange(&g_rooms, g16(buf + 14));
                if (n >= 20) { InterlockedExchange(&g_searching, g16(buf + 16)); InterlockedExchange(&g_code_players, g16(buf + 18)); }   /* O2 (a newer lobby) */
            }
            break;
        case T_MATCH_STATE:
            if (n >= 4 + 49 && g_joined) mm_state(buf + 4, n - 4, ms_now());
            break;
        case T_ARENA_STATE:
            if (g_joined) arena_state(buf + 4, n - 4);
            break;
        case T_LISTING:
            if (n >= 12 + NI_SIZE && g_joined) {
                if (n >= 12 + NI_SIZE + BUILD_LEN) {         /* O1: the room's build (a LIST with flag 1) */
                    char b[BUILD_LEN + 1] = "";
                    memcpy(b, buf + 12 + NI_SIZE, BUILD_LEN);
                    listing_build(g32(buf + 8), b);
                }
                nnldn_listing(g32(buf + 4), g32(buf + 8), buf + 12);
            }
            break;
        case T_ROOM_INFO:
            if (g_joined) rooms_info(buf + 4, n - 4);
            break;
        case T_ROOMS_END:
            if (g_joined) rooms_end(buf + 4, n - 4);
            break;
        case T_LIST_END:
            if (n >= 9 && g_joined) nnldn_list_end(g32(buf + 4));
            break;
        case T_PEERS:
            if (g_joined && g_direct) read_peers(buf + 4, n - 4);
            break;
        case T_RB:                                       /* R3: a rollback message of the other player */
            if (n > 8 && g_joined) { uint32_t src = g32(buf + 4), mp = g_rb_match_peer;
                if (mp && src != mp && !nnldn_other_ip()) { InterlockedIncrement64(&g_rb_rx_other); break; }     /* (R3 net3) not our opponent */
                if (src && src != g_ip) { g_rb_peer = src; rbn_rx(buf + 8, n - 8); } }
            break;
        }
    }
}

static void leave(void) {
    if (!g_joined) return;
    uint8_t p[12];
    hdr(p, T_LEAVE);
    s64(p + 4, g_cookie);
    tx(p, sizeof p, &g_lobby);
}

void net_start(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return;
    {   /* U1: "Restart the game now" of the settings menu starts this copy while the old one is still closing: wait for it to
         * end first (its UPnP mapping and its lobby place are removed by its exit handlers; at most 15 s) */
        const char *w = getenv("SSBU_RESTART_WAIT_PID");
        if (w && *w) {
            HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(w, NULL, 10));
            if (ph) { printf("[online] waiting for the previous copy (process %s) to end\n", w); fflush(stdout); WaitForSingleObject(ph, 15000); CloseHandle(ph); }
        }
    }
    if (net_online()) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_nonce = rnd64();
        g_unjoined_since = ms_now();
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock != INVALID_SOCKET) {
            DWORD timeout = 50, bytes = 0;
            BOOL off = FALSE;
            int big = 1 << 20;
            setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
            setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
            WSAIoctl(g_sock, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);   /* (no ICMP errors on receive) */
            sim_init();                                  /* SSBU_NET_SIM (test only) */
            HANDLE th = CreateThread(NULL, 0, client_thread, NULL, 0, NULL);
            if (th) { SetThreadDescription(th, L"online lobby client"); CloseHandle(th); }
            atexit(leave);
        } else say("no UDP socket (error %d): online play unavailable", WSAGetLastError());
    }
    InitOnceComplete(&once, 0, NULL);
}

/* net_stats.h: the F2 overlay's online fields, read once per drawn frame on the render thread; never blocks.
 *   state    NET_OFFLINE (online play off), NET_CONNECTING (asked for, no lobby answer yet), NET_CONNECTED (joined the
 *            lobby), NET_DISCONNECTED (no answer from the lobby for GONE_AFTER_MS, or refused)
 *   players  in this player's Local Wireless room (this PC included); 1 while in the lobby but in no room
 *   ping_ms  round trip to the other player of the room: the direct path's last hello answer when the path is direct,
 *            otherwise the round trip to the lobby (relayed: the lobby is in between); -1 = unknown
 *   delay, rollback_*  0: the game's own Local Wireless netcode is delay based and is not ours to measure (rollback, N1). */
void net_stats_get(NetStats *o) {
    memset(o, 0, sizeof *o);
    o->ping_ms = -1;
    if (g_cfg <= 0) { o->state = NET_OFFLINE; return; }
    if (!g_joined) { o->state = net_lobby_gone() || g_reject[0] ? NET_DISCONNECTED : NET_CONNECTING; return; }
    o->state = NET_CONNECTED;
    {   /* R3: an online rollback match (rb_net.c / rb_rollback.c) fills delay and rollback; its own round trip */
        int rbn_started(void), rbn_rtt_ms(void); void rbn_stats(int *delay, int *last, int *maxk);
        if (rbn_started()) { rbn_stats(&o->delay, &o->rollback_last, &o->rollback_max); o->players = 2; o->ping_ms = rbn_rtt_ms(); return; }
    }
    int in_room = nnldn_players();
    o->players = in_room ? in_room : 1;
    o->ping_ms = (int)g_ping;
    if (in_room > 1 && TryAcquireSRWLockShared(&g_peers_lock)) {
        uint64_t t = ms_now();
        for (int i = 0; i < g_npeers; i++) if (fresh(&g_peers[i], t)) { o->ping_ms = g_peers[i].rtt; break; }
        ReleaseSRWLockShared(&g_peers_lock);
    }
}

void net_status(char *out, size_t n) {
    if (!net_online()) { if (n) out[0] = 0; return; }
    if (!g_joined) { snprintf(out, n, "online: %s", g_reject[0] ? g_reject : "connecting to the lobby"); return; }
    static const char *m[] = {"lobby", "LAN", "host", "join"};
    char srch[32] = "";
    if (g_searching >= 0) snprintf(srch, sizeof srch, " (%ld searching)", (long)g_searching);
    snprintf(out, n, "online (%s): %ld players%s, %ld rooms, 10.13.%u.%u, ping %ld ms%s%s", m[g_mode], (long)g_players, srch, (long)g_rooms, g_ip >> 8 & 255, g_ip & 255,
             (long)g_ping, g_mode == NET_MODE_HOST && g_room_code[0] ? ", room code " : "", g_mode == NET_MODE_HOST ? g_room_code : "");
}
