/* THE GUEST THREADS: the nn::os objects, parking every other guest thread at a wait while memory is copied, and the job system's fibers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "game_state_internal.h"
/* Functions and variables of other runtime files that only this file uses: */
extern DWORD g_r1_fls_idx;
void os_r1_fiber_set(uint64_t key, void *fiber, Ctx *fctx, int fresh);
void os_r1_fiber_get(uint64_t key, uint64_t v[6]);
void os_r1_fiber_put(uint64_t key, const uint64_t v[6]);
void *os_r1_fiber_fls(uint64_t key);
void os_r1_fiber_put_fls(uint64_t key, void *fiber, void *fls);
void *nnunwind_catch_table(size_t *bytes);
void *fighter_host_fls_slot(void);
void nvn_gl_r1_cb_area(void **cbs, void **ncbs, void **all);

/* ---------------------------------------------------------------- nn::os objects (compared, not restored) */

OsRec *g_os; int g_nos, g_oscap, g_os_diff, g_os_mode;
static OsRec *g_os2; static int g_nos2, g_os2cap;
void os_cb(uint64_t key, int sig, int cnt, uint64_t len, uint64_t thread, uint64_t stack, uint64_t size) {
    (void)stack; (void)size;
    if (thread) return;
    OsRec **arr = g_os_mode ? &g_os2 : &g_os; int *n = g_os_mode ? &g_nos2 : &g_nos, *cap = g_os_mode ? &g_os2cap : &g_oscap;
    if (*n >= *cap) { *cap = *cap ? *cap * 2 : 4096; *arr = realloc(*arr, (size_t)*cap * sizeof **arr); }
    OsRec r = {key, sig, cnt, len}; (*arr)[(*n)++] = r;
}
int cmp_os(const void *x, const void *y) { const OsRec *a = x, *b = y; return a->key < b->key ? -1 : a->key > b->key; }
void os_compare(const char *when) {
    g_os_mode = 1; g_nos2 = 0; os_r1_objs(os_cb); g_os_mode = 0;
    qsort(g_os2, (size_t)g_nos2, sizeof *g_os2, cmp_os);
    int diff = 0, shown = 0, j = 0;
    for (int i = 0; i < g_nos2; i++) {
        while (j < g_nos && g_os[j].key < g_os2[i].key) j++;
        int same = j < g_nos && g_os[j].key == g_os2[i].key && g_os[j].sig == g_os2[i].sig && g_os[j].cnt == g_os2[i].cnt && g_os[j].len == g_os2[i].len;
        if (!same) {
            diff++;
            if (shown++ < 12) LOG("r1:   os object %llx %s: signaled %d count %d queue %llu (at the save: %s %d %d %llu)\n", (unsigned long long)g_os2[i].key, when,
                                  g_os2[i].sig, g_os2[i].cnt, (unsigned long long)g_os2[i].len, j < g_nos && g_os[j].key == g_os2[i].key ? "" : "new",
                                  j < g_nos && g_os[j].key == g_os2[i].key ? g_os[j].sig : 0, j < g_nos && g_os[j].key == g_os2[i].key ? g_os[j].cnt : 0,
                                  (unsigned long long)(j < g_nos && g_os[j].key == g_os2[i].key ? g_os[j].len : 0));
        }
    }
    LOG("r1: nn::os objects %s: %d of %d differ from the save point (events / semaphores / queues; not restored)\n", when, diff, g_nos2);
    g_os_diff = diff;
}

static void threads_report(const char *when) {
    Ctx *tc; const char *nm; HANDLE h; char buf[4096]; int o = 0;
    for (int i = 0; guest_thread_info(i, &tc, &nm, &h) && o < 3900; i++)
        if (h) o += snprintf(buf + o, sizeof buf - o, " [%s: %s]", nm ? nm : "?", tc->in_import ? tc->in_import : "guest code");
    buf[o] = 0;
    LOG("r1: guest threads %s:%s\n", when, buf);
}

/* ---------------------------------------------------------------- the other guest threads: held at a wait while memory is copied */

/* (R3 hs16) cok: rsp / rip read by th_blocked while this thread is held (suspended by us ever since): live_rsps uses them
 * instead of asking Windows again (GetThreadContext of a thread blocked in the kernel costs ~20 us: 0.5 ms per capture) */
int g_audio_free;

GuestThreads g_guest_threads = { .park_ms = 500 };      /* the one thread table (see GuestThreads in game_state_internal.h) */
int waiting_import(const char *s) { return s && (strstr(s, "Wait") || strstr(s, "wait") || strstr(s, "Sleep") || strstr(s, "Receive") || strstr(s, "Fiber") || strstr(s, "Timed")); }
/* restore_mode 0: hold every guest thread (except this one) once it is inside a waiting import, note where;
 * restore_mode 1: hold each once it waits at the SAME guest return address as at the save (500 ms at most) */
