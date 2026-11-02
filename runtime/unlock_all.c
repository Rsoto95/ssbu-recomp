/* unlock_all.c - "unlock every fighter and stage" for players who want to go straight to competitive play.
 *
 * graphics.ini `unlock_all = on` (SSBU_UNLOCK_ALL=1 / 0 wins): the game's questions "is this fighter available?" and "does
 * the save have this fighter / stage?" are answered "yes" for every base-roster fighter and every stage the save has not
 * unlocked yet, so the select screens and the rest of the game (the after-match challenger asks the save) agree. Nothing is
 * written into the save by this file: with the key off again the game is as before.
 *
 * The function, 0x7101797640 (13.0.4), found by the new-fighter roster work (mods\mods_n4_src\n4_roster.cpp, runs U3, U5,
 * U7): x0 = a ui_chara id (0xc1 << 56 | hash40("ui_chara_<name>"), sometimes with a database row in bits 40..47), w1 = a
 * mode, w2 = an option; the answer is bit 0 of w0. Every address here is a 13.0.4 one passed through GA(); their 13.0.5
 * counterparts are in hostrt.c g_code_13_0_5 (2026-10-30: each function the same size with every word equal, PC-relative
 * fields masked; the six save getters did not move).
 *
 * Downloadable fighters are left alone: owning one is what makes it available (build\config\dlc.txt, nnmisc.c), and a
 * "yes" for one whose files are not installed would put a fighter on the screen that cannot load.
 * SSBU_UNLOCK_LOG=1: one log line per new (thing, caller, save read in or not) question with the game's answer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);    /* fighter_sdk\hook_host.c */
const char *hook_error(void);

#define FN_AVAILABLE 0x7101797640ULL
#define FN_ROSTER    0x710326f0f0ULL     /* (out vector, w1..w6 options): builds the list of ui_chara ids a screen shows */
#define RA_MILESTONE 0x7101779854ULL     /* its call between the title and the main menu: the unlock milestones count that list (n4_roster.cpp, run U2) */
#define MASK40 0xffffffffffULL

/* The save object's "is it unlocked?" getters (seen in the 13.0.4 binary: six functions of one shape, 0x7100556ad0..
 * 0x7100557200, each x0 = the save object, x1 = an id with its kind in bits 56..63; w0 = 1 when the save accepts it). The
 * fighter one is what the availability check above asks (its default branch) and what the after-match code asks, so the
 * screens and the rest of the game agree.
 *
 * What a save "unlock" is, found 2026-10-29 while trying to write real ones (not done; kept for the next attempt): each kind
 * is a std::map in the save object (end node at save + 168 / 192 / 216 / 240 / 264 / 288, count 8 bytes after; 0x30-byte
 * nodes: links, colour, the id at +32, an int at +40) whose int indexes the kind's vector of 16-byte entries {id, state}
 * (begin at save + 16 + 24 * kind); the getter accepts a state when state == table value XOR key for the table at
 * **(0x7105332f68) (count +4, values +8, key +656). The save loader 0x7100557e00 builds them. Adding nodes and accepted
 * entries in memory worked for kinds 0x13 and 0x5a (the game showed its announcements and played on), but the save file the
 * game then wrote did not keep them: it is made from somewhere else. Kind 0xc4 kept the game on one screen after the title even
 * with such records. On the select screen a locked fighter is decided before the getter (0x7103265d40 reads the fighter
 * database), which is why fighters are answered by the availability hook as well. */
static struct { uint64_t addr; uint8_t tag; const char *what; ImportFn orig; } g_kind[] = {
    {0x7100556ad0ULL, 0xc1, "fighter", 0},      /* ui_chara ids */
    {0x7100556c40ULL, 0x69, "stage", 0},        /* ui_stage ids */
    {0x7100556db0ULL, 0x13, "kind 0x13", 0},
    {0x7100556f20ULL, 0x5a, "kind 0x5a", 0},
    {0x7100557090ULL, 0xc4, "kind 0xc4", 0},
    {0x7100557200ULL, 0x28, "kind 0x28", 0},
};
#define NKIND ((int)(sizeof g_kind / sizeof g_kind[0]))
/* Which kinds unlock_all answers "yes" for (bit i = g_kind[i]); SSBU_UNLOCK_KINDS=<hex> for tests. Fighters and stages: a
 * copy of a save with 23 fighters and 11 stages locked went through title, menus, stage select and three fights with results
 * (2026-10-29). The other four are NOT safe this way: each alone keeps the game from reaching the main menu after the title. */
static unsigned g_kinds = 3;

static ImportFn g_orig, g_orig_roster;
static int g_on, g_log, g_quiet;
static uint64_t g_dlc[24];
static int g_ndlc;

