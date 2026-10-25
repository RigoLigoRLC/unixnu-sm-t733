// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "T733Tlmm.h"
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "GeniSpi.h"
#include "Fts8203.h"
extern const uint8_t t733_touch_firmware[];
extern const uint32_t t733_touch_firmware_size;
bool t733_touch_hid_start(void);
void t733_touch_hid_frame(const struct Fts8203Frame *);
}

namespace {

/* TouchBoard retains the MMIO maps and protocol buffers for the single touch worker. */
struct TouchBoard {
    IOMemoryMap *maps[4];
    uintptr_t gcc;
    uintptr_t tlmm;
    uintptr_t se;
    uintptr_t wrapper;
    struct geni_spi spi;
    struct Fts8203 chip;
};

static TouchBoard board;
static uint32_t started;
static uint32_t displayAwake = 1;
static uint32_t touchReady;
static uint32_t powerRequest;
static uint32_t powerCompleted;
static int powerResult;

static uint32_t read32(uintptr_t base, uint32_t offset)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void write32(uintptr_t base, uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool map(unsigned slot, uint64_t physical, size_t bytes, uintptr_t &address)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(physical, bytes);
    if (!memory) {
        return false;
    }
    board.maps[slot] = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!board.maps[slot]) {
        return false;
    }
    address = static_cast<uintptr_t>(board.maps[slot]->getVirtualAddress());
    return address != 0;
}

static void delay(void *, unsigned microseconds)
{
    if (microseconds >= 2000) {
        IOSleep(microseconds / 1000);
        microseconds %= 1000;
    }
    if (microseconds) {
        IODelay(microseconds);
    }
}

static bool waitMask(uintptr_t base, uint32_t offset, uint32_t mask, uint32_t expected)
{
    for (unsigned i = 0; i < 1000; ++i) {
        if ((read32(base, offset) & mask) == expected) {
            return true;
        }
        IODelay(10);
    }
    printf("T733 touch: clock timeout reg=%x value=%x mask=%x expected=%x\n",
        offset, read32(base, offset), mask, expected);
    return false;
}

static bool enableClock(uint32_t voteRegister, uint32_t voteMask, uint32_t branchRegister, bool hasHwcg)
{
    uint32_t before = read32(board.gcc, voteRegister);
    write32(board.gcc, voteRegister, before | voteMask);
    uint32_t after = read32(board.gcc, voteRegister);
    uint32_t branch = read32(board.gcc, branchRegister);
    bool hwcg = hasHwcg && (branch & (1u << 1));
    printf("T733 touch: clock vote-reg=%x mask=%x before=%x after=%x branch-reg=%x cbcr=%x hwcg=%u\n",
        voteRegister, voteMask, before, after, branchRegister, branch, hwcg);
    if ((after & voteMask) != voteMask) {
        printf("T733 touch: clock vote rejected reg=%x mask=%x value=%x\n",
            voteRegister, voteMask, after);
        return false;
    }

    // Yupik AHB branches in HWCG mode stop while idle even with an active APSS vote.
    if (hwcg) {
        printf("T733 touch: clock branch-reg=%x accepted hardware gating\n", branchRegister);
        return true;
    }

    // clk_branch2 accepts CLK_OFF clear or the NOC FSM ON state after voting.
    for (unsigned i = 0; i < 200; ++i) {
        branch = read32(board.gcc, branchRegister);
        if (!(branch & (1u << 31)) || (branch & 0xf0000000u) == 0x20000000u) {
            printf("T733 touch: clock branch-reg=%x running cbcr=%x polls=%u\n",
                branchRegister, branch, i);
            return true;
        }
        IODelay(1);
    }
    printf("T733 touch: clock branch timeout reg=%x cbcr=%x vote-reg=%x vote=%x\n",
        branchRegister, branch, voteRegister, read32(board.gcc, voteRegister));
    return false;
}

static void pin(unsigned gpio, unsigned function, unsigned pull, unsigned drive, bool output, bool high)
{
    uint32_t offset = gpio * 0x1000;
    uint32_t config = read32(board.tlmm, offset);
    if (output) {
        uint32_t value = read32(board.tlmm, offset + 4);
        write32(board.tlmm, offset + 4, (value & ~2u) | (high ? 2u : 0u));
    }
    config &= ~0x3ffu;
    config |= pull | (function << 2) | (drive << 6) | (output ? 1u << 9 : 0u);
    write32(board.tlmm, offset, config);
}

