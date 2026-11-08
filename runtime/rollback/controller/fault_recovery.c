/* FAULT RECOVERY: the safety net under the rollback. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Premise: a state the snapshot ring restored can still be wrong in a way nothing measures yet (an object that is not what the code believes), and the game then
 * reads through a bad pointer. Without this file the process ends. With it:
 *   1. ABSORB. A fault inside one of the tiny memory helpers of the lifted code (rd8 / rd16 / rd32 / rd64 / wr8 .. wr64 / the vector loads and stores: `mov (%rcx), %al ; ret`) is
 *      answered from the exception filter: the helper "returns" 0 (a store does nothing) and the thread goes on. A call through a pointer that is not a function (call_indirect)
 *      is skipped with x0 = 0. Either way the pass is marked TAINTED and the thread keeps running to its normal wait, so it is exactly where the ring expects it.
 *   2. RELOAD. At the next safe point after the pass (the same place every rollback is done), the tainted pass is not captured: the newest state of the ring from BEFORE it
 *      is loaded and the pass is run again (a re-run pass, the pads recorded for it). Faults are only counted, nothing else of that pass is kept.
 *   3. WALK BACK. If the re-run faults again, the state before the previous one is tried, down to the oldest of the ring; when they are used up (or too many faults in one
 *      pass, or too many recoveries in a match) the net lets go and the fault ends the game as before.
 * What it does NOT cover: a fault that is not in a leaf helper (a read inlined in a big function, the host's own code, the exception unwinder's terminate) and a thread that
 * never comes back to its wait. Those still end the game; the report says which kind each fault was, so the next helper to cover is known.
 * SSBU_RB_RECOVER=0: off. SSBU_RB_FAULT_TEST=<n>: every n real passes a deliberate fault is made in a leaf read (tests the whole net without waiting for a real one).
 * No game content in this file. */
#include "../model/game_state_internal.h"

int ra_fight_on(void);

static volatile LONG g_fr_pending;                      /* a fault was absorbed since the last safe point */
static volatile LONG g_fr_in_pass;                      /* faults absorbed in the pass that is running */
static volatile LONG64 g_fr_total, g_fr_recoveries, g_fr_giveups, g_fr_not_absorbable;
static volatile LONG g_fr_depth;                        /* consecutive recoveries: how far back the next one goes */
static long long g_fr_tainted_pass = -1;               /* the pass whose faults started the walk back */
static volatile LONG g_fr_off;                          /* the net let go: faults end the game again */
static volatile LONG g_fr_lines;
static int g_fr_enabled = -1;

#define FR_BAIL 256                                     /* past this many faults in one pass a fault cuts the cascade (see rb_fault_try_absorb) instead of being absorbed */
#define FR_MAX_PER_PASS 4096                            /* a bad object makes the code read through it and fault again and again (a null read gives 0, then 0+offset faults...): absorb the whole cascade so the thread still reaches
                                                         * its safe point, where the pass is reloaded once from the last healthy state (the reload, not read-by-read patching, is what fixes the object). More than this in one pass is a true runaway: give up. */
#define FR_MAX_RECOVERIES 400                           /* in one run: a fight that needs more than this is not being saved by it */

static int fr_enabled(void) {
    if (g_fr_enabled < 0) { const char *e = getenv("SSBU_RB_RECOVER"); g_fr_enabled = !(e && e[0] == '0'); }
    return g_fr_enabled && !g_fr_off && g_rg_ready && ra_fight_on();
}

/* The marks of an absorbed fault, shared by both ways in (a leaf helper and a bad indirect call). Returns 0 when the net must not absorb this one. */
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

/* A leaf helper: [prefixes] opcode ModRM(rcx) then RET. Returns the length of the faulting instruction, 0 when the bytes at rip are not that. */
static int fr_leaf_len(const uint8_t *p) {
    for (int len = 2; len <= 5; len++) if (p[len] == 0xC3 && (p[len - 1] & 0xC7) == 0x01) return len;      /* ModRM mod 00, rm = rcx */
    return 0;
}

/* EMULATION of a plain memory move that faulted, for reads and writes the compiler put INSIDE a big function (no helper to return from). A load gives 0 (the destination register
 * is cleared, a 32/64-bit one whole, an 8/16-bit one only its low part, an XMM register all of it), a store is dropped, and execution goes on after the instruction. Only the moves that
 * the lifted code is made of are understood: mov r,[m] / mov [m],r / mov [m],imm, movzx / movsx / movsxd, the SSE moves (movups movaps movdqa movdqu movss movsd movq). Anything else
 * (arithmetic with a memory operand, string moves, a segment override, an unknown opcode) is refused: the flags would be wrong. Works on a copy of the context; 1 = understood. */
