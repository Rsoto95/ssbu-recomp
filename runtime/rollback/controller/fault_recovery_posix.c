/* FAULT RECOVERY on Linux: the portable half of controller/fault_recovery.c. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Windows file absorbs a fault in a leaf memory helper with SEH and marks the pass tainted; the ring then reloads the
 * newest healthy state at the safe point. On POSIX the same taint comes from a SIGSEGV/SIGBUS handler (the lifted host code
 * is x86-64, so the instruction forms are the same; rb_fault_try_absorb below), from the unwinder (nnunwind.c delivers a
 * throw through the host stack instead of terminating), from a repaired x29, or from a runtime call that refused a bad
 * request. The reload loop is the same. A tainted pass that faults again is cut back to the innermost nro_catch_call frame
 * of the same host stack (nnunwind_bail_context_posix), or keeps absorbing up to the runaway limit when there is none. */
#include "../model/game_state_internal.h"

int ra_fight_on(void);

static volatile LONG g_fr_pending;                      /* a fault was absorbed since the last safe point */
static volatile LONG g_fr_in_pass;                      /* faults absorbed in the pass that is running */
static volatile LONG64 g_fr_total, g_fr_recoveries, g_fr_giveups, g_fr_not_absorbable;
static volatile LONG g_fr_depth;                        /* consecutive recoveries: how far back the next one goes */
static long long g_fr_tainted_pass = -1;               /* the pass whose faults started the walk back */
static volatile LONG g_fr_off;                          /* the net let go: faults end the game again */
static volatile LONG g_fr_contain;                      /* the online match is being left: faults are absorbed, not fatal */
static volatile LONG g_fr_lines;
static int g_fr_enabled = -1;

#define FR_BAIL 256                                     /* past this many faults in one pass a fault cuts the cascade (see rb_fault_try_absorb) instead of being absorbed */
#define FR_MAX_PER_PASS 4096                            /* a bad object makes the code read through it and fault again and again: absorb the whole cascade so the thread still reaches
                                                         * its safe point, where the pass is reloaded once from the last healthy state (the reload, not read-by-read patching, is what fixes the object). More than this in one pass is a true runaway: give up. */
#define FR_MAX_RECOVERIES 400                           /* in one run: a fight that needs more than this is not being saved by it */

static int fr_enabled(void) {
    if (g_fr_enabled < 0) { const char *e = getenv("SSBU_RB_RECOVER"); g_fr_enabled = !(e && e[0] == '0'); }
    return g_fr_enabled && g_rg_ready && ra_fight_on() && (g_fr_contain || !g_fr_off);
}

/* The marks of an absorbed fault. Returns 0 when the net must not absorb this one. */
/* shortcut: network heap objects still rewind; exclude them if safe-point recovery cannot stabilize online play. */
static int fr_note(const char *what, uint64_t where_, uint64_t addr) {
    if (InterlockedIncrement(&g_fr_in_pass) > FR_MAX_PER_PASS) { InterlockedExchange(&g_fr_off, 1); InterlockedIncrement64(&g_fr_giveups); LOG("r1: FAULT RECOVERY: more than %d faults in one pass: the net lets go\n", FR_MAX_PER_PASS); return 0; }
    { void rg_dump_recent_loads(const char *why); if (g_fr_in_pass == 1 && g_fr_total < 20) rg_dump_recent_loads("a fault in this pass (before the safety net acts)"); }
    if (g_fr_in_pass == 1 && g_fr_total < 20) {           /* the fiber the faulting thread is on: born inside a rolled-back window? its guest stack, and the faulting address: saved by the ring? */
        extern uint64_t os_fiber_cur_key(void); extern int os_fiber_stack(uint64_t key, uint64_t *lo, uint64_t *hi); extern const char *rb_born_info(uint64_t key, char *buf, size_t n); const char *r1_addr_class(uint64_t a);
        uint64_t fk = os_fiber_cur_key(), lo = 0, hi = 0; char bi[160];
        int has = os_fiber_stack(fk, &lo, &hi);
        LOG("r1: FAULT RECOVERY: the fault (%s, address %llx: %s) is on fiber %llx (%s)%s\n", what, (unsigned long long)addr, addr >= 0x10000 ? r1_addr_class(addr) : "a small number, not memory",
            (unsigned long long)fk, fk ? rb_born_info(fk, bi, sizeof bi) : "the thread's own context", has ? "" : "");
        if (has) LOG("r1: FAULT RECOVERY:   that fiber's guest stack %llx..%llx: low end %s, high end %s\n", (unsigned long long)lo, (unsigned long long)hi, r1_addr_class(lo), r1_addr_class(hi - 8));
    }
    InterlockedIncrement64(&g_fr_total); InterlockedExchange(&g_fr_pending, 1);
    if (InterlockedIncrement(&g_fr_lines) <= 40)
        LOG("r1: FAULT RECOVERY: absorbed fault #%lld (%s) at %llx, address %llx, pass %lld%s\n", (long long)g_fr_total, what, (unsigned long long)where_, (unsigned long long)addr, g_rgs_next, g_ra_phase == 2 ? " RE-RUN" : "");
    return 1;
}

