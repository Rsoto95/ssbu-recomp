/* THE POSIX PAGE TRACKER for the snapshot ring. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Window 1 (the game's big address region) is watched with GetWriteWatch: the reservation was made with MEM_WRITE_WATCH
 * (nnsdk.c os_AllocateAddressRegion), and the Linux platform layer implements the watch on userfaultfd. Window 2 (image +
 * modules) is compared page by page, and the extras (everything outside the windows) are compared when the write watch
 * says they were written (or every time when their range could not be watched).
 *
 * This file also owns the pieces the Windows build keeps in tracking_experiments/write_marks.c: the shadow of the first
 * full save, the extras' flat copies, the page-copy jobs and the hooks whose only job here is to keep the linker happy or
 * to answer "off" (marks, protection debugging, fault recovery, presentation pool). The real state save/restore is
 * full_save.c / threads.c; the ring is snapshot_ring.c.
 */
#include "game_state_internal.h"
#include <string.h>
#include <stdlib.h>

/* ---- the windows, the shadow and the maps (write_marks.c on Windows) ---- */
uint8_t *g_win1; uint64_t g_wn1;
uint8_t *g_sh1, *g_sh1c, *g_sh2, *g_sh2c;
uint8_t *g_dc, *g_ns1, *g_ns2;
Extra g_x[8192]; int g_nx; size_t g_xbytes;
int g_prot_on, g_prot = -1, g_mcheck;
Ctx g_mctx;
int g_full_sweep = 1;                       /* the POSIX scan does not use the mark block lists */
uint8_t *g_blk1; volatile LONG g_blk1_dirty;
uint32_t *g_blk1_list, *g_blk2_list; size_t g_blk1_n, g_blk2_n;
long long g_miss_total, g_miss_remarked; int g_miss_cycles;
double g_ms_s[4096], g_ms_r[4096], g_ms_scan[4096]; size_t g_pg_s[4096], g_pg_r[4096]; int g_nms, g_nmr;

uint8_t **pm1, **pm2; uint64_t *plo1, *pn1, *pn2;
int g_ls_active;
volatile LONG64 g_read_retry;
long long g_xls_caps, g_xls_jobs, g_xls_loads, g_xls_loadpages;
volatile LONG64 g_xls_out_commit, g_xls_out_write;
#define XLS_OUT_MAX 65536
uint64_t g_xls_out_pg[XLS_OUT_MAX]; volatile LONG g_xls_out_npg;
static void xls_out_record(uint64_t pg) {
    LONG n = g_xls_out_npg;
    for (LONG i = 0; i < n && i < XLS_OUT_MAX; i++) if (g_xls_out_pg[i] == pg) return;
    LONG k = InterlockedIncrement(&g_xls_out_npg) - 1;
    if (k < XLS_OUT_MAX) g_xls_out_pg[k] = pg;
}
int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static size_t g_ls_caps, g_ls_loads, g_ls_dirty, g_ls_dirty2, g_ls_overflow;

void blk1_add(size_t p0, size_t p1) { (void)p0; (void)p1; }
void blk1_rebuild(void) {}
void blk1_init(void) {}

uint8_t *in_shadow(uint8_t *a, uint8_t **cmap, size_t *pi) {
    if (g_win1 && a >= g_win1 && a < g_win1 + g_wn1) { *pi = (size_t)(a - g_win1) >> 12; *cmap = g_sh1c; return g_sh1 + (a - g_win1); }
    if ((uintptr_t)a >= WIN2 && (uintptr_t)a < WIN2 + WN2) { *pi = (size_t)((uintptr_t)a - WIN2) >> 12; *cmap = g_sh2c; return g_sh2 + ((uintptr_t)a - WIN2); }
    return NULL;
}
int sh_commit(uint8_t *sh, uint8_t *cmap, size_t pi) {
    if (!sh || !cmap || cmap[pi]) return sh != NULL;
    size_t c0 = pi & ~(size_t)15;
    if (!VirtualAlloc(sh - (pi - c0) * PG, 16 * PG, MEM_COMMIT, PAGE_READWRITE)) return 0;
    memset(cmap + c0, 1, 16);
    return 1;
}
int mark_bind(void) { return 0; }             /* no marked game DLL on Linux */
int ls_use_exe_maps(void) {
    pm1 = &r1_m1; pm2 = &r1_m2; plo1 = &r1_lo1; pn1 = &r1_n1; pn2 = &r1_n2;
    return 1;
}

