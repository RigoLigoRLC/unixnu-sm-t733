// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733FuelGauge.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
void t733_battery_static_init(void) __asm__("__GLOBAL__sub_I_T733Battery.cpp");
extern kmod_info_t g_kernel_kmod_info;
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *),
    void *context, uint32_t *readback, uint64_t *router);
}

// BatteryInterrupt owns the TLMM summary interrupt and GPIO54 for the SM5714.
// The interrupt callback masks pending lines and wakes the battery worker. The
// worker clears SM5714 status over I2C before re-enabling GPIO54. Other pending
// APPS GPIO sources are masked and reported because no driver owns them here.
struct BatteryInterrupt {
    IOMemoryMap *mapping;
    IOSimpleLock *lock;
    uintptr_t base;
    bool attached;
    uint32_t pending;
    uint32_t count;
    uint32_t unownedCount;
    uint32_t lastUnownedPin;
};

static BatteryInterrupt batteryInterrupt;
static constexpr unsigned kBatteryGPIO = 54;
static constexpr unsigned kTlmmGPIOCount = 176;
static constexpr uint32_t kTlmmEnable = 1u;
static constexpr uint32_t kTlmmRawStatus = 1u << 4;
static constexpr uint32_t kTlmmApps = 3u << 5;

static uint32_t gpioRead(unsigned pin, unsigned offset)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(
        batteryInterrupt.base + pin * 0x1000 + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void gpioWrite(unsigned pin, unsigned offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(
        batteryInterrupt.base + pin * 0x1000 + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void maskGPIO(unsigned pin)
{
    gpioWrite(pin, 8, gpioRead(pin, 8) & ~(kTlmmEnable | kTlmmRawStatus));
    gpioWrite(pin, 12, gpioRead(pin, 12) & ~1u);
}

static void batteryInterruptHandler(void *)
{
    bool wake = false;
    IOSimpleLockLock(batteryInterrupt.lock);
    for (unsigned pin = 0; pin < kTlmmGPIOCount; ++pin) {
        uint32_t config = gpioRead(pin, 8);
        if (!(config & kTlmmEnable) || (config & (7u << 5)) != kTlmmApps ||
            !(gpioRead(pin, 12) & 1u)) {
            continue;
        }

        maskGPIO(pin);
        if (pin == kBatteryGPIO) {
            ++batteryInterrupt.count;
        } else {
            ++batteryInterrupt.unownedCount;
            batteryInterrupt.lastUnownedPin = pin;
        }
        wake = true;
    }
    IOSimpleLockUnlock(batteryInterrupt.lock);

    if (wake) {
        __atomic_store_n(&batteryInterrupt.pending, 1u, __ATOMIC_RELEASE);
        thread_wakeup(reinterpret_cast<event_t>(&batteryInterrupt.pending));
    }
}

static bool attachBatteryInterrupt(void)
{
    if (batteryInterrupt.attached) {
        return true;
    }
    if (!batteryInterrupt.mapping) {
        IODeviceMemory *memory = IODeviceMemory::withRange(0x0f100000,
            kTlmmGPIOCount * 0x1000);
        if (!memory) {
            return false;
        }
        batteryInterrupt.mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
        if (!batteryInterrupt.mapping) {
            return false;
        }
        batteryInterrupt.base = batteryInterrupt.mapping->getVirtualAddress();
    }
    if (!batteryInterrupt.lock) {
        batteryInterrupt.lock = IOSimpleLockAlloc();
        if (!batteryInterrupt.lock) {
            return false;
        }
    }

    maskGPIO(kBatteryGPIO);
    gpioWrite(kBatteryGPIO, 0, gpioRead(kBatteryGPIO, 0) & ~0x3ffu);
    gpioWrite(kBatteryGPIO, 8, (gpioRead(kBatteryGPIO, 8) & ~0xffu) | kTlmmApps);

    uint32_t readback = 0;
    uint64_t router = 0;
    IOReturn result = armcpu_spi_attach(240, false, batteryInterruptHandler,
        nullptr, &readback, &router);
    batteryInterrupt.attached = result == kIOReturnSuccess;
    printf("T733 battery: GPIO54 IRQ attach=%x GIC=240 config=%x router=%llx level=%u\n",
        result, readback, router, gpioRead(kBatteryGPIO, 4) & 1u);
    return batteryInterrupt.attached;
}

static void maskBatteryInterrupt(void)
{
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(batteryInterrupt.lock);
    maskGPIO(kBatteryGPIO);
    IOSimpleLockUnlockEnableInterrupt(batteryInterrupt.lock, state);
}

static bool rearmBatteryInterrupt(void)
{
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(batteryInterrupt.lock);
    bool high = (gpioRead(kBatteryGPIO, 4) & 1u) != 0;
    if (high) {
        gpioWrite(kBatteryGPIO, 12, gpioRead(kBatteryGPIO, 12) & ~1u);
        gpioWrite(kBatteryGPIO, 8,
            gpioRead(kBatteryGPIO, 8) | kTlmmRawStatus | kTlmmEnable);
    }
    IOSimpleLockUnlockEnableInterrupt(batteryInterrupt.lock, state);
    return high;
}

static void waitForBatteryEvent(unsigned milliseconds)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(milliseconds, NSEC_PER_MSEC, &deadline);
    assert_wait_deadline(reinterpret_cast<event_t>(&batteryInterrupt.pending),
        THREAD_UNINT, deadline);
    // Publishing the wait before checking pending closes the interrupt/wait race.
    if (__atomic_load_n(&batteryInterrupt.pending, __ATOMIC_ACQUIRE)) {
        clear_wait(current_thread(), THREAD_AWAKENED);
    }
    thread_block(THREAD_CONTINUE_NULL);
}

// T733Battery publishes SM5714 measurements through the native power-source API.
// Its boot-lifetime worker owns the service and performs every hardware read and
// property update serially. powerd consumes the registry and change notifications.
class T733Battery : public IOPMPowerSource {
    OSDeclareDefaultStructors(T733Battery)

    uint32_t receivedFields;
    uint32_t sampleCount;
    bool published;
    uint8_t lastPowerState;

    void setValue(const char *name, OSObject *value)
    {
        const OSSymbol *key = OSSymbol::withCString(name);
        if (key) {
            setPSProperty(key, value);
            key->release();
        }
    }

    void setNumber(const char *name, uint32_t value)
    {
        OSNumber *number = OSNumber::withNumber(value, 32);
        if (number) {
            setValue(name, number);
            number->release();
        }
    }

    void setBoolean(const char *name, bool value)
    {
        setValue(name, value ? kOSBooleanTrue : kOSBooleanFalse);
    }

public:
    bool init(void) override
    {
        if (!IOPMPowerSource::init()) {
            return false;
        }
        receivedFields = 0;
        sampleCount = 0;
        published = false;
        lastPowerState = 0xff;
        setName("T733Battery");
        setProperty("Built-In", kOSBooleanTrue);
        setProperty("DeviceName", "SM5714 Battery");
        setProperty("T733CapacityUnit", "percent");
        setTimeRemaining(-1);
        return true;
    }

    void acceptSample(const T733FuelGaugeSample &sample, int result)
    {
        const uint32_t required = T733_FG_SOC | T733_FG_CHARGER_STATUS;
        bool firstPublication = false;
        bool powerChanged = false;
        ++sampleCount;

        if (sample.valid & T733_FG_SOC) {
            // powerd calculates the percentage from current/max. mAh is unavailable.
            setCurrentCapacity(static_cast<unsigned>(sample.soc_permille) / 10);
            setMaxCapacity(100);
            setNumber("T733StateOfChargePermille", static_cast<uint32_t>(sample.soc_permille));
            setAtWarnLevel(sample.soc_permille <= 150);
            setAtCriticalLevel(sample.soc_permille <= 50);
        }
        if (sample.valid & T733_FG_VOLTAGE) {
            setVoltage(static_cast<unsigned>(sample.voltage_mv));
        }
        if (sample.valid & T733_FG_CURRENT) {
            setAmperage(sample.current_ma);
            setNumber("InstantAmperage", static_cast<uint32_t>(sample.current_ma));
        }
        if (sample.valid & T733_FG_CHARGER_STATUS) {
            uint8_t state = sample.external_power | (sample.charging << 1) |
                (sample.full << 2) | (sample.battery_present << 3);
            powerChanged = state != lastPowerState;
            lastPowerState = state;
            setBatteryInstalled(sample.battery_present);
            setExternalConnected(sample.external_power);
            setBoolean("AppleRawExternalConnected", sample.external_power);
            setIsCharging(sample.charging);
            setBoolean("FullyCharged", sample.full);
        }
        if (sample.valid & T733_FG_TEMPERATURE) {
            setNumber("T733GaugeTemperatureDeciC", static_cast<uint32_t>(sample.gauge_temperature_deci_c));
        }
        if (sample.valid & T733_FG_CYCLE) {
            // The SM5714 register only supplies the low eight bits of its counter.
            setNumber("T733GaugeCycleCountLow8", sample.cycle_count);
        }

        receivedFields |= sample.valid;
        setNumber("T733SampleValidFields", sample.valid);
        setNumber("T733ReadError", static_cast<uint32_t>(result));
        setBoolean("T733SampleCurrent", (sample.valid & required) == required);
        if (result || (sample.valid & required) != required) {
            const OSSymbol *error = OSSymbol::withCString("SM5714ReadIncomplete");
            if (error) {
                setErrorCondition(const_cast<OSSymbol *>(error));
                error->release();
            }
        } else if (properties->getObject(errorConditionKey)) {
            properties->removeObject(errorConditionKey);
            removeProperty(errorConditionKey);
            settingsChangedSinceUpdate = true;
        }

        // Partial reads update only their valid fields; previous measurements survive.
        updateStatus();
        if (!published && (receivedFields & required) == required) {
            registerService();
            published = true;
            firstPublication = true;
            printf("T733 battery: IOPMPowerSource registered current=%u max=%u installed=%u\n",
                currentCapacity(), maxCapacity(), batteryInstalled());
        }
        if (sampleCount == 1 || firstPublication || powerChanged || !(sampleCount % 12)) {
            printf("T733 battery: sample=%u valid=%x result=%d published=%u soc_permille=%d voltage_mv=%d current_ma=%d external=%u charging=%u full=%u present=%u retained_percent=%u\n",
                sampleCount, sample.valid, result, published,
                sample.valid & T733_FG_SOC ? sample.soc_permille : -1,
                sample.valid & T733_FG_VOLTAGE ? sample.voltage_mv : -1,
                sample.valid & T733_FG_CURRENT ? sample.current_ma : 0,
                sample.external_power, sample.charging, sample.full,
                sample.battery_present, currentCapacity());
        }
    }
};

OSDefineMetaClassAndStructors(T733Battery, IOPMPowerSource)

static void batteryWorker(void *, wait_result_t)
{
    OSDictionary *matching = IOService::resourceMatching("IOBSD");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 60000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!provider) {
        printf("T733 battery: IOBSD unavailable\n");
        thread_terminate(current_thread());
        return;
    }

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        provider->release();
        thread_terminate(current_thread());
        return;
    }
    t733_battery_static_init();
    if (OSMetaClass::postModLoad(handle) != kOSReturnSuccess) {
        provider->release();
        thread_terminate(current_thread());
        return;
    }

    T733Battery *battery = new T733Battery;
    bool attached = battery && battery->init() && battery->attach(provider);
    if (!attached || !battery->start(provider)) {
        printf("T733 battery: power-source start failed\n");
        if (attached) {
            battery->detach(provider);
        }
        OSSafeReleaseNULL(battery);
        provider->release();
        thread_terminate(current_thread());
        return;
    }
    provider->release();

    bool initialized = false;
    bool irqConfigured = false;
    unsigned attempts = 0;
    unsigned irqAttempts = 0;
    uint32_t lastIRQCount = 0;
    uint32_t lastUnownedCount = 0;
    unsigned drainFailures = 0;
    for (;;) {
        __atomic_exchange_n(&batteryInterrupt.pending, 0u, __ATOMIC_ACQ_REL);
        if (!initialized) {
            int result = t733_fuel_gauge_init();
            ++attempts;
            initialized = result == 0;
            if (initialized || attempts == 1 || !(attempts % 12)) {
                printf("T733 battery: hardware init=%d attempt=%u\n", result, attempts);
            }
            if (!initialized) {
                IOSleep(5000);
                continue;
            }
        }

        if (!irqConfigured) {
            ++irqAttempts;
            if (attachBatteryInterrupt()) {
                int result = t733_fuel_gauge_irq_setup();
                irqConfigured = result == 0;
                if (irqConfigured || irqAttempts == 1 || !(irqAttempts % 12)) {
                    printf("T733 battery: SM5714 IRQ setup=%d attempt=%u\n", result, irqAttempts);
                }
            }
        }

        T733FuelGaugeIRQ irq = {};
        int drainResult = 0;
        if (irqConfigured) {
            maskBatteryInterrupt();
            drainResult = t733_fuel_gauge_irq_drain(&irq);
        }

        T733FuelGaugeSample sample = {};
        int result = t733_fuel_gauge_read(&sample);
        battery->acceptSample(sample, result);

        unsigned waitMilliseconds = 5000;
        if (irqConfigured) {
            bool armed = !drainResult && rearmBatteryInterrupt();
            IOInterruptState state = IOSimpleLockLockDisableInterrupt(batteryInterrupt.lock);
            uint32_t count = batteryInterrupt.count;
            uint32_t unowned = batteryInterrupt.unownedCount;
            uint32_t unownedPin = batteryInterrupt.lastUnownedPin;
            IOSimpleLockUnlockEnableInterrupt(batteryInterrupt.lock, state);

            if (!armed) {
                ++drainFailures;
                waitMilliseconds = 250;
            } else {
                drainFailures = 0;
            }
            if (irq.source_seen || count != lastIRQCount || drainFailures == 1 ||
                (drainFailures && !(drainFailures % 20))) {
                printf("T733 battery: IRQ count=%u source=%x remaining=%x charger=%02x/%02x/%02x/%02x/%02x muic=%02x/%02x fg=%04x drain=%d armed=%u retries=%u\n",
                    count, irq.source_seen, irq.source_remaining,
                    irq.charger[0], irq.charger[1], irq.charger[2], irq.charger[3], irq.charger[4],
                    irq.muic[0], irq.muic[1], irq.fuel_gauge, drainResult, armed, drainFailures);
            }
            if (unowned != lastUnownedCount) {
                printf("T733 battery: masked unowned TLMM IRQ GPIO=%u count=%u\n", unownedPin, unowned);
            }
            lastIRQCount = count;
            lastUnownedCount = unowned;
        }
        waitForBatteryEvent(waitMilliseconds);
    }
}

extern "C" void t733_battery_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("t733-battery", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    static unsigned started;
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }

    thread_t thread = THREAD_NULL;
    kern_return_t result = kernel_thread_start(batteryWorker, nullptr, &thread);
    if (result == KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        printf("T733 battery: worker start failed=%d\n", result);
    }
}
