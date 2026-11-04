/* R3 (TEST ONLY, default OFF): the ONLINE ROLLBACK SESSION, the parts that do not touch the game's state
 * (plan\phases\R3_online_rollback.md). The game side (snapshot ring, re-run passes, the pads given to the game) is
 * rb_rollback.c, included by r1_state.c; the transport is net_relay.c (packet type 16, through the lobby / the host's
 * game). This file: the input queues (rb_input.c), the wire format (rb_wire.c), the time sync (rb_timesync.c), the
 * receive queue, the RTT / clock-offset measurement, the per-frame state checksums, a test bot and the session report.
 * Design after Slippi's netplay (project-slippi/Ishiiruka, GPL-2.0-or-later) and melee-unlocked's mu_net
 * (GPL-2.0-or-later), N1 / N1b studies; no code copied. No game content in this file.
 *
 *   SSBU_RB_NET=net        two games over net_relay (SSBU_ONLINE host / join / lobby with a private code)
 *   SSBU_RB_NET=loop[,<f>] one game: the other player is a bot whose pads "arrive" <f> frames late (default 4):
 *                          rollback without a network, for the determinism proof (f = 0: never late = reference)
 *   SSBU_RB_NET_PLAYER=0|1 this game's player (default: 0, 1 in SSBU_ONLINE=join mode)
 *   SSBU_RB_NET_DELAY=<n>  input delay in frames (default: from the measured round trip, 1..4)
 *   SSBU_RB_NET_BOT=<seed> the local player's pads come from a test bot instead of the controller / input sequence
 *   SSBU_RB_NET_LOG=<file> one line per finalized frame: "f <frame> h <checksum>" (compare two games' files)
 *   SSBU_RB_NET_AHEAD=<n>  most frames this game runs ahead of the newest remote pad (default 7 = the ring depth)
 *   SSBU_RB_NET_FIND=0     (R3 net3) no opponent search through the lobby (lobby / LAN mode without a room: the old way)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "../rollback.h"                                         /* rb_input.c, rb_wire.c, rb_timesync.c are units of their own (build.py); rb.h is their API */
#include "net_stats.h"                                  /* (R3 net3) NetMatch, MM_*: the lobby's opponent search (net_relay.c) */
#include "gfx_options.h"                                /* gfx_input_delay: graphics.ini input_delay (F1 > Gameplay) */

int net_rb_send(const void *p, int n);                 /* net_relay.c: 1 = sent (or queued by the latency simulation) */
int net_rb_ready(void);                                /* net_relay.c: joined a lobby (the host's game or a server) */
int net_rb_no_destination(void);                       /* net_relay.c (pub1): messages not sent: no other player in the room on the public lobby */
int net_mode(void);
int net_online(void);

static int g_on = -1, g_loop, g_loop_lat = 4, g_local, g_delay_cfg = -1, g_ahead = 7, g_started, g_peer_started;
static unsigned g_bot_seed; static int g_bot;
static FILE *g_hlog;
static volatile LONG64 g_cur;                          /* the frame this game is on (read by the receive thread for PONGs) */
/* (2026-11-03) when g_cur last moved on (now_us): the clock-offset samples count whole frames AND the time into the frame.
 * Whole frames only, the error stayed fixed while both games ran steady (+2 frames - rtt/2 = +28 ms on one PC, -1 frame -
 * rtt/2 = -21 ms on the other at a 9 ms ping): both games took turns slowing down for a lead that was not there and both
 * ran at ~59 fps (16.94 ms a frame), a refresh missed every second or so on a 60 Hz screen: the stutter. A PONG's time_us
 * (never read by older games) carries the answerer's time into its frame, marked by bit 63. */
static volatile LONG64 g_cur_t_us;
#define PONG_PHASE_MARK (1ull << 63)
static LARGE_INTEGER g_qpf;
static uint64_t now_us(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf); return (uint64_t)(t.QuadPart / (double)g_qpf.QuadPart * 1e6); }

int rbn_on(void) {
    if (g_on >= 0) return g_on;
    const char *e = getenv("SSBU_RB_NET");
    g_on = e && e[0] && e[0] != '0';
    if (!g_on) return 0;
    if (!_strnicmp(e, "loop", 4)) { g_loop = 1; if (e[4] == ',') g_loop_lat = atoi(e + 5); }
    g_local = 0;
    if (!g_loop && net_online() && net_mode() == 3) g_local = 1;      /* NET_MODE_JOIN */
    if ((e = getenv("SSBU_RB_NET_PLAYER"))) g_local = atoi(e) ? 1 : 0;
    if ((e = getenv("SSBU_RB_NET_DELAY"))) g_delay_cfg = atoi(e);
    if ((e = getenv("SSBU_RB_NET_BOT")) && *e) { g_bot = 1; g_bot_seed = (unsigned)strtoul(e, NULL, 0); }
    if ((e = getenv("SSBU_RB_NET_AHEAD")) && atoi(e) > 0) g_ahead = atoi(e);
    if ((e = getenv("SSBU_RB_NET_LOG")) && *e) g_hlog = fopen(e, "w");
    printf("[rbnet] on: %s, local player %d, delay %s, ahead at most %d, bot %s\n", g_loop ? "loop test (no network)" : "over net_relay", g_local + 1, g_delay_cfg >= 0 ? "fixed" : "from the round trip", g_ahead, g_bot ? "on" : "off");
    if (g_loop) printf("[rbnet] loop: remote pads arrive %d frames after the frame that would send them\n", g_loop_lat);
    fflush(stdout);
    return g_on;
}
int rbn_local_player(void) { return g_local; }
int rbn_ahead(void) { return g_ahead; }

/* ---------------------------------------------------------------- the test bot: a pure function of (seed, frame) */
static uint32_t mix32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static RbPad bot_pad(unsigned seed, long long f) {
    /* segments of 6..29 frames; each segment one action. Walking alternates sides around the middle of the stage. */
    RbPad p; memset(&p, 0, sizeof p);
    {   static long long off = -2; if (off == -2) { const char *e = getenv("SSBU_RB_NET_BOT_WALKOFF"); off = e && *e ? atoll(e) : -1; }      /* (R3 lwr, test) from that frame on: left held (the fighter walks off the stage: the match ends) */
        if (off >= 0 && f >= off) { p.lx = -32767; return p; } }
    long long start = 1; uint32_t h = 0; int seg = 0;
    for (;; seg++) { h = mix32(seed * 0x9E3779B9u ^ (uint32_t)seg * 0x85EBCA6Bu); long long len = 6 + (h % 24); if (f < start + len) break; start += len; }
    int act = (int)((h >> 8) % 10);
    int dir = (seg & 1) ? 1 : -1;
    switch (act) {
    case 0: case 1: break;                                            /* nothing */
    case 2: case 3: p.lx = (int16_t)(dir * 32767); break;            /* walk / run */
    case 4: p.buttons = 1ull << 0; break;                            /* A (jab) */
    case 5: p.buttons = 1ull << 1; break;                            /* B (neutral special) */
    case 6: if (((f - start) & 15) < 3) p.buttons = 1ull << 2; break;        /* X (jump), tapped */
    case 7: p.lx = (int16_t)(dir * 32767); p.buttons = 1ull << 0; break;     /* forward A */
    case 8: p.ly = -32767; break;                                     /* crouch */
    case 9: p.buttons = (((f - start) & 7) < 4) ? 1ull << 0 : 0; break;       /* A mashed */
    }
    return p;
}
int rbn_bot_on(void) { return g_bot; }

/* ---------------------------------------------------------------- the match handshake (before the match; rb_hs.h)
 * Over the same channel as the pads (packet type 16), message types 0x48.. (the pad messages are 1..4):
 *   CHOICE  joiner -> host, every 100 ms from its match set-up: its own player-1 choice (fighter, colour, controls)
 *   PROPOSE host -> joiner, every 100 ms once the host is at its set-up and has the joiner's CHOICE: the whole match
 *           (player 1 = the host's choice, player 2 = the joiner's, the host's stage / items / stocks, a new seed)
 *   ACCEPT  joiner -> host (answered on the receive thread to every PROPOSE: a lost ACCEPT is made good)
 *   REFUSE  either way: protocol / game build differ, or the other game stopped waiting (the reason is printed on both sides)
 *   BLOCK   both, at the match start: the game's match block (rules + the two player records) in chunks; each game
 *           compares it with its own and prints the differing ranges (a check, nothing is changed)
 * Header of every message: type, protocol, game build, exe link stamp (a different stamp is only reported), nonce, echo.
 * (hs12) Every wait has its own nonce (a fresh random number, never 0): CHOICE carries the joiner's, PROPOSE the host's and
 * echoes the joiner's, ACCEPT echoes the host's; the host takes only CHOICEs that arrive while it waits. A message of an
 * older wait is ignored, so a wait given up on one side and the next match set-up never mix (two matches in one session).
 * While waiting: a message box over the last picture (menu_notice + nvn_front_osk_redraw: the game itself is held at its
 * set-up), Esc (game window in front) or B held on a controller stops waiting, the wait limit is SSBU_RB_NET_HS_WAIT
 * (seconds, default 120). No agreement = this match is played locally (Final Destination, one stock) and left at once
 * (rb_rollback.c: player 1 walks off the stage), back to the menus; the other game is told (REFUSE) so that it stops waiting. */
#include "handshake.h"
#define RBH_PROTO 7                                     /* 3 (hs12): RbhMatch carries the stage set-up context; 4 (hs14): PADS end with a mode byte, the stage is drawn from both picks; 5 (R3 lwr): Local Wireless room matches, LAYOUT; 6 (2026-11-02): a Battle Arena's spectators (RBS_*; an older game refuses: native) */
enum { RBH_CHOICE = 0x48, RBH_PROPOSE = 0x49, RBH_ACCEPT = 0x4a, RBH_REFUSE = 0x4b, RBH_BLOCK = 0x4c, RBH_LAYOUT = 0x4d };      /* (R3 lwr) LAYOUT: a room match, each game's own player's control layout */
#pragma pack(push, 1)
typedef struct { uint8_t type, proto; int16_t game_build; uint32_t exe_stamp; uint32_t nonce, echo; RbhMatch m; } RbhMsg;
typedef struct { uint8_t type, proto; int16_t game_build; uint32_t exe_stamp; uint32_t nonce; uint16_t off, len, total; uint8_t data[768]; } RbhBlock;
#pragma pack(pop)
extern int g_game_build;                                /* hostrt.c */
uint64_t ga(uint64_t addr_13_0_4);                      /* hostrt.c (GA) */
void host_fixed_random_reseed(uint64_t seed);           /* os_threads.c: nn::os::GenerateRandomBytes from the agreed seed on */
void menu_notice(const char *text);                     /* menu.cpp (menu_stub.c: nothing) */
void nvn_front_osk_redraw(void); long long nvn_front_presents(void);      /* nvn_gl.c: the last frame presented again (with the notice) */
uint64_t nnhid_menu_buttons(void);                      /* nnhid.c: every controller's buttons, Npad bit order (B = bit 1), not seen by the game */
static SRWLOCK g_hs_lock = SRWLOCK_INIT;
static volatile LONG g_hs_round;                        /* this game's match set-ups so far (log lines) */
static volatile LONG g_hs_waiting;                      /* rbn_handshake is waiting */
static uint32_t g_hs_nonce;                             /* this game's nonce of the newest wait (0 = none yet) */
static uint32_t g_hs_peer_nonce;                        /* the other game's nonce seen in this wait (0 = none yet) */
static RbhMsg g_hs_peer;  static int g_hs_have_peer;    /* host: the joiner's newest CHOICE of this wait */
static RbhMsg g_hs_prop;  static int g_hs_have_prop;    /* host: the proposal sent; joiner: the proposal accepted (this wait) */
static volatile LONG g_hs_accepted, g_hs_refused; static char g_hs_why[160];
static uint64_t g_hs_seed; static uint32_t g_match_id;  /* the agreed match: its seed, and its id (tag of the pad and block messages) */
/* (2026-10-30) the input delay: each game's own (SSBU_RB_NET_DELAY, else graphics.ini input_delay = F1 > Gameplay, default 3;
 * 0 = auto) travels in RbhMatch.pad0 (the joiner's CHOICE); the host's proposal carries the larger of the two and both games
 * play the match with it (a different delay on each side would desync: each takes the other's first frames as empty) */
static int g_hs_delay;
static int my_delay(void) { int d = g_delay_cfg >= 0 ? g_delay_cfg : gfx_input_delay(); return d < 0 ? 0 : d > 12 ? 12 : d; }
static volatile LONG g_hs_cancelled;                    /* this match: 1 = no agreement: played locally and left at once; 2 = (pub2) set up
                                                           * inside a Local Wireless room: the game's own netcode plays it, no rollback, nothing changed */
static int g_match_n;                                   /* (hs12) online matches of this session so far (1 = the first; rbn_start) */
/* (hs12) the tag byte at the end of every PADS message: the agreed match id and the match number of the session. Run tw7:
 * the other game's last pads of match 1 arrived after this game's match-1 reset and were taken at the start of match 2
 * (their acknowledgement 1081 made each game send no pads any more: both waited for ever at frame 7).
 * (owner 2026-10-31: "si vos cerras y volves a entrar a mi sala sin que yo cierre mi juego explota") the match number is
 * each game's own count: the other player closed the game and came back, here it was match 2, there match 1, every pad
 * was dropped as of another match and both waited for ever at frame 0. The tag is now of the agreed id only (as match 1
 * had it), and the host, who draws the seed, never draws one whose tag is that of its own last match (hs_new_seed). */
static int match_tag(uint64_t seed) { return (uint8_t)((((uint32_t)(seed ^ (seed >> 32))) | 1u) ^ 0x3bu); }
static int g_last_tag = -1;                             /* the tag of this game's last match (rbn_start), -1 before the first */
static uint8_t pads_tag(void) { return (uint8_t)(g_match_id ^ 0x3bu); }
static long long g_hs_tx, g_hs_rx;
#define RBH_BLK 0xa48                                   /* match block + 0 .. + 0x580 + 2 * 0x260 + 8 */
static uint8_t g_hs_mine_blk[RBH_BLK], g_hs_peer_blk[RBH_BLK]; static int g_hs_mine_ok, g_hs_peer_got, g_hs_blk_done; static uint8_t g_hs_peer_have[RBH_BLK / 768 + 1];
static uint32_t exe_stamp(void) {
#ifdef _WIN32
    const uint8_t *b = (const uint8_t *)GetModuleHandleA(NULL); if (!b) return 0;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
#else                                                   /* POSIX builds: ELF / Mach-O have no link time: the exe's modification time */
    char p[MAX_PATH]; struct stat st;
    DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
    return n && n < MAX_PATH && !stat(p, &st) ? (uint32_t)st.st_mtime : 0;
#endif
}
int rbn_handshake_wanted(void) {
    static int w = -1;
    if (w < 0) { const char *e = getenv("SSBU_RB_NET"), *h = getenv("SSBU_RB_NET_HANDSHAKE");
        w = e && e[0] && e[0] != '0' && _strnicmp(e, "loop", 4) && !(h && h[0] == '0'); }
    return w;
}
uint64_t rbn_match_seed(void) {
    if (g_loop && !g_hs_seed) { const char *e = getenv("SSBU_RB_NET_LOOP_SEED"); if (e && *e) return strtoull(e, NULL, 0); }   /* (hs15, test) the loop game's generators from a seed */
    return g_hs_seed;
}
static volatile LONG g_hs_room;                         /* (pub2) the last set-up was inside a Local Wireless room (until the next set-up: the fight-off of the
                                                           * room's waiting-stage fight comes AFTER the room match's set-up, run rm4) */
int rbn_match_cancelled(void) { return g_hs_room ? 2 : (int)g_hs_cancelled; }      /* 0, 1 (not agreed: walk off) or 2 (pub2: a room fight, the game's own netcode) */
/* (2026-11-04) this set-up's handshake agreed on a rollback match between the two fighters (cleared at every set-up):
 * rollback_controller.c rbg_pad gives both players no pad in the fight's frames before the match start */
static volatile LONG g_hs_agreed;
int rbn_agreed(void) { return (int)g_hs_agreed; }
/* (R3 lwr) debug_mode.c tells which set-up this is: 1 = a Local Wireless room's waiting-stage fight (the record builder called from
 * the waiting-stage set-up, one player), 0 = any other (the select screens' "ready to fight", also the room's match) */
static int g_setup_waiting, g_hs_lwr;
void rbn_set_setup_waiting(int waiting) { g_setup_waiting = waiting; }
int rbn_room_rollback(void) { return g_hs_lwr; }
/* (2026-11-02) a Battle Arena of the game's own Online menu (SSBU_ONLINE_MENU=1, our NEX server): debug_mode.c says at the
 * set-up whether this PC's player 1 panel is the other game's (network) player: then this game is player 2 */
static int g_arena_p1_net;
void rbn_arena_hint(int p1_is_network) { g_arena_p1_net = p1_is_network; }       /* (R3 lwr) the last agreed match is a Local Wireless room's match played with rollback */
static int rbn_peer_heard_ms(void);                     /* (R3 lwr) ms since the other game's rollback session was last heard, -1 = never */
static volatile LONG g_lay_have_mine, g_lay_have_peer; static uint8_t g_lay_peer_has; static uint16_t g_lay_peer[11]; static uint8_t g_lay_peer_ctl[0x2d];
static RbhMsg g_lay_mine;
const char *rbn_hs_why(void) { return g_hs_why; }      /* (R3 pub1) why the last handshake failed (rb_rollback.c's notice) */
static void rtt_reset(void);                            /* (R3 net3) a new opponent: its round trip is measured afresh */
static void hs_head(RbhMsg *m, int type) { memset(m, 0, sizeof *m); m->type = (uint8_t)type; m->proto = RBH_PROTO; m->game_build = (int16_t)g_game_build; m->exe_stamp = exe_stamp(); m->nonce = g_hs_nonce; }
static void hs_send(const void *p, int n) { if (net_rb_send(p, n)) g_hs_tx++; }
static int hs_check(const RbhMsg *m, char *why, size_t n) {           /* 0 = the other game cannot play this match with us */
    if (m->proto != RBH_PROTO) { snprintf(why, n, "handshake protocol %d here, %d there", RBH_PROTO, m->proto); return 0; }
    if (m->game_build != (int16_t)g_game_build) { snprintf(why, n, "game build %d here, %d there (13.0.4 = 0, 13.0.5 = 1): both players need the same game version", g_game_build, m->game_build); return 0; }
    return 1;
}
static void hs_refuse(uint32_t echo, const char *why) {
    RbhMsg r; hs_head(&r, RBH_REFUSE); r.echo = echo; snprintf((char *)&r.m, sizeof r.m, "%s", why); hs_send(&r, sizeof r);
}
/* (R3 lwr) a room match: each game knows only its OWN player's 11-word text field of the panel state (+ 0x54, record + 0x28:
 * the name-tag text in runs lwr17..20; hs12 took it for the control layout) (the other player is a network player in the room's
 * records). At player 2's record (both games are at the same set-up) each sends its own and waits for the other's
 * (at most SSBU_RB_NET_ROOM_WAIT s / 2; sent again every 50 ms, a received one is answered once). 1 = *other is the other
 * player's layout (*other_has: it had one); 0 = not received (each game keeps what it has: the match may differ, logged). */
