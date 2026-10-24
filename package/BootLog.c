// SPDX-License-Identifier: MIT
// The writer saves kernel messages and the console ring in NativeUfs's final
// 32 MiB window. The SHLOG001 header is compatible with the sheng log reader.
#include <kern/thread.h>
#include <kern/startup.h>
#include <machine/atomic.h>
#include <pexpert/pexpert.h>
#include <sys/msgbuf.h>
#include <string.h>
#include <IOKit/IOLib.h>

extern int native_ufs_log_open(uint64_t *, uint64_t *, const char **);
extern int native_ufs_log_write(uint64_t, uint32_t, const void *);
extern int native_ufs_log_sync(void);
extern int native_ufs_log_panic_write(const void *);
extern unsigned native_ufs_store_gpt_check(void);
extern unsigned native_usb_log_copy(uint64_t *, char *, unsigned) __attribute__((weak_import));
extern uint64_t native_usb_log_total(void) __attribute__((weak_import));
extern struct msgbuf *msgbufp;
void native_boot_log_panic(const char *, unsigned long);

enum {
    LogBlock = 4096,
    LogMsgFirst = 1,
    LogMsgBlocks = 32,
    LogSlotFirst = 33,
    LogSlotBlocks = 16,
    LogPeriodMs = 2000,
};

// BootLogHeader describes the committed message buffer and text slots.
struct BootLogHeader {
    char magic[8];
    uint32_t version, header_bytes;
    uint64_t boot_id, first_lba;
    uint32_t window_blocks, msg_first, msg_blocks, msg_bytes;
    uint32_t slot_first, slot_blocks, slot_count, slots_written;
    uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
    char tag[32];
};

static uint8_t message_buffer[LogMsgBlocks * LogBlock] __attribute__((aligned(LogBlock)));
static uint8_t slot_buffer[LogSlotBlocks * LogBlock] __attribute__((aligned(LogBlock)));
static uint8_t header_buffer[LogBlock] __attribute__((aligned(LogBlock)));
static uint8_t panic_buffer[LogBlock] __attribute__((aligned(LogBlock)));
static uint32_t _Atomic panic_claimed;
static uint32_t _Atomic window_ready;
static uint64_t boot_id;

