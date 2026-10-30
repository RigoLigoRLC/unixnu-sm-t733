#ifndef NATIVE_USB_TASKS_H
#define NATIVE_USB_TASKS_H

#include <stdint.h>

struct task;

#define USB_STACK_FRAMES 32
#define USB_STACK_PATH 192
#define USB_KERNEL_FRAMES 16
#define USB_WAIT_MESSAGE 64

enum UsbStackStop {
    USB_STACK_NOT_REQUESTED,
    USB_STACK_NOT_WAITING,
    USB_STACK_COMPLETE,
    USB_STACK_LIMIT,
    USB_STACK_DEADLINE,
    USB_STACK_INVALID,
    USB_STACK_PMAP_BUSY,
    USB_STACK_PAGE_BUSY,
    USB_STACK_NOT_RESIDENT,
    USB_STACK_MAPPING_CHANGED,
    USB_STACK_UNSUPPORTED,
    USB_STACK_NOT_ATTACHED,
    USB_STACK_CONTINUATION
};

/* UsbThreadRow contains fields copied while the scheduler lock is held. */
struct UsbThreadRow {
    uint64_t id, wait_event, continuation;
    uint64_t user_pc, user_lr;
    unsigned state, waiting, runnable, suspended;
    int priority, wait_result;
    unsigned busy, user_state_valid;
    uint64_t bsd_wait_channel;
    char wait_message[USB_WAIT_MESSAGE];
    unsigned wait_fields_valid;
    uint64_t kernel_sp, kernel_fp, kernel_lr;
    uint64_t kernel_frames[USB_KERNEL_FRAMES];
    unsigned kernel_frame_count, kernel_stop;
    uint64_t user_sp, user_fp, user_x16, user_args[8];
    uint64_t frames[USB_STACK_FRAMES];
    unsigned frame_count, stack_stop, block_hint;
    uint64_t ipc_options;
    unsigned ipc_receive_name, ipc_type, ipc_special, ipc_link, ipc_busy;
    int ipc_reply_pid;
};

/* UsbTaskSummary describes the rows copied under one task lock. */
struct UsbTaskSummary {
    unsigned thread_count, copied_threads, busy, truncated;
    uint64_t next_thread;
    unsigned cursor_found;
    uint64_t cache_base, cache_slide, main_load;
    unsigned cache_valid;
    char main_path[USB_STACK_PATH];
};

void native_usb_copy_thread_rows(struct task *, struct UsbThreadRow *, unsigned,
                                unsigned, unsigned, uint64_t, uint64_t,
                                struct UsbTaskSummary *);

/* The caller keeps the waiting thread off CPU while its BSD fields are copied. */
void native_usb_copy_wait_fields(void *, struct UsbThreadRow *);

#endif
