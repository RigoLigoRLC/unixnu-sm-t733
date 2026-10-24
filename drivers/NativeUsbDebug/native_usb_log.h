/* Reference source: https://github.com/xyyyds0106/ios-vermeer-lab
 * Revision: d68e05df683f863f054b0f9c063c54d2b1342b2a
 * Reference file: xnu/vermeer/vermeer_log_ring.h
 * All Rights Reserved by the original authors for upstream-derived code.
 * No explicit upstream license grant was found. See NOTICE for provenance.
 */
#ifndef NATIVE_USB_LOG_H
#define NATIVE_USB_LOG_H
#include <sys/types.h>

void native_usb_log_append(const char *data, int size);
unsigned native_usb_log_copy(uint64_t *position, char *out, unsigned capacity);
uint64_t native_usb_log_total(void);
uint64_t native_usb_log_dropped(void);
uint64_t native_usb_log_overwritten(void);
void native_usb_debug_start(void);
void native_usb_tasks_snapshot(const char *process_name);
void native_usb_debug_panic(const char *message, unsigned long caller);
#endif
