/* NativeUsbThreadSnapshot copies scheduler fields under the task and thread
 * locks. The BSD caller owns a process reference throughout this operation.
 * All lock acquisitions are attempted once; formatting happens after return.
 */
#if XNU_NATIVE_USB_DEBUG
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/spl.h>
#include <arm/thread.h>
#include <arm64/proc_reg.h>
#include <arm/pmap/pmap_internal.h>
#include <vm/vm_map.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_port.h>
#include <string.h>
#include "native_usb_tasks.h"

extern pmap_paddr_t pmap_vtophys(pmap_t, addr64_t);
extern int native_usb_shared_region_info(task_t, uint64_t *, uint64_t *);

static bool stack_user_range(uint64_t address, unsigned size)
{
    return address >= PAGE_SIZE && address < (UINT64_C(1) << 39) &&
           size <= (UINT64_C(1) << 39) - address;
}

/* The resident reader holds the pmap and each source page's PVH while copying.
 * Pmap exclusion protects page-table storage; PVH exclusion blocks disconnect
 * and page reuse. The translation is checked again after the PVH is acquired.
 */
static unsigned stack_read_resident(pmap_t pmap, uint64_t address, void *destination,
                                    unsigned size)
{
#if XNU_MONITOR
    return USB_STACK_UNSUPPORTED;
#else
    if (!stack_user_range(address, size) || size > USB_STACK_PATH || !pmap) {
        return USB_STACK_INVALID;
    }
    if (!pmap_try_lock(pmap, PMAP_LOCK_EXCLUSIVE)) {
        return USB_STACK_PMAP_BUSY;
    }

    unsigned result = USB_STACK_COMPLETE;
    unsigned copied = 0;
    while (copied < size) {
        uint64_t va = address + copied;
        pmap_paddr_t pa = pmap_vtophys(pmap, va);
        if (!pa || !pa_valid(pa)) {
            result = USB_STACK_NOT_RESIDENT;
            break;
        }
        unsigned index = pa_index(pa);
        hw_lock_bit_t *word = (hw_lock_bit_t *)(&pv_head_table[index]) + PVH_LOCK_WORD;
        if (!hw_lock_bit_try(word, PVH_LOCK_BIT - PVH_LOCK_WORD * 32, &pmap_lck_grp)) {
            result = USB_STACK_PAGE_BUSY;
            break;
        }
        if (pmap_vtophys(pmap, va) != pa) {
            result = USB_STACK_MAPPING_CHANGED;
            pvh_unlock(index);
            break;
        }
        unsigned chunk = (unsigned)(PAGE_SIZE - (pa & PAGE_MASK));
        if (chunk > size - copied) {
            chunk = size - copied;
        }
        memcpy((char *)destination + copied, (const void *)phystokv(pa), chunk);
        pvh_unlock(index);
        copied += chunk;
    }
    pmap_unlock(pmap, PMAP_LOCK_EXCLUSIVE);
    return result;
#endif
}

static void stack_copy_images(task_t task, struct UsbTaskSummary *summary)
{
    summary->main_load = task->mach_header_vm_address;
    summary->cache_valid = native_usb_shared_region_info(task, &summary->cache_base,
                                                        &summary->cache_slide);
    uint64_t info[2] = {0};
    uint64_t entry[3] = {0};
    pmap_t pmap = vm_map_pmap(task->map);
    if (stack_read_resident(pmap, task_get_all_image_info_addr(task), info, sizeof(info)) !=
            USB_STACK_COMPLETE || !(info[0] >> 32) ||
        stack_read_resident(pmap, info[1], entry, sizeof(entry)) != USB_STACK_COMPLETE ||
        entry[0] != summary->main_load) {
        return;
    }
    for (unsigned offset = 0; offset < sizeof(summary->main_path) - 1; ++offset) {
        char byte = 0;
        if (stack_read_resident(pmap, entry[1] + offset, &byte, 1) != USB_STACK_COMPLETE) {
            summary->main_path[0] = 0;
            return;
        }
        summary->main_path[offset] = byte;
        if (!byte) {
            return;
        }
    }
    summary->main_path[0] = 0;
}

static void stack_copy_frames(task_t task, struct UsbThreadRow *copy, uint64_t deadline)
{
    uint64_t frame = copy->user_fp;
    uint64_t start = copy->user_sp;
    uint64_t short_deadline;
    clock_interval_to_deadline(1, NSEC_PER_MSEC, &short_deadline);
    if (short_deadline < deadline) {
        deadline = short_deadline;
    }
    copy->stack_stop = USB_STACK_COMPLETE;
    for (unsigned n = 0; frame && n < USB_STACK_FRAMES; ++n) {
        if (mach_absolute_time() >= deadline) {
            copy->stack_stop = USB_STACK_DEADLINE;
            return;
        }
        if (!stack_user_range(start, 16) || !stack_user_range(frame, 16) || (frame & 15) ||
            frame < start || frame - start > 1024 * 1024) {
            copy->stack_stop = USB_STACK_INVALID;
            return;
        }
        uint64_t pair[2] = {0};
        copy->stack_stop = stack_read_resident(vm_map_pmap(task->map), frame, pair, sizeof(pair));
        if (copy->stack_stop != USB_STACK_COMPLETE) {
            return;
        }
        if (pair[1]) {
            if (!stack_user_range(pair[1], 4) || (pair[1] & 3)) {
                copy->stack_stop = USB_STACK_INVALID;
                return;
            }
            copy->frames[copy->frame_count++] = pair[1];
        }
        if (!pair[0]) {
            return;
        }
        if (pair[0] <= frame) {
            copy->stack_stop = USB_STACK_INVALID;
            return;
        }
        frame = pair[0];
    }
    if (frame) {
        copy->stack_stop = USB_STACK_LIMIT;
    }
}

