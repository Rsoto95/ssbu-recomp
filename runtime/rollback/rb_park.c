/* Cooperative parking of guest threads at SDK import boundaries. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Windows build suspends a thread and keeps it suspended inside a waiting import. On POSIX that is not possible, so
 * every guest thread stops itself at the entry or return of an SDK import while a park is requested. The controller
 * (threads.c park / runahead.c ra_hold_*) requests a park, waits until every slot is HELD, copies memory, then releases.
 * A thread that holds a guest mutex is not parked (a simulation thread may need that mutex): it reports MUTEX and the
 * hold is refused, exactly like a failed SuspendThread loop.
 */
#include <stdio.h>
#include <string.h>
#include "rb_park.h"

typedef struct { Ctx *c; volatile LONG state; volatile LONG let_go, hold_forever; volatile uintptr_t host_sp; void *volatile fiber; } RbSlot;
static RbSlot g_slot[256];
static volatile LONG g_nslot;
static volatile LONG g_park_on;
static DWORD g_park_owner;
static CONDITION_VARIABLE g_park_cv = CONDITION_VARIABLE_INIT;
static SRWLOCK g_park_lock = SRWLOCK_INIT;

int waiting_import(const char *s);
int os_mutexes_held_here(void);

static RbSlot *slot_of(Ctx *c) {
    int n = __atomic_load_n(&g_nslot, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) if (g_slot[i].c == c) return &g_slot[i];
    return NULL;
}

void rb_park_bind(Ctx *c) {
    if (!c) return;
    for (int i = 0; i < 256; i++) {
        Ctx *none = NULL;
        if (__atomic_compare_exchange_n(&g_slot[i].c, &none, c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_slot[i].state, RB_PARK_RUNNING, __ATOMIC_RELEASE);
            if (i >= __atomic_load_n(&g_nslot, __ATOMIC_ACQUIRE)) __atomic_store_n(&g_nslot, i + 1, __ATOMIC_RELEASE);
            return;
        }
        if (g_slot[i].c == c) return;
    }
    printf("r1: rb_park: more than 256 guest threads; no slot for %p\n", (void *)c);
}

void rb_park_unbind(Ctx *c) {
    RbSlot *s = slot_of(c);
    if (!s) return;
    __atomic_store_n(&s->state, RB_PARK_RUNNING, __ATOMIC_RELEASE);
    __atomic_store_n(&s->c, NULL, __ATOMIC_RELEASE);
}

void rb_park_request(void) {
    g_park_owner = GetCurrentThreadId();
    __atomic_store_n(&g_park_on, 1, __ATOMIC_RELEASE);
}

void rb_park_release(void) {
    AcquireSRWLockExclusive(&g_park_lock);
    __atomic_store_n(&g_park_on, 0, __ATOMIC_RELEASE);
    WakeAllConditionVariable(&g_park_cv);
    ReleaseSRWLockExclusive(&g_park_lock);
}

void rb_park_let_go(Ctx *c) {
    RbSlot *s = slot_of(c);
    if (!s) return;
    AcquireSRWLockExclusive(&g_park_lock);
    __atomic_store_n(&s->hold_forever, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->let_go, 1, __ATOMIC_RELEASE);
    WakeAllConditionVariable(&g_park_cv);
    ReleaseSRWLockExclusive(&g_park_lock);
}

void rb_park_hold_forever(Ctx *c) {
    RbSlot *s = slot_of(c);
    if (!s) return;
    __atomic_store_n(&s->hold_forever, 1, __ATOMIC_RELEASE);
}

void rb_park_gate(Ctx *c) {
    if (!__atomic_load_n(&g_park_on, __ATOMIC_ACQUIRE)) return;          /* one load when no park is in progress */
    if (GetCurrentThreadId() == g_park_owner) return;
    RbSlot *s = slot_of(c);
    if (!s) return;
    if (!waiting_import(c->in_import)) { __atomic_store_n(&s->state, RB_PARK_RUNNING, __ATOMIC_RELEASE); return; }
    if (os_mutexes_held_here()) { __atomic_store_n(&s->state, RB_PARK_MUTEX, __ATOMIC_RELEASE); return; }
    AcquireSRWLockExclusive(&g_park_lock);
    if (__atomic_load_n(&g_park_on, __ATOMIC_ACQUIRE)) {
        { volatile uintptr_t here = (uintptr_t)&here; s->host_sp = here; }
        s->fiber = GetCurrentFiber();
        __atomic_store_n(&s->state, RB_PARK_HELD, __ATOMIC_RELEASE);
        while ((__atomic_load_n(&g_park_on, __ATOMIC_ACQUIRE) || __atomic_load_n(&s->hold_forever, __ATOMIC_ACQUIRE)) && !__atomic_load_n(&s->let_go, __ATOMIC_ACQUIRE))
            SleepConditionVariableSRW(&g_park_cv, &g_park_lock, 1000, 0);
        __atomic_store_n(&s->let_go, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s->state, RB_PARK_RUNNING, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&g_park_lock);
}

int rb_park_parked(Ctx *c) { RbSlot *s = slot_of(c); return s ? (int)__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) : RB_PARK_RUNNING; }
int rb_park_owner(void) { return (int)g_park_owner; }
uintptr_t rb_park_host_sp(Ctx *c) { RbSlot *s = slot_of(c); return s ? __atomic_load_n(&s->host_sp, __ATOMIC_ACQUIRE) : 0; }
void *rb_park_fiber(Ctx *c) { RbSlot *s = slot_of(c); return s ? __atomic_load_n(&s->fiber, __ATOMIC_ACQUIRE) : NULL; }
