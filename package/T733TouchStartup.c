// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <kern/startup.h>

extern void t733_touch_start(void);

static void
start_touch(const void *argument)
{
    (void)argument;
    t733_touch_start();
}

STARTUP_ARG(EARLY_BOOT, STARTUP_RANK_LAST, start_touch, 0);
