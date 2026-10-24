// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733TouchHidABI.h"
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSUnserialize.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "Fts8203.h"
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
void t733_touch_hid_static_init(void) __asm__("__GLOBAL__sub_I_T733TouchHid.cpp");
extern kmod_info_t g_kernel_kmod_info;
}

// Each Finger collection owns one FT8203 ID. Its bits are Tip, Confidence and Untouch.
#define T733_FINGER \
    0x09,0x22,0xa1,0x02,0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02, \
    0x09,0x47,0x81,0x02,0x09,0x34,0x81,0x02,0x75,0x05,0x95,0x01,0x81,0x03, \
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x01,0x81,0x02, \
    0x09,0x31,0x81,0x02,0x05,0x0d,0xc0,

static const uint8_t descriptor[] = {
    // The first application collection supplies the service identity before publication.
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x03,
    T733_FINGER T733_FINGER T733_FINGER T733_FINGER T733_FINGER
    T733_FINGER T733_FINGER T733_FINGER T733_FINGER T733_FINGER
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x25,0x0a,0x81,0x02,0xc0
};
#undef T733_FINGER

namespace {

// Original IOHIDEventDriver dispatch slots and fields match NativeHidInput's 21H461 ABI.
enum : uint32_t {
    kDigitizer = 11, kIndex = 0xb0005, kMask = 0xb0007, kTouch = 0xb0009,
    kX = 0xb0000, kY = 0xb0001, kFromEdge = 0x800
};
static uintptr_t originalDispatch;
static unsigned dispatchCount;
static unsigned digitizerCount;
static unsigned lifecycleChanges;
static bool edgeContact[256];
static unsigned edgeX = 48 * 65536 / 1600;
static unsigned edgeY = 48 * 65536 / 2560;

static bool installTouchPersonality()
{
    // The original generic driver receives its touchscreen properties during init.
    static const char xml[] =
        "<array><dict>"
        "<key>IOClass</key><string>IOHIDEventDriver</string>"
        "<key>IOProviderClass</key><string>IOHIDInterface</string>"
        "<key>CFBundleIdentifier</key><string>com.apple.iokit.IOHIDFamily</string>"
        "<key>HIDDefaultBehavior</key><string></string>"
        "<key>Transport</key><string>SPI</string>"
        "<key>IOPropertyMatch</key><dict>"
        "<key>Product</key><string>Samsung SM-T733 FT8203 Touchscreen</string>"
        "</dict>"
        "<key>DeviceUsagePairs</key><array><dict>"
        "<key>DeviceUsagePage</key><integer>13</integer>"
        "<key>DeviceUsage</key><integer>4</integer>"
        "</dict></array>"
        "<key>PrimaryUsagePage</key><integer>13</integer>"
        "<key>PrimaryUsage</key><integer>4</integer>"
        "<key>Built-In</key><true/>"
        "<key>DisplayIntegrated</key><true/>"
        "<key>DigitizerCollectionDispatch</key><true/>"
        "</dict></array>";
    OSObject *object = OSUnserializeXML(xml);
    OSArray *personalities = OSDynamicCast(OSArray, object);
    bool installed = personalities && gIOCatalogue &&
        gIOCatalogue->addDrivers(personalities, false);
    OSSafeReleaseNULL(object);
    printf("T733 touch: touchscreen event personality installed=%u\n", installed);
    return installed;
}

template <typename R, typename... A>
static R eventCall(void *object, unsigned slot, A... args)
{
    uintptr_t *table = *reinterpret_cast<uintptr_t **>(object);
    return reinterpret_cast<R (*)(void *, A...)>(table[slot])(object, args...);
}

static bool markEdge(OSObject *event)
{
    if (eventCall<uint32_t, IOOptionBits>(event, 18, 0) != kDigitizer) {
        return false;
    }
    unsigned id = eventCall<int32_t, uint32_t, IOOptionBits>(event, 23, kIndex, 0);
    if (id >= 256) {
        return false;
    }
    uint32_t mask = eventCall<int32_t, uint32_t, IOOptionBits>(event, 23, kMask, 0);
    bool down = eventCall<int32_t, uint32_t, IOOptionBits>(event, 23, kTouch, 0) != 0;
    if (mask & 2) {
        ++lifecycleChanges;
        if (lifecycleChanges <= 64 || !(lifecycleChanges % 128)) {
            printf("T733 touch: HID lifecycle=%u id=%u touch=%u mask=%x x=%d y=%d\n",
                lifecycleChanges, id, down, mask,
                eventCall<int32_t, uint32_t, IOOptionBits>(event, 25, kX, 0),
                eventCall<int32_t, uint32_t, IOOptionBits>(event, 25, kY, 0));
        }
    }
    if ((mask & 2) && down) {
        int32_t x = eventCall<int32_t, uint32_t, IOOptionBits>(event, 25, kX, 0);
        int32_t y = eventCall<int32_t, uint32_t, IOOptionBits>(event, 25, kY, 0);
        edgeContact[id] = x < (int32_t)edgeX || x > (int32_t)(65536 - edgeX) ||
            y < (int32_t)edgeY || y > (int32_t)(65536 - edgeY);
    }
    bool edge = edgeContact[id];
    if (edge) {
        eventCall<void, uint32_t, int32_t, IOOptionBits>(event, 24, kMask, mask | kFromEdge, 0);
    }
    if ((mask & 2) && !down) {
        edgeContact[id] = false;
    }
    return edge;
}

static void dispatch(IOService *service, OSObject *event, IOOptionBits options)
{
    ++dispatchCount;
    uint32_t type = event ? eventCall<uint32_t, IOOptionBits>(event, 18, 0) : 0;
    if (event && type == kDigitizer) {
        ++digitizerCount;
        OSArray *children = *reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event) + 24);
        bool edge = false;
        unsigned childCount = 0;
        if (children && OSDynamicCast(OSArray, children)) {
            childCount = children->getCount();
            for (unsigned i = 0; i < children->getCount(); ++i) {
                OSObject *child = children->getObject(i);
                if (child && child->metaCast("IOHIDEvent")) {
                    edge |= markEdge(child);
                }
            }
            if (edge) {
                uint32_t mask = eventCall<int32_t, uint32_t, IOOptionBits>(event, 23, kMask, 0);
                eventCall<void, uint32_t, int32_t, IOOptionBits>(event, 24, kMask, mask | kFromEdge, 0);
            }
        } else if (!children) {
            markEdge(event);
        }
        if (digitizerCount <= 3 || !(digitizerCount % 128)) {
            printf("T733 touch: HID collection children=%u touch=%d mask=%x\n",
                childCount,
                eventCall<int32_t, uint32_t, IOOptionBits>(event, 23, kTouch, 0),
                eventCall<int32_t, uint32_t, IOOptionBits>(event, 23, kMask, 0));
        }
    }
    if (dispatchCount <= 3 || !(dispatchCount % 128)) {
        printf("T733 touch: HID dispatch=%u digitizers=%u type=%u\n",
            dispatchCount, digitizerCount, type);
    }
    reinterpret_cast<void (*)(IOService *, OSObject *, IOOptionBits)>(originalDispatch)(service, event, options);
}