/* ---- the page ids of the two windows ---- */
static struct { uint8_t *a; size_t n; } g_win1r[4096]; static int g_nwin1r;
static struct { uint8_t *a; size_t n; } g_win2[1024]; static int g_nwin2;
static uint32_t *g_pending; static size_t g_npending, g_npending_cap;
static volatile LONG g_pending_overflow;
static SRWLOCK g_pending_lock = SRWLOCK_INIT;
static PVOID *g_addrs; static size_t g_addrs_cap;
static uint8_t *g_zero_page, *g_taken; static size_t g_taken_n;
static void xls_setup(void);

static int win1_id(const uint8_t *a, uint32_t *id) {
    if (!g_win1 || a < g_win1 || a >= g_win1 + g_wn1) return 0;
    *id = (uint32_t)((a - g_win1) >> 12); return 1;
}
static int win2_id(const uint8_t *a, uint32_t *id) {
    if ((uintptr_t)a < WIN2 || (uintptr_t)a >= WIN2 + WN2) return 0;
    *id = (uint32_t)(g_rg_n1 + (((uintptr_t)a - WIN2) >> 12)); return 1;
}
static void pending_add(uint32_t id) {
    AcquireSRWLockExclusive(&g_pending_lock);
    if (g_npending < g_npending_cap) g_pending[g_npending++] = id;
    else InterlockedExchange(&g_pending_overflow, 1);
    ReleaseSRWLockExclusive(&g_pending_lock);
}
void ls_range_event(uint64_t a, uint64_t n, int what) {
    (void)what;
    if (!g_ls_active || !n || !g_rg_n1) return;
    uint8_t *p = (uint8_t *)(uintptr_t)a, *e = (uint8_t *)(uintptr_t)(a + n);
    for (; p < e; p += PG) {
        uint8_t *q = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(PG - 1));
        uint32_t id;
        if (win1_id(q, &id)) { if (!g_dc || !g_dc[id]) pending_add(id); }
        else if (win2_id(q, &id)) pending_add(id);
        else if (what == 2) { InterlockedIncrement64(&g_xls_out_commit); xls_out_record((uint64_t)(uintptr_t)q); }
        else if (what == 0) { InterlockedIncrement64(&g_xls_out_write); xls_out_record((uint64_t)(uintptr_t)q); }
    }
    /* range events before g_rg_n1 exists are covered by the base scan */
}
void r1_ww_note(void *const *pages, size_t cnt) {
    if (!g_ls_active || !g_rg_n1) return;
    for (size_t i = 0; i < cnt; i++) {
        uint8_t *q = (uint8_t *)((uintptr_t)pages[i] & ~(uintptr_t)(PG - 1));
        uint32_t id;
        if (win1_id(q, &id)) pending_add(id); else if (win2_id(q, &id)) pending_add(id);
    }
}
void r1_ww_note_range(const void *p, size_t n) { ls_range_event((uint64_t)(uintptr_t)p, n, 0); }
int r1_ww_taken(const void *p, size_t n) {     /* did our window-1 poll consume write bits of this range? */
    if (!g_taken || !g_win1 || !n) return 0;
    uintptr_t lo = (uintptr_t)p & ~(uintptr_t)(PG - 1), hi = ((uintptr_t)p + n + PG - 1) & ~(uintptr_t)(PG - 1);
    if (lo < (uintptr_t)g_win1 || hi > (uintptr_t)g_win1 + g_wn1) return 0;
    int any = 0;
    for (uintptr_t a = lo; a < hi; a += PG) { size_t id = (a - (uintptr_t)g_win1) >> 12; if (__atomic_exchange_n(&g_taken[id], 0, __ATOMIC_ACQ_REL)) any = 1; }
    return any;
}

static void drain_pending(void (*fn)(uint32_t id, int w, size_t j)) {
    AcquireSRWLockExclusive(&g_pending_lock);
    for (size_t i = 0; i < g_npending; i++) {
        uint32_t id = g_pending[i];
        if (id < g_rg_n1) fn(id, 0, id);
        else fn(id, 1, id - g_rg_n1);
    }
    g_npending = 0;
    ReleaseSRWLockExclusive(&g_pending_lock);
}

