// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <string.h>
#include "NativeAdrenoA6xxService.h"

static const OSData *property(IORegistryEntry *entry, const char *name)
{
    return OSDynamicCast(OSData, entry->getProperty(name));
}

static bool scalar(IORegistryEntry *entry, const char *name, uint32_t &value)
{
    const OSData *data = property(entry, name);
    if (!data || data->getLength() != sizeof(value)) {
        return false;
    }

    memcpy(&value, data->getBytesNoCopy(), sizeof(value));
    return true;
}

extern "C" int native_adreno_a6xx_resources(NativeAdrenoA6xxResources *resources)
{
    if (!resources) {
        return -1;
    }

    IORegistryEntry *gpu = IORegistryEntry::fromPath("/arm-io/adreno", gIODTPlane);
    IORegistryEntry *bus = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    if (!gpu || !bus) {
        if (gpu) {
            gpu->release();
        }
        if (bus) {
            bus->release();
        }
        return -1;
    }

    NativeAdrenoA6xxResources result = {};
    const OSData *reg = property(gpu, "reg");
    const OSData *ranges = property(bus, "ranges");
    const OSData *platform = property(gpu, "native-adreno-platform-reg");
    const OSData *names = property(gpu, "native-adreno-reg-names");
    bool valid = reg && reg->getLength() == 16 && ranges && ranges->getLength() >= 24 &&
        platform && platform->getLength() % 16 == 0 && names;

    if (valid) {
        uint64_t gpuReg[2];
        uint64_t busRange[3];
        memcpy(gpuReg, reg->getBytesNoCopy(), sizeof(gpuReg));
        memcpy(busRange, ranges->getBytesNoCopy(), sizeof(busRange));
        valid = gpuReg[0] >= busRange[0] && gpuReg[1] &&
            gpuReg[0] - busRange[0] <= busRange[2] &&
            gpuReg[1] <= busRange[2] - (gpuReg[0] - busRange[0]);
        result.gpu = {busRange[1] + gpuReg[0] - busRange[0], gpuReg[1]};
    }

    struct NamedWindow {
        const char *name;
        NativeAdrenoA6xxWindow *window;
    };
    const NamedWindow windows[] = {
        {"gmu", &result.gmu}, {"smmu", &result.smmu},
        {"gpucc", &result.gpucc}, {"gcc", &result.gcc},
        {"pdc", &result.pdc}, {"rscc", &result.rscc},
        {"command-db", &result.commandDb}, {"rpmh", &result.rpmh},
        {"aop", &result.aop}, {"ipcc", &result.ipcc}, {"cx-misc", &result.cxMisc},
        {"zap", &result.zap},
    };

    if (valid) {
        const char *text = static_cast<const char *>(names->getBytesNoCopy());
        size_t cursor = 0;
        const size_t nameBytes = names->getLength();
        const size_t count = platform->getLength() / 16;
        const uint8_t *pairs = static_cast<const uint8_t *>(platform->getBytesNoCopy());
        for (size_t index = 0; index < count && valid; ++index) {
            if (cursor >= nameBytes) {
                valid = false;
                break;
            }

            size_t length = strnlen(text + cursor, nameBytes - cursor);
            if (length == nameBytes - cursor) {
                valid = false;
                break;
            }

            bool found = false;
            for (const NamedWindow &candidate : windows) {
                if (strcmp(candidate.name, text + cursor) == 0) {
                    if (candidate.window->bytes) {
                        valid = false;
                        break;
                    }

                    memcpy(candidate.window, pairs + index * 16, 16);
                    found = candidate.window->bytes &&
                        candidate.window->physical + candidate.window->bytes > candidate.window->physical;
                    break;
                }
            }
            valid = valid && found;
            cursor += length + 1;
        }
        valid = valid && cursor == nameBytes;
        for (const NamedWindow &candidate : windows) {
            valid = valid && candidate.window->bytes;
        }
    }

    struct NamedScalar {
        const char *name;
        uint32_t *value;
    };
    const NamedScalar scalars[] = {
        {"native-adreno-chip-id", &result.chipId},
        {"native-adreno-gmem-bytes", &result.gmemBytes},
        {"native-adreno-frequency-hz", &result.frequencyHz},
        {"native-adreno-highest-bank-bit", &result.highestBankBit},
        {"native-adreno-gpu-bank", &result.gpuBank},
        {"native-adreno-gpu-asid", &result.gpuAsid},
        {"native-adreno-gpu-slot", &result.gpuSlot},
        {"native-adreno-gpu-sid", &result.gpuSid},
        {"native-adreno-gpu-sid-mask", &result.gpuSidMask},
        {"native-adreno-gmu-bank", &result.gmuBank},
        {"native-adreno-gmu-asid", &result.gmuAsid},
        {"native-adreno-gmu-slot", &result.gmuSlot},
        {"native-adreno-gmu-sid", &result.gmuSid},
        {"native-adreno-gmu-sid-mask", &result.gmuSidMask},
    };
    for (const NamedScalar &candidate : scalars) {
        valid = scalar(gpu, candidate.name, *candidate.value) && valid;
    }

    gpu->release();
    bus->release();
    if (!valid) {
        return -1;
    }

    *resources = result;
    return 0;
}