/* (owner 2026-10-22) the message also carries the player's own name-tag controls: 0x2b bytes (profile + 0x24 .. + 0x4e) and the
 * option byte of its controller kind (debug_mode.c lwr_own_controls), in RbhMatch.ctx_hist (not used by a LAYOUT otherwise). */
int rbn_room_layout(const uint16_t mine[11], int has, const uint8_t mine_ctl[0x2d], uint16_t other[11], int *other_has, uint8_t other_ctl[0x2d]) {
    AcquireSRWLockExclusive(&g_hs_lock);
    hs_head(&g_lay_mine, RBH_LAYOUT); g_lay_mine.nonce = g_match_id; memcpy(g_lay_mine.m.layout[0], mine, sizeof g_lay_mine.m.layout[0]); g_lay_mine.m.has_layout[0] = (uint8_t)(has != 0);
    memcpy(g_lay_mine.m.ctx_hist, mine_ctl, 0x2d);      /* (+ 0x2c: 1 = a GameCube controller) */
    g_lay_have_mine = 1;
    RbhMsg ans = g_lay_mine; ans.echo = 1;
    ReleaseSRWLockExclusive(&g_hs_lock);
    double limit = (getenv("SSBU_RB_NET_ROOM_WAIT") ? atof(getenv("SSBU_RB_NET_ROOM_WAIT")) : 10.0) * 500.0;
    uint64_t t0 = GetTickCount64(), last = 0;
    while (!g_lay_have_peer && GetTickCount64() - t0 < limit) {
        if (GetTickCount64() - last >= 50) { last = GetTickCount64(); hs_send(&g_lay_mine, sizeof g_lay_mine); }
        Sleep(2);
    }
    for (int k = 0; k < 3; k++) hs_send(&ans, sizeof ans);          /* (the other game may still wait: answers that do not ask for one back) */
    int ok = g_lay_have_peer != 0;
    if (ok) { memcpy(other, g_lay_peer, sizeof g_lay_peer); *other_has = g_lay_peer_has; memcpy(other_ctl, g_lay_peer_ctl, sizeof g_lay_peer_ctl); }
    printf("[rbnet] room match: player name and controls (name tag text, profile controls): this game's player's sent, the other player's %s after %llu ms\n", ok ? "received" : "NOT RECEIVED (the match may differ)", (unsigned long long)(GetTickCount64() - t0));
    fflush(stdout);
    return ok;
}
/* the receive thread: a handshake message (1 = it was one) */
static int hs_rx(const uint8_t *p, int n) {
    if (n < 1 || p[0] < RBH_CHOICE || p[0] > RBH_LAYOUT) return 0;
    g_hs_rx++;
    if (p[0] == RBH_BLOCK) {
        if (n < (int)offsetof(RbhBlock, data)) return 1;
        RbhBlock b; memset(&b, 0, sizeof b); memcpy(&b, p, (size_t)n < sizeof b ? (size_t)n : sizeof b);
        if (b.nonce == g_match_id && b.off + b.len <= RBH_BLK && b.len <= sizeof b.data) { AcquireSRWLockExclusive(&g_hs_lock); memcpy(g_hs_peer_blk + b.off, b.data, b.len); g_hs_peer_have[b.off / 768] = 1; ReleaseSRWLockExclusive(&g_hs_lock); }
        return 1;
    }
    if (n < (int)sizeof(RbhMsg)) return 1;
    RbhMsg m; memcpy(&m, p, sizeof m);
    char why[160];
    AcquireSRWLockExclusive(&g_hs_lock);
    int waiting = g_hs_waiting && g_hs_nonce;
    switch (m.type) {
    case RBH_CHOICE:                                    /* (host, while it waits: an older CHOICE is not for this match) */
        if (g_local != 0 || !waiting) break;
        if (!hs_check(&m, why, sizeof why)) { snprintf(g_hs_why, sizeof g_hs_why, "%s", why); g_hs_refused = 1; hs_refuse(m.nonce, why); break; }
        g_hs_peer = m; g_hs_have_peer = 1; g_hs_peer_nonce = m.nonce;
        if (g_hs_have_prop && g_hs_prop.echo == m.nonce) hs_send(&g_hs_prop, sizeof g_hs_prop);   /* our proposal was lost */
        break;
    case RBH_PROPOSE:                                   /* (joiner: for our newest wait; answered after the wait too, a lost ACCEPT is made good) */
        if (g_local != 1 || !g_hs_nonce || m.echo != g_hs_nonce) break;
        if (!hs_check(&m, why, sizeof why)) { snprintf(g_hs_why, sizeof g_hs_why, "%s", why); g_hs_refused = 1; hs_refuse(m.nonce, why); break; }
        if (g_hs_have_prop && g_hs_prop.nonce != m.nonce) break;     /* (one proposal per wait) */
        if (!g_hs_have_prop && !waiting) break;
        g_hs_prop = m; g_hs_have_prop = 1; g_hs_peer_nonce = m.nonce;
        { RbhMsg a; hs_head(&a, RBH_ACCEPT); a.echo = m.nonce; a.m = m.m; hs_send(&a, sizeof a); }
        break;
    case RBH_ACCEPT:                                    /* (host) */
        if (g_local == 0 && waiting && g_hs_have_prop && m.echo == g_hs_prop.nonce && !memcmp(&m.m, &g_hs_prop.m, sizeof m.m)) g_hs_accepted = 1;
        break;
    case RBH_LAYOUT:                                    /* (R3 lwr) the other game's own player's control layout for this match; answered once with ours */
        if (m.nonce != g_match_id || !g_match_id || m.proto != RBH_PROTO) break;
        memcpy(g_lay_peer, m.m.layout[0], sizeof g_lay_peer); g_lay_peer_has = m.m.has_layout[0]; memcpy(g_lay_peer_ctl, m.m.ctx_hist, sizeof g_lay_peer_ctl); g_lay_have_peer = 1;
        if (g_lay_have_mine && !m.echo) { RbhMsg a = g_lay_mine; a.echo = 1; hs_send(&a, sizeof a); }     /* (2026-11-03) the answer is marked as one (echo 1): answered
                                                         * with echo 0, each game answered the other's answer for ever (~21,000 messages a second, the ping ~60 ms) */
        break;
    case RBH_REFUSE:                                    /* about our wait, or from the other game's wait we deal with */
        if (!waiting || !((m.echo && m.echo == g_hs_nonce) || (g_hs_peer_nonce && m.nonce == g_hs_peer_nonce))) break;
        snprintf(g_hs_why, sizeof g_hs_why, "the other game refused: %.120s", (const char *)&m.m); g_hs_refused = 2;
        break;
    }
    ReleaseSRWLockExclusive(&g_hs_lock);
    return 1;
}
static uint64_t hs_rand(void) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    uint64_t s = (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64() * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)&t;
    s ^= s >> 29; s *= 0xBF58476D1CE4E5B9ull; s ^= s >> 32; s *= 0x94D049BB133111EBull; s ^= s >> 31;
    return s;
}
static uint64_t hs_new_seed(void) {
    const char *e = getenv("SSBU_RB_NET_SEED");
    uint64_t s = e && *e ? strtoull(e, NULL, 0) : hs_rand();
    if (!s) s = 1;
    while (match_tag(s) == g_last_tag) s += 2;            /* (pads_tag) +2 always changes the tag byte */
    return s;
}
static void hs_print(const char *what, const RbhMatch *m) {
    printf("[rbnet] handshake: %s: player 1 ui_chara %016llx colour %d controls %s, player 2 ui_chara %016llx colour %d controls %s; stage entry %016llx form %u flags %x second %016llx; hazards %d, items %u, rule kind %d, stocks %d; seed %016llx\n", what,
           (unsigned long long)m->chara[0], m->colour[0], m->has_layout[0] ? "own layout" : "none", (unsigned long long)m->chara[1], m->colour[1], m->has_layout[1] ? "own layout" : "none",
           (unsigned long long)*(const uint64_t *)m->stage, *(const uint32_t *)(m->stage + 8), *(const uint32_t *)(m->stage + 12), (unsigned long long)*(const uint64_t *)(m->stage + 16),
           m->hazards, m->items, m->rule_kind, m->stock, (unsigned long long)m->seed);
    fflush(stdout);
}
/* ---------------------------------------------------------------- (2026-11-02) spectators of a Battle Arena
 * A member of the arena whose seat does not fight this match (debug_mode.c: the select table + 0x30) watches it. Its game runs
 * the same match set-up as the fighters' (under the game's own netcode every member's game simulates the arena's match itself
 * from the fighters' inputs), so it can play the match as a LOCAL match fed with both fighters' FINAL pads: no ring, never a
 * guess (the controller's g_rbg_noring path: every pass waits for both players' pads of its frame). Messages through the
 * lobby's arena group (net_relay.c net_rbg_send, packet type 27, key = the arena's gathering id, always via the lobby: one
 * message, every other member; an older lobby drops them: nobody hears the spectators and the match stays native):
 *   HERE   fighter -> group, every 100 ms during its handshake: puts it in the group (the spectators' HELLOs reach it)
 *   HELLO  spectator -> group, every 100 ms at its set-up: its seat, the fighting seats (mask), what it sees of the match
 *          (seat 0's fighter, rule kind, stocks); flags 1 = it refuses (the game's own netcode shows it the match)
 *   MATCH  player 1 -> group, every 100 ms from its player-2 record until its fight starts: the agreed RbhMatch (seed, stage
 *          set-up inputs), the seats of players 1 / 2, both players' layouts and name-tag controls as player 1 wrote them
 *   NATIVE fighter -> group (3 times): the game's own netcode plays this match (no agreement, not 1-on-1, ...)
 *   PADS   fighter -> group, every pass of its fight (and every 50 ms while it waits): its newest 8 FINAL pads as an rb_wire
 *          PADS message (player = its rank, its finalized checksum frame)
 *   NACK   spectator -> group: rank, from frame: that fighter answers with up to 64 pads from that frame (its history keeps
 *          RBS_HIST frames: a spectator whose fight starts late gets the first frames too)
 * Header: type, protocol (RBH_PROTO), game build, exe stamp, arena; PADS / NACK then the match tag (pads_tag) and the rank.
 * Player 1 proposes the match only once every spectator's HELLO agrees (protocol, build, stamp, mask, set-up); else both
 * fighters fail their handshake (native) and send NATIVE. A spectator converts only on MATCH: nobody converts unless all
 * agreed. Fighters never wait for a spectator during the fight. SSBU_RB_NET_ARENA_SPEC=1: the game's own netcode for every
 * arena match with spectators (the old way); =0 (test): the fighters play rollback and the spectators are not fed;
 * SSBU_RB_NET_SPEC_WAIT=<s>: how long a spectator waits for MATCH at its set-up (default 20, longer than the fighters' 10). */
enum { RBS_HELLO = 0x50, RBS_MATCH = 0x51, RBS_NATIVE = 0x52, RBS_PADS = 0x53, RBS_NACK = 0x54, RBS_HERE = 0x55 };
#define RBS_B 4                                         /* the spectator's jitter buffer: frames of both fighters' pads it aims to hold */
#define RBS_HIST 4096                                   /* a fighter's history of its own final pads (68 s), for NACKs */
#define RBS_CK 1024                                     /* the fighters' checksums kept until the spectator's frame is final */
#define RBS_LCTL 0x2d                                   /* debug_mode.c LWR_CTL + 2: a player's name-tag controls, option byte, GameCube flag */
#pragma pack(push, 1)
typedef struct { uint8_t type, proto; int16_t game_build; uint32_t exe_stamp; uint32_t gid; } RbsHead;
typedef struct { RbsHead h; uint8_t seat, mask, flags, rule_kind, stock, pad[3]; uint64_t chara0; } RbsHello;
typedef struct { RbsHead h; uint32_t match_id; uint8_t seat_of_rank[2], ctl_ok, pad; RbhMatch m; uint16_t lay[2][11]; uint8_t ctl[2][RBS_LCTL]; } RbsMatch;
typedef struct { RbsHead h; char why[160]; } RbsNative;
typedef struct { RbsHead h; uint8_t tag, rank; int32_t from; } RbsNack;
typedef struct { RbsHead h; uint8_t tag, rank; } RbsPadsHead;                     /* + an rb_wire PADS message */
#pragma pack(pop)
static SRWLOCK g_rbs_lock = SRWLOCK_INIT;
static volatile uint32_t g_rbs_gid;                     /* the arena of this match's group (0 = none: no message sent or taken) */
static int g_rbs_sor[2];                                /* the seats of players 1 and 2 */
static volatile LONG g_rbs_need;                        /* player 1, in its handshake: the spectators whose HELLO it needs */
static volatile LONG g_rbs_fighter;                     /* this game fights the agreed match of an arena with spectators: PADS to the group */
static RbsHello g_rbs_expect;                           /* player 1: what a spectator's HELLO must say (g_rbs_lock) */
static volatile LONG g_rbs_seats, g_rbs_bad; static char g_rbs_bad_why[200];      /* player 1: seats with a matching HELLO; a HELLO that refused / differs */
static RbhMatch g_rbs_agreed;                           /* player 1: the agreed match (MATCH) */
static RbsMatch g_rbs_match;                            /* spectator: the MATCH received; player 1: the one sent (g_rbs_lock) */
static volatile LONG g_rbs_match_tx; static uint64_t g_rbs_match_until;           /* player 1: MATCH sent again every 100 ms (ping thread) */
static volatile LONG g_rbs_wait, g_rbs_got; static char g_rbs_native_why[200];    /* spectator at its set-up: waiting; 1 = MATCH, 2 = NATIVE */
static volatile LONG g_spec;                            /* this game watches the match being set up / played (fed by the fighters) */
static int g_spec_saved_local;                          /* (g_local is -1 while watching: both players are remote) */
static uint16_t g_spec_lay[2][11]; static uint8_t g_spec_ctl[2][RBS_LCTL]; static int g_spec_ctl_ok;
static volatile LONG64 g_rbs_tx, g_rbs_rx;
static int rbs_spectate(long round, uint32_t gid, int players, int seat, const int sor[2], const RbhMatch *mine, RbhMatch *out);
static void rbs_head(void *m, int type);
static void rbs_send(const void *p, int n);
static void rbs_native(const char *why, int spectator_seat);      /* -1: a fighter's NATIVE; else that spectator's refusing HELLO */
static void rbs_hist_reset(void);
static int rbs_popc(LONG v) { int n = 0; while (v) { n += (int)(v & 1); v >>= 1; } return n; }
int rbn_spectating(void) { return g_spec != 0; }
/* Esc (this game's window in front) or B on any controller, held 0.3 s; SSBU_RB_NET_TEST_CANCEL=<s>: as if pressed after s seconds (test) */
static int hs_stop_pressed(double waited_ms) {
    static ULONGLONG since; static double test = -2;
    if (test == -2) { const char *e = getenv("SSBU_RB_NET_TEST_CANCEL"); test = e && *e ? atof(e) * 1000.0 : -1; }
    if (test >= 0 && waited_ms >= test) return 1;
    int down = 0;
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) down = 1;
    if (waited_ms > 500 && (nnhid_menu_buttons() & 2)) down = 1;
    ULONGLONG now = GetTickCount64();
    if (!down) { since = 0; return 0; }
    if (!since) since = now;
    return now - since >= 300;
}
/* (hs14) the fair rule for what the two players chose differently (host, when it builds the proposal):
 *  - rules (stock / time kind, stocks, items, hazards) and the stage set-up's inputs: the host's (the joiner sees them in the
 *    proposal and its game takes them; a different rule KIND on the joiner's screens is only reported, see debug_mode.c);
 *  - the stage: one of the two players' own picks, drawn with the agreed seed (bit 40 of it: 0 = the host's pick, 1 = the
 *    joiner's), so over many matches each player plays on his own pick half the time and neither game can steer it (the seed
 *    is new for every match). A joiner pick that is not a stage entry (no ui_stage id) leaves the host's.
 *    SSBU_RB_NET_STAGE=host / joiner forces one side (tests). */
