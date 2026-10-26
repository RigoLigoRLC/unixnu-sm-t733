// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void keyStoreStaticInit(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_keyevents_allocate(void);
static IOService *keyService;
static bool attempted;
static uint32_t calls;
static uint8_t softwareWrappingKey[32];
static bool softwareWrappingKeyReady;

static void loadSoftwareWrappingKey(void) {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) {
        return;
    }

    OSData *data = OSDynamicCast(OSData, chosen->getProperty("software-keystore-key"));
    if (data && data->getLength() == sizeof(softwareWrappingKey)) {
        bcopy(data->getBytesNoCopy(), softwareWrappingKey, sizeof(softwareWrappingKey));
        softwareWrappingKeyReady = true;
    }

    chosen->removeProperty("software-keystore-key");
    chosen->release();
}

extern "C" bool native_aks_copy_software_key(uint8_t *out, size_t capacity) {
    if (!softwareWrappingKeyReady || !out || capacity < sizeof(softwareWrappingKey)) {
        return false;
    }

    bcopy(softwareWrappingKey, out, sizeof(softwareWrappingKey));
    return true;
}

extern "C" bool native_aks_state_enabled(void) {
    unsigned enabled = 0;
    return PE_parse_boot_argn("sheng-aks", &enabled, sizeof(enabled)) && enabled == 1;
}

extern "C" uint32_t native_aks_lock_state(void) {
    return 3;
}

extern "C" void native_keyevents_open_refused(void) {
    IOLog("T733 key state: open refused\n");
}

extern "C" void native_aks_opened(void) {
    IOLog("T733 key state: client opened\n");
}

extern "C" void native_aks_call(uint32_t selector, int32_t result) {
    if (calls++ < 32) {
        IOLog("T733 key state: selector=%u result=%x\n", selector, result);
    }
}

extern "C" void t733_keystore_start(void) {
    unsigned enabled = 0;
    if (attempted || !PE_parse_boot_argn("offline-key-events", &enabled, sizeof(enabled)) ||
        enabled != 1 || !native_aks_state_enabled()) {
        return;
    }
    attempted = true;
    loadSoftwareWrappingKey();

    const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
    bool exists = name && OSMetaClass::getMetaClassWithName(name);
    OSSafeReleaseNULL(name);
    if (exists) {
        IOLog("T733 key state: original service class already present\n");
        return;
    }
    IOService *provider = IOService::getResourceService();
    void *handle = provider ? OSMetaClass::preModLoad(g_kernel_kmod_info.name) : nullptr;
    if (!handle) {
        return;
    }
    keyStoreStaticInit();
    OSReturn status = OSMetaClass::postModLoad(handle);
    if (status) {
        IOLog("T733 key state: class registration failed %x\n", status);
        return;
    }

    IOService *service = native_keyevents_allocate();
    bool attached = service && service->init() && service->attach(provider);
    bool started = attached && service->start(provider);
    if (!started) {
        if (attached) {
            service->detach(provider);
        }
        OSSafeReleaseNULL(service);
        IOLog("T733 key state: service start failed\n");
        return;
    }
    keyService = service;
    IOLog("T733 key state: service ready, software wrapping key=%u\n",
          softwareWrappingKeyReady ? 1U : 0U);
}
