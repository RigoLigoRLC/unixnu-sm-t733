// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <kern/startup.h>

extern void t733_battery_start(void);

static void start_battery(const void *argument)
{
    (void)argument;
    t733_battery_start();
}

STARTUP_ARG(EARLY_BOOT, STARTUP_RANK_LAST, start_battery, 0);