static void gpioOutput(unsigned gpio, bool high)
{
    uint32_t offset = gpio * 0x1000 + 4;
    uint32_t value = read32(board.tlmm, offset);
    write32(board.tlmm, offset, (value & ~2u) | (high ? 2u : 0u));
}

static uint32_t seRead(void *context, uint32_t offset)
{
    return read32(reinterpret_cast<uintptr_t>(context), offset);
}

static void seWrite(void *context, uint32_t offset, uint32_t value)
{
    write32(reinterpret_cast<uintptr_t>(context), offset, value);
}

static void reset(void *, int high)
{
    gpioOutput(50, high != 0);
}

static void stage(void *, const char *name, unsigned value, int result)
{
    printf("T733 touch: %s value=%x result=%d\n", name, value, result);
}

static int transfer(void *, const uint8_t *tx, uint8_t *rx, size_t length)
{
    gpioOutput(59, false);
    int result = geni_spi_transfer(&board.spi, tx, rx, length, 0);
    gpioOutput(59, true);
    if (result) {
        printf("T733 touch: SPI result=%d bytes=%lu irq=%x state=%x tx=%x rx=%x tx-rem=%u rx-rem=%u\n",
            result, (unsigned long)length, board.spi.snap_irq, board.spi.snap_status,
            board.spi.snap_txfs, board.spi.snap_rxfs,
            board.spi.snap_tx_remaining, board.spi.snap_rx_remaining);
        printf("T733 touch: SPI recovery state=%x tx=%x rx=%x\n",
            board.spi.recovery_status, board.spi.recovery_txfs, board.spi.recovery_rxfs);
    }
    return result;
}

static int configure(void)
{
    if (!map(0, 0x00100000, 0x60000, board.gcc) ||
        !map(1, 0x0f100000, 0x100000, board.tlmm) ||
        !map(2, 0x00a98000, 0x4000, board.se) ||
        !map(3, 0x00ac0000, 0x2000, board.wrapper)) {
        return -12;
    }

    // Wrapper 1 AHB votes precede SE14 clock and register access.
    if (!enableClock(0x52008, 1u << 20, 0x18004, true) ||
        !enableClock(0x52008, 1u << 21, 0x18008, true)) {
        return -110;
    }

    printf("T733 touch: GCC rcg=%x cfg=%x branch=%x vote=%x\n",
        read32(board.gcc, 0x18730), read32(board.gcc, 0x18734),
        read32(board.gcc, 0x1872c), read32(board.gcc, 0x52000));
    // SRC_SEL=0 selects the 19.2 MHz XO; HID_DIV=1 divides by one, M/N mode is off.
    uint32_t config = read32(board.gcc, 0x18734);
    write32(board.gcc, 0x18734, (config & ~0x371fu) | 1u);
    write32(board.gcc, 0x18730, read32(board.gcc, 0x18730) | 1u);
    if (!waitMask(board.gcc, 0x18730, 1u, 0)) {
        return -110;
    }
    if (!enableClock(0x52000, 1u << 13, 0x1872c, false)) {
        return -110;
    }

    pin(56, 1, 0, 2, false, false);
    pin(57, 1, 0, 2, false, false);
    pin(58, 1, 0, 2, false, false);
    pin(59, 0, 0, 2, true, true);
    pin(50, 0, 3, 3, true, true);
    pin(80, 0, 0, 2, false, false);
    pin(81, 0, 3, 3, false, false);
    printf("T733 touch: panel-select=%u irq=%u reset=%x cs=%x\n",
        read32(board.tlmm, 80 * 0x1000 + 4) & 1,
        read32(board.tlmm, 81 * 0x1000 + 4) & 1,
        read32(board.tlmm, 50 * 0x1000 + 4), read32(board.tlmm, 59 * 0x1000 + 4));

    board.spi.se = {reinterpret_cast<void *>(board.se), seRead, seWrite, delay};
    board.spi.wrapper = {reinterpret_cast<void *>(board.wrapper), seRead, seWrite, delay};
    board.spi.poll_only = 1;
    board.spi.poll_limit = 30000;
    board.spi.allow_fifo_disabled = 1;
    int result = geni_spi_read_info(&board.spi);
    printf("T733 touch: GENI result=%d fw=%x proto=%u if-disable=%x fifo=%ux%u rx-depth=%u status=%x wrapper=%x\n",
        result, board.spi.info.fw_revision, board.spi.info.protocol, board.spi.info.if_disable,
        board.spi.info.tx_fifo_depth, board.spi.info.fifo_width_bits,
        board.spi.info.rx_fifo_depth, board.spi.info.geni_status, board.spi.info.qup_hw_version);
    if (result) {
        return result;
    }
    if (board.spi.info.geni_status & GENI_STATUS_M_ACTIVE) {
        return -16;
    }
    result = geni_spi_init(&board.spi);
    if (!result) {
        result = geni_spi_setup(&board.spi, 0, 1);
    }
    if (!result) {
        result = geni_spi_set_clock(&board.spi, 4800000, 19200000, 0);
    }
    board.chip.io = {nullptr, transfer, delay, reset, stage};
    return result;
}