static struct { HANDLE h; DWORD tid; int timeouts; } g_tmo[256]; static int g_ntmo;      /* (by thread id too: a handle value can be reused, see live_teb_addr) */
static int *tmo_of(HANDLE h) {
    DWORD tid = GetThreadId(h);
    for (int i = 0; i < g_ntmo; i++) if (g_tmo[i].h == h) { if (g_tmo[i].tid != tid) { g_tmo[i].tid = tid; g_tmo[i].timeouts = 0; } return &g_tmo[i].timeouts; }
    if (g_ntmo < 256) { g_tmo[g_ntmo].h = h; g_tmo[g_ntmo].tid = tid; g_tmo[g_ntmo].timeouts = 0; return &g_tmo[g_ntmo++].timeouts; }
    return NULL;
}
void park(int restore_mode) {
    DWORD me = GetCurrentThreadId();
    Ctx *tc; const char *nm; HANDLE h;
    if (!restore_mode) {
        g_guest_threads.count = 0;
        for (int i = 0; guest_thread_info(i, &tc, &nm, &h) && g_guest_threads.count < 256; i++) {
            if (!h || GetThreadId(h) == me) continue;
            Th *t = &g_guest_threads.table[g_guest_threads.count++]; memset(t, 0, sizeof *t); t->h = h; t->c = tc; t->name = nm ? nm : "?";
        }
        g_guest_threads.info_count = 0; while (guest_thread_info(g_guest_threads.info_count, &tc, &nm, &h)) g_guest_threads.info_count++;
    }
    g_guest_threads.mismatch = 0;
    for (int i = 0; i < g_guest_threads.count; i++) {
        Th *t = &g_guest_threads.table[i];
        double t0 = now_ms();
        t->held = 0; t->parked = 0; t->same = 0; t->wait_ms = 0;
        if (g_audio_free && (strstr(t->name, "nus3") || strstr(t->name, "udio") || strstr(t->name, "sindou"))) continue;   /* audio runs on: its memory is left out */
        int *tm = tmo_of(t->h);
        if (tm && *tm >= 2) { if (restore_mode) t->same = 0; continue; }     /* never reaches a wait (nopus decode): not held */
        for (;;) {
            if (SuspendThread(t->h) == (DWORD)-1) break;
            const char *imp = t->c->in_import;
            int ok = waiting_import(imp) && (!restore_mode || (t->c->import_from == t->from && imp == t->imp));
            if (ok || now_ms() - t0 > g_guest_threads.park_ms) { t->held = 1; t->cok = 0; t->parked = ok; if (!ok && tm) (*tm)++; t->wait_ms = now_ms() - t0; break; }
            ResumeThread(t->h);
            SwitchToThread();
        }
        if (!restore_mode) { t->from = t->c->import_from; t->imp = t->c->in_import; t->regs = *t->c; }
        else { t->same = t->parked; if (!t->same) g_guest_threads.mismatch++; }
    }
}
void slow_report(const char *when) {        /* which thread made the hold long */
    char b[1500]; int o = 0;
    for (int i = 0; i < g_guest_threads.count && o < 1400; i++) if (g_guest_threads.table[i].wait_ms > 1.0) o += snprintf(b + o, sizeof b - o, " [%s %.1f ms%s]", g_guest_threads.table[i].name, g_guest_threads.table[i].wait_ms, g_guest_threads.table[i].parked ? "" : " timeout");
    b[o] = 0;
    if (o) LOG("r1: hold %s: slow threads:%s\n", when, b);
}
void unpark(void) { for (int i = 0; i < g_guest_threads.count; i++) if (g_guest_threads.table[i].held) { ResumeThread(g_guest_threads.table[i].h); g_guest_threads.table[i].held = 0; } }
void restore_thread_regs(void) {     /* callee-saved registers + sp of each thread held at the same wait as at the save */
    for (int i = 0; i < g_guest_threads.count; i++) {
        Th *t = &g_guest_threads.table[i];
        if (!t->same || (t->imp && strstr(t->imp, "Fiber"))) continue;
        for (int r = 19; r < 32; r++) t->c->x[r] = t->regs.x[r];
        for (int r = 8; r < 16; r++) t->c->v[r] = t->regs.v[r];
    }
}
void park_report(const char *when) {
    char buf[6000]; int o = 0;
    for (int i = 0; i < g_guest_threads.count && o < 5800; i++)
        o += snprintf(buf + o, sizeof buf - o, " [%s %s%s @%llx]", g_guest_threads.table[i].name, g_guest_threads.table[i].parked ? "" : "NOT PARKED ", g_guest_threads.table[i].imp ? g_guest_threads.table[i].imp : "-", (unsigned long long)g_guest_threads.table[i].from);
    buf[o] = 0;
    LOG("r1: other guest threads %s (%d): %s\n", when, g_guest_threads.count, buf);
}

/* ---------------------------------------------------------------- the job system's fibers */
void os_r1_fibers(void (*cb)(uint64_t key, Ctx *c, uint64_t stack, uint64_t stack_size, void *fiber));
/* A fiber that is switched away (a script coroutine waiting for its next frame, a job waiting on a counter, a finished
 * fiber) keeps its host state in two places that are not guest memory: the Windows fiber block (saved registers:
 * CONTEXT at +0x30, StackBase +0x10, StackLimit +0x18) and its host stack from the saved RSP to StackBase. Both are
 * copied with its guest context (Ctx); a restore puts all three back when the fiber is switched away then too. */