static int fr_emulate(CONTEXT *cx) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)cx->Rip, *q = p;
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
    DWORD64 *gpr = &cx->Rax;                                 /* rax rcx rdx rbx rsp rbp rsi rdi r8..r15: the order of the x86 register numbers */
    if (kind == K_LOAD) {
        if (reg == 4) return 0;                              /* never clear rsp */
        if (width == 8) { if (!rex && reg >= 4 && reg < 8) gpr[reg - 4] &= ~(DWORD64)0xFF00; else gpr[reg] &= ~(DWORD64)0xFF; }
        else if (width == 16) gpr[reg] &= ~(DWORD64)0xFFFF;
        else gpr[reg] = 0;
    } else if (kind == K_XMM) memset(&(&cx->Xmm0)[reg & 15], 0, 16);
    cx->Rip += (DWORD64)len;
    return 1;
}

/* Called first thing by the unhandled-exception filter (hostrt.c). 1 = absorbed, the filter returns EXCEPTION_CONTINUE_EXECUTION. */
int rb_fault_try_absorb(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !fr_enabled()) return 0;
    {   /* (2026-11-08, the owner's match against Flame: c0000005 in ResInflateThread writing 0x29c58230000, +7, +f ... one fault
         * per 8 bytes from the start of a page of window 1, stepped over each time until the game died) a page of window 1 the
         * game had committed but not yet written at the state a load went back to, and decommitted later, was left decommitted
         * by the load (snapshot_ring.c, fixed there too): to the game it is committed memory. A fault on a reserved, uncommitted
         * page of window 1 now commits it (zeros, as a fresh commit) and the instruction runs again; the commit is told to the
         * page tracking (r1_on_commit) so its writes are captured. SSBU_RB_FAULT_COMMIT=0: off. */
        static int on = -1; if (on < 0) { const char *e = getenv("SSBU_RB_FAULT_COMMIT"); on = !(e && e[0] == '0'); }
        uint8_t *fa = (uint8_t *)(uintptr_t)er->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION fm;
        if (on && g_win1 && fa >= g_win1 && fa < g_win1 + g_wn1 && VirtualQuery(fa, &fm, sizeof fm) && fm.State == MEM_RESERVE) {
            uint8_t *pg = (uint8_t *)((uintptr_t)fa & ~(uintptr_t)(PG - 1));
            if (VirtualAlloc(pg, PG, MEM_COMMIT, PAGE_READWRITE)) {
                void r1_on_commit(void *p, size_t n); static long long n_commit;
                r1_on_commit(pg, PG);
                if (++n_commit <= 20 || n_commit % 100 == 0) LOG("r1: FAULT RECOVERY: a fault on an uncommitted page of window 1 (%p, thread %lu): committed (zeros) and run again (%lld so far)\n", (void *)fa, (unsigned long)GetCurrentThreadId(), n_commit);
                return 1;
            }
        }
    }
    CONTEXT *cx = ep->ContextRecord;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)cx->Rip;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(ip, &mi, sizeof mi) || !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) || (uintptr_t)ip + 8 > (uintptr_t)mi.BaseAddress + mi.RegionSize) return 0;
    {   /* CUT instead of absorb (send the thread to nnunwind_bail_trampoline: its host stack unwound to a catch frame of its own stack, the pass reloaded at its safe point) when
         *  - the pass is already tainted: after one intervention the thread runs on garbage, and absorbing more only lets it go further (a real match, 2026-10-28: after a throw was
         *    delivered it called address 1, then into KERNELBASE with garbage, and died there),
         *  - past FR_BAIL faults in one pass (looping on garbage),
         *  - or the fault is not in the game's own code (Windows, the runtime): those are never stepped over (an instruction of KERNELBASE "absorbed" is exactly how that match ended).
         * Without a catch frame on the stack: a fault in the game's code is still absorbed, one outside it ends the game as before. */
        extern int nnunwind_has_bail_target(uint64_t rsp); extern void nnunwind_bail_trampoline(void); extern uint64_t guest_function_of(uint64_t rip);
        int in_game = guest_function_of(cx->Rip) != 0;
        if ((g_fr_pending || g_fr_in_pass >= FR_BAIL || !in_game) && nnunwind_has_bail_target(cx->Rsp)) {
            if (!fr_note(in_game ? "cut (pass already tainted)" : "cut (fault outside the game's code)", cx->Rip, er->ExceptionInformation[1])) return 0;
            { extern int nnunwind_bail_context(CONTEXT *cx); if (nnunwind_bail_context(cx)) return 1; }      /* the context is loaded from the catch frame's jmp_buf */
            return 0;
        }
        if (!in_game) { InterlockedIncrement64(&g_fr_not_absorbable); LOG("r1: FAULT RECOVERY: a fault at %p is outside the game's code and this thread has no catch frame: not absorbable\n", (void *)ip); return 0; }
    }
    if (!fr_leaf_len(ip)) {                              /* not a leaf helper: a plain memory move inside a bigger function can still be stepped over */
        if ((uintptr_t)ip + 16 <= (uintptr_t)mi.BaseAddress + mi.RegionSize) {
            CONTEXT *tmp = (CONTEXT *)malloc(sizeof *tmp);
            if (tmp) {
                *tmp = *cx;
                if (fr_emulate(tmp) && fr_note("mov stepped over", cx->Rip, er->ExceptionInformation[1])) { *cx = *tmp; free(tmp); return 1; }
                free(tmp);
                if (g_fr_in_pass > FR_MAX_PER_PASS) return 0;      /* fr_note let go */
            }
        }
        InterlockedIncrement64(&g_fr_not_absorbable);
        LOG("r1: FAULT RECOVERY: a fault at %p is not a plain move: not absorbable (bytes %02x %02x %02x %02x %02x %02x %02x %02x)\n", (void *)ip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
        return 0;
    }
    if (cx->Rsp & 7) return 0;
    if (!fr_note("leaf helper", cx->Rip, er->ExceptionInformation[1])) return 0;
    cx->Rax = 0; memset(&cx->Xmm0, 0, sizeof cx->Xmm0);      /* a read gives 0, a write is dropped */
    cx->Rip = *(uint64_t *)(uintptr_t)cx->Rsp; cx->Rsp += 8;   /* the helper returns */
    return 1;
}

