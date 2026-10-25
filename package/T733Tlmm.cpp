// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733Tlmm.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);

namespace {

enum : unsigned {
    kPins = 176,
    kEnable = 1,
    kRaw = 1u << 4,
    kApps = 3u << 5,
};

/* TlmmPin holds one boot-lifetime consumer and its interrupt delivery counters.
 * The controller masks and acknowledges each observed interrupt before calling
 * its consumer. The consumer drains its device on a worker and calls rearm.
 */
struct TlmmPin {
    T733TlmmHandler handler;
    void *context;
    T733TlmmTrigger trigger;
    uint32_t deliveries;
    uint32_t deferredEdges;
    uint32_t unowned;
};

static TlmmPin pins[kPins];
static IOMemoryMap *mapping;
static uintptr_t base;
static IOSimpleLock *lock;
static uint32_t initState;
static uint32_t summaries;
static uint32_t unowned;

static uint32_t read(unsigned pin, unsigned offset)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + pin * 0x1000 + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void write(unsigned pin, unsigned offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + pin * 0x1000 + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void mask(unsigned pin)
{
    write(pin, 8, read(pin, 8) & ~kEnable);
}

static void ack(unsigned pin)
{
    // Yupik intr_ack_high is false; zero clears the latch.
    write(pin, 12, read(pin, 12) & ~1u);
}

static bool ready(unsigned pin)
{
    return pin < kPins && __atomic_load_n(&initState, __ATOMIC_ACQUIRE) == 2;
}

static void interrupt(void *)
{
    IOSimpleLockLock(lock);
    ++summaries;
    IOSimpleLockUnlock(lock);

    for (unsigned pin = 0; pin < kPins; ++pin) {
        T733TlmmHandler handler = nullptr;
        void *context = nullptr;
        IOSimpleLockLock(lock);
        uint32_t config = read(pin, 8);
        if ((config & kEnable) && (config & (7u << 5)) == kApps && (read(pin, 12) & 1)) {
            mask(pin);
            ack(pin);
            handler = pins[pin].handler;
            context = pins[pin].context;
            if (handler) {
                ++pins[pin].deliveries;
            } else {
                ++pins[pin].unowned;
                ++unowned;
            }
        }
        IOSimpleLockUnlock(lock);
        if (handler) {
            handler(pin, context);
        }
    }
}

static int initialize()
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&initState, &expected, 1u, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&initState, __ATOMIC_ACQUIRE) == 1) {
            IOSleep(1);
        }
        return __atomic_load_n(&initState, __ATOMIC_ACQUIRE) == 2 ? 0 : -6;
    }

    lock = IOSimpleLockAlloc();
    IODeviceMemory *memory = lock ? IODeviceMemory::withRange(0x0f100000, kPins * 0x1000) : nullptr;
    if (memory) {
        mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
    }
    if (!mapping) {
        __atomic_store_n(&initState, 3u, __ATOMIC_RELEASE);
        return -12;
    }
    base = mapping->getVirtualAddress();
    uint32_t config = 0;
    uint64_t router = 0;
    IOReturn result = armcpu_spi_attach(240, false, interrupt, nullptr, &config, &router);
    printf("T733 TLMM: summary INTID240 attach=%x config=%x router=%llx\n", result, config, router);
    __atomic_store_n(&initState, result == kIOReturnSuccess ? 2u : 3u, __ATOMIC_RELEASE);
    return result == kIOReturnSuccess ? 0 : -6;
}

} // namespace

extern "C" int t733_tlmm_irq_register(unsigned pin, T733TlmmTrigger trigger,
    T733TlmmHandler handler, void *context)
{
    if (pin >= kPins || !handler || trigger > T733_TLMM_EDGE_BOTH) {
        return -22;
    }
    int result = initialize();
    if (result) {
        return result;
    }
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    if (pins[pin].handler) {
        IOSimpleLockUnlockEnableInterrupt(lock, state);
        return -16;
    }
    mask(pin);
    write(pin, 0, read(pin, 0) & ~((15u << 2) | (1u << 9)));
    uint32_t config = (read(pin, 8) & ~0xffu) | kApps | kRaw;
    switch (trigger) {
        case T733_TLMM_LEVEL_LOW:
            break;
        case T733_TLMM_LEVEL_HIGH:
            config |= 1u << 1;
            break;
        case T733_TLMM_EDGE_FALLING:
            config |= (2u << 2) | (1u << 1);
            break;
        case T733_TLMM_EDGE_RISING:
            config |= (1u << 2) | (1u << 1);
            break;
        case T733_TLMM_EDGE_BOTH:
            config |= (3u << 2) | (1u << 1);
            break;
    }
    write(pin, 8, config);
    ack(pin);
    pins[pin].trigger = trigger;
    pins[pin].context = context;
    pins[pin].handler = handler;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    printf("T733 TLMM: GPIO%u registered trigger=%u config=%x\n", pin, trigger, config);
    return 0;
}

