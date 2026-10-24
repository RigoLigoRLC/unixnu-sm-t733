// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void displayStaticInit(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" void t733_keystore_start(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);

static IOService *pendingDisplay;
static IOService *activeDisplay;
static IOPlatformDevice *activeProvider;
static bool initialized;
static bool consoleReleased;
static uint32_t completedFrames;
static uint32_t rejectedFrames;

static bool videoAvailable(PE_Video &video) {
    return !PE_current_console(&video) && video.v_baseAddr &&
        video.v_width == 1600 && video.v_height == 2560 &&
        video.v_depth == 32 && !video.v_rotate &&
        video.v_rowBytes >= video.v_width * 4 && video.v_rowBytes <= 65536;
}

static bool publishWord(IORegistryEntry *entry, const char *name, uint32_t value) {
    OSData *data = OSData::withBytes(&value, sizeof(value));
    bool result = data && entry->setProperty(name, data);
    OSSafeReleaseNULL(data);
    return result;
}

extern "C" void native_legacy_display_probe(void) {
    if (initialized) {
        return;
    }
    initialized = true;

    PE_Video video = {};
    if (!videoAvailable(video)) {
        IOLog("T733 display: firmware framebuffer geometry unavailable\n");
        return;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        return;
    }
    displayStaticInit();
    OSReturn status = OSMetaClass::postModLoad(handle);
    if (status) {
        IOLog("T733 display: class registration failed %x\n", status);
        return;
    }

    IOService *display = native_legacy_display_allocate();
    if (!display || !display->init()) {
        OSSafeReleaseNULL(display);
        IOLog("T733 display: instance initialization failed\n");
        return;
    }
    pendingDisplay = display;
    IOLog("T733 display: initialized original Legacy, waiting for IOBSD\n");
}

extern "C" void native_legacy_display_late_probe(void) {
    t733_keystore_start();
    IOService *display = pendingDisplay;
    if (!display) {
        return;
    }
    pendingDisplay = nullptr;

    kern_return_t load = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    bool surfaceReady = load == KERN_SUCCESS && surface;
    OSSafeReleaseNULL(surface);
    OSSafeReleaseNULL(matching);
    PE_Video video = {};
    if (!surfaceReady || !videoAvailable(video)) {
        IOLog("T733 display: IOSurface unavailable status=%x\n", load);
        display->release();
        return;
    }

    IOService *parent = IOService::getResourceService();
    IORegistryEntry *treeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    IOPlatformDevice *provider = new IOPlatformDevice;
    IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
    OSArray *ranges = OSArray::withCapacity(1);
    const char type[] = "display-subsystem";
    OSData *typeData = OSData::withBytes(type, sizeof(type));
    bool providerAttached = false, providerStarted = false, treeAttached = false;
    bool displayAttached = false, displayStarted = false;
    if (parent && treeParent && provider && memory && ranges && typeData && provider->init()) {
        ranges->setObject(memory);
        provider->setDeviceMemory(ranges);
        provider->setName("T733FirmwareFramebuffer");
        providerAttached = provider->attach(parent);
        if (providerAttached) {
            providerStarted = provider->start(parent);
        }
        if (providerStarted && provider->setProperty("device_type", typeData)) {
            provider->setName("gop-display", gIODTPlane);
            treeAttached = provider->attachToParent(treeParent, gIODTPlane);
        }
        SInt32 score = 0;
        if (treeAttached && display->probe(provider, &score) == display) {
            displayAttached = display->attach(provider);
        }
        if (displayAttached) {
            displayStarted = display->start(provider);
        }
    }

    OSSafeReleaseNULL(typeData);
    OSSafeReleaseNULL(ranges);
    OSSafeReleaseNULL(memory);
    if (displayStarted) {
        activeDisplay = display;
        activeProvider = provider;
        OSSafeReleaseNULL(treeParent);
        IOLog("T733 display: CPU framebuffer service ready 1600x2560 stride=%lu\n", video.v_rowBytes);
        return;
    }

    IOLog("T733 display: start failed provider=%u tree=%u attached=%u\n",
          providerStarted, treeAttached, displayAttached);
    if (displayAttached) {
        display->detach(provider);
    }
    if (treeAttached) {
        provider->detachFromParent(treeParent, gIODTPlane);
    }
    if (providerStarted) {
        provider->stop(parent);
    }
    if (providerAttached) {
        provider->detach(parent);
    }
    OSSafeReleaseNULL(provider);
    OSSafeReleaseNULL(treeParent);
    display->release();
}

extern "C" bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *mapping,
                                                   uint32_t width, uint32_t height) {
    PE_Video video = {};
    if (!videoAvailable(video) || width != 1600 || height != 2560 ||
        mapping->getPhysicalAddress() != video.v_baseAddr) {
        return false;
    }

    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) {
        return false;
    }
    bool result = publishWord(chosen, "display-scale", 2) && publishWord(chosen, "display-rotation", 0);
    chosen->release();
    IOLog("T733 display: scanout cache option=%llx physical=%llx bytes=%llu\n",
          (uint64_t)(mapping->getMapOptions() & kIOMapCacheMask),
          (uint64_t)mapping->getPhysicalAddress(), (uint64_t)mapping->getLength());
    return result;
}

extern "C" bool native_legacy_prepare_scanout(void) {
    if (!consoleReleased) {
        IOLog("T733 display: first CPU frame, releasing text console\n");
        PE_initialize_console(nullptr, kPEDisableScreen);
        consoleReleased = true;
    }
    return true;
}

extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot, void *object, void *caller) {
    panic("T733 display: unsupported original backend slot=%u object=%p caller=%p frames=%u",
          slot, object, caller, completedFrames);
}

