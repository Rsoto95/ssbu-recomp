/* TEST AND MEASUREMENT CODE of the ring: the forced-rollback simulation (SSBU_RB_SIM), the profilers and the verifier. Not used in a real match.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "../model/game_state_internal.h"
static void rgv_after(void);
/* Functions and variables of other runtime files that only this file uses: */
uint64_t guest_function_of(uint64_t rip);

/* ---------------------------------------------------------------- the rollback simulation (SSBU_RB_SIM) */
static DWORD g_rgp_main; static int g_rgs_prof; extern volatile int g_rgp_seg; static void rgs_samp_start(void);
int g_rgs_lastlite;                              /* R3 (SSBU_RB_NET_LASTLITE=1): the last re-run pass skips like the others (the real pass after it runs everything) */
int g_rgs_on = -1, g_rgs_n = 10, g_rgs_k = 7, g_rgs_warm = 30, g_rgs_skip, g_rgs_left, g_rgs_ki;
long long g_rgs_next, g_rgs_real_since, g_rgs_done, g_rgs_nocap, g_rgs_padmiss, g_rgs_ksum[RG_MAXSLOT];
static PadRec g_rgs_pad[256]; static long long g_rgs_padf[256]; static int g_rgs_polls_cur; static long long g_rgs_multi;
static double g_rgs_pass_t0, g_rgs_pass[2]; static long long g_rgs_npass[2];
static double g_rgs_pl[2], g_rgs_plv[2][RG_NST / 4]; static long long g_rgs_npl[2]; static int g_rgs_nplv[2];      /* (round 3) re-run passes: 0 = skipped ones (not the last), 1 = the last one */
static double g_rgs_pass_v[2][RG_NST / 4]; static int g_rgs_pass_nv[2];
       /* R3: rb_rollback.c */
int rgs_wanted(void) {
    if (g_rgs_on < 0 && rbg_on()) { g_rgs_on = 1; g_rgs_n = 1 << 30; return 1; }      /* R3 online rollback (SSBU_RB_NET): the ring driven by the input queues */
    if (g_rgs_on < 0) { const char *e = getenv("SSBU_RB_SIM"); g_rgs_on = e && e[0] && e[0] != '0'; if (g_rgs_on) sscanf(e, "%d,%d,%d", &g_rgs_n, &g_rgs_k, &g_rgs_warm); if (g_rgs_k < 1) g_rgs_k = 1; if (g_rgs_k > RG_MAXSLOT - 1) g_rgs_k = RG_MAXSLOT - 1; if (g_rgs_n < 1) g_rgs_n = 1; }
    return g_rgs_on;
}

/* (R3 hs16) the main thread's CPU cycles per pass (QueryThreadCycleTime): a measure of the code's own speed that other
 * processes on the PC disturb much less than wall time (A/B of builds on a busy PC) */
static ULONG64 g_rgs_cyc0; static double g_rgs_cyc[2];
ULONG64 rgs_cyc_now(void) { ULONG64 v = 0; QueryThreadCycleTime(GetCurrentThread(), &v); return v; }
static int g_rgs_newfight;                               /* (2026-10-28) a fight ended: the next one makes the ring's base again (it kept fight 1's base: fight 2 froze) */
/* at the safe point (H2) of every pass while the fight runs */
void rgs_step(Ctx *c) {
    double t = now_ms(); ULONG64 cy = rgs_cyc_now();
    call_trace_poll();
    g_rgp_main = GetCurrentThreadId(); g_rgp_seg = 1; if (g_rgs_prof == 3 && g_rg_ready) rgs_samp_start();
    if (g_rgs_pass_t0 > 0 && g_rg_ready) {                /* the pass that just ended: real (0) or re-run (1), without the capture / load */
        int kd = g_ra_phase == 2; double d = t - g_rgs_pass_t0; if (g_rgs_cyc0) g_rgs_cyc[kd] += (double)(cy - g_rgs_cyc0);
        if (kd) { int l = g_rgs_left <= 1; g_rgs_pl[l] += d; g_rgs_npl[l]++; if (g_rgs_nplv[l] < RG_NST / 4) g_rgs_plv[l][g_rgs_nplv[l]++] = d; }
        g_rgs_pass[kd] += d; g_rgs_npass[kd]++; if (g_rgs_pass_nv[kd] < RG_NST / 4) g_rgs_pass_v[kd][g_rgs_pass_nv[kd]++] = d;
    }
    if (g_rgs_polls_cur > 1) g_rgs_multi++;
    g_rgs_polls_cur = 0;
    {   static int was_on; void dm_audio_queue_report(const char *when);      /* (2026-10-28) the sound engine's command queue at the start and end of each fight */
        int on = !g_ra_off && ra_fight_on();
        if (on != was_on) {
            dm_audio_queue_report(on ? "fight start" : "fight over, before the audio threads are let go");
            if (!on && !rbg_on()) g_rgs_newfight = 1;     /* (the simulation: the next fight makes the ring's base again) */
            if (!on && g_rb_audio_off) {                  /* (the audio threads were held for the fight: its backlog is dropped while they still are) */
                int rb_audio_wait_parked(double max_ms); void dm_audio_queue_drop(void); void dm_audio_pump(Ctx *c, int force);
                if (rb_audio_wait_parked(50)) { dm_audio_pump(c, 1); dm_audio_queue_drop(); }     /* (2026-10-29) the pump runs only when the queue is filling: what is left is emptied here */
                else LOG("r1: AUDIO QUEUE: an audio thread still runs at the end of the fight: the backlog is left as it is\n");
            }
            was_on = on;
        }
        if (on && g_rb_audio_off) { void dm_audio_pump(Ctx *c, int force); dm_audio_pump(c, 0); } }   /* (the audio threads are held: the main thread empties the sound engine's queue once it is filling) */
    if (g_ra_off || !ra_fight_on()) { void rb_audio_fight_over(void); void nnaudio_gate(int on); nnaudio_gate(0); ls_fight_over(); InterlockedExchange(&g_rb_audio_off, 0); rb_audio_fight_over(); if (g_ra_phase) g_ra_after_quiet = 1; g_ra_phase = 0; g_rgs_left = 0; g_rgs_pass_t0 = 0; if (rbg_on()) rbg_fight_off(); return; }   /* (R3: the report once at the end of each match; hs12: the session reset for the next match) */
    if (rbg_on()) { rbg_step(c); g_rgs_pass_t0 = now_ms(); g_rgs_cyc0 = rgs_cyc_now(); g_ra_t_mark = g_rgs_pass_t0; return; }      /* R3: online rollback */
    long long i = g_rgs_next;                             /* the pass that ended here */
    if (g_rg_keep_on < 0) { const char *e = getenv("SSBU_RB_KEEP"); g_rg_keep_on = !e || atoi(e) != 0; }
    {   /* will a load follow this capture? then the threads stay held from the capture to the load (SSBU_RB_KEEP=0: not) */
        int ph = g_ra_phase == 2 && g_rgs_left - 1 <= 0 ? 0 : g_ra_phase;
        g_rg_keep = g_rg_keep_on && g_rg_ready && ph == 0 && g_rg_caps + 1 >= g_rgs_warm && g_rgs_real_since + (ph == 0) >= g_rgs_n;
    }
    rb_fault_test_tick();                                 /* (SSBU_RB_FAULT_TEST=<n>: a deliberate fault in a leaf read every n real passes) */
    if (rb_fault_pending()) {                             /* the pass that ended had a fault absorbed (fault_recovery.c): it is not captured; the newest healthy state before it is loaded and it runs again */
        long long best;
        if (rb_fault_recover(i, c, &best)) {
            g_rgs_left = (int)(i - best); g_rgs_next = best + 1; g_ra_phase = 2; g_rgs_real_since = 0;
            r1_ring_let_go();
            g_rgs_pass_t0 = now_ms(); g_ra_t_mark = g_rgs_pass_t0;
            return;
        }
    }
    if (g_rgs_newfight && g_rg_ready) {                   /* (2026-10-28) a new fight in this session: the ring's base made again, as online play does at each match (rollback_controller.c) */
        if (!r1_ring_rebase(i, c)) { g_rgs_pass_t0 = now_ms(); return; }      /* (no quiet moment: the next pass) */
        g_rgs_newfight = 0; g_ra_phase = 0; g_rgs_left = 0; g_rgs_real_since = 0; g_rgs_next = i + 1;
        LOG("r1: RB SIM: a new fight: the ring's base made again at pass %lld (fight frame %lld)\n", i, g_session.fight_frame);
        g_rgs_pass_t0 = now_ms(); g_ra_t_mark = g_rgs_pass_t0;
        return;
    }
    int ok = 0, rebased = 0;
    if (g_ra_phase == 0 && g_rg_ready) { int rb_maybe_rebase(long long, Ctx *); g_rg_keep = 0; if (rb_maybe_rebase(i, c)) { ok = 1; rebased = 1; } }   /* OPTION 1: pages committed outside the ring brought in by remaking the base (snapshot_ring.c) */
    if (!rebased || !ok) ok = r1_ring_capture(i, c);
    g_rg_keep = 0;
    if (g_rgv_target >= 0 && i == g_rgv_target) { if (ok) rgv_after(); else g_rgv_target = -1; }
    if (!g_rg_ready) { g_rgs_pass_t0 = 0; return; }
    if (!ok) g_rgs_nocap++;
    if (ok) rb_fault_pass_clean(i);
    if (g_ra_phase == 2 && --g_rgs_left <= 0) { g_ra_phase = 0; g_ra_after_quiet = 1; g_rgs_left = 0; }
    g_rgs_next = i + 1;
    if (g_ra_phase == 0) g_rgs_real_since++;
    if (g_ra_phase == 0 && ok && g_rg_caps >= g_rgs_warm && g_rgs_real_since >= g_rgs_n) {
        int k = 1 + g_rgs_ki % g_rgs_k;
        long long target = i - k, best = -1;
        best = rg_best_state(target);                          /* the newest QUIET state at or before target (snapshot_ring.c) */
        if (best >= 0 && r1_ring_load(best, c) > 0) {
            g_rgs_ki++; g_rgs_real_since = 0; g_rgs_done++;
            g_rgs_left = (int)(i - best); g_rgs_next = best + 1; g_ra_phase = 2;
            if (g_rgs_left < RG_MAXSLOT) g_rgs_ksum[g_rgs_left]++;
            if (g_rgs_done <= 30 || g_rgs_done % 50 == 0) LOG("r1: RB SIM: rollback %lld at pass %lld (fight frame %lld) to pass %lld (k %d) in %.3f ms\n", g_rgs_done, i, g_session.fight_frame, best, g_rgs_left, g_rg_last[1]);
        }
    }
    r1_ring_let_go();                                     /* (no load after a kept capture) */
    g_rgs_pass_t0 = now_ms(); g_ra_t_mark = g_rgs_pass_t0;
}
/* ---- logic-only re-run (R2_logic_only_study.md section 6): presentation-side callees of the main loop skipped in re-run passes
 * but the last (SSBU_RB_SIM_LITE=<mask>: 1 camera, 2 UI, 4 lighting, 8 render layers, 16 recorder, 32 dynamic resolution,
 * 64 effect emitter tick); only when called from the main loop FUN 0x7103747270. SSBU_RB_SIM_PROF=1: the same hooks time their
 * calls (real and re-run passes) without skipping anything. */