GuestFibers g_guest_fibers = { .pool_cap = 256u << 20, .live_cache = -1 };      /* the one fiber save area (see GuestFibers in game_state_internal.h) */
static int g_fib_mode;      /* 0 while fibers_save runs, 1 while fibers_restore runs (fib_cb does the matching half) */
void os_r1_fiber_set(uint64_t key, void *fiber, Ctx *fctx, int fresh);
void os_r1_fiber_get(uint64_t key, uint64_t v[6]); void os_r1_fiber_put(uint64_t key, const uint64_t v[6]);
static struct { uint64_t key; void *fiber; Ctx *c; DWORD tid; } g_cur[2048]; static int g_ncur;
DWORD os_r1_fiber_tid(uint64_t key); void *os_r1_fiber_fls(uint64_t key);
void os_r1_fiber_put_fls(uint64_t key, void *fiber, void *fls); extern long long g_r1_fls_stale;      /* os_threads.c (R2: the block note restored with the fiber) */
extern DWORD g_r1_fls_idx;
/* fighter_host.c keeps a per-fiber block (FsFiber: module, script, ctx, depth ...) in a fiber-local slot; its index g_fs_fls is a
 * static there: its address is read from this exe's linker map. The first 72 bytes (mod, script, ctx, the counters) are
 * saved / restored with the fiber. */
static uint64_t g_fls_var;

/* (R3 pub1) The statics of other files that are part of the state (nnunwind.c's catch table, fighter_host.c's FLS index, nvn_gl.c's
 * command-buffer save area) were found by name in the exe's linker map (<exe>.map beside it). The prebuilt runtime package (setup.py
 * --rollback) ships ssbu_native_rb.exe WITHOUT its map (and an exe copied under another name has none either): every lookup gave 0,
 * so the catch table was never captured nor restored by the ring and a load put a coroutine's host stack back without the
 * record of its catch frame ("f_720052e2f0 handled an exception but was not entered through nro_catch_call", pub1h at frame 72).
 * Now the files give the addresses themselves; the map is not read any more (SSBU_R1_MAPSYM=1: the old way, test only). */
void *nnunwind_catch_table(size_t *bytes); void *fighter_host_fls_slot(void); void nvn_gl_r1_cb_area(void **cbs, void **ncbs, void **all);
static uint64_t map_sym_file(const char *want) {      /* a static of this exe, by its linker map (the old way) */
    char path[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH); if (n < 5) return 0;
    strcpy(path + n - 4, ".map");
    FILE *f = fopen(path, "r"); if (!f) { LOG("r1: %s: NO linker map %s\n", want, path); return 0; }
    char line[512]; uint64_t r = 0;
    while (fgets(line, sizeof line, f)) { char sec[32], name[128]; unsigned long long va; if (sscanf(line, " %31s %127s %llx", sec, name, &va) == 3 && !strcmp(name, want)) { r = (uint64_t)(uintptr_t)GetModuleHandleA(NULL) + va - 0x140000000ull; break; } }
    fclose(f);
    LOG("r1: %s at %llx (linker map)\n", want, (unsigned long long)r);
    return r;
}
uint64_t map_sym(const char *want) {
    { const char *e = getenv("SSBU_R1_MAPSYM"); if (e && e[0] == '1') return map_sym_file(want); }
    void *cbs, *ncbs, *all; nvn_gl_r1_cb_area(&cbs, &ncbs, &all);
    void *p = !strcmp(want, "g_catch") ? nnunwind_catch_table(NULL) : !strcmp(want, "g_fs_fls") ? fighter_host_fls_slot() :
              !strcmp(want, "g_cbs") ? cbs : !strcmp(want, "g_ncbs") ? ncbs : !strcmp(want, "g_all_cb") ? all : NULL;
    LOG("r1: %s at %llx (from the code)\n", want, (unsigned long long)(uintptr_t)p);
    return (uint64_t)(uintptr_t)p;
}
void fls_index_find(void) {
    static int tried; if (!tried) { tried = 1; g_fls_var = map_sym("g_fs_fls"); g_catch_addr = map_sym("g_catch"); }
    if (g_fls_var && g_r1_fls_idx == 0xFFFFFFFF && *(DWORD *)(uintptr_t)g_fls_var != 0xFFFFFFFF) g_r1_fls_idx = *(DWORD *)(uintptr_t)g_fls_var;
}

static int fib_switched_away(Ctx *c) { return !c->in_import || strstr(c->in_import, "SwitchToFiber") != NULL; }
static int fib_host(void *fiber, uint64_t *rsp, uint64_t *base) {
    uint8_t *f = fiber;
    uint64_t sb = *(uint64_t *)(f + 0x10), sl = *(uint64_t *)(f + 0x18), sp = *(uint64_t *)(f + 0x30 + 0x98);
    if (!(sl < sb && sp >= sl && sp < sb && sb - sp < (64u << 20))) return 0;
    *rsp = sp; *base = sb; return 1;
}
/* (R3 hs13, tl4 / nmr: c00000ff in ntdll's unwinder at a lua2cpp throw, rsp on one fiber's host stack while the thread's
 * TEB names another fiber) a restore must never copy a saved host stack over a stack some thread is running on now: the
 * stack pointers of every guest thread and of this thread are read first, and a switched-away fiber whose host stack
 * holds one of them is not copied back (counted, logged). SSBU_R1_LIVESTK=0: copied anyway (the old way, test). */