extern "C" void t733_tlmm_irq_mask(unsigned pin)
{
    if (!ready(pin)) {
        return;
    }
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    if (pins[pin].handler) {
        mask(pin);
    }
    IOSimpleLockUnlockEnableInterrupt(lock, state);
}

extern "C" void t733_tlmm_irq_ack(unsigned pin)
{
    if (!ready(pin)) {
        return;
    }
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    if (pins[pin].handler) {
        ack(pin);
    }
    IOSimpleLockUnlockEnableInterrupt(lock, state);
}

extern "C" bool t733_tlmm_irq_rearm(unsigned pin)
{
    if (!ready(pin)) {
        return false;
    }
    T733TlmmHandler handler = nullptr;
    void *context = nullptr;
    bool armed = false;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    TlmmPin &entry = pins[pin];
    if (entry.handler) {
        mask(pin);
        bool level = (read(pin, 4) & 1) != 0;
        bool edge = entry.trigger >= T733_TLMM_EDGE_FALLING;
        if (edge && (read(pin, 12) & 1)) {
            ack(pin);
            ++entry.deliveries;
            ++entry.deferredEdges;
            handler = entry.handler;
            context = entry.context;
            armed = true;
        } else if (edge || (entry.trigger == T733_TLMM_LEVEL_LOW ? level : !level)) {
            if (!edge) {
                ack(pin);
            }
            write(pin, 8, read(pin, 8) | kRaw | kEnable);
            armed = true;
        }
    }
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    if (handler) {
        handler(pin, context);
    }
    return armed;
}

extern "C" int t733_tlmm_level(unsigned pin)
{
    return ready(pin) ? static_cast<int>(read(pin, 4) & 1) : -1;
}

extern "C" uint32_t t733_tlmm_irq_count(unsigned pin)
{
    if (!ready(pin)) {
        return 0;
    }
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    uint32_t count = pins[pin].deliveries;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    return count;
}

extern "C" void t733_tlmm_dump_stats(void)
{
    if (!ready(0)) {
        printf("T733 TLMM: unavailable state=%u\n", __atomic_load_n(&initState, __ATOMIC_ACQUIRE));
        return;
    }
    struct Snapshot {
        uint32_t deliveries, deferred, unowned, config, pending, level;
        bool registered;
    };
    Snapshot *snapshot = static_cast<Snapshot *>(IOMalloc(sizeof(Snapshot) * kPins));
    if (!snapshot) {
        printf("T733 TLMM: snapshot allocation failed\n");
        return;
    }
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    uint32_t summaryCount = summaries;
    uint32_t unownedCount = unowned;
    for (unsigned pin = 0; pin < kPins; ++pin) {
        snapshot[pin] = {pins[pin].deliveries, pins[pin].deferredEdges, pins[pin].unowned,
            read(pin, 8), read(pin, 12) & 1, read(pin, 4) & 1, pins[pin].handler != nullptr};
    }
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    printf("T733 TLMM: summary=%u unowned=%u\n", summaryCount, unownedCount);
    for (unsigned pin = 0; pin < kPins; ++pin) {
        const Snapshot &s = snapshot[pin];
        if (s.registered || s.unowned) {
            printf("T733 TLMM: pin=%u registered=%u delivered=%u deferred=%u unowned=%u masked=%u pending=%u level=%u config=%x\n",
                pin, s.registered, s.deliveries, s.deferred, s.unowned,
                !(s.config & kEnable), s.pending, s.level, s.config);
        }
    }
    IOFree(snapshot, sizeof(Snapshot) * kPins);
}
