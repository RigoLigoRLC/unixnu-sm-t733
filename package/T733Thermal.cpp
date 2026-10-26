// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733Thermal.h"
#include "NativeQcomTsens.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>

static constexpr unsigned kIntervalMs = 200;
static constexpr uint64_t kStaleMs = 1000;
static constexpr uint16_t kSensorMasks[3] = {0x001e, 0x1f80, 0x6000};
static constexpr int32_t kTripMilliC[3] = {110000, 108000, 108000};
static constexpr int32_t kReleaseMilliC = 100000;

// ThermalGroup represents the stock CPU sensor group and its hysteresis state.
// A complete sample permits release. Missing sensors retain an active limit;
// one second without complete coverage also requests the minimum CPU row.
struct ThermalGroup {
    int32_t maximumMilliC;
    uint64_t lastCompleteMs;
    uint64_t invalidSamples;
    uint64_t transitions;
    bool haveComplete;
    bool complete;
    bool hot;
    bool stale;
    bool capped;
};

struct ThermalStatistics {
    QcomTsensSample controller[2];
    ThermalGroup group[3];
    uint64_t samples;
    uint64_t sampledMs;
    uint64_t mappingFailures[2];
    uint64_t invalidSensorSamples[2];
};

static uint32_t started;
static uint32_t statsReady;
static uint32_t caps[3];
static IOLock *statsLock;
static ThermalStatistics statistics;

static uint64_t monotonicMs()
{
    uint64_t ticks;
    uint64_t rate;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks) :: "memory");
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(rate));
    return rate >= 1000 ? ticks / (rate / 1000) : 0;
}

static void updateGroup(ThermalGroup &group, const QcomTsensSample &sample,
                        unsigned index, uint64_t now)
{
    uint16_t wanted = kSensorMasks[index];
    group.complete = (sample.validMask & wanted) == wanted;
    int32_t maximum = -2147483647;
    bool any = false;
    for (unsigned sensor = 0; sensor < 16; ++sensor) {
        if (!(wanted & sample.validMask & (1u << sensor))) {
            continue;
        }
        any = true;
        if (sample.milliC[sensor] > maximum) {
            maximum = sample.milliC[sensor];
        }
    }

    if (any && maximum >= kTripMilliC[index]) {
        group.hot = true;
    }
    if (group.complete) {
        group.maximumMilliC = maximum;
        group.lastCompleteMs = now;
        group.haveComplete = true;
        if (maximum <= kReleaseMilliC) {
            group.hot = false;
        }
    } else {
        ++group.invalidSamples;
    }

    group.stale = !group.haveComplete || now - group.lastCompleteMs >= kStaleMs;
    bool capped = group.hot || group.stale;
    if (group.capped && (!group.complete || maximum > kReleaseMilliC)) {
        capped = true;
    }
    if (capped != group.capped) {
        ++group.transitions;
    }
    group.capped = capped;
}

static void thermalWorker(void *, wait_result_t)
{
    NativeQcomTsens controllers[2] = {};
    ThermalStatistics next = {};
    const uint64_t srot[2] = {0x0c222000, 0x0c223000};
    const uint64_t tm[2] = {0x0c263000, 0x0c265000};
    const uint16_t masks[2] = {0x7fff, 0x0fff};
    bool mapped[2] = {};
    uint32_t previousCaps[3] = {};
    for (unsigned group = 0; group < 3; ++group) {
        next.group[group].capped = true;
    }

    // selected-stock.dts CPU step trips supply these thresholds. The minimum
    // ordinary EPSS row is this port's response to a trip. The stock shared
    // apc1 voltage cooling device couples sensors 7..12 to CPU4 and CPU7.
    printf("T733 thermal: TSENS v2 poll=%u ms trips_mC=110000,108000,108000 release_mC=100000 stale_ms=1000 cap=lowest-ordinary-row\n",
           kIntervalMs);

    for (;;) {
        for (unsigned controller = 0; controller < 2; ++controller) {
            if (!mapped[controller] && next.samples % 25 == 0) {
                mapped[controller] = controllers[controller].initialize(
                    srot[controller], tm[controller], masks[controller]);
                if (!mapped[controller]) {
                    ++next.mappingFailures[controller];
                }
            }
            controllers[controller].sample(next.controller[controller]);
            uint16_t missing = masks[controller] &
                               ~next.controller[controller].validMask;
            for (unsigned sensor = 0; sensor < 16; ++sensor) {
                if (missing & (1u << sensor)) {
                    ++next.invalidSensorSamples[controller];
                }
            }
        }

        next.sampledMs = monotonicMs();
        ++next.samples;
        for (unsigned group = 0; group < 3; ++group) {
            updateGroup(next.group[group], next.controller[0], group,
                        next.sampledMs);
        }

        uint32_t requested[3] = {
            next.group[0].capped ? 0u : UINT32_MAX,
            next.group[1].capped ? 0u : UINT32_MAX,
            (next.group[1].capped || next.group[2].capped) ? 0u : UINT32_MAX,
        };
        for (unsigned domain = 0; domain < 3; ++domain) {
            __atomic_store_n(&caps[domain], requested[domain], __ATOMIC_RELEASE);
        }

        IOLockLock(statsLock);
        statistics = next;
        IOLockUnlock(statsLock);

        if (next.samples == 1 || requested[0] != previousCaps[0] ||
            requested[1] != previousCaps[1] || requested[2] != previousCaps[2]) {
            printf("T733 thermal: sample=%llu valid=%04x max_mC=%d,%d,%d complete=%u,%u,%u cap=%u,%u,%u\n",
                   next.samples, next.controller[0].validMask,
                   next.group[0].maximumMilliC, next.group[1].maximumMilliC,
                   next.group[2].maximumMilliC, next.group[0].complete,
                   next.group[1].complete, next.group[2].complete,
                   requested[0], requested[1], requested[2]);
        }
        for (unsigned domain = 0; domain < 3; ++domain) {
            previousCaps[domain] = requested[domain];
        }
        IOSleep(kIntervalMs);
    }
}