static RgsLite g_rgl_h[] = {
    {0x710361d040ull, 1, 0}, {0x71036186d0ull, 1, 0}, {0x7103864700ull, 1, 0}, {0x7103619080ull, 1, 0}, {0x7103618b80ull, 1, 0},
    {0x7103632850ull, 2, 0}, {0x710361e7e0ull, 2, 0}, {0x71035c13d0ull, 2, 0},
    {0x7103575fa0ull, 4, 0}, {0x7103575c60ull, 4, 0}, {0x71035763c0ull, 4, 0}, {0x7103593c40ull, 4, 0},
    {0x71037857f0ull, 8, 0},
    {0x71036f2c00ull, 16, 0}, {0x71036f2d40ull, 16, 0}, {0x71036e6e00ull, 16, 0}, {0x71036f7410ull, 16, 1},
    {0x7103618ef0ull, 32, 0},
    {0x710356d7a0ull, 64, 0},
};

int g_rgs_lite; static uint64_t g_rgl_lo, g_rgl_hi;
static void rgs_lite_hook(Ctx *c, void *user) {
    RgsLite *h = user; uint64_t lr = c->x[30];
    int inloop = lr >= g_rgl_lo && lr < g_rgl_hi, kd = g_ra_phase == 2;
    if (inloop && (g_rgs_lite & h->grp) && g_rgs_on > 0 && g_ra_phase == 2 && (g_rgs_left > 1 || g_rgs_lastlite)) { h->sk++; c->x[0] = h->ret; return; }
    if (!g_rgs_prof || !inloop) { h->orig(c); return; }
    double t = now_ms(); h->orig(c); h->ms[kd] += now_ms() - t; h->n[kd]++;
}
/* ---- SSBU_RB_SIM_PROF=2 (R2 round 3): where the logic step's time goes. The game's task tree walker FUN 0x7103724a80
 * (main_loop calls it at 0x710374c624 per sim step; it calls node->vt[0x28], the children through vt[0x28] of the child
 * container with a callback that re-enters the walker, then node->vt[0x30]) is timed per node class (vtable), exclusive of
 * its children, on the main thread; the job-counter waits by return address; the job runner FUN 0x7103548240 per job
 * (worker CPU time, keyed by the job object's vtable). Real and re-run passes apart. */