static void scan_window1(void (*fn)(uint32_t id, int w, size_t j), int reset) {
    if (!g_rg_n1) return;
    for (int r = 0; r < g_nwin1r; r++) {
        size_t np = g_win1r[r].n / PG;
        if (!np) continue;
        if (!g_addrs || g_addrs_cap < np) continue;
        ULONG_PTR cnt = np; ULONG gran = 0;
        if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, g_win1r[r].a, g_win1r[r].n, g_addrs, &cnt, &gran)) {
            static int said;
            if (said++ < 4) LOG("r1: LS: GetWriteWatch failed on window 1 range %p + %.1f MB; every page of it is treated as written\n", (void *)g_win1r[r].a, g_win1r[r].n / 1048576.0);
            for (size_t p = 0; p < np; p++) {
                uint32_t id = (uint32_t)((g_win1r[r].a - g_win1) / PG + p);
                if (g_taken && id < g_taken_n) __atomic_store_n(&g_taken[id], 1, __ATOMIC_RELEASE);
                if (!g_dc[id] && !g_ns1[id]) fn(id, 0, id);
            }
            g_ls_overflow++;
            continue;
        }
        g_ls_dirty += cnt;
        for (ULONG_PTR i = 0; i < cnt; i++) {
            size_t id = (size_t)(((uint8_t *)g_addrs[i] - g_win1) >> 12);
            if (g_taken && id < g_taken_n) __atomic_store_n(&g_taken[id], 1, __ATOMIC_RELEASE);
            if (id < g_rg_n1 && !g_dc[id] && !g_ns1[id]) fn((uint32_t)id, 0, id);
        }
    }
}
static void scan_window2(void (*fn)(uint32_t id, int w, size_t j)) {
    if (!g_rg_n1) return;
    for (int r = 0; r < g_nwin2; r++) {
        for (size_t p = 0; p < g_win2[r].n; p += PG) {
            uint8_t *a = g_win2[r].a + p;
            size_t j = (size_t)((uintptr_t)a - WIN2) >> 12;
            if (g_ns2[j]) continue;
            uint32_t id = (uint32_t)(g_rg_n1 + j);
            uint8_t *src = rg_src(id);
            g_ls_dirty2++;
            if (src == (uint8_t *)1) { fn(id, 1, j); continue; }
            if (!src) src = g_zero_page;
            if (!src || memcmp(a, src, PG)) fn(id, 1, j);
        }
    }
}
void ls_start_after_base(void) {
    g_win1r[0].a = NULL; g_nwin1r = 0;
    for (int i = 0; i < g_memory_map.range_count && g_nwin1r < 4096; i++) {
        uint8_t *a = g_memory_map.ranges[i].a, *b = a + g_memory_map.ranges[i].n;
        if (!g_win1 || b <= g_win1 || a >= g_win1 + g_wn1) continue;
        if (a < g_win1) a = g_win1;
        if (b > g_win1 + g_wn1) b = g_win1 + g_wn1;
        g_win1r[g_nwin1r].a = a; g_win1r[g_nwin1r].n = (size_t)(b - a); g_nwin1r++;
    }
    g_nwin2 = 0;
    for (int i = 0; i < g_memory_map.range_count && g_nwin2 < 1024; i++) {
        uint8_t *a = g_memory_map.ranges[i].a, *b = a + g_memory_map.ranges[i].n;
        if ((uintptr_t)b <= WIN2 || (uintptr_t)a >= WIN2 + WN2) continue;
        if ((uintptr_t)a < WIN2) a = (uint8_t *)(uintptr_t)WIN2;
        if ((uintptr_t)b > WIN2 + WN2) b = (uint8_t *)(uintptr_t)(WIN2 + WN2);
        g_win2[g_nwin2].a = a; g_win2[g_nwin2].n = (size_t)(b - a); g_nwin2++;
    }
    size_t maxp = 0;
    for (int i = 0; i < g_nwin1r; i++) if (g_win1r[i].n / PG > maxp) maxp = g_win1r[i].n / PG;
    for (int i = 0; i < g_nwin2; i++) if (g_win2[i].n / PG > maxp) maxp = g_win2[i].n / PG;
    if (maxp && maxp > g_addrs_cap) {
        PVOID *n = realloc(g_addrs, (maxp + 1) * sizeof *n);
        if (n) { g_addrs = n; g_addrs_cap = maxp; }
    }
    if (!g_rg_n1 && g_nwin1r) {                      /* the first base: without the OS write watch window 1 cannot be tracked */
        PVOID one; ULONG_PTR cnt = 1; ULONG gran = 0;
        if (GetWriteWatch(0, g_win1r[0].a, g_win1r[0].n < PG ? g_win1r[0].n : PG, &one, &cnt, &gran)) {
            LOG("r1: LS: window 1 has no GetWriteWatch (userfaultfd unavailable): the rollback ring is OFF\n");
            g_mark = 0; g_ls_active = 0; return;
        }
    }
    xls_setup();
    if (!g_zero_page) g_zero_page = calloc(1, PG);
    if (!g_taken || g_taken_n < (g_wn1 >> 12)) { free(g_taken); g_taken_n = (size_t)(g_wn1 >> 12); g_taken = calloc(g_taken_n, 1); }
    else memset(g_taken, 0, g_taken_n);
    if (!g_pending) {
        g_npending_cap = 1u << 20;
        g_pending = malloc(g_npending_cap * sizeof *g_pending);
        if (!g_pending) g_npending_cap = 0;
    }
    g_npending = 0; InterlockedExchange(&g_pending_overflow, 0);
    for (int i = 0; i < g_nwin1r; i++) ResetWriteWatch(g_win1r[i].a, g_win1r[i].n);
    g_ls_active = 1;
    mark_network_threads();      /* the windows exist now (the base was saved with the threads held): mark Pia/NetSystem */
    LOG("r1: LS (POSIX): windows: %d window-1 ranges, %d window-2 ranges; window 1 %p + %.1f MB\n",
        g_nwin1r, g_nwin2, (void *)g_win1, g_wn1 / 1048576.0);
}
void ls_new_base(void) { ls_start_after_base(); }
void ls_fight_over(void) { g_ls_active = 0; g_npending = 0; }
void ls_after_capture(void) {}
/* One capture can name the same page twice (a host write both in the pending list and in GetWriteWatch). The ring's undo
 * log must not get two entries for one page, so a small open-addressing set keeps the first. */
