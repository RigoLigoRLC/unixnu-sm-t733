// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733FuelGauge.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>

namespace {

// The selected stock DTS assigns SM5714 to SE3, using TLMM GPIO12/13.
// QUP03 IO0 is SDA and IO1 is SCL; the GPIO output latch stays low.
enum : unsigned {
    kSda = 0,
    kScl = 0x1000,
    kOutputEnable = 1u << 9,
    kFuelGaugeAddress = 0x71,
    kChargerAddress = 0x49,
    kMuicAddress = 0x25,
    kHalfPeriodUs = 10,
    kIRQSourceMuic = 1u << 0,
    kIRQSourceCharger = 1u << 1,
    kIRQSourceFuelGauge = 1u << 2,
    kIRQSources = kIRQSourceMuic | kIRQSourceCharger | kIRQSourceFuelGauge,
    kFuelGaugeIRQMask = 0x04,
    kFuelGaugeIRQLock = 1u << 7,
};

/* FuelGaugeBus owns the GPIO12/13 maps for the battery worker's SMBus reads.
 * Releasing a line selects GPIO input; driving it selects a zero output latch.
 * Writes select SRAM read addresses or configure interrupt masks; charger control
 * and gauge calibration registers retain their existing values.
 */
struct FuelGaugeBus {
    IOMemoryMap *mapping;
    uintptr_t base;
    uint32_t sdaConfig;
    uint32_t sclConfig;
    uint16_t deviceID;
    bool ready;
};

static FuelGaugeBus bus;

static uint32_t read32(unsigned offset)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(bus.base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void write32(unsigned offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(bus.base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void driveLow(unsigned pin)
{
    uint32_t config = pin == kSda ? bus.sdaConfig : bus.sclConfig;
    write32(pin, config | kOutputEnable);
}

static void release(unsigned pin)
{
    write32(pin, pin == kSda ? bus.sdaConfig : bus.sclConfig);
}

static bool level(unsigned pin)
{
    return (read32(pin + 4) & 1) != 0;
}

static int clockHigh()
{
    release(kScl);
    for (unsigned elapsed = 0; elapsed < 1000; elapsed += 5) {
        if (level(kScl)) {
            IODelay(kHalfPeriodUs);
            return 0;
        }
        IODelay(5);
    }
    return -60;
}

static int start()
{
    release(kSda);
    int result = clockHigh();
    if (result || !level(kSda)) {
        return result ? result : -16;
    }
    driveLow(kSda);
    IODelay(kHalfPeriodUs);
    driveLow(kScl);
    IODelay(kHalfPeriodUs);
    return 0;
}

static int stop()
{
    driveLow(kScl);
    driveLow(kSda);
    IODelay(kHalfPeriodUs);
    int result = clockHigh();
    release(kSda);
    IODelay(kHalfPeriodUs);
    release(kScl);
    return result;
}

static int writeByte(uint8_t value)
{
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (value & 0x80) {
            release(kSda);
        } else {
            driveLow(kSda);
        }
        IODelay(kHalfPeriodUs);
        int result = clockHigh();
        if (result) {
            return result;
        }
        driveLow(kScl);
        value <<= 1;
    }
    release(kSda);
    IODelay(kHalfPeriodUs);
    int result = clockHigh();
    if (!result && level(kSda)) {
        result = -6;
    }
    driveLow(kScl);
    IODelay(kHalfPeriodUs);
    return result;
}

static int readByte(uint8_t &value, bool acknowledge)
{
    value = 0;
    release(kSda);
    for (unsigned bit = 0; bit < 8; ++bit) {
        IODelay(kHalfPeriodUs);
        int result = clockHigh();
        if (result) {
            return result;
        }
        value = static_cast<uint8_t>((value << 1) | level(kSda));
        driveLow(kScl);
    }
    if (acknowledge) {
        driveLow(kSda);
    }
    IODelay(kHalfPeriodUs);
    int result = clockHigh();
    driveLow(kScl);
    release(kSda);
    IODelay(kHalfPeriodUs);
    return result;
}

static int readRegister(uint8_t address, uint8_t reg, uint8_t *data, unsigned length)
{
    int result = start();
    if (result) {
        return result;
    }
    result = writeByte(address << 1);
    if (!result) {
        result = writeByte(reg);
    }
    if (!result) {
        result = start();
    }
    if (!result) {
        result = writeByte((address << 1) | 1);
    }
    for (unsigned i = 0; !result && i < length; ++i) {
        result = readByte(data[i], i + 1 < length);
    }
    int stopResult = stop();
    return result ? result : stopResult;
}

static int readWord(uint8_t reg, uint16_t &value)
{
    uint8_t bytes[2] = {};
    int result = readRegister(kFuelGaugeAddress, reg, bytes, 2);
    value = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
    return result;
}

static int writeRegister(uint8_t address, uint8_t reg, const uint8_t *data,
    unsigned length)
{
    int result = start();
    if (result) {
        return result;
    }
    result = writeByte(address << 1);
    if (!result) {
        result = writeByte(reg);
    }
    for (unsigned index = 0; !result && index < length; ++index) {
        result = writeByte(data[index]);
    }
    int stopResult = stop();
    return result ? result : stopResult;
}

static int writeGaugeIRQMask(uint16_t mask)
{
    const uint8_t bytes[] = {static_cast<uint8_t>(mask),
        static_cast<uint8_t>(mask >> 8)};
    return writeRegister(kFuelGaugeAddress, kFuelGaugeIRQMask, bytes, sizeof(bytes));
}

static int readSram(uint16_t address, uint16_t &value)
{
    int result = start();
    if (result) {
        return result;
    }
    // 0x8c selects the read address; 0x8e/0x8f SRAM write ports are never used.
    const uint8_t command[] = {kFuelGaugeAddress << 1, 0x8c,
        static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8)};
    for (unsigned i = 0; !result && i < sizeof(command); ++i) {
        result = writeByte(command[i]);
    }
    int stopResult = stop();
    if (result || stopResult) {
        return result ? result : stopResult;
    }
    return readWord(0x8d, value);
}

static bool recordResult(int result, int &firstError)
{
    if (result && !firstError) {
        firstError = result;
    }
    return result == 0;
}

static int readIRQGroups(uint8_t source, T733FuelGaugeIRQ &irq)
{
    int firstError = 0;
    irq.source_seen |= source;
    if (source & kIRQSourceCharger) {
        uint8_t status[5] = {};
        if (recordResult(readRegister(kChargerAddress, 0x01, status,
            sizeof(status)), firstError)) {
            for (unsigned index = 0; index < sizeof(status); ++index) {
                irq.charger[index] |= status[index];
            }
        }
    }
    if (source & kIRQSourceMuic) {
        uint8_t status[2] = {};
        if (recordResult(readRegister(kMuicAddress, 0x01, status,
            sizeof(status)), firstError)) {
            for (unsigned index = 0; index < sizeof(status); ++index) {
                irq.muic[index] |= status[index];
            }
        }
    }
    if (source & kIRQSourceFuelGauge) {
        uint16_t mask = 0;
        if (recordResult(readWord(kFuelGaugeIRQMask, mask), firstError)) {
            mask &= ~kFuelGaugeIRQLock;
            if (recordResult(writeGaugeIRQMask(mask | kFuelGaugeIRQLock), firstError)) {
                uint16_t status = 0;
                if (recordResult(readWord(0x02, status), firstError)) {
                    irq.fuel_gauge |= status;
                }
            }
            // Unlock even if the preceding write or read failed on the bus.
            recordResult(writeGaugeIRQMask(mask), firstError);
        }
    }
    return firstError;
}

static int setIRQMasks(bool enable)
{
    // CHG: VBUS good/UVLO/OVP, battery presence, charging, top-off and done.
    const uint8_t charger[] = {static_cast<uint8_t>(enable ? 0xf8 : 0xff),
        static_cast<uint8_t>(enable ? 0x93 : 0xff), 0xff, 0xff, 0xff};
    // MUIC: VBUS/RID detach, charger type and VBUS voltage update.
    const uint8_t muic[] = {static_cast<uint8_t>(enable ? 0xed : 0xff),
        static_cast<uint8_t>(enable ? 0xfb : 0xff)};
    int firstError = 0;
    for (unsigned index = 0; index < sizeof(charger); ++index) {
        recordResult(writeRegister(kChargerAddress, 0x07 + index,
            &charger[index], 1), firstError);
    }
    for (unsigned index = 0; index < sizeof(muic); ++index) {
        recordResult(writeRegister(kMuicAddress, 0x03 + index,
            &muic[index], 1), firstError);
    }
    uint16_t gaugeMask = 0;
    if (recordResult(readWord(kFuelGaugeIRQMask, gaugeMask), firstError)) {
        gaugeMask = (gaugeMask | 0x000f) & ~kFuelGaugeIRQLock;
        recordResult(writeGaugeIRQMask(gaugeMask), firstError);
    }
    return firstError;
}

} // namespace

extern "C" int t733_fuel_gauge_irq_setup(void)
{
    int result = t733_fuel_gauge_init();
    if (result) {
        return result;
    }
    result = setIRQMasks(false);
    if (result) {
        return result;
    }
    T733FuelGaugeIRQ initial = {};
    result = readIRQGroups(kIRQSources, initial);
    if (!result) {
        result = setIRQMasks(true);
    }
    if (result) {
        (void)setIRQMasks(false);
    }
    return result;
}

extern "C" int t733_fuel_gauge_irq_drain(struct T733FuelGaugeIRQ *irq)
{
    if (!irq) {
        return -22;
    }
    bzero(irq, sizeof(*irq));
    if (!bus.ready) {
        return -19;
    }
    for (unsigned pass = 0; pass <= 4; ++pass) {
        int result = readRegister(kChargerAddress, 0x00, &irq->source_remaining, 1);
        if (result) {
            return result;
        }
        if (!irq->source_remaining) {
            return 0;
        }
        if (irq->source_remaining & ~kIRQSources) {
            return -5;
        }
        if (pass == 4) {
            return -35;
        }
        ++irq->passes;
        result = readIRQGroups(irq->source_remaining, *irq);
        if (result) {
            return result;
        }
    }
    return -35;
}

extern "C" int t733_fuel_gauge_init(void)
{
    if (bus.ready) {
        return 0;
    }
    if (!bus.mapping) {
        IODeviceMemory *memory = IODeviceMemory::withRange(0x0f10c000, 0x2000);
        if (!memory) {
            return -12;
        }
        bus.mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
        if (!bus.mapping) {
            return -12;
        }
        bus.base = static_cast<uintptr_t>(bus.mapping->getVirtualAddress());
        if (!bus.base) {
            OSSafeReleaseNULL(bus.mapping);
            return -12;
        }

        uint32_t sdaBefore = read32(kSda);
        uint32_t sclBefore = read32(kScl);
        // GPIO function, 2 mA and pull-up match the stock SE3 electrical settings.
        bus.sdaConfig = (sdaBefore & ~0x3ffu) | 3u;
        bus.sclConfig = (sclBefore & ~0x3ffu) | 3u;
        release(kSda);
        release(kScl);
        write32(kSda + 4, 0);
        write32(kScl + 4, 0);
        IODelay(50);
        printf("T733 fuel: GPIO I2C SDA12=%x->%x SCL13=%x->%x levels=%u/%u\n",
            sdaBefore, read32(kSda), sclBefore, read32(kScl), level(kSda), level(kScl));
    }

    uint8_t chargerID = 0;
    int result = readRegister(kChargerAddress, 0x50, &chargerID, 1);
    if (!result && (chargerID & 7) != 1) {
        result = -19;
    }
    if (!result) {
        result = readWord(0x00, bus.deviceID);
    }
    if (!result && bus.deviceID >= 0xff) {
        result = -19;
    }
    printf("T733 fuel: charger-id=%x gauge-id=%x result=%d lines=%u/%u\n",
        chargerID, bus.deviceID, result, level(kSda), level(kScl));
    bus.ready = result == 0;
    return result;
}

extern "C" int t733_fuel_gauge_read(struct T733FuelGaugeSample *sample)
{
    if (!sample) {
        return -22;
    }
    bzero(sample, sizeof(*sample));
    int result = t733_fuel_gauge_init();
    if (result) {
        return result;
    }
    sample->device_id = bus.deviceID;
    int firstError = 0;
    uint8_t status1 = 0, status2 = 0;
    bool chargerValid = recordResult(readRegister(kChargerAddress, 0x0d, &status1, 1), firstError);
    chargerValid &= recordResult(readRegister(kChargerAddress, 0x0e, &status2, 1), firstError);
    if (chargerValid) {
        sample->charger_status1 = status1;
        sample->charger_status2 = status2;
        sample->battery_present = !(status2 & 4);
        sample->external_power = (status1 & 1) != 0;
        sample->full = (status2 & 0x20) != 0;
        sample->charging = !sample->full && (status2 & 8) != 0;
        sample->valid |= T733_FG_CHARGER_STATUS;
    }

    if (!recordResult(readWord(0x10, sample->system_status), firstError)) {
        return firstError;
    }
    // The gauge must retain its programmed battery model; this driver never initializes it.
    if (!(sample->system_status & 0x1000) || sample->system_status == 0xffff) {
        return firstError ? firstError : -35;
    }
    bool auxiliaryValid = recordResult(readWord(0x22, sample->auxiliary_status), firstError);
    uint16_t raw = 0;
    if (recordResult(readSram(0x00, raw), firstError)) {
        sample->raw_soc = raw;
        if (raw <= 0x6500) {
            sample->soc_permille = (static_cast<uint32_t>(raw) * 10) >> 8;
            if (sample->soc_permille > 1000) {
                sample->soc_permille = 1000;
            }
            sample->valid |= T733_FG_SOC;
        } else {
            recordResult(-5, firstError);
        }
    }
    if (recordResult(readSram(0x03, raw), firstError)) {
        int delta = (static_cast<int>(raw & 0x7fff) * 10) / 109;
        sample->voltage_mv = 2700 + ((raw & 0x8000) ? -delta : delta);
        sample->valid |= T733_FG_VOLTAGE;
    }
    if (recordResult(readSram(0x05, raw), firstError) && auxiliaryValid) {
        int current = (static_cast<int>(raw & 0x7fff) * 1000) / 2044;
        if (raw & 0x8000) {
            current = -current;
        } else if (!(sample->auxiliary_status & 0x20) && chargerValid && !sample->charging) {
            current = 0;
        }
        if (sample->auxiliary_status & 0x400) {
            current = current * 25 / 10;
        }
        if (sample->system_status & 0x8000) {
            current = 0;
        }
        sample->current_ma = current;
        sample->valid |= T733_FG_CURRENT;
    }
    if (recordResult(readSram(0x07, raw), firstError)) {
        int temperature = ((static_cast<int>(raw & 0x7fff) * 10 * 2989) >> 11) >> 8;
        sample->gauge_temperature_deci_c = (raw & 0x8000) ? -temperature : temperature;
        sample->valid |= T733_FG_TEMPERATURE;
    }
    if (recordResult(readSram(0x87, raw), firstError)) {
        sample->cycle_count = raw & 0xff;
        sample->valid |= T733_FG_CYCLE;
    }
    return firstError;
}
