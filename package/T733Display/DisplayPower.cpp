// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "QcomDsi.h"

extern "C" int t733_touch_display_power(bool);
extern "C" int t733_display_i2c_start(void);
extern "C" int t733_display_boost_voltage(void);

namespace {

/* DisplayPower owns command serialization and panel brightness.
 * Firmware retains DSI video scanout and the panel/touch shared supply rails.
 * GPIO98 disables the backlight boost before a zero PWM command; resume enables
 * it and programs the MAX77816 voltage over SE10 before restoring brightness.
 * Sleep leaves that programming intact, while DCS controls panel scanning and
 * the touch worker owns its SPI sleep command and firmware restart.
 */
struct DisplayPower {
    IOLock *lock;
    IOMemoryMap *controllerMap;
    IOMemoryMap *boostMap;
    IOMemoryMap *gccMap;
    IOMemoryMap *displayClockMap;
    uintptr_t controller;
    uintptr_t boost;
    uintptr_t gcc;
    uintptr_t displayClock;
    QcomDsi dsi;
    uint32_t level;
    uint32_t appliedLevel;
    bool powerRequested; // The framebuffer's power request gates brightness-driven wake.
    bool awake;
    bool boostEnabled;
    bool ready;
};

static DisplayPower panel;

static uint32_t readRegister(void *context, uint32_t offset)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(context) + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void writeRegister(void *context, uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(context) + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void delay(void *, unsigned microseconds)
{
    IODelay(microseconds);
}

static IOMemoryMap *map(uint64_t address, uintptr_t &base, size_t bytes = 4096)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(address, bytes);
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

static bool enableDisplayBranch(uintptr_t address, uint32_t offset, bool waitForRunning = true)
{
    void *base = reinterpret_cast<void *>(address);
    uint32_t before = readRegister(base, offset);
    writeRegister(base, offset, before | 1u);
    if (!waitForRunning) {
        uint32_t after = readRegister(base, offset);
        IOLog("T733 display: always-on clock offset=%x before=%x after=%x\n", offset, before, after);
        return (after & 1) != 0;
    }
    uint32_t after = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        after = readRegister(base, offset);
        if (!(after & 0x80000000u) || (after & 0xf0000000u) == 0x20000000u) {
            IOLog("T733 display: clock offset=%x before=%x after=%x\n", offset, before, after);
            return (after & 1) != 0;
        }
        IODelay(10);
    }
    IOLog("T733 display: clock timeout offset=%x before=%x after=%x\n", offset, before, after);
    return false;
}

static bool selectDisplayXo(uint32_t commandOffset)
{
    void *base = reinterpret_cast<void *>(panel.displayClock);
    uint32_t before = readRegister(base, commandOffset + 4);
    uint32_t config = (before & ~0x371fu) | 1u;
    if (before != config) {
        writeRegister(base, commandOffset + 4, config);
        writeRegister(base, commandOffset, readRegister(base, commandOffset) | 1u);
    }
    for (unsigned i = 0; i < 1000; ++i) {
        uint32_t command = readRegister(base, commandOffset);
        if (!(command & 1)) {
            IOLog("T733 display: XO parent rcg=%x cfg-before=%x cfg-after=%x cmd=%x\n",
                  commandOffset, before, readRegister(base, commandOffset + 4), command);
            return true;
        }
        IODelay(10);
    }
    IOLog("T733 display: XO parent update timeout rcg=%x cmd=%x\n",
          commandOffset, readRegister(base, commandOffset));
    return false;
}

static bool enableDisplayRegisterClocks(void)
{
    panel.gccMap = map(0x00127000, panel.gcc);
    panel.displayClockMap = map(0x0af00000, panel.displayClock, 0x6000);
    if (!panel.gccMap || !panel.displayClockMap) {
        return false;
    }
    // The stock GCC/DISPCC probes enable these always-on branches without a
    // halt wait. GCC display AHB can report HALT while HWCG parks an idle bus.
    if (!enableDisplayBranch(panel.gcc, 0x004, false) ||
        !enableDisplayBranch(panel.gcc, 0x01c, false) ||
        !enableDisplayBranch(panel.displayClock, 0x5008, false)) {
        return false;
    }
    void *base = reinterpret_cast<void *>(panel.displayClock);
    IOLog("T733 display: inherited power gdsc=%x mdss-reset=%x ahb-cfg=%x esc-cfg=%x byte-cfg=%x\n",
          readRegister(base, 0x1004), readRegister(base, 0x1000),
          readRegister(base, 0x1174), readRegister(base, 0x10f8), readRegister(base, 0x10dc));
    // AHB and escape each support the 19.2 MHz XO with HID_DIV=1. Selecting it
    // gives both command paths a live parent independently of the GPLL0 vote.
    // The byte/pixel parents and inherited core power state keep scanout running.
    // The selected stock MDSS node binds iface_clk to DISP_CC_MDSS_AHB_CLK
    // (clock 1, offset 0x1050). NON_GDSC_AHB is a separate, unused clock 30.
    if (!selectDisplayXo(0x1170) || !selectDisplayXo(0x10f4) ||
        !enableDisplayBranch(panel.displayClock, 0x1050) ||
        !enableDisplayBranch(panel.displayClock, 0x1030) ||
        !enableDisplayBranch(panel.displayClock, 0x1034) ||
        !enableDisplayBranch(panel.displayClock, 0x1038)) {
        return false;
    }
    return true;
}

static int command(const uint8_t *bytes, size_t count)
{
    int result = qcom_dsi_dcs(&panel.dsi, bytes, count);
    if (result) {
        IOLog("T733 display: DCS=%02x result=%d status=%x interrupt=%x\n",
              bytes[0], result, panel.dsi.last_status, panel.dsi.last_interrupt);
    }
    return result;
}

static int command(uint8_t opcode)
{
    return command(&opcode, 1);
}

static int boost(bool enabled)
{
    if (panel.boostEnabled == enabled) {
        return 0;
    }
    void *base = reinterpret_cast<void *>(panel.boost);
    uint32_t value = readRegister(base, 4);
    writeRegister(base, 4, (value & ~2u) | (enabled ? 2u : 0u));
    panel.boostEnabled = enabled;
    if (enabled) {
        IOSleep(1);
        int result = t733_display_boost_voltage();
        if (result) {
            writeRegister(base, 4, readRegister(base, 4) & ~2u);
            panel.boostEnabled = false;
            return result;
        }
    }
    return 0;
}

static int brightness(uint32_t level)
{
    if (panel.appliedLevel == level && panel.boostEnabled == (level != 0)) {
        return 0;
    }
    int result = boost(level != 0);
    if (result) {
        return result;
    }
    uint32_t pwm = 0;
    if (level > 125) {
        pwm = 1200 + (level - 125) * 2031 / 130;
    } else if (level > 2) {
        pwm = 32 + (level - 2) * 1168 / 123;
    } else if (level) {
        pwm = 32;
    }
    const uint8_t payload[] = {0x51, static_cast<uint8_t>(pwm >> 4), static_cast<uint8_t>(pwm & 15)};
    const uint8_t control[] = {0x53, static_cast<uint8_t>(level > 14 ? 0x2c : 0x24)};
    const bool controlChanged = panel.appliedLevel == UINT32_MAX ||
        (panel.appliedLevel > 14) != (level > 14);
    result = command(payload, sizeof(payload));
    // The panel keeps 0x53 until its dimming mode changes or it resumes.
    if (!result && controlChanged) {
        result = command(control, sizeof(control));
    }
    if (result) {
        boost(false);
        panel.appliedLevel = UINT32_MAX;
    } else {
        panel.appliedLevel = level;
    }
    return result;
}

// The caller holds panel.lock across DSI commands and the touch worker request.
static int setPanelAwake(bool awake)
{
    if (panel.awake == awake) {
        return 0;
    }

    int result = 0;
    if (!awake) {
        boost(false);
        result = t733_touch_display_power(false);
        int displayResult = brightness(0);
        if (!displayResult) {
            displayResult = command(0x28);
        }
        if (!displayResult) {
            displayResult = command(0x10);
            IOSleep(32);
        }
        if (!result) {
            result = displayResult;
        }
        panel.awake = false;
    } else {
        result = command(0x11);
        IOSleep(120);
        if (!result) {
            result = command(0x29);
            IOSleep(20);
        }
        if (!result) {
            result = t733_touch_display_power(true);
        }
        if (!result) {
            panel.appliedLevel = UINT32_MAX;
            result = brightness(panel.level);
        }
        panel.awake = !result;
    }

    IOLog("T733 display: panel awake=%u result=%d\n", awake, result);
    return result;
}

} // namespace