static RgpE g_rgp[RGP_N], g_rgpj[RGP_N], g_rgpw[64]; static SRWLOCK g_rgpj_lock = SRWLOCK_INIT;
static ImportFn g_rgp_orig_tree, g_rgp_orig_run, g_rgp_orig_x[8]; static int g_rgp_depth; static double g_rgp_child[256];
static RgpE *rgp_slot(RgpE *t, int cap, uint64_t key) {
    unsigned h = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 52) % (unsigned)cap;
    for (int i = 0; i < cap; i++) { RgpE *e = &t[(h + (unsigned)i) % (unsigned)cap]; if (e->key == key) return e; if (!e->key) { e->key = key; e->dmin = 999; return e; } }
    return NULL;
}
static void rgp_tree(Ctx *c, void *user) {
    (void)user;
    if (!g_rg_ready || GetCurrentThreadId() != g_rgp_main || g_rgp_depth >= 255) { g_rgp_orig_tree(c); return; }
    uint64_t node = c->x[0], vt = node ? rd64(node) : 0, lr = c->x[30]; int kd = g_ra_phase == 2, d = g_rgp_depth;
    g_rgp_child[d + 1] = 0; g_rgp_depth++;
    double t0 = now_ms(); g_rgp_orig_tree(c); double dt = now_ms() - t0;
    g_rgp_depth--;
    RgpE *e = rgp_slot(g_rgp, RGP_N, vt ? vt : 1);
    if (e) { e->n[kd]++; e->incl[kd] += dt; e->excl[kd] += dt - g_rgp_child[d + 1]; if (d < e->dmin) e->dmin = d; }
    g_rgp_child[d] += dt;
    if (d == 0) { RgpE *w = rgp_slot(g_rgpw, 64, lr | (1ull << 62)); if (w) { w->n[kd]++; w->incl[kd] += dt; } }
}
static void rgp_run(Ctx *c, void *user) {          /* the job runner (task workers): x0 = the job's node */
    (void)user;
    if (!g_rg_ready) { g_rgp_orig_run(c); return; }
    uint64_t node = c->x[0], vt = node ? rd64(node - 8) : 0; int kd = g_ra_phase == 2;
    double t0 = now_ms(); g_rgp_orig_run(c); double dt = now_ms() - t0;
    AcquireSRWLockExclusive(&g_rgpj_lock);
    RgpE *e = rgp_slot(g_rgpj, RGP_N, vt ? vt : 1); if (e) { e->n[kd]++; e->incl[kd] += dt; }
    ReleaseSRWLockExclusive(&g_rgpj_lock);
}
/* a few single functions of the logic step, timed when called on main: poll, dispatch, the EffectManager update */

static uint64_t g_rgp_xa[RGP_NX];                        /* (the running build's addresses, set by rgp_init) */
static void rgp_x(Ctx *c, void *user) {
    int i = (int)(intptr_t)user; int kd = g_ra_phase == 2;
    if (!g_rg_ready || GetCurrentThreadId() != g_rgp_main) { g_rgp_orig_x[i](c); return; }
    double t0 = now_ms(); g_rgp_orig_x[i](c); double dt = now_ms() - t0;
    RgpE *w = rgp_slot(g_rgpw, 64, g_rgp_xa[i] | (2ull << 62)); if (w) { w->n[kd]++; w->incl[kd] += dt; }
}
void rgp_wait_note(uint64_t lr, double dt) {      /* from ra_kick: a job-counter wait on main */
    if (g_rgs_prof < 2 || !g_rg_ready || GetCurrentThreadId() != g_rgp_main) return;
    RgpE *w = rgp_slot(g_rgpw, 64, lr | (3ull << 62)); if (w) { w->n[g_ra_phase == 2]++; w->incl[g_ra_phase == 2] += dt; }
}
static void rgp_init(void) {
    g_rgp_xa[0] = GA(0x71036676e0ull); g_rgp_xa[1] = GA(0x710375ea70ull); g_rgp_xa[2] = GA(0x710355b540ull); g_rgp_xa[3] = GA(0x7103569c70ull);
    int a = hook_install(GA(0x7103724a80ull), rgp_tree, NULL, &g_rgp_orig_tree), b = hook_install(GA(0x7103548240ull), rgp_run, NULL, &g_rgp_orig_run), x = 0;
    for (int i = 0; i < RGP_NX; i++) x += hook_install(g_rgp_xa[i], rgp_x, (void *)(intptr_t)i, &g_rgp_orig_x[i]);
    LOG("r1: RB SIM profile 2: task tree walker %s, job runner %s, %d of %d single functions\n", a ? "hooked" : "FAILED", b ? "hooked" : "FAILED", x, RGP_NX);
}
static int rgp_cmp(const void *a, const void *b) { const RgpE *x = a, *y = b; double p = x->excl[0] + x->incl[0] * 1e-9, q = y->excl[0] + y->incl[0] * 1e-9; return p < q ? 1 : p > q ? -1 : 0; }
static int rgp_cmpi(const void *a, const void *b) { const RgpE *x = a, *y = b; return x->incl[0] < y->incl[0] ? 1 : x->incl[0] > y->incl[0] ? -1 : 0; }
/* ---- SSBU_RB_SIM_PROF=3: a SAMPLING profile of the main thread during the fight (one helper thread suspends main every
 * ~100 us, walks its host stack with the x64 unwind data and maps every frame to the lifted guest function it is in,
 * guest_function_of). Per segment of the pass (1 = safe point -> poll, 2 = poll -> frame end = the logic step, 3 = frame
 * end -> safe point) and pass kind (real / re-run): self and inclusive sample counts per guest function. Test only. */
uint64_t guest_function_of(uint64_t rip);
volatile int g_rgp_seg;                                     /* set by r1_poll / r1_frame_end / rgs_step */

