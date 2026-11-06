/* Network stack exclusions, register preservation and safe waits on restore. No game files needed.
 * Linux: clang -O0 -ffunction-sections -fdata-sections -Iruntime/plat -Iruntime \
 *   tests/rb_network_stack_test.c runtime/rollback/model/threads.c runtime/rollback/model/runahead.c \
 *   -Wl,--gc-sections -o /tmp/opencode/rb_network_stack_test
 * Run normally, with --strict (SSBU_RB_NET_THREAD_WAIT=0, SSBU_RB_WORKER_WAIT=0, SSBU_RB_THREAD_WAIT=0), and with --full
 * (SSBU_RB_NET_THREAD_EXCLUDE=0: the network threads are fully rewound like Windows).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include "rollback/model/game_state_internal.h"
#include "rollback/rb_park.h"

FILE *g_log;
uint8_t *g_win1;
uint64_t g_wn1;
static int marked;
static const uint64_t stack_lo = 0x7f88be628000ull, stack_size = 0x4000;
static double clock_ms;
static int park_state, mutexes, let_go;
static int full_mode;

double now_ms(void) { return clock_ms++; }
void rb_park_request(void) {}
int rb_park_parked(Ctx *c) { (void)c; return park_state; }
void rb_park_let_go(Ctx *c) { (void)c; let_go++; park_state = RB_PARK_RUNNING; }
int os_mutexes_held_by(Ctx *c) { (void)c; return mutexes; }

int os_thread_is_network_name(const char *name) {
    return name && (strstr(name, "Pia") || strstr(name, "NetSystem"));
}
int os_thread_guest_stack(HANDLE h, uint64_t *lo, uint64_t *hi) {
    if (!h) return 0;
    *lo = stack_lo; *hi = stack_lo + stack_size;
    return 1;
}
void r1_nosim(uint64_t a, uint64_t n) {
    assert(a == stack_lo);
    assert(n == stack_size);
    marked++;
}
static void check_wait(const char *name, int state, int same, int held_mutex, int running, int strict) {
    static const char saved_wait[] = "SleepThread", other_wait[] = "WaitEvent";
    Ctx c = {0};
    Th *t = &g_guest_threads.table[0];
    memset(t, 0, sizeof *t);
    t->c = &c; t->name = name; t->parked = 1; t->from = 0x100; t->imp = saved_wait;
    c.in_import = running ? NULL : same ? saved_wait : other_wait;
    c.import_from = same ? t->from : 0x200;
    g_guest_threads.count = 1;
    clock_ms = 0; park_state = state; mutexes = held_mutex; let_go = 0;
    ra_hold_restore();
    int net = !full_mode && os_thread_is_network_name(name);
    int worker = name && strstr(name, "TaskWorker");
    int relaxed_net = !strict && (net || worker) && !held_mutex && !running;   /* network / worker: taken at another wait, mismatch stays 0 */
    int relaxed_any = !strict && !relaxed_net && !held_mutex && !running;     /* any other thread: taken too, counted as a mismatch */
    assert(t->held);
    assert(t->same == (same && !running));
    assert(g_guest_threads.mismatch == (!same && !relaxed_net));
    assert(g_guest_threads.running == running);
    if ((relaxed_net || relaxed_any) && !same) {
        assert(clock_ms >= 5 && clock_ms < 30);
        assert(let_go == 0);                              /* a parked thread is kept, not run to revisit an old wait */
    }
}
int main(int argc, char **argv) {
    int strict = argc > 1 && !strcmp(argv[1], "--strict");
    int full = argc > 1 && !strcmp(argv[1], "--full");
    full_mode = full;
    setenv("SSBU_RB_NET_THREAD_EXCLUDE", full ? "0" : "1", 1);
    setenv("SSBU_RB_NET_THREAD_WAIT", strict ? "0" : "1", 1);
    setenv("SSBU_RB_WORKER_WAIT", strict ? "0" : "1", 1);
    setenv("SSBU_RB_THREAD_WAIT", strict ? "0" : "1", 1);
    Ctx ctx[4] = {0};
    g_log = stdout;
    g_guest_threads.count = 4;
    g_guest_threads.table[0].name = "Pia ReceiveThreadStream";
    g_guest_threads.table[0].h = (HANDLE)(uintptr_t)1;
    g_guest_threads.table[1].name = "NetSystem";
    g_guest_threads.table[1].h = (HANDLE)(uintptr_t)2;
    g_guest_threads.table[2].name = "TaskWorker1";
    g_guest_threads.table[2].h = (HANDLE)(uintptr_t)3;
    g_guest_threads.table[3].name = "Pia SendThreadStream";  /* no stack: skipped */
    mark_network_threads();                              /* before the base maps exist */
    g_win1 = (uint8_t *)(uintptr_t)(stack_lo - PG);
    g_wn1 = stack_size + 2 * PG;
    mark_network_threads();                              /* after the base, then at subsequent captures */
    mark_network_threads();
    assert(marked == (full ? 0 : 6));
    for (int i = 0; i < 4; i++) {
        Th *t = &g_guest_threads.table[i];
        t->c = &ctx[i]; t->same = 1;
        t->c->x[19] = 7; t->regs.x[19] = 42;
    }
    restore_thread_regs();
    if (full) assert(ctx[0].x[19] == 42 && ctx[1].x[19] == 42 && ctx[3].x[19] == 42);   /* network threads rewind too */
    else assert(ctx[0].x[19] == 7 && ctx[1].x[19] == 7 && ctx[3].x[19] == 7);
    assert(ctx[2].x[19] == 42);                            /* simulation threads still rewind */
    check_wait("NetSystem", RB_PARK_HELD, 0, 0, 0, strict);
    check_wait("Pia ReceiveThreadStream", RB_PARK_RUNNING, 0, 0, 0, strict);
    check_wait("TaskWorker1", RB_PARK_HELD, 0, 0, 0, strict);
    check_wait("TaskWorker1", RB_PARK_RUNNING, 1, 0, 0, strict);
    check_wait("TaskWorker1", RB_PARK_MUTEX, 0, 1, 0, strict);
    check_wait("TaskWorker2", RB_PARK_RUNNING, 0, 0, 1, strict);
    check_wait("NetSystem", RB_PARK_HELD, 1, 0, 0, strict);
    check_wait("NetSystem", RB_PARK_MUTEX, 0, 1, 0, strict);
    check_wait("NetSystem", RB_PARK_RUNNING, 0, 0, 1, strict);
    /* (2026-11-03) any other thread whose wait moved (a render/service thread): taken at another wait after 5 ms,
     * registers not restored, and it counts as a mismatch (which no longer refuses the load). */
    check_wait("render dispatch", RB_PARK_HELD, 0, 0, 0, strict);
    check_wait("render dispatch", RB_PARK_HELD, 1, 0, 0, strict);
    check_wait("render dispatch", RB_PARK_RUNNING, 0, 0, 1, strict);
    check_wait("render dispatch", RB_PARK_MUTEX, 0, 1, 0, strict);
    puts("network stack exclusion: ok");
    return 0;
}
