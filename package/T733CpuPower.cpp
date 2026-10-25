// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733CpuPower.h"
#include "T733Thermal.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

static constexpr unsigned kDomains = 3;
static constexpr unsigned kRows = 40;
static constexpr uint64_t kXO = 19200000;
static constexpr unsigned kIntervalMs = 10;
static constexpr unsigned kLoadIntervalMs = 200;

// CpuDomain owns one EPSS request and its firmware-provided frequency table.
// The worker serializes requests and SDPM writes; the statistics lock protects
// copied observations read by the USB thread. Hardware owns voltage transitions.
struct CpuDomain {
    uintptr_t base;
    uintptr_t sdpm;
    uint32_t khz[kRows];
    uint32_t uv[kRows];
    uint32_t cores[kRows];
    unsigned rows;
    unsigned first;
    unsigned count;
    unsigned requested;
    unsigned load;
    uint32_t loadTargetKhz;
    unsigned online;
    unsigned lowSamples;
    uint32_t state;
    uint32_t irq;
    uint32_t limitKhz;
    uint32_t thermalKhz;
    unsigned demand;
    unsigned running;
    unsigned peakHold;
    unsigned heldDemand;
    uint64_t notifications;
    T733CpuDemand cpuDemand[4];
    uint32_t cycleKhz[4];
    uint64_t changes;
    bool ready;
};

static CpuDomain domains[kDomains];
static T733CpuLoad previousLoad[8];
static uint32_t previousCycles[8];
static bool haveLoad[8];
static IOMemoryMap *maps[3];
static IOLock *statsLock;
static uint32_t started;
static uint32_t ready;
static uint64_t sampleCount;
static uint64_t counterRate;

static uint32_t readRegister(uintptr_t address)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(address);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void writeRegister(uintptr_t address, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(address) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t counter()
{
    uint64_t value;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}

static uintptr_t mapRegisters(unsigned slot, uint64_t physical, unsigned length)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(physical, length);
    if (!memory) {
        return 0;
    }
    maps[slot] = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    return maps[slot] ? maps[slot]->getVirtualAddress() : 0;
}

static bool readTable(CpuDomain &domain, uint32_t alternateKhz)
{
    for (unsigned row = 0; row < kRows; ++row) {
        uint32_t word = readRegister(domain.base + 0x100 + row * 4);
        uint32_t src = word >> 30;
        uint32_t khz = src ? static_cast<uint32_t>(kXO / 1000) * (word & 0xff)
                           : alternateKhz;
        uint32_t cores = (word >> 16) & 7;
        uint32_t uv = (readRegister(domain.base + 0x200 + row * 4) & 0xfff) * 1000;
        if (row && khz == domain.khz[row - 1] && cores == domain.cores[row - 1]) {
            break;
        }
        if (!khz || !uv || (row && khz < domain.khz[row - 1])) {
            printf("T733 DVFS: invalid LUT domain=%u row=%u raw=%x khz=%u uv=%u\n",
                domain.first, row, word, khz, uv);
            return false;
        }
        domain.khz[row] = khz;
        domain.uv[row] = uv;
        domain.cores[row] = cores;
        ++domain.rows;
    }
    unsigned ordinary = 0;
    for (unsigned row = 0; row < domain.rows; ++row) {
        ordinary += domain.cores[row] == 4;
    }
    domain.requested = readRegister(domain.base + 0x320);
    // The stock DT supplies max_cores=4 for all three domains, including prime.
    return ordinary != 0 && domain.requested < domain.rows;
}

static unsigned chooseRow(const CpuDomain &domain, uint32_t targetKhz, uint32_t capKhz)
{
    unsigned chosen = kRows;
    for (unsigned row = 0; row < domain.rows; ++row) {
        if (domain.cores[row] != 4 || domain.khz[row] > capKhz) {
            continue;
        }
        chosen = row;
        if (domain.khz[row] >= targetKhz) {
            break;
        }
    }
    return chosen;
}

static void requestRow(CpuDomain &domain, unsigned row)
{
    if (row == domain.requested || row >= domain.rows) {
        return;
    }
    uint32_t oldKhz = domain.khz[domain.requested];
    uint32_t newKhz = domain.khz[row];
    if (domain.sdpm && newKhz > oldKhz) {
        writeRegister(domain.sdpm, newKhz / 1000);
    }
    writeRegister(domain.base + 0x320, row);
    if (domain.sdpm && newKhz < oldKhz) {
        writeRegister(domain.sdpm, newKhz / 1000);
    }
    domain.requested = row;
    ++domain.changes;
}