static RgsS g_rgss[RGS_SN]; static long long g_rgss_tot[2][6], g_rgss_host[2][6]; static HANDLE g_rgss_main; static volatile int g_rgss_run;
static RgsS *rgss_slot(uint64_t fn) {
    unsigned h = (unsigned)((fn * 0x9e3779b97f4a7c15ull) >> 48) % RGS_SN;
    for (int i = 0; i < 64; i++) { RgsS *e = &g_rgss[(h + (unsigned)i) % RGS_SN]; if (e->fn == fn) return e; if (!e->fn) { e->fn = fn; return e; } }
    return NULL;
}
static DWORD WINAPI rgss_thread(void *p) {
    (void)p; uint64_t fr[64]; int nf;
    double next = now_ms();
    for (;;) {
        while (now_ms() < next) YieldProcessor();
        next += 0.1;
        int seg = g_rgp_seg, kd = g_ra_phase == 2;
        if (kd && g_rgs_left <= 1 && seg < 4) continue;              /* (the last re-run pass runs everything: not counted) */
        if (!g_rg_ready || !g_rgss_main || seg < 1 || seg > 5) { if (now_ms() > next + 5) next = now_ms(); continue; }
        if (SuspendThread(g_rgss_main) == (DWORD)-1) continue;
        CONTEXT x; memset(&x, 0, sizeof x); x.ContextFlags = CONTEXT_FULL; nf = 0;
        if (GetThreadContext(g_rgss_main, &x)) {
            for (int d = 0; d < 64 && nf < 64; d++) {
                DWORD64 base = 0; PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(x.Rip, &base, NULL);
                uint64_t g = guest_function_of(x.Rip);
                if (g && (!nf || fr[nf - 1] != g)) fr[nf++] = g; else if (!g && d == 0) fr[nf++] = ((uint64_t)x.Rip >> 4 << 4) | (1ull << 63);      /* 1 = host code (imports, hooks); capture / load: the host address */
                if (!f) break;
                void *hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, x.Rip, f, &x, &hd, &est, NULL);
                if (!x.Rip) break;
            }
        }
        ResumeThread(g_rgss_main);
        if (!nf) continue;
        g_rgss_tot[kd][seg]++;
        RgsS *e = rgss_slot(fr[0]); if (e) e->self[kd][seg]++;
        if (fr[0] == 1 || (fr[0] >> 63)) g_rgss_host[kd][seg]++;
        for (int i = 0; i < nf; i++) { int dup = 0; for (int j = 0; j < i; j++) if (fr[j] == fr[i]) dup = 1; if (dup) continue; RgsS *q = rgss_slot(fr[i]); if (q) q->incl[kd][seg]++; }
    }
    return 0;
}
static void rgs_samp_start(void) {
    if (g_rgss_run || g_rgs_prof != 3) return;
    g_rgss_run = 1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_rgss_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(CreateThread(NULL, 0, rgss_thread, NULL, 0, NULL));
    LOG("r1: RB PROF 3: sampling the main thread every 0.1 ms during the fight\n");
}
static int g_rgss_k[3];
static int rgss_cmp(const void *a, const void *b) {
    const RgsS *x = a, *y = b; int k = g_rgss_k[0];
    int sg = g_rgss_k[2]; long long p = g_rgss_k[1] ? x->incl[k][sg] : x->self[k][sg], q = g_rgss_k[1] ? y->incl[k][sg] : y->self[k][sg];
    return p < q ? 1 : p > q ? -1 : 0;
}
static RgsS g_rgss_c[RGS_SN];
static void rgs_samp_report(void) {
    if (!g_rgss_run) return;
    long long np = g_rgs_npass[0] ? g_rgs_npass[0] : 1, nr = g_rgs_npl[0] ? g_rgs_npl[0] : 1;      /* re-run = the skipped passes only */
    LOG("r1: RB PROF 3 samples (0.1 ms each) real / re-run: start->poll %lld / %lld, logic %lld / %lld, frame end->safe point %lld / %lld; host code in the logic %lld / %lld; passes %lld / %lld\n",
        g_rgss_tot[0][1], g_rgss_tot[1][1], g_rgss_tot[0][2], g_rgss_tot[1][2], g_rgss_tot[0][3], g_rgss_tot[1][3], g_rgss_host[0][2], g_rgss_host[1][2], np, nr);
    LOG("r1: RB PROF 3 samples in captures %lld / %lld, loads %lld / %lld (host code %lld / %lld)\n", g_rgss_tot[0][4], g_rgss_tot[1][4], g_rgss_tot[0][5], g_rgss_tot[1][5], g_rgss_host[0][4] + g_rgss_host[0][5], g_rgss_host[1][4] + g_rgss_host[1][5]);
    memcpy(g_rgss_c, g_rgss, sizeof g_rgss);
    static const char *sgn[6] = {"", "start->poll", "logic", "frame end->safe point", "capture", "load"};
    for (int sg = 1; sg <= 5; sg++) for (int kd = 0; kd < 2; kd++) for (int inc = 1; inc >= 0; inc--) {
        if (sg >= 4 && kd == 1) continue;                    /* captures / loads: one list (real + re-run passes together) */
        if (sg != 2 && kd == 0 && sg < 4) continue;          /* start->poll / tail: the re-run passes only */
        g_rgss_k[0] = kd; g_rgss_k[1] = inc; g_rgss_k[2] = sg;
        if (sg >= 4) for (int i = 0; i < RGS_SN; i++) { g_rgss_c[i].self[0][sg] += g_rgss[i].self[1][sg]; g_rgss_c[i].incl[0][sg] += g_rgss[i].incl[1][sg]; }
        qsort(g_rgss_c, RGS_SN, sizeof *g_rgss_c, rgss_cmp);
        long long tot = sg >= 4 ? g_rgss_tot[0][sg] + g_rgss_tot[1][sg] : g_rgss_tot[kd][sg]; long long div = sg >= 4 ? (g_rg_caps + g_rg_loads ? (sg == 4 ? g_rg_caps : g_rg_loads) : 1) : (kd ? nr : np);
        for (int i = 0; i < (sg == 2 ? 80 : 40); i++) {
            RgsS *e = &g_rgss_c[i]; long long v = inc ? e->incl[kd][sg] : e->self[kd][sg]; if (!v) break;
            LOG("r1: RB PROF 3 %s %s %s %llx: %.4f ms per %s (%lld of %lld samples); self %.4f\n", kd ? "re-run" : "real", sgn[sg], inc ? "incl" : "self", (unsigned long long)e->fn,
                v * 0.1 / div, sg == 4 ? "capture" : sg == 5 ? "load" : "pass", v, tot, e->self[kd][sg] * 0.1 / div);
        }
        memcpy(g_rgss_c, g_rgss, sizeof g_rgss);
    }
}
static RgpE g_rgp_s[RGP_N];
static void rgp_report(void) {
    if (g_rgs_prof < 2) return;
    rgs_samp_report();
    if (g_rgs_prof != 2) return;
    long long np = g_rgs_npass[0] ? g_rgs_npass[0] : 1, nr = g_rgs_npass[1] ? g_rgs_npass[1] : 1;
    LOG("r1: RB PROF per pass (real %lld, re-run %lld): ms per pass real / re-run\n", g_rgs_npass[0], g_rgs_npass[1]);
    memcpy(g_rgp_s, g_rgpw, sizeof g_rgpw); qsort(g_rgp_s, 64, sizeof *g_rgp_s, rgp_cmpi);
    for (int i = 0; i < 64; i++) if (g_rgp_s[i].key) { uint64_t k = g_rgp_s[i].key; int ty = (int)(k >> 62); k &= ~(3ull << 62);
        LOG("r1: RB PROF %s %llx: calls %lld / %lld, %.4f / %.4f ms per pass\n", ty == 1 ? "tree walk from" : ty == 2 ? "function" : "wait at", (unsigned long long)k, g_rgp_s[i].n[0], g_rgp_s[i].n[1], g_rgp_s[i].incl[0] / np, g_rgp_s[i].incl[1] / nr); }
    memcpy(g_rgp_s, g_rgp, sizeof g_rgp); qsort(g_rgp_s, RGP_N, sizeof *g_rgp_s, rgp_cmp);
    for (int i = 0; i < RGP_N && i < 80; i++) if (g_rgp_s[i].key) LOG("r1: RB PROF node vt %llx depth %d: calls/pass %.2f / %.2f, exclusive %.4f / %.4f ms per pass, inclusive %.4f / %.4f\n",
        (unsigned long long)g_rgp_s[i].key, g_rgp_s[i].dmin, g_rgp_s[i].n[0] / (double)np, g_rgp_s[i].n[1] / (double)nr, g_rgp_s[i].excl[0] / np, g_rgp_s[i].excl[1] / nr, g_rgp_s[i].incl[0] / np, g_rgp_s[i].incl[1] / nr);
    AcquireSRWLockExclusive(&g_rgpj_lock); memcpy(g_rgp_s, g_rgpj, sizeof g_rgpj); ReleaseSRWLockExclusive(&g_rgpj_lock); qsort(g_rgp_s, RGP_N, sizeof *g_rgp_s, rgp_cmpi);
    for (int i = 0; i < RGP_N && i < 60; i++) if (g_rgp_s[i].key) LOG("r1: RB PROF job vt %llx: jobs/pass %.2f / %.2f, worker ms per pass %.4f / %.4f\n",
        (unsigned long long)g_rgp_s[i].key, g_rgp_s[i].n[0] / (double)np, g_rgp_s[i].n[1] / (double)nr, g_rgp_s[i].incl[0] / np, g_rgp_s[i].incl[1] / nr);
}
void rgs_lite_init(void) {
    const char *e = getenv("SSBU_RB_SIM_LITE"); g_rgs_lite = e ? atoi(e) : 0; g_rgs_prof = getenv("SSBU_RB_SIM_PROF") ? atoi(getenv("SSBU_RB_SIM_PROF")) : 0;
    if (g_rgs_prof == 2) rgp_init();
    if (!g_rgs_lite && !g_rgs_prof) return;
    g_rgl_lo = GA(0x7103747270ull); g_rgl_hi = GA(0x710374e000ull);
    int ok = 0;
    for (int i = 0; i < RGL_N; i++) if ((g_rgs_prof || (g_rgs_lite & g_rgl_h[i].grp)) && hook_install(GA(g_rgl_h[i].a), rgs_lite_hook, &g_rgl_h[i], &g_rgl_h[i].orig)) ok++;
    LOG("r1: RB SIM logic-only re-run: skip mask %d, profile %d: %d hooks installed\n", g_rgs_lite, g_rgs_prof, ok);
}
static void rgs_lite_report(void) {
    if (!g_rgs_lite && !g_rgs_prof) return;
    for (int i = 0; i < RGL_N; i++) if (g_rgl_h[i].orig) LOG("r1: RB SIM lite %llx (group %d): skipped %lld; timed real %lld calls %.4f ms/call, re-run %lld calls %.4f ms/call\n", (unsigned long long)g_rgl_h[i].a, g_rgl_h[i].grp, g_rgl_h[i].sk,
        g_rgl_h[i].n[0], g_rgl_h[i].n[0] ? g_rgl_h[i].ms[0] / g_rgl_h[i].n[0] : 0, g_rgl_h[i].n[1], g_rgl_h[i].n[1] ? g_rgl_h[i].ms[1] / g_rgl_h[i].n[1] : 0);
}
/* ---- SSBU_RB_SIM_VERIFY=1: every rollback, the whole tracked state is compared, not only the fighters' hash: at the load, a hash of the
 * NEWEST value of every page the newer slots changed; when the re-run is back at that pass, the same pages (and every page the re-run
 * wrote that the original window did not: against its value at the loaded state). Window pages and extras counted apart. */