/* ---------------------------------------------------------------- SIGSEGV/ucontext: a fault in a leaf memory helper or a plain move of the lifted code
 * The lifted host code is x86-64 on Linux too, so the Windows instruction forms are the same; only the calling convention of the tiny
 * helpers differs (System V: the address is in rdi, not rcx), and fr_emulate parses the ModRM, so it does not care. A leaf helper is
 * "one memory move + ret": the register file is left as if the helper returned 0 (a load) or did nothing (a store), and execution
 * resumes after the helper's ret. A move inside a bigger function is stepped over the same way. Anything else is refused: the pass
 * then ends as before. */
#include <signal.h>
#include <ucontext.h>

enum { G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15, G_RDI, G_RSI, G_RBP, G_RBX, G_RDX, G_RAX, G_RCX, G_RSP, G_RIP, G_EFL, G_CSGSFS, G_ERR };

typedef struct { uint64_t g[16]; uint8_t xmm[16][16]; uint64_t rip; } FrRegs;
static void fr_regs_load(FrRegs *r, ucontext_t *u) {
    greg_t *g = u->uc_mcontext.gregs;
    r->g[0] = (uint64_t)g[G_RAX]; r->g[1] = (uint64_t)g[G_RCX]; r->g[2] = (uint64_t)g[G_RDX]; r->g[3] = (uint64_t)g[G_RBX];
    r->g[4] = (uint64_t)g[G_RSP]; r->g[5] = (uint64_t)g[G_RBP]; r->g[6] = (uint64_t)g[G_RSI]; r->g[7] = (uint64_t)g[G_RDI];
    r->g[8] = (uint64_t)g[G_R8]; r->g[9] = (uint64_t)g[G_R9]; r->g[10] = (uint64_t)g[G_R10]; r->g[11] = (uint64_t)g[G_R11];
    r->g[12] = (uint64_t)g[G_R12]; r->g[13] = (uint64_t)g[G_R13]; r->g[14] = (uint64_t)g[G_R14]; r->g[15] = (uint64_t)g[G_R15];
    if (u->uc_mcontext.fpregs) memcpy(r->xmm, u->uc_mcontext.fpregs->_xmm, sizeof r->xmm); else memset(r->xmm, 0, sizeof r->xmm);
    r->rip = (uint64_t)g[G_RIP];
}
static void fr_regs_store(const FrRegs *r, ucontext_t *u) {
    greg_t *g = u->uc_mcontext.gregs;
    g[G_RAX] = (greg_t)r->g[0]; g[G_RCX] = (greg_t)r->g[1]; g[G_RDX] = (greg_t)r->g[2]; g[G_RBX] = (greg_t)r->g[3];
    g[G_RSP] = (greg_t)r->g[4]; g[G_RBP] = (greg_t)r->g[5]; g[G_RSI] = (greg_t)r->g[6]; g[G_RDI] = (greg_t)r->g[7];
    g[G_R8] = (greg_t)r->g[8]; g[G_R9] = (greg_t)r->g[9]; g[G_R10] = (greg_t)r->g[10]; g[G_R11] = (greg_t)r->g[11];
    g[G_R12] = (greg_t)r->g[12]; g[G_R13] = (greg_t)r->g[13]; g[G_R14] = (greg_t)r->g[14]; g[G_R15] = (greg_t)r->g[15];
    if (u->uc_mcontext.fpregs) memcpy(u->uc_mcontext.fpregs->_xmm, r->xmm, sizeof r->xmm);
    g[G_RIP] = (greg_t)r->rip;
}

/* EMULATION of a plain memory move that faulted (the same forms as controller/fault_recovery.c): a load gives 0 (the destination
 * register cleared), a store is dropped, execution goes on after the instruction. Works on a copy of the registers; 1 = understood. */
