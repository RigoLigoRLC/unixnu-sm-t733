// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void t733_thermal_start(void);
// Zero requests the lowest legal ordinary LUT row; UINT32_MAX removes the cap.
uint32_t t733_thermal_cap_khz(unsigned domain);
void t733_thermal_dump_stats(void);

#ifdef __cplusplus
}
#endif