static int g_rgv_on = -1; static uint8_t *g_rgv_in; static uint32_t *g_rgv_id; static uint64_t *g_rgv_h; static size_t g_rgv_n;

long long g_rgv_target = -1, g_rgv_from, g_rgv_runs, g_rgv_bad_runs, g_rgv_pages, g_rgv_diff[3];
static uint64_t rgv_hash(const uint8_t *p) {
    if (!p) return 0x9e3779b97f4a7c15ull;
    if (p == (uint8_t *)1) return 1;
    const uint64_t *q = (const uint64_t *)p; uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < PG / 8; i++) h = (h ^ q[i]) * 1099511628211ull + (h >> 29);
    return h;
}
static void rgvd_keep(size_t n, const uint8_t *src); static void rgvd_show(size_t n, uint32_t id, const uint8_t *now);
static uint8_t *rg_src_v(uint32_t id, uint32_t v) { uint32_t s = g_rg_idx[id]; g_rg_idx[id] = v; uint8_t *r = rg_src(id); g_rg_idx[id] = s; return r; }
/* SSBU_RB_SIM_VERIFY=3: besides the tracked pages, snapshot the pages the ring does NOT save (committed/written outside it during the fight, page_tracker_learned.c) at the load and compare them after the
 * re-run returns to the same pass. If many of those differ, the state the ring cannot restore is the cause of the post-rollback crashes and bringing those pages into the ring (option 2) would fix it;
 * if ~none differ, the corruption is elsewhere (host-side) and option 2 would not be enough. First 12 rollbacks. */
extern uint64_t g_xls_out_pg[]; extern volatile LONG g_xls_out_npg;
static uint64_t *g_rgvo_h; static int g_rgvo_n;
static uint64_t rgv_hash(const uint8_t *p);
static void rgvo_before(void) {
    if (g_rgv_on < 3 || g_rgv_runs >= 12) { g_rgvo_n = 0; return; }
    int n = (int)g_xls_out_npg; if (n > 65536) n = 65536;
    if (!g_rgvo_h) g_rgvo_h = malloc((size_t)65536 * 8);
    if (!g_rgvo_h) { g_rgvo_n = 0; return; }
    g_rgvo_n = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *a = (uint8_t *)(uintptr_t)g_xls_out_pg[i];
        MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_COMMIT) continue;
        g_xls_out_pg[g_rgvo_n] = g_xls_out_pg[i];       /* compact to the ones still committed, so rgvo_after lines up */
        g_rgvo_h[g_rgvo_n] = rgv_hash(a); g_rgvo_n++;
    }
}
static void rgvo_after(void) {
    if (g_rgv_on < 3 || g_rgv_runs >= 12 || !g_rgvo_h || !g_rgvo_n) return;
    int differ = 0; char first[300]; int o = 0; first[0] = 0;
    for (int i = 0; i < g_rgvo_n; i++) {
        uint8_t *a = (uint8_t *)(uintptr_t)g_xls_out_pg[i];
        MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_COMMIT) { differ++; continue; }
        if (rgv_hash(a) != g_rgvo_h[i]) { differ++; if (o < 260) o += snprintf(first + o, sizeof first - o, " %llx", (unsigned long long)(uintptr_t)a); }
    }
    LOG("r1: RB VERIFY OUTSIDE-RING: %d of %d pages the ring does not save differ after the re-run%s%s\n", differ, g_rgvo_n, differ ? "; first:" : " (so these pages are not the cause)", first);
}
void rgv_before(int si, long long target) {        /* in r1_ring_load, before the undo logs are applied */
    if (g_rgv_on < 0) g_rgv_on = getenv("SSBU_RB_SIM_VERIFY") ? atoi(getenv("SSBU_RB_SIM_VERIFY")) : 0;
    if (!g_rgv_on) return;
    rgvo_before();
    size_t tot = g_rg_n1 + g_rg_n2 + g_rg_nx;
    if (!g_rgv_in) { g_rgv_in = calloc(tot, 1); g_rgv_id = malloc((size_t)RGV_MAX * 4); g_rgv_h = malloc((size_t)RGV_MAX * 8); }
    g_rgv_n = 0;
    for (int i = g_rg_count - 1; i > si; i--) {
        RgSlot *s = rg_slot(i);
        for (size_t n = 0; n < s->nent; n++) {
            uint32_t id = s->ent[2 * n]; if (g_rgv_in[id] || g_rgv_n >= RGV_MAX / 2) continue;
            g_rgv_in[id] = 1; g_rgv_id[g_rgv_n] = id; g_rgv_h[g_rgv_n] = rgv_hash(rg_src(id)); rgvd_keep(g_rgv_n, rg_src(id)); g_rgv_n++;
        }
    }
    g_rgv_target = target; g_rgv_from = rg_slot(si)->frame;
}
/* (R2 round 3) SSBU_RB_SIM_VERIFY=2: for the first 12 rollbacks the original bytes of the compared pages are kept and every
 * differing page is printed: its address (where()), the number of differing 8-byte words, the first 4 (offset, original, re-run). */

static uint8_t *g_rgvd; static long long g_rgvd_lines;
static void rgvd_keep(size_t n, const uint8_t *src) {
    if (g_rgv_on < 2 || g_rgv_runs >= 12 || n >= RGVD_MAX) return;
    if (!g_rgvd) g_rgvd = VirtualAlloc(NULL, (size_t)RGVD_MAX * PG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_rgvd) return;
    if (!src) memset(g_rgvd + n * PG, 0, PG); else if (src == (uint8_t *)1) memset(g_rgvd + n * PG, 0xdd, PG); else memcpy(g_rgvd + n * PG, src, PG);
}
static void rgvd_show(size_t n, uint32_t id, const uint8_t *now) {
    if (g_rgv_on < 2 || g_rgv_runs >= 12 || n >= RGVD_MAX || !g_rgvd || g_rgvd_lines > 6000) return;
    static uint8_t z[PG]; if (!now) now = z; if (now == (uint8_t *)1) return;
    const uint64_t *a = (const uint64_t *)(g_rgvd + n * PG), *b = (const uint64_t *)now; int nd = 0; char t[400]; int o = 0; t[0] = 0;
    for (int i = 0; i < PG / 8; i++) if (a[i] != b[i]) { nd++; if (nd <= 4 && o < 360) o += snprintf(t + o, sizeof t - o, " +%03x: %llx -> %llx", i * 8, (unsigned long long)a[i], (unsigned long long)b[i]); }
    g_rgvd_lines++;
    LOG("r1: RB VERIFY page %s (%p) rollback %lld: %d words differ;%s\n", where(rg_addr(id)), (void *)rg_addr(id), g_rgv_runs, nd, t);
}
static int rgv_kind(uint32_t id) { return id < g_rg_n1 ? 0 : id < g_rg_n1 + g_rg_n2 ? 1 : 2; }
/* differing pages by 64 KB block (where() name of the block), over every rollback: which memory is not reproduced */

