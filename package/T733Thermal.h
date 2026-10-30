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

enum T733GpuThermalFlags {
    T733GpuThermalHot = 1u << 0,
    T733GpuThermalStale = 1u << 1,
    T733GpuThermalComplete = 1u << 2,
};

// The GPU cap uses the existing TSENS worker's controller 1 sensors 1 and 2.
// A hot or stale group returns 315 MHz; complete readings at or below 90 C
// release the cap to 550 MHz. The caller selects and requests a legal GPU row.
// Optional outputs report the hottest currently valid sensor in milli-Celsius,
// its group's valid sensor bits, and T733GpuThermalFlags. With no valid sensor,
// temperatureMilliC retains the last complete reading or zero before sampling.
uint32_t native_adreno_thermal_cap_hz(int32_t *temperatureMilliC,
                                    uint32_t *validMask, uint32_t *flags);

#ifdef __cplusplus
}
#endif