static int fr_emulate(FrRegs *r) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)r->rip, *q = p;
    int opsz16 = 0, rep = 0, rex = 0;
    for (; ; q++) {
        if (*q == 0x66) opsz16 = 1;
        else if (*q == 0xF3 || *q == 0xF2) rep = *q;
        else if (*q == 0x2E || *q == 0x36 || *q == 0x3E || *q == 0x26 || *q == 0x64 || *q == 0x65 || *q == 0xF0 || *q == 0x67) return 0;      /* segment overrides, lock, address-size: not understood */
        else break;
    }
    if ((*q & 0xF0) == 0x40) rex = *q++;
    int W = (rex & 8) != 0, R = (rex & 4) ? 8 : 0;
    uint8_t op = *q++; int op2 = -1;
    if (op == 0x0F) op2 = *q++;
    enum { K_NONE, K_LOAD, K_STORE, K_XMM } kind = K_NONE; int width = 0, imm = 0;
    int gw = W ? 64 : opsz16 ? 16 : 32;
    if (op2 < 0) {
        switch (op) {
        case 0x8A: kind = K_LOAD; width = 8; break;
        case 0x8B: kind = K_LOAD; width = gw; break;
        case 0x63: kind = K_LOAD; width = W ? 64 : 32; break;
        case 0x88: case 0x89: kind = K_STORE; break;
        case 0xC6: kind = K_STORE; imm = 1; break;
        case 0xC7: kind = K_STORE; imm = opsz16 ? 2 : 4; break;
        default: return 0;
        }
    } else {
        switch (op2) {
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: kind = K_LOAD; width = gw; break;
        case 0x10: case 0x28: case 0x6F: kind = K_XMM; break;
        case 0x7E: kind = rep == 0xF3 ? K_XMM : K_STORE; break;
        case 0x11: case 0x29: case 0x7F: case 0xD6: kind = K_STORE; break;
        default: return 0;
        }
    }
    uint8_t modrm = *q++; int mod = modrm >> 6, reg = ((modrm >> 3) & 7) | R, rm = modrm & 7;
    if (mod == 3) return 0;                                  /* no memory operand: this is not the instruction that faulted */
    if ((op == 0xC6 || op == 0xC7) && op2 < 0 && (reg & 7) != 0) return 0;
    if (rm == 4) { uint8_t sib = *q++; if (mod == 0 && (sib & 7) == 5) q += 4; }
    if (mod == 1) q += 1; else if (mod == 2) q += 4; else if (mod == 0 && rm == 5) q += 4;
    q += imm;
    int len = (int)(q - p); if (len < 2 || len > 15) return 0;
    if (kind == K_LOAD) {
        if (reg == 4) return 0;                              /* never clear rsp */
        if (width == 8) { if (!rex && reg >= 4 && reg < 8) r->g[reg - 4] &= ~(uint64_t)0xFF00; else r->g[reg] &= ~(uint64_t)0xFF; }
        else if (width == 16) r->g[reg] &= ~(uint64_t)0xFFFF;
        else r->g[reg] = 0;
    } else if (kind == K_XMM) memset(r->xmm[reg & 15], 0, 16);
    r->rip += (uint64_t)len;
    return 1;
}

/* TEST ONLY (SSBU_RB_FAULT_TEST): the deliberate faults of rb_fault_test_tick are in this file, not in the game's code; this flag lets
 * the signal handler absorb them like one of the game's own. */
static volatile LONG g_fr_testing;