typedef struct { uint32_t id, gen; } CapSeen;
static CapSeen *g_seen; static uint32_t g_seen_gen;
#define SEEN_N (1u << 20)
static void fn_dedup(uint32_t id, int w, void (*fn)(uint32_t, int, size_t), size_t j) {
    if (!g_seen) { fn(id, w, j); return; }
    uint32_t h = (id * 2654435761u) & (SEEN_N - 1);
    for (uint32_t k = 0; k < 64; k++, h = (h + 1) & (SEEN_N - 1)) {
        CapSeen *s = &g_seen[h];
        if (s->gen != g_seen_gen) { s->gen = g_seen_gen; s->id = id; fn(id, w, j); return; }
        if (s->id == id) return;
    }
    fn(id, w, j);
}
static void (*g_cap_fn)(uint32_t, int, size_t);
static void cap_cb(uint32_t id, int w, size_t j) { fn_dedup(id, w, g_cap_fn, j); }
void ls_capture_pages(void (*fn)(uint32_t id, int w, size_t j)) {
    g_ls_caps++; g_ls_dirty = g_ls_dirty2 = 0;
    if (!g_seen) g_seen = calloc(SEEN_N, sizeof *g_seen);
    if (++g_seen_gen == 0) { memset(g_seen, 0, SEEN_N * sizeof *g_seen); g_seen_gen = 1; }
    g_cap_fn = fn;
    drain_pending(cap_cb);
    if (InterlockedExchange(&g_pending_overflow, 0)) {
        for (int i = 0; i < g_nwin2; i++) for (size_t p = 0; p < g_win2[i].n; p += PG) {
            size_t j = (size_t)((uintptr_t)(g_win2[i].a + p) - WIN2) >> 12;
            if (!g_ns2[j]) fn((uint32_t)(g_rg_n1 + j), 1, j);
        }
    }
    scan_window1(cap_cb, 1);
    scan_window2(cap_cb);
}
void ls_for_each_learned_page(void (*fn)(uint32_t id, int w, size_t j)) {
    g_ls_loads++;
    drain_pending(fn);
    scan_window1(fn, 1);
    scan_window2(fn);
}
void ls_report(void) {
    LOG("r1: LS report (POSIX): bases %zu, captures %zu (%zu window-1 pages, %zu window-2 pages compared), loads %zu (%lld extras pages); pending overflow %zu; GetWriteWatch fallbacks %zu\n",
        g_ls_caps, g_ls_caps, g_ls_dirty, g_ls_dirty2, g_ls_loads, g_xls_loadpages, g_ls_overflow, g_ls_overflow);
}

