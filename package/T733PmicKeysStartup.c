// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <kern/startup.h>
#include "T733PmicKeys.h"

static void start_keys(const void *argument)
{
    (void)argument;
    t733_pmic_keys_start();
}

STARTUP_ARG(EARLY_BOOT, STARTUP_RANK_LAST, start_keys, 0);