static int g_fib_live_on = -1;
static uint64_t g_live_rip[512], g_live_fib[512]; static const char *g_live_nm[512]; static HANDLE g_live_h[512];
static uint64_t live_teb_fiber(HANDLE h);
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h);
static void live_rsps(void) {
    if (g_guest_fibers.live_cache < 0) { const char *e = getenv("SSBU_R1_LIVECACHE"); g_guest_fibers.live_cache = !(e && e[0] == '0'); }      /* (hs16) =0: every thread asked again (the old way) */
    g_guest_fibers.live_count = 0;
    { volatile uint64_t here = (uint64_t)(uintptr_t)&here; g_guest_fibers.live_rsp[g_guest_fibers.live_count] = here; g_live_rip[g_guest_fibers.live_count] = 0; g_live_nm[g_guest_fibers.live_count] = "this thread"; g_live_h[g_guest_fibers.live_count] = NULL; g_live_fib[g_guest_fibers.live_count] = (uint64_t)(uintptr_t)GetCurrentFiber(); g_guest_fibers.live_count++; }
    Ctx *tc; const char *nm; HANDLE h;
    for (int i = 0; guest_thread_info(i, &tc, &nm, &h) && g_guest_fibers.live_count < 512; i++) {
        if (!h || GetThreadId(h) == GetCurrentThreadId()) continue;
        CONTEXT x; memset(&x, 0, sizeof x); x.ContextFlags = CONTEXT_CONTROL;
        int got = 0;
        if (g_guest_fibers.live_cache) for (int j = 0; j < g_guest_threads.count; j++) if (g_guest_threads.table[j].h == h) { if (g_guest_threads.table[j].held && g_guest_threads.table[j].cok) { x.Rsp = g_guest_threads.table[j].rsp; x.Rip = g_guest_threads.table[j].rip; got = 1; g_guest_fibers.live_cached++; } break; }
        if (!got) { got = GetThreadContext(h, &x) != 0; g_guest_fibers.live_asked++; }
        if (got) { g_guest_fibers.live_rsp[g_guest_fibers.live_count] = x.Rsp; g_live_rip[g_guest_fibers.live_count] = x.Rip; g_live_nm[g_guest_fibers.live_count] = nm ? nm : "?"; g_live_h[g_guest_fibers.live_count] = h; g_live_fib[g_guest_fibers.live_count] = live_teb_fiber(h); g_guest_fibers.live_count++; }
    }
}
static void live_teb(HANDLE h, uint64_t *fd, uint64_t *sb, uint64_t *sl) {     /* (diagnosis) that thread's TEB: FiberData, StackBase, StackLimit */
    *fd = *sb = *sl = 0;
    typedef LONG (WINAPI *QitFn)(HANDLE, int, void *, ULONG, ULONG *);
    static QitFn qit; if (!qit) qit = (QitFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    struct { LONG st; void *teb; void *pid, *tid; ULONG_PTR mask; LONG pri, bpri; } tb; memset(&tb, 0, sizeof tb);
    HANDLE hh = h ? h : GetCurrentThread();
    if (!qit || qit(hh, 0, &tb, sizeof tb, NULL) < 0 || !tb.teb) return;
    uint8_t *t = tb.teb; *sb = *(uint64_t *)(t + 0x08); *sl = *(uint64_t *)(t + 0x10); *fd = *(uint64_t *)(t + 0x20);
}
/* A thread's TEB, cached by handle AND thread id: (2026-10-30, two crashes of the room maker's game at the first match after a
 * player left the room and came back) the game ends network threads there and starts new ones; a new thread can get the
 * handle value of an ended one, whose TEB is freed: the cache by handle alone gave that TEB and reading it crashed. An entry
 * of another thread id is made again, and the TEB is read with ReadProcessMemory (an ended thread's freed TEB: no fault). */
static struct { HANDLE h; DWORD tid; uint8_t *teb; } g_teb_cache[512]; static int g_nteb;
static uint8_t *live_teb_addr(HANDLE h) {
    DWORD tid = GetThreadId(h);
    if (!tid) return NULL;
    int i;
    for (i = 0; i < g_nteb; i++) if (g_teb_cache[i].h == h) { if (g_teb_cache[i].tid == tid) return g_teb_cache[i].teb; break; }
    typedef LONG (WINAPI *QitFn)(HANDLE, int, void *, ULONG, ULONG *);
    static QitFn qit; if (!qit) qit = (QitFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    struct { LONG st; void *teb; void *pid, *tid; ULONG_PTR mask; LONG pri, bpri; } tb; memset(&tb, 0, sizeof tb);
    if (!qit || qit(h, 0, &tb, sizeof tb, NULL) < 0) return NULL;
    if (i == g_nteb && g_nteb < 512) g_nteb++;
    if (i < g_nteb) { g_teb_cache[i].h = h; g_teb_cache[i].tid = tid; g_teb_cache[i].teb = tb.teb; }
    return tb.teb;
}
static uint64_t live_teb_fiber(HANDLE h) {               /* that thread's current fiber (TEB + 0x20), 0 if it cannot be read */
    uint8_t *t = live_teb_addr(h);
    uint64_t v = 0;
    if (t && !ReadProcessMemory(GetCurrentProcess(), t + 0x20, &v, sizeof v, NULL)) v = 0;      /* (a thread ending right now: no fault) */
    return v;
}
static int live_running(void *fiber) {                 /* the host fiber is the current fiber of a thread (its TEB says so) */
    for (int i = 0; i < g_guest_fibers.live_count; i++) if (g_live_fib[i] && g_live_fib[i] == (uint64_t)(uintptr_t)fiber) return i + 1;
    return 0;
}
/* (2026-10-30, os_r1_fibers_reap) the current host fiber of every guest thread read again now; then r1_fiber_running(f) */
void r1_live_fibers_refresh(void) { live_rsps(); }
int r1_fiber_running(void *fiber) { return live_running(fiber) != 0; }
static int live_on_stack(const uint8_t *blk) {          /* a fiber block (saved copy): +0x20 DeallocationStack, +0x10 StackBase */
    uint64_t lo = *(const uint64_t *)(blk + 0x20), hi = *(const uint64_t *)(blk + 0x10);
    for (int i = 0; i < g_guest_fibers.live_count; i++) if (g_guest_fibers.live_rsp[i] >= lo && g_guest_fibers.live_rsp[i] < hi) return i + 1;
    return 0;
}
static void fib_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    DWORD tid = os_r1_fiber_tid(key);
    static int fiblog = -1; if (fiblog < 0) fiblog = getenv("SSBU_R1_FIBLOG") != NULL;
    if (fiblog) LOG("r1:   [%s] fiber %llx %s tid %lu fn %llx imp %s @%llx sp %llx stack %llx..%llx host %p ctx %p" "\n", g_fib_mode ? "R" : "S", (unsigned long long)key, !c ? "NEVER-RUN" : tid ? "RUNNING" : c->in_import ? "SWITCHED-AWAY" : "FINISHED", (unsigned long)tid, c ? (unsigned long long)c->fn : 0ull, c && c->in_import ? c->in_import : "-", c ? (unsigned long long)c->import_from : 0ull, c ? (unsigned long long)c->x[31] : 0ull, (unsigned long long)stack, (unsigned long long)(stack + size), fiber, (void *)c);
    if (g_fib_mode == 1) {                                    /* restore: the current host fiber / context of each key */
        if (g_ncur < 2048) { g_cur[g_ncur].key = key; g_cur[g_ncur].fiber = fiber; g_cur[g_ncur].c = c; g_cur[g_ncur].tid = tid; g_ncur++; }
        return;
    }
    if (g_guest_fibers.count >= 1024) return;
    Fib *f = &g_guest_fibers.table[g_guest_fibers.count++];
    f->key = key; f->c = c; f->fiber = fiber; f->susp = 0; f->stk = NULL; f->n = 0; f->imp = NULL; f->from = 0; f->sp = 0; os_r1_fiber_get(key, f->obj); f->fls = tid ? NULL : os_r1_fiber_fls(key); if (f->fls) memcpy(f->flsb, f->fls, sizeof f->flsb);
    if (!c) return;                                           /* initialised, never run */
    f->regs = *c; f->imp = c->in_import; f->from = c->import_from; f->sp = c->x[31];
    if (!tid && g_fib_live_on && fiber && live_running(fiber)) { tid = GetThreadId(g_live_h[live_running(fiber) - 1] ? g_live_h[live_running(fiber) - 1] : GetCurrentThread()); g_guest_fibers.teb_running++; }   /* (hs13) running in truth: its stack is live, not saved */
    uint64_t rsp, base;
    if (!tid && fib_host(fiber, &rsp, &base)) {
        size_t n = (size_t)(base - rsp) + 64;
        if (g_guest_fibers.pool_used + n <= g_guest_fibers.pool_cap) {
            f->stk = g_guest_fibers.pool + g_guest_fibers.pool_used; g_guest_fibers.pool_used += (n + 63) & ~(size_t)63;
            f->rsp = rsp - 64; f->n = n; memcpy(f->stk, (void *)(uintptr_t)f->rsp, n);
            memcpy(f->blk, fiber, FIB_BLOCK); f->susp = 1; g_guest_fibers.bytes += n + FIB_BLOCK + sizeof(Ctx);
        }
    } else if (!tid) g_guest_fibers.unreadable_stack++;
    if (tid) g_guest_fibers.running++;
}
static uint64_t g_rest_lo[1024], g_rest_hi[1024]; static int g_nrest;     /* (hs13) the host stacks the last restore copied back */
int rest_stack(uint64_t a) { for (int i = 0; i < g_nrest; i++) if (a >= g_rest_lo[i] && a < g_rest_hi[i]) return 1; return 0; }
void nnunwind_trace_note(const char *what, uint64_t cfa, uint64_t jb);      /* (hs15) nnunwind.c, SSBU_UNWIND_TRACE */
int g_r1_ctr = -1;

/* snapshot_ring.c load recorder, right after fibers_restore_all: fiber objects that exist now and not in the state just loaded (made after it) */
/* snapshot_ring.c rg_gstk_load: is some context that runs now, and was not put back by the load, using guest stack [lo, hi)? (a born fiber running now, or a fiber not put back) */
/* snapshot_ring.c rg_opg_load: the guest stack ranges of every fiber, in the state loaded and now (those pages are left to rg_gstk_load and its checks) */
int os_fiber_stack(uint64_t key, uint64_t *lo, uint64_t *hi);
int fibers_stack_ranges(uint64_t *lo, uint64_t *hi, int max) {
    int n = 0;
    for (int i = 0; i < g_guest_fibers.count && n < max; i++) { Fib *f = &g_guest_fibers.table[i]; if (f->obj[5]) { lo[n] = f->obj[4]; hi[n] = f->obj[4] + f->obj[5]; n++; } }
    for (int j = 0; j < g_ncur && n < max; j++) { uint64_t a, b; if (os_fiber_stack(g_cur[j].key, &a, &b)) { lo[n] = a; hi[n] = b; n++; } }
    return n;
}
int fibers_live_sp_in(uint64_t lo, uint64_t hi) {
    for (int j = 0; j < g_ncur; j++) {
        if (!g_cur[j].tid || !g_cur[j].c) continue;
        int i; for (i = 0; i < g_guest_fibers.count; i++) if (g_guest_fibers.table[i].key == g_cur[j].key) break;
        if (i < g_guest_fibers.count && g_guest_fibers.table[i].rok) continue;
        uint64_t sp = g_cur[j].c->x[31]; if (sp >= lo && sp < hi) return 1;
    }
    return 0;
}
int fibers_born_count(char *buf, size_t n) {
    int born = 0; size_t o = 0; if (n) buf[0] = 0;
    for (int j = 0; j < g_ncur; j++) {
        int i; for (i = 0; i < g_guest_fibers.count; i++) if (g_guest_fibers.table[i].key == g_cur[j].key) break;
        if (i < g_guest_fibers.count) continue;
        born++;
        if (o + 48 < n) o += (size_t)snprintf(buf + o, n - o, " %llx%s", (unsigned long long)g_cur[j].key, g_cur[j].tid ? "(running)" : "");
    }
    return born;
}
/* The fibers a load found born after the state it loaded (the last 64), for crash reports: was the fiber of a failing throw one of them? */
static uint64_t g_born_keys[64]; static long long g_born_pass[64]; static int g_nborn_keys;
const char *rb_born_info(uint64_t key, char *buf, size_t n) {
    for (int k = 0; k < 64 && k < g_nborn_keys; k++) if (g_born_keys[k] == key) { snprintf(buf, n, "BORN INSIDE A ROLLED-BACK WINDOW, reset at the load before pass %lld", g_born_pass[k]); return buf; }
    snprintf(buf, n, "not one of the %d fibers born inside a rolled-back window", g_nborn_keys < 64 ? g_nborn_keys : 64); return buf;
}
void fibers_restore_all(void) {
    g_nrest = 0;
    g_ncur = 0; g_fib_mode = 1; os_r1_fibers(fib_cb);
    g_guest_fibers.restored = g_guest_fibers.not_restored = g_guest_fibers.replaced = 0;
    if (g_fib_live_on < 0) { const char *e = getenv("SSBU_R1_LIVESTK"); g_fib_live_on = !(e && e[0] == '0'); }
    live_rsps();
    for (int i = 0; i < g_guest_fibers.count; i++) {
        Fib *f = &g_guest_fibers.table[i]; f->rok = 0;
        int j; for (j = 0; j < g_ncur; j++) if (g_cur[j].key == f->key) break;
        void *cf = j < g_ncur ? g_cur[j].fiber : NULL; Ctx *cc = j < g_ncur ? g_cur[j].c : NULL;
        os_r1_fiber_put(f->key, f->obj);                     /* entry, argument, stack, host fiber, context as at the save */
        if (f->susp || !f->c) os_r1_fiber_put_fls(f->key, f->fiber, f->susp ? f->fls : NULL);   /* (R2) the block note of the host fiber put back; none for a fresh one */
        if (!f->c) {                                          /* never ran at the save: it must start from the beginning */
            R1_CTR(cf != f->fiber || cc ? "restore: fiber never ran then: fresh host fiber (key, old fiber)" : "restore: fiber never ran then: same (key, fiber)", f->key, (uint64_t)(uintptr_t)cf);
            if (cf != f->fiber || cc) { os_r1_fiber_set(f->key, NULL, NULL, 1); g_guest_fibers.replaced++; } else g_guest_fibers.restored++;
            continue;
        }
        int running_now = j < g_ncur && g_cur[j].tid != 0;
        int live = f->susp && !running_now ? live_on_stack(f->blk) : 0;
        if (live) {
            g_guest_fibers.live_seen++;
            if (g_guest_fibers.live_seen <= 20) {
                uint64_t fd, sb, sl; live_teb(g_live_h[live - 1], &fd, &sb, &sl);
                LOG("r1:   fiber %llx: its host stack %p [%llx..%llx] is IN USE now by %s (rsp %llx rip %llx; its TEB: fiber %llx stack %llx..%llx); saved rsp %llx + %zu, host fiber now %p, run tid now %lu: %s\n",
                    (unsigned long long)f->key, f->fiber, (unsigned long long)*(uint64_t *)(f->blk + 0x20), (unsigned long long)*(uint64_t *)(f->blk + 0x10), g_live_nm[live - 1],
                    (unsigned long long)g_guest_fibers.live_rsp[live - 1], (unsigned long long)g_live_rip[live - 1], (unsigned long long)fd, (unsigned long long)sl, (unsigned long long)sb,
                    (unsigned long long)f->rsp, f->n, cf, j < g_ncur ? (unsigned long)g_cur[j].tid : 0ul, g_fib_live_on ? "not copied back" : "copied back anyway (SSBU_R1_LIVESTK=0)");
            }
            if (g_fib_live_on) { g_guest_fibers.live_skipped++; g_guest_fibers.not_restored++; R1_CTR("restore: fiber stack in use, not copied back (key, lo)", f->key, *(uint64_t *)(f->blk + 0x20)); continue; }
        }
        if (f->susp && !running_now) {                        /* whole host + guest state back */
            if (g_nrest < 1024) { g_rest_lo[g_nrest] = *(uint64_t *)(f->blk + 0x20); g_rest_hi[g_nrest] = *(uint64_t *)(f->blk + 0x10); g_nrest++; }
            R1_CTR("restore: fiber stack copied back (key, lo)", f->key, *(uint64_t *)(f->blk + 0x20));
            memcpy((void *)(uintptr_t)f->rsp, f->stk, f->n);
            memcpy(f->fiber, f->blk, FIB_BLOCK);
            if (f->fls) memcpy(f->fls, f->flsb, sizeof f->flsb);
            *f->c = f->regs;
            g_guest_fibers.restored++; f->rok = 2;      /* (2: switched away then and now: host stack, block, registers all the capture's) */
        } else if (!f->susp && cf == f->fiber && cc == f->c && cc->in_import == f->imp && cc->import_from == f->from && cc->x[31] == f->sp) {
            R1_CTR("restore: fiber running at the save: registers only (key, fiber)", f->key, (uint64_t)(uintptr_t)f->fiber);
            for (int r = 19; r < 32; r++) cc->x[r] = f->regs.x[r];
            for (int r = 8; r < 16; r++) cc->v[r] = f->regs.v[r];
            g_guest_fibers.restored++; f->rok = 1;
        } else {
            g_guest_fibers.not_restored++; R1_CTR("restore: fiber NOT restored (key, fiber)", f->key, (uint64_t)(uintptr_t)f->fiber);
            LOG("r1:   fiber %llx NOT restored: now %s, at the save %s @%llx sp %llx (switched away then: %d, same host fiber: %d)\n", (unsigned long long)f->key,
                running_now ? "running" : "?", f->imp ? f->imp : "guest code", (unsigned long long)f->from, (unsigned long long)f->sp, f->susp, cf == f->fiber);
        }
    }
    {   /* Fibers BORN since the state just loaded (they exist now, not in its table) and switched away now: in the state loaded they did not exist, so their host side goes back to "never set up"
         * (no host fiber, no context). Left as they were, they kept the host fiber and stack of their life after that state: a fiber object whose guest memory is reused by a coroutine made again
         * in the re-run then ran on that stale host side (a dense rollback run, 2026-10-28: a fiber born in a rolled-back window at 21c4699cf80, a reused address, and 24 frames later every re-run of
         * one pass failed the same way, beyond any reload). Set up again by the re-run, it gets a fresh host fiber (fib_init); used without that, the switch is refused (os_threads.c).
         * A born fiber that is RUNNING now is left alone (its thread is inside it). SSBU_R1_BORNRESET=0: off. */
        static int on = -1; if (on < 0) { const char *e = getenv("SSBU_R1_BORNRESET"); on = !(e && e[0] == '0'); }
        static long long nreset;
        if (on) for (int j = 0; j < g_ncur; j++) {
            int i; for (i = 0; i < g_guest_fibers.count; i++) if (g_guest_fibers.table[i].key == g_cur[j].key) break;
            if (i < g_guest_fibers.count || g_cur[j].tid) continue;
            os_r1_fiber_set(g_cur[j].key, NULL, NULL, 0);
            { int b = g_nborn_keys++ % 64; g_born_keys[b] = g_cur[j].key; g_born_pass[b] = g_rgs_next; }
            if (++nreset <= 20) LOG("r1: fiber %llx was born after the state loaded: its host side is reset to never set up (%lld so far)\n", (unsigned long long)g_cur[j].key, nreset);
        }
    }
}
int g_busy_depth, g_busy_log = 0, g_wait_idle;
static struct { uint64_t key, min_depth; } g_fd[2048]; static int g_nfd;
static void idle_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    (void)fiber;
    if (!c) return;
    if (!c->in_import || !strstr(c->in_import, "SwitchToFiber")) return;
    uint64_t d = stack + size - c->x[31];
    int i; for (i = 0; i < g_nfd; i++) if (g_fd[i].key == key) break;
    if (i == g_nfd) { if (g_nfd >= 2048) return; g_fd[g_nfd].key = key; g_fd[g_nfd].min_depth = d; g_nfd++; }
    if (d < g_fd[i].min_depth) g_fd[i].min_depth = d;
    if (d != g_fd[i].min_depth) {
        g_busy_depth++;
        if (g_busy_log > 0) {             /* the busy fiber's guest frame chain (x29 links) */
            char b[600]; int o = 0; uint64_t fp = c->x[29];
            for (int n = 0; n < 12 && fp >= c->x[31] && fp < stack + size && o < 560; n++) { o += snprintf(b + o, sizeof b - o, " %llx", (unsigned long long)rd64(fp + 8)); fp = rd64(fp); }
            b[o] = 0;
            LOG("r1:   busy fiber %llx depth %llx (idle %llx): x30 %llx, frames:%s\n", (unsigned long long)key, (unsigned long long)d, (unsigned long long)g_fd[i].min_depth, (unsigned long long)c->x[30], b);
        }
    }
}
/* every fiber that is switched away sits at its shallowest stack depth seen so far (the scheduler's top): no job half
 * done. Called at every frame end of the fight, so the shallowest depth is learnt before the first save. */