/* ---- the extras' write watch (LS-X) ---- */
typedef struct { uint8_t *a; size_t n, e0; int ww; } Xr;
#define XR_MAX 4096
static Xr g_xr[XR_MAX]; static int g_nxr; static size_t g_xn;
static uint8_t *g_xdirty;
void ppool_init(void) {}
uint8_t *ppool_bookkeeping(size_t *n) { if (n) *n = 0; return NULL; }
static void xls_setup(void) {
    free(g_xdirty); g_xdirty = NULL; g_nxr = 0; g_xn = 0;
    size_t total = 0;
    for (int i = 0; i < g_nx && g_nxr < XR_MAX; i++) {
        if (!g_x[i].n) continue;
        g_xr[g_nxr].a = g_x[i].a; g_xr[g_nxr].n = g_x[i].n; g_xr[g_nxr].e0 = total;
        g_xr[g_nxr].ww = plat_writewatch_add(g_x[i].a, g_x[i].n);
        total += g_x[i].n / PG;
        g_nxr++;
    }
    g_xn = total;
    if (!g_xn) return;
    g_xdirty = calloc(g_xn, 1);
    if (!g_xdirty) { g_xn = 0; return; }
    for (int i = 0; i < g_nxr; i++) {
        if (g_xr[i].ww) ResetWriteWatch(g_xr[i].a, g_xr[i].n);
        else memset(g_xdirty + g_xr[i].e0, 1, g_xr[i].n / PG);
    }
}
static PVOID *g_xaddrs; static size_t g_xaddrs_cap;
void xww_poll(void) {
    if (!g_xdirty) return;
    for (int i = 0; i < g_nxr; i++) {
        size_t np = g_xr[i].n / PG;
        if (!np) continue;
        if (!g_xr[i].ww) { memset(g_xdirty + g_xr[i].e0, 1, np); continue; }
        if (g_xaddrs_cap < np) { PVOID *n = realloc(g_xaddrs, (np + 1) * sizeof *n); if (!n) { memset(g_xdirty + g_xr[i].e0, 1, np); continue; } g_xaddrs = n; g_xaddrs_cap = np; }
        ULONG_PTR cnt = np; ULONG gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_xr[i].a, g_xr[i].n, g_xaddrs, &cnt, &gran)) { memset(g_xdirty + g_xr[i].e0, 1, np); continue; }
        for (ULONG_PTR k = 0; k < cnt; k++) g_xdirty[g_xr[i].e0 + (((uint8_t *)g_xaddrs[k] - g_xr[i].a) >> 12)] = 1;
    }
}
int xls_skip(size_t e) { if (!g_xdirty || e >= g_xn) return 0; return !g_xdirty[e]; }
void xls_decay(const uint32_t *xjob, const uint8_t *xdiff, size_t nxj) {
    (void)xdiff;
    if (!g_xdirty) return;
    for (size_t i = 0; i < nxj; i++) if (xjob[i] < g_xn) g_xdirty[xjob[i]] = 0;
}

