// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc_internal.h>
#include <kern/processor.h>
#include <kern/startup.h>
#include <mach/processor.h>
#include <mach/processor_info.h>
#include <arm/machine_routines.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include "T733CpuPower.h"

// CpuNotifications stores only atomics; scheduler callbacks never enter the worker.
struct CpuNotifications {
    uint32_t state;
    uint32_t qos;
    uint32_t peak;
    uint64_t latency;
    uint64_t notifications;
    uint64_t threadId;
};

static struct CpuNotifications notifications[8];
static uint64_t latencyOneMs;
static uint64_t latencyFiveMs;

static unsigned class_percent(unsigned cls)
{
    switch (cls) {
    case PERFCONTROL_CLASS_IDLE:
        return 0;
    case PERFCONTROL_CLASS_REALTIME:
    case PERFCONTROL_CLASS_ABOVEUI:
        return 100;
    case PERFCONTROL_CLASS_UI:
        return 75;
    case PERFCONTROL_CLASS_USER_INITIATED:
        return 70;
    case PERFCONTROL_CLASS_NONUI:
        return 50;
    case PERFCONTROL_CLASS_UTILITY:
        return 35;
    default:
        return 20;
    }
}

static void record_peak(unsigned cpu, unsigned percent)
{
    uint32_t old = __atomic_load_n(&notifications[cpu].peak, __ATOMIC_RELAXED);
    while (old < percent && !__atomic_compare_exchange_n(&notifications[cpu].peak,
        &old, percent, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void record_thread(unsigned cpu, struct perfcontrol_thread_data *data)
{
    if (cpu >= 8 || !data) {
        return;
    }
    unsigned cls = data->perfctl_class;
    unsigned percent = class_percent(cls);
    unsigned running = cls != PERFCONTROL_CLASS_IDLE;
    __atomic_store_n(&notifications[cpu].state,
        running | (cls << 8) | (percent << 16), __ATOMIC_RELEASE);
    record_peak(cpu, percent);
    __atomic_fetch_add(&notifications[cpu].notifications, 1, __ATOMIC_RELAXED);
}

static void perf_switch(perfcontrol_event event, uint32_t cpu, uint64_t timestamp,
    uint32_t flags, struct perfcontrol_thread_data *offcore,
    struct perfcontrol_thread_data *oncore, struct perfcontrol_cpu_counters *counters,
    void *unused)
{
    (void)event;
    (void)timestamp;
    (void)flags;
    (void)offcore;
    (void)counters;
    (void)unused;
    if (cpu < 8 && oncore) {
        __atomic_store_n(&notifications[cpu].threadId, oncore->thread_id, __ATOMIC_RELAXED);
    }
    record_thread(cpu, oncore);
}

static void perf_update(perfcontrol_event event, uint32_t cpu, uint64_t timestamp,
    uint32_t flags, struct perfcontrol_thread_data *data, void *unused)
{
    (void)event;
    (void)timestamp;
    (void)flags;
    (void)unused;
    if (cpu < 8 && data && data->thread_id ==
        __atomic_load_n(&notifications[cpu].threadId, __ATOMIC_RELAXED)) {
        record_thread(cpu, data);
    }
}

static void perf_oncore(perfcontrol_state_t state, going_on_core_t data)
{
    (void)state;
    unsigned cpu = (unsigned)cpu_number();
    if (cpu >= 8) {
        return;
    }
    __atomic_store_n(&notifications[cpu].qos, data->qos_class, __ATOMIC_RELAXED);
    __atomic_store_n(&notifications[cpu].latency,
        data->scheduling_latency_at_same_basepri, __ATOMIC_RELAXED);
    uint64_t latency = data->scheduling_latency_at_same_basepri;
    if (latency >= latencyFiveMs) {
        record_peak(cpu, 100);
    } else if (latency >= latencyOneMs) {
        record_peak(cpu, 85);
    }
}

void t733_cpu_power_register_perfcontrol(void)
{
    nanoseconds_to_absolutetime(1000000, &latencyOneMs);
    nanoseconds_to_absolutetime(5000000, &latencyFiveMs);
    static struct sched_perfcontrol_callbacks callbacks = {
        .version = SCHED_PERFCONTROL_CALLBACKS_VERSION_6,
        .oncore = perf_oncore,
        .csw = perf_switch,
        .state_update = perf_update,
    };
    sched_perfcontrol_register_callbacks(&callbacks, 0);
}

void t733_cpu_power_read_demand(unsigned cpu, T733CpuDemand *demand)
{
    uint32_t state = __atomic_load_n(&notifications[cpu].state, __ATOMIC_ACQUIRE);
    demand->running = state & 1;
    demand->perfClass = (state >> 8) & 0xff;
    demand->percent = (state >> 16) & 0xff;
    demand->qos = __atomic_load_n(&notifications[cpu].qos, __ATOMIC_RELAXED);
    demand->peakPercent = __atomic_exchange_n(&notifications[cpu].peak, 0, __ATOMIC_RELAXED);
    demand->latency = __atomic_load_n(&notifications[cpu].latency, __ATOMIC_RELAXED);
    demand->notifications = __atomic_load_n(&notifications[cpu].notifications, __ATOMIC_RELAXED);
}

int t733_cpu_power_read_load(unsigned cpu, T733CpuLoad *load)
{
    processor_t processor = cpu_to_processor((int)cpu);
    if (!processor) {
        return -1;
    }

    host_t host;
    processor_basic_info_data_t basic = { 0 };
    mach_msg_type_number_t count = PROCESSOR_BASIC_INFO_COUNT;
    if (processor_info(processor, PROCESSOR_BASIC_INFO, &host,
        (processor_info_t)&basic, &count) != KERN_SUCCESS) {
        return -1;
    }
    load->online = basic.running;

    processor_cpu_load_info_data_t ticks = { 0 };
    count = PROCESSOR_CPU_LOAD_INFO_COUNT;
    // The kernel implementation snapshots recount with its existing synchronization.
    if (processor_info(processor, PROCESSOR_CPU_LOAD_INFO, &host,
        (processor_info_t)&ticks, &count) != KERN_SUCCESS) {
        return -1;
    }
    load->user = ticks.cpu_ticks[CPU_STATE_USER];
    load->system = ticks.cpu_ticks[CPU_STATE_SYSTEM];
    load->idle = ticks.cpu_ticks[CPU_STATE_IDLE];
    return 0;
}

static void start_cpu_power(const void *argument)
{
    (void)argument;
    t733_cpu_power_start();
}

STARTUP_ARG(EARLY_BOOT, STARTUP_RANK_LAST, start_cpu_power, 0);