static void hs_stage_rule(RbhMatch *m, const RbhMatch *peer) {
    const char *e = getenv("SSBU_RB_NET_STAGE");
    uint64_t hs = *(const uint64_t *)m->stage, js = *(const uint64_t *)peer->stage; uint32_t hf = *(const uint32_t *)(m->stage + 8);
    int valid = (js >> 56) == 0x69, draw = (int)((m->seed >> 40) & 1);
    int joiner = valid && (e && !_stricmp(e, "joiner") ? 1 : e && !_stricmp(e, "host") ? 0 : draw);
    if (joiner) memcpy(m->stage, peer->stage, sizeof m->stage);
    printf("[rbnet] handshake: stage: the host picked %016llx form %u, the joiner %016llx form %u; drawn with the agreed seed (%s): the %s's pick%s\n",
           (unsigned long long)hs, hf,
           (unsigned long long)js, *(const uint32_t *)(peer->stage + 8), e && *e ? e : "random", joiner ? "joiner" : "host", valid ? "" : " (the joiner's is not a stage entry)");
    fflush(stdout);
}
int rbn_handshake(const RbhMatch *mine, RbhMatch *out) {
    if (!rbn_handshake_wanted() || rbn_on() <= 0 || g_loop) return 0;
    LONG round = InterlockedIncrement(&g_hs_round);
    /* (R3 pub2: the Local Wireless "deadlock" of lb1 / lb3 and of the testers 2026-10-20) a fight set up while this game is in a Local
     * Wireless room is either the room's waiting-stage fight (each game alone, one player) or the room's own match, which the game
     * plays with its own online netcode: BOTH games build the records of BOTH players from the room (run nb2: player 1 = the room
     * maker's fighter, player 2 = the joiner's, the same on both PCs). Rollback cannot run under that netcode, and holding one game
     * here stopped its traffic: the other game waited on its loading screen for it, then left the room. So no handshake and no
     * rollback for these fights: they are played exactly as without rollback, whatever the other game's setting; a box says so
     * once. Rollback matches: Smash (versus) with a room code (host / join modes). SSBU_RB_NET_ROOM_MATCH=1: the old way (test). */
    InterlockedExchange(&g_hs_room, 0);
    InterlockedExchange(&g_hs_agreed, 0);
    /* (2026-11-02) spectators: what the last set-up left (a watched match that never reached its fight; a fighter's group) */
    AcquireSRWLockExclusive(&g_hs_lock); if (g_spec) { g_local = g_spec_saved_local; InterlockedExchange(&g_spec, 0); } ReleaseSRWLockExclusive(&g_hs_lock);
    InterlockedExchange(&g_rbs_fighter, 0); InterlockedExchange(&g_rbs_need, 0); InterlockedExchange(&g_rbs_match_tx, 0); g_rbs_gid = 0;
    /* (R3 lwr, owner 2026-10-21: "il rollback deve essere attivo anche nel local wireless con le stanze") the room's MATCH (not its
     * waiting-stage fight) with rollback on both games: the room has already agreed fighters, stage and rules on both PCs, so the
     * handshake here only checks that and agrees on a seed (no menus to wait for: both games reach this set-up together when the
     * room maker starts the match; at most SSBU_RB_NET_ROOM_WAIT s, default 10). debug_mode.c then makes the fight a local versus
     * match (the room's melee mode "local wireless" -> "local", match block + 0), so the game's own online netcode does not play it,
     * and rb_rollback.c runs it as every rollback match (room maker = player 1, port i = player i). It is tried only when the other
     * player of a 1-on-1 room is heard by this game's rollback session (its pings reach only a game with rollback on); otherwise,
     * and whenever the agreement fails, the fight is left exactly as the game set it up (pub2: the game's own netcode), so a
     * player with rollback off is never held. SSBU_RB_NET_ROOM_ROLLBACK=0: the pub2 way for every room fight. */
    int lwr = 0, arena = 0; uint32_t agid = 0;
    g_hs_lwr = 0;
    /* (2026-11-02) a Battle Arena's match (the match block's melee mode 0x14: 1 in a Local Wireless room, 0 a local versus
     * match): played like a Local Wireless room's (R3 lwr below: the arena's set-up on both PCs, only the seed new, made a
     * local versus match with rollback), the opponent found in the lobby by the arena (our NEX server says which one this
     * player is in: online_menu.inc), 1-on-1 only. Anything not agreed: the game's own online netcode plays it, unchanged. */
    {   int dm_melee_mode(void), online_menu_arena(uint32_t *gid, int *players, int *own_seat), dm_arena_fighters(int seat_of_rank[2]);
        const char *rr = getenv("SSBU_RB_NET_ARENA"), *sp = getenv("SSBU_RB_NET_ARENA_SPEC");
        int mode = dm_melee_mode();
        if (mode == 0x14) {
            /* (2026-11-02, second round) the fighters are any two seats (debug_mode.c: the select table + 0x30); this game's own
             * seat is its place in the arena's join order (our NEX server); a member whose seat does not fight is a spectator.
             * Every member decides the same from the same table and member list. (2026-11-02, spectators) a spectator watches
             * the match fed by the fighters (rbs_spectate: HELLO, then player 1's MATCH or NATIVE); player 1 proposes only once
             * every spectator said HELLO; SSBU_RB_NET_ARENA_SPEC=1: with spectators the game's own netcode plays the match for
             * everyone (the old way), =0 (test): the fighters play with rollback, the spectators are not fed. */
            int players = 0, seat = -1, sor[2] = {0, 1}, ok = online_menu_arena(&agid, &players, &seat), nfight = dm_arena_fighters(sor);
            int rank = seat == sor[0] ? 0 : seat == sor[1] ? 1 : -1, spectators = players - 2, spec_test = sp && sp[0] == '0', spec_off = sp && sp[0] == '1';
            const char *why = rr && rr[0] == '0' ? "arena rollback off (SSBU_RB_NET_ARENA=0)" : !ok ? "our NEX server did not say which arena" : !agid ? "the server knows no arena of this player"
                            : nfight != 2 ? "not a 1-on-1 match" : seat < 0 ? "this player's seat is not known" : spectators > 0 && spec_off ? "spectators in the arena and SSBU_RB_NET_ARENA_SPEC=1 (the game's own netcode for everyone)"
                            : rank < 0 && spec_test ? "this player watches (TEST SSBU_RB_NET_ARENA_SPEC=0: spectators are not fed)" : NULL;
            printf("[arena] decision at the set-up (handshake %ld): arena %u, %d members (%d spectators), %d fighting (seats %d and %d), this game is seat %d = %s: %s\n", (long)round, agid, players, spectators > 0 ? spectators : 0, nfight,
                   sor[0], sor[1], seat, rank == 0 ? "player 1" : rank == 1 ? "player 2" : "a spectator", why ? why : rank < 0 ? "watch it fed by the fighters (rollback spectator)" : spectators > 0 && !spec_test ? "rollback, with the spectators fed" : "rollback");
            fflush(stdout);
            if (!why && rank < 0) return rbs_spectate(round, agid, players, seat, sor, mine, out);
            if (!why) {
                arena = lwr = 1;
                AcquireSRWLockExclusive(&g_hs_lock); g_local = rank; ReleaseSRWLockExclusive(&g_hs_lock);
                if (spectators > 0 && !spec_test) {         /* (2026-11-02) the group: player 1 waits for every spectator's HELLO before it proposes */
                    AcquireSRWLockExclusive(&g_rbs_lock);
                    g_rbs_gid = agid; g_rbs_sor[0] = sor[0]; g_rbs_sor[1] = sor[1]; g_rbs_seats = 0; g_rbs_bad = 0; g_rbs_bad_why[0] = 0;
                    memset(&g_rbs_expect, 0, sizeof g_rbs_expect); rbs_head(&g_rbs_expect, RBS_HELLO);
                    g_rbs_expect.mask = (uint8_t)((1u << sor[0]) | (1u << sor[1])); g_rbs_expect.rule_kind = mine->rule_kind; g_rbs_expect.stock = mine->stock; g_rbs_expect.chara0 = mine->chara[0];
                    InterlockedExchange(&g_rbs_need, rank == 0 ? spectators : 0);
                    ReleaseSRWLockExclusive(&g_rbs_lock);
                }
                printf("[rbnet] handshake %ld: a Battle Arena's match (arena %u, %d members, fighting seats %d and %d; this game is seat %d = player %d%s): agreeing on a rollback match with its other fighter (found in the lobby by the arena)\n",
                       (long)round, agid, players, sor[0], sor[1], seat, g_local + 1, spectators > 0 ? (spec_test ? "; TEST: the spectators are not fed" : rank == 0 ? "; waiting for every spectator's HELLO before proposing" : "; the spectators will be fed") : "");
                fflush(stdout);
            } else {
                InterlockedExchange(&g_hs_room, 1);
                printf("[rbnet] handshake %ld: a Battle Arena's match (arena %u, %d members, %d fighting; this game's seat %d): %s: the game's own online netcode plays it; no rollback, nothing changed\n",
                       (long)round, agid, players, nfight, seat, why);
                fflush(stdout);
                if (agid && spectators > 0) { g_rbs_gid = agid; rbs_native(why, rank < 0 ? seat : -1); }     /* (2026-11-02) the others need not wait for this game */
                return 0;
            }
        } }
    InterlockedExchange(&g_lay_have_peer, 0); InterlockedExchange(&g_lay_have_mine, 0);      /* (a LAYOUT of an older match is not this one's) */
    {   int nnldn_role(void), nnldn_players(void); const char *e = getenv("SSBU_RB_NET_ROOM_MATCH"), *rr = getenv("SSBU_RB_NET_ROOM_ROLLBACK");
        if (nnldn_role() && !(e && e[0] == '1')) {
            int heard = rbn_peer_heard_ms(), players = nnldn_players(), want = !(rr && rr[0] == '0');
            if (!g_setup_waiting && want && players == 2 && heard >= 0 && heard < 3000) {
                lwr = 1;
                printf("[rbnet] handshake %ld: the Local Wireless room's match (%s; 2 players; the other game's rollback session heard %d ms ago): agreeing on a rollback match\n", (long)round, nnldn_role() == 1 ? "made here" : "joined", heard);
                fflush(stdout);
            } else {
                static int told; void menu_popup(const char *title, const char *text);
                InterlockedExchange(&g_hs_room, 1);
                printf("[rbnet] handshake %ld: this fight is set up inside a Local Wireless room (%s; %s): the game's own online netcode plays it; no rollback, no handshake, nothing changed\n", (long)round, nnldn_role() == 1 ? "made here" : "joined",
                       g_setup_waiting ? "the room's waiting-stage fight" : !want ? "room rollback off (SSBU_RB_NET_ROOM_ROLLBACK=0)" : players != 2 ? "not a 1-on-1 room" : "the other player's game has rollback off (never heard)");
                fflush(stdout);
                if (!g_setup_waiting && !told++) menu_popup("Rollback netcode", "This room match uses the game's own online netcode: the other player's game has rollback off (or the room has more than 2 players). Rollback is used in a 1-on-1 room when both players have it on.");
                return 0;
            }
        } }
    /* (R3 pub1, testers 2026-10-20) through a lobby server (and on a LAN) NEITHER game is in JOIN mode, so both were "the host":
     * each waited for the other's CHOICE and sent nothing ("messages sent 0, received 0" for 120 s). The roles now come from
     * the Local Wireless room: the game that made the room is player 1 (host of the handshake), the one that joined it player 2.
     * SSBU_RB_NET_PLAYER still forces one (tests). */
    {   int nnldn_role(void); int role = nnldn_role();
        if (role && !getenv("SSBU_RB_NET_PLAYER")) { int want = role == 2; if (want != g_local) printf("[rbnet] handshake: this game %s the Local Wireless room: it is player %d (was %d by the online mode)\n", role == 1 ? "made" : "joined", want + 1, g_local + 1); g_local = want; } }
    /* (R3 net3, goal 3) a rollback match without a room code: this game is in the lobby (or LAN) mode and not in a Local Wireless
     * room, so it does not know the other player. It asks the lobby for an opponent who does the same (lobby 1.4, MATCH op 2:
     * net_relay.c net_match_find_rb); the lobby pairs two such games of the same build and lobby code and says who is player 1
     * (the one that waited longer). Then the handshake below runs as with a room code, with the rollback messages addressed to
     * that opponent. The box shows the search. SSBU_RB_NET_FIND=0: the old way (no search). */
    /* (owner 2026-10-21: "così hai tolto la possibilità alla gente di giocare offline") a Smash fight with rollback on and no room
     * code searched the lobby for an opponent: local fights against friends or CPUs were gone. Now a fight without a room code
     * (online play off, lobby or LAN mode) is a local fight, untouched, like a room fight; the online rollback match comes from
     * a Local Wireless room's match (R3 lwr) or a room code (host / join). SSBU_RB_NET_FIND=1: the lobby search (test). */
    {   const char *e = getenv("SSBU_RB_NET_FIND"); int mode = net_online() > 0 ? net_mode() : -1;
        if (!lwr /* (R3 lwr: a room match) */ && mode != 2 /* NET_MODE_HOST */ && mode != 3 /* NET_MODE_JOIN */ && !(e && e[0] == '1') && !net_arena_queued() /* (arena / room: in its queue: the match waits for the turn) */) {
            InterlockedExchange(&g_hs_room, 1);
            printf("[rbnet] handshake %ld: no room code: a local fight, played as without rollback\n", (long)round);
            fflush(stdout);
            return 0;
        } }
    int find = 0, phase = 1, ar_wait = 0;
    {   int nnldn_role(void); const char *e = getenv("SSBU_RB_NET_FIND"); int mode = net_online() > 0 ? net_mode() : -1;
        find = !nnldn_role() && (mode == 0 /* NET_MODE_LOBBY */ || mode == 1 /* NET_MODE_LAN */) && ((e && e[0] == '1') || net_arena_queued() || arena);     /* (lobby 1.6: the arena's turn searches by itself) */
        /* (room, 2026-10-30) in the room's queue but not its turn yet: the match waits here, with no time limit, and searches
         * the moment the turn comes (the loser of the match on goes to the back): the players only press start and wait */
        if (find) { phase = 0; if (arena) net_match_find_rb_key(agid); else if (net_arena_queued() && !net_arena_turn()) ar_wait = 1; else net_match_find_rb(); } }
    double limit = getenv("SSBU_RB_NET_HS_WAIT") ? atof(getenv("SSBU_RB_NET_HS_WAIT")) * 1000.0 : 120000.0;
    if (lwr) limit = getenv("SSBU_RB_NET_ROOM_WAIT") ? atof(getenv("SSBU_RB_NET_ROOM_WAIT")) * 1000.0 : 10000.0;     /* (R3 lwr) both games are at this set-up together */
    double spec_grace = getenv("SSBU_RB_NET_SPEC_GRACE") ? atof(getenv("SSBU_RB_NET_SPEC_GRACE")) * 1000.0 : 4000.0;      /* (2026-11-03) player 1: the spectators' HELLOs, see below */
    if (spec_grace > limit / 2) spec_grace = limit / 2;
    uint64_t t0 = GetTickCount64(), last_tx = 0, last_log = t0, last_note = 0, last_redraw = 0, last_here = 0;
    long long presents = nvn_front_presents(); uint64_t presents_at = t0, notes = 0, redraws = 0;
    int ok = 0, stopped = 0;
    AcquireSRWLockExclusive(&g_hs_lock);
    do g_hs_nonce = (uint32_t)hs_rand(); while (!g_hs_nonce);
    g_hs_peer_nonce = 0; g_hs_have_peer = g_hs_have_prop = 0; g_hs_accepted = g_hs_refused = 0; g_hs_why[0] = 0; g_hs_cancelled = 0; g_hs_delay = 0;
    g_hs_waiting = 1;
    ReleaseSRWLockExclusive(&g_hs_lock);
    if (find) printf("[rbnet] handshake %ld: no room code and no Local Wireless room: asking the lobby for an opponent who starts a rollback match too; own choice ui_chara %016llx colour %d (at most %.0f s; nonce %08x)\n", (long)round,
           (unsigned long long)mine->chara[0], mine->colour[0], limit / 1000.0, g_hs_nonce);
    else printf("[rbnet] handshake %ld: this game is the %s; own choice ui_chara %016llx colour %d; waiting for the other game (at most %.0f s; nonce %08x)\n", (long)round, g_local ? "joiner (player 2)" : "host (player 1)",
           (unsigned long long)mine->chara[0], mine->colour[0], limit / 1000.0, g_hs_nonce);
    fflush(stdout);
    NetMatch mm; memset(&mm, 0, sizeof mm);
    for (;;) {
        uint64_t now = GetTickCount64();
        if (g_hs_refused) break;
        if (ar_wait) {                                  /* (room) waiting for the turn */
            if (!net_arena_queued()) { snprintf(g_hs_why, sizeof g_hs_why, "you are not in the room's queue any more"); break; }
            if (net_arena_turn()) { ar_wait = 0; t0 = now; net_match_find_rb(); printf("[rbnet] handshake %ld: the room's turn came: asking the lobby for the opponent of the turn\n", (long)round); fflush(stdout); }
        }
        if (!ar_wait && (double)(now - t0) > limit) { snprintf(g_hs_why, sizeof g_hs_why, phase ? "no answer from the other player after %.0f s" : "no opponent found in %.0f s", limit / 1000.0); break; }
        if (hs_stop_pressed((double)(now - t0))) { snprintf(g_hs_why, sizeof g_hs_why, "stopped waiting (Esc / B) after %.0f s", (now - t0) / 1000.0); stopped = 1; break; }
        if (phase == 0 && !ar_wait) {                   /* (R3 net3) the lobby's opponent search */
            net_match_get(&mm);
            if (mm.kind == 2 && (mm.state == MM_MATCHED || mm.state == MM_TOGETHER) && mm.peer) {
                int want = arena ? g_local : mm.role == MM_ROLE_JOIN;      /* (an arena: the player order of its set-up, the same on both PCs) */
                AcquireSRWLockExclusive(&g_hs_lock); g_local = want; ReleaseSRWLockExclusive(&g_hs_lock);
                rtt_reset();
                printf("[rbnet] handshake %ld: the lobby found an opponent after %.1f s: %s (10.13.%u.%u); this game is the %s\n", (long)round, (now - t0) / 1000.0, mm.peer_name, mm.peer >> 8 & 255, mm.peer & 255, want ? "joiner (player 2)" : "host (player 1)");
                fflush(stdout);
                phase = 1; t0 = now; last_note = 0;
            } else if (mm.state == MM_TIMEOUT || mm.state == MM_OLD_LOBBY || mm.state == MM_PEER_LEFT || mm.state == MM_EXPIRED || mm.state == MM_OFFLINE || (mm.kind != 2 && mm.state != MM_ASKING)) {
                snprintf(g_hs_why, sizeof g_hs_why, "%s", mm.state == MM_TIMEOUT ? "no opponent found in time (nobody with your version and mods searched)" :
                         mm.state == MM_OLD_LOBBY ? "the lobby server is an older version (it cannot find rollback opponents): use a room code" :
                         mm.state == MM_OFFLINE ? "online play is off" : mm.kind != 2 ? "another search was started (F1 > Online)" : "the opponent left");
                break;
            }
        }
        AcquireSRWLockExclusive(&g_hs_lock);
        if (phase == 0) { /* searching: nothing to send yet */ }
        else if (g_local == 0) {                             /* host: proposal = our choice + the joiner's */
            if (g_rbs_need && g_rbs_bad && g_hs_have_peer && !g_hs_have_prop && !g_hs_refused) {      /* (2026-11-02) a spectator refused / differs: the game's own netcode for everyone */
                AcquireSRWLockShared(&g_rbs_lock); snprintf(g_hs_why, sizeof g_hs_why, "%s", g_rbs_bad_why); ReleaseSRWLockShared(&g_rbs_lock);
                g_hs_refused = 1; hs_refuse(g_hs_peer.nonce, g_hs_why);
            }
            /* (2026-11-03) a spectator whose game never says HELLO (it left the arena's screens or closed, but the arena still
             * lists it: owner's run, Player-8dac in the menus while seats 0 and 2 fought) no longer costs the fighters their match:
             * after SSBU_RB_NET_SPEC_GRACE s (default 4, at most half the time limit) player 1 proposes with the spectators it has;
             * the silent one is not fed (its game, if it comes, shows the match its own way). Before, player 1 waited the whole
             * 10 s, the other fighter (which hears nothing until the proposal) gave up first, and both games played the arena's
             * match with the game's own netcode: the other player's fighter could not be moved, and one player's controller
             * behind the keyboard (player 2 of that PC) did not play his fighter. */
            if (g_rbs_need && g_hs_have_peer && !g_hs_have_prop && !g_hs_refused && !g_rbs_bad && (double)(now - t0) >= spec_grace) {
                AcquireSRWLockShared(&g_rbs_lock); LONG have = rbs_popc(g_rbs_seats), need = g_rbs_need, seats = g_rbs_seats; ReleaseSRWLockShared(&g_rbs_lock);
                if (have < need) {
                    InterlockedExchange(&g_rbs_need, have);
                    printf("[arena] handshake %ld: %ld of %ld spectators said HELLO in %.1f s (seats %02lx): player 1 goes on without the others (they are not fed)\n", (long)round, (long)have, (long)need, (now - t0) / 1000.0, (unsigned long)seats);
                    fflush(stdout);
                }
            }
            if (!g_hs_have_prop && g_hs_have_peer && lwr && !g_hs_refused && rbs_popc(g_rbs_seats) >= g_rbs_need) {      /* (R3 lwr) the room's match: both games built it from the room; only the seed is new; (2026-11-02) and every spectator's HELLO is in */
                if (g_hs_peer.exe_stamp != exe_stamp()) printf("[rbnet] handshake: NOTE the other game's exe was linked at another time (stamp %08x here, %08x there): a different build may desync\n", exe_stamp(), g_hs_peer.exe_stamp);
                if (g_hs_peer.m.chara[0] != mine->chara[0] || g_hs_peer.m.rule_kind != mine->rule_kind || g_hs_peer.m.stock != mine->stock) {
                    snprintf(g_hs_why, sizeof g_hs_why, "the room's match differs on the two games (player 1 %016llx here, %016llx there; rule kind %d / %d, stocks %d / %d)", (unsigned long long)mine->chara[0], (unsigned long long)g_hs_peer.m.chara[0], mine->rule_kind, g_hs_peer.m.rule_kind, mine->stock, g_hs_peer.m.stock);
                    g_hs_refused = 1; hs_refuse(g_hs_peer.nonce, g_hs_why);
                } else {
                    RbhMatch m = *mine; m.seed = hs_new_seed();
                    m.pad0 = (uint8_t)(g_hs_peer.m.pad0 > my_delay() ? g_hs_peer.m.pad0 : my_delay());
                    hs_head(&g_hs_prop, RBH_PROPOSE); g_hs_prop.echo = g_hs_peer.nonce; g_hs_prop.m = m; g_hs_have_prop = 1;
                }
            }
            if (!g_hs_have_prop && g_hs_have_peer && !lwr) {
                if (g_hs_peer.exe_stamp != exe_stamp()) printf("[rbnet] handshake: NOTE the other game's exe was linked at another time (stamp %08x here, %08x there): a different build may desync\n", exe_stamp(), g_hs_peer.exe_stamp);
                RbhMatch m = *mine;
                m.chara[1] = g_hs_peer.m.chara[0]; m.colour[1] = g_hs_peer.m.colour[0];
                m.has_layout[1] = g_hs_peer.m.has_layout[0]; memcpy(m.layout[1], g_hs_peer.m.layout[0], sizeof m.layout[1]);
                if (m.chara[1] == m.chara[0] && m.colour[1] == m.colour[0]) m.colour[1] = (uint8_t)((m.colour[0] + 1) & 7);   /* same fighter, same colour: the joiner takes the next one */
                m.seed = hs_new_seed();
                m.pad0 = (uint8_t)(g_hs_peer.m.pad0 > my_delay() ? g_hs_peer.m.pad0 : my_delay());
                hs_stage_rule(&m, &g_hs_peer.m);
                hs_head(&g_hs_prop, RBH_PROPOSE); g_hs_prop.echo = g_hs_peer.nonce; g_hs_prop.m = m; g_hs_have_prop = 1;
            }
            if (g_hs_accepted && g_hs_have_prop) { *out = g_hs_prop.m; ok = 1; }
            else if (g_hs_have_prop && now - last_tx >= 100) { last_tx = now; hs_send(&g_hs_prop, sizeof g_hs_prop); }
        } else {                                        /* joiner: our choice until the proposal comes */
            if (g_hs_have_prop) {
                if (g_hs_prop.exe_stamp != exe_stamp()) printf("[rbnet] handshake: NOTE the other game's exe was linked at another time (stamp %08x here, %08x there): a different build may desync\n", exe_stamp(), g_hs_prop.exe_stamp);
                *out = g_hs_prop.m; ok = 1;
            } else if (now - last_tx >= 100 && net_rb_ready()) { last_tx = now; RbhMsg c; hs_head(&c, RBH_CHOICE); c.m = *mine; c.m.pad0 = (uint8_t)my_delay(); hs_send(&c, sizeof c); }
        }
        if (ok) g_hs_waiting = 0;
        ReleaseSRWLockExclusive(&g_hs_lock);
        if (ok) break;
        if (now - last_note >= 250) {                   /* the message box, and the last frame shown again while the game itself is held here */
            last_note = now; notes++;
            char t[400]; double w = (now - t0) / 1000.0;
            char line[200];
            NetArena ra; if (find) net_arena_get(&ra); else memset(&ra, 0, sizeof ra);
            if (ar_wait) {                               /* (room) what the player sees while it waits for the turn */
                char now_on[120] = "";
                if (ra.has_match) snprintf(now_on, sizeof now_on, "\nNow playing: %s vs %s.", ra.match[0].name, ra.match[1].name);
                snprintf(t, sizeof t, "%s's room: waiting for your turn (place %d in the queue).%s\nKeep this screen: your match starts by itself when it is your turn.\nPress Esc or hold B to stop waiting.",
                         ra.host[0] ? ra.host : "The", ra.place, now_on);
                menu_notice(t);
            } else {
            if (phase == 0 && ra.known && ra.status != NET_ARENA_OUT) snprintf(line, sizeof line, "Your turn in the room: starting your match against %s...", ra.opponent);
            else if (phase == 0) snprintf(line, sizeof line, !net_rb_ready() ? "Online match: connecting to the lobby..." : mm.searching > 0 ? "Online match: finding an opponent (%d of %d players of your version searching)..." : "Online match: finding an opponent...", mm.searching, mm.players);
            else if (lwr && g_rbs_need) snprintf(line, sizeof line, "Battle Arena: agreeing on a rollback match with the other fighter and the spectators (%d of %d ready)...", rbs_popc(g_rbs_seats), (int)g_rbs_need);
            else if (lwr) snprintf(line, sizeof line, "Room match: agreeing on a rollback match with the other player...");
            else if (find) snprintf(line, sizeof line, g_hs_peer_nonce ? "Online match with %s: agreeing on the match..." : "Online match: opponent found (%s), waiting for their game...", mm.peer_name);
            else snprintf(line, sizeof line, "%s", !net_rb_ready() ? "Not connected to the other player's game yet (lobby / room not joined)." : net_rb_no_destination() && !g_hs_peer_nonce ? "The other player is not in your Local Wireless room: both players must be in the same room." : g_hs_peer_nonce ? "The other player is here: agreeing on the match..." : "Waiting for the other player to choose a fighter and start...");
            snprintf(t, sizeof t, "%s\nWaiting %.0f s (this game gives up after %.0f s and goes back to the character select).\nPress Esc or hold B to stop waiting.", line, w, limit / 1000.0);
            menu_notice(t);
            }
        }
        {   long long p = nvn_front_presents();
            if (p != presents) { presents = p; presents_at = now; }
            else if (now - presents_at > 100 && now - last_redraw >= 100) { last_redraw = now; redraws++; nvn_front_osk_redraw(); } }
        if (g_rbs_gid && now - last_here >= 100) { last_here = now; RbsHead h; memset(&h, 0, sizeof h); rbs_head(&h, RBS_HERE); rbs_send(&h, sizeof h); }     /* (2026-11-02) in the arena's group */
        if (g_rbs_need && now - last_log >= 5000) printf("[arena] handshake %ld: player 1 waits for the spectators: %d of %d HELLO received (seats %02lx)\n", (long)round, rbs_popc(g_rbs_seats), (int)g_rbs_need, (unsigned long)g_rbs_seats);
        if (now - last_log >= 5000) { last_log = now; printf("[rbnet] handshake %ld: still waiting (%s%.0f s; lobby %s; other game %s; messages sent %lld, received %lld; notice updates %llu, frames shown again %llu)\n", (long)round, phase ? "" : "finding an opponent, ", (now - t0) / 1000.0,
                                                          net_rb_ready() ? (net_rb_no_destination() ? "joined, no other player in the room" : "joined") : "NOT joined", g_hs_peer_nonce ? "seen" : "not seen", g_hs_tx, g_hs_rx, (unsigned long long)notes, (unsigned long long)redraws); fflush(stdout); }
        Sleep(5);
    }
    if (!ok) {
        AcquireSRWLockExclusive(&g_hs_lock);
        int told = g_hs_refused == 2; uint32_t echo = g_hs_peer_nonce;
        g_hs_waiting = 0; g_hs_cancelled = lwr ? 0 : 1;
        if (lwr) InterlockedExchange(&g_hs_room, 1);          /* (R3 lwr) nothing was changed: the game's own netcode plays the room's match */
        ReleaseSRWLockExclusive(&g_hs_lock);
        if (find) { net_match_get(&mm); if (mm.kind == 2 && (mm.state == MM_ASKING || mm.state == MM_SEARCHING || mm.state == MM_MATCHED)) net_match_cancel(); }   /* (R3 net3) the lobby and the opponent are told */
        if (g_rbs_need && g_hs_have_peer && !g_hs_have_prop && !g_hs_refused && !stopped)      /* (2026-11-02) the time ran out on the spectators' HELLOs */
            snprintf(g_hs_why, sizeof g_hs_why, "not every spectator's game was ready in time (%d of %d HELLO)", rbs_popc(g_rbs_seats), (int)g_rbs_need);
        if (!told) { char w[160]; snprintf(w, sizeof w, "the other player %s", stopped ? "stopped waiting" : "gave up waiting (time limit)"); for (int k = 0; k < 3; k++) { hs_refuse(echo, w); Sleep(20); } }
        InterlockedExchange(&g_rbs_need, 0);
        if (g_rbs_gid) rbs_native(g_hs_why, -1);         /* (2026-11-02) the spectators: the game's own netcode shows them this match */
        if (lwr) {                                       /* (R3 lwr) */
            menu_notice(NULL);
            printf("[rbnet] handshake %ld FAILED in the %s: %s. The %s match is played by the game's own online netcode, as set up (no rollback)\n", (long)round, arena ? "Battle Arena" : "Local Wireless room", g_hs_why, arena ? "arena's" : "room's");
            fflush(stdout); return 0;
        }
        char t[400]; snprintf(t, sizeof t, "No online match: %s.\nThis match is left at once: back to the character select.", g_hs_why); menu_notice(t);
        printf("[rbnet] handshake %ld FAILED: %s. This match is played on this PC only (Final Destination, one stock) and left at once (player 1 walks off): back to the character select (notice updates %llu, frames shown again %llu)\n",
               (long)round, g_hs_why, (unsigned long long)notes, (unsigned long long)redraws);
        fflush(stdout); return 0;
    }
    menu_notice(NULL);
    g_hs_lwr = lwr;
    if (g_rbs_gid) {                                    /* (2026-11-02) an arena with spectators: they are fed (MATCH from player 1's player-2 record, PADS every pass) */
        InterlockedExchange(&g_rbs_need, 0); g_rbs_agreed = *out; rbs_hist_reset(); InterlockedExchange(&g_rbs_fighter, 1);      /* (the history of an older match never answers a NACK of this one) */
        printf("[arena] handshake %ld: agreed with the other fighter%s: this game sends its final pads to the arena's group every pass\n", (long)round, g_local == 0 ? " and every spectator (HELLO)" : "");
    }
    g_hs_seed = out->seed; g_match_id = (uint32_t)(out->seed ^ (out->seed >> 32)) | 1u;
    InterlockedExchange(&g_hs_agreed, 1);
    g_hs_delay = out->pad0;
    printf("[rbnet] handshake %ld: input delay %d frames for this match (%s; here %d, 0 = auto)\n", (long)round, g_hs_delay,
           g_hs_delay ? "the larger of the two players' settings" : "both on auto: from the round trip", my_delay());
    host_fixed_random_reseed(g_hs_seed);
    { void fh_new_fight(void); fh_new_fight(); }     /* (fighter_host.c) this match's first fighter sets the generators from its seed */
    printf("[rbnet] handshake %ld agreed after %.2f s (messages sent %lld, received %lld; match id %08x; notice updates %llu, frames shown again %llu)\n", (long)round, (GetTickCount64() - t0) / 1000.0, g_hs_tx, g_hs_rx, g_match_id,
           (unsigned long long)notes, (unsigned long long)redraws);
    hs_print("the match", out);
    return 1;
}
/* rbn_start: the game's match block of this game, sent to the other in chunks (a check of the agreed start) */
static void hs_block_send(void) {
    if (!g_hs_mine_ok) return;
    for (int off = 0; off < RBH_BLK; off += 768) {
        RbhBlock b; memset(&b, 0, sizeof b); b.type = RBH_BLOCK; b.proto = RBH_PROTO; b.game_build = (int16_t)g_game_build; b.exe_stamp = exe_stamp(); b.nonce = g_match_id;
        b.off = (uint16_t)off; b.len = (uint16_t)(RBH_BLK - off < 768 ? RBH_BLK - off : 768); b.total = RBH_BLK; memcpy(b.data, g_hs_mine_blk + off, b.len);
        hs_send(&b, (int)(offsetof(RbhBlock, data) + b.len));
    }
}
static void hs_block_capture(void) {
    SIZE_T got = 0; uint64_t a = ga(0x71052c41b0ULL);            /* the game's global match block (debug_mode.c MELEE_INFO) */
    g_hs_mine_ok = ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)a, g_hs_mine_blk, RBH_BLK, &got) && got == RBH_BLK;
}
static void hs_block_compare(void) {                    /* main thread (rbn_drain): once both copies are complete */
    if (g_hs_blk_done || !g_hs_mine_ok) return;
    for (int k = 0; k * 768 < RBH_BLK; k++) if (!g_hs_peer_have[k]) return;
    g_hs_blk_done = 1;
    char line[900]; int n = 0, ranges = 0, bytes = 0;
    AcquireSRWLockExclusive(&g_hs_lock);
    for (int i = 0; i < RBH_BLK; i++) {
        if (g_hs_mine_blk[i] == g_hs_peer_blk[i]) continue;
        int j = i; while (j + 1 < RBH_BLK && g_hs_mine_blk[j + 1] != g_hs_peer_blk[j + 1]) j++;
        bytes += j - i + 1;
        if (ranges++ < 24 && n < (int)sizeof line - 80) n += snprintf(line + n, sizeof line - n, " +%03x..%03x (here %02x there %02x)", i, j, g_hs_mine_blk[i], g_hs_peer_blk[i]);
        i = j;
    }
    ReleaseSRWLockExclusive(&g_hs_lock);
    if (!ranges) printf("[rbnet] match block check: IDENTICAL on both games (%d bytes: rules + the two player records)\n", RBH_BLK);
    else printf("[rbnet] match block check: %d ranges (%d bytes) differ (offsets from the match block; players at +0x580 + 0x260 * i):%s\n", ranges, bytes, line);
    fflush(stdout);
}