static void stack_copy_ipc(thread_t thread, struct UsbThreadRow *copy)
{
    copy->ipc_reply_pid = -1;
    if (thread->block_hint != kThreadWaitPortReceive) {
        return;
    }
    ipc_object_t object = thread->ith_object;
    copy->ipc_options = thread->ith_option;
    copy->ipc_receive_name = thread->ith_receiver_name;
    if (!IO_VALID(object)) {
        return;
    }
    copy->ipc_type = io_otype(object);
    if (copy->ipc_type != IOT_PORT) {
        return;
    }
    ipc_port_t port = ip_object_to_port(object);
    if (!ip_mq_lock_try(port)) {
        copy->ipc_busy = 1;
        return;
    }
    if (ip_active(port)) {
        copy->ipc_receive_name = ip_get_receiver_name(port);
        copy->ipc_special = port->ip_specialreply;
        copy->ipc_link = port->ip_sync_link_state;
        if (port->ip_specialreply) {
            copy->ipc_reply_pid = ipc_special_reply_get_pid_locked(port);
        }
    }
    ip_mq_unlock(port);
}

void native_usb_copy_thread_rows(struct task *task, struct UsbThreadRow *rows,
                                unsigned capacity, unsigned focused, unsigned stacks,
                                uint64_t deadline,
                                struct UsbTaskSummary *summary)
{
    if (task == TASK_NULL) {
        return;
    }
    if (!task_lock_try(task)) {
        summary->busy = 1;
        return;
    }

    summary->thread_count = task->thread_count;
    if (stacks && task->map) {
        stack_copy_images(task, summary);
    }
    if (focused) {
        thread_t thread;
        queue_iterate(&task->threads, thread, thread_t, task_threads) {
            if (summary->copied_threads == capacity || mach_absolute_time() >= deadline) {
                summary->truncated = 1;
                break;
            }
            struct UsbThreadRow *copy = &rows[summary->copied_threads++];
            copy->id = thread->thread_id;
            copy->stack_stop = stacks ? USB_STACK_NOT_WAITING : USB_STACK_NOT_REQUESTED;
            boolean_t mutex_owned = thread_mtx_try(thread);
            spl_t level = splsched();
            if (simple_lock_try(&thread->sched_lock, &thread_lck_grp)) {
                copy->state = thread->state;
                copy->waiting = !!(thread->state & TH_WAIT);
                copy->runnable = !!(thread->state & TH_RUN);
                copy->suspended = !!(thread->state & TH_SUSP);
                copy->wait_event = thread->wait_event;
                copy->wait_result = thread->wait_result;
                copy->priority = thread->sched_pri;
                copy->block_hint = thread->block_hint;
                if (!(thread->state & TH_RUN)) {
                    copy->continuation = (uintptr_t)thread->continuation;
                }
                /* TH_RUN clears after the thread switches off CPU. Its mutex
                 * excludes state setters while the scheduler lock blocks wakeup.
                 */
                if (mutex_owned && thread->active && (thread->state & TH_WAIT) &&
                    !(thread->state & (TH_RUN | TH_TERMINATE | TH_TERMINATE2))) {
                    arm_saved_state_t *saved = get_user_regs(thread);
                    if (saved && is_saved_state64(saved)) {
                        copy->user_pc = get_saved_state_pc(saved);
                        copy->user_lr = get_saved_state_lr(saved);
                        copy->user_state_valid = 1;
                        copy->user_fp = get_saved_state_fp(saved);
                        copy->user_sp = get_saved_state_sp(saved);
                        copy->user_x16 = get_saved_state_reg(saved, 16);
                        for (unsigned reg = 0; reg < 8; ++reg) {
                            copy->user_args[reg] = get_saved_state_reg(saved, reg);
                        }
                        if (stacks && task->map) {
                            stack_copy_ipc(thread, copy);
                            stack_copy_frames(task, copy, deadline);
                        }
                    }
                }
                thread_unlock(thread);
            } else {
                copy->busy = 1;
            }
            splx(level);
            if (mutex_owned) {
                thread_mtx_unlock(thread);
            }
        }
    }
    task_unlock(task);
}
#endif