/* ---- the parallel page copy / compare (write_marks.c on Windows) ---- */
CopyJob *g_cj; size_t g_ncj; static size_t g_cjcap;
int (*volatile cp_f_cmp)(uint8_t *, const uint8_t *) = NULL;
uint8_t *g_cp_cmp; int g_cp_cond; volatile long long g_cp_wrote[9];
volatile LONG64 g_cp_fault; volatile int g_cp_restore; volatile LONG64 g_cp_recommitted; int g_cp_defer;
static int cp_op_cmp(uint8_t *d, const uint8_t *s) { return memcmp(d, s, PG) != 0; }
static int cp_op_copy(uint8_t *d, const uint8_t *s) { memcpy(d, s, PG); return 1; }
static int cp_op_cond(uint8_t *d, const uint8_t *s) { if (memcmp(d, s, PG)) { memcpy(d, s, PG); return 1; } return 0; }
static int g_ncp = -1; static HANDLE g_cp_go[8], g_cp_done[8];
void cp_add(uint8_t *d, const uint8_t *s) {
    if (g_ncj >= g_cjcap) { size_t nc = g_cjcap ? g_cjcap * 2 : 65536; CopyJob *n = realloc(g_cj, nc * sizeof *n); if (!n) return; g_cj = n; g_cjcap = nc; }
    g_cj[g_ncj].d = d; g_cj[g_ncj].s = s; g_ncj++;
}
void cp_add_range(uint8_t *d, const uint8_t *s, size_t n) { for (size_t o = 0; o < n; o += PG) cp_add(d + o, s + o); }
static int page_ok(const uint8_t *p) {
    if (!p) return 0;
    uintptr_t u = (uintptr_t)p;
    if (g_win1 && u >= (uintptr_t)g_win1 && u < (uintptr_t)g_win1 + g_wn1) return 1;
    if (u >= WIN2 && u < WIN2 + WN2) return 1;
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery(p, &m, sizeof m) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
static int cp_recommit(uint8_t *d, const uint8_t *s) {
    if (g_cp_restore && page_ok(s) && VirtualAlloc(d, PG, MEM_COMMIT, PAGE_READWRITE)) {
        memcpy(d, s, PG); InterlockedIncrement64(&g_cp_recommitted); return 1;
    }
    InterlockedIncrement64(&g_cp_fault); return 0;
}
static void cp_one(size_t i) {
    uint8_t *d = g_cj[i].d; const uint8_t *s = g_cj[i].s;
    if (g_cp_cmp) { int diff = 0; if (page_ok(s) && page_ok(d)) diff = cp_op_cmp(d, s); else InterlockedIncrement64(&g_cp_fault); g_cp_cmp[i] = (uint8_t)diff; return; }
    if (!page_ok(s)) { InterlockedIncrement64(&g_cp_fault); return; }
    if (!page_ok(d)) { cp_recommit(d, s); return; }
    if (g_cp_cond) g_cp_wrote[0] += cp_op_cond(d, s);
    else cp_op_copy(d, s);
}
static void cp_part(int k) {
    size_t a = g_ncj * (size_t)k / (size_t)(g_ncp + 1), b = g_ncj * (size_t)(k + 1) / (size_t)(g_ncp + 1);
    if (g_cp_cmp && g_cp_cond) { long long w = 0; for (size_t i = a; i < b; i++) if (page_ok(g_cj[i].s) && page_ok(g_cj[i].d)) w += cp_op_cond(g_cj[i].d, g_cj[i].s); else InterlockedIncrement64(&g_cp_fault); g_cp_wrote[0] += w; return; }
    for (size_t i = a; i < b; i++) cp_one(i);
}
static DWORD WINAPI cp_thread(void *p) { int k = (int)(intptr_t)p; for (;;) { WaitForSingleObject(g_cp_go[k], INFINITE); cp_part(k + 1); SetEvent(g_cp_done[k]); } return 0; }
void cp_init(void) {
    if (!cp_f_cmp) cp_f_cmp = cp_op_cmp;
    if (g_ncp >= 0) return;
    const char *e = getenv("SSBU_RUNAHEAD_COPY_THREADS");
    g_ncp = e ? atoi(e) : rgs_wanted() ? 6 : 2;
    SYSTEM_INFO si; GetSystemInfo(&si); int nc = (int)si.dwNumberOfProcessors; if (g_ncp > nc - 2) g_ncp = nc > 2 ? nc - 2 : 0;
    if (g_ncp < 0) g_ncp = 0; if (g_ncp > 8) g_ncp = 8;
    for (int k = 0; k < g_ncp; k++) { g_cp_go[k] = CreateEventA(NULL, FALSE, FALSE, NULL); g_cp_done[k] = CreateEventA(NULL, FALSE, FALSE, NULL); HANDLE t = CreateThread(NULL, 0, cp_thread, (void *)(intptr_t)k, 0, NULL); SetThreadPriority(t, THREAD_PRIORITY_HIGHEST); CloseHandle(t); }
}
void cp_run(void) {
    if (g_ncp > 0 && g_ncj >= 64) { for (int k = 0; k < g_ncp; k++) SetEvent(g_cp_go[k]); cp_part(0); WaitForMultipleObjects((DWORD)g_ncp, g_cp_done, TRUE, INFINITE); }
    else { int keep = g_ncp; g_ncp = 0; cp_part(0); g_ncp = keep; }
    g_ncj = 0;
}

/* ---- mark / protection / debug / fast-save stubs ---- */
size_t mark_sweep(int dir) { (void)dir; return 0; }
void mark_save(Ctx *c) { save(c); }
void mark_restore(Ctx *c) { restore(c); }
void prot_all(int on) { (void)on; }
void prot_open(uint64_t a, uint64_t n) { (void)a; (void)n; }
void prot_report(void) {}
void mark_report(void) {}
LONG CALLBACK prot_veh(EXCEPTION_POINTERS *e) { (void)e; return 0; }
LONG CALLBACK r1_veh(EXCEPTION_POINTERS *e) { (void)e; return 0; }
void call_trace_init(void) {}
void call_trace_poll(void) {}
void sm_capture(long long f) { (void)f; }
void sm_load(long long f) { (void)f; }
void sm_report(int final) { (void)final; }
void rg_audit(void) {}
void rg_roundtrip_after_load(void) {}
size_t g_async_cap; uint8_t **g_async; size_t g_nfp;
int is_async(uint8_t *a) { (void)a; return 0; }
void async_add(uint8_t *a) { (void)a; }
void async_learn(void) {}
void fast_build(void) {}
void fast_save(Ctx *c) { save(c); }
void fast_restore(Ctx *c) { restore(c); }
void fast_report(void) {}

/* ---- host stacks inside the guest windows are not simulation state ---- */
long long g_hs_pages, g_hs_ranges, g_hs_fibers_later;
uint64_t *g_bf_lo, *g_bf_hi; size_t g_nbf, g_cbf; SRWLOCK g_bf_lock = SRWLOCK_INIT;
static void hs_exclude(uint64_t lo, uint64_t hi, const char *what) {
    if (!g_ns1 || !g_ns2 || hi <= lo || hi - lo > (1ull << 32)) return;
    long long n = 0;
    for (uint64_t p = lo & ~(uint64_t)(PG - 1); p < hi; p += PG) {
        if (g_win1 && p >= (uint64_t)(uintptr_t)g_win1 && p < (uint64_t)(uintptr_t)g_win1 + g_wn1) { size_t i = (size_t)(p - (uint64_t)(uintptr_t)g_win1) >> 12; if (!g_ns1[i]) { g_ns1[i] = 1; n++; } }
        else if (p >= WIN2 && p < WIN2 + WN2) { size_t i = (size_t)(p - WIN2) >> 12; if (!g_ns2[i]) { g_ns2[i] = 1; n++; } }
    }
    if (n) { g_hs_pages += n; g_hs_ranges++; LOG("r1: host stack inside a guest window left out of the state: %s %llx..%llx (%lld pages)\n", what, (unsigned long long)lo, (unsigned long long)hi, n); }
}
static void bf_add(void *fiber) {
    uintptr_t lo, hi;
    if (!plat_fiber_bounds(fiber, &lo, &hi)) return;
    AcquireSRWLockExclusive(&g_bf_lock);
    size_t i; for (i = 0; i < g_nbf; i++) if (g_bf_lo[i] == lo) { g_bf_hi[i] = hi; break; }
    if (i == g_nbf) {
        if (g_nbf == g_cbf) { size_t nc = g_cbf ? g_cbf * 2 : 1024; uint64_t *a = realloc(g_bf_lo, nc * 8), *b = a ? realloc(g_bf_hi, nc * 8) : NULL; if (a) g_bf_lo = a; if (b) { g_bf_hi = b; g_cbf = nc; } }
        if (g_nbf < g_cbf) { g_bf_lo[g_nbf] = lo; g_bf_hi[g_nbf] = hi; g_nbf++; }
    }
    ReleaseSRWLockExclusive(&g_bf_lock);
}
static void hs_fiber_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    (void)key; (void)c; (void)stack; (void)size;
    uintptr_t lo, hi;
    if (fiber && plat_fiber_bounds(fiber, &lo, &hi)) { bf_add(fiber); hs_exclude(lo, hi, "fiber stack"); }
}
static void hs_fiber_born(void *fiber) { uintptr_t lo, hi; if (fiber && plat_fiber_bounds(fiber, &lo, &hi)) { bf_add(fiber); hs_exclude(lo, hi, "fiber stack (new)"); g_hs_fibers_later++; } }
void r1_host_stack_note(uint64_t lo, uint64_t hi) { hs_exclude(lo, hi, "host stack (noted)"); }
void hs_exclude_all(void) {
    Ctx *tc; const char *nm; HANDLE h;
    for (int i = 0; guest_thread_info(i, &tc, &nm, &h); i++) if (h) {
        uintptr_t lo, hi;
        if (plat_thread_stack_bounds(h, &lo, &hi)) hs_exclude(lo, hi, "thread stack");
    }
    os_r1_fibers(hs_fiber_cb);
    g_r1_fiber_born = hs_fiber_born;
    LOG("r1: host stacks: %lld pages in %lld ranges inside the guest windows left out (%lld fibers made later)\n", g_hs_pages, g_hs_ranges, g_hs_fibers_later);
}