/* ---------------------------------------------------------------- receive queue (net thread -> main) */
#define RQ_N 1024
#define RQ_MAX 1100
static struct { uint64_t at_us; long long at_frame; int n, grp; uint8_t b[RQ_MAX]; } *g_rq;      /* (2026-11-02) grp 1: a fighter's PADS from the arena's group (a spectator's) */
static volatile LONG g_rq_head, g_rq_tail; static SRWLOCK g_rq_lock = SRWLOCK_INIT;
static volatile LONG64 g_rx_pkts, g_rx_bad, g_rx_drop, g_tx_pkts, g_tx_fail, g_rx_last_ms, g_rx_oldmatch;
static volatile LONG g_rtt_us = -1, g_rtt_min_us = -1;
static long long g_rtt_n; static double g_rtt_sum;      /* (under g_rq_lock) */
#define TSQ 64
static int32_t g_tsq[TSQ]; static int g_tsq_n;          /* clock-offset samples waiting for the main thread (under g_rq_lock) */
/* (hs14) the mode byte at the end of every PADS message (after the match tag): bit 7 = this game plays the match WITHOUT the
 * ring (delay-based, r1_ring.c g_rg_nomem), bits 0..6 = the delay that covers the one-way trip as this game measured it at
 * its match start (frozen then, so both games choose from the same two numbers). The start barrier ends only with a PADS
 * message of the other game, so both know both bytes before frame 1. When either game has no ring, BOTH use the larger of
 * the two delays (SSBU_RB_NET_PEERDELAY=0: the old way, the game with the ring keeps its own rollback delay and the one
 * without it stalls almost every frame: pm1j 1,913 stalls in 2,400 frames). */