static struct { uint64_t blk; long long n, runs; long long last; } g_rgv_b[RGV_NB]; static int g_rgv_nb;
static void rgv_note(uint32_t id) {
    uint64_t blk = (uint64_t)(uintptr_t)rg_addr(id) >> 16;
    for (int i = 0; i < g_rgv_nb; i++) if (g_rgv_b[i].blk == blk) { g_rgv_b[i].n++; if (g_rgv_b[i].last != g_rgv_runs) { g_rgv_b[i].runs++; g_rgv_b[i].last = g_rgv_runs; } return; }
    if (g_rgv_nb < RGV_NB) { g_rgv_b[g_rgv_nb].blk = blk; g_rgv_b[g_rgv_nb].n = 1; g_rgv_b[g_rgv_nb].runs = 1; g_rgv_b[g_rgv_nb].last = g_rgv_runs; g_rgv_nb++; }
}
static int rgv_cmpb(const void *a, const void *b) { long long x = ((const long long *)a)[1], y = ((const long long *)b)[1]; return x < y ? 1 : x > y ? -1 : 0; }
static void rgv_blocks_report(void) {
    if (!g_rgv_nb) return;
    qsort(g_rgv_b, (size_t)g_rgv_nb, sizeof g_rgv_b[0], rgv_cmpb);
    char b[3000]; int o = 0;
    for (int i = 0; i < g_rgv_nb && i < 40 && o < 2900; i++) o += snprintf(b + o, sizeof b - o, " [%s: %lld pages, %lld rollbacks]", where((uint8_t *)(uintptr_t)(g_rgv_b[i].blk << 16)), g_rgv_b[i].n, g_rgv_b[i].runs);
    b[o] = 0;
    LOG("r1: RB VERIFY differing 64 KB blocks (%d, the 40 largest):%s\n", g_rgv_nb, b);
}
/* The differing pages of one verification grouped by 64 KB block (where() names the block): the largest ones are printed after the summary line. */
#define RGV_NB 256
static struct { uint64_t blk; long long n; } g_rgv_blk[RGV_NB]; static int g_rgv_nblk;
static void rgv_blk_note(uint8_t *addr) {
    uint64_t b = (uint64_t)(uintptr_t)addr & ~0xffffull; int i;
    for (i = 0; i < g_rgv_nblk; i++) if (g_rgv_blk[i].blk == b) { g_rgv_blk[i].n++; return; }
    if (g_rgv_nblk < RGV_NB) { g_rgv_blk[g_rgv_nblk].blk = b; g_rgv_blk[g_rgv_nblk++].n = 1; }
}
static void rgv_blk_print(void) {
    for (int shown = 0; shown < 10 && g_rgv_nblk; shown++) {
        int best = 0; for (int i = 1; i < g_rgv_nblk; i++) if (g_rgv_blk[i].n > g_rgv_blk[best].n) best = i;
        LOG("r1: RB VERIFY   block %s: %lld pages differ\n", where((uint8_t *)(uintptr_t)g_rgv_blk[best].blk), g_rgv_blk[best].n);
        g_rgv_blk[best] = g_rgv_blk[--g_rgv_nblk];
    }
    g_rgv_nblk = 0;
}
static void rgv_after(void) {                               /* right after the capture of the re-run pass that reached the target */
    long long d[3] = {0, 0, 0}, pages = (long long)g_rgv_n; char first[400]; int o = 0; first[0] = 0;
    for (size_t n = 0; n < g_rgv_n; n++) {
        uint32_t id = g_rgv_id[n];
        if (rgv_hash(rg_src(id)) != g_rgv_h[n]) { int k = rgv_kind(id); d[k]++; rgv_note(id); rgv_blk_note(rg_addr(id)); rgvd_show(n, id, rg_src(id)); if (o < 330 && d[k] <= 3) o += snprintf(first + o, sizeof first - o, " %s", where(rg_addr(id))); }
    }
    size_t extra = 0;
    for (int i = 0; i < g_rg_count; i++) {                  /* pages the re-run wrote that the original window did not */
        RgSlot *s = rg_slot(i); if (s->frame <= g_rgv_from || s->frame > g_rgv_target) continue;
        for (size_t n = 0; n < s->nent; n++) {
            uint32_t id = s->ent[2 * n]; if (g_rgv_in[id]) continue;
            if (g_rgv_n + extra >= RGV_MAX) continue;
            g_rgv_in[id] = 2; g_rgv_id[g_rgv_n + extra++] = id;
            pages++;
            if (rgv_hash(rg_src(id)) != rgv_hash(rg_src_v(id, s->ent[2 * n + 1]))) { int k = rgv_kind(id); d[k]++; rgv_note(id); rgv_blk_note(rg_addr(id)); if (o < 330 && d[k] <= 3) o += snprintf(first + o, sizeof first - o, " %s(re-run only)", where(rg_addr(id))); }
        }
    }
    for (size_t n = 0; n < g_rgv_n + extra; n++) g_rgv_in[g_rgv_id[n]] = 0;
    g_rgv_runs++; g_rgv_pages += pages; for (int k = 0; k < 3; k++) g_rgv_diff[k] += d[k];
    if (d[0] || d[1]) g_rgv_bad_runs++;
    if (d[0] || d[1] || d[2] || g_rgv_runs <= 5) LOG("r1: RB VERIFY rollback to %lld, back at %lld: %lld pages compared; differ: window 1 %lld, window 2 %lld, extras %lld;%s\n", g_rgv_from, g_rgv_target, pages, d[0], d[1], d[2], first);
    if (d[0] || d[1] || d[2]) { if (g_rgv_runs <= 8 || g_rgv_runs % 50 == 0) rgv_blk_print(); else g_rgv_nblk = 0; }
    rgvo_after();                                           /* VERIFY=3: the pages the ring does not save */
    g_rgv_target = -1;
}
int rgs_skip_now(void) { return g_rgs_on > 0 && g_ra_phase == 2 && (g_rgs_left > 1 || g_rgs_lastlite) ? g_rgs_skip : 0; }
int rgs_poll(void) { g_rgs_polls_cur++; return g_ra_phase == 2; }    /* a re-run pass: the input sequence waits */
/* SSBU_RB_SIM_PERTURB=<percent> (default 0 = off): in a re-run pass of the simulation, that percentage of the player's pads are changed (one
 * button toggled, the left stick set to a random value) instead of replayed. The forced-rollback simulation replays the SAME pads, so a page
 * that a load left stale is written again with the same values and nothing reads it wrongly; a rollback in a real match replays DIFFERENT
 * pads (the guess was wrong), and the game then runs code it did not run the first time. This makes the simulation do the same. The timeline
 * after such a pass is simply the new one; there is no reference to compare against, so it finds crashes, not wrong values. */