/* ---- after the first full save: shadow, extras, maps (write_marks.c mark_setup) ---- */
static void mark_undo(void) {
    if (g_sh1) VirtualFree(g_sh1, 0, MEM_RELEASE);
    if (g_sh2) VirtualFree(g_sh2, 0, MEM_RELEASE);
    free(g_sh1c); free(g_sh2c); free(g_dc); free(g_ns1); free(g_ns2);
    g_sh1 = g_sh2 = g_sh1c = g_sh2c = g_dc = g_ns1 = g_ns2 = NULL;
    for (int i = 0; i < g_nx; i++) { free(g_x[i].b); g_x[i].b = NULL; }
    g_nx = 0; g_xbytes = 0; g_win1 = NULL; g_wn1 = 0;
}
int mark_setup(void) {
    size_t big = 0; uint8_t *base = NULL;
    for (int i = 0; i < g_memory_map.heap_count; i++) {
        MEMORY_BASIC_INFORMATION m; uint8_t *p = g_memory_map.heap_base[i];
        while (VirtualQuery(p, &m, sizeof m) && m.AllocationBase == g_memory_map.heap_base[i] && m.RegionSize) p = (uint8_t *)m.BaseAddress + m.RegionSize;
        size_t span = (size_t)(p - g_memory_map.heap_base[i]);
        if (span > big && g_memory_map.heap_base[i] != g_ar) { big = span; base = g_memory_map.heap_base[i]; }
    }
    if (!base || big < (16ull << 30)) { LOG("r1: LS setup: no game address region >= 16 GB found: marks OFF\n"); return 0; }
    g_win1 = base; g_wn1 = big;
    g_sh1 = VirtualAlloc(NULL, g_wn1, MEM_RESERVE, PAGE_NOACCESS); g_sh1c = calloc(g_wn1 >> 12, 1);
    g_dc = calloc(g_wn1 >> 12, 1); g_ns1 = calloc(g_wn1 >> 12, 1); g_ns2 = calloc(WN2 >> 12, 1);
    g_sh2 = VirtualAlloc(NULL, WN2, MEM_RESERVE, PAGE_NOACCESS); g_sh2c = calloc(WN2 >> 12, 1);
    if (!g_sh1 || !g_sh1c || !g_dc || !g_ns1 || !g_ns2 || !g_sh2 || !g_sh2c) { LOG("r1: LS setup: shadow allocation FAILED\n"); mark_undo(); return 0; }
    hs_exclude_all();
    g_nx = 0; g_xbytes = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {                    /* the extras: runs of pages outside both windows */
        Rng *r = &g_memory_map.ranges[i];
        for (size_t p = 0; p < r->n / PG; p++) {
            uint8_t *cm; size_t pi;
            if (in_shadow(r->a + p * PG, &cm, &pi)) continue;
            if (g_nx < 8192) {
                if (g_nx && g_x[g_nx - 1].a + g_x[g_nx - 1].n == r->a + p * PG) g_x[g_nx - 1].n += PG;
                else { g_x[g_nx].a = r->a + p * PG; g_x[g_nx].n = PG; g_x[g_nx].b = NULL; g_nx++; }
            }
        }
    }
    for (int i = 0; i < g_nx; i++) {
        g_x[i].b = malloc(g_x[i].n); g_xbytes += g_x[i].n;
        if (!g_x[i].b) { LOG("r1: LS setup: allocation of %.1f KB for an extra FAILED\n", g_x[i].n / 1024.0); mark_undo(); return 0; }
    }
    size_t shb = 0, xo = 0; int cx = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {
        Rng *r = &g_memory_map.ranges[i]; size_t np = r->n / PG, d0 = 0;
        for (size_t p = 0; p < np; p++) {
            uint8_t *a = r->a + p * PG, *cm; size_t pi; uint8_t *sh = in_shadow(a, &cm, &pi);
            if (sh) {
                if (!sh_commit(sh, cm, pi)) { LOG("r1: LS setup: shadow commit FAILED after %.1f MB: OFF\n", shb / 1048576.0); mark_undo(); return 0; }
                memcpy(sh, r->s + p * PG, PG); shb += PG;
            } else {
                if (cx < g_nx && xo == g_x[cx].n) { cx++; xo = 0; }
                if (cx < g_nx && g_x[cx].a + xo == a) { memcpy(g_x[cx].b + xo, r->s + p * PG, PG); xo += PG; }
            }
            if (g_rs_drop && (p + 1 - d0 == 256 || p + 1 == np)) { VirtualFree(r->s + d0 * PG, (p + 1 - d0) * PG, MEM_DECOMMIT); d0 = p + 1; }
        }
    }
    uint8_t *m1 = VirtualAlloc(NULL, g_wn1 >> 12, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *m2 = VirtualAlloc(NULL, WN2 >> 12, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!m1 || !m2) { if (m1) VirtualFree(m1, 0, MEM_RELEASE); if (m2) VirtualFree(m2, 0, MEM_RELEASE); LOG("r1: LS setup: mark map allocation FAILED\n"); mark_undo(); return 0; }
    *pm1 = m1; *pm2 = m2; *plo1 = (uint64_t)(uintptr_t)g_win1; MemoryBarrier(); *pn2 = WN2; *pn1 = g_wn1;
    r1_m1 = m1; r1_m2 = m2; r1_lo1 = (uint64_t)(uintptr_t)g_win1; MemoryBarrier(); r1_n2 = WN2; r1_n1 = g_wn1;
    g_mctx = g_ctx_save;
    ls_start_after_base();
    LOG("r1: LS on: window 1 = %p, %.1f GB; window 2 = %llx + %.0f GB; shadow %.1f MB; extras %d ranges %.1f MB\n",
        (void *)g_win1, g_wn1 / 1073741824.0, WIN2, WN2 / 1073741824.0, shb / 1048576.0, g_nx, g_xbytes / 1048576.0);
    return 1;
}