extern "C" void t733_thermal_start(void)
{
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }
    statsLock = IOLockAlloc();
    if (!statsLock) {
        printf("T733 thermal: lock allocation failed; minimum CPU rows retained\n");
        __atomic_store_n(&started, 0u, __ATOMIC_RELEASE);
        return;
    }

    thread_t thread = THREAD_NULL;
    kern_return_t result = kernel_thread_start(thermalWorker, nullptr, &thread);
    if (result != KERN_SUCCESS) {
        IOLockFree(statsLock);
        statsLock = nullptr;
        __atomic_store_n(&started, 0u, __ATOMIC_RELEASE);
        printf("T733 thermal: worker start failed=%d; minimum CPU rows retained\n", result);
        return;
    }
    thread_deallocate(thread);
    __atomic_store_n(&statsReady, 1u, __ATOMIC_RELEASE);
}

extern "C" uint32_t t733_thermal_cap_khz(unsigned domain)
{
    if (domain >= 3) {
        return UINT32_MAX;
    }
    return __atomic_load_n(&caps[domain], __ATOMIC_ACQUIRE);
}

extern "C" void t733_thermal_dump_stats(void)
{
    if (!__atomic_load_n(&statsReady, __ATOMIC_ACQUIRE)) {
        printf("T733 thermal: ready=0 cap=minimum\n");
        return;
    }

    ThermalStatistics snapshot;
    IOLockLock(statsLock);
    snapshot = statistics;
    IOLockUnlock(statsLock);
    uint64_t now = monotonicMs();
    printf("T733 thermal: samples=%llu age_ms=%llu poll_ms=%u\n",
           snapshot.samples, now - snapshot.sampledMs, kIntervalMs);
    for (unsigned controller = 0; controller < 2; ++controller) {
        const QcomTsensSample &sample = snapshot.controller[controller];
        printf("T733 thermal: controller=%u version=%08x control=%08x trdy=%08x requested=%04x enabled=%04x valid=%04x map_fail=%llu invalid_sensor_samples=%llu\n",
               controller, sample.version, sample.control, sample.ready,
               sample.requestedMask, sample.enabledMask, sample.validMask,
               snapshot.mappingFailures[controller],
               snapshot.invalidSensorSamples[controller]);
        for (unsigned sensor = 0; sensor < 16; ++sensor) {
            if (!(sample.requestedMask & (1u << sensor))) {
                continue;
            }
            printf("T733 thermal: controller=%u sensor=%u status=%s valid=%u milliC=%d raw=%08x\n",
                   controller, sensor,
                   native_qcom_tsens_status_name(sample.status[sensor]),
                   (sample.validMask >> sensor) & 1, sample.milliC[sensor],
                   sample.raw[sensor]);
        }
    }
    for (unsigned domain = 0; domain < 3; ++domain) {
        const ThermalGroup &group = snapshot.group[domain];
        printf("T733 thermal: domain=%u mask=%04x trip_mC=%d release_mC=%d max_mC=%d have_value=%u complete=%u hot=%u stale=%u age_ms=%llu invalid_samples=%llu transitions=%llu cap_khz=%u\n",
               domain, kSensorMasks[domain], kTripMilliC[domain], kReleaseMilliC,
               group.maximumMilliC, group.haveComplete, group.complete,
               group.hot, group.stale,
               group.haveComplete ? now - group.lastCompleteMs : UINT64_MAX,
               group.invalidSamples, group.transitions,
               t733_thermal_cap_khz(domain));
    }
}