static int g_lockstep, g_my_lockd, g_peerdelay = -1;
static volatile LONG g_peer_mode = -1;                 /* the other game's mode byte of this match (-1 = none seen yet) */
static uint8_t my_mode(void) { return (uint8_t)((g_lockstep ? 0x80 : 0) | (g_my_lockd & 0x7f)); }
static void send_msg(const RbMsg *m) {
    uint8_t b[RB_PADS_HEAD + RB_MAX_PADS_PER_MSG * RB_PAD_WIRE + 2];
    size_t n = rb_msg_build(b, sizeof b - 2, m);
    if (!n || g_loop) return;
    if (m->type == RB_MSG_PADS) { b[n++] = pads_tag(); b[n++] = my_mode(); }      /* (hs12) the match these pads belong to: pads of another match are dropped; (hs14) this game's mode byte */
    if (net_rb_send(b, (int)n)) InterlockedIncrement64(&g_tx_pkts); else InterlockedIncrement64(&g_tx_fail);
}
/* net_relay.c's receive thread: one rollback message from the other player */
void rbn_rx(const uint8_t *p, int n) {
    if (rbn_on() > 0 && n > 0 && hs_rx(p, n)) return;  /* the match handshake */
    if (rbn_on() <= 0 || n <= 0 || n > RQ_MAX) { InterlockedIncrement64(&g_rx_bad); return; }
    if (p[0] == RB_MSG_PADS && n >= RB_PADS_HEAD + 2 && (n - RB_PADS_HEAD) % RB_PAD_WIRE == 2) {     /* (hs12) the match tag, (hs14) then the mode byte, at the end */
        if (!g_started || p[n - 2] != pads_tag()) { InterlockedIncrement64(&g_rx_oldmatch); return; }      /* (between matches: the other game sends them again) */
        InterlockedExchange(&g_peer_mode, p[n - 1]);    /* (before it is queued: rbn_drain sees it with the message that starts the match) */
        n -= 2;
    }
    InterlockedIncrement64(&g_rx_pkts); InterlockedExchange64(&g_rx_last_ms, (LONG64)GetTickCount64());
    /* (2026-11-03) a ping or pong that arrives twice (straight from the other player, and 1 in 30 also through the lobby:
     * net_relay.c net_rb_send) counts once, the first copy: the late copy through the lobby was answered and measured again,
     * and a Battle Arena match at 9 ms showed a 50 ms round trip (the input delay and the time sync come from it) */
    static uint64_t seen_ping[16], seen_pong[16]; static unsigned seen_ping_i, seen_pong_i;
    if (p[0] == RB_MSG_PING || p[0] == RB_MSG_PONG) {
        RbMsg d; if (rb_msg_parse(p, (size_t)n, &d)) {
            uint64_t key = p[0] == RB_MSG_PING ? (uint64_t)d.time_us : (uint64_t)d.echo_us;
            uint64_t *seen = p[0] == RB_MSG_PING ? seen_ping : seen_pong; unsigned *at = p[0] == RB_MSG_PING ? &seen_ping_i : &seen_pong_i;
            for (int k = 0; k < 16; k++) if (seen[k] == key) return;
            seen[(*at)++ % 16] = key;
        }
    }
    if (p[0] == RB_MSG_PING) {                          /* answered at once on this thread (the round trip excludes the frame wait) */
        RbMsg m, r; if (!rb_msg_parse(p, (size_t)n, &m)) { InterlockedIncrement64(&g_rx_bad); return; }
        memset(&r, 0, sizeof r); r.type = RB_MSG_PONG; r.frame = (int32_t)g_cur; r.echo_us = m.time_us;
        {   uint64_t t = now_us(), t0 = (uint64_t)g_cur_t_us; uint64_t into = t0 && t >= t0 ? t - t0 : 0;
            r.time_us = PONG_PHASE_MARK | (into < 1000000 ? into : 999999); }      /* (the time into this game's frame) */
        send_msg(&r); return;
    }
    if (p[0] == RB_MSG_PONG) {                          /* the round trip at once (also before the match: the delay is chosen from it) */
        RbMsg m; if (!rb_msg_parse(p, (size_t)n, &m)) { InterlockedIncrement64(&g_rx_bad); return; }
        long rtt = (long)(now_us() - m.echo_us);
        if (rtt <= 0 || rtt >= 5000000) return;
        InterlockedExchange(&g_rtt_us, rtt); if (g_rtt_min_us < 0 || rtt < g_rtt_min_us) InterlockedExchange(&g_rtt_min_us, rtt);
        AcquireSRWLockExclusive(&g_rq_lock);
        g_rtt_sum += rtt; g_rtt_n++;
        if (g_started && m.frame > 0 && g_cur > 0 && g_tsq_n < TSQ) {
            long long off = (g_cur - m.frame) * 16683 - rtt / 2;                  /* > 0: this game runs ahead */
            uint64_t t0 = (uint64_t)g_cur_t_us, t = now_us();
            if ((m.time_us & PONG_PHASE_MARK) && t0 && t >= t0) {                /* both games' time into their frames (an older game: whole frames only) */
                long long mine = (long long)(t - t0), theirs = (long long)(m.time_us & ~PONG_PHASE_MARK);
                if (mine < 4 * 16683 && theirs < 4 * 16683) off += mine - theirs;
            }
            g_tsq[g_tsq_n++] = (int32_t)off;
        }
        ReleaseSRWLockExclusive(&g_rq_lock);
        return;
    }
    AcquireSRWLockExclusive(&g_rq_lock);
    if (!g_rq) g_rq = calloc(RQ_N, sizeof *g_rq);
    LONG next = (g_rq_head + 1) % RQ_N;
    if (g_rq && next != g_rq_tail) { g_rq[g_rq_head].at_us = now_us(); g_rq[g_rq_head].at_frame = g_cur; g_rq[g_rq_head].n = n; g_rq[g_rq_head].grp = 0; memcpy(g_rq[g_rq_head].b, p, (size_t)n); g_rq_head = next; }
    else InterlockedIncrement64(&g_rx_drop);
    ReleaseSRWLockExclusive(&g_rq_lock);
}
static int rbn_peer_heard_ms(void) {                    /* (R3 lwr) any rollback message of the other game (its pings come every 100 ms) */
    LONG64 last = g_rx_last_ms;
    return last ? (int)(GetTickCount64() - (ULONGLONG)last) : -1;
}
static void rtt_reset(void) {                           /* (R3 net3) */
    AcquireSRWLockExclusive(&g_rq_lock); g_rtt_sum = 0; g_rtt_n = 0; ReleaseSRWLockExclusive(&g_rq_lock);
    InterlockedExchange(&g_rtt_us, -1); InterlockedExchange(&g_rtt_min_us, -1);
}
/* a thread that pings the other player every 100 ms from the start (the delay is chosen from it when the match begins) */
static DWORD WINAPI ping_thread(void *u) {
    (void)u;
    for (;;) {
        Sleep(100);
        if (!net_rb_ready()) continue;
        if (g_rbs_match_tx) {                           /* (2026-11-02) player 1: MATCH to the arena's spectators until its fight starts */
            if (g_started || GetTickCount64() > g_rbs_match_until) InterlockedExchange(&g_rbs_match_tx, 0);
            else { RbsMatch mm; AcquireSRWLockShared(&g_rbs_lock); mm = g_rbs_match; ReleaseSRWLockShared(&g_rbs_lock); rbs_send(&mm, sizeof mm); }
        }
        if (g_spec) continue;                           /* (2026-11-02) a spectator: silent toward the fighters (no ping, no time sync) */
        RbMsg m; memset(&m, 0, sizeof m); m.type = RB_MSG_PING; m.frame = (int32_t)g_cur; m.time_us = now_us();
        send_msg(&m);
    }
    return 0;
}
void rbn_init(void) {                                   /* r1_state.c ra_init: before the match */
    static int done; if (done++ || rbn_on() <= 0) return;
    if (!g_loop) CloseHandle(CreateThread(NULL, 0, ping_thread, NULL, 0, NULL));
}

/* (2026-11-02) a Battle Arena's spectators, the in-match part (at the end of this file) */
static void rbs_hist_reset(void);
static void rbs_fighter_pads(void);
static long long spec_confirmed(void);
static void spec_rx_pads(const RbMsg *m);
static void spec_nack(int force);
static void spec_final(long long f);
static void spec_start(void);
static void spec_report(FILE *o, long long frames);
static double spec_pace(long long f);
/* ---------------------------------------------------------------- the session */
static long long g_f0;                                  /* the base frame (the ring's first capture) */
static int g_delay, g_remote = 1;
static long long g_peer_ack, g_sent_newest;
static long long g_stat_pred, g_stat_padget, g_stat_mis;
static long long g_ts_n; static double g_ts_extra;     /* time sync: extra ms per frame for the next frames */
static int g_ts_left;
static uint64_t g_hacc = 0xcbf29ce484222325ull; static int g_hlines;
#define HR 512
static uint64_t g_hash[HR]; static long long g_hframe[HR], g_hfinal_upto; static long long g_desync, g_check_ok, g_desync_first = -1;
/* (2026-11-03) the newest frame whose checksum is SETTLED: final (both players' pads known) AND older than any rollback can
 * reach (10 frames behind the frame this game is on; the ring holds 8). A frame is final the moment the other player's pad
 * arrives, but its re-run with that pad comes at the next pass: sent or compared then, the checksum was the one of the guess,
 * and a fight identical on both games (the sampled frames' values the same) reported "DESYNC" on 97 % of its frames. */
#define PCQ 64                                          /* (2026-11-03) the other game's checksums waiting until their frame is settled here */
static long long g_pcq_f[PCQ]; static uint32_t g_pcq_v[PCQ]; static unsigned g_pcq_i;      /* (one slot, always overwritten by a newer one: "compared 0") */
static long long settled_upto(void) { long long c = (long long)g_cur - 10; return g_hfinal_upto < c ? g_hfinal_upto : c; }
static long long g_peer_check_f; static uint32_t g_peer_check;

/* (hs13) g_lockstep: a match without the rollback ring: every frame waits for the other pad */
void rbn_set_lockstep(int on) { g_lockstep = on; }
int rbn_loop(void) { return g_loop; }
static int lock_delay(void) {                           /* the delay that covers the whole one-way trip (+ 1 frame): the wait is short or none */
    if (g_loop) return g_loop_lat + 1;
    int rt = g_rtt_min_us > 0 ? g_rtt_min_us : g_rtt_us;
    if (rt <= 0) return 4;
    int d = (int)ceil(rt / 2000.0 / (1000.0 / 60.0)) + 1;
    return d < 1 ? 1 : d > 12 ? 12 : d;
}
static int peer_lockstep(void) { return g_peer_mode >= 0 && (g_peer_mode & 0x80) && g_peerdelay; }
static int auto_delay(void) {
    if (g_hs_delay > 0) return g_hs_delay;              /* the delay both games agreed on in the handshake */
    if (g_delay_cfg >= 0) return g_delay_cfg;
    if (g_lockstep && (!g_peerdelay || g_loop)) return lock_delay();     /* (the old way; the loop test) */
    if (g_lockstep || peer_lockstep()) {                /* (hs14) either game has no ring: both take the larger of the two announced delays */
        int d = g_my_lockd, pd = g_peer_mode >= 0 ? (g_peer_mode & 0x7f) : 0;
        return pd > d ? pd : d;
    }
    if (g_loop) return 2;
    int rtt = g_rtt_min_us > 0 ? g_rtt_min_us : g_rtt_us;
    if (rtt <= 0) return 2;
    double one_way = rtt / 2000.0 / (1000.0 / 60.0);     /* frames */
    int d = (int)(one_way - 2.0 + 0.5);                   /* up to 2 frames of lateness covered by rollback */
    return d < 1 ? 1 : d > 4 ? 4 : d;
}
void rbn_start(long long f0) {
    g_f0 = f0; g_started = 1; g_match_n++; g_last_tag = pads_tag();
    if (g_hlog) { fprintf(g_hlog, "m %d id %08x\n", g_match_n, g_match_id); fflush(g_hlog); }      /* (rbn_cmp.py: frames are per match) */
    g_remote = g_spec ? 0 : 1 - g_local;              /* (2026-11-02) a spectator: both players remote (g_local -1) */
    if (g_peerdelay < 0) { const char *e = getenv("SSBU_RB_NET_PEERDELAY"); g_peerdelay = !(e && e[0] == '0'); }
    g_my_lockd = lock_delay();                          /* (hs14) announced in every PADS message of this match (frozen here) */
    g_delay = auto_delay();
    if (g_delay < 1) g_delay = 1;                        /* (frames 1..delay are empty on both sides: no rollback can reach the base state, made before the start barrier) */
    rb_input_reset(2, g_local, g_delay);
    rb_ts_reset(16683);
    if (!g_loop && rbn_handshake_wanted() && !g_spec) { hs_block_capture(); hs_block_send(); }     /* the match block check (not a spectator's: it writes to no fighter) */
    InterlockedExchange(&g_rbs_match_tx, 0); if (g_rbs_fighter) rbs_hist_reset();      /* (2026-11-02) a fighter with spectators: its pad history from frame 1 */
    if (g_spec) spec_start();
    printf("[rbnet] match %d (id %08x) start at frame %lld: local player %d, input delay %d frames (round trip %.1f ms, best %.1f ms)\n", g_match_n, g_match_id, f0, g_local + 1, g_delay,
           g_rtt_us > 0 ? g_rtt_us / 1000.0 : -1.0, g_rtt_min_us > 0 ? g_rtt_min_us / 1000.0 : -1.0);
    fflush(stdout);
}
/* after the start barrier: the delay again from the round trip measured meanwhile (no frame has run: the queues start over;
 * pads of the other game already queued are sent again by it, from our acknowledgement on) */
void rbn_choose_delay(void) {
    if (g_spec) { printf("[rbnet] spectator: after the start barrier: both fighters' pads are coming (players 1 and 2 heard); no delay of its own (it sends no pads)\n"); fflush(stdout); return; }
    int d = auto_delay(); if (d < 1) d = 1;
    if (g_lockstep && !g_peerdelay) printf("[rbnet] after the start barrier: this game has no snapshot ring (old way, SSBU_RB_NET_PEERDELAY=0): the other game keeps its own delay\n");
    else if (!g_loop && (g_lockstep || peer_lockstep())) printf("[rbnet] after the start barrier: %s snapshot ring: delay-based pacing, both games use input delay %d (announced here %d, there %d; mode byte here %02x, there %02x)\n",
        g_lockstep && peer_lockstep() ? "neither game has a" : g_lockstep ? "this game has no" : "the other game has no", d, g_my_lockd, g_peer_mode >= 0 ? (int)(g_peer_mode & 0x7f) : -1, my_mode(), (unsigned)(g_peer_mode & 0xff));
    else if (!g_loop) printf("[rbnet] after the start barrier: both games have the ring (mode byte here %02x, there %02x)\n", my_mode(), (unsigned)(g_peer_mode & 0xff));
    fflush(stdout);
    if (d == g_delay) return;
    printf("[rbnet] after the start barrier: input delay %d -> %d frames (round trip %.1f ms, best %.1f ms)\n", g_delay, d, g_rtt_us > 0 ? g_rtt_us / 1000.0 : -1.0, g_rtt_min_us > 0 ? g_rtt_min_us / 1000.0 : -1.0);
    fflush(stdout);
    g_delay = d; rb_input_reset(2, g_local, g_delay); if (g_rbs_fighter) rbs_hist_reset();
}
int rbn_started(void) { return g_started; }
int rbn_delay(void) { return g_started ? g_delay : 0; }
static void send_pads(void) {
    long long newest = rb_input_local_newest_frame();
    if (newest < 1 || g_loop) return;
    RbMsg m; memset(&m, 0, sizeof m);
    m.type = RB_MSG_PADS; m.player = g_local; m.newest = (int32_t)newest; m.ack = rb_input_remote_newest_consecutive_frame(g_remote);
    long long from = g_peer_ack + 1; if (newest - from + 1 > RB_MAX_PADS_PER_MSG) from = newest - RB_MAX_PADS_PER_MSG + 1; if (from < 1) from = 1;
    for (long long f = newest; f >= from; f--) { RbPad p; if (!rb_input_local_get((int)f, &p)) break; m.pads[m.count++] = p; }
    long long cf = settled_upto();
    if (cf > 0 && g_hframe[cf % HR] == cf) { m.check_frame = (int32_t)cf; m.check = (uint32_t)(g_hash[cf % HR] ^ (g_hash[cf % HR] >> 32)); }
    send_msg(&m); g_sent_newest = newest;
    rbs_fighter_pads();                                 /* (2026-11-02) a fighter of an arena with spectators: the same final pads to the group */
}
/* the local player's pad of a real pass (frame f): kept for f + delay and sent */
void rbn_local_pad(long long f, RbPad now) {
    if (g_spec) return;                                 /* (2026-11-02) a spectator has no player of its own */
    if (g_bot) now = bot_pad(g_bot_seed, f);
    rb_input_local((int)f, now);
    send_pads();
}
/* (hs10) SSBU_RB_NET_DETAIL=1: the pads each pass gave the game and the fighters' frame lines of the last simulation of each
 * frame, written as "d" lines next to the "f" lines of SSBU_RB_NET_LOG. Always: a local check at the finalization (every read
 * of the final simulation of a frame must have got the final, received pad of both players). */