static uint64_t hash40(const char *s) {      /* as debug_mode.c: length << 32 | crc32 */
    uint32_t crc = 0xffffffffu; size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) { crc ^= (uint8_t)s[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ((uint64_t)n << 32) | (uint32_t)~crc;
}
static int is_dlc(uint64_t id) {
    for (int i = 0; i < g_ndlc; i++) if ((id & MASK40) == g_dlc[i]) return 1;
    return 0;
}

static void available_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t id = c->x[0], lr = c->x[30];
    uint32_t mode = (uint32_t)c->x[1];
    g_orig(c);
    int was = (int)(c->x[0] & 1), dlc = is_dlc(id);
    if (g_on && !g_quiet && !was && !dlc && (id >> 56) == 0xc1) c->x[0] = 1;
    if (g_log) {
        static struct { uint64_t h, lr; uint32_t mode; } seen[512]; static int nseen;
        int i; for (i = 0; i < nseen; i++) if (seen[i].h == (id & MASK40) && seen[i].lr == lr && seen[i].mode == mode) break;
        if (i == nseen && nseen < 512) {
            seen[nseen].h = id & MASK40; seen[nseen].lr = lr; seen[nseen].mode = mode; nseen++;
            printf("unlock_all: available? %llx mode %u from %llx: game %d -> %d%s\n", (unsigned long long)id, mode, (unsigned long long)lr,
                   was, (int)(c->x[0] & 1), dlc ? " (downloadable: left alone)" : "");
        }
    }
}

static void kind_hook(Ctx *c, void *user) {
    int k = (int)(intptr_t)user;
    uint64_t id = c->x[1], lr = c->x[30], save = c->x[0];
    g_kind[k].orig(c);
    int was = (int)(c->x[0] & 1), dlc = k == 0 && is_dlc(id);
    if (g_on && !g_quiet && (g_kinds >> k & 1) && !was && !dlc && (uint8_t)(id >> 56) == g_kind[k].tag) c->x[0] = 1;
    if (g_log) {
        static struct { uint64_t h, lr; int loaded; } seen[1024]; static int nseen;
        int loaded = *(volatile uint64_t *)(uintptr_t)(save + 176) != 0;      /* fighter records there = the save is read in (before: every answer is 0) */
        int i; for (i = 0; i < nseen; i++) if (seen[i].h == id && seen[i].lr == lr && seen[i].loaded == loaded) break;
        if (i == nseen && nseen < 1024) {
            seen[nseen].h = id; seen[nseen].lr = lr; seen[nseen].loaded = loaded; nseen++;
            printf("unlock_all: %s %llx in the save? from %llx: game %d -> %d%s%s\n", g_kind[k].what, (unsigned long long)id, (unsigned long long)lr,
                   was, (int)(c->x[0] & 1), dlc ? " (downloadable: left alone)" : "", loaded ? "" : " (save not read in yet)");
        }
    }
}

/* The milestone count gets the game's own answers, so no reward is paid for fighters the save has not unlocked and nothing
 * is written into the save. */
static void roster_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t out = c->x[0], lr = c->x[30];
    int quiet = lr == GA(RA_MILESTONE);
    g_quiet += quiet;
    g_orig_roster(c);
    g_quiet -= quiet;
    if (g_log) {
        uint64_t b = *(uint64_t *)(uintptr_t)out, e = *(uint64_t *)(uintptr_t)(out + 8);
        printf("unlock_all: roster list from %llx: %lld fighters%s\n", (unsigned long long)lr, (long long)((e - b) / 8), quiet ? " (milestone count: the game's own answers)" : "");
    }
}

void unlock_all_init(void) {
    const char *e = getenv("SSBU_UNLOCK_ALL"), *l = getenv("SSBU_UNLOCK_LOG");
    g_on = e && *e ? atoi(e) > 0 : gfx_unlock_all();
    g_log = l && atoi(l) > 0;
    if (!g_on && !g_log) return;
    static const char *const dlc[] = {"packun", "jack", "brave", "buddy", "dolly", "master", "tantan", "pickel", "edge", "eflame_first",
                                      "elight_first", "eflame_only", "elight_only", "demon", "trail"};      /* the downloadable fighters' ui_chara names */
    for (size_t i = 0; i < sizeof dlc / sizeof dlc[0]; i++) { char n[48]; snprintf(n, sizeof n, "ui_chara_%s", dlc[i]); g_dlc[g_ndlc++] = hash40(n); }
    const char *ks = getenv("SSBU_UNLOCK_KINDS");
    if (ks && *ks) g_kinds = (unsigned)strtoul(ks, NULL, 16);
    int ok = hook_install(GA(FN_AVAILABLE), available_hook, NULL, &g_orig) && hook_install(GA(FN_ROSTER), roster_hook, NULL, &g_orig_roster);
    for (int k = 0; k < NKIND && ok; k++) ok = hook_install(GA(g_kind[k].addr), kind_hook, (void *)(intptr_t)k, &g_kind[k].orig) != 0;
    printf("unlock_all: %s (kinds %x; hooks: %s)\n", g_on ? "on" : "logging only", g_kinds, ok ? "installed" : hook_error());
}
