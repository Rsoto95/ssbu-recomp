/* Ranked ladder (lobby 1.5, tools/net/ladder.py): the result of an online rollback match, handed to net_relay.c (RESULT)
 * when the fight is over. Both games send theirs; the lobby counts the game only when the two agree, so what is sent must
 * be the same on both: the state of the last FINAL frame of the fight.
 *  - net_session.c rbn_pass_hash, after every pass (re-run passes included): the stocks of both fighters (their
 *    information records) and the damage the fighters' frame line noted in that pass (fighter_host.c, the game's own
 *    getter), kept per frame; a re-run pass of a frame replaces what the pass on a guess kept;
 *  - net_session.c rbn_finalize: a frame whose pads of both players are known and that no rollback can change any more is
 *    final: its sample becomes the result;
 *  - rollback_controller.c rbg_fight_off (an agreed rollback match is over): the result goes to the lobby.
 * Flags: a match that ended with both players still holding stocks before the time limit (someone left it) is marked
 * abandoned; one where the other game went silent (5 s) disconnected. The lobby counts neither.
 * SSBU_LADDER=0: nothing kept, nothing sent. No game content in this file. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net_stats.h"

int rbn_started(void);
int rbn_local_player(void);
int rbn_peer_gone(void);
uint64_t rbn_match_seed(void);
long long rbn_desyncs(void);
int dm_ladder_match(int *rule_kind, int *stock, uint64_t chara[2], uint32_t *stage);      /* debug_mode.c */
int dm_ladder_stocks(int entry);

#define LR_RING 512                                     /* frames kept (more than any rollback window) */
#define LR_TIME_LIMIT (7 * 60 * 60)                     /* the ladder's 7 minutes, in fight frames */
typedef struct { long long frame; int stocks[2]; float dmg[2]; } LrSample;
static int g_lr_on = -1;
static float g_lr_dmg[2];                               /* the damage noted by the pass running now */
static LrSample g_lr_ring[LR_RING], g_lr_last;
static int g_lr_have, g_lr_gone;

static int lr_on(void) {
    if (g_lr_on < 0) { const char *e = getenv("SSBU_LADDER"); g_lr_on = !(e && e[0] == '0'); }
    return g_lr_on;
}
int ladder_wants_damage(void) { return lr_on() && rbn_started(); }
/* fighter_host.c, the fighters' frame line of every pass: entry 0 / 1 = player 1 / 2 */
void ladder_note_damage(int entry, float dmg) { if (entry == 0 || entry == 1) g_lr_dmg[entry] = dmg; }
/* net_session.c rbn_pass_hash: pass f is over */
void ladder_pass(long long f) {
    if (f <= 0 || !lr_on() || !rbn_started()) return;
    LrSample *s = &g_lr_ring[f % LR_RING];
    s->frame = f;
    for (int k = 0; k < 2; k++) { s->stocks[k] = dm_ladder_stocks(k); s->dmg[k] = g_lr_dmg[k]; }
}
/* net_session.c rbn_finalize: frame f is final */
void ladder_final(long long f) {
    if (f <= 0 || !lr_on()) return;
    const LrSample *s = &g_lr_ring[f % LR_RING];
    if (s->frame == f && s->stocks[0] >= 0 && s->stocks[1] >= 0) { g_lr_last = *s; g_lr_have = 1; }
    if (rbn_peer_gone()) g_lr_gone = 1;
}
static int dmg10(float d) { return d <= 0 ? 0 : d >= 6553.5f ? 65535 : (int)(d * 10.0f + 0.5f); }
/* rollback_controller.c rbg_fight_off: an agreed rollback match is over */
void ladder_fight_off(void) {
    NetLadderResult r;
    int have = g_lr_have;
    LrSample s = g_lr_last;
    g_lr_have = 0;
    int gone = g_lr_gone;
    g_lr_gone = 0;
    memset(g_lr_ring, 0, sizeof g_lr_ring);
    memset(g_lr_dmg, 0, sizeof g_lr_dmg);
    if (!lr_on() || !have) return;
    {   int rbn_spectating(void); if (rbn_spectating()) return; }      /* (2026-11-02) a Battle Arena's spectator: not its match */
    memset(&r, 0, sizeof r);
    if (!dm_ladder_match(&r.rule_kind, &r.stock, r.chara, &r.stage)) return;   /* (not an agreed online match: never ranked) */
    r.seed = rbn_match_seed();
    r.slot = rbn_local_player();
    for (int k = 0; k < 2; k++) { r.stocks[k] = s.stocks[k]; r.dmg10[k] = dmg10(s.dmg[k]); }
    r.frames = (uint32_t)s.frame;
    r.desyncs = (uint32_t)rbn_desyncs();
    if (gone) r.flags |= NET_LADDER_F_DISCONNECT;
    if (s.stocks[0] > 0 && s.stocks[1] > 0 && s.frame < LR_TIME_LIMIT - 60) r.flags |= NET_LADDER_F_ABANDON;
    net_ladder_result(&r);
}
