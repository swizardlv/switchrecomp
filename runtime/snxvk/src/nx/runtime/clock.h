/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's clock (clock.c): the host's monotonic time, counted from the run's start */
#ifndef SNX_CLOCK_H
#define SNX_CLOCK_H
#include "common/cpu/aarch64/cpu.h"

void clock_start(void); /* the run's start: guest time 0 */
u64 g_t0_ns(void);      /* plat_ns() at clock_start */
u64 guest_ns(void);     /* nanoseconds since clock_start */
u64 guest_ticks(void);  /* the same time as the console's 19.2 MHz system tick (svcGetSystemTick, CNTPCT_EL0) */

#endif