static uint64_t
ticks(void)
{
    uint64_t value;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static uint32_t
copy_messages(void)
{
    memset(message_buffer, 0, sizeof(message_buffer));
    struct msgbuf *buffer = msgbufp;
    if (!buffer || !buffer->msg_bufc || buffer->msg_size <= 0) {
        return 0;
    }

    int size = buffer->msg_size;
    int next = buffer->msg_bufx;
    if (next < 0 || next >= size) {
        next = 0;
    }
    int kept = size < (int)sizeof(message_buffer) ? size : (int)sizeof(message_buffer);
    int first = (next + size - kept) % size;
    uint32_t copied = 0;
    for (int index = 0; index < kept; ++index) {
        char value = buffer->msg_bufc[(first + index) % size];
        if (value) {
            message_buffer[copied++] = value;
        }
    }
    return copied;
}

// The panic record occupies the final block, outside the normal slot ring.
// It uses fixed storage and a single nonblocking UFS attempt.
void
native_boot_log_panic(const char *message, unsigned long caller)
{
    if (!os_atomic_load(&window_ready, acquire) ||
        !os_atomic_cmpxchg(&panic_claimed, 0, 1, acquire)) {
        return;
    }

    memcpy(panic_buffer, "T733PAN1", 8);
    memcpy(panic_buffer + 8, &boot_id, sizeof(boot_id));
    snprintf((char *)panic_buffer + 16, sizeof(panic_buffer) - 16,
        "caller=%lx: %s\n", caller, message ? message : "(no message)");
    native_ufs_log_panic_write(panic_buffer);
}

static void
writer(void *argument, wait_result_t result)
{
    (void)argument;
    (void)result;
    thread_set_thread_name(current_thread(), "t733_boot_log");
    uint64_t first = 0, blocks = 0;
    const char *reason = "";
    while (!native_ufs_log_open(&first, &blocks, &reason)) {
        if (strcmp(reason, "no root layout")) {
            printf("T733 BootLog: open refused: %s\n", reason);
            goto done;
        }
        IOSleep(1000);
    }
    if (blocks <= LogSlotFirst + LogSlotBlocks + 1 || blocks > UINT32_MAX) {
        goto done;
    }

    struct BootLogHeader header = {0};
    memcpy(header.magic, "SHLOG001", 8);
    header.version = 1;
    header.header_bytes = sizeof(header);
    header.boot_id = boot_id;
    header.first_lba = first;
    header.window_blocks = (uint32_t)blocks;
    header.msg_first = LogMsgFirst;
    header.msg_blocks = LogMsgBlocks;
    header.slot_first = LogSlotFirst;
    header.slot_blocks = LogSlotBlocks;
    header.slot_count = (uint32_t)(blocks - LogSlotFirst - 1) / LogSlotBlocks;
    header.gpt_open = header.gpt_last = native_ufs_store_gpt_check();
    header.state = 2;
    PE_parse_boot_argn("t733-build", header.tag, sizeof(header.tag));
    header.tag[sizeof(header.tag) - 1] = 0;
    if (header.gpt_open) {
        printf("T733 BootLog: GPT changed before logging (%u)\n", header.gpt_open);
        goto done;
    }

    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (!frequency) {
        goto done;
    }
    uint64_t cursor = 0;
    uint32_t errors = 0;
    os_atomic_store(&window_ready, 1, release);
    printf("T733 BootLog: LUN0 LBA %llu, %llu blocks, interval %ums\n",
        first, blocks, (unsigned)LogPeriodMs);

    for (;;) {
        header.uptime_s = (uint32_t)((ticks() - boot_id) / frequency);
        memset(slot_buffer, 0, sizeof(slot_buffer));
        int count = snprintf((char *)slot_buffer, sizeof(slot_buffer),
            "#slot %u t=%us tag=%s\n[console]\n",
            header.slots_written, header.uptime_s, header.tag);
        unsigned used = (unsigned)count;
        if (&native_usb_log_copy && &native_usb_log_total) {
            uint64_t end = native_usb_log_total();
            while (cursor < end && used < sizeof(slot_buffer) - 1) {
                unsigned capacity = (unsigned)sizeof(slot_buffer) - 1 - used;
                if (capacity > LogBlock) {
                    capacity = LogBlock;
                }
                unsigned copied = native_usb_log_copy(&cursor,
                    (char *)slot_buffer + used, capacity);
                if (!copied) {
                    break;
                }
                used += copied;
            }
        }

        int ok = native_ufs_log_write(LogSlotFirst +
            (uint64_t)(header.slots_written % header.slot_count) * LogSlotBlocks,
            LogSlotBlocks, slot_buffer);
        header.msg_bytes = copy_messages();
        ok = native_ufs_log_write(LogMsgFirst, LogMsgBlocks, message_buffer) && ok;
        ++header.rounds;
        if (ok) {
            ++header.slots_written;
        } else {
            ++header.write_errors;
        }
        if (!(header.rounds % 30)) {
            header.gpt_last = native_ufs_store_gpt_check();
        }
        if (header.gpt_last != header.gpt_open) {
            header.state = 4;
        }
        memset(header_buffer, 0, sizeof(header_buffer));
        memcpy(header_buffer, &header, sizeof(header));
        ok = native_ufs_log_write(0, 1, header_buffer) && ok;
        ok = native_ufs_log_sync() && ok;
        errors = ok ? 0 : errors + 1;
        if (errors >= 3 || header.state == 4) {
            printf("T733 BootLog: stopped errors=%u GPT=%u\n", errors, header.gpt_last);
            break;
        }
        IOSleep(LogPeriodMs);
    }

done:
    os_atomic_store(&window_ready, 0, release);
    thread_terminate(current_thread());
}

static void
start(const void *argument)
{
    (void)argument;
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("sheng-log", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    boot_id = ticks();
    thread_t thread;
    if (kernel_thread_start(writer, NULL, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}
STARTUP_ARG(EARLY_BOOT, STARTUP_RANK_LAST, start, NULL);