static void updateDomain(CpuDomain &domain, unsigned domainIndex, uint64_t elapsed, bool sampleLoad)
{
    if (!domain.ready) {
        return;
    }
    domain.demand = 0;
    domain.running = 0;
    domain.notifications = 0;
    unsigned peak = 0;
    for (unsigned core = 0; core < domain.count; ++core) {
        T733CpuDemand &demand = domain.cpuDemand[core];
        t733_cpu_power_read_demand(domain.first + core, &demand);
        domain.notifications += demand.notifications;
        domain.running += demand.running;
        if (demand.running && demand.percent > domain.demand) {
            domain.demand = demand.percent;
        }
        if (demand.peakPercent > peak) {
            peak = demand.peakPercent;
        }
    }
    if (peak >= domain.heldDemand) {
        domain.heldDemand = peak;
        domain.peakHold = 3;
    } else if (domain.peakHold && --domain.peakHold == 0) {
        domain.heldDemand = peak;
    }
    if (domain.heldDemand > domain.demand) {
        domain.demand = domain.heldDemand;
    }

    unsigned maximum = chooseRow(domain, UINT32_MAX, UINT32_MAX);
    if (sampleLoad) {
        domain.load = 0;
        domain.online = 0;
        for (unsigned core = 0; core < domain.count; ++core) {
            unsigned cpu = domain.first + core;
            T733CpuLoad now = {};
            uint32_t cycles = readRegister(domain.base + 0x3c4 + (core + 1) * 4);
            bool valid = t733_cpu_power_read_load(cpu, &now) == 0 && now.online;
            domain.cycleKhz[core] = 0;
            if (valid) {
                ++domain.online;
                if (haveLoad[cpu]) {
                    uint64_t busy = static_cast<uint32_t>(now.user - previousLoad[cpu].user);
                    busy += static_cast<uint32_t>(now.system - previousLoad[cpu].system);
                    uint64_t idle = static_cast<uint32_t>(now.idle - previousLoad[cpu].idle);
                    if (busy + idle) {
                        unsigned load = static_cast<unsigned>(busy * 100 / (busy + idle));
                        if (load > domain.load) {
                            domain.load = load;
                        }
                    }
                    // Long scheduler stalls can include multiple 32-bit wraps.
                    if (elapsed && elapsed < counterRate / 2) {
                        uint64_t measuredKhz = static_cast<uint64_t>(
                            static_cast<uint32_t>(cycles - previousCycles[cpu]))
                            * counterRate / elapsed / 1000;
                        domain.cycleKhz[core] = static_cast<uint32_t>(
                            measuredKhz > UINT32_MAX ? UINT32_MAX : measuredKhz);
                    }
                }
            }
            previousLoad[cpu] = now;
            previousCycles[cpu] = cycles;
            haveLoad[cpu] = valid;
        }
        uint64_t proportionalKhz = static_cast<uint64_t>(domain.khz[domain.requested])
            * domain.load / 70;
        domain.loadTargetKhz = domain.load >= 80 ? domain.khz[maximum]
            : static_cast<uint32_t>(proportionalKhz > domain.khz[maximum]
                ? domain.khz[maximum] : proportionalKhz);
    }

    domain.state = readRegister(domain.base + 0x20);
    domain.irq = readRegister(domain.base + 0x30c);
    domain.limitKhz = 0;
    uint32_t cap = UINT32_MAX;
    if ((domain.irq & 4) && (domain.state & 0xff)) {
        domain.limitKhz = (domain.state & 0xff) * static_cast<uint32_t>(kXO / 1000);
        cap = domain.limitKhz;
        if (cap >= domain.khz[domain.requested]) {
            writeRegister(domain.base + 0x308, readRegister(domain.base + 0x308) | 4);
        }
    }
    domain.thermalKhz = t733_thermal_cap_khz(domainIndex);
    if (domain.thermalKhz < cap) {
        cap = domain.thermalKhz;
    }

    uint32_t target = domain.loadTargetKhz;
    uint32_t schedulerKhz = static_cast<uint32_t>(
        static_cast<uint64_t>(domain.khz[maximum]) * domain.demand / 100);
    if (schedulerKhz > target) {
        target = schedulerKhz;
    }
    unsigned row = chooseRow(domain, target, cap);
    if (row == kRows) {
        // Hardware remains responsible when its cap falls below the lowest row.
        row = chooseRow(domain, 0, UINT32_MAX);
    }
    bool constrained = domain.khz[domain.requested] > cap;
    if (domain.khz[row] < domain.khz[domain.requested] && !constrained) {
        if (++domain.lowSamples < 30) {
            return;
        }
    } else {
        domain.lowSamples = 0;
    }
    requestRow(domain, row);
}

