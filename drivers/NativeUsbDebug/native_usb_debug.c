/* Source: https://github.com/xyyyds0106/ios-vermeer-lab
 * Revision: d68e05df683f863f054b0f9c063c54d2b1342b2a
 * Upstream file: xnu/vermeer/vermeer_usb_debug.c
 * All Rights Reserved by the original authors for upstream-derived code.
 * No explicit upstream license grant was found. See NOTICE for provenance.
 */
/* NativeUsbDebug owns one DWC3 device controller and permanent DMA buffers.
 * The board supplies SM7325 register windows and stream identifiers in /chosen.
 * Firmware supplies PHY power and clocks; this driver stops its old DMA before
 * assigning the USB stream a private disabled-translation context, initializes
 * CDC ACM and ECM, and polls events. Console writers only append to the RAM ring.
 */
#if XNU_NATIVE_USB_DEBUG
#include "native_usb_log.h"
#include "native_usb_network.h"
#include "vermeer_usb_protocol.h"
#include <arm/caches_internal.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
extern void armcpu_dump_smp(void) __attribute__((weak_import));
extern void native_ufs_dump_stats(void) __attribute__((weak_import));
extern void t733_cpu_power_dump_stats(void) __attribute__((weak_import));
extern void native_ram_block_dump_stats(void) __attribute__((weak_import));
extern void t733_tlmm_dump_stats(void) __attribute__((weak_import));
extern void t733_pmic_keys_dump(void) __attribute__((weak_import));
extern void t733_display_dump(void) __attribute__((weak_import));
extern void native_usb_net_receive(const void *, unsigned) __attribute__((weak_import));
extern void native_usb_net_set_link(int) __attribute__((weak_import));
extern void native_usb_net_start(void) __attribute__((weak_import));
extern void native_usb_net_dump_stats(void) __attribute__((weak_import));

#define GCTL 0xc110
#define GEVNTADRLO 0xc400
#define GEVNTADRHI 0xc404
#define GEVNTSIZ 0xc408
#define GEVNTCOUNT 0xc40c
#define DCFG 0xc700
#define DCTL 0xc704
#define DEVTEN 0xc708
#define DSTS 0xc70c
#define DALEPENA 0xc720
#define EPBASE(n) (0xc800 + 16 * (n))
#define RUN (1u << 31)
#define RESET (1u << 30)
#define HALTED (1u << 22)
#define KEEP_CONNECT (1u << 19)
#define ACTIVE (1u << 10)
#define FORCE_REMOVE (1u << 11)
#define SET_CONFIG 1
#define SET_RESOURCE 2
#define STALL 4
#define CLEAR_STALL 5
#define START 6
#define END 8
#define START_CONFIG 9
#define T_NORMAL (1u << 4)
#define T_SETUP (2u << 4)
#define T_STATUS2 (3u << 4)
#define T_STATUS3 (4u << 4)
#define T_DATA (5u << 4)
#define HWO 1u
#define TRB_FLAGS (HWO | 2u | (1u << 10) | (1u << 11))
#define EVENTS 0
#define EVENT_SIZE 4096
/* Separate cache lines: hardware never shares a dirty CPU-owned TRB line. */
#define SETUP 0x1300
#define DATA 0x1400
#define NOTIFY 0x1700
#define RX 0x1800
#define TX 0x2000
#define RX_SIZE 512
#define TX_SIZE 2048
#define DMA_SIZE 0x4000
#define NET_RX 0x2800
#define NET_TX 0x3000
#define NET_NOTIFY 0x3800
#define NET_BUFFER_SIZE 2048
#define NET_FRAME_SIZE 1514
#define NET_QUEUE_SIZE 32
#define ENDPOINTS 10
static const unsigned trb_off[ENDPOINTS] = {
    0x1000, 0x1040, 0x1080, 0x10c0, 0x1100, 0x1140, 0x1180, 0x11c0, 0x1200, 0x1240
};
static const char *const usb_strings[] = {
    NULL, "UniXNU", "T733 Kernel Log and Ethernet", "T733DBG001", "027330000001"
};
/* The transmit queue owns copies until the USB worker copies one into DMA. */
static struct {
    uint32_t _Atomic lock, link, generation;
    unsigned head, count;
    struct {
        unsigned length, generation;
        uint8_t frame[NET_FRAME_SIZE];
    } entries[NET_QUEUE_SIZE];
} net_queue;
/* UsbBoard contains the versioned little-endian /chosen property. */
static struct {
    uint32_t version, core, core_size, scratch, phy, eud;
    uint32_t gcc, gcc_size, smmu, smmu_size, stream, protected_stream;
    uint32_t protected_bank, reserved[3];
} board;
static uint32_t _Atomic controller_owner;
static bool ready;
static bool panicking;
static bool tasks_requested;
static char tasks_process_name[64];
static bool cpus_requested;
static bool ufs_requested;
static bool gpio_requested;
static bool dvfs_requested;
static bool display_requested;
static bool network_requested;
static uint64_t panic_deadline;
static struct {
    vm_offset_t regs, dma, smmu;
    uint64_t pa, cursor, tx_bytes, rx_bytes;
    uint32_t pos, events, setups, mps;
    unsigned resource[ENDPOINTS], requested[ENDPOINTS];
    bool busy[ENDPOINTS], failed, configured, dtr, notify, reconnect;
    unsigned net_alt, net_filter, net_notice;
    bool net_reconfigure, net_discard, net_zlp, net_active;
    uint64_t net_tx_frames, net_rx_frames, net_dropped;
    enum { SETUP_PHASE, DATA_PHASE, WAIT2, WAIT3, STATUS_PHASE } phase;
    bool data_in, set_line;
    uint8_t coding[7];
    char command[64];
    unsigned command_n;
    bool command_overflow;
    int sleep_channel;
    uint64_t tx_start, reset_count, recovery_attempts, recovery_deadline, failures;
    uint32_t last_event, failure_event;
    const char *last_failure;
} u;
static void network_link(bool up);

