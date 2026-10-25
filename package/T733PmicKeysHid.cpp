// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733PmicKeys.h"
#include "T733TouchHidABI.h"
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSUnserialize.h>
#include <mach/kmod.h>

extern "C" {
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void t733_keys_hid_static_init(void) __asm__("__GLOBAL__sub_I_T733PmicKeysHid.cpp");
extern kmod_info_t g_kernel_kmod_info;
}

namespace {
static const uint8_t descriptor[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03,
    0x09, 0x30, 0x09, 0xe9, 0x09, 0xea, 0x95, 0x03, 0x81, 0x02,
    0x95, 0x04, 0x81, 0x03, 0xc0,
};

static bool installPersonality()
{
    static const char xml[] =
        "<array><dict>"
        "<key>IOClass</key><string>IOHIDEventDriver</string>"
        "<key>IOProviderClass</key><string>IOHIDInterface</string>"
        "<key>CFBundleIdentifier</key><string>com.apple.iokit.IOHIDFamily</string>"
        "<key>HIDDefaultBehavior</key><string></string>"
        "<key>Transport</key><string>SPMI</string>"
        "<key>IOPropertyMatch</key><dict>"
        "<key>Product</key><string>Samsung SM-T733 PMIC Keys</string>"
        "</dict>"
        "<key>DeviceUsagePairs</key><array><dict>"
        "<key>DeviceUsagePage</key><integer>12</integer>"
        "<key>DeviceUsage</key><integer>1</integer>"
        "</dict></array>"
        "<key>PrimaryUsagePage</key><integer>12</integer>"
        "<key>PrimaryUsage</key><integer>1</integer>"
        "<key>Built-In</key><true/>"
        "</dict></array>";
    OSObject *object = OSUnserializeXML(xml);
    OSArray *personalities = OSDynamicCast(OSArray, object);
    bool installed = personalities && gIOCatalogue && gIOCatalogue->addDrivers(personalities, false);
    OSSafeReleaseNULL(object);
    return installed;
}

static bool eventDriverAttached(IORegistryEntry *entry, unsigned depth)
{
    if (entry->metaCast("IOHIDEventDriver")) {
        return true;
    }
    if (!depth) {
        return false;
    }
    OSIterator *iterator = entry->getChildIterator(gIOServicePlane);
    if (!iterator) {
        return false;
    }
    bool found = false;
    while (OSObject *object = iterator->getNextObject()) {
        IORegistryEntry *child = OSDynamicCast(IORegistryEntry, object);
        if (child && eventDriverAttached(child, depth - 1)) {
            found = true;
            break;
        }
    }
    iterator->release();
    return found;
}
} // namespace

/* T733PmicKeysHid exposes the board's three buttons as an internal Consumer
 * Control device. The PMIC worker supplies the complete pressed mask, and its
 * private work loop passes report 2 to the original IOHIDFamily parser. The
 * original IOHIDEventDriver publishes power and volume events to user space.
 */
class T733PmicKeysHid : public IOHIDDevice {
    OSDeclareDefaultStructors(T733PmicKeysHid)
    IOWorkLoop *loop = nullptr;

    static IOReturn deliver(OSObject *owner, void *input, void *, void *, void *)
    {
        T733PmicKeysHid *self = OSDynamicCast(T733PmicKeysHid, owner);
        if (!self || !eventDriverAttached(self, 4)) {
            return kIOReturnNotReady;
        }
        uint8_t bytes[] = {2, static_cast<uint8_t>(*static_cast<uint32_t *>(input) & 14)};
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(bytes, sizeof(bytes), kIODirectionNone);
        if (!buffer) {
            return kIOReturnNoMemory;
        }
        IOReturn result = self->handleReport(buffer, 0, 0);
        buffer->release();
        return result;
    }

public:
    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    OSString *newTransportString() const override
    {
        return OSString::withCString("SPMI");
    }

    OSString *newProductString() const override
    {
        return OSString::withCString("Samsung SM-T733 PMIC Keys");
    }

    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override
    {
        if (!out) {
            return kIOReturnBadArgument;
        }
        *out = IOBufferMemoryDescriptor::withBytes(descriptor, sizeof(descriptor), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    bool start(IOService *provider) override
    {
        loop = IOWorkLoop::workLoop();
        if (!loop) {
            return false;
        }
        setProperty("Built-In", true);
        setProperty("HIDDefaultBehavior", true);
        if (!IOHIDDevice::start(provider)) {
            return false;
        }
        registerService();
        printf("T733 keys: HID Consumer Control registered report=2 usages=30/e9/ea\n");
        return true;
    }

    void free() override
    {
        OSSafeReleaseNULL(loop);
        IOHIDDevice::free();
    }

    IOReturn publish(uint32_t mask)
    {
        return loop->runAction(deliver, this, &mask);
    }
};

OSDefineMetaClassAndStructors(T733PmicKeysHid, IOHIDDevice)
static T733PmicKeysHid *keysDevice;

extern "C" bool t733_pmic_keys_hid_start(void)
{
    int status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if (!status) {
        status = OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
            true, false, kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
    }
    if (status || !installPersonality()) {
        return false;
    }
    const OSSymbol *name = OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    bool compatible = meta && meta->getClassSize() == 208;
    OSSafeReleaseNULL(name);
    if (!compatible) {
        return false;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        return false;
    }
    t733_keys_hid_static_init();
    if (OSMetaClass::postModLoad(handle) != kOSReturnSuccess) {
        return false;
    }
    OSDictionary *matching = IOService::serviceMatching("IOHIDResource");
    IOService *resource = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!resource) {
        return false;
    }
    T733PmicKeysHid *device = new T733PmicKeysHid;
    bool attached = device && device->init() && device->attach(resource);
    bool running = attached && device->start(resource);
    if (!running) {
        if (attached) {
            device->detach(resource);
        }
        OSSafeReleaseNULL(device);
    } else {
        keysDevice = device;
    }
    resource->release();
    if (running) {
        for (unsigned tries = 0; tries < 200; ++tries) {
            if (eventDriverAttached(device, 4)) {
                printf("T733 keys: original IOHIDEventDriver attached\n");
                return true;
            }
            IOSleep(50);
        }
        printf("T733 keys: IOHIDEventDriver still pending after registration\n");
    }
    return running;
}

extern "C" int t733_pmic_keys_hid_report(void *, uint32_t pressed)
{
    return keysDevice ? static_cast<int>(keysDevice->publish(pressed)) : -6;
}