static void reportFrameFailure(int result, unsigned failures)
{
    const uint8_t *head = board.chip.frame_head;
    printf("T733 touch: frame result=%d count=%u wire-status=%x irq=%u head=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
        result, failures, board.chip.last_status,
        read32(board.tlmm, 81 * 0x1000 + 4) & 1,
        head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7],
        head[8], head[9], head[10], head[11], head[12], head[13], head[14], head[15]);
}

static uint32_t touchPending;

static void touchInterrupt(unsigned, void *)
{
    __atomic_store_n(&touchPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(&touchPending));
}

static void armTouchAfterFirmware(void)
{
    // A low line gets one initial read; a completed pulse retains its raw latch.
    t733_tlmm_irq_mask(81);
    if (t733_tlmm_level(81) == 0) {
        t733_tlmm_irq_ack(81);
        touchInterrupt(81, nullptr);
    } else {
        t733_tlmm_irq_rearm(81);
    }
}

static void waitForTouchEvent(void)
{
    while (!__atomic_exchange_n(&touchPending, 0u, __ATOMIC_ACQ_REL)) {
        assert_wait(reinterpret_cast<event_t>(&touchPending), THREAD_UNINT);
        if (__atomic_load_n(&touchPending, __ATOMIC_ACQUIRE)) {
            clear_wait(current_thread(), THREAD_AWAKENED);
        }
        thread_block(THREAD_CONTINUE_NULL);
    }
}