static uint32_t read_reg(vm_offset_t base, unsigned off)
{
    uint32_t v = *(volatile uint32_t *)(base + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
static uint32_t rd(unsigned off)
{
    return read_reg(u.regs, off);
}
static void wr(unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(u.regs + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void sync_dma(unsigned off, unsigned size)
{
    flush_dcache64((addr64_t)(u.pa + off), size, TRUE);
    __asm__ volatile("dsb sy" ::: "memory");
}
static void fail(const char *why)
{
    if (!u.failed) {
        ++u.failures;
        u.last_failure = why;
        u.failure_event = u.last_event;
        if (!panicking) {
            printf("NativeUsbDebug: recovery needed: %s; DMA retained\n", why);
        }
    }
    u.failed = true;
    network_link(false);
}
static int epcmd(unsigned ep, unsigned cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
    if ((u.failed && (cmd & 15) != END) || ep >= ENDPOINTS)
        return -1;
    wr(EPBASE(ep) + 8, p0);
    wr(EPBASE(ep) + 4, p1);
    wr(EPBASE(ep), p2);
    wr(EPBASE(ep) + 12, cmd | ACTIVE);
    for (unsigned n = 0; n < 1000; n++) {
        if (panicking && mach_absolute_time() >= panic_deadline)
            return -1;
        uint32_t v = rd(EPBASE(ep) + 12);
        if (!(v & ACTIVE)) {
            if ((v >> 12) & 15) {
                fail("endpoint command status");
                return -1;
            }
            if ((cmd & 15) == START)
                u.resource[ep] = (v >> 16) & 0x7f;
            return 0;
        }
        delay(1);
    }
    fail("endpoint command timeout");
    return -1;
}
static void transfer(unsigned ep, unsigned buf, unsigned length, unsigned kind)
{
    if (u.failed)
        return;
    if (ep >= ENDPOINTS || u.busy[ep] || buf > DMA_SIZE || length > DMA_SIZE - buf) {
        fail("transfer ownership/bounds");
        return;
    }
    unsigned off = trb_off[ep];
    uint64_t p = u.pa + buf, t = u.pa + off;
    volatile uint32_t *trb = (volatile uint32_t *)(u.dma + off);
    trb[0] = (uint32_t)p;
    trb[1] = (uint32_t)(p >> 32);
    trb[2] = length;
    trb[3] = kind | TRB_FLAGS;
    u.requested[ep] = length;
    if (length)
        sync_dma(buf, length);
    sync_dma(off, 16);
    u.busy[ep] = true;
    (void)epcmd(ep, START, (uint32_t)(t >> 32), (uint32_t)t, 0);
}
static int complete(unsigned ep)
{
    if (ep >= ENDPOINTS || !u.busy[ep]) {
        fail("unexpected completion");
        return -1;
    }
    sync_dma(trb_off[ep], 16);
    volatile uint32_t *t = (volatile uint32_t *)(u.dma + trb_off[ep]);
    unsigned left = t[2] & 0xffffff;
    if ((t[3] & HWO) || (t[2] >> 28) || left > u.requested[ep]) {
        fail("invalid DMA completion");
        return -1;
    }
    u.busy[ep] = false;
    return (int)(u.requested[ep] - left);
}
static void setup_arm(void)
{
    u.phase = SETUP_PHASE;
    transfer(0, SETUP, 8, T_SETUP);
}
static void stall(void)
{
    (void)epcmd(0, STALL, 0, 0, 0);
    setup_arm();
}
static void receive_arm(void)
{
    transfer(2, RX, RX_SIZE, T_NORMAL);
}
static void network_link(bool up)
{
    os_atomic_store(&net_queue.link, up ? 1 : 0, release);
    if (!up) {
        os_atomic_inc(&net_queue.generation, relaxed);
    }
}

int native_usb_net_transmit(const void *frame, unsigned length)
{
    if (!frame || length < 14 || length > NET_FRAME_SIZE) {
        return EMSGSIZE;
    }
    unsigned generation = os_atomic_load(&net_queue.generation, acquire);
    if (!os_atomic_load(&net_queue.link, acquire)) {
        return ENETDOWN;
    }
    if (!os_atomic_cmpxchg(&net_queue.lock, 0, 1, acquire)) {
        return EAGAIN;
    }
    int error = 0;
    if (!os_atomic_load(&net_queue.link, acquire) ||
        generation != os_atomic_load(&net_queue.generation, acquire)) {
        error = ENETDOWN;
    } else if (net_queue.count == NET_QUEUE_SIZE) {
        error = ENOBUFS;
    } else {
        unsigned slot = (net_queue.head + net_queue.count) % NET_QUEUE_SIZE;
        net_queue.entries[slot].generation = generation;
        net_queue.entries[slot].length = length;
        memcpy(net_queue.entries[slot].frame, frame, length);
        ++net_queue.count;
    }
    os_atomic_store(&net_queue.lock, 0, release);
    return error;
}

static void network_alt(unsigned alt)
{
    network_link(false);
    u.net_reconfigure = true;
    for (unsigned ep = 6; ep <= 7; ++ep) {
        if (u.busy[ep] && u.resource[ep]) {
            if (epcmd(ep, END | FORCE_REMOVE | (u.resource[ep] << 16), 0, 0, 0)) {
                return;
            }
            u.busy[ep] = false;
        }
    }
    wr(DALEPENA, rd(DALEPENA) & ~((1u << 6) | (1u << 7)));
    u.net_alt = alt;
    u.net_filter = alt ? 0x0e : 0;
    u.net_active = u.net_zlp = u.net_discard = false;
    u.net_notice = 1;
}

static void stop_transfers(void)
{
    network_link(false);
    u.net_alt = u.net_notice = 0;
    u.net_reconfigure = u.net_active = u.net_zlp = u.net_discard = false;
    if (u.busy[3]) {
        u.cursor = u.tx_start;
    }
    for (unsigned ep = 0; ep < ENDPOINTS; ep++) {
        if (u.busy[ep]) {
            if (u.resource[ep] && !epcmd(ep, END | FORCE_REMOVE | (u.resource[ep] << 16), 0, 0, 0)) {
                u.busy[ep] = false;
            }
        }
    }
    u.configured = false;
    u.dtr = false;
    u.notify = false;
}
static void configure(void)
{
    if (u.configured || u.failed)
        return;
    if (epcmd(0, START_CONFIG | (2u << 16), 0, 0, 0) ||
        epcmd(2, SET_CONFIG, (2u << 1) | (u.mps << 3), (1u << 8) | (2u << 25), 0) ||
        epcmd(2, SET_RESOURCE, 1, 0, 0) ||
        epcmd(3, SET_CONFIG, (2u << 1) | (u.mps << 3) | (1u << 17), (1u << 8) | (3u << 25), 0) ||
        epcmd(3, SET_RESOURCE, 1, 0, 0) ||
        epcmd(5, SET_CONFIG, (3u << 1) | (16u << 3) | (2u << 17),
              (1u << 8) | (8u << 16) | (5u << 25), 0) ||
        epcmd(5, SET_RESOURCE, 1, 0, 0) ||
        epcmd(6, SET_CONFIG, (2u << 1) | (u.mps << 3), (1u << 8) | (6u << 25), 0) ||
        epcmd(6, SET_RESOURCE, 1, 0, 0) ||
        epcmd(7, SET_CONFIG, (2u << 1) | (u.mps << 3) | (3u << 17),
              (1u << 8) | (7u << 25), 0) ||
        epcmd(7, SET_RESOURCE, 1, 0, 0) ||
        epcmd(9, SET_CONFIG, (3u << 1) | (16u << 3) | (4u << 17),
              (1u << 8) | (8u << 16) | (9u << 25), 0) ||
        epcmd(9, SET_RESOURCE, 1, 0, 0))
        return;
    wr(DALEPENA, 3 | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 9));
    u.configured = true;
    receive_arm();
    u.net_notice = 1;
    printf("NativeUsbDebug: CDC ACM/ECM configured, bulk MPS %u\n", u.mps);
}
static unsigned string_desc(unsigned index, uint8_t *data)
{
    if (!index) {
        data[0] = 4;
        data[1] = 3;
        data[2] = 9;
        data[3] = 4;
        return 4;
    }
    if (index >= sizeof(usb_strings) / sizeof(usb_strings[0]))
        return 0;
    unsigned n = 2;
    for (const char *s = usb_strings[index]; *s && n < 126; s++) {
        data[n++] = (uint8_t)*s;
        data[n++] = 0;
    }
    data[0] = (uint8_t)n;
    data[1] = 3;
    return n;
}
static void handle_setup(void)
{
    sync_dma(SETUP, 8);
    const uint8_t *s = (const uint8_t *)(u.dma + SETUP);
    uint8_t *data = (uint8_t *)(u.dma + DATA);
    unsigned value = s[2] | (s[3] << 8), index = s[4] | (s[5] << 8), length = s[6] | (s[7] << 8),
             n = 0;
    int kind = vusb_request(s[0], s[1], value, index, length);
    u.data_in = (s[0] & 0x80) != 0;
    u.set_line = false;
    if (u.setups++ < 40 && !panicking)
        printf("NativeUsbDebug: setup %02x %02x %04x %04x len%u\n", s[0], s[1], value, index,
               length);
    switch (kind) {
    case VUSB_DESC:
        switch (value >> 8) {
        case 1:
            if (value & 255)
                break;
            memcpy(data, vusb_device, sizeof(vusb_device));
            n = sizeof(vusb_device);
            break;
        case 2:
        case 7:
            if (value & 255)
                break;
            memcpy(data, vusb_config, sizeof(vusb_config));
            n = sizeof(vusb_config);
            data[1] = (uint8_t)(value >> 8);
            /* Configuration and other-speed configuration have opposite MPS. */
            for (unsigned i = 0; i < n; i += data[i])
                if (data[i + 1] == 5 && (data[i + 3] & 3) == 2) {
                    unsigned m = (value >> 8) == 7 ? (u.mps == 512 ? 64 : 512) : u.mps;
                    data[i + 4] = (uint8_t)m;
                    data[i + 5] = (uint8_t)(m >> 8);
                }
            break;
        case 3:
            n = string_desc(value & 255, data);
            break;
        case 6:
            if (!(value & 255)) {
                memcpy(data, vusb_qualifier, sizeof(vusb_qualifier));
                n = sizeof(vusb_qualifier);
            }
            break;
        default:
            break;
        }
        if (!n) {
            stall();
            return;
        }
        break;
    case VUSB_ADDRESS:
        wr(DCFG, (rd(DCFG) & ~(127u << 3)) | (value << 3));
        break;
    case VUSB_CONFIG:
        if (!value && u.configured) {
            u.reconnect = true;
            return;
        }
        if (value)
            configure();
        break;
    case VUSB_GET_CONFIG:
        data[0] = u.configured ? 1 : 0;
        n = 1;
        break;
    case VUSB_STATUS:
        data[0] = data[1] = 0;
        n = 2;
        break;
    case VUSB_INTERFACE:
        if (!u.configured) {
            stall();
            return;
        }
        if (index == 3) {
            network_alt(value);
        }
        break;
    case VUSB_GET_INTERFACE:
        if (!u.configured) {
            stall();
            return;
        }
        data[0] = index == 3 ? u.net_alt : 0;
        n = 1;
        break;
    case VUSB_PACKET_FILTER:
        if (!u.configured) {
            stall();
            return;
        }
        u.net_filter = value;
        break;
    case VUSB_SET_LINE:
        u.set_line = true;
        break;
    case VUSB_GET_LINE:
        memcpy(data, u.coding, 7);
        n = 7;
        break;
    case VUSB_CONTROL:
        if (u.configured) {
            bool old = u.dtr;
            u.dtr = (value & 1) != 0;
            u.notify = true;
            if (u.dtr && !old && !panicking)
                printf("NativeUsbDebug: connected; help/status/ping/replay/tasks\n");
        }
        break;
    case VUSB_BREAK:
        break;
    case VUSB_CLEAR_HALT:
        if (!u.configured || ((index == 3 || index == 0x83) && !u.net_alt)) {
            stall();
            return;
        }
        if (index == 3 || index == 0x83) {
            unsigned filter = u.net_filter;
            network_alt(u.net_alt);
            u.net_filter = filter;
            (void)epcmd((index & 15) * 2 + ((index >> 7) & 1), CLEAR_STALL, 0, 0, 0);
            break;
        }
        if (u.busy[(index & 15) * 2 + ((index >> 7) & 1)]) {
            u.reconnect = true;
            return;
        }
        (void)epcmd((index & 15) * 2 + ((index >> 7) & 1), CLEAR_STALL, 0, 0, 0);
        break;
    default:
        stall();
        return;
    }
    if (u.failed)
        return;
    if (!length) {
        u.phase = WAIT2;
        return;
    }
    u.phase = DATA_PHASE;
    if (u.data_in)
        transfer(1, DATA, n < length ? n : length, T_DATA);
    else if (u.set_line)
        transfer(0, DATA, 64, T_DATA);
    else
        stall();
}
static void command_line(void)
{
    u.command[u.command_n] = 0;
    if (u.command_overflow)
        printf("T733 DEBUG: command too long\n");
    else if (!strcmp(u.command, "ping"))
        printf("T733 DEBUG: pong T733DBG001\n");
    else if (!strcmp(u.command, "status")) {
        printf("T733 DEBUG: T733DBG001 events=%u tx=%llu rx=%llu log=%llu dropped=%llu "
               "overwritten=%llu resets=%llu\n",
               u.events, u.tx_bytes, u.rx_bytes, native_usb_log_total(), native_usb_log_dropped(),
               native_usb_log_overwritten(), u.reset_count);
        printf("T733 USB: recoveries=%llu failures=%llu last_event=%08x failure_event=%08x last_failure=%s DCTL=%08x DSTS=%08x\n",
               u.recovery_attempts, u.failures, u.last_event, u.failure_event,
               u.last_failure ? u.last_failure : "none", rd(DCTL), rd(DSTS));
        ufs_requested = true;
        gpio_requested = true;
        dvfs_requested = true;
        network_requested = true;
    } else if (!strcmp(u.command, "network")) {
        network_requested = true;
    } else if (!strcmp(u.command, "ufs")) {
        ufs_requested = true;
    } else if (!strcmp(u.command, "gpio")) {
        gpio_requested = true;
    } else if (!strcmp(u.command, "dvfs")) {
        dvfs_requested = true;
    } else if (!strcmp(u.command, "display")) {
        display_requested = true;
    } else if (!strcmp(u.command, "tasks")) {
        tasks_process_name[0] = '\0';
        tasks_requested = true;
    } else if (!strncmp(u.command, "tasks ", 6) && u.command[6]) {
        strlcpy(tasks_process_name, u.command + 6, sizeof(tasks_process_name));
        tasks_requested = true;
    } else if (!strcmp(u.command, "cpus")) {
        cpus_requested = true;
    } else if (!strcmp(u.command, "replay")) {
        if (u.busy[3]) {
            u.tx_start = 0;
        }
        u.cursor = 0;
        printf("T733 DEBUG: replay retained RAM log\n");
    } else if (u.command_n)
        printf("T733 DEBUG: commands help status ping replay tasks [process-name] cpus ufs gpio dvfs display network\n");
    u.command_n = 0;
    u.command_overflow = false;
}
static void receive_done(unsigned length)
{
    sync_dma(RX, RX_SIZE);
    u.rx_bytes += length;
    const uint8_t *p = (const uint8_t *)(u.dma + RX);
    for (unsigned i = 0; i < length; i++) {
        if (p[i] == '\r' || p[i] == '\n')
            command_line();
        else if (p[i] >= 32 && p[i] <= 126) {
            if (u.command_n < sizeof(u.command) - 1)
                u.command[u.command_n++] = (char)p[i];
            else
                u.command_overflow = true;
        }
    }
    receive_arm();
}
static void transmit(void)
{
    if (!u.configured || !u.dtr || u.failed)
        return;
    if (u.notify && !u.busy[5]) {
        const uint8_t msg[10] = {0xa1, 0x20, 0, 0, 0, 0, 2, 0, 3, 0};
        memcpy((void *)(u.dma + NOTIFY), msg, sizeof(msg));
        u.notify = false;
        transfer(5, NOTIFY, sizeof(msg), T_NORMAL);
    }
    if (u.busy[3])
        return;
    u.tx_start = u.cursor;
    unsigned n = native_usb_log_copy(&u.cursor, (char *)(u.dma + TX), TX_SIZE);
    /* Serial stream must have a short packet, with no padding/invented bytes. */
    if (n && !(n % u.mps)) {
        n--;
        u.cursor--;
    }
    if (n) {
        transfer(3, TX, n, T_NORMAL);
        u.tx_bytes += n;
    }
}
static bool network_accept(const uint8_t *frame)
{
    const uint8_t host[6] = {2, 0x73, 0x30, 0, 0, 1};
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    if (u.net_filter & 1) {
        return true;
    }
    if (!memcmp(frame, broadcast, 6)) {
        return (u.net_filter & 8) != 0;
    }
    if (frame[0] & 1) {
        return (u.net_filter & 2) != 0;
    }
    return (u.net_filter & 4) && !memcmp(frame, host, 6);
}

static void network_receive(unsigned length)
{
    sync_dma(NET_RX, NET_BUFFER_SIZE);
    /* A short packet, including a ZLP, terminates exactly one ECM frame.
     * Oversized transfers are discarded through their terminating packet. */
    if (length == NET_BUFFER_SIZE) {
        u.net_discard = true;
    } else {
        if (!u.net_discard && length >= 14 && length <= NET_FRAME_SIZE && native_usb_net_receive) {
            native_usb_net_receive((const void *)(u.dma + NET_RX), length);
            ++u.net_rx_frames;
        } else if (length || u.net_discard) {
            ++u.net_dropped;
        }
        u.net_discard = false;
    }
}

static void network_poll(void)
{
    if (panicking || u.failed || u.reconnect || !u.configured) {
        return;
    }
    if (u.net_reconfigure) {
        /* END completion events must be drained before these TRBs are reused. */
        if (rd(GEVNTCOUNT) & 0xffff) {
            return;
        }
        u.net_reconfigure = false;
        if (u.net_alt) {
            if (epcmd(6, SET_CONFIG, (2u << 1) | (u.mps << 3),
                      (1u << 8) | (6u << 25), 0) ||
                epcmd(7, SET_CONFIG, (2u << 1) | (u.mps << 3) | (3u << 17),
                      (1u << 8) | (7u << 25), 0)) {
                return;
            }
            wr(DALEPENA, rd(DALEPENA) | (1u << 6) | (1u << 7));
            network_link(native_usb_net_receive != NULL);
        }
    }
    if (u.net_notice && !u.busy[9]) {
        uint8_t msg[16] = {0xa1, 0, 0, 0, 2, 0, 0, 0};
        unsigned size = 8;
        if (u.net_notice == 1) {
            msg[2] = os_atomic_load(&net_queue.link, acquire) ? 1 : 0;
            u.net_notice = msg[2] ? 2 : 0;
        } else {
            msg[1] = 0x2a;
            msg[6] = 8;
            unsigned rate = u.mps == 512 ? 480000000 : 12000000;
            for (unsigned i = 0; i < 4; ++i) {
                msg[8 + i] = msg[12 + i] = (uint8_t)(rate >> (8 * i));
            }
            size = 16;
            u.net_notice = 0;
        }
        memcpy((void *)(u.dma + NET_NOTIFY), msg, size);
        transfer(9, NET_NOTIFY, size, T_NORMAL);
    }
    if (!u.net_alt) {
        return;
    }
    if (!u.busy[6]) {
        transfer(6, NET_RX, NET_BUFFER_SIZE, T_NORMAL);
    }
    if (u.busy[7]) {
        return;
    }
    if (u.net_active) {
        if (u.net_zlp) {
            u.net_zlp = false;
            transfer(7, NET_TX, 0, T_NORMAL);
            return;
        }
        ++u.net_tx_frames;
        u.net_active = false;
    }
    if (!os_atomic_cmpxchg(&net_queue.lock, 0, 1, acquire)) {
        return;
    }
    unsigned length = 0;
    while (net_queue.count) {
        unsigned slot = net_queue.head;
        net_queue.head = (slot + 1) % NET_QUEUE_SIZE;
        --net_queue.count;
        if (net_queue.entries[slot].generation != os_atomic_load(&net_queue.generation, relaxed) ||
            !network_accept(net_queue.entries[slot].frame)) {
            ++u.net_dropped;
            continue;
        }
        length = net_queue.entries[slot].length;
        memcpy((void *)(u.dma + NET_TX), net_queue.entries[slot].frame, length);
        break;
    }
    os_atomic_store(&net_queue.lock, 0, release);
    if (length) {
        u.net_active = true;
        u.net_zlp = (length % u.mps) == 0;
        transfer(7, NET_TX, length, T_NORMAL);
    }
}

static void event(uint32_t e)
{
    u.last_event = e;
    if (e & 1) {
        if ((e & 0xfe) != 0) {
            fail("unexpected device event format");
            return;
        }
        unsigned type = (e >> 8) & 15;
        if (type == 1) {
            if (u.configured || u.phase != SETUP_PHASE) {
                u.reconnect = true;
                return;
            }
            wr(DCFG, rd(DCFG) & ~(127u << 3));
        } else if (type == 2) {
            unsigned speed = rd(DSTS) & 7;
            if (speed != 0 && speed != 1 && speed != 3) {
                fail("unexpected non-USB2 speed");
                return;
            }
            u.mps = speed == 0 ? 512 : 64;
            (void)epcmd(0, SET_CONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10), 0);
            (void)epcmd(1, SET_CONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10) | (1u << 25),
                        0);
            if (!panicking)
                printf("NativeUsbDebug: connected speed=%u\n", speed);
        } else if (!type)
            u.reconnect = true;
        else if (type == 9 || type == 11)
            fail("controller/event overflow");
        return;
    }
    unsigned ep = (e >> 1) & 31, type = (e >> 6) & 15, status = (e >> 12) & 15;
    if (type == 1) {
        if (ep >= ENDPOINTS || !u.busy[ep] ||
            (u.net_reconfigure && (ep == 6 || ep == 7)))
            return;
        int n = complete(ep);
        if (n < 0)
            return;
        if (status & 1) {
            if (ep == 3) {
                u.cursor = u.tx_start;
            }
            fail("USB transfer bus error");
            return;
        }
        if (ep == 7 && (unsigned)n != u.requested[ep]) {
            fail("ECM incomplete transmit");
            return;
        }
        if (ep == 2) {
            if (!panicking)
                receive_done((unsigned)n);
            return;
        }
        if (ep == 6) {
            if (!panicking) {
                network_receive((unsigned)n);
            }
            return;
        }
        if (ep > 1)
            return;
        switch (u.phase) {
        case SETUP_PHASE:
            if (ep != 0 || n != 8)
                fail("setup length");
            else
                handle_setup();
            break;
        case DATA_PHASE:
            if (u.set_line) {
                if (ep != 0 || n != 7) {
                    fail("line coding length");
                    break;
                }
                sync_dma(DATA, 64);
                memcpy(u.coding, (void *)(u.dma + DATA), 7);
            }
            u.phase = WAIT3;
            break;
        case STATUS_PHASE:
            setup_arm();
            break;
        default:
            fail("control completion order");
            break;
        }
    } else if (ep <= 1 && type == 3 && status == 2 && (u.phase == WAIT2 || u.phase == WAIT3)) {
        unsigned direction = u.phase == WAIT2 ? 1 : (u.data_in ? 0 : 1);
        if (ep != direction) {
            fail("control status direction");
            return;
        }
        unsigned kind = u.phase == WAIT3 ? T_STATUS3 : T_STATUS2;
        u.phase = STATUS_PHASE;
        transfer(ep, DATA, 0, kind);
    }
}
static void write_reg(vm_offset_t base, unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(base + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* The USB stream receives a private identity context after DWC3 has halted.
 * Exact-match ownership is required. Other stream entries and global SMMU
 * registers are retained, including the board's protected storage context.
 */
static bool configure_stream(void)
{
    unsigned count = read_reg(u.smmu, 0x20) & 255;
    uint32_t id1 = read_reg(u.smmu, 0x24);
    unsigned banks = id1 & 255;
    unsigned pages = 1U << (((id1 >> 28) & 7) + 1);
    unsigned slot = 0xffffffffU, free_slot = 0xffffffffU, matches = 0;
    bool used[128] = {false};
    if (!count || count > 128 || banks < 2 || banks > 128 || (id1 >> 31) ||
        (pages + banks) * 4096 > board.smmu_size || !(read_reg(u.smmu, 0x28) & (1U << 12))) {
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        uint32_t smr = read_reg(u.smmu, 0x800 + 4 * i);
        uint32_t route = read_reg(u.smmu, 0xc00 + 4 * i);
        if (!(smr & 0x80000000U)) {
            free_slot = i;
            continue;
        }
        unsigned mask = (smr >> 16) & 0x7fff;
        if (!((smr ^ board.stream) & ~mask & 0xffff)) {
            printf("NativeUsbDebug: SID=%x slot=%u smr=%08x s2cr=%08x\n", board.stream, i, smr,
                   route);
            if (mask || ++matches > 1) {
                return false;
            }
            slot = i;
        }
        if (((route >> 16) & 3) == 0 && (route & 255) < banks) {
            used[route & 255] = true;
        }
    }
    if (slot == 0xffffffffU) {
        slot = free_slot;
    }
    if (slot == 0xffffffffU) {
        return false;
    }
    if (matches && ((read_reg(u.smmu, 0xc00 + 4 * slot) >> 16) & 3) == 1) {
        return true;
    }

    unsigned bank = banks;
    for (unsigned i = 1; i < banks; ++i) {
        if (i != board.protected_bank && !used[i] && !(read_reg(u.smmu, (pages + i) * 4096) & 1)) {
            bank = i;
            break;
        }
    }
    if (bank == banks) {
        return false;
    }
    unsigned cb = (pages + bank) * 4096;
    write_reg(u.smmu, cb, 0);
    write_reg(u.smmu, 0x1800 + 4 * bank, 1);
    write_reg(u.smmu, 0x1000 + 4 * bank, 0x10000);
    if ((read_reg(u.smmu, cb) & 1) || !(read_reg(u.smmu, 0x1800 + 4 * bank) & 1)) {
        return false;
    }
    write_reg(u.smmu, 0xc00 + 4 * slot, bank);
    write_reg(u.smmu, 0x800 + 4 * slot, 0x80000000U | board.stream);
    printf("NativeUsbDebug: SID=%x private identity CB=%u DMA=%llx bytes=%x\n", board.stream, bank,
           u.pa, DMA_SIZE);
    return read_reg(u.smmu, 0xc00 + 4 * slot) == bank;
}

static bool stop_controller(void)
{
    wr(DEVTEN, 0);
    /* DWC3 must retire active endpoints and their command events before halt.
     * END does not release DMA for reuse; start_controller also completes reset. */
    stop_transfers();
    wr(DCTL, rd(DCTL) & ~(RUN | KEEP_CONNECT));
    for (unsigned i = 0; i < 500; ++i) {
        unsigned pending = rd(GEVNTCOUNT) & 0xffff;
        if (pending) {
            wr(GEVNTCOUNT, pending);
        }
        if (rd(DSTS) & HALTED) {
            return true;
        }
        delay(1000);
    }
    return (rd(DSTS) & HALTED) != 0;
}

static bool start_controller(void)
{
    if (!stop_controller()) {
        /* The SM7325 gadget driver also uses device core reset after halt times
         * out. DMA remains untouched until reset completes and HALTED is set. */
        fail("controller halt timeout; resetting core");
    }
    wr(DCTL, (rd(DCTL) & ~(RUN | KEEP_CONNECT)) | RESET);
    for (unsigned i = 0; i < 500 && (rd(DCTL) & RESET); ++i) {
        delay(1000);
    }
    if ((rd(DCTL) & RESET) || !(rd(DSTS) & HALTED)) {
        fail("controller reset timeout");
        return false;
    }
    /* The board's DWC3 driver requires a 50 ms PHY synchronization interval. */
    delay(50000);
    /* The reset completes before any memory referenced by old TRBs is reused. */
    u.failed = false;
    memset((void *)u.dma, 0, DMA_SIZE);
    memset(u.busy, 0, sizeof(u.busy));
    memset(u.resource, 0, sizeof(u.resource));
    u.pos = 0;
    u.mps = 512;
    u.configured = u.dtr = u.notify = u.reconnect = false;
    u.command_n = 0;
    u.command_overflow = false;
    sync_dma(0, DMA_SIZE);
    wr(GEVNTADRLO, (uint32_t)u.pa);
    wr(GEVNTADRHI, (uint32_t)(u.pa >> 32));
    wr(GEVNTSIZ, (1U << 31) | EVENT_SIZE);
    wr(GEVNTCOUNT, rd(GEVNTCOUNT) & 0xffff);
    wr(DCFG, rd(DCFG) & ~(7U | (127U << 3)));
    if (epcmd(0, START_CONFIG, 0, 0, 0) ||
        epcmd(0, SET_CONFIG, 64U << 3, (1U << 8) | (1U << 10), 0) ||
        epcmd(0, SET_RESOURCE, 1, 0, 0) ||
        epcmd(1, SET_CONFIG, 64U << 3, (1U << 8) | (1U << 10) | (1U << 25), 0) ||
        epcmd(1, SET_RESOURCE, 1, 0, 0)) {
        return false;
    }
    wr(DALEPENA, 3);
    setup_arm();
    if (u.failed) {
        return false;
    }
    wr(DEVTEN, 7 | (1U << 9) | (1U << 11));
    wr(DCTL, rd(DCTL) | RUN);
    ++u.reset_count;
    return true;
}

static void poll_events(void)
{
    unsigned count = rd(GEVNTCOUNT) & 0xffff;
    if (count > EVENT_SIZE || (count & 3)) {
        fail("event ring size/alignment");
        return;
    }
    if (panicking && count > 64) {
        count = 64;
    }
    if (count) {
        sync_dma(EVENTS, EVENT_SIZE);
        unsigned consumed = 0;
        while (consumed < count && !u.failed && !u.reconnect) {
            uint32_t e = *(volatile uint32_t *)(u.dma + u.pos);
            u.pos = (u.pos + 4) % EVENT_SIZE;
            consumed += 4;
            ++u.events;
            event(e);
        }
        wr(GEVNTCOUNT, consumed);
    }
    if (!u.reconnect) {
        transmit();
        network_poll();
    }
}

static void usb_thread(void *arg __unused, wait_result_t result __unused)
{
    unsigned reported_generation = 0;
    bool reported_link = false;
    if (native_usb_net_start) {
        native_usb_net_start();
    }
    for (;;) {
        if (os_atomic_cmpxchg(&controller_owner, 0, 1, acquire)) {
            if (!u.failed && !u.reconnect) {
                poll_events();
            }
            if ((u.reconnect || u.failed) && !panicking && mach_absolute_time() >= u.recovery_deadline) {
                ++u.recovery_attempts;
                if (start_controller()) {
                    clock_interval_to_deadline(250, NSEC_PER_MSEC, &u.recovery_deadline);
                    printf("NativeUsbDebug: controller restarted attempt=%llu; waiting for host enumeration\n",
                           u.recovery_attempts);
                } else {
                    clock_interval_to_deadline(1, NSEC_PER_SEC, &u.recovery_deadline);
                }
            }
            os_atomic_store(&controller_owner, 0, release);
        }
        if (!panicking && native_usb_net_set_link) {
            unsigned generation = os_atomic_load(&net_queue.generation, acquire);
            bool link = os_atomic_load(&net_queue.link, acquire) != 0;
            if (generation != reported_generation && reported_link) {
                native_usb_net_set_link(0);
                reported_link = false;
            }
            if (link != reported_link) {
                native_usb_net_set_link(link);
                reported_link = link;
            }
            reported_generation = generation;
        }
        if (network_requested && !panicking) {
            network_requested = false;
            printf("T733 ECM: alt=%u filter=%x tx=%llu rx=%llu dropped=%llu\n",
                   u.net_alt, u.net_filter, u.net_tx_frames, u.net_rx_frames, u.net_dropped);
            if (native_usb_net_dump_stats) {
                native_usb_net_dump_stats();
            }
        }
        if (tasks_requested && !panicking) {
            tasks_requested = false;
            native_usb_tasks_snapshot(tasks_process_name);
        }
        if (cpus_requested && !panicking) {
            cpus_requested = false;
            if (armcpu_dump_smp) {
                armcpu_dump_smp();
            } else {
                printf("T733 CPUS: ArmCpu diagnostics unavailable\n");
            }
        }
        if (ufs_requested && !panicking) {
            ufs_requested = false;
            if (native_ufs_dump_stats) {
                native_ufs_dump_stats();
            } else {
                printf("T733 UFS: NativeUfs diagnostics unavailable\n");
            }
            if (native_ram_block_dump_stats) {
                native_ram_block_dump_stats();
            }
        }
        if (gpio_requested && !panicking) {
            gpio_requested = false;
            if (t733_tlmm_dump_stats) {
                t733_tlmm_dump_stats();
            } else {
                printf("T733 GPIO: TLMM diagnostics unavailable\n");
            }
            if (t733_pmic_keys_dump) {
                t733_pmic_keys_dump();
            }
        }
        if (dvfs_requested && !panicking) {
            dvfs_requested = false;
            if (t733_cpu_power_dump_stats) {
                t733_cpu_power_dump_stats();
            } else {
                printf("T733 DVFS: CPU power driver unavailable\n");
            }
        }
        if (display_requested && !panicking) {
            display_requested = false;
            if (t733_display_dump) {
                t733_display_dump();
            }
        }
        uint64_t deadline;
        clock_interval_to_deadline(1, NSEC_PER_MSEC, &deadline);
        (void)assert_wait_deadline(&u.sleep_channel, THREAD_UNINT, deadline);
        thread_block(THREAD_CONTINUE_NULL);
    }
}

void native_usb_debug_panic(const char *message, unsigned long caller)
{
    extern void native_boot_log_panic(const char *, unsigned long) __attribute__((weak_import));
    if (native_boot_log_panic) {
        native_boot_log_panic(message, caller);
    }
    char line[640];
    int length = snprintf(line, sizeof(line), "\nT733 PANIC caller=%lx: %s\n", caller,
                          message ? message : "(no message)");
    native_usb_log_append(line, length < (int)sizeof(line) ? length : (int)sizeof(line) - 1);
    if (!ready || !os_atomic_cmpxchg(&controller_owner, 0, 1, acquire)) {
        return;
    }
    panicking = true;
    /* The latest retained bytes include the panic even when boot output has
     * accumulated faster than the host consumes it. No scheduling is needed. */
    if (!u.busy[3]) {
        uint64_t total = native_usb_log_total();
        if (total > u.cursor + 16384) {
            u.cursor = total - 16384;
        }
    }
    clock_interval_to_deadline(150, NSEC_PER_MSEC, &panic_deadline);
    while (mach_absolute_time() < panic_deadline && !u.failed && !u.reconnect) {
        poll_events();
        if (u.cursor >= native_usb_log_total() && !u.busy[3]) {
            break;
        }
        delay(10);
    }
    panicking = false;
    os_atomic_store(&controller_owner, 0, release);
}

void native_usb_debug_start(void)
{
    static bool attempted;
    int enabled = 0;
    PE_parse_boot_argn("native-usb-debug", &enabled, sizeof(enabled));
    if (enabled != 1 || attempted) {
        return;
    }
    attempted = true;
    DTEntry chosen;
    const void *value = NULL;
    unsigned size = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "native-usb-debug-config", &value, &size) != kSuccess ||
        size != sizeof(board)) {
        printf("NativeUsbDebug: missing board configuration\n");
        return;
    }
    memcpy(&board, value, sizeof(board));
    if (board.version != 1 || board.core_size < 0xe000 || board.core_size > 0x100000 ||
        board.gcc_size != 0x1000 || board.smmu_size > 0x100000 || board.smmu_size < 0x10000 ||
        board.stream > 0x7fff || board.stream == board.protected_stream) {
        printf("NativeUsbDebug: invalid board configuration\n");
        return;
    }
    vm_offset_t gcc = ml_io_map(board.gcc, board.gcc_size);
    vm_offset_t phy = ml_io_map(board.phy, 0x114);
    vm_offset_t eud = ml_io_map(board.eud, 4);
    if (!gcc || !phy || !eud) {
        return;
    }
    uint32_t power = read_reg(gcc, 4);
    const unsigned gates[] = {0x10, 0x1c, 0x18, 0x7c, 0x80};
    printf("NativeUsbDebug: GCC power=%08x PHY common=%08x EUD=%08x\n", power, read_reg(phy, 0x54),
           read_reg(eud, 0));
    if (!(power & (1U << 31))) {
        printf("NativeUsbDebug: firmware USB power unavailable\n");
        return;
    }
    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); ++i) {
        uint32_t gate = read_reg(gcc, gates[i]);
        printf("NativeUsbDebug: GCC +%x=%08x\n", gates[i], gate);
        if (gate & (1U << 31)) {
            printf("NativeUsbDebug: firmware USB clock halted\n");
            return;
        }
    }
    u.regs = ml_io_map(board.core, board.core_size);
    u.smmu = ml_io_map(board.smmu, board.smmu_size);
    vm_offset_t qs = ml_io_map(board.scratch, 0x100);
    if (!u.regs || !u.smmu || !qs) {
        return;
    }
    uint32_t id = rd(0xc120);
    unsigned width = rd(0xc140) >> 24;
    printf("NativeUsbDebug: id=%08x width=%u GCTL=%08x DCTL=%08x\n", id, width, rd(GCTL), rd(DCTL));
    if (((id >> 16) != 0x5533 && (id >> 16) != 0x3331) || ((rd(GCTL) >> 12) & 3) != 2) {
        return;
    }
    unsigned hardware = rd(0xc14c);
    unsigned inputs = (hardware >> 18) & 31;
    unsigned endpoints = (hardware >> 12) & 63;
    if (inputs < 5 || endpoints < inputs + 4) {
        printf("NativeUsbDebug: insufficient composite endpoint resources in=%u total=%u\n",
               inputs, endpoints);
        return;
    }
    unsigned dma_bits = width < 44 ? width : 44;
    const unsigned smmu_widths[] = {32, 36, 40, 42, 44, 48};
    uint32_t id2 = read_reg(u.smmu, 0x28);
    for (unsigned shift = 0; shift <= 8; shift += 4) {
        unsigned encoding = (id2 >> shift) & 15;
        if (encoding >= sizeof(smmu_widths) / sizeof(smmu_widths[0])) {
            return;
        }
        if (smmu_widths[encoding] < dma_bits) {
            dma_bits = smmu_widths[encoding];
        }
    }
    if (dma_bits < 32) {
        return;
    }
    ppnum_t max_page = (ppnum_t)(((1ULL << dma_bits) - 1) >> PAGE_SHIFT);
    if (kmem_alloc_contig(kernel_map, &u.dma, DMA_SIZE, DMA_SIZE - 1, max_page,
                          (DMA_SIZE - 1) >> PAGE_SHIFT,
                          KMA_DATA | KMA_ZERO | KMA_PERMANENT,
                          VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
        return;
    }
    ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)u.dma);
    u.pa = ptoa_64(pn);
    if (!vusb_dma_allowed(u.pa, DMA_SIZE, width) || !stop_controller() || !configure_stream()) {
        printf("NativeUsbDebug: DMA/controller/stream handoff failed\n");
        return;
    }
    write_reg(qs, 0x10, read_reg(qs, 0x10) | (1U << 20) | (1U << 28));
    write_reg(qs, 0x30, read_reg(qs, 0x30) | (1U << 24));
    u.coding[1] = 0xc2;
    u.coding[2] = 1;
    u.coding[6] = 8;
    if (!start_controller()) {
        return;
    }
    ready = true;
    thread_t thread;
    if (kernel_thread_start(usb_thread, NULL, &thread) != KERN_SUCCESS) {
        ready = false;
        (void)stop_controller();
        return;
    }
    thread_deallocate(thread);
    printf("NativeUsbDebug: running T733DBG001 DMA=%llx; waiting for host DTR\n", u.pa);
}
#endif