static void worker(void *, wait_result_t)
{
    statsLock = IOLockAlloc();
    uintptr_t epss = mapRegisters(0, 0x18591000, 0x3000);
    uintptr_t sdpm = mapRegisters(1, 0x636000, 0x1000);
    uintptr_t gcc = mapRegisters(2, 0x100000, 0x1000);
    if (!statsLock || !epss || !sdpm || !gcc) {
        printf("T733 DVFS: register mapping failed\n");
        return;
    }
    uint64_t pll = kXO * readRegister(gcc + 4)
        + ((kXO * (readRegister(gcc + 0x40) & 0xffff)) >> 16);
    uint32_t alternateKhz = static_cast<uint32_t>(pll / 2 / 1000);
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(counterRate));
    if (!counterRate) {
        printf("T733 DVFS: architectural counter unavailable\n");
        return;
    }

    for (unsigned index = 0; index < kDomains; ++index) {
        CpuDomain &domain = domains[index];
        domain.base = epss + index * 0x1000;
        domain.sdpm = index ? sdpm + 0xf04 + index * 4 : 0;
        domain.first = index == 0 ? 0 : index == 1 ? 4 : 7;
        domain.count = index == 0 ? 4 : index == 1 ? 3 : 1;
        domain.ready = readTable(domain, alternateKhz);
        printf("T733 DVFS: domain=%u CPUs=%u+%u LUT=%u request=%u ready=%u alternate_khz=%u\n",
            index, domain.first, domain.count, domain.rows, domain.requested,
            domain.ready, alternateKhz);
        for (unsigned row = 0; row < domain.rows; ++row) {
            printf("T733 DVFS: LUT domain=%u row=%u khz=%u uv=%u cores=%u\n",
                index, row, domain.khz[row], domain.uv[row], domain.cores[row]);
        }
    }
    t733_thermal_start();
    t733_cpu_power_register_perfcontrol();
    __atomic_store_n(&ready, 1u, __ATOMIC_RELEASE);
    uint64_t previous = counter();
    for (;;) {
        IOSleep(kIntervalMs);
        uint64_t now = counter();
        bool sampleLoad = now - previous >= counterRate * kLoadIntervalMs / 1000;
        IOLockLock(statsLock);
        for (unsigned index = 0; index < kDomains; ++index) {
            updateDomain(domains[index], index, now - previous, sampleLoad);
        }
        ++sampleCount;
        IOLockUnlock(statsLock);
        if (sampleLoad) {
            previous = now;
        }
    }
}

extern "C" void t733_cpu_power_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("t733-cpu-dvfs", &enabled, sizeof(enabled)) || !enabled) {
        return;
    }
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }
    thread_t thread = THREAD_NULL;
    kern_return_t result = kernel_thread_start(worker, nullptr, &thread);
    if (result == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
    printf("T733 DVFS: worker=%d period_ms=%u\n", result, kIntervalMs);
}

extern "C" void t733_cpu_power_dump_stats(void)
{
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {
        printf("T733 DVFS: ready=0 started=%u\n", started);
        return;
    }
    CpuDomain snapshot[kDomains];
    IOLockLock(statsLock);
    memcpy(snapshot, domains, sizeof(snapshot));
    uint64_t samples = sampleCount;
    IOLockUnlock(statsLock);
    for (unsigned index = 0; index < kDomains; ++index) {
        const CpuDomain &domain = snapshot[index];
        printf("T733 DVFS: sample=%llu domain=%u ready=%u online=%u load=%u request=%u khz=%u state=%x irq=%x limit_khz=%u changes=%llu cycles_khz=%u,%u,%u,%u\n",
            samples, index, domain.ready, domain.online, domain.load, domain.requested,
            domain.requested < domain.rows ? domain.khz[domain.requested] : 0,
            domain.state, domain.irq, domain.limitKhz, domain.changes,
            domain.cycleKhz[0], domain.cycleKhz[1], domain.cycleKhz[2], domain.cycleKhz[3]);
        printf("T733 DVFS: domain=%u scheduler_percent=%u running=%u notifications=%llu thermal_cap_khz=%u\n",
            index, domain.demand, domain.running, domain.notifications, domain.thermalKhz);
        for (unsigned core = 0; core < domain.count; ++core) {
            const T733CpuDemand &demand = domain.cpuDemand[core];
            printf("T733 DVFS: cpu=%u running=%u class=%u qos=%u latency_ticks=%llu percent=%u\n",
                domain.first + core, demand.running, demand.perfClass, demand.qos,
                demand.latency, demand.percent);
        }
    }
    t733_thermal_dump_stats();
}
