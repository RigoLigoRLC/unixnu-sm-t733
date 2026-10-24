/* Reference source: https://github.com/xyyyds0106/ios-vermeer-lab
 * Revision: d68e05df683f863f054b0f9c063c54d2b1342b2a
 * Reference files: xnu/vermeer/vermeer_debug.c and vermeer_log_ring.h
 * All Rights Reserved by the original authors for upstream-derived code.
 * No explicit upstream license grant was found. See NOTICE for provenance.
 */
/* The console ring retains output before USB starts and across cable changes.
 * Writers never wait, allocate memory, or call a console routine. USB owns its
 * read position; ring replacement and failed writer acquisition have separate
 * byte counters. The host saves the complete received stream to disk.
 */
#include "native_usb_log.h"
#include <machine/atomic.h>
#include <sys/types.h>

#if XNU_NATIVE_USB_DEBUG
#ifndef NATIVE_USB_LOG_BYTES
#define NATIVE_USB_LOG_BYTES (4U * 1024U * 1024U)
#endif
static char log_bytes[NATIVE_USB_LOG_BYTES];
static uint64_t total;
static uint32_t _Atomic writer;
static uint64_t _Atomic dropped;
static uint64_t _Atomic published;

void native_usb_log_append(const char *data, int size)
{
    if (!data || size <= 0) {
        return;
    }
    if (!os_atomic_cmpxchg(&writer, 0, 1, acquire)) {
        os_atomic_add(&dropped, (uint64_t)size, relaxed);
        return;
    }

    for (int index = 0; index < size; ++index) {
        log_bytes[total % NATIVE_USB_LOG_BYTES] = data[index];
        ++total;
    }
    os_atomic_store(&published, total, release);
    os_atomic_store(&writer, 0, release);
}

unsigned native_usb_log_copy(uint64_t *position, char *out, unsigned capacity)
{
    if (!os_atomic_cmpxchg(&writer, 0, 1, acquire)) {
        return 0;
    }

    uint64_t first = total > NATIVE_USB_LOG_BYTES ? total - NATIVE_USB_LOG_BYTES : 0;
    if (*position < first || *position > total) {
        *position = first;
    }
    unsigned count = total - *position < capacity ? (unsigned)(total - *position) : capacity;
    for (unsigned index = 0; index < count; ++index) {
        out[index] = log_bytes[(*position + index) % NATIVE_USB_LOG_BYTES];
    }
    *position += count;
    os_atomic_store(&writer, 0, release);
    return count;
}

uint64_t native_usb_log_total(void)
{
    return os_atomic_load(&published, acquire);
}

uint64_t native_usb_log_dropped(void)
{
    return os_atomic_load(&dropped, relaxed);
}

uint64_t native_usb_log_overwritten(void)
{
    uint64_t count = native_usb_log_total();
    return count > NATIVE_USB_LOG_BYTES ? count - NATIVE_USB_LOG_BYTES : 0;
}
#endif