extern "C" void native_legacy_registration_observed(IOService *display) {
    IOLog("T733 display: registered %s\n", display->getMetaClass()->getClassName());
}

extern "C" void native_legacy_pm_observed(unsigned stage, IOReturn status, unsigned long ordinal) {
    IOLog("T733 display: PM stage=%u status=%x ordinal=%lu\n", stage, status, ordinal);
}

extern "C" void native_legacy_client_enter(task_t, uint32_t type) {
    IOLog("T733 display: opening user client type=%u\n", type);
}

extern "C" void native_legacy_client_observed(IOReturn status, IOUserClient *) {
    IOLog("T733 display: user client status=%x\n", status);
}

extern "C" void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *, const void *) {
    if (!accepted && rejectedFrames++ < 16) {
        IOLog("T733 display: rejected swap reason=%u\n", reason);
    }
}

extern "C" void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format,
                                               uint32_t planes, uint64_t stride, uint64_t allocation, uint64_t bytes) {
    if (rejectedFrames < 16) {
        IOLog("T733 display: surface %llux%llu format=%x planes=%u stride=%llu allocation=%llu bytes=%llu\n",
              width, height, format, planes, stride, allocation, bytes);
    }
}

extern "C" void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes) {
    if (status || completedFrames < 4) {
        IOLog("T733 display: CPU transfer status=%x bytes=%llu\n", status, bytes);
    }
}

extern "C" void native_legacy_retirement_observed(IOService *) {
    ++completedFrames;
    if (completedFrames <= 4) {
        IOLog("T733 display: completed frame=%u\n", completedFrames);
    }
}

extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes) {
    if (completedFrames < 4) {
        IOLog("T733 display: CPU copy ticks=%llu bytes=%llu\n", ticks, bytes);
    }
}

extern "C" void t733_display_mapping_observed(uint64_t scanout, uint64_t surface) {
    static bool reported;
    if (reported) {
        return;
    }
    reported = true;

    // A fresh source read resolves any access-flag fault while interrupts are
    // still enabled, before AT inspects the current page-table entry.
    const volatile uint32_t *sourcePixel = reinterpret_cast<const volatile uint32_t *>(surface);
    const uint32_t pixel = *sourcePixel;
    (void)pixel;

    // Both mappings have been accessed by the first copy. The hardware
    // translation reports their effective attributes while they remain mapped.
    uint64_t savedPar, scanoutPar, surfacePar, mair, sctlr, mpidr;
    const boolean_t interruptsEnabled = ml_set_interrupts_enabled(FALSE);
    __asm__ volatile(
        "mrs %0, par_el1\n\t"
        "at s1e1r, %6\n\t"
        "isb\n\t"
        "mrs %1, par_el1\n\t"
        "at s1e1r, %7\n\t"
        "isb\n\t"
        "mrs %2, par_el1\n\t"
        "mrs %3, mair_el1\n\t"
        "mrs %4, sctlr_el1\n\t"
        "mrs %5, mpidr_el1\n\t"
        "msr par_el1, %0\n\t"
        : "=&r"(savedPar), "=&r"(scanoutPar), "=&r"(surfacePar),
          "=&r"(mair), "=&r"(sctlr), "=&r"(mpidr)
        : "r"(scanout), "r"(surface)
        : "memory");
    ml_set_interrupts_enabled(interruptsEnabled);

    IOLog("T733 display: mapping mpidr=%llx mair=%llx sctlr=%llx\n",
          mpidr, mair, sctlr);
    IOLog("T733 display: scanout va=%llx par=%llx attr=%02llx fault=%llu\n",
          scanout, scanoutPar, scanoutPar >> 56, scanoutPar & 1);
    IOLog("T733 display: surface va=%llx par=%llx attr=%02llx fault=%llu\n",
          surface, surfacePar, surfacePar >> 56, surfacePar & 1);
}

extern "C" void t733_display_submission_observed(uint64_t start, uint64_t mapTicks,
    uint64_t copyTicks, uint64_t totalTicks, uint64_t bytes,
    bool reused, uint64_t hits, uint64_t mappings, bool paired,
    uint64_t cpuStart, uint64_t cpuEnd, int boundCpu, uint64_t bindTicks) {
    static uint64_t frequency;
    static uint64_t previousStart;
    static uint64_t previousReport;
    static uint64_t submittedFrames;

    if (!frequency) {
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        IOLog("T733 display: CPU timing frequency=%llu scans=0\n",
              frequency);
    }

    const uint64_t interval = previousStart ? start-previousStart : 0;
    previousStart = start;
    ++submittedFrames;

    if (submittedFrames <= 4 || start-previousReport >= frequency*5) {
        previousReport = start;
        IOLog("T733 display: CPU frame=%llu interval_ticks=%llu map_ticks=%llu "
              "copy_ticks=%llu total_ticks=%llu bytes=%llu source_reused=%u "
              "map_hits=%llu map_new=%llu paired=%u cpu_start=%llx cpu_end=%llx "
              "bound_cpu=%d bind_ticks=%llu\n",
              submittedFrames, interval, mapTicks, copyTicks, totalTicks, bytes,
              reused, hits, mappings, paired, cpuStart, cpuEnd, boundCpu, bindTicks);
    }
}

extern "C" void native_legacy_vsync_observed(unsigned event, unsigned hz) {
    if (event == 4 || event == 5) {
        IOLog("T733 display: refresh event=%u hz=%u\n", event, hz);
    }
}

extern "C" bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t) {
    return false;
}

extern "C" void native_legacy_gpu_fallback_observed(int status) {
    IOLog("T733 display: CPU transfer selected status=%x\n", status);
}

extern "C" void native_legacy_swap_stage(unsigned) {
}

extern "C" void native_legacy_frame_observed(unsigned, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
}

extern "C" void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *, uint64_t) {
}