static int g_detail = -1;
static int detail_on(void) { if (g_detail < 0) { const char *e = getenv("SSBU_RB_NET_DETAIL"); g_detail = e && atoi(e) > 0; } return g_detail; }
static RbPad g_acc_pad[2]; static int g_acc_n[2], g_acc_var[2];          /* reads in the pass now running */
#define HR2 512
static RbPad g_upad[HR2][2]; static int g_un[HR2][2], g_uvar[HR2][2];   /* reads of the last simulation of each frame */
static uint8_t g_ulines[HR2][4][40]; static int g_unl[HR2];
static long long g_padcheck_bad, g_padcheck_first = -1;
RbPad rbn_pad(int player, long long f) {
    int pred = 0; RbPad p = rb_input_get(player, (int)f, &pred);
    g_stat_padget++; g_stat_pred += pred;
    if (player >= 0 && player < 2) { if (g_acc_n[player] && !rb_pad_equal(&g_acc_pad[player], &p)) g_acc_var[player] = 1; g_acc_pad[player] = p; g_acc_n[player]++; }
    return p;
}
static void on_check(long long cf, uint32_t ck) {
    if (cf > settled_upto()) { g_pcq_f[g_pcq_i % PCQ] = cf; g_pcq_v[g_pcq_i % PCQ] = ck; g_pcq_i++; return; }      /* (compared at rbn_finalize) */
    if (cf <= 0 || g_hframe[cf % HR] != cf) return;      /* not final here yet: kept for later */
    uint32_t mine = (uint32_t)(g_hash[cf % HR] ^ (g_hash[cf % HR] >> 32));
    if (mine == ck) g_check_ok++;
    else { g_desync++; if (g_desync_first < 0) { g_desync_first = cf; printf("[rbnet] DESYNC: frame %lld checksum here %08x, the other game %08x\n", cf, mine, ck); fflush(stdout);
            /* (2026-11-03) what each fighter had in that frame on THIS game (the other game's log has its own): position, damage,
             * status, motion, motion frame, the script generators' hash; compared by hand, the first value that differs is the cause */
            {   int x = (int)(cf % HR2);
                for (int q = 0; q < g_unl[x] && q < 4; q++) { struct { int k, st; uint64_t mot; float mf, fx, fy, dmg; uint64_t rh; } r3; memcpy(&r3, g_ulines[x][q], sizeof r3);
                    printf("[rbnet] DESYNC frame %lld, fighter line %d here: entry %d status %x motion %llx motion frame %.3f x %.4f y %.4f damage %.2f generators %016llx\n",
                           cf, q, r3.k, r3.st, (unsigned long long)r3.mot, (double)r3.mf, (double)r3.fx, (double)r3.fy, (double)r3.dmg, (unsigned long long)r3.rh); }
                fflush(stdout); }
            /* (2026-11-03, owner: "you should send an advice when that happens") the players are told at once */
            void menu_popup(const char *title, const char *text);
            menu_popup("Desync", "The two games no longer show the same fight (desync detected).\nWhat you see is not what the other player sees: finish or leave this fight.\nThe log has the details (it is sent to the group's server when the game closes).");
        } }
}
/* (hs15, test) SSBU_RB_NET_LOOP_BOT2=<seed>: the loop test's other player is the bot of that seed (11 = the joiner of a pair) */
static unsigned loop_bot2(void) {
    static int on = -1; static unsigned seed;
    if (on < 0) { const char *e = getenv("SSBU_RB_NET_LOOP_BOT2"); on = e && *e; if (on) seed = (unsigned)strtoul(e, NULL, 0); }
    return on ? seed : g_bot_seed ^ 0x5bd1e995u;
}
static RbPad loop_remote_pad(long long f) {             /* with SSBU_RB_NET_LOOP_BOT2: exactly what that bot sends as a pair's other game (its input delay applied) */
    if (!getenv("SSBU_RB_NET_LOOP_BOT2")) return bot_pad(loop_bot2(), f);
    RbPad z; memset(&z, 0, sizeof z);
    return f <= g_delay ? z : rb_pad_rest(bot_pad(loop_bot2(), f - g_delay));
}
/* main thread, at the safe point: the messages received since the last call */
void rbn_drain(long long cur) {
    if (cur != g_cur) InterlockedExchange64(&g_cur_t_us, (LONG64)now_us());
    InterlockedExchange64(&g_cur, cur);
    if (g_loop && g_started) {                          /* loop test: the bot's pad of frame f arrives at frame f + delay - lat */
        static long long lat2_from = -2; static int lat2;          /* (hs15, test) SSBU_RB_NET_LOOP_LAT2=<frame>,<lat>: the lateness changes at that frame */
        if (lat2_from == -2) { const char *e = getenv("SSBU_RB_NET_LOOP_LAT2"); lat2_from = -1; if (e && *e) sscanf(e, "%lld,%d", &lat2_from, &lat2); }
        static int jit = -2; static unsigned jseed;                /* (hs15, test) SSBU_RB_NET_LOOP_JIT=<seed>: lateness g_loop_lat or one less, changing every 4..20 frames */
        if (jit == -2) { const char *e = getenv("SSBU_RB_NET_LOOP_JIT"); jit = e && *e; if (jit) jseed = (unsigned)strtoul(e, NULL, 0); }
        int lat = lat2_from >= 0 && cur >= lat2_from ? lat2 : g_loop_lat;
        if (jit) { uint32_t h = mix32(jseed * 0x9E3779B9u ^ (uint32_t)(cur / (4 + (mix32(jseed) % 17))) * 0x85EBCA6Bu); lat -= (int)(h & 1); }
        long long upto = cur + g_delay - lat;
        for (long long f = rb_input_remote_newest_consecutive_frame(g_remote) + 1; f <= upto; f++) rb_input_remote_put(g_remote, (int)f, loop_remote_pad(f));
        return;
    }
    if (rbn_handshake_wanted()) hs_block_compare();
    {   int32_t ts[TSQ]; int nts;
        AcquireSRWLockExclusive(&g_rq_lock); nts = g_tsq_n; memcpy(ts, g_tsq, sizeof(int32_t) * (size_t)nts); g_tsq_n = 0; ReleaseSRWLockExclusive(&g_rq_lock);
        for (int k = 0; k < nts; k++) rb_ts_sample(ts[k]);
    }
    for (;;) {
        static uint8_t b[RQ_MAX]; int n = 0, grp = 0; uint64_t at = 0; long long atf = 0;
        AcquireSRWLockExclusive(&g_rq_lock);
        if (g_rq && g_rq_tail != g_rq_head) { n = g_rq[g_rq_tail].n; grp = g_rq[g_rq_tail].grp; at = g_rq[g_rq_tail].at_us; atf = g_rq[g_rq_tail].at_frame; memcpy(b, g_rq[g_rq_tail].b, (size_t)n); g_rq_tail = (g_rq_tail + 1) % RQ_N; }
        ReleaseSRWLockExclusive(&g_rq_lock);
        if (!n) break;
        RbMsg m;
        if (!rb_msg_parse(b, (size_t)n, &m)) { g_rx_bad++; continue; }
        if (g_spec) { if (grp && m.type == RB_MSG_PADS && g_started) spec_rx_pads(&m); continue; }     /* (2026-11-02) a spectator: only the fighters' group PADS */
        if (m.type != RB_MSG_PADS || !g_started || m.player != g_remote) continue;
        g_peer_started = 1;
        for (int k = 0; k < m.count; k++) rb_input_remote_put(g_remote, m.newest - k, m.pads[k]);
        if (m.ack > g_peer_ack && m.ack <= rb_input_local_newest_frame()) g_peer_ack = m.ack;      /* (hs12: never an acknowledgement of pads not sent yet) */
        if (m.check_frame > 0) on_check(m.check_frame, m.check);
    }
    if (g_spec && g_started) spec_nack(0);              /* (2026-11-02) a gap in a fighter's pads: asked for again */
}
int rbn_take_mismatch(void) { int m = rb_input_take_mismatch(); if (m) g_stat_mis++; return m; }
void rbn_put_mismatch(int m) { rb_input_put_mismatch(m); if (m) g_stat_mis--; }      /* (R3 pub1) r1_ring_load refused: the wrong guess stays pending */
long long rbn_confirmed(void) { return g_spec ? spec_confirmed() : rb_input_remote_newest_consecutive_frame(g_remote); }
int rbn_peer_gone(void) {
    if (g_loop || !g_started || g_spec) return 0;      /* (2026-11-02: a spectator never goes on alone: rbn_spec_wait) */
    LONG64 last = g_rx_last_ms;
    return last && GetTickCount64() - (ULONGLONG)last > 5000;
}

/* ---------------------------------------------------------------- checksums: fighter_host.c gives each fighter's frame line */
static uint8_t g_lacc[4][40]; static int g_lacc_n;
void rbn_hash_add(const void *p, size_t n) {
    if (g_on <= 0) return;
    if (g_lacc_n < 4 && n <= 40) { memset(g_lacc[g_lacc_n], 0, 40); memcpy(g_lacc[g_lacc_n], p, n); g_lacc_n++; }
    const uint8_t *b = p; while (n--) { g_hacc ^= *b++; g_hacc *= 0x100000001b3ull; }
    g_hlines++;
}
/* the safe point after pass f: the lines since the last call belong to frame f (a re-run pass overwrites the value) */
/* (hs15, pp4: a frame simulated with a wrong pad can leave the fighters' frame lines unchanged, e.g. a button during an
 * animation that ignores it) the frame's checksum also folds the pads the pass gave the game (the last read of each
 * player, whether the reads varied) and the extra fighter fields of rbn_hash_extra: a frame whose final simulation got a
 * wrong pad now differs from the other game's. SSBU_RB_NET_CKPADS=0 = the old checksum (test). */
static int g_ckpads = -1;
int rbn_hash_wanted(void) { return g_on > 0; }
void rbn_hash_extra(const void *p, size_t n) {          /* folded into the frame's checksum only (not a "d" line field) */
    if (g_on <= 0) return;
    if (g_ckpads < 0) { const char *e = getenv("SSBU_RB_NET_CKPADS"); g_ckpads = !(e && e[0] == '0'); }
    if (!g_ckpads) return;
    const uint8_t *b = p; while (n--) { g_hacc ^= *b++; g_hacc *= 0x100000001b3ull; }
}
static uint64_t hash_pads(uint64_t h) {
    if (g_ckpads < 0) { const char *e = getenv("SSBU_RB_NET_CKPADS"); g_ckpads = !(e && e[0] == '0'); }
    if (!g_ckpads) return h;
    for (int q = 0; q < 2; q++) {
        struct { uint64_t buttons; int16_t lx, ly, rx, ry; uint32_t read, varied; } r;
        memset(&r, 0, sizeof r);
        if (g_acc_n[q]) { r.buttons = g_acc_pad[q].buttons; r.lx = g_acc_pad[q].lx; r.ly = g_acc_pad[q].ly; r.rx = g_acc_pad[q].rx; r.ry = g_acc_pad[q].ry; r.read = 1; r.varied = (uint32_t)g_acc_var[q]; }
        const uint8_t *b = (const uint8_t *)&r; for (size_t n = 0; n < sizeof r; n++) { h ^= b[n]; h *= 0x100000001b3ull; }
    }
    return h;
}
void ladder_pass(long long f); void ladder_final(long long f);      /* ladder_report.c (lobby 1.5: a ranked match's result) */
void rbn_pass_hash(long long f) {
    ladder_pass(f);
    if (g_hlines && f > 0) { g_hash[f % HR] = hash_pads(g_hacc); g_hframe[f % HR] = f; }
    if (f > 0) { int x = (int)(f % HR2);
        for (int q = 0; q < 2; q++) { g_upad[x][q] = g_acc_pad[q]; g_un[x][q] = g_acc_n[q]; g_uvar[x][q] = g_acc_var[q]; }
        memcpy(g_ulines[x], g_lacc, sizeof g_lacc); g_unl[x] = g_lacc_n; }
    memset(g_acc_n, 0, sizeof g_acc_n); memset(g_acc_var, 0, sizeof g_acc_var); g_lacc_n = 0;
    g_hacc = 0xcbf29ce484222325ull; g_hlines = 0;
}
static void detail_frame(long long f) {              /* frame f is final: the pads its last simulation used against the final ones */
    int x = (int)(f % HR2);
    for (int q = 0; q < 2; q++) {
        RbPad fin; int pred = 0;
        if (q == g_local) { if (!rb_input_local_get((int)f, &fin)) memset(&fin, 0, sizeof fin); }
        else fin = rb_input_get(q, (int)f, &pred);
        int bad = g_un[x][q] && (!rb_pad_equal(&g_upad[x][q], &fin) || g_uvar[x][q]);
        if (bad || pred) { g_padcheck_bad++; if (g_padcheck_first < 0) g_padcheck_first = f;
            if (g_padcheck_bad <= 20) { printf("[rbnet] PAD CHECK: frame %lld player %d: its last simulation got %llx %d %d (reads %d, varied %d), the final pad is %llx %d %d%s\n", f, q + 1,
                (unsigned long long)g_upad[x][q].buttons, g_upad[x][q].lx, g_upad[x][q].ly, g_un[x][q], g_uvar[x][q], (unsigned long long)fin.buttons, fin.lx, fin.ly, pred ? " (NOT received)" : ""); fflush(stdout); } }
    }
    if (g_hlog && detail_on()) {
        fprintf(g_hlog, "d %lld p1 %llx %d %d %d %d n%d p2 %llx %d %d %d %d n%d", f, (unsigned long long)g_upad[x][0].buttons, g_upad[x][0].lx, g_upad[x][0].ly, g_upad[x][0].rx, g_upad[x][0].ry, g_un[x][0],
                (unsigned long long)g_upad[x][1].buttons, g_upad[x][1].lx, g_upad[x][1].ly, g_upad[x][1].rx, g_upad[x][1].ry, g_un[x][1]);
        for (int l = 0; l < g_unl[x]; l++) { const uint8_t *b = g_ulines[x][l]; int k, st; uint64_t mot, rh; float mf, px, py, dmg;
            memcpy(&k, b, 4); memcpy(&st, b + 4, 4); memcpy(&mot, b + 8, 8); memcpy(&mf, b + 16, 4); memcpy(&px, b + 20, 4); memcpy(&py, b + 24, 4); memcpy(&dmg, b + 28, 4); memcpy(&rh, b + 32, 8);
            fprintf(g_hlog, " | e%d st %x mot %llx mf %.3f x %.4f y %.4f dmg %.3f rng %016llx", k, st, (unsigned long long)mot, mf, px, py, dmg, (unsigned long long)rh); }
        fputc('\n', g_hlog);
    }
}
/* no rollback pending: every frame up to min(cur, newest remote pad) is final */
void rbn_finalize(long long cur) {
    long long upto = g_spec ? spec_confirmed() : rb_input_remote_newest_consecutive_frame(g_remote);
    if (upto > cur) upto = cur;
    for (long long f = g_hfinal_upto + 1; f <= upto; f++) {
        if (g_hlog && g_hframe[f % HR] == f) fprintf(g_hlog, "f %lld h %016llx\n", f, (unsigned long long)g_hash[f % HR]);
        detail_frame(f);
        if (f % 60 == 0 && f <= 600) {                  /* (2026-11-03) the same frames on both games, to compare their logs line by line */
            int x = (int)(f % HR2);
            printf("[rbnet] frame %lld (final) checksum %08x:", f, g_hframe[f % HR] == f ? (uint32_t)(g_hash[f % HR] ^ (g_hash[f % HR] >> 32)) : 0u);
            for (int q = 0; q < g_unl[x] && q < 4; q++) { struct { int k, st; uint64_t mot; float mf, fx, fy, dmg; uint64_t rh; } r3; memcpy(&r3, g_ulines[x][q], sizeof r3);
                printf(" | fighter %d: status %x motion %llx frame %.3f x %.4f y %.4f damage %.2f gen %016llx", q, r3.st, (unsigned long long)r3.mot, (double)r3.mf, (double)r3.fx, (double)r3.fy, (double)r3.dmg, (unsigned long long)r3.rh); }
            printf("\n"); fflush(stdout);
        }
        g_hfinal_upto = f;
        if (g_spec) spec_final(f);                      /* (2026-11-02) the fighters' checksums of this frame */
        ladder_final(f);
    }
    if (g_hlog && (upto & 63) == 0) fflush(g_hlog);
    {   long long st = settled_upto();
        for (int q = 0; q < PCQ; q++) if (g_pcq_f[q] > 0 && g_pcq_f[q] <= st) { long long cf = g_pcq_f[q]; g_pcq_f[q] = 0; on_check(cf, g_pcq_v[q]); } }
}

/* ---------------------------------------------------------------- time sync: extra wait per frame (ms) */
static double g_tsbias = -1;
double rbn_pace_extra(long long f) {
    if (!g_started || g_loop) return 0;
    if (g_spec) return spec_pace(f);                    /* (2026-11-02) a spectator: from its buffer of the fighters' pads */
    if (f % 30 == 0) {
        rb_ts_decide((int)f);
        double est_ms = rb_ts_offset() / 1000.0;
        /* (hs14) delay-based pacing (one game without the ring): that game aims at running half a frame BEHIND the other
         * (SSBU_RB_NET_TSBIAS ms, default 8), so the other player's pad of its frame is there before it needs it (a stall there
         * is a hitch); the game with the ring aims at the same split from its side */
        if (g_tsbias < 0) { const char *e = getenv("SSBU_RB_NET_TSBIAS"); g_tsbias = e ? atof(e) : 8.0; }
        if (g_peerdelay && g_lockstep) est_ms += g_tsbias; else if (peer_lockstep()) est_ms -= g_tsbias;
        /* ahead: half of the lead given back over the next 30 frames (the other game, behind, does nothing; both halves meet) */
        /* (2026-11-03) 8 ms, half a frame (clock_sync.c's and Slippi's threshold), not 2: the estimate's own noise is a few ms */
        if (est_ms >= 8.0) { g_ts_extra = est_ms / 2.0 / 30.0; if (g_ts_extra > 4.0) g_ts_extra = 4.0; g_ts_left = 30; g_ts_n++; }
        else g_ts_left = 0;
    }
    if (g_ts_left > 0) { g_ts_left--; return g_ts_extra; }
    return 0;
}
int rbn_rtt_ms(void) { return g_rtt_us > 0 ? (int)(g_rtt_us / 1000) : -1; }
void rbn_resend(void) { if (g_started) { if (g_spec) spec_nack(1); else send_pads(); } }     /* (2026-11-02) a spectator waiting: the missing pads asked for */
/* the match-start barrier: an empty PADS message says "in the match" (count 0, newest 0) */
void rbn_hello(void) {
    if (!g_started || g_loop || g_spec) return;        /* (2026-11-02) a spectator says nothing: the fighters' PADS start its match */
    { static int n; if (rbn_handshake_wanted() && n++ < 20 && (n % 4) == 0) hs_block_send(); }   /* (the block check again, a few times) */
    if (rb_input_local_newest_frame() >= 1) { send_pads(); return; }
    RbMsg m; memset(&m, 0, sizeof m); m.type = RB_MSG_PADS; m.player = g_local; m.newest = 0; m.count = 0; send_msg(&m);
    rbs_fighter_pads();                                 /* (2026-11-02) the spectators' start barrier */
}
int rbn_peer_started(void) { return g_loop || g_peer_started; }
void rbn_status(char *out, size_t n) {
    LONG q = (g_rq_head - g_rq_tail + RQ_N) % RQ_N;
    if (g_spec) { snprintf(out, n, "spectator: player 1's pads up to %d, player 2's up to %d; group messages received %lld (queue %ld)", rb_input_remote_newest_consecutive_frame(0), rb_input_remote_newest_consecutive_frame(1), (long long)g_rbs_rx, (long)q); return; }
    snprintf(out, n, "remote pads up to %d, local up to %d, the other game has ours up to %lld; packets sent %lld received %lld (bad %lld, queue %ld, full %lld)",
             rb_input_remote_newest_consecutive_frame(g_remote), rb_input_local_newest_frame(), g_peer_ack, (long long)g_tx_pkts, (long long)g_rx_pkts, (long long)g_rx_bad, (long)q, (long long)g_rx_drop);
}
double rbn_offset_ms(void) { return rb_ts_offset() / 1000.0; }

/* (hs12) the end of a match (rb_rollback.c, the first pass after the fight is off): everything of the session that
 * belongs to one match starts over (input queues and frames at the next rbn_start, checksums, counters, the block check,
 * received messages not drained yet); the round trip, the pings and the transport stay. */
