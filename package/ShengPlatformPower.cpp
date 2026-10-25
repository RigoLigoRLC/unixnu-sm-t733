// SPDX-License-Identifier: MIT
#include "OriginalPMGRABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void sheng_pmgr_static_init(void) __asm__("__GLOBAL__sub_I_ShengPlatformPower.cpp");
IOService *sheng_original_pmgr_nub_create(IORegistryEntry *, ApplePMGR *);
extern int (*PE_halt_restart)(unsigned int);
int native_ufs_shutdown_complete(void) __attribute__((weak_import));
}

// The platform callback reads the completion flag set by the UFS power-tree
// notification before PSCI stops the machine. It cannot wait for storage here
// because IOKit may already have disabled interrupts. Panic restart uses PSCI directly.
static int t733_halt_restart(unsigned type)
{
    uint64_t function;
    bool orderly = false;
    switch (type) {
        case kPEHaltCPU:
        case kPEUPSDelayHaltCPU:
            function = 0x84000008;
            orderly = true;
            break;
        case kPERestartCPU:
            function = 0x84000009;
            orderly = true;
            break;
        case kPEPanicRestartCPU:
            function = 0x84000009;
            break;
        default:
            return 0;
    }

    if (orderly && native_ufs_shutdown_complete && !native_ufs_shutdown_complete()) {
        IOLog("T733PlatformPower: storage shutdown incomplete\n");
        return -1;
    }

    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("dsb sy\n\tsmc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");

    IOLog("T733PlatformPower: PSCI %llx returned %lld\n", function, x0);
    return -1;
}

__attribute__((noreturn)) static void unavailable(unsigned slot)
{
    panic("T733PlatformPower: unsupported power operation %u", slot);
}

// ShengPlatformPower supplies the original ApplePMGRNub with a live provider.
// Its AON methods use the original ApplePMGR implementations. CPU and platform
// power methods stop at the operation requested because SM7325 has no backend.
class ShengPlatformPower : public ApplePMGR {
    OSDeclareDefaultStructors(ShengPlatformPower)
public:
    void enableCPUCore(unsigned, uint64_t) override
    {
        unavailable(168);
    }
    void enableCPUCore(unsigned) override
    {
        unavailable(169);
    }
    void disableCPUCore(unsigned) override
    {
        unavailable(170);
    }
    void enableCPUCluster(unsigned) override
    {
        unavailable(171);
    }
    void disableCPUCluster(unsigned) override
    {
        unavailable(172);
    }
    void initCPUIdle(ml_processor_info_t *) override
    {
        unavailable(173);
    }
    void enterCPUIdle(UInt64 *) override
    {
        unavailable(174);
    }
    void exitCPUIdle(UInt64 *) override
    {
        unavailable(175);
    }
    void updateCPUIdle(UInt64 *) override
    {
        unavailable(176);
    }
#include "GuardedPMGRSlots.inc"
};

OSDefineMetaClassWithInit(ShengPlatformPower, ApplePMGR, )
OSMetaClassConstructorInit(ShengPlatformPower, ApplePMGR, )
OSDefineBasicStructors(ShengPlatformPower, ApplePMGR)

// The original provider exceeds kalloc_type's 16 KiB allocation limit.
void *ShengPlatformPower::operator new(size_t size)
{
    return OSObject::operator new(size);
}

void ShengPlatformPower::operator delete(void *memory, size_t size)
{
    OSObject::operator delete(memory, size);
}

static_assert(sizeof(ShengPlatformPower) == 0x4b8a8, "Original provider storage");

static const OSMetaClass *lookup(const char *text)
{
    const OSSymbol *name = OSSymbol::withCString(text);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return meta;
}

extern "C" void sheng_pmgr_run(void)
{
    unsigned mode = 0;
    if (!PE_parse_boot_argn("sheng-pmgr", &mode, sizeof(mode)) || mode != 2) {
        panic("T733PlatformPower: sheng-pmgr=2 is required");
    }

    kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
    if (result != KERN_SUCCESS) {
        panic("T733PlatformPower: ApplePMGR load failed 0x%x", result);
    }

    const OSMetaClass *parent = lookup("ApplePMGR");
    const OSMetaClass *nubMeta = lookup("ApplePMGRNub");
    if (parent != &ApplePMGR::gMetaClass || parent->getClassSize() != 0x4b8a8 ||
        !nubMeta || nubMeta->getClassSize() != 0x740) {
        panic("T733PlatformPower: original class ABI mismatch");
    }

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        panic("T733PlatformPower: metaclass registration unavailable");
    }
    sheng_pmgr_static_init();
    result = OSMetaClass::postModLoad(handle);
    if (result != KERN_SUCCESS) {
        panic("T733PlatformPower: metaclass registration failed 0x%x", result);
    }

    IOService *platform = IOService::getPlatform();
    ShengPlatformPower *provider = new ShengPlatformPower;
    IORegistryEntry *description = new IORegistryEntry;
    if (!platform || !provider || !description || !provider->init() || !description->init()) {
        panic("T733PlatformPower: provider allocation failed");
    }

    // The registry description contains no register ranges or hardware devices.
    description->setName("gts7fewifi-platform-power");
    IOService *nub = sheng_original_pmgr_nub_create(description, provider);
    if (!nub || nub->getMetaClass() != nubMeta || !provider->metaCast("ApplePMGR")) {
        panic("T733PlatformPower: original nub creation failed");
    }
    if (!provider->attach(platform) || !provider->start(platform) ||
        !nub->attach(provider) || !nub->start(provider)) {
        panic("T733PlatformPower: service attachment failed");
    }

    provider->registerService(kIOServiceSynchronous);
    nub->registerService(kIOServiceSynchronous);
    OSSafeReleaseNULL(description);
    PE_halt_restart = t733_halt_restart;
    printf("T733PlatformPower: original ApplePMGRNub published\n");
}
