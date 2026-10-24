// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>

/* IOHIDDevice describes the 208-byte, 207-slot original IOHIDFamily device used by NativeHidInput. */
class IOHIDDevice : public IOService {
    OSDeclareAbstractStructors(IOHIDDevice)
    unsigned char originalPrivate[0xd0 - sizeof(IOService)];

public:
    using IOService::setProperty;
    IOReturn Dispatch(IORPC) override;
    void free() override;
    bool init(OSDictionary *properties = nullptr) override;
    bool setProperty(const OSSymbol *, OSObject *) override;
    bool willTerminate(IOService *, IOOptionBits) override;
    IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *) override;
    IOReturn updateReport(IOReportChannelList *, IOReportUpdateAction, void *, void *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    bool handleOpen(IOService *, IOOptionBits, void *) override;
    void handleClose(IOService *, IOOptionBits) override;
    bool handleIsOpen(const IOService *) const override;
    bool matchPropertyTable(OSDictionary *, SInt32 *) override;
    IOReturn message(UInt32, IOService *, void *) override;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;

#define T733_HID_SLOT(number) virtual void opaque##number();
    T733_HID_SLOT(168)
    T733_HID_SLOT(169)
    T733_HID_SLOT(170)
    virtual OSString *newTransportString() const;
    T733_HID_SLOT(172)
    virtual OSString *newProductString() const;
    T733_HID_SLOT(174)
    T733_HID_SLOT(175)
    T733_HID_SLOT(176)
    T733_HID_SLOT(177)
    T733_HID_SLOT(178)
    T733_HID_SLOT(179)
    virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const = 0;
    virtual IOReturn handleReport(IOMemoryDescriptor *, unsigned, IOOptionBits);
    T733_HID_SLOT(182)
    T733_HID_SLOT(183)
    T733_HID_SLOT(184)
    T733_HID_SLOT(185)
    T733_HID_SLOT(186)
    T733_HID_SLOT(187)
    T733_HID_SLOT(188)
    T733_HID_SLOT(189)
    T733_HID_SLOT(190)
    T733_HID_SLOT(191)
    T733_HID_SLOT(192)
    T733_HID_SLOT(193)
    T733_HID_SLOT(194)
    T733_HID_SLOT(195)
    T733_HID_SLOT(196)
    T733_HID_SLOT(197)
    T733_HID_SLOT(198)
    T733_HID_SLOT(199)
    T733_HID_SLOT(200)
    T733_HID_SLOT(201)
    T733_HID_SLOT(202)
    T733_HID_SLOT(203)
    T733_HID_SLOT(204)
    T733_HID_SLOT(205)
    T733_HID_SLOT(206)
#undef T733_HID_SLOT
};

static_assert(sizeof(IOHIDDevice) == 0xd0, "Original IOHIDDevice instance size");