int fibers_idle(void) {
    g_busy_depth = 0;
    os_r1_fibers(idle_cb);
    return g_busy_depth == 0;
}
static int g_busy_run;
static void quiet_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    (void)key; (void)fiber;
    if (c && !c->in_import && c->x[31] != ((stack + size) & ~15ull)) g_busy_run++;       /* executing guest code right now */
}
/* hold the guest threads at a moment when no fiber is executing guest code (a worker thread looks parked in
 * SwitchToFiber while the fiber it runs may be in the middle of a job) */
static int g_park_retries;
void park_quiet(int restore_mode) {
    static int on = -1; if (on < 0) on = getenv("SSBU_R1_QUIET") != NULL;     /* opt-in: the 4p run r1t4 hung with it */
    if (!on) { park(restore_mode); return; }
    for (g_park_retries = 0; ; g_park_retries++) {
        park(restore_mode);
        g_busy_run = 0; os_r1_fibers(quiet_cb);
        if (!g_busy_run || g_park_retries >= 300) break;
        unpark(); SwitchToThread();
    }
}
void fibers_save(void) { if (g_fib_live_on < 0) { const char *e = getenv("SSBU_R1_LIVESTK"); g_fib_live_on = !(e && e[0] == '0'); } if (g_fib_live_on) live_rsps(); g_guest_fibers.count = 0; g_fib_mode = 0; g_ncur = 0; g_guest_fibers.pool_used = 0; g_guest_fibers.bytes = 0; g_guest_fibers.unreadable_stack = 0; g_guest_fibers.running = 0; os_r1_fibers(fib_cb); }
void fibers_restore(void) {
    fibers_restore_all();
    int born = 0; for (int j = 0; j < g_ncur; j++) { int i; for (i = 0; i < g_guest_fibers.count; i++) if (g_guest_fibers.table[i].key == g_cur[j].key) break; if (i == g_guest_fibers.count) born++; }
    Ctx *tc; const char *nm; HANDLE h; int nt = 0; for (int i = 0; guest_thread_info(i, &tc, &nm, &h); i++) nt++;
    if (g_nborn) LOG("r1: %d guest threads created in the window, held for good at the restore:%s\n", g_nborn, g_born);
    LOG("r1: in the window: %d fiber objects made that the save did not have; guest threads now %d (at the save %d + this one)\n", born, nt, g_guest_threads.count);
}
void fibers_report(void) {
    int shown = 0;
    for (int i = 0; i < g_guest_fibers.count; i++) {
        int dup = 0; for (int j = 0; j < i; j++) if (g_guest_fibers.table[j].imp == g_guest_fibers.table[i].imp && g_guest_fibers.table[j].from == g_guest_fibers.table[i].from) dup = 1;
        if (dup || shown++ > 12) continue;
        int n = 0; for (int j = i; j < g_guest_fibers.count; j++) if (g_guest_fibers.table[j].imp == g_guest_fibers.table[i].imp && g_guest_fibers.table[j].from == g_guest_fibers.table[i].from) n++;
        LOG("r1:   %d fibers in %s @%llx\n", n, g_guest_fibers.table[i].imp ? g_guest_fibers.table[i].imp : "guest code", (unsigned long long)g_guest_fibers.table[i].from);
    }
}