static unsigned numberProperty(IORegistryEntry *entry, const char *name)
{
    OSNumber *value = OSDynamicCast(OSNumber, entry->getProperty(name));
    return value ? value->unsigned32BitValue() : 0xffffffffu;
}

static void describeChildren(IORegistryEntry *entry, unsigned depth, unsigned &remaining)
{
    if (!entry || depth > 4 || !remaining) {
        return;
    }
    --remaining;
    printf("T733 touch: HID node depth=%u name=%s class=%s page=%x usage=%x built-in=%u integrated=%u\n",
        depth, entry->getName(), entry->getMetaClass()->getClassName(),
        numberProperty(entry, "PrimaryUsagePage"), numberProperty(entry, "PrimaryUsage"),
        entry->getProperty("Built-In") == kOSBooleanTrue,
        entry->getProperty("DisplayIntegrated") == kOSBooleanTrue);
    OSIterator *iterator = entry->getChildIterator(gIOServicePlane);
    if (!iterator) {
        return;
    }
    while (remaining) {
        OSObject *child = iterator->getNextObject();
        if (!child) {
            break;
        }
        describeChildren(OSDynamicCast(IORegistryEntry, child), depth + 1, remaining);
    }
    iterator->release();
}

static IORegistryEntry *findDriver(IORegistryEntry *parent, unsigned depth)
{
    if (!depth) {
        return nullptr;
    }
    OSIterator *iterator = parent->getChildIterator(gIOServicePlane);
    if (!iterator) {
        return nullptr;
    }
    IORegistryEntry *found = nullptr;
    while (OSObject *child = iterator->getNextObject()) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
        if (!entry) {
            continue;
        }
        found = entry->metaCast("IOHIDEventDriver") ? entry : findDriver(entry, depth - 1);
        if (found) {
            break;
        }
    }
    iterator->release();
    return found;
}

} // namespace

