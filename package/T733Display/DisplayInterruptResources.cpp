// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeDpuInterrupt.h"

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <string.h>

extern "C" IOReturn armcpu_spi_register(uint32_t interrupt, bool edge,
    void (*callback)(void *), void *context);
extern "C" IOReturn armcpu_spi_enable(uint32_t interrupt);
extern "C" IOReturn armcpu_spi_disable(uint32_t interrupt);

namespace {

bool readProperty(IORegistryEntry *entry, const char *name, void *result, unsigned bytes) {
    const OSData *data = OSDynamicCast(OSData, entry->getProperty(name));
    if (!data || data->getLength() != bytes) {
        return false;
    }
    memcpy(result, data->getBytesNoCopy(), bytes);

    return true;
}

}

extern "C" bool native_dpu_interrupt_resources(NativeDpuInterruptResources *resources) {
    if (!resources) {
        return false;
    }
    IORegistryEntry *display = IORegistryEntry::fromPath("/arm-io/mdss", gIODTPlane);
    IORegistryEntry *bus = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    if (!display || !bus) {
        OSSafeReleaseNULL(display);
        OSSafeReleaseNULL(bus);
        return false;
    }

    NativeDpuInterruptResources result = {};
    uint64_t reg[2] = {};
    uint64_t ranges[3] = {};
    uint32_t interrupt[3] = {};
    const bool valid = readProperty(display, "reg", reg, sizeof(reg)) &&
        readProperty(bus, "ranges", ranges, sizeof(ranges)) &&
        readProperty(display, "interrupts", interrupt, sizeof(interrupt)) &&
        readProperty(display, "native-interrupt-base", &result.interruptBase, sizeof(uint32_t)) &&
        readProperty(display, "native-interface-base", &result.interfaceBase, sizeof(uint32_t)) &&
        readProperty(display, "native-control-base", &result.controlBase, sizeof(uint32_t)) &&
        readProperty(display, "native-vsync-mask", &result.vsyncMask, sizeof(uint32_t));
    display->release();
    bus->release();
    if (!valid || interrupt[0] != 0 || interrupt[1] > 987 || interrupt[2] != 4 ||
        reg[0] < ranges[0] || reg[0] - ranges[0] > ranges[2] ||
        reg[1] > ranges[2] - (reg[0] - ranges[0]) ||
        ranges[1] > UINT64_MAX - (reg[0] - ranges[0])) {
        return false;
    }

    result.physical = ranges[1] + reg[0] - ranges[0];
    result.bytes = reg[1];
    result.interrupt = interrupt[1] + 32;
    *resources = result;

    return true;
}

extern "C" IOReturn native_dpu_interrupt_register(uint32_t interrupt,
    void (*callback)(void *), void *context) {
    return armcpu_spi_register(interrupt, false, callback, context);
}

extern "C" IOReturn native_dpu_interrupt_enable(uint32_t interrupt) {
    return armcpu_spi_enable(interrupt);
}

extern "C" IOReturn native_dpu_interrupt_disable(uint32_t interrupt) {
    return armcpu_spi_disable(interrupt);
}