extern "C" bool t733_display_power_start(void)
{
    if (panel.ready) {
        return true;
    }
    unsigned enabled = 0;
    PE_parse_boot_argn("t733-display-power", &enabled, sizeof(enabled));
    if (!enabled) {
        return false;
    }
    panel.lock = IOLockAlloc();
    panel.controllerMap = map(0x0ae94000, panel.controller);
    panel.boostMap = map(0x0f162000, panel.boost);
    if (!panel.lock || !panel.controllerMap || !panel.boostMap) {
        return false;
    }
    panel.dsi = {reinterpret_cast<void *>(panel.controller), readRegister, writeRegister, delay, 0, 0};
    const uint32_t inheritedControl = readRegister(panel.dsi.context, 4);
    if (!enableDisplayRegisterClocks()) {
        IOLog("T733 display: register clocks unavailable inherited-dsi=%x\n", inheritedControl);
        return false;
    }
    const uint32_t control = readRegister(panel.dsi.context, 4);
    IOLog("T733 display: DSI0 physical=%llx version=%x ctrl-before=%x ctrl-after=%x status=%x active-v=%x\n",
          static_cast<uint64_t>(panel.controllerMap->getPhysicalAddress()),
          readRegister(panel.dsi.context, 0), inheritedControl, control,
          readRegister(panel.dsi.context, 8), readRegister(panel.dsi.context, 0x28));
    const uint32_t gpio = readRegister(reinterpret_cast<void *>(panel.boost), 0);
    if ((control & 3) != 3 || (gpio & 0x3c) || !(gpio & 0x200)) {
        IOLog("T733 display: inherited video/boost unavailable dsi=%x gpio98=%x\n", control, gpio);
        return false;
    }
    int result = t733_display_i2c_start();
    if (result) {
        IOLog("T733 display: boost transport unavailable result=%d\n", result);
        return false;
    }
    panel.level = 125;
    panel.appliedLevel = UINT32_MAX;
    panel.powerRequested = true;
    panel.awake = true;
    panel.boostEnabled = (readRegister(reinterpret_cast<void *>(panel.boost), 4) & 2) != 0;
    panel.ready = true;
    IOLog("T733 display: DSI0 FIFO and MAX77816/GPIO98 ready; shared rails retained\n");
    return true;
}

