// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/sockio.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_ether.h>
#include <net/ethernet.h>
#include <net/kpi_interface.h>
#include <net/init.h>
#include <net/bpf.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include "native_usb_network.h"

#define NET_RX_SLOTS 64u
#define NET_FRAME_MAX 1514u

/* UsbRxFrame stores a frame copied by the single USB receive worker. The
 * network worker is the sole consumer. Release/acquire indices protect frame
 * storage, and the link generation rejects frames from an earlier connection.
 */
struct UsbRxFrame {
    unsigned length;
    unsigned link_generation;
    unsigned char bytes[NET_FRAME_MAX];
};

static struct UsbRxFrame rx_ring[NET_RX_SLOTS];
static unsigned rx_head;
static unsigned rx_tail;
static unsigned link_generation;
static unsigned start_requested;
static unsigned worker_started;
static unsigned interface_ready;
static unsigned interface_detached;
static unsigned wake_event;
static ifnet_t network_interface;
static const unsigned char device_mac[ETHER_ADDR_LEN] = { 2, 0x73, 0x30, 0, 0, 2 };
static const unsigned char broadcast_mac[ETHER_ADDR_LEN] = { 255, 255, 255, 255, 255, 255 };
static uint64_t rx_frames;
static uint64_t rx_bytes;
static uint64_t rx_dropped;
static uint64_t rx_errors;
static uint64_t tx_frames;
static uint64_t tx_bytes;
static uint64_t tx_errors;
static uint64_t link_changes;

static void wake_worker(void)
{
    __atomic_store_n(&wake_event, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&wake_event);
}