static void worker(void *, wait_result_t)
{
    OSDictionary *matching = IOService::resourceMatching("IOBSD");
    IOService *bsd = matching ? IOService::waitForMatchingService(matching, 60000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!bsd) {
        printf("T733 touch: IOBSD unavailable\n");
        thread_terminate(current_thread());
        return;
    }
    bsd->release();

    int result = configure();
    if (!result) {
        result = fts8203_start(&board.chip, t733_touch_firmware, t733_touch_firmware_size);
    }
    if (result || !t733_touch_hid_start()) {
        printf("T733 touch: initialization stopped result=%d\n", result);
        thread_terminate(current_thread());
        return;
    }
    result = t733_tlmm_irq_register(81, T733_TLMM_EDGE_FALLING, touchInterrupt, nullptr);
    if (result) {
        printf("T733 touch: GPIO81 TLMM registration failed=%d\n", result);
        thread_terminate(current_thread());
        return;
    }
    armTouchAfterFirmware();
    __atomic_store_n(&touchReady, 1u, __ATOMIC_RELEASE);
    touchInterrupt(81, nullptr);
    printf("T733 touch: FT8203 scanning 1600x2560 SPI=4800000 PIO IRQ81 worker\n");

    unsigned failures = 0;
    unsigned recoveries = 0;
    unsigned frames = 0;
    uint16_t lastActive = 0;
    struct Fts8203Frame frame = {};
    bool awake = true;
    for (;;) {
        waitForTouchEvent();
        const uint32_t request = __atomic_load_n(&powerRequest, __ATOMIC_ACQUIRE);
        const bool wanted = __atomic_load_n(&displayAwake, __ATOMIC_ACQUIRE) != 0;
        if (wanted != awake) {
            t733_tlmm_irq_mask(81);
            frame = {};
            frame.up = lastActive;
            t733_touch_hid_frame(&frame);
            lastActive = 0;
            if (wanted) {
                result = fts8203_start(&board.chip, t733_touch_firmware, t733_touch_firmware_size);
            } else {
                result = fts8203_sleep(&board.chip);
            }
            awake = wanted && !result;
            failures = 0;
            recoveries = 0;
            __atomic_store_n(&powerResult, result, __ATOMIC_RELEASE);
            printf("T733 touch: display awake=%u result=%d\n", awake, result);
            if (awake) {
                armTouchAfterFirmware();
            }
        }
        __atomic_store_n(&powerCompleted, request, __ATOMIC_RELEASE);
        if (!awake) {
            continue;
        }
        // The dispatcher acknowledged this frame; raw capture remains enabled during SPI.
        result = fts8203_frame(&board.chip, &frame);
        if (!result) {
            failures = 0;
            t733_touch_hid_frame(&frame);
            if (frame.active != lastActive || frame.down || frame.up || (frame.active && !(frames % 128))) {
                printf("T733 touch: frame=%u contacts=%x down=%x up=%x x0=%u y0=%u delivered=%u irq=%u head=%02x%02x%02x%02x%02x%02x%02x%02x\n", frames,
                    frame.active, frame.down, frame.up, frame.x[0], frame.y[0], t733_tlmm_irq_count(81),
                    read32(board.tlmm, 81 * 0x1000 + 4) & 1,
                    board.chip.frame_head[1], board.chip.frame_head[2], board.chip.frame_head[3],
                    board.chip.frame_head[4], board.chip.frame_head[5], board.chip.frame_head[6],
                    board.chip.frame_head[7], board.chip.frame_head[8]);
            }
            lastActive = frame.active;
            ++frames;
        } else {
            ++failures;
            if (failures == 1) {
                frame = {};
                t733_touch_hid_frame(&frame);
                lastActive = 0;
            }
            if (failures == 1 || !(failures % 640)) {
                reportFrameFailure(result, failures);
                int state = fts8203_state(&board.chip, result != FTS8203_FRAME_EMPTY);
                printf("T733 touch: frame state=%d action=%s\n", state,
                    state == 1 ? "reload-confirmed-rom" : "wait-for-frame");
                if (state == 1) {
                    if (++recoveries > 3) {
                        printf("T733 touch: confirmed ROM recovery limit reached\n");
                        t733_tlmm_irq_mask(81);
                        awake = false;
                        continue;
                    }
                    result = fts8203_start(&board.chip, t733_touch_firmware, t733_touch_firmware_size);
                    printf("T733 touch: recovery=%u result=%d\n", recoveries, result);
                    if (result) {
                        t733_tlmm_irq_mask(81);
                        awake = false;
                        continue;
                    }
                    failures = 0;
                    armTouchAfterFirmware();
                    continue;
                }
            }
        }
        t733_tlmm_irq_rearm(81);
    }
}

} // namespace

extern "C" int t733_touch_display_power(bool awake)
{
    __atomic_store_n(&displayAwake, awake ? 1u : 0u, __ATOMIC_RELEASE);
    const uint32_t request = __atomic_add_fetch(&powerRequest, 1u, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&touchReady, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    touchInterrupt(81, nullptr);
    for (unsigned i = 0; i < 600; ++i) {
        if (__atomic_load_n(&powerCompleted, __ATOMIC_ACQUIRE) == request) {
            return __atomic_load_n(&powerResult, __ATOMIC_ACQUIRE);
        }
        IOSleep(5);
    }
    return -60;
}

extern "C" void t733_touch_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("t733-touch", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }
    thread_t thread = THREAD_NULL;
    kern_return_t result = kernel_thread_start(worker, nullptr, &thread);
    if (result == KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        printf("T733 touch: worker start failed=%d\n", result);
    }
}