/* T733TouchHid publishes normalized FT8203 contact sets through the original HID parser.
 * Its private work loop serializes reports. Hardware IDs retain their previous coordinates
 * in their fixed report slots through release and inactivity. The original event driver
 * combines the Finger collections into one event containing the active and released IDs.
 */
class T733TouchHid : public IOHIDDevice {
    OSDeclareDefaultStructors(T733TouchHid)
    IOWorkLoop *loop = nullptr;
    bool ready = false;
    unsigned bindingAttempts = 0;
    unsigned reports = 0;
    unsigned transform = 0;
    struct Fts8203Frame previous = {};

    bool bindEvents()
    {
        if (ready) {
            return true;
        }
        IORegistryEntry *driver = findDriver(this, 4);
        if (!driver) {
            ++bindingAttempts;
            if (bindingAttempts == 1 || !(bindingAttempts % 640)) {
                printf("T733 touch: HID awaiting event driver attempts=%u\n", bindingAttempts);
                unsigned remaining = 12;
                describeChildren(this, 0, remaining);
            }
            return false;
        }
        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(256 * sizeof(uintptr_t)));
        if (!copy) {
            return false;
        }
        memcpy(copy, *reinterpret_cast<uintptr_t **>(driver), 256 * sizeof(uintptr_t));
        originalDispatch = copy[198];
        copy[198] = reinterpret_cast<uintptr_t>(&dispatch);
        __asm__ volatile("dmb ish" ::: "memory");
        *reinterpret_cast<uintptr_t **>(driver) = copy;
        ready = true;
        OSDictionary *digitizer = OSDynamicCast(OSDictionary, driver->getProperty("Digitizer"));
        bool collectionDispatch = digitizer &&
            digitizer->getObject("collectionDispatch") == kOSBooleanTrue;
        printf("T733 touch: original IOHIDEventDriver attached collection=%u, edge gestures enabled\n",
            collectionDispatch);
        unsigned remaining = 12;
        describeChildren(this, 0, remaining);
        return true;
    }

    void coordinates(uint16_t rawX, uint16_t rawY, uint16_t &x, uint16_t &y)
    {
        unsigned px = rawX, py = rawY;
        unsigned maxX = 1599, maxY = 2559;
        if (transform & 1) {
            px = rawY;
            py = rawX;
            maxX = 2559;
            maxY = 1599;
        }
        if (transform & 2) {
            px = maxX - px;
        }
        if (transform & 4) {
            py = maxY - py;
        }
        x = (uint16_t)(px * 32767 / maxX);
        y = (uint16_t)(py * 32767 / maxY);
    }

    static IOReturn deliver(OSObject *owner, void *input, void *, void *, void *)
    {
        T733TouchHid *self = OSDynamicCast(T733TouchHid, owner);
        const struct Fts8203Frame *frame = static_cast<const struct Fts8203Frame *>(input);
        if (!self || !frame || !self->bindEvents()) {
            return kIOReturnNotReady;
        }
        uint16_t restarted = frame->down & self->previous.active;
        if (restarted) {
            // A fresh FT DOWN reuses an ID only after the previous contact has ended.
            struct Fts8203Frame ended = self->previous;
            ended.active &= ~restarted;
            ended.down = 0;
            ended.up = restarted;
            IOReturn released = deliver(owner, &ended, nullptr, nullptr, nullptr);
            printf("T733 touch: HID explicit down reused=%x release-result=%x\n", restarted, released);
            if (released != kIOReturnSuccess) {
                return released;
            }
        }
        uint16_t present = frame->active | self->previous.active;
        if (!present) {
            return kIOReturnSuccess;
        }
        uint8_t report[62] = {3};
        unsigned count = 0;
        for (unsigned id = 0; id < FTS8203_CONTACTS; ++id) {
            bool down = (frame->active & (1u << id)) != 0;
            if (down) {
                ++count;
            }
            const struct Fts8203Frame *source = down ? frame : &self->previous;
            uint16_t x, y;
            self->coordinates(source->x[id], source->y[id], x, y);
            uint8_t *finger = report + 1 + 6 * id;
            // Confidence stays set for Finger events, including the final Untouch.
            finger[0] = down ? 3 : 6;
            finger[1] = id;
            finger[2] = x;
            finger[3] = x >> 8;
            finger[4] = y;
            finger[5] = y >> 8;
        }
        report[61] = count;
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        if (!buffer) {
            return kIOReturnNoMemory;
        }
        IOReturn result = self->handleReport(buffer, 0, 0);
        buffer->release();
        ++self->reports;
        if (self->reports <= 3 || frame->active != self->previous.active || !(self->reports % 128)) {
            printf("T733 touch: HID report=%u active=%x contacts=%u slots=10 result=%x dispatch=%u digitizers=%u\n",
                self->reports, frame->active, count, result, dispatchCount, digitizerCount);
        }
        if (result == kIOReturnSuccess) {
            for (unsigned id = 0; id < FTS8203_CONTACTS; ++id) {
                if (frame->active & (1u << id)) {
                    self->previous.x[id] = frame->x[id];
                    self->previous.y[id] = frame->y[id];
                }
            }
            self->previous.active = frame->active;
            self->previous.down = frame->down;
            self->previous.up = frame->up;
        }
        return result;
    }