void native_usb_net_receive(const void *bytes, unsigned length)
{
    unsigned generation = __atomic_load_n(&link_generation, __ATOMIC_ACQUIRE);
    if (!bytes || length < ETHER_HDR_LEN || length > NET_FRAME_MAX || !(generation & 1)
        || !__atomic_load_n(&interface_ready, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&rx_dropped, 1, __ATOMIC_RELAXED);
        return;
    }

    unsigned head = __atomic_load_n(&rx_head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(&rx_tail, __ATOMIC_ACQUIRE);
    if ((unsigned)(head - tail) >= NET_RX_SLOTS) {
        __atomic_fetch_add(&rx_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    struct UsbRxFrame *frame = &rx_ring[head % NET_RX_SLOTS];
    frame->length = length;
    frame->link_generation = generation;
    memcpy(frame->bytes, bytes, length);
    __atomic_store_n(&rx_head, head + 1, __ATOMIC_RELEASE);
    wake_worker();
}

void native_usb_net_set_link(int up)
{
    unsigned old = __atomic_load_n(&link_generation, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((old & 1) == (unsigned)(up != 0)) {
            return;
        }
        unsigned next = ((old + 2) & ~1u) | (up != 0);
        if (__atomic_compare_exchange_n(&link_generation, &old, next, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    wake_worker();
}

static errno_t network_output(ifnet_t interface, mbuf_t packets)
{
    errno_t first_error = 0;
    while (packets) {
        mbuf_t packet = packets;
        packets = mbuf_nextpkt(packet);
        mbuf_setnextpkt(packet, NULL);
        size_t length = mbuf_pkthdr_len(packet);
        unsigned char bytes[NET_FRAME_MAX];
        errno_t error = 0;
        if (!__atomic_load_n(&interface_ready, __ATOMIC_ACQUIRE)
            || !(__atomic_load_n(&link_generation, __ATOMIC_ACQUIRE) & 1)
            || !(ifnet_flags(interface) & IFF_UP)) {
            error = ENETDOWN;
        } else if (length < ETHER_HDR_LEN || length > sizeof(bytes)) {
            error = EMSGSIZE;
        } else {
            error = mbuf_copydata(packet, 0, length, bytes);
            if (!error) {
                error = native_usb_net_transmit(bytes, (unsigned)length);
            }
        }
        if (!error) {
            bpf_tap_out(interface, DLT_EN10MB, packet, NULL, 0);
            ifnet_stat_increment_out(interface, 1, (uint32_t)length, 0);
            __atomic_fetch_add(&tx_frames, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&tx_bytes, length, __ATOMIC_RELAXED);
        } else {
            ifnet_stat_increment_out(interface, 0, 0, 1);
            __atomic_fetch_add(&tx_errors, 1, __ATOMIC_RELAXED);
            if (!first_error) {
                first_error = error;
            }
        }
        mbuf_freem(packet);
    }
    return first_error;
}

static errno_t network_ioctl(ifnet_t interface, u_long command, void *data)
{
    struct ifreq *request = data;
    switch (command) {
    case SIOCSIFADDR:
        return ifnet_set_flags(interface, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
    case SIOCSIFFLAGS:
        return ifnet_set_flags(interface, (ifnet_flags(interface) & IFF_UP)
            ? IFF_RUNNING : 0, IFF_RUNNING);
    case SIOCSIFMTU:
        if (request->ifr_mtu < 576 || request->ifr_mtu > ETHERMTU) {
            return EINVAL;
        }
        return ifnet_set_mtu(interface, (uint32_t)request->ifr_mtu);
    case SIOCGIFDEVMTU:
        request->ifr_devmtu.ifdm_current = (int)ifnet_mtu(interface);
        request->ifr_devmtu.ifdm_min = 576;
        request->ifr_devmtu.ifdm_max = ETHERMTU;
        return 0;
    case SIOCADDMULTI:
    case SIOCDELMULTI:
        return 0;
    case SIOCGIFMEDIA32:
    case SIOCGIFMEDIA64: {
        struct ifmediareq *media = data;
        user_addr_t list = command == SIOCGIFMEDIA64
            ? ((struct ifmediareq64 *)data)->ifmu_ulist
            : CAST_USER_ADDR_T(((struct ifmediareq32 *)data)->ifmu_ulist);
        int available = IFM_ETHER | IFM_AUTO;
        int count = media->ifm_count;
        media->ifm_current = available;
        media->ifm_active = available;
        media->ifm_mask = 0;
        media->ifm_status = IFM_AVALID
            | ((__atomic_load_n(&link_generation, __ATOMIC_ACQUIRE) & 1) ? IFM_ACTIVE : 0);
        media->ifm_count = 1;
        if (list && count > 0) {
            return copyout(&available, list, sizeof(available));
        }
        return 0;
    }
    case SIOCSIFMEDIA:
        return request->ifr_media == (IFM_ETHER | IFM_AUTO) ? 0 : EINVAL;
    default:
        return EOPNOTSUPP;
    }
}

static void network_detached(ifnet_t interface)
{
    (void)interface;
    __atomic_store_n(&interface_ready, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&interface_detached, 1u, __ATOMIC_RELEASE);
    wake_worker();
}

static void post_link(ifnet_t interface, unsigned generation)
{
    struct LinkEventData {
        uint32_t family;
        uint32_t unit;
        char name[IFNAMSIZ];
    };
    _Alignas(struct kern_event_msg) char message[sizeof(struct kern_event_msg)
        + sizeof(struct LinkEventData)] = { 0 };
    struct kern_event_msg *header = (struct kern_event_msg *)message;
    struct LinkEventData *event = (struct LinkEventData *)(header + 1);
    header->total_size = sizeof(message);
    header->vendor_code = KEV_VENDOR_APPLE;
    header->kev_class = KEV_NETWORK_CLASS;
    header->kev_subclass = KEV_DL_SUBCLASS;
    header->event_code = (generation & 1) ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
    event->family = ifnet_family(interface);
    event->unit = (uint32_t)ifnet_unit(interface);
    strlcpy(event->name, ifnet_name(interface), sizeof(event->name));
    ifnet_event(interface, header);
    __atomic_fetch_add(&link_changes, 1, __ATOMIC_RELAXED);
    printf("T733 USB net: unet0 link=%u generation=%u\n", generation & 1, generation >> 1);
}

static errno_t attach_interface(void)
{
    struct ifnet_init_eparams parameters = { 0 };
    parameters.ver = IFNET_INIT_CURRENT_VERSION;
    parameters.len = sizeof(parameters);
    parameters.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
    parameters.uniqueid = device_mac;
    parameters.uniqueid_len = sizeof(device_mac);
    parameters.name = "unet";
    parameters.unit = 0;
    parameters.family = IFNET_FAMILY_ETHERNET;
    parameters.type = IFT_ETHER;
    parameters.output = network_output;
    parameters.demux = ether_demux;
    parameters.add_proto = ether_add_proto;
    parameters.del_proto = ether_del_proto;
    parameters.check_multi = ether_check_multi;
    parameters.framer_extended = ether_frameout_extended;
    parameters.ioctl = network_ioctl;
    parameters.detach = network_detached;
    parameters.broadcast_addr = broadcast_mac;
    parameters.broadcast_len = sizeof(broadcast_mac);
    ifnet_t interface = NULL;
    errno_t error = ifnet_allocate_extended(&parameters, &interface);
    if (error) {
        return error;
    }
    ifnet_set_addrlen(interface, ETHER_ADDR_LEN);
    ifnet_set_hdrlen(interface, ETHER_HDR_LEN);
    ifnet_set_mtu(interface, ETHERMTU);
    ifnet_set_flags(interface, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
    ifnet_set_offload(interface, 0);
    struct sockaddr_dl address = { 0 };
    address.sdl_len = sizeof(address);
    address.sdl_family = AF_LINK;
    address.sdl_type = IFT_ETHER;
    address.sdl_alen = ETHER_ADDR_LEN;
    memcpy(LLADDR(&address), device_mac, sizeof(device_mac));
    error = ifnet_attach(interface, &address);
    if (error) {
        ifnet_release(interface);
        return error;
    }
    network_interface = interface;
    bpfattach(interface, DLT_EN10MB, ETHER_HDR_LEN);
    __atomic_store_n(&interface_ready, 1u, __ATOMIC_RELEASE);
    return 0;
}

static void input_frame(ifnet_t interface, const struct UsbRxFrame *frame)
{
    if (frame->link_generation != __atomic_load_n(&link_generation, __ATOMIC_ACQUIRE)
        || !(ifnet_flags(interface) & IFF_UP)) {
        __atomic_fetch_add(&rx_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    mbuf_t packet = NULL;
    unsigned chunks = 1;
    errno_t error = mbuf_allocpacket(MBUF_DONTWAIT, frame->length, &chunks, &packet);
    if (error) {
        __atomic_fetch_add(&rx_errors, 1, __ATOMIC_RELAXED);
        return;
    }
    error = mbuf_copyback(packet, 0, frame->length, frame->bytes, MBUF_DONTWAIT);
    if (error) {
        mbuf_freem(packet);
        __atomic_fetch_add(&rx_errors, 1, __ATOMIC_RELAXED);
        return;
    }
    void *header = mbuf_data(packet);
    mbuf_pkthdr_setrcvif(packet, interface);
    mbuf_pkthdr_setheader(packet, header);
    mbuf_pkthdr_setlen(packet, frame->length - ETHER_HDR_LEN);
    error = mbuf_setdata(packet, (char *)header + ETHER_HDR_LEN, frame->length - ETHER_HDR_LEN);
    if (error) {
        mbuf_freem(packet);
        __atomic_fetch_add(&rx_errors, 1, __ATOMIC_RELAXED);
        return;
    }
    bpf_tap_in(interface, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
    struct ifnet_stat_increment_param stats = { 0 };
    stats.packets_in = 1;
    stats.bytes_in = frame->length;
    // ifnet_input consumes the mbuf on both success and failure.
    error = ifnet_input(interface, packet, &stats);
    if (error) {
        __atomic_fetch_add(&rx_errors, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&rx_frames, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&rx_bytes, frame->length, __ATOMIC_RELAXED);
    }
}

static void network_worker(void *argument, wait_result_t result)
{
    (void)argument;
    (void)result;
    errno_t error = attach_interface();
    printf("T733 USB net: attach unet0 result=%d MAC=02:73:30:00:00:02 MTU=1500\n", error);
    if (error) {
        return;
    }
    unsigned observed_link = UINT32_MAX;
    for (;;) {
        if (__atomic_load_n(&interface_detached, __ATOMIC_ACQUIRE)) {
            ifnet_release(network_interface);
            network_interface = NULL;
            return;
        }
        unsigned generation = __atomic_load_n(&link_generation, __ATOMIC_ACQUIRE);
        if (generation != observed_link) {
            post_link(network_interface, generation);
            observed_link = generation;
        }
        unsigned tail = __atomic_load_n(&rx_tail, __ATOMIC_RELAXED);
        unsigned head = __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE);
        if (tail != head) {
            input_frame(network_interface, &rx_ring[tail % NET_RX_SLOTS]);
            __atomic_store_n(&rx_tail, tail + 1, __ATOMIC_RELEASE);
            continue;
        }
        __atomic_store_n(&wake_event, 0u, __ATOMIC_RELEASE);
        assert_wait((event_t)&wake_event, THREAD_UNINT);
        if (__atomic_load_n(&wake_event, __ATOMIC_ACQUIRE)
            || __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE) != tail
            || __atomic_load_n(&link_generation, __ATOMIC_ACQUIRE) != observed_link
            || __atomic_load_n(&interface_detached, __ATOMIC_ACQUIRE)) {
            clear_wait(current_thread(), THREAD_AWAKENED);
        }
        thread_block(THREAD_CONTINUE_NULL);
    }
}

static void network_stack_ready(void)
{
    if (__atomic_exchange_n(&worker_started, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }
    thread_t thread = THREAD_NULL;
    kern_return_t result = kernel_thread_start(network_worker, NULL, &thread);
    if (result == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
    printf("T733 USB net: worker=%d\n", result);
}

void native_usb_net_start(void)
{
    if (__atomic_exchange_n(&start_requested, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }
    errno_t error = net_init_add(network_stack_ready);
    if (error == EALREADY) {
        network_stack_ready();
    } else if (error) {
        printf("T733 USB net: net_init_add=%d\n", error);
    }
}

void native_usb_net_dump_stats(void)
{
    unsigned generation = __atomic_load_n(&link_generation, __ATOMIC_ACQUIRE);
    printf("T733 USB net: ready=%u link=%u generation=%u queued=%u rx=%llu bytes=%llu drop=%llu error=%llu tx=%llu bytes=%llu error=%llu link_changes=%llu\n",
        __atomic_load_n(&interface_ready, __ATOMIC_ACQUIRE), generation & 1, generation >> 1,
        __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE) - __atomic_load_n(&rx_tail, __ATOMIC_ACQUIRE),
        __atomic_load_n(&rx_frames, __ATOMIC_RELAXED), __atomic_load_n(&rx_bytes, __ATOMIC_RELAXED),
        __atomic_load_n(&rx_dropped, __ATOMIC_RELAXED), __atomic_load_n(&rx_errors, __ATOMIC_RELAXED),
        __atomic_load_n(&tx_frames, __ATOMIC_RELAXED), __atomic_load_n(&tx_bytes, __ATOMIC_RELAXED),
        __atomic_load_n(&tx_errors, __ATOMIC_RELAXED), __atomic_load_n(&link_changes, __ATOMIC_RELAXED));
}
