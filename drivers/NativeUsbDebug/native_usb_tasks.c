/* NativeUsbTasks copies a bounded process and scheduler snapshot for USB.
 * The process helper returns references without waiting for exec. Each task
 * and scheduler lock is tried once; locked fields are copied into static rows
 * before references are released. Formatting only touches the copied rows.
 */
#if XNU_NATIVE_USB_DEBUG
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc_internal.h>

#include "native_usb_log.h"
#include "native_usb_tasks.h"
#include <kern/clock.h>

#define USB_TASK_PROCESSES 256
#define USB_TASK_THREADS 512
#define USB_THREADS_PER_PROCESS 48

extern int native_usb_process_refs(proc_t *, unsigned, unsigned *, unsigned *, bool *);

/* UsbProcessRow holds copied fields; no proc or task pointer escapes collection. */
struct UsbProcessRow {
    int pid;
    char name[64];
    unsigned process_state, thread_count, first_thread, copied_threads;
    bool focused, task_busy, name_busy, truncated;
    bool stacks;
    struct UsbTaskSummary task_info;
};

static proc_t process_refs[USB_TASK_PROCESSES];
static struct UsbProcessRow processes[USB_TASK_PROCESSES];
static struct UsbThreadRow thread_snapshots[USB_TASK_THREADS];
static unsigned sample_number;
static const char *const focus_names[] = {
    "SpringBoard", "backboardd",   "launchd",           "runningboardd", "mediaserverd", "cfprefsd",
    "containerd",  "mobileassetd", "identityservicesd", "contextstored", "locationd",
    "chronod", "liveactivitiesd", "Setup"};
static const char *const stack_names[] = {
    "SpringBoard", "backboardd", "chronod", "liveactivitiesd", "contextstored", "locationd",
    "Setup"};
static const char *const stack_stops[] = {
    "not-requested", "not-offcpu-waiting", "complete", "frame-limit", "deadline",
    "invalid-frame", "pmap-busy", "page-busy", "not-resident", "mapping-changed", "unsupported"};

static void tasks_emit(const char *format, ...) __printflike(1, 2);

static void tasks_emit(const char *format, ...)
{
    char line[384];
    va_list args;
    va_start(args, format);
    int size = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (size > 0) {
        native_usb_log_append(line, size < (int)sizeof(line) ? size : (int)sizeof(line) - 1);
    }
}