void rbn_match_reset(void) {
    g_started = 0; g_peer_started = 0; g_f0 = 0; InterlockedExchange(&g_peer_mode, -1); g_peer_ack = 0; g_sent_newest = 0;
    g_stat_pred = g_stat_padget = g_stat_mis = 0; g_ts_n = 0; g_ts_extra = 0; g_ts_left = 0;
    g_hacc = 0xcbf29ce484222325ull; g_hlines = 0; g_lacc_n = 0;
    memset(g_hash, 0, sizeof g_hash); memset(g_hframe, 0, sizeof g_hframe); g_hfinal_upto = 0;
    g_desync = 0; g_check_ok = 0; g_desync_first = -1; g_peer_check_f = 0; g_peer_check = 0; memset(g_pcq_f, 0, sizeof g_pcq_f); g_pcq_i = 0;
    memset(g_acc_n, 0, sizeof g_acc_n); memset(g_acc_var, 0, sizeof g_acc_var); memset(g_un, 0, sizeof g_un); memset(g_unl, 0, sizeof g_unl);
    g_padcheck_bad = 0; g_padcheck_first = -1;
    AcquireSRWLockExclusive(&g_hs_lock); g_hs_mine_ok = 0; g_hs_blk_done = 0; memset(g_hs_peer_have, 0, sizeof g_hs_peer_have); ReleaseSRWLockExclusive(&g_hs_lock);
    AcquireSRWLockExclusive(&g_rq_lock); g_rq_tail = g_rq_head; g_tsq_n = 0; ReleaseSRWLockExclusive(&g_rq_lock);
    InterlockedExchange64(&g_cur, 0);
    if (g_hs_cancelled) menu_notice(NULL);
    InterlockedExchange(&g_hs_cancelled, 0);
    if (g_spec) {                                       /* (2026-11-02) a watched match is over: this game is an arena member again */
        AcquireSRWLockExclusive(&g_hs_lock); g_local = g_spec_saved_local; InterlockedExchange(&g_spec, 0); ReleaseSRWLockExclusive(&g_hs_lock);
        printf("[rbnet] spectator: the watched match is over\n");
    }
    /* (2026-11-03) the controller ports are NOT put back here: the results screen comes after this, and the PC of player 2
     * played on port 2, which became unconnected: nobody could press A on the results ("no pueden apretar A"). They are put
     * back at the next set-up, as before 9a7e976 (debug_mode.c). */
    printf("[rbnet] match %d over: the session is ready for the next match (pads of an older match dropped so far: %lld)\n", g_match_n, (long long)g_rx_oldmatch);
    fflush(stdout);
}
int rbn_match_number(void) { return g_match_n; }
long long rbn_desyncs(void) { return g_desync; }       /* (ladder_report.c) checksum mismatches of this match so far */
static long long rbs_hist_upto(void);
void rbn_report(FILE *o, long long frames) {
    if (g_on <= 0) return;
    if (g_hlog) fflush(g_hlog);
    fprintf(o, "[rbnet] report (match %d, id %08x): %lld frames; delay %d; round trip mean %.1f ms (best %.1f, %lld samples); clock offset estimate %.1f ms, time-sync corrections %lld\n",
            g_match_n, g_match_id, frames, g_delay, g_rtt_n ? g_rtt_sum / g_rtt_n / 1000.0 : -1.0, g_rtt_min_us > 0 ? g_rtt_min_us / 1000.0 : -1.0, g_rtt_n, rb_ts_offset() / 1000.0, g_ts_n);
    fprintf(o, "[rbnet] report: pads given %lld, predicted %lld (%.1f%%), mispredictions %lld; finalized frames %lld; checksums compared %lld, DESYNCS %lld (first %lld)\n",
            g_stat_padget, g_stat_pred, g_stat_padget ? 100.0 * g_stat_pred / g_stat_padget : 0.0, g_stat_mis, g_hfinal_upto, g_check_ok + g_desync, g_desync, g_desync_first);
    fprintf(o, "[rbnet] report: pad check of the final simulations: %lld pad values differ from the final ones (first frame %lld)\n", g_padcheck_bad, g_padcheck_first);
    fprintf(o, "[rbnet] report: packets sent %lld (failed %lld), received %lld (bad %lld, queue full %lld); remote pads up to %d, local up to %d\n",
            (long long)g_tx_pkts, (long long)g_tx_fail, (long long)g_rx_pkts, (long long)g_rx_bad, (long long)g_rx_drop, rb_input_remote_newest_consecutive_frame(g_remote), rb_input_local_newest_frame());
    {   void net_rb_path_stats(long long *dtx, long long *drx, long long *ltx, long long *lrx, int *direct_now);
        long long dtx, drx, ltx, lrx; int dn; net_rb_path_stats(&dtx, &drx, &ltx, &lrx, &dn);
        fprintf(o, "[rbnet] report: path (whole session): straight to the opponent sent %lld, received %lld; through the lobby sent %lld, received %lld; direct path answering now: %s\n",
                dtx, drx, ltx, lrx, dn ? "yes" : "NO"); }
    if (g_spec) spec_report(o, frames);                 /* (2026-11-02) */
    else if (g_rbs_fighter) fprintf(o, "[arena] report: this fighter sent the arena's group %lld messages (pads every pass, answers to NACKs), received %lld; its pad history up to frame %lld\n", (long long)g_rbs_tx, (long long)g_rbs_rx, rbs_hist_upto());
    fflush(o);
}

/* ---------------------------------------------------------------- (2026-11-02) spectators of a Battle Arena (RBS_*, see above the handshake) */
int net_rbg_send(uint64_t key, const void *p, int n);  /* net_relay.c: packet type 27, to every other member of the arena's group */
static void rbs_head(void *m, int type) {
    RbsHead *h = (RbsHead *)m; h->type = (uint8_t)type; h->proto = RBH_PROTO; h->game_build = (int16_t)g_game_build; h->exe_stamp = exe_stamp(); h->gid = g_rbs_gid;
}
static void rbs_send(const void *p, int n) { if (g_rbs_gid && net_rbg_send(g_rbs_gid, p, n)) InterlockedIncrement64(&g_rbs_tx); }
static void rbs_native(const char *why, int spectator_seat) {
    if (!g_rbs_gid) return;
    for (int k = 0; k < 3; k++) {
        if (spectator_seat >= 0) { RbsHello h; memset(&h, 0, sizeof h); rbs_head(&h, RBS_HELLO); h.seat = (uint8_t)spectator_seat; h.flags = 1; rbs_send(&h, sizeof h); }
        else { RbsNative m; memset(&m, 0, sizeof m); rbs_head(&m, RBS_NATIVE); snprintf(m.why, sizeof m.why, "%s", why); rbs_send(&m, sizeof m); }
        Sleep(20);
    }
    printf("[arena] %s sent to the arena's group (arena %u, 3 times): %s\n", spectator_seat >= 0 ? "a refusing HELLO (this spectator watches with the game's own netcode)" : "NATIVE (the game's own netcode plays this match)", g_rbs_gid, why);
    fflush(stdout);
}
/* the spectator at its set-up (rbn_handshake, its seat does not fight): HELLO every 100 ms until player 1's MATCH (or NATIVE,
 * or the time limit, or Esc / B). MATCH: this game plays the agreed match as a local match fed by both fighters (1, *out =
 * the match, as a fighter's handshake gives it); otherwise nothing is changed and the game's own netcode shows the match (0). */
static int rbs_spectate(long round, uint32_t gid, int players, int seat, const int sor[2], const RbhMatch *mine, RbhMatch *out) {
    double limit = getenv("SSBU_RB_NET_SPEC_WAIT") ? atof(getenv("SSBU_RB_NET_SPEC_WAIT")) * 1000.0 : 20000.0;
    RbsHello h; memset(&h, 0, sizeof h);
    AcquireSRWLockExclusive(&g_rbs_lock);
    g_rbs_gid = gid; g_rbs_sor[0] = sor[0]; g_rbs_sor[1] = sor[1]; g_rbs_native_why[0] = 0;
    InterlockedExchange(&g_rbs_got, 0); InterlockedExchange(&g_rbs_wait, 1);
    rbs_head(&h, RBS_HELLO); h.seat = (uint8_t)seat; h.mask = (uint8_t)((1u << sor[0]) | (1u << sor[1])); h.rule_kind = mine->rule_kind; h.stock = mine->stock; h.chara0 = mine->chara[0];
    ReleaseSRWLockExclusive(&g_rbs_lock);
    printf("[rbnet] spectator: handshake %ld: a Battle Arena's match (arena %u, %d members, fighting seats %d and %d; this game is seat %d and watches): HELLO to the arena's group every 100 ms, waiting for player 1's MATCH (at most %.0f s)\n",
           round, gid, players, sor[0], sor[1], seat, limit / 1000.0);
    fflush(stdout);
    uint64_t t0 = GetTickCount64(), last_tx = 0, last_note = 0, last_log = t0, last_redraw = 0, presents_at = t0; long long presents = nvn_front_presents(), hellos = 0; int stopped = 0;
    for (;;) {
        uint64_t now = GetTickCount64();
        if (g_rbs_got) break;
        if ((double)(now - t0) > limit) break;
        if (hs_stop_pressed((double)(now - t0))) { stopped = 1; break; }
        if (now - last_tx >= 100) { last_tx = now; rbs_send(&h, sizeof h); hellos++; }
        if (now - last_note >= 250) { last_note = now; char t[400];
            snprintf(t, sizeof t, "Battle Arena: you are watching this match.\nWaiting for the fighters to start it (%.0f s; after %.0f s the game shows it its own way).\nPress Esc or hold B to stop waiting.", (now - t0) / 1000.0, limit / 1000.0);
            menu_notice(t); }
        {   long long pr = nvn_front_presents();
            if (pr != presents) { presents = pr; presents_at = now; }
            else if (now - presents_at > 100 && now - last_redraw >= 100) { last_redraw = now; nvn_front_osk_redraw(); } }
        if (now - last_log >= 5000) { last_log = now; printf("[rbnet] spectator: handshake %ld: still waiting for player 1's MATCH (%.0f s; HELLO sent %lld, group messages received %lld; lobby %s)\n", round, (now - t0) / 1000.0, hellos, (long long)g_rbs_rx, net_rb_ready() ? "joined" : "NOT joined"); fflush(stdout); }
        Sleep(5);
    }
    AcquireSRWLockExclusive(&g_rbs_lock); int got = (int)g_rbs_got; RbsMatch mm = g_rbs_match; char nwhy[200]; snprintf(nwhy, sizeof nwhy, "%s", g_rbs_native_why); InterlockedExchange(&g_rbs_wait, 0); ReleaseSRWLockExclusive(&g_rbs_lock);
    menu_notice(NULL);
    uint32_t mid = (uint32_t)(mm.m.seed ^ (mm.m.seed >> 32)) | 1u;
    char why[260]; why[0] = 0;
    if (got == 2) snprintf(why, sizeof why, "NATIVE from a fighter (%s)", nwhy);
    else if (stopped) snprintf(why, sizeof why, "stopped waiting (Esc / B)");
    else if (got != 1) snprintf(why, sizeof why, "no MATCH from player 1 in %.0f s", limit / 1000.0);
    else if (!mm.m.seed || mm.match_id != mid) snprintf(why, sizeof why, "the MATCH message is damaged (match id %08x, %08x from its seed)", mm.match_id, mid);
    else if (mm.seat_of_rank[0] != sor[0] || mm.seat_of_rank[1] != sor[1]) snprintf(why, sizeof why, "player 1 says seats %d and %d fight, this game sees %d and %d", mm.seat_of_rank[0], mm.seat_of_rank[1], sor[0], sor[1]);
    if (why[0]) {
        if (got != 2) rbs_native(why, seat);           /* (player 1, if it still waits for this HELLO, gives up at once) */
        InterlockedExchange(&g_hs_room, 1);
        printf("[rbnet] spectator: handshake %ld: %s after %.1f s: the game's own online netcode shows this match; nothing changed\n", round, why, (GetTickCount64() - t0) / 1000.0);
        fflush(stdout);
        return 0;
    }
    *out = mm.m;
    AcquireSRWLockExclusive(&g_hs_lock);
    g_spec_saved_local = g_local; g_local = -1; InterlockedExchange(&g_spec, 1);
    memcpy(g_spec_lay, mm.lay, sizeof g_spec_lay); memcpy(g_spec_ctl, mm.ctl, sizeof g_spec_ctl); g_spec_ctl_ok = mm.ctl_ok;
    g_hs_cancelled = 0; g_hs_waiting = 0;
    ReleaseSRWLockExclusive(&g_hs_lock);
    g_hs_lwr = 1;
    g_hs_seed = out->seed; g_match_id = mid; g_hs_delay = out->pad0;
    host_fixed_random_reseed(g_hs_seed);
    { void fh_new_fight(void); fh_new_fight(); }     /* (fighter_host.c) as a fighter's game: the generators from this match's seed */
    printf("[rbnet] spectator: handshake %ld: MATCH from player 1 after %.2f s (HELLO sent %lld; match id %08x; players' controls %s): this game shows the match as a local match fed with both fighters' final pads (no ring, no guesses; buffer %d frames)\n",
           round, (GetTickCount64() - t0) / 1000.0, hellos, g_match_id, mm.ctl_ok ? "exchanged" : "NOT exchanged by the fighters (each with the defaults)", RBS_B);
    hs_print("the match (spectator)", out);
    return 1;
}
/* debug_mode.c (a spectator, at the fighters' records): both players' layouts and controls as player 1 wrote them; the
 * return value is whether the fighters exchanged their controls (-1: this game does not watch) */
int rbn_spec_controls(uint16_t lay[2][11], uint8_t ctl[2][RBS_LCTL]) {
    if (!g_spec) return -1;
    memcpy(lay, g_spec_lay, sizeof g_spec_lay); memcpy(ctl, g_spec_ctl, sizeof g_spec_ctl);
    return g_spec_ctl_ok;
}
/* debug_mode.c (player 1, at player 2's record, after the fighters' rbn_room_layout): MATCH to the spectators, now and then
 * every 100 ms (ping thread) until this game's fight starts */
void rbn_spec_match_send(const uint16_t lay[2][11], const uint8_t ctl[2][RBS_LCTL], int ctl_ok) {
    if (!g_rbs_fighter || g_local != 0 || !g_rbs_gid) return;
    RbsMatch m; memset(&m, 0, sizeof m); rbs_head(&m, RBS_MATCH);
    m.match_id = g_match_id; m.seat_of_rank[0] = (uint8_t)g_rbs_sor[0]; m.seat_of_rank[1] = (uint8_t)g_rbs_sor[1]; m.ctl_ok = (uint8_t)(ctl_ok != 0);
    m.m = g_rbs_agreed; memcpy(m.lay, lay, sizeof m.lay); memcpy(m.ctl, ctl, sizeof m.ctl);
    AcquireSRWLockExclusive(&g_rbs_lock); g_rbs_match = m; g_rbs_match_until = GetTickCount64() + 25000; ReleaseSRWLockExclusive(&g_rbs_lock);
    rbs_send(&m, sizeof m);
    InterlockedExchange(&g_rbs_match_tx, 1);
    printf("[arena] MATCH sent to the arena's spectators (arena %u, match id %08x, seats %d and %d, controls %s; %d bytes): again every 100 ms until this fight starts\n", g_rbs_gid, g_match_id, g_rbs_sor[0], g_rbs_sor[1], ctl_ok ? "exchanged" : "not exchanged", (int)sizeof m);
    fflush(stdout);
}

/* a fighter: the history of its own final pads (frames 1 ..), what the spectators get (PADS, NACK answers) */
static RbPad g_rbs_hist[RBS_HIST]; static int g_rbs_hist_f[RBS_HIST]; static long long g_rbs_hist_n;
static long long g_rbs_nack_rx;
static void rbs_hist_reset(void) { AcquireSRWLockExclusive(&g_rbs_lock); memset(g_rbs_hist_f, 0, sizeof g_rbs_hist_f); g_rbs_hist_n = 0; g_rbs_nack_rx = 0; ReleaseSRWLockExclusive(&g_rbs_lock); }
static long long rbs_hist_upto(void) { return g_rbs_hist_n; }
static void rbs_hist_update(void) {
    long long nf = rb_input_local_newest_frame();
    AcquireSRWLockExclusive(&g_rbs_lock);
    for (long long f = g_rbs_hist_n + 1; f <= nf; f++) {
        RbPad p; if (!rb_input_local_get((int)f, &p)) memset(&p, 0, sizeof p);
        g_rbs_hist[f % RBS_HIST] = p; g_rbs_hist_f[f % RBS_HIST] = (int)f;
    }
    if (nf > g_rbs_hist_n) g_rbs_hist_n = nf;
    ReleaseSRWLockExclusive(&g_rbs_lock);
}
/* PADS to the group: this fighter's pads `from` .. (at most `most`, newest first as rb_wire has them); check: its newest
 * finalized checksum (the main thread only) */
static void rbs_send_pads(long long from, int most, int check) {
    uint8_t b[sizeof(RbsPadsHead) + RB_PADS_HEAD + RB_MAX_PADS_PER_MSG * RB_PAD_WIRE + 16];
    RbMsg m; memset(&m, 0, sizeof m); m.type = RB_MSG_PADS; m.player = g_local;
    if (most > RB_MAX_PADS_PER_MSG) most = RB_MAX_PADS_PER_MSG;
    AcquireSRWLockShared(&g_rbs_lock);
    long long upto = g_rbs_hist_n;
    if (from < 1) from = 1;
    if (from < upto - RBS_HIST + 1) from = upto - RBS_HIST + 1;
    long long newest = from + most - 1; if (newest > upto) newest = upto;
    m.newest = (int32_t)upto;
    if (newest >= from) { m.newest = (int32_t)newest; for (long long f = newest; f >= from && m.count < most; f--) { int x = (int)(f % RBS_HIST); if (g_rbs_hist_f[x] != f) break; m.pads[m.count++] = g_rbs_hist[x]; } }
    ReleaseSRWLockShared(&g_rbs_lock);
    if (check) { long long cf = settled_upto(); if (cf > 0 && g_hframe[cf % HR] == cf) { m.check_frame = (int32_t)cf; m.check = (uint32_t)(g_hash[cf % HR] ^ (g_hash[cf % HR] >> 32)); } }
    RbsPadsHead h; memset(&h, 0, sizeof h); rbs_head(&h, RBS_PADS); h.tag = pads_tag(); h.rank = (uint8_t)g_local;
    memcpy(b, &h, sizeof h);
    size_t k = rb_msg_build(b + sizeof h, sizeof b - sizeof h, &m);
    if (k) rbs_send(b, (int)(sizeof h + k));
}
static void rbs_fighter_pads(void) {                    /* every pass of the fight (send_pads), every 50 ms while waiting, at the start barrier */
    if (!g_rbs_fighter || !g_started || g_loop) return;
    rbs_hist_update();
    rbs_send_pads(g_rbs_hist_n - 7, 8, 1);
}

