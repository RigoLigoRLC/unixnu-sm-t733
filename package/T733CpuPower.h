// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// T733CpuLoad contains cumulative scheduler ticks for one online processor.
typedef struct {
    uint32_t user;
    uint32_t system;
    uint32_t idle;
    uint32_t online;
} T733CpuLoad;

// T733CpuDemand is the worker's snapshot of scheduler notifications for one CPU.
typedef struct {
    uint32_t running;
    uint32_t perfClass;
    uint32_t qos;
    uint32_t percent;
    uint32_t peakPercent;
    uint64_t latency;
    uint64_t notifications;
} T733CpuDemand;

void t733_cpu_power_register_perfcontrol(void);
void t733_cpu_power_read_demand(unsigned cpu, T733CpuDemand *demand);

void t733_cpu_power_start(void);
void t733_cpu_power_dump_stats(void);
int t733_cpu_power_read_load(unsigned cpu, T733CpuLoad *load);

#ifdef __cplusplus
}
#endif
