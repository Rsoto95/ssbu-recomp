/* THE GUEST C HEAP inside the snapshot (SSBU_R1): malloc / new served from a reservation that the save rolls back with the rest.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "game_state_internal.h"

/* ---------------------------------------------------------------- the guest C heap inside the snapshot (SSBU_R1 only)
 * Normally guest malloc / new go to the host CRT heap (boot.c): that memory is outside every saved range, and a block
 * freed during the K frames would come back as a dangling pointer after the restore. With SSBU_R1 the malloc family
 * is served from one reservation whose allocator state (bump pointer, free lists) lives in its own first page, so the
 * save / restore rolls the guest C heap back with everything else. Power-of-two classes, 16-byte header before the
 * payload: [block start, class]. Blocks made before (by the CRT) are recognised by address and freed by the CRT. */

static SRWLOCK g_ar_lock = SRWLOCK_INIT;

static int in_arena(const void *p) { return g_ar && (const uint8_t *)p >= g_ar && (const uint8_t *)p < g_ar + AR_SIZE; }
static void *ar_alloc(size_t n, size_t align) {
    if (align < 16) align = 16;
    size_t need = n + 16 + (align > 16 ? align : 0);
    int cl = 0; while ((32ull << cl) < need) cl++;
    AcquireSRWLockExclusive(&g_ar_lock);
    uint8_t *b;
    if (ARH->head[cl]) { b = g_ar + ARH->head[cl]; ARH->head[cl] = *(uint64_t *)b; }
    else {
        uint64_t sz = 32ull << cl, off = (ARH->bump + 31) & ~31ull;
        if (off + sz > AR_SIZE) { ReleaseSRWLockExclusive(&g_ar_lock); return NULL; }
        if (off + sz > ARH->committed) {
            uint64_t to = (off + sz + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
            if (!VirtualAlloc(g_ar + ARH->committed, to - ARH->committed, MEM_COMMIT, PAGE_READWRITE)) { ReleaseSRWLockExclusive(&g_ar_lock); return NULL; }
            ARH->committed = to;
        }
        ARH->bump = off + sz; b = g_ar + off;
    }
    ARH->allocs++;
    ReleaseSRWLockExclusive(&g_ar_lock);
    uint8_t *p = (uint8_t *)(((uintptr_t)b + 16 + align - 1) & ~(uintptr_t)(align - 1));
    ((uint64_t *)p)[-2] = (uint64_t)(uintptr_t)b; ((uint64_t *)p)[-1] = (uint64_t)cl;
    return p;
}
static size_t ar_usable(void *p) { uint8_t *b = (uint8_t *)(uintptr_t)((uint64_t *)p)[-2]; return (32ull << ((uint64_t *)p)[-1]) - (size_t)((uint8_t *)p - b); }
static void ar_free(void *p) {
    if (!p) return;
    if (!in_arena(p)) { _aligned_free(p); return; }
    uint8_t *b = (uint8_t *)(uintptr_t)((uint64_t *)p)[-2]; int cl = (int)((uint64_t *)p)[-1];
    AcquireSRWLockExclusive(&g_ar_lock);
    *(uint64_t *)b = ARH->head[cl]; ARH->head[cl] = (uint64_t)(b - g_ar); ARH->frees++;
    ReleaseSRWLockExclusive(&g_ar_lock);
}

static void r_malloc(Ctx *c) { c->x[0] = (uint64_t)(uintptr_t)ar_alloc(RA0 ? RA0 : 1, 16); }
static void r_free(Ctx *c) { ar_free((void *)(uintptr_t)RA0); }
static void r_calloc(Ctx *c) { size_t n = (size_t)(RA0 * RA1); void *p = ar_alloc(n ? n : 1, 16); if (p) memset(p, 0, n); c->x[0] = (uint64_t)(uintptr_t)p; }
static void r_realloc(Ctx *c) {
    void *o = (void *)(uintptr_t)RA0; size_t n = RA1 ? (size_t)RA1 : 1;
    if (!o) { c->x[0] = (uint64_t)(uintptr_t)ar_alloc(n, 16); return; }
    size_t old = in_arena(o) ? ar_usable(o) : _aligned_msize(o, 16, 0);
    if (in_arena(o) && n <= old) { c->x[0] = RA0; return; }
    void *p = ar_alloc(n, 16);
    if (p) { memcpy(p, o, old < n ? old : n); ar_free(o); }
    c->x[0] = (uint64_t)(uintptr_t)p;
}
static void r_aligned_alloc(Ctx *c) { c->x[0] = (uint64_t)(uintptr_t)ar_alloc(RA1 ? RA1 : 1, RA0); }
static void r_new_aligned(Ctx *c) { c->x[0] = (uint64_t)(uintptr_t)ar_alloc(RA0 ? RA0 : 1, RA1); }
static void r_posix_memalign(Ctx *c) { void *p = ar_alloc(RA2 ? RA2 : 1, RA1); wr64(RA0, (uint64_t)(uintptr_t)p); c->x[0] = p ? 0 : 12; }
static void r_usable_size(Ctx *c) { void *p = (void *)(uintptr_t)RA0; c->x[0] = !p ? 0 : in_arena(p) ? ar_usable(p) : _aligned_msize(p, 16, 0); }

static void r1_mark_range(uint64_t a, uint64_t n) {
    if (!n || !r1_n1) return;
    for (uint64_t p = a & ~(uint64_t)(PG - 1); p < a + n; p += PG) {
        uint64_t o = p - r1_lo1;
        if (o < r1_n1) r1_m1[o >> 12] = 1;
        else { o = p - WIN2; if (o < r1_n2) r1_m2[o >> 12] = 1; }
    }
}
static void m_memcpy(Ctx *c) { r1_mark_range(c->x[0], c->x[2]); memcpy((void *)(uintptr_t)c->x[0], (void *)(uintptr_t)c->x[1], (size_t)c->x[2]); }
static void m_memmove(Ctx *c) { r1_mark_range(c->x[0], c->x[2]); memmove((void *)(uintptr_t)c->x[0], (void *)(uintptr_t)c->x[1], (size_t)c->x[2]); }
static void m_memset(Ctx *c) { r1_mark_range(c->x[0], c->x[2]); memset((void *)(uintptr_t)c->x[0], (int)c->x[1], (size_t)c->x[2]); }
static uint8_t *g_dc_unused;                           /* window 1: 1 = the game decommitted the page (no VirtualQuery per page: ~1 ms each in this region) */
static void dc_set(void *p, size_t n, int v) {
    if (!g_dc || !g_win1) return;
    uint8_t *a = (uint8_t *)p;
    if (a < g_win1 || a >= g_win1 + g_wn1) return;
    size_t i0 = (size_t)(a - g_win1) >> 12, i1 = (size_t)(a + n - g_win1 + PG - 1) >> 12;
    if (i1 > (g_wn1 >> 12)) i1 = g_wn1 >> 12;
    memset(g_dc + i0, v, i1 - i0);
}
void r1_on_decommit(void *p, size_t n) { r1_mark_range((uint64_t)(uintptr_t)p, n); ls_range_event((uint64_t)(uintptr_t)p, n, 1); dc_set(p, n, 1); }
void r1_on_commit(void *p, size_t n) {
    dc_set(p, n, 0);
    ls_range_event((uint64_t)(uintptr_t)p, n, 2);              /* R4: new pages of a fight read-only (their first write faults) */
    uint8_t *a = (uint8_t *)p;
    if (g_blk1 && n && a >= g_win1 && a < g_win1 + g_wn1) blk1_add((size_t)(a - g_win1) >> 12, ((size_t)(a - g_win1) + n + PG - 1) >> 12);      /* round 5: the sweep covers it */
}

void r1_read_retry_note(void) { InterlockedIncrement64(&g_read_retry); }      /* (R4) nnfs.c read_at: a read repeated after ERROR_NOACCESS */
const char *guest_thread_name_of(DWORD tid);        /* os_threads.c */
/* HOST WRITES DURING A FIGHT (diagnostic, always counted, logged only with SSBU_RB_HOSTWRITES=1): every host-side write into guest memory that reports itself here (the file reads of
 * nnfs.c, the pad poll of nnhid.c). A write that lands while a rollback is possible is state the ring may restore under an operation that is still going on: the thread that
 * issued it is restored to before the call, but the file position / queue it advanced is host state. The line says which thread, how much, and in which pass and phase. */
volatile LONG64 g_hw_fight, g_hw_rerun; static volatile LONG g_hw_lines;
void r1_wrote(uint64_t a, uint64_t n) {
    r1_mark_range(a, n); ls_range_event(a, n, 0); if (g_prot_on) prot_open(a, n);          /* host code that writes guest memory (nnhid.c, nnfs.c reads: the kernel's writes do not fault) */
    if (!g_rg_ready || !ra_fight_on()) return;           /* (ra_fight_on reads guest memory: not before the ring exists, e.g. the file reads at boot) */
    InterlockedIncrement64(&g_hw_fight); if (g_ra_phase == 2) InterlockedIncrement64(&g_hw_rerun);
    static int on = -1; if (on < 0) { const char *e = getenv("SSBU_RB_HOSTWRITES"); on = e && e[0] == '1'; }
    if (on && InterlockedIncrement(&g_hw_lines) <= 200) {
        const char *tn = guest_thread_name_of(GetCurrentThreadId());
        LOG("r1: HOST WRITE in a fight: thread %lu (%s) wrote %llu bytes at %llx, pass %lld%s\n", (unsigned long)GetCurrentThreadId(), tn ? tn : "not a guest thread", (unsigned long long)n, (unsigned long long)a,
            g_rgs_next, g_ra_phase == 2 ? " RE-RUN" : "");
    }
}
/* "not simulation": pages written by audio decoding etc.: not rolled back, not checked */
void r1_nosim(uint64_t a, uint64_t n) {
    if (!g_ns1 || !n) return;
    for (uint64_t p = a & ~(uint64_t)(PG - 1); p < a + n; p += PG) {
        uint64_t o = p - (uint64_t)(uintptr_t)g_win1;
        if (o < g_wn1) g_ns1[o >> 12] = 1; else { o = p - WIN2; if (o < WN2) g_ns2[o >> 12] = 1; }
    }
}
int is_nosim(uint8_t *a) {
    if (!g_ns1) return 0;
    if (g_win1 && a >= g_win1 && a < g_win1 + g_wn1) return g_ns1[(size_t)(a - g_win1) >> 12];
    if ((uintptr_t)a >= WIN2 && (uintptr_t)a < WIN2 + WN2) return g_ns2[(size_t)((uintptr_t)a - WIN2) >> 12];
    return 0;
}
#ifdef __linux__
/* A network thread ended: its guest stack is ordinary simulation memory again when the game reuses it. (Only the network
 * stack pages are cleared here; host stacks / audio pages are never reused as simulation memory.) */
void r1_sim_clear(uint64_t a, uint64_t n) {
    if (!g_ns1 || !n) return;
    for (uint64_t p = a & ~(uint64_t)(PG - 1); p < a + n; p += PG) {
        uint64_t o = p - (uint64_t)(uintptr_t)g_win1;
        if (o < g_wn1) g_ns1[o >> 12] = 0; else { o = p - WIN2; if (o < WN2) g_ns2[o >> 12] = 0; }
    }
}
#endif
/* nnsdk.c: a decommit changes the pages too */
void r1_register_heap(void) {
    if (r1_on() && (getenv("SSBU_R1_MARK") || ra_wanted())) {
        static const ImportDef md[] = { {"memcpy", m_memcpy}, {"memmove", m_memmove}, {"memset", m_memset} };
        register_imports(md, 3);
    }
    if (!r1_on() || getenv("SSBU_R1_CRT_HEAP")) return;
    g_ar = VirtualAlloc(NULL, AR_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_ar || !VirtualAlloc(g_ar, 1u << 20, MEM_COMMIT, PAGE_READWRITE)) { g_ar = NULL; LOG("r1: guest heap reservation FAILED\n"); return; }
    ARH->bump = 4096; ARH->committed = 1u << 20;
    static const ImportDef defs[] = {
        {"malloc", r_malloc}, {"free", r_free}, {"calloc", r_calloc}, {"realloc", r_realloc},
        {"aligned_alloc", r_aligned_alloc}, {"memalign", r_aligned_alloc}, {"posix_memalign", r_posix_memalign},
        {"malloc_usable_size", r_usable_size},
        {"_Znwm", r_malloc}, {"_Znam", r_malloc}, {"_ZdlPv", r_free}, {"_ZdaPv", r_free},
        {"_ZnwmRKSt9nothrow_t", r_malloc}, {"_ZnamRKSt9nothrow_t", r_malloc},
        {"_ZdlPvm", r_free}, {"_ZdaPvm", r_free}, {"_ZdlPvRKSt9nothrow_t", r_free}, {"_ZdaPvRKSt9nothrow_t", r_free},
        {"_ZnwmSt11align_val_t", r_new_aligned}, {"_ZnamSt11align_val_t", r_new_aligned},
        {"_ZdlPvSt11align_val_t", r_free}, {"_ZdaPvSt11align_val_t", r_free},
    };
    register_imports(defs, (int)(sizeof defs / sizeof defs[0]));
    LOG("r1: guest malloc / new served from a 32 GB reservation at %p (inside the snapshot)\n", (void *)g_ar);
}
