/* Online play is not in the Linux build yet (net_relay.c uses Winsock extensions and COM UPnP; tools/lifter/build.py
 * leaves it out there). These are the net_relay.h functions nnldn.c calls, answering as with SSBU_ONLINE unset:
 * net_online() is 0, so nnldn_register() registers nothing and the game's Local Wireless stays the offline console's. */
#include <stdint.h>
#include <string.h>
#include "net_relay.h"

int net_online(void) { return 0; }
int net_mode(void) { return 0; }
int net_log_level(void) { return 0; }
void net_start(void) {}
uint32_t net_my_ip(void) { return 0; }
int net_rb_no_destination(void) { return 0; }      /* (R3 pub1) rb_net.c's notice */
int net_rb_ready(void) { return 0; }                /* rb_net.c (rollback over the network: off without online play) */
int net_rb_send(const void *p, int n) { (void)p; (void)n; return 0; }
int net_rbg_send(uint64_t key, const void *p, int n) { (void)key; (void)p; (void)n; return 0; }      /* (2026-11-02) a Battle Arena's spectators */
int net_lobby_gone(void) { return 0; }
void net_list_rooms(uint32_t seq) { (void)seq; }
void net_send(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, uint32_t len) {
    (void)src; (void)sport; (void)dst; (void)dport; (void)data; (void)len;
}
int net_room_refusal(uint32_t host, char *why, size_t n) { (void)host; (void)why; (void)n; return 0; }
/* the settings menu (menu.cpp: the Online pages, the stats overlay): what the online code answers when it is off */
const char *net_room_code(void) { return ""; }
const char *net_lan_code(void) { return ""; }
const char *net_upnp_result(void) { return ""; }
#include "net_stats.h"
void net_stats_get(NetStats *o) { memset(o, 0, sizeof *o); o->state = NET_OFFLINE; o->ping_ms = -1; }
int net_rooms_get(NetRoom *out, int max, int *state) { (void)out; (void)max; if (state) *state = 0; return 0; }
const char *net_build_id(void) { return ""; }
const char *net_mods_text(void) { return "none"; }
uint32_t net_match_host(void) { return 0; }
void net_match_find(void) {}
void net_match_find_rb(void) {}
void net_match_cancel(void) {}
void net_match_get(NetMatch *o) { memset(o, 0, sizeof *o); o->state = MM_OFFLINE; o->players = o->searching = o->code_players = -1; }
void net_ladder_get(NetLadder *o) { memset(o, 0, sizeof *o); o->state = NET_LADDER_OFF; o->last_outcome = -1; }   /* (ladder: off without online play) */
void net_ladder_result(const NetLadderResult *r) { (void)r; }
void net_tourney_get(NetTourney *o) { memset(o, 0, sizeof *o); o->me = o->set = -1; }   /* (tournaments: none without online play) */
void net_tourney_peek(NetTourney *o) { net_tourney_get(o); }
void net_tourney_look(int tid) { (void)tid; }
void net_tourney_create(const char *name, int bo, int bo_top8, int bo_finals, int bans, int stocks, int noshow_min) { (void)name; (void)bo; (void)bo_top8; (void)bo_finals; (void)bans; (void)stocks; (void)noshow_min; }
void net_tourney_simple(int op, int tid) { (void)op; (void)tid; }
void net_tourney_seed_mode(int tid, int mode) { (void)tid; (void)mode; }
void net_tourney_move(int tid, int from, int to) { (void)tid; (void)from; (void)to; }
void net_tourney_dq(int tid, int entrant) { (void)tid; (void)entrant; }
void net_tourney_result(int tid, int set, int winner) { (void)tid; (void)set; (void)winner; }
void net_tourney_stage(int tid, int set, const char *stage) { (void)tid; (void)set; (void)stage; }
const char *net_tourney_stage_name(const char *stage) { return stage; }
int net_tourney_queued(void) { return 0; }
int net_tourney_turn(void) { return 0; }
void net_tourney_ready(uint64_t chara, int confirm) { (void)chara; (void)confirm; }
void net_tourney_unready(void) {}
int net_tourney_match(char *stage, int n, int *stocks) { (void)stage; (void)n; (void)stocks; return 0; }
int net_account_code(char *out, int n) { if (n > 0) out[0] = 0; return 0; }      /* (accounts: none without online play) */
int net_account_use_code(const char *code) { (void)code; return 0; }
const char *net_online_name(void) { return ""; }
void net_arena_get(NetArena *o) { memset(o, 0, sizeof *o); }            /* (arena: nothing without online play) */
void net_arena_join(int in) { (void)in; }
int net_arena_turn(void) { return 0; }
int net_arena_queued(void) { return 0; }
void net_arena_create(void) {}
void net_arena_close(void) {}
void net_arena_kick(const char *name) { (void)name; }