static int g_rgs_perturb = -1; static uint32_t g_rgs_prng = 2463534242u;
static void rgs_perturb_pad(uint64_t *b, int *lx, int *ly) {
    if (g_rgs_perturb < 0) { const char *e = getenv("SSBU_RB_SIM_PERTURB"); g_rgs_perturb = e ? atoi(e) : 0; if (g_rgs_perturb > 0) LOG("r1: RB SIM: %d%% of the re-run pads are changed (SSBU_RB_SIM_PERTURB)\n", g_rgs_perturb); }
    if (g_rgs_perturb <= 0) return;
    g_rgs_prng = g_rgs_prng * 1664525u + 1013904223u;
    if ((int)((g_rgs_prng >> 16) % 100) >= g_rgs_perturb) return;
    g_rgs_prng = g_rgs_prng * 1664525u + 1013904223u;
    uint32_t r = g_rgs_prng >> 8;
    /* SSBU_RB_SIM_PERTURB_WHAT=b|s|bs (default bs): change only a button, only the left stick, or both. A stick value of -32768 is outside
     * the console pad's range (-32767..32767): the stick is now kept inside it. Added to find out whether a crash seen with perturbed pads
     * comes from the rollback or from the odd pad values themselves. */
    static int what = -1; if (what < 0) { const char *e = getenv("SSBU_RB_SIM_PERTURB_WHAT"); what = (e && e[0] == 'b' && !e[1]) ? 1 : (e && e[0] == 's' && !e[1]) ? 2 : 3; }
    if (what & 1) *b ^= 1ull << (r % 12);                         /* one of the 12 lowest button bits */
    if (what & 2) { *lx = (int)(((r >> 4) & 0xffff) % 65535u) - 32767; *ly = (int)(((r >> 12) & 0xffff) % 65535u) - 32767; }
    static int shown; if (shown++ < 30) LOG("r1: RB SIM perturbed pad %d: buttons %llx, left stick %d %d (pass %lld)\n", shown, (unsigned long long)*b, *lx, *ly, g_rgs_next);
}
/* SSBU_RB_SIM_FORCEPADS="<pass>:<buttons hex>:<lx>:<ly>,..." (test only): in a REAL pass (the first time a frame runs) the player's pad of that pass is replaced
 * by the one listed. With SSBU_RB_SIM=<a very large number> no rollback ever happens, so this feeds the game exactly the pads that a perturbed run
 * (SSBU_RB_SIM_PERTURB; its log lists "perturbed pad ... (pass N)") ended up using, with no rollback at all: if the game then fails the same way,
 * the failure comes from those inputs and not from the restore of a state. */
typedef struct { long long pass; uint64_t b; int lx, ly; } ForcedPad;
static ForcedPad g_forced[128]; static int g_nforced = -1;
static void rgs_forced_pad(long long pass, uint64_t *b, int *lx, int *ly) {
    if (g_nforced < 0) {
        g_nforced = 0; const char *e = getenv("SSBU_RB_SIM_FORCEPADS");
        for (const char *s = e; s && *s && g_nforced < 128;) {
            long long ps; unsigned long long bb; int x, y, n = 0;
            if (sscanf(s, "%lld:%llx:%d:%d%n", &ps, &bb, &x, &y, &n) != 4) break;
            g_forced[g_nforced].pass = ps; g_forced[g_nforced].b = bb; g_forced[g_nforced].lx = x; g_forced[g_nforced].ly = y; g_nforced++;
            s += n; while (*s == ',' || *s == ' ') s++;
        }
        if (g_nforced) LOG("r1: RB SIM: %d forced pads (SSBU_RB_SIM_FORCEPADS), first at pass %lld\n", g_nforced, g_forced[0].pass);
    }
    for (int i = 0; i < g_nforced; i++) if (g_forced[i].pass == pass) {
        *b = g_forced[i].b; *lx = g_forced[i].lx; *ly = g_forced[i].ly;
        LOG("r1: RB SIM forced pad at pass %lld: buttons %llx, left stick %d %d\n", pass, (unsigned long long)*b, *lx, *ly);
        return;
    }
}
void rgs_pad(uint64_t *b, int *lx, int *ly, int *rx, int *ry) {
    if (rbg_on()) { rbg_pad(0, b, lx, ly, rx, ry); return; }      /* R3 */
    long long p = g_rgs_next; int x = (int)(p & 255);
    if (g_ra_phase == 2) {
        if (g_rgs_padf[x] == p) { PadRec r = g_rgs_pad[x]; *b = r.b; *lx = r.lx; *ly = r.ly; *rx = r.rx; *ry = r.ry; rgs_perturb_pad(b, lx, ly); } else g_rgs_padmiss++;
    } else { rgs_forced_pad(p, b, lx, ly); PadRec r = {*b, *lx, *ly, *rx, *ry}; g_rgs_pad[x] = r; g_rgs_padf[x] = p; }
}