public:
    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    OSString *newTransportString() const override
    {
        return OSString::withCString("SPI");
    }

    OSString *newProductString() const override
    {
        return OSString::withCString("Samsung SM-T733 FT8203 Touchscreen");
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
        PE_parse_boot_argn("t733-touch-transform", &transform, sizeof(transform));
        transform &= 7;
        edgeX = 48 * 65536 / ((transform & 1) ? 2560 : 1600);
        edgeY = 48 * 65536 / ((transform & 1) ? 1600 : 2560);
        setProperty("HIDDefaultBehavior", true);
        setProperty("Built-In", true);
        setProperty("DisplayIntegrated", true);
        setProperty("DigitizerCollectionDispatch", kOSBooleanTrue);
        if (!IOHIDDevice::start(provider)) {
            return false;
        }
        registerService();
        printf("T733 touch: HID registered touchscreen transform=%u descriptor=%lu\n",
            transform, (unsigned long)sizeof(descriptor));
        return true;
    }

    void free() override
    {
        OSSafeReleaseNULL(loop);
        IOHIDDevice::free();
    }

    IOReturn publish(const struct Fts8203Frame *frame)
    {
        return loop->runAction(deliver, this, const_cast<struct Fts8203Frame *>(frame));
    }
};

OSDefineMetaClassAndStructors(T733TouchHid, IOHIDDevice)
static T733TouchHid *touchDevice;

extern "C" bool t733_touch_hid_start(void)
{
    if (touchDevice) {
        return true;
    }
    int status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if (!status) {
        status = OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
            true, false, kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
    }
    printf("T733 touch: original HID load=%d\n", status);
    if (status) {
        return false;
    }
    if (!installTouchPersonality()) {
        return false;
    }
    const OSSymbol *name = OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    bool compatible = meta && meta->getClassSize() == 208;
    OSSafeReleaseNULL(name);
    if (!compatible) {
        printf("T733 touch: original HID device ABI unavailable\n");
        return false;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        return false;
    }
    t733_touch_hid_static_init();
    if (OSMetaClass::postModLoad(handle) != kOSReturnSuccess) {
        return false;
    }
    OSDictionary *matching = IOService::serviceMatching("IOHIDResource");
    IOService *resource = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!resource) {
        printf("T733 touch: IOHIDResource unavailable\n");
        return false;
    }
    T733TouchHid *device = new T733TouchHid;
    bool attached = device && device->init() && device->attach(resource);
    bool running = attached && device->start(resource);
    if (!running) {
        if (attached) {
            device->detach(resource);
        }
        OSSafeReleaseNULL(device);
    } else {
        touchDevice = device;
    }
    resource->release();
    return running;
}

extern "C" void t733_touch_hid_frame(const struct Fts8203Frame *frame)
{
    static unsigned failures;
    if (touchDevice) {
        IOReturn result = touchDevice->publish(frame);
        if (result != kIOReturnSuccess && result != kIOReturnNotReady && !(failures++ % 128)) {
            printf("T733 touch: HID report result=%x\n", result);
        }
    }
}
