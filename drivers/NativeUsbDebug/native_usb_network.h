/* NativeUsbDebug exchanges complete Ethernet frames with the network adapter.
 * transmit copies into a bounded queue and returns zero or an errno value.
 * receive copies the frame before returning; set_link only queues a state change.
 */
#ifndef NATIVE_USB_NETWORK_H
#define NATIVE_USB_NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

int native_usb_net_transmit(const void *frame, unsigned length);
void native_usb_net_receive(const void *frame, unsigned length);
void native_usb_net_set_link(int up);
void native_usb_net_start(void);
void native_usb_net_dump_stats(void);

#ifdef __cplusplus
}
#endif

#endif