static double rg_pct(const double *v, int n, int p) {
    if (n <= 0) return 0;
    double *w = malloc((size_t)n * sizeof *w); memcpy(w, v, (size_t)n * sizeof *w); qsort(w, (size_t)n, sizeof *w, cmp_d);
    double r = w[(size_t)n * p / 100 < (size_t)n ? (size_t)n * p / 100 : (size_t)n - 1]; free(w); return r;
}
static double rg_mean(const double *v, int n) { double s = 0; for (int i = 0; i < n; i++) s += v[i]; return n ? s / n : 0; }
void rgs_report(void) {
    sm_report(1);                                         /* (SSBU_STATE_MEASURE) */
    ls_report();                                          /* R4 */
    if (!g_rg_ready) { if (rbg_on()) rbg_report(); return; }      /* (hs14: a match without the ring has its report at the exit too) */
    rbg_report();
    LOG("r1: RING memory (hs14, fix %s): captures given up for lack of pool memory %lld (pages put back and marked again), old states freed early %lld, captures skipped to keep the oldest needed state %lld; test refusals %lld\n",
        g_rg_poolfix ? "on" : "OFF", g_rg_short_n, g_rg_freed_slots, g_rg_skip_full, g_rg_tp_refused);
    int n = g_rgc_n; double pg = 0, xp = 0; for (int i = 0; i < n; i++) { pg += (double)g_rgc_pg[i]; xp += (double)g_rgc_xp[i]; }
    LOG("r1: RING report: %lld captures (%lld skipped: a thread not at a wait), %lld loads, %u pool buffers made (%.1f MB), %u free; log overflows %lld\n",
        g_rg_caps, g_rg_cap_fail, g_rg_loads, g_rg_bump, g_rg_bump * (double)PG / 1048576.0, g_rg_nfree, g_rg_overflow);
    LOG("r1: RING loads right after a capture with the threads kept held: %lld (threads kept at the same wait: %lld); fibers %d (last); stale fiber-block notes refused %lld\n", g_rg_kept_n, g_rg_kept_same, g_guest_fibers.count, g_r1_fls_stale);
    LOG("r1: RING capture: median %.3f mean %.3f p99 %.3f ms; parts (means): hold %.3f, sweep + undo log %.3f, page copy (parallel) %.3f, fibers + threads + command buffers + let go %.3f; pages %.0f + extras %.0f mean (%.1f MB)\n",
        rg_pct(g_rgc[0], n, 50), rg_mean(g_rgc[0], n), rg_pct(g_rgc[0], n, 99), rg_mean(g_rgc[1], n), rg_mean(g_rgc[2], n), rg_mean(g_rgc[3], n), rg_mean(g_rgc[4], n), n ? pg / n : 0, n ? xp / n : 0, n ? (pg + xp) / n * PG / 1048576.0 : 0);
    {   long long w = 0; for (int q = 0; q < 9; q++) w += g_cp_wrote[q];
        LOG("r1: RING loads (net3, SSBU_RB_LOADCMP): pages to load %lld, of them written (they differed) %lld (%.1f%%)\n", g_rg_ldpg, w, g_rg_ldpg ? 100.0 * (double)w / (double)g_rg_ldpg : 0.0); }
    for (int k = 0; k < RG_MAXSLOT; k++) {
        int m = g_rgl_n[k]; if (!m) continue;
        static double v[RG_NL], h[RG_NL], cpy[RG_NL]; double p = 0;
        for (int i = 0; i < m; i++) { v[i] = g_rgl[k][i][0]; h[i] = g_rgl[k][i][1]; cpy[i] = g_rgl[k][i][2]; p += (double)g_rgl_pg[k][i]; }
        LOG("r1: RING load k %d (%d): median %.3f mean %.3f p99 %.3f ms (hold %.3f, undo + page copy %.3f, rest %.3f); pages to load %.0f mean (%.1f MB)\n",
            k, m, rg_pct(v, m, 50), rg_mean(v, m), rg_pct(v, m, 99), rg_mean(h, m), rg_mean(cpy, m), rg_mean(v, m) - rg_mean(h, m) - rg_mean(cpy, m), p / m, p / m * PG / 1048576.0);
    }
    if (g_rgs_on > 0) {
        if (g_rgv_on > 0) rgv_blocks_report();
        if (g_rgv_on > 0) LOG("r1: RB VERIFY: %lld rollbacks checked, %lld with window pages differing; pages compared %lld; differ: window 1 %lld, window 2 %lld, extras %lld\n", g_rgv_runs, g_rgv_bad_runs, g_rgv_pages, g_rgv_diff[0], g_rgv_diff[1], g_rgv_diff[2]);
        rgs_lite_report();
        if (g_rgpg_on > 0) {
            static struct { uint64_t blk; long long n[3]; } c[RGPG_N]; memcpy(c, g_rgpg, sizeof c);
            for (int i = 0; i < RGPG_N; i++) for (int j = i + 1; j < RGPG_N; j++) if (c[j].n[0] > c[i].n[0]) { uint64_t b = c[i].blk; long long n0 = c[i].n[0], n1 = c[i].n[1], n2 = c[i].n[2]; c[i] = c[j]; c[j].blk = b; c[j].n[0] = n0; c[j].n[1] = n1; c[j].n[2] = n2; }
            double t0 = 0, t1 = 0; for (int i = 0; i < RGPG_N; i++) { t0 += c[i].n[0]; t1 += c[i].n[1]; }
            LOG("r1: RB PAGES captures real %lld / re-run skipped %lld / last re-run %lld; pages per capture real %.0f, re-run skipped %.0f\n", g_rgpg_caps[0], g_rgpg_caps[1], g_rgpg_caps[2], t0 / (g_rgpg_caps[0] ? g_rgpg_caps[0] : 1), t1 / (g_rgpg_caps[1] ? g_rgpg_caps[1] : 1));
            for (int i = 0; i < 60 && c[i].n[0]; i++) LOG("r1: RB PAGES block %s (1 MB): per capture real %.1f, re-run skipped %.1f, last re-run %.1f\n", where((uint8_t *)(uintptr_t)(c[i].blk << 20)),
                c[i].n[0] / (double)(g_rgpg_caps[0] ? g_rgpg_caps[0] : 1), c[i].n[1] / (double)(g_rgpg_caps[1] ? g_rgpg_caps[1] : 1), c[i].n[2] / (double)(g_rgpg_caps[2] ? g_rgpg_caps[2] : 1));
        }
        rgp_report();
        rb_fault_report();
        { extern long long g_rg_audio_busy; extern int rb_audio_running(void); if (g_rg_audio_busy) LOG("r1: AUDIO: %lld loads waited for an audio thread to park (running now: %d)\n", g_rg_audio_busy, rb_audio_running()); }
        { extern volatile LONG64 g_xls_out_commit, g_xls_out_write; LOG("r1: PAGES OUTSIDE THE RING in fights so far: %lld committed, %lld written by the host (never saved or restored)\n", (long long)g_xls_out_commit, (long long)g_xls_out_write); }
        { extern volatile LONG64 g_hw_fight, g_hw_rerun; LOG("r1: HOST WRITES reported in fights so far: %lld, of them in re-run passes %lld (SSBU_RB_HOSTWRITES=1 lists them)\n", (long long)g_hw_fight, (long long)g_hw_rerun); }
        { void dm_audio_queue_report(const char *when); dm_audio_queue_report("in the fight"); }
        LOG("r1: RB SIM: %lld rollbacks (k: 1 %lld, 2 %lld, 3 %lld, 4 %lld, 5 %lld, 6 %lld, 7 %lld), captures failed %lld, re-run pads missing %lld, passes with more than one poll %lld\n",
            g_rgs_done, g_rgs_ksum[1], g_rgs_ksum[2], g_rgs_ksum[3], g_rgs_ksum[4], g_rgs_ksum[5], g_rgs_ksum[6], g_rgs_ksum[7], g_rgs_nocap, g_rgs_padmiss, g_rgs_multi);
        LOG("r1: RB SIM passes (safe point to safe point, without the capture / load): real %lld mean %.3f median %.3f ms; re-run %lld mean %.3f median %.3f ms (kick skip mask in re-run passes but the last: %d)\n",
            g_rgs_npass[0], g_rgs_npass[0] ? g_rgs_pass[0] / g_rgs_npass[0] : 0, rg_pct(g_rgs_pass_v[0], g_rgs_pass_nv[0], 50), g_rgs_npass[1], g_rgs_npass[1] ? g_rgs_pass[1] / g_rgs_npass[1] : 0, rg_pct(g_rgs_pass_v[1], g_rgs_pass_nv[1], 50), g_rgs_skip);
        LOG("r1: RB SIM main-thread CPU per pass (safe point to safe point, without the capture / load; QueryThreadCycleTime): real %.3f, re-run %.3f million cycles\n",
            g_rgs_npass[0] ? g_rgs_cyc[0] / g_rgs_npass[0] / 1e6 : 0, g_rgs_npass[1] ? g_rgs_cyc[1] / g_rgs_npass[1] / 1e6 : 0);
        LOG("r1: RB SIM re-run passes but the last (the replayed frames a rollback pays for): %lld mean %.3f median %.3f ms; last re-run passes %lld mean %.3f median %.3f ms\n", g_rgs_npl[0], g_rgs_npl[0] ? g_rgs_pl[0] / g_rgs_npl[0] : 0, rg_pct(g_rgs_plv[0], g_rgs_nplv[0], 50), g_rgs_npl[1], g_rgs_npl[1] ? g_rgs_pl[1] / g_rgs_npl[1] : 0, rg_pct(g_rgs_plv[1], g_rgs_nplv[1], 50));
        for (int k = 0; k < 3; k += 2) if (g_rapn[k]) LOG("r1: RB SIM %s passes (%lld), means ms: pass start -> poll %.3f, poll -> frame end %.3f, frame end call %.3f, frame end -> safe point %.3f\n",
            k == 0 ? "real" : "re-run", g_rapn[k], g_rap[k][0] / g_rapn[k], g_rap[k][1] / g_rapn[k], g_rap[k][2] / g_rapn[k], g_rap[k][3] / g_rapn[k]);
    }
}