/* Called by hostrt.c's SIGNAL handler before its report. 1 = absorbed, the thread continues at the adjusted context. */
int rb_fault_try_absorb(void *uc, void *addr) {
    ucontext_t *u = uc;
    if (!fr_enabled()) return 0;
    greg_t *g = u->uc_mcontext.gregs;
    uintptr_t ip = (uintptr_t)g[G_RIP];
    if (!g_fr_testing && guest_function_of(ip) == 0) { InterlockedIncrement64(&g_fr_not_absorbable); LOG("r1: FAULT RECOVERY: a fault at %p is outside the game's code: not absorbable\n", (void *)ip); return 0; }
    int noted = 0;
    if (!g_fr_testing && (g_fr_pending || g_fr_in_pass >= FR_BAIL)) {   /* the pass is already tainted: go back to a catch frame instead of stepping over more of the garbage */
        if (!fr_note("cut (pass already tainted)", ip, (uint64_t)(uintptr_t)addr)) return 0;
        noted = 1;
        { extern int nnunwind_bail_context_posix(void *uc); if (nnunwind_bail_context_posix(u)) return 1; }
    }
    FrRegs r;
    fr_regs_load(&r, u);
    const uint8_t *p = (const uint8_t *)ip;
    if (!fr_emulate(&r)) {
        InterlockedIncrement64(&g_fr_not_absorbable);
        LOG("r1: FAULT RECOVERY: a fault at %p is not a plain move: not absorbable (bytes %02x %02x %02x %02x %02x %02x %02x %02x)\n", (void *)ip, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        return 0;
    }
    int len = (int)(r.rip - ip);
    if (len >= 2 && len <= 15 && p[len] == 0xC3) {          /* a leaf helper: return 0 (a load) or do nothing (a store) */
        uint64_t sp = r.g[4];
        if (sp & 7) return 0;
        if (!noted && !fr_note("leaf helper", ip, (uint64_t)(uintptr_t)addr)) return 0;
        r.g[0] = 0; memset(r.xmm[0], 0, 16);
        r.g[4] = sp + 8; r.rip = *(uint64_t *)(uintptr_t)sp;   /* the helper returns */
    } else if (!noted && !fr_note("mov stepped over", ip, (uint64_t)(uintptr_t)addr)) return 0;
    fr_regs_store(&r, u);
    return 1;
}

/* call_indirect on an address that is not a function (hostrt.c): skip the call, x0 = 0. */
int rb_fault_absorb_icall(Ctx *c, uint64_t addr) {
    if (!fr_enabled()) return 0;
    if (!fr_note("indirect call", c->x[30] - 4, addr)) return 0;
    c->x[0] = 0;
    return 1;
}

/* nnunwind.c: a throw whose guest frames cannot be walked is delivered to its handler through the host stack instead of ending the game. The pass is marked tainted. */
int rb_fault_absorb_unwind(Ctx *c) {
    if (!fr_enabled()) return 0;
    return fr_note("exception unwinder", c->x[30] - 4, 0);
}

/* nnunwind.c step(): a frame whose x29 was wrong was walked from sp instead. The state is suspect, so the pass is marked like an absorbed fault. */
/* (does not spend the pass's fault budget: an unwind walks many frames and may repair several; it only taints the pass so it is reloaded) */
void rb_fault_note_repair(uint64_t where_) {
    if (!fr_enabled()) return;
    InterlockedIncrement64(&g_fr_total); InterlockedExchange(&g_fr_pending, 1);
    if (InterlockedIncrement(&g_fr_lines) <= 40) LOG("r1: FAULT RECOVERY: x29 repaired from sp at %llx, pass %lld%s\n", (unsigned long long)where_, g_rgs_next, g_ra_phase == 2 ? " RE-RUN" : "");
}
/* os_threads.c and other runtime code: a bad request refused there taints the pass like an absorbed fault */
void rb_fault_note_external(const char *what, uint64_t where_, uint64_t addr) { if (fr_enabled()) fr_note(what, where_, addr); }
int rb_fault_net_active(void) { return fr_enabled(); }
int rb_fault_pending(void) { return g_fr_pending != 0; }

/* The safe point of a pass that ended TAINTED: `i` is the pass, the caller has not captured it. Loads the newest healthy state older than `i` (further back each time the
 * same pass faults again) and tells the caller to run the passes again. 1 = loaded (the caller sets up the re-run: *best_out is the frame loaded), 0 = the net let go. */
int rb_fault_recover(long long i, Ctx *c, long long *best_out) {
    InterlockedExchange(&g_fr_pending, 0); InterlockedExchange(&g_fr_in_pass, 0);
    LONG depth = InterlockedIncrement(&g_fr_depth);
    if (i > g_fr_tainted_pass) g_fr_tainted_pass = i;
    if (g_fr_recoveries >= FR_MAX_RECOVERIES) { InterlockedExchange(&g_fr_off, 1); InterlockedIncrement64(&g_fr_giveups); LOG("r1: FAULT RECOVERY: %d recoveries in this run: the net lets go\n", FR_MAX_RECOVERIES); return 0; }
    long long target = i - depth, best = -1;
    best = rg_best_state(target);                          /* the newest QUIET state at or before target (snapshot_ring.c) */
    if (best < 0) { InterlockedExchange(&g_fr_off, 1); InterlockedIncrement64(&g_fr_giveups); LOG("r1: FAULT RECOVERY: pass %lld faulted %ld times in a row and the ring has no older state: the net lets go\n", i, (long)depth); return 0; }
    g_rg_keep = 0;
    double t0 = now_ms();
    int lr = 0;
    for (int tries = 0; tries < 60; tries++) {          /* r1_ring_load returns -1 when a thread is still running or an audio thread holds a mutex: transient, clears within a frame. Only 0 (frame not in the ring) is final. */
        lr = r1_ring_load(best, c);
        if (lr != -1) break;
        Sleep(1);
    }
    if (lr <= 0) { InterlockedExchange(&g_fr_off, 1); InterlockedIncrement64(&g_fr_giveups); LOG("r1: FAULT RECOVERY: loading state %lld %s: the net lets go\n", best, lr == 0 ? "not in the ring" : "still refused after 60 ms"); return 0; }
    InterlockedIncrement64(&g_fr_recoveries);
    LOG("r1: FAULT RECOVERY: pass %lld was tainted: state of pass %lld loaded (%ld back) in %.2f ms, passes %lld .. %lld run again (recovery #%lld, %lld faults absorbed in all)\n",
        i, best, (long)depth, now_ms() - t0, best + 1, i, (long long)g_fr_recoveries, (long long)g_fr_total);
    *best_out = best;
    return 1;
}

/* The net lets go for the rest of the run (no state to go back to): the pending mark is dropped and faults end the game as before. */
void rb_fault_give_up(void) { InterlockedExchange(&g_fr_pending, 0); InterlockedExchange(&g_fr_off, 1); InterlockedIncrement64(&g_fr_giveups); }

/* The online match is being left (rollback_controller.c): keep absorbing the bad calls until player 1 walks off, instead of ending the process. */
void rb_fault_contain(void) { InterlockedExchange(&g_fr_contain, 1); }
void rb_fault_contain_off(void) { InterlockedExchange(&g_fr_contain, 0); }

/* A new match: the ring was rebased, so the safety net starts fresh too (a give-up in one match must not end the next one). */
void rb_fault_match_start(void) {
    InterlockedExchange(&g_fr_contain, 0); InterlockedExchange(&g_fr_off, 0); InterlockedExchange(&g_fr_depth, 0);
    InterlockedExchange(&g_fr_in_pass, 0); InterlockedExchange(&g_fr_pending, 0); g_fr_tainted_pass = -1;
}

/* A pass `i` ended with no fault. The walk back (how far the next recovery goes) starts from the newest state again only once the pass that FAULTED has come through clean: a clean pass before
 * it is not news (a dense run, 2026-10-28: the walk went 1 back, 2 back, 1 back... 400 times at the same pass, because the clean pass in between reset it, and never reached an older state). */
void rb_fault_pass_clean(long long i) {
    if (!g_fr_pending && g_fr_depth && i >= g_fr_tainted_pass) { InterlockedExchange(&g_fr_depth, 0); g_fr_tainted_pass = -1; }
    InterlockedExchange(&g_fr_in_pass, 0);
}

void rb_fault_report(void) {
    rg_quiet_report();
    if (g_fr_total || g_fr_not_absorbable || g_fr_giveups)
        LOG("r1: FAULT RECOVERY so far: %lld faults absorbed, %lld recoveries, %lld not absorbable, %lld times the net let go%s\n", (long long)g_fr_total, (long long)g_fr_recoveries, (long long)g_fr_not_absorbable, (long long)g_fr_giveups, g_fr_contain ? " (contained until the match ends)" : g_fr_off ? " (OFF now)" : "");
}

/* SSBU_RB_FAULT_TEST: a leaf read of an address that is not mapped, the same shape as the lifted code's rd8. */
/* The address comes from a volatile variable: with a constant argument the compiler specialises the function (mov al,[0x20], no ret) and the fault is not the shape of rd8 any more. */
__declspec(noinline) static uint8_t fr_test_leaf_read(const uint8_t *p) { return *p; }
static volatile uintptr_t g_fr_test_addr = 0x20;
void rb_fault_test_tick(void) {
    static long long every = -1, n;
    if (every < 0) { const char *e = getenv("SSBU_RB_FAULT_TEST"); every = e ? atoll(e) : 0; }
    if (every > 0 && g_ra_phase == 0 && ++n % every == 0) {
        static int turn;
        InterlockedExchange(&g_fr_testing, 1);
        switch (turn++ % 3) {                           /* the three shapes the net understands: a leaf helper, a read inside a bigger function, a write inside a bigger function */
        case 0: { volatile uint8_t v = fr_test_leaf_read((const uint8_t *)g_fr_test_addr); (void)v; break; }
        case 1: { volatile uint32_t *p = (volatile uint32_t *)g_fr_test_addr; volatile uint32_t v = *p; (void)v; break; }
        default: { volatile uint64_t *p = (volatile uint64_t *)g_fr_test_addr; *p = 5; break; }
        }
        InterlockedExchange(&g_fr_testing, 0);
    }
}
