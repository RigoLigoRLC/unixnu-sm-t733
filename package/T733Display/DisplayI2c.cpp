// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include "NativeGeniI2c.h"

namespace {

static IOMemoryMap *gccMap;
static IOMemoryMap *pinsMap;
static uintptr_t gcc;
static uintptr_t pins;
static NativeGeniI2c *bus;

static uint32_t read(uintptr_t base, unsigned offset)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void write(uintptr_t base, unsigned offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static IOMemoryMap *map(uint64_t physical, size_t size, uintptr_t &base)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(physical, size);
    if (!memory) {
        return nullptr;
    }
    IOMemoryMap *mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (mapping) {
        base = mapping->getVirtualAddress();
    }
    return mapping;
}

static bool enableClockBranch(unsigned mask, unsigned branch, bool hardwareGating)
{
    write(gcc, 0x52008, read(gcc, 0x52008) | mask);
    if ((read(gcc, 0x52008) & mask) != mask) {
        return false;
    }
    if (hardwareGating && (read(gcc, branch) & 2)) {
        return true;
    }
    for (unsigned i = 0; i < 1000; ++i) {
        uint32_t state = read(gcc, branch);
        if (!(state & 0x80000000u) || (state & 0xf0000000u) == 0x20000000u) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

static int prepareGpio(void *)
{
    for (unsigned line = 0; line < 2; ++line) {
        unsigned offset = line * 0x1000;
        write(pins, offset + 4, read(pins, offset + 4) & ~2u);
        write(pins, offset, (read(pins, offset) & ~0x3ffu) | 3);
    }
    return 0;
}

static void driveLow(void *, unsigned line, bool low)
{
    unsigned offset = line * 0x1000;
    uint32_t config = read(pins, offset);
    write(pins, offset, (config & ~0x200u) | (low ? 0x200u : 0u));
}

static int readLine(void *, unsigned line)
{
    return read(pins, line * 0x1000 + 4) & 1;
}

} // namespace

extern "C" int t733_display_i2c_start(void)
{
    if (bus) {
        return 0;
    }
    gccMap = map(0x00100000, 0x60000, gcc);
    pinsMap = map(0x0f128000, 0x2000, pins);
    if (!gccMap || !pinsMap) {
        return -12;
    }
    if (!enableClockBranch(1u << 20, 0x18004, true) || !enableClockBranch(1u << 21, 0x18008, true)) {
        return -60;
    }
    write(gcc, 0x18274, (read(gcc, 0x18274) & ~0x371fu) | 1);
    write(gcc, 0x18270, read(gcc, 0x18270) | 1);
    unsigned polls = 0;
    while ((read(gcc, 0x18270) & 1) && polls++ < 1000) {
        IODelay(10);
    }
    if ((read(gcc, 0x18270) & 1) || !enableClockBranch(1u << 24, 0x1826c, false)) {
        return -60;
    }
    // GPIO40/41 select QUP12 at 2 mA with the stock pull-ups.
    write(pins, 0, (read(pins, 0) & ~0x3ffu) | 7);
    write(pins, 0x1000, (read(pins, 0x1000) & ~0x3ffu) | 7);
    const NativeGeniI2cConfig config = {
        0x00a88000, 0x4000, 0x00ac0000, 0x2000, 19200000, 100000, 100000,
        nullptr, prepareGpio, driveLow, readLine
    };
    int result = native_geni_i2c_start(&config, &bus);
    IOLog("T733 display: SE10 I2C start result=%d\n", result);
    return result;
}

extern "C" int t733_display_boost_voltage(void)
{
    if (!bus) {
        return -19;
    }
    const uint8_t setting[] = {0x04, 0x78};
    int result = native_geni_i2c_write(bus, 0x18, setting, sizeof(setting));
    uint8_t value = 0;
    if (!result) {
        result = native_geni_i2c_write_read(bus, 0x18, setting, 1, &value, 1);
    }
    if (!result && value != 0x78) {
        result = -5;
    }
    IOLog("T733 display: MAX77816 voltage=%02x result=%d\n", value, result);
    return result;
}