/* call_indirect on an address that is not a function (hostrt.c): skip the call, x0 = 0. */
int rb_fault_absorb_icall(Ctx *c, uint64_t addr) {
    if (!fr_enabled()) return 0;
    {   /* the pass is already tainted: this thread is running on garbage; cut it back to a catch frame of its own stack instead of skipping the call and going on */
        extern int nnunwind_has_bail_target(uint64_t rsp); extern void nnunwind_bail_trampoline(void);
        volatile char here = 0;
        if (g_fr_pending && nnunwind_has_bail_target((uint64_t)(uintptr_t)&here) && fr_note("cut (pass already tainted: indirect call)", c->x[30] - 4, addr)) nnunwind_bail_trampoline();
    }
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
 * same pass faults again) and tells the caller to run the passes again. 1 = loaded (the caller sets up the re-run: *best is the frame loaded), 0 = the net let go. */
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

/* A pass ended with no fault: the walk back starts from the newest state again next time. */
/* A pass `i` ended with no fault. The walk back (how far the next recovery goes) starts from the newest state again only once the pass that FAULTED has come through clean: a clean pass before
 * it is not news (a dense run, 2026-10-28: the walk went 1 back, 2 back, 1 back... 400 times at the same pass, because the clean pass in between reset it, and never reached an older state). */
void rb_fault_pass_clean(long long i) {
    if (!g_fr_pending && g_fr_depth && i >= g_fr_tainted_pass) { InterlockedExchange(&g_fr_depth, 0); g_fr_tainted_pass = -1; }
    InterlockedExchange(&g_fr_in_pass, 0);
}

void rb_fault_report(void) {
    rg_quiet_report();
    if (g_fr_total || g_fr_not_absorbable || g_fr_giveups)
        LOG("r1: FAULT RECOVERY so far: %lld faults absorbed, %lld recoveries, %lld not absorbable, %lld times the net let go%s\n", (long long)g_fr_total, (long long)g_fr_recoveries, (long long)g_fr_not_absorbable, (long long)g_fr_giveups, g_fr_off ? " (OFF now)" : "");
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
        switch (turn++ % 3) {                           /* the three shapes the net understands: a leaf helper, a read inside a bigger function, a write inside a bigger function */
        case 0: { volatile uint8_t v = fr_test_leaf_read((const uint8_t *)g_fr_test_addr); (void)v; break; }
        case 1: { volatile uint32_t *p = (volatile uint32_t *)g_fr_test_addr; volatile uint32_t v = *p; (void)v; break; }
        default: { volatile uint64_t *p = (volatile uint64_t *)g_fr_test_addr; *p = 5; break; }
        }
    }
}
