// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <kern/startup.h>

extern void native_adreno_a6xx_start(void);

static void start_gpu(const void *argument)
{
    (void)argument;
    native_adreno_a6xx_start();
}

STARTUP_ARG(EARLY_BOOT, STARTUP_RANK_LAST, start_gpu, 0);