extern "C" IOReturn t733_display_set_brightness(uint32_t value, uint32_t maximum)
{
    if (!panel.ready) {
        return kIOReturnNotReady;
    }
    if (!maximum || value > maximum) {
        return kIOReturnBadArgument;
    }
    const uint32_t level = static_cast<uint32_t>((static_cast<uint64_t>(value) * 255 + maximum / 2) / maximum);
    IOLockLock(panel.lock);
    bool changed = panel.level != level || (panel.awake && panel.appliedLevel != level);
    panel.level = level;
    // CoreBrightness fades to zero when blanking an asserted display. The
    // framebuffer remains powered for rendering, while its panel can sleep.
    const bool awake = panel.powerRequested && level != 0;
    int result = panel.awake != awake ? setPanelAwake(awake) :
        (panel.awake && changed ? brightness(level) : 0);
    IOLockUnlock(panel.lock);
    if (changed || result) {
        IOLog("T733 display: brightness=%u/%u panel=%u result=%d\n", value, maximum, level, result);
    }
    return result ? kIOReturnIOError : kIOReturnSuccess;
}

extern "C" IOReturn t733_display_set_millinits(uint32_t milliNits, uint32_t *platformLevel)
{
    if (milliNits > 500000) {
        milliNits = 500000;
    }
    // The FT8203 stock table pairs levels 2/125/255 with 4/190/500 nits.
    uint32_t level = 0;
    if (milliNits > 190000) {
        level = 125 + (milliNits - 190000) * 130 / 310000;
    } else if (milliNits > 4000) {
        level = 2 + (milliNits - 4000) * 123 / 186000;
    } else if (milliNits) {
        level = 2;
    }
    if (platformLevel) {
        *platformLevel = level;
    }
    return t733_display_set_brightness(level, 255);
}

extern "C" IOReturn t733_display_set_power(bool awake)
{
    if (!panel.ready) {
        return kIOReturnNotReady;
    }
    IOLockLock(panel.lock);
    panel.powerRequested = awake;
    int result = setPanelAwake(awake && panel.level != 0);
    IOLockUnlock(panel.lock);

    return result ? kIOReturnIOError : kIOReturnSuccess;
}

extern "C" void t733_display_dump(void)
{
    IOLog("T733 display: snapshot ready=%u awake=%u brightness=%u applied=%u boost=%u\n",
          panel.ready, panel.awake, panel.level, panel.appliedLevel, panel.boostEnabled);
    if (panel.gcc) {
        void *base = reinterpret_cast<void *>(panel.gcc);
        IOLog("T733 display: GCC DISP_AHB=%x DISP_XO=%x DISP_HF_AXI=%x DISP_SF_AXI=%x\n",
              readRegister(base, 4), readRegister(base, 0x1c),
              readRegister(base, 0x0c), readRegister(base, 0x14));
    }
    if (panel.displayClock) {
        void *base = reinterpret_cast<void *>(panel.displayClock);
        IOLog("T733 display: DISPCC gdsc=%x mdss-reset=%x xo=%x non-gdsc-ahb=%x ahb=%x\n",
              readRegister(base, 0x1004), readRegister(base, 0x1000), readRegister(base, 0x5008),
              readRegister(base, 0x2004), readRegister(base, 0x1050));
        IOLog("T733 display: DISPCC ahb-rcg=%x/%x esc-rcg=%x/%x byte-rcg=%x/%x byte=%x intf=%x esc=%x pixel=%x\n",
              readRegister(base, 0x1170), readRegister(base, 0x1174),
              readRegister(base, 0x10f4), readRegister(base, 0x10f8),
              readRegister(base, 0x10d8), readRegister(base, 0x10dc),
              readRegister(base, 0x1030), readRegister(base, 0x1034),
              readRegister(base, 0x1038), readRegister(base, 0x1010));
    }
    if (panel.controller) {
        void *base = reinterpret_cast<void *>(panel.controller);
        IOLog("T733 display: DSI version=%x ctrl=%x status=%x fifo=%x active-h=%x active-v=%x total=%x dma=%x irq=%x\n",
              readRegister(base, 0), readRegister(base, 4), readRegister(base, 8), readRegister(base, 12),
              readRegister(base, 0x24), readRegister(base, 0x28), readRegister(base, 0x2c),
              readRegister(base, 0x3c), readRegister(base, 0x110));
    }
}
