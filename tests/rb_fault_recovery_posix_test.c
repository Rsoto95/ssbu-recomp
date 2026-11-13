/* Linux fault recovery: a network-thread fault still taints the pass for a safe-point reload.
 * No signals or game files: pass a synthetic x86-64 ucontext to the real handler.
 * clang -O0 -ffunction-sections -fdata-sections -Iruntime/plat -Iruntime \
 *   tests/rb_fault_recovery_posix_test.c runtime/rollback/controller/fault_recovery_posix.c \
 *   -Wl,--gc-sections -o /tmp/opencode/rb_fault_recovery_posix_test
 * Run once normally and once with --off (SSBU_RB_RECOVER=0).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include <ucontext.h>
#include "rollback/model/game_state_internal.h"

FILE *g_log;
int g_rg_ready, g_rg_keep, g_ra_phase;
long long g_rgs_next = 2;
static int fighting = 1, in_game = 1, loads;

int ra_fight_on(void) { return fighting; }
uint64_t guest_function_of(uint64_t ip) { (void)ip; return in_game; }
DWORD GetCurrentThreadId(void) { return 1; }
const char *guest_thread_name_of(DWORD tid) { (void)tid; return "NetSystem"; }
int os_thread_is_network_name(const char *s) { return !strcmp(s, "NetSystem"); }
void rg_dump_recent_loads(const char *why) { (void)why; }
uint64_t os_fiber_cur_key(void) { return 0; }
int os_fiber_stack(uint64_t key, uint64_t *lo, uint64_t *hi) { (void)key; (void)lo; (void)hi; return 0; }
const char *rb_born_info(uint64_t key, char *buf, size_t n) { (void)key; (void)buf; (void)n; return "test"; }
const char *r1_addr_class(uint64_t a) { (void)a; return "test"; }
int nnunwind_bail_context_posix(void *uc) { (void)uc; return 0; }
long long rg_best_state(long long target) { return target; }
int r1_ring_load(long long frame, Ctx *c) { (void)c; assert(frame == 1); loads++; return 1; }
double now_ms(void) { return 0; }
void Sleep(DWORD ms) { (void)ms; }
int rb_fault_try_absorb(void *uc, void *addr);
void rb_fault_note_repair(uint64_t where_);

static void recover(void) {
    Ctx c = {0}; long long best = -1;
    assert(rb_fault_pending());
    assert(rb_fault_recover(2, &c, &best));
    assert(best == 1 && !rb_fault_pending());
    rb_fault_pass_clean(2);
}
int main(int argc, char **argv) {
    int off = argc > 1 && !strcmp(argv[1], "--off");
    setenv("SSBU_RB_RECOVER", off ? "0" : "1", 1);
    g_log = stdout;
    static const uint8_t mov[] = {0x48, 0x8b, 0x40, 0x08, 0x90, 0x90, 0x90, 0x90}; /* mov rax,[rax+8] */
    static const uint8_t nop[8] = {0x90};
    ucontext_t u = {0};
    u.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)mov;
    assert(!rb_fault_try_absorb(&u, (void *)8));             /* no ring */
    g_rg_ready = 1; fighting = 0;
    assert(!rb_fault_try_absorb(&u, (void *)8));             /* outside a fight */
    fighting = 1;
    if (off) {
        assert(!rb_fault_try_absorb(&u, (void *)8));
        rb_fault_note_repair(0x100);
        assert(!rb_fault_pending());
    } else {
        assert(rb_fault_try_absorb(&u, (void *)8));
        assert(u.uc_mcontext.gregs[REG_RIP] == (greg_t)(uintptr_t)(mov + 4));
        assert(u.uc_mcontext.gregs[REG_RAX] == 0);
        recover();
        rb_fault_note_repair(0x100);
        recover();
        assert(loads == 2);
        u.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)mov;
        in_game = 0;
        assert(!rb_fault_try_absorb(&u, (void *)8));         /* host faults remain fatal */
        in_game = 1; u.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)nop;
        assert(!rb_fault_try_absorb(&u, (void *)8));         /* unsupported instructions remain fatal */
        assert(!rb_fault_pending());
        rb_fault_give_up();
        u.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)mov;
        assert(!rb_fault_try_absorb(&u, (void *)8));
        rb_fault_contain();                                  /* the online match is being left: the same fault is absorbed, not fatal */
        u.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)mov;
        assert(rb_fault_try_absorb(&u, (void *)8));
        assert(u.uc_mcontext.gregs[REG_RIP] == (greg_t)(uintptr_t)(mov + 4));
        rb_fault_contain_off();
        rb_fault_give_up();
        u.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)mov;
        assert(!rb_fault_try_absorb(&u, (void *)8));
        rb_fault_match_start();                              /* a fresh match: the safety net is on again */
        u.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)mov;
        assert(rb_fault_try_absorb(&u, (void *)8));
    }
    puts("POSIX network fault recovery: ok");
    return 0;
}