void native_usb_tasks_snapshot(const char *process_name)
{
    unsigned sample = ++sample_number;
    unsigned examined = 0, skipped = 0, thread_rows = 0;
    bool truncated = false, timed_out = false;
    bool targeted = process_name && process_name[0];
    bool target_seen = false;
    bool seen[sizeof(focus_names) / sizeof(focus_names[0])] = {false};
    uint64_t deadline;
    clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
    int count =
        native_usb_process_refs(process_refs, USB_TASK_PROCESSES, &examined, &skipped, &truncated);
    if (count < 0) {
        tasks_emit("T733 TASKS: process list busy; retry tasks\n");
        return;
    }

    memset(processes, 0, sizeof(processes));
    memset(thread_snapshots, 0, sizeof(thread_snapshots));
    for (int index = 0; index < count; ++index) {
        proc_t process = process_refs[index];
        struct UsbProcessRow *row = &processes[index];
        row->pid = proc_pid(process);
        row->process_state = process->p_stat;
        if (lck_mtx_try_lock(&process->p_mlock)) {
            strlcpy(row->name, proc_best_name(process), sizeof(row->name));
            proc_unlock(process);
        } else {
            strlcpy(row->name, "(name locked)", sizeof(row->name));
            row->name_busy = true;
        }
        for (unsigned name = 0; name < sizeof(focus_names) / sizeof(focus_names[0]); ++name) {
            if (!strcmp(row->name, focus_names[name])) {
                row->focused = true;
                seen[name] = true;
            }
        }
        for (unsigned name = 0; name < sizeof(stack_names) / sizeof(stack_names[0]); ++name) {
            if (!strcmp(row->name, stack_names[name])) {
                row->stacks = true;
            }
        }

        if (targeted) {
            row->focused = !strcmp(row->name, process_name);
            row->stacks = row->focused;
            target_seen |= row->focused;
        }

        if (mach_absolute_time() >= deadline) {
            timed_out = true;
            row->truncated = true;
        } else {
            struct UsbTaskSummary *summary = &row->task_info;
            unsigned capacity = USB_TASK_THREADS - thread_rows;
            if (capacity > USB_THREADS_PER_PROCESS) {
                capacity = USB_THREADS_PER_PROCESS;
            }
            if (!row->focused) {
                capacity = 0;
            }
            row->first_thread = thread_rows;
            native_usb_copy_thread_rows(proc_task(process), &thread_snapshots[thread_rows],
                                       capacity, row->focused, row->stacks, deadline, summary);
            row->thread_count = summary->thread_count;
            row->copied_threads = summary->copied_threads;
            row->task_busy = summary->busy;
            row->truncated = summary->truncated;
            thread_rows += summary->copied_threads;
        }
        proc_rele(process);
        process_refs[index] = PROC_NULL;
    }

    tasks_emit("T733 TASKS: begin processes=%d examined=%u exec_or_dead_skipped=%u truncated=%u "
               "time_limit=%u; thread state WAIT=1 SUSP=2 RUN=4; saved-user PC only for off-CPU "
               "waiting threads with both locks\n",
               count, examined, skipped, truncated, timed_out);
    for (int index = 0; index < count; ++index) {
        const struct UsbProcessRow *row = &processes[index];
        tasks_emit("T733 PROC: pid=%d name=%s pstat=%u threads=%u focus=%u task_busy=%u "
                   "name_busy=%u truncated=%u\n",
                   row->pid, row->name, row->process_state, row->thread_count, row->focused,
                   row->task_busy, row->name_busy, row->truncated);
        if (row->stacks) {
            const struct UsbTaskSummary *info = &row->task_info;
            tasks_emit("T733 STACK_CACHE: sample=%u pid=%d name=%s valid=%u slide=0x%llx "
                       "base=0x%llx main_load=0x%llx\n",
                       sample, row->pid, row->name, info->cache_valid,
                       (unsigned long long)info->cache_slide,
                       (unsigned long long)info->cache_base, (unsigned long long)info->main_load);
            if (info->main_path[0]) {
                tasks_emit("T733 STACK_IMAGE: sample=%u pid=%d name=%s load=0x%llx path=%s\n",
                           sample, row->pid, row->name, (unsigned long long)info->main_load,
                           info->main_path);
            }
        }
        for (unsigned n = 0; n < row->copied_threads; ++n) {
            const struct UsbThreadRow *copy = &thread_snapshots[row->first_thread + n];
            tasks_emit(
                "T733 THREAD: pid=%d tid=%llu busy=%u state=0x%x wait=%u runnable=%u suspended=%u "
                "priority=%d wait_result=%d wait_event=0x%llx continuation=0x%llx\n",
                row->pid, (unsigned long long)copy->id, copy->busy, copy->state,
                copy->waiting, copy->runnable, copy->suspended,
                copy->priority, copy->wait_result, (unsigned long long)copy->wait_event,
                (unsigned long long)copy->continuation);
            if (copy->user_state_valid) {
                tasks_emit("T733 USERSTATE: pid=%d tid=%llu saved_pc=0x%llx saved_lr=0x%llx\n",
                           row->pid, (unsigned long long)copy->id,
                           (unsigned long long)copy->user_pc, (unsigned long long)copy->user_lr);
            }
            if (row->stacks) {
                unsigned frame = 0;
                if (copy->user_state_valid) {
                    tasks_emit("T733 STACK: sample=%u pid=%d tid=%llu name=%s frame=%u "
                               "pc=0x%llx kind=pc\n", sample, row->pid,
                               (unsigned long long)copy->id, row->name, frame++,
                               (unsigned long long)copy->user_pc);
                    if (copy->user_lr) {
                        tasks_emit("T733 STACK: sample=%u pid=%d tid=%llu name=%s frame=%u "
                                   "pc=0x%llx kind=lr\n", sample, row->pid,
                                   (unsigned long long)copy->id, row->name, frame++,
                                   (unsigned long long)copy->user_lr);
                    }
                    for (unsigned f = 0; f < copy->frame_count; ++f) {
                        tasks_emit("T733 STACK: sample=%u pid=%d tid=%llu name=%s frame=%u "
                                   "pc=0x%llx kind=fp\n", sample, row->pid,
                                   (unsigned long long)copy->id, row->name, frame++,
                                   (unsigned long long)copy->frames[f]);
                    }
                    tasks_emit("T733 STACK_REGS: sample=%u pid=%d tid=%llu sp=0x%llx fp=0x%llx "
                               "x16=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
                               sample, row->pid, (unsigned long long)copy->id,
                               (unsigned long long)copy->user_sp, (unsigned long long)copy->user_fp,
                               (unsigned long long)copy->user_x16,
                               (unsigned long long)copy->user_args[0],
                               (unsigned long long)copy->user_args[1],
                               (unsigned long long)copy->user_args[2],
                               (unsigned long long)copy->user_args[3]);
                    tasks_emit("T733 STACK_REGS_MORE: sample=%u pid=%d tid=%llu "
                               "x4=0x%llx x5=0x%llx x6=0x%llx x7=0x%llx\n",
                               sample, row->pid, (unsigned long long)copy->id,
                               (unsigned long long)copy->user_args[4],
                               (unsigned long long)copy->user_args[5],
                               (unsigned long long)copy->user_args[6],
                               (unsigned long long)copy->user_args[7]);
                    tasks_emit("T733 STACK_IPC: sample=%u pid=%d tid=%llu hint=%u type=%u "
                               "options=0x%llx receive_name=0x%x special=%u link=%u "
                               "reply_pid=%d busy=%u\n", sample, row->pid,
                               (unsigned long long)copy->id, copy->block_hint, copy->ipc_type,
                               (unsigned long long)copy->ipc_options, copy->ipc_receive_name,
                               copy->ipc_special, copy->ipc_link, copy->ipc_reply_pid, copy->ipc_busy);
                }
                const char *stop = copy->stack_stop < sizeof(stack_stops) / sizeof(stack_stops[0])
                    ? stack_stops[copy->stack_stop] : "unknown";
                tasks_emit("T733 STACK_END: sample=%u pid=%d tid=%llu name=%s frames=%u stop=%s\n",
                           sample, row->pid, (unsigned long long)copy->id, row->name, frame, stop);
            }
        }
    }
    if (targeted) {
        tasks_emit("T733 FOCUS: name=%s observed=%u\n", process_name, target_seen);
    } else {
        for (unsigned name = 0; name < sizeof(focus_names) / sizeof(focus_names[0]); ++name) {
            tasks_emit("T733 FOCUS: name=%s observed=%u\n", focus_names[name], seen[name]);
        }
    }
    tasks_emit("T733 TASKS: end thread_rows=%u; observations are sequential, missing names may be "
               "in exec or beyond the limit\n",
               thread_rows);
}
#endif