/* the spectator in the match */
static long long g_spec_newest[2], g_spec_pkts, g_spec_nack_tx, g_spec_waits, g_spec_long_waits, g_spec_slow, g_spec_fast, g_spec_lead_n, g_spec_lead_sum, g_spec_lead_min, g_spec_lead_max;
static long long g_spec_ck_ok, g_spec_ck_bad, g_spec_ck_first = -1, g_spec_ck_lost;
static double g_spec_wait_ms, g_spec_wait_max;
static long long g_spec_far;                            /* pads left out: too far ahead for the input ring (asked for again) */
static int g_spec_seen, g_spec_noticed, g_spec_said;
static uint64_t g_spec_nack_ms[2];
static int g_spec_ck_f[2][RBS_CK]; static uint32_t g_spec_ck[2][RBS_CK];
static void spec_start(void) {
    memset(g_spec_newest, 0, sizeof g_spec_newest); g_spec_pkts = g_spec_nack_tx = g_spec_waits = g_spec_long_waits = g_spec_slow = g_spec_fast = g_spec_lead_n = g_spec_lead_sum = 0;
    g_spec_lead_min = -1; g_spec_lead_max = 0; g_spec_ck_ok = g_spec_ck_bad = g_spec_ck_lost = g_spec_far = 0; g_spec_ck_first = -1; g_spec_wait_ms = g_spec_wait_max = 0;
    g_spec_seen = g_spec_noticed = g_spec_said = 0; memset(g_spec_nack_ms, 0, sizeof g_spec_nack_ms); memset(g_spec_ck_f, 0, sizeof g_spec_ck_f);
    printf("[rbnet] spectator: match %d (id %08x) start: showing players 1 and 2 (seats %d and %d) from their final pads; buffer %d frames; waiting for both fighters' pads (start barrier)\n", g_match_n, g_match_id, g_rbs_sor[0], g_rbs_sor[1], RBS_B);
    fflush(stdout);
}
static long long spec_confirmed(void) {
    long long a = rb_input_remote_newest_consecutive_frame(0), b = rb_input_remote_newest_consecutive_frame(1);
    return a < b ? a : b;
}
static void spec_compare(int p, long long f, uint32_t ck) {
    uint32_t mine = (uint32_t)(g_hash[f % HR] ^ (g_hash[f % HR] >> 32));
    if (mine == ck) { g_spec_ck_ok++; return; }
    g_spec_ck_bad++; if (g_spec_ck_first < 0) g_spec_ck_first = f;
    if (g_spec_ck_bad <= 5) { printf("[rbnet] spectator: CHECK MISMATCH with player %d at frame %lld: here %08x, the fighter %08x (this view differs from the fight; notice only)\n", p + 1, f, mine, ck); fflush(stdout); }
}
static void spec_check(int p, long long cf, uint32_t ck) {
    if (cf <= 0) return;
    if (cf <= g_hfinal_upto) { if (g_hframe[cf % HR] == cf) spec_compare(p, cf, ck); else g_spec_ck_lost++; return; }
    g_spec_ck_f[p][cf % RBS_CK] = (int)cf; g_spec_ck[p][cf % RBS_CK] = ck;      /* (compared when this frame is final here) */
}
static void spec_final(long long f) {
    for (int p = 0; p < 2; p++) { int x = (int)(f % RBS_CK); if (g_spec_ck_f[p][x] != f) continue; g_spec_ck_f[p][x] = 0; if (g_hframe[f % HR] == f) spec_compare(p, f, g_spec_ck[p][x]); else g_spec_ck_lost++; }
}
static void spec_rx_pads(const RbMsg *m) {              /* rbn_drain: a fighter's PADS (player = its rank) */
    int p = m->player; if (p < 0 || p > 1) return;
    g_spec_pkts++;
    if (!(g_spec_seen & (1 << p))) { g_spec_seen |= 1 << p; printf("[rbnet] spectator: first pads of player %d (newest frame %d)\n", p + 1, m->newest); fflush(stdout); }
    if (g_spec_seen == 3) g_peer_started = 1;           /* (the start barrier: both fighters are in the match) */
    /* (a spectator far behind the fighters: a pad more than the input ring ahead of the next frame to show would take the slot
     * of a frame not shown yet, which would then run on a guess; such pads are left out and asked for again later by NACK) */
    long long ahead_max = (long long)g_cur + RB_RING - 32;
    for (int k = 0; k < m->count; k++) { long long f = (long long)m->newest - k; if (f <= ahead_max) rb_input_remote_put(p, (int)f, m->pads[k]); else g_spec_far++; }
    if (m->newest > g_spec_newest[p]) g_spec_newest[p] = m->newest;
    if (m->check_frame > 0) spec_check(p, m->check_frame, m->check);
}
static void spec_nack(int force) {                      /* force: the controller waits (rbn_resend) */
    uint64_t now = GetTickCount64();
    for (int p = 0; p < 2; p++) {
        long long c = rb_input_remote_newest_consecutive_frame(p);
        int want = force ? c < g_cur + 1 : g_spec_newest[p] > c;
        if (!want || now - g_spec_nack_ms[p] < 60) continue;
        g_spec_nack_ms[p] = now;
        RbsNack k; memset(&k, 0, sizeof k); rbs_head(&k, RBS_NACK); k.tag = pads_tag(); k.rank = (uint8_t)p; k.from = (int32_t)(c + 1);
        rbs_send(&k, sizeof k); g_spec_nack_tx++;
        if (g_spec_nack_tx <= 10 || g_spec_nack_tx % 100 == 0) { printf("[rbnet] spectator: NACK %lld to player %d: pads from frame %lld (its newest seen %lld)%s\n", g_spec_nack_tx, p + 1, c + 1, g_spec_newest[p], force ? " (waiting)" : ""); fflush(stdout); }
    }
}
/* the jitter buffer: under RBS_B frames of both fighters' pads ahead, a little slower; over 3 x RBS_B, faster (catching up) */
static double spec_pace(long long f) {
    long long lead = spec_confirmed() - f; if (lead < 0) lead = 0;
    g_spec_lead_n++; g_spec_lead_sum += lead; if (g_spec_lead_min < 0 || lead < g_spec_lead_min) g_spec_lead_min = lead; if (lead > g_spec_lead_max) g_spec_lead_max = lead;
    if (lead > 3 * RBS_B) { g_spec_fast++; return lead > 30 ? -8.0 : -4.0; }
    if (lead < RBS_B) { g_spec_slow++; return 3.0; }
    return 0;
}
static void spec_give_up(const char *why) {
    AcquireSRWLockExclusive(&g_hs_lock); snprintf(g_hs_why, sizeof g_hs_why, "%s", why); InterlockedExchange(&g_hs_cancelled, 1); ReleaseSRWLockExclusive(&g_hs_lock);
    printf("[rbnet] spectator: %s at frame %lld: this view is given up (its player 1 walks off the stage to end the match here)\n", why, (long long)g_cur);
    spec_report(stdout, g_cur);
}
/* rollback_controller.c, at the start barrier: no pads from the fighters in time */
void rbn_spec_give_up(const char *why) { if (g_spec) spec_give_up(why); }
/* (2026-11-03) a fighter: the other game stopped sending its moves while still answering (its fight was over: the two games no
 * longer showed the same fight); this match is left the way of a match not agreed (rollback_controller.c: player 1 walks off) */
void rbn_give_up(const char *why) {
    AcquireSRWLockExclusive(&g_hs_lock); snprintf(g_hs_why, sizeof g_hs_why, "%s", why); InterlockedExchange(&g_hs_cancelled, 1); ReleaseSRWLockExclusive(&g_hs_lock);
    printf("[rbnet] %s at frame %lld (checksums compared %lld, DESYNCS %lld, first %lld): this match is left here\n", why, (long long)g_cur, g_check_ok + g_desync, g_desync, g_desync_first);
    fflush(stdout);
}
/* rollback_controller.c, every turn of its wait for the pads of the next frame (waited_ms since the wait began): a notice after
 * 1 s; after 30 s the match is given up (1: the controller leaves it the way of a match not agreed) */
int rbn_spec_wait(double waited_ms) {
    if (!g_spec) return 0;
    static uint64_t last_note;
    if (waited_ms >= 1000) {
        if (!g_spec_said) { g_spec_said = 1; printf("[rbnet] spectator: waiting for the fighters at frame %lld for 1 s (player 1's pads up to %d, newest seen %lld; player 2's up to %d, newest seen %lld): notice shown\n", (long long)g_cur + 1,
                                                   rb_input_remote_newest_consecutive_frame(0), g_spec_newest[0], rb_input_remote_newest_consecutive_frame(1), g_spec_newest[1]); fflush(stdout); }
        if (GetTickCount64() - last_note >= 500) { last_note = GetTickCount64(); char t[300];
            snprintf(t, sizeof t, "Watching the match: waiting for the fighters (%.0f s).\nThe match goes on by itself as soon as their moves arrive.", waited_ms / 1000.0); menu_notice(t); g_spec_noticed = 1; }
    }
    if (waited_ms >= 30000) { spec_give_up("the fighters' pads stopped coming for 30 s"); return 1; }
    return 0;
}
void rbn_spec_wait_done(double waited_ms) {             /* the wait is over (the pads came) */
    if (!g_spec) return;
    g_spec_waits++; g_spec_wait_ms += waited_ms; if (waited_ms > g_spec_wait_max) g_spec_wait_max = waited_ms; if (waited_ms >= 100) g_spec_long_waits++;
    if (g_spec_said) { printf("[rbnet] spectator: the fighters' pads came again after %.1f s\n", waited_ms / 1000.0); fflush(stdout); }
    if (g_spec_noticed) menu_notice(NULL);
    g_spec_noticed = g_spec_said = 0;
}
static void spec_report(FILE *o, long long frames) {
    fprintf(o, "[rbnet] spectator: report (match %d, id %08x): %lld frames shown (final %lld); player 1's pads up to %d (newest seen %lld), player 2's up to %d (newest seen %lld); pad messages %lld, group messages received %lld, NACKs sent %lld, pads too far ahead left out %lld\n",
            g_match_n, g_match_id, frames, g_hfinal_upto, rb_input_remote_newest_consecutive_frame(0), g_spec_newest[0], rb_input_remote_newest_consecutive_frame(1), g_spec_newest[1], g_spec_pkts, (long long)g_rbs_rx, g_spec_nack_tx, g_spec_far);
    fprintf(o, "[rbnet] spectator: report: waits for the fighters %lld (%lld of 100 ms or more; %.0f ms in all, longest %.0f ms); buffer ahead %lld..%lld frames (mean %.1f); frames slowed (under %d) %lld, sped up (over %d) %lld\n",
            g_spec_waits, g_spec_long_waits, g_spec_wait_ms, g_spec_wait_max, g_spec_lead_min < 0 ? 0 : g_spec_lead_min, g_spec_lead_max, g_spec_lead_n ? (double)g_spec_lead_sum / g_spec_lead_n : 0.0, RBS_B, g_spec_slow, 3 * RBS_B, g_spec_fast);
    fprintf(o, "[rbnet] spectator: report: the fighters' checksums compared %lld, MISMATCHES %lld (first frame %lld), not comparable (too old) %lld\n", g_spec_ck_ok + g_spec_ck_bad, g_spec_ck_bad, g_spec_ck_first, g_spec_ck_lost);
    fflush(o);
}

/* net_relay.c's receive thread: a message of the arena's group (packet type 27) */
void rbs_rx(uint32_t src, const uint8_t *p, int n) {
    (void)src;
    if (rbn_on() <= 0 || n < (int)sizeof(RbsHead)) return;
    RbsHead h; memcpy(&h, p, sizeof h);
    if (h.type < RBS_HELLO || h.type > RBS_HERE || !g_rbs_gid || h.gid != g_rbs_gid) return;
    InterlockedIncrement64(&g_rbs_rx);
    switch (h.type) {
    case RBS_HELLO: {                                   /* (player 1, in its handshake) */
        if (!g_rbs_need || n < (int)sizeof(RbsHello)) break;
        RbsHello m; memcpy(&m, p, sizeof m);
        char why[200]; why[0] = 0;
        AcquireSRWLockExclusive(&g_rbs_lock);
        LONG bit = m.seat < 8 ? 1L << m.seat : 0;
        if (m.flags & 1) snprintf(why, sizeof why, "the spectator at seat %d watches this match with the game's own netcode", m.seat);
        else if (m.h.proto != RBH_PROTO) snprintf(why, sizeof why, "the spectator at seat %d has another version of the rollback (protocol %d here, %d there)", m.seat, RBH_PROTO, m.h.proto);
        else if (m.h.game_build != g_rbs_expect.h.game_build) snprintf(why, sizeof why, "the spectator at seat %d has another game version (build %d here, %d there)", m.seat, g_rbs_expect.h.game_build, m.h.game_build);
        else if (m.h.exe_stamp != g_rbs_expect.h.exe_stamp) snprintf(why, sizeof why, "the spectator at seat %d runs another build of the PC port (exe stamp %08x here, %08x there)", m.seat, g_rbs_expect.h.exe_stamp, m.h.exe_stamp);
        else if (!bit || (g_rbs_expect.mask & bit)) snprintf(why, sizeof why, "seat %d is not a spectator's seat here", m.seat);
        else if (m.mask != g_rbs_expect.mask) snprintf(why, sizeof why, "the spectator at seat %d sees other fighting seats (mask %02x here, %02x there)", m.seat, g_rbs_expect.mask, m.mask);
        else if (m.rule_kind != g_rbs_expect.rule_kind || m.stock != g_rbs_expect.stock || ((g_rbs_expect.mask & 1) && m.chara0 != g_rbs_expect.chara0))
            snprintf(why, sizeof why, "the spectator at seat %d sees another match (rule kind %d / %d, stocks %d / %d, seat 0's fighter %016llx / %016llx)", m.seat, g_rbs_expect.rule_kind, m.rule_kind, g_rbs_expect.stock, m.stock,
                     (unsigned long long)g_rbs_expect.chara0, (unsigned long long)m.chara0);
        int fresh = 0;
        if (why[0]) { if (!g_rbs_bad) { snprintf(g_rbs_bad_why, sizeof g_rbs_bad_why, "%s", why); InterlockedExchange(&g_rbs_bad, 1); fresh = 1; } }
        else if (!(g_rbs_seats & bit)) { g_rbs_seats |= bit; fresh = 2; }
        int have = rbs_popc(g_rbs_seats), need = (int)g_rbs_need;
        ReleaseSRWLockExclusive(&g_rbs_lock);
        if (fresh == 1) printf("[arena] HELLO from the spectator at seat %d REFUSED: %s\n", m.seat, why);
        if (fresh == 2) printf("[arena] HELLO from the spectator at seat %d: ready (%d of %d spectators)\n", m.seat, have, need);
        if (fresh) fflush(stdout);
        break; }
    case RBS_MATCH:                                     /* (a spectator at its set-up) */
        if (!g_rbs_wait || n < (int)sizeof(RbsMatch)) break;
        AcquireSRWLockExclusive(&g_rbs_lock);
        if (!g_rbs_got && h.proto == RBH_PROTO) { memcpy(&g_rbs_match, p, sizeof g_rbs_match); InterlockedExchange(&g_rbs_got, 1); }
        ReleaseSRWLockExclusive(&g_rbs_lock);
        break;
    case RBS_NATIVE: {
        if (!g_rbs_wait) break;
        RbsNative m; memset(&m, 0, sizeof m); memcpy(&m, p, (size_t)n < sizeof m ? (size_t)n : sizeof m); m.why[sizeof m.why - 1] = 0;
        AcquireSRWLockExclusive(&g_rbs_lock);
        if (!g_rbs_got) { snprintf(g_rbs_native_why, sizeof g_rbs_native_why, "%s", m.why); InterlockedExchange(&g_rbs_got, 2); }
        ReleaseSRWLockExclusive(&g_rbs_lock);
        break; }
    case RBS_PADS: {                                    /* (a spectator in its match) into the receive queue, drained with the game's frames */
        if (!g_spec || !g_started || n < (int)sizeof(RbsPadsHead) + RB_PADS_HEAD) break;
        RbsPadsHead ph; memcpy(&ph, p, sizeof ph);
        if (ph.tag != pads_tag()) { InterlockedIncrement64(&g_rx_oldmatch); break; }
        const uint8_t *q = p + sizeof ph; int qn = n - (int)sizeof ph;
        if (qn > RQ_MAX) { InterlockedIncrement64(&g_rx_bad); break; }
        InterlockedIncrement64(&g_rx_pkts); InterlockedExchange64(&g_rx_last_ms, (LONG64)GetTickCount64());
        AcquireSRWLockExclusive(&g_rq_lock);
        if (!g_rq) g_rq = calloc(RQ_N, sizeof *g_rq);
        LONG next = (g_rq_head + 1) % RQ_N;
        if (g_rq && next != g_rq_tail) { g_rq[g_rq_head].at_us = now_us(); g_rq[g_rq_head].at_frame = g_cur; g_rq[g_rq_head].n = qn; g_rq[g_rq_head].grp = 1; memcpy(g_rq[g_rq_head].b, q, (size_t)qn); g_rq_head = next; }
        else InterlockedIncrement64(&g_rx_drop);
        ReleaseSRWLockExclusive(&g_rq_lock);
        break; }
    case RBS_NACK: {                                    /* (a fighter, also after its fight: the history stays until its next set-up) */
        if (!g_rbs_fighter || n < (int)sizeof(RbsNack)) break;
        RbsNack k; memcpy(&k, p, sizeof k);
        if (k.tag != pads_tag() || k.rank != (uint8_t)g_local) break;
        long long c = ++g_rbs_nack_rx;
        rbs_send_pads(k.from, RB_MAX_PADS_PER_MSG, 0);
        if (c <= 10 || c % 100 == 0) { printf("[arena] NACK %lld from a spectator: this fighter's pads from frame %d sent again (history up to %lld)\n", c, k.from, g_rbs_hist_n); fflush(stdout); }
        break; }
    }
}
