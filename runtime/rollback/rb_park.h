/* Cooperative parking of guest threads at SDK import boundaries (POSIX rollback). SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RB_PARK_H
#define RB_PARK_H
#include <stdint.h>
#include <stddef.h>
#include "hostrt.h"
#include "plat/windows.h"

enum { RB_PARK_RUNNING = 0, RB_PARK_HELD = 1, RB_PARK_MUTEX = 2 };

void rb_park_bind(Ctx *c);
void rb_park_unbind(Ctx *c);
void rb_park_gate(Ctx *c);
void rb_park_request(void);
void rb_park_release(void);
void rb_park_let_go(Ctx *c);        /* release one thread, the others stay held */
void rb_park_hold_forever(Ctx *c);  /* a thread born inside a rolled-back window: keep it at the gate */
int  rb_park_parked(Ctx *c);
int  rb_park_owner(void);
/* what the thread was doing when it stopped at the gate */
uintptr_t rb_park_host_sp(Ctx *c);
void *rb_park_fiber(Ctx *c);

#endif
