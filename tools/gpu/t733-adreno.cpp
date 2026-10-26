// SPDX-License-Identifier: MIT
#include "NativeAdrenoABI.h"
#include "AdrenoPM4.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint32_t kTunnelMagic = 0x4e544e4c;
constexpr uint32_t kExpectedChip = 0x06030500;
constexpr uint32_t kExpectedGmem = 512 * 1024;
constexpr uint64_t kPageBytes = 4096;
constexpr uint32_t kCpuMapFlags = 0x101;

enum class Stage {
    Query,
    Map,
    Submit,
};

/* Options contains the selected IOKit endpoint and the last operation to run.
 * Query reads the driver's ABI and device data. Map additionally allocates two
 * BOs and maps their pages into the task and GPU. Submit writes CP_MEM_WRITE
 * commands into the first BO, waits for the kernel fence, and reads the second.
 */
struct Options {
    Stage stage = Stage::Submit;
    const char *service = "NativeAdrenoGpuPublished";
    uint32_t open_type = 0x4147;
    uint32_t wait_seconds = 120;
    uint64_t timeout_ns = 2000000000;
    bool tunnel = false;
};

void report(int priority, const char *format, ...)
{
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    printf("t733-adreno: %s\n", line);
    fflush(stdout);
    syslog(priority, "%s", line);
}

bool number(const char *text, uint64_t maximum, uint64_t *result)
{
    char *end = nullptr;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || !text[0] || text[0] == '-' || !end || *end || value > maximum) {
        return false;
    }

    *result = value;
    return true;
}

void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s [--stage query|map|submit] [--wait-seconds N] "
        "[--timeout-ms N]\n"
        "       [--service NAME] [--open-type N] [--tunnel]\n", program);
}

bool parse(int argc, char **argv, Options *options)
{
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (!strcmp(argument, "--tunnel")) {
            options->tunnel = true;
            continue;
        }

        if (index + 1 >= argc) {
            return false;
        }

        const char *value = argv[++index];
        if (!strcmp(argument, "--stage")) {
            if (!strcmp(value, "query")) {
                options->stage = Stage::Query;
            } else if (!strcmp(value, "map")) {
                options->stage = Stage::Map;
            } else if (!strcmp(value, "submit")) {
                options->stage = Stage::Submit;
            } else {
                return false;
            }
        } else if (!strcmp(argument, "--service")) {
            options->service = value;
        } else {
            uint64_t parsed = 0;
            if (!number(value, UINT32_MAX, &parsed)) {
                return false;
            }

            if (!strcmp(argument, "--open-type")) {
                options->open_type = static_cast<uint32_t>(parsed);
            } else if (!strcmp(argument, "--wait-seconds") && parsed <= 600) {
                options->wait_seconds = static_cast<uint32_t>(parsed);
            } else if (!strcmp(argument, "--timeout-ms") && parsed && parsed <= 30000) {
                options->timeout_ns = parsed * 1000000;
            } else {
                return false;
            }
        }
    }

    return true;
}

/* Connection owns the IOKit library and user-client port. The driver owns BOs
 * and queued GPU work behind that port. Closing the port lets the driver stop
 * or finish outstanding DMA before it releases the remaining BO pages.
 */
struct Connection {
    void *library = nullptr;
    mach_port_t port = MACH_PORT_NULL;
    bool tunnel = false;
    uint32_t selector = 0;

    using Matching = void *(*)(const char *);
    using GetService = mach_port_t (*)(mach_port_t, void *);
    using Open = kern_return_t (*)(mach_port_t, mach_port_t, uint32_t, mach_port_t *);
    using Close = kern_return_t (*)(mach_port_t);
    using Release = kern_return_t (*)(mach_port_t);
    using SetProperty = kern_return_t (*)(mach_port_t, CFStringRef, CFTypeRef);
    using Call = kern_return_t (*)(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);
    using Map = kern_return_t (*)(mach_port_t, uint32_t, mach_port_t, uint64_t *, uint64_t *, uint32_t);
    using Unmap = kern_return_t (*)(mach_port_t, uint32_t, mach_port_t, uint64_t);

    Matching matching = nullptr;
    GetService get_service = nullptr;
    Open open_port = nullptr;
    Close close_port = nullptr;
    Release release = nullptr;
    SetProperty set_property = nullptr;
    Call call_struct = nullptr;
    Map map = nullptr;
    Unmap unmap = nullptr;

    ~Connection()
    {
        if (port && close_port) {
            kern_return_t result = close_port(port);
            report(result ? LOG_ERR : LOG_NOTICE, "IOServiceClose status=0x%08x", result);
        }

        if (library) {
            dlclose(library);
        }
    }

    mach_port_t wait_service(const char *name, uint32_t seconds)
    {
        report(LOG_NOTICE, "waiting service=%s timeout_seconds=%u", name, seconds);
        for (uint32_t attempt = 0; attempt <= seconds * 2; ++attempt) {
            mach_port_t service = get_service(MACH_PORT_NULL, matching(name));
            if (service) {
                report(LOG_NOTICE, "service=%s found", name);
                return service;
            }

            if (attempt < seconds * 2) {
                usleep(500000);
            }
        }

        report(LOG_ERR, "service=%s unavailable after %u seconds", name, seconds);
        return MACH_PORT_NULL;
    }

    bool open(const Options &options)
    {
        library = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            report(LOG_ERR, "IOKit load failed: %s", dlerror());
            return false;
        }

        matching = reinterpret_cast<Matching>(dlsym(library, "IOServiceMatching"));
        get_service = reinterpret_cast<GetService>(dlsym(library, "IOServiceGetMatchingService"));
        open_port = reinterpret_cast<Open>(dlsym(library, "IOServiceOpen"));
        close_port = reinterpret_cast<Close>(dlsym(library, "IOServiceClose"));
        release = reinterpret_cast<Release>(dlsym(library, "IOObjectRelease"));
        set_property = reinterpret_cast<SetProperty>(dlsym(library, "IORegistryEntrySetCFProperty"));
        call_struct = reinterpret_cast<Call>(dlsym(library, "IOConnectCallStructMethod"));
        map = reinterpret_cast<Map>(dlsym(library, "IOConnectMapMemory64"));
        unmap = reinterpret_cast<Unmap>(dlsym(library, "IOConnectUnmapMemory64"));
        if (!matching || !get_service || !open_port || !close_port || !release ||
            !set_property || !call_struct || !map || !unmap) {
            report(LOG_ERR, "IOKit required export unavailable");
            return false;
        }

        uint32_t control_wait = options.wait_seconds > 120 ? 120 : options.wait_seconds;
        mach_port_t control = wait_service("NativeAdrenoControl", control_wait);
        if (!control) {
            return false;
        }

        kern_return_t start_result = set_property(control, CFSTR("Start"), kCFBooleanTrue);
        release(control);
        report(start_result ? LOG_ERR : LOG_NOTICE,
            "NativeAdrenoControl Start=true status=0x%08x", start_result);

        mach_port_t service = wait_service(options.service, options.wait_seconds);
        if (!service) {
            return false;
        }

        kern_return_t result = open_port(service, mach_task_self(), options.open_type, &port);
        release(service);
        report(result ? LOG_ERR : LOG_NOTICE, "service=%s type=0x%x open_status=0x%08x",
            options.service, options.open_type, result);
        if (result || !port) {
            return false;
        }

        tunnel = options.tunnel;
        if (tunnel) {
            const uint32_t probe[2] = {kTunnelMagic, UINT32_MAX};
            for (selector = 0; selector < 256; ++selector) {
                if (!call_struct(port, selector, probe, sizeof(probe), nullptr, nullptr)) {
                    report(LOG_NOTICE, "tunnel selector=%u", selector);
                    return true;
                }
            }

            report(LOG_ERR, "tunnel probe failed for selectors 0 through 255");
            return false;
        }

        return true;
    }

    bool call(uint32_t method, const void *input, size_t input_bytes, void *output, size_t output_bytes)
    {
        uint8_t wrapped[8 + sizeof(NativeAdrenoAbiSubmit)];
        uint32_t selected = method;
        if (tunnel) {
            const uint32_t header[2] = {kTunnelMagic, method};
            memcpy(wrapped, header, sizeof(header));
            if (input_bytes) {
                memcpy(wrapped + sizeof(header), input, input_bytes);
            }

            input = wrapped;
            input_bytes += sizeof(header);
            selected = selector;
        }

        size_t actual_bytes = output_bytes;
        kern_return_t result = call_struct(port, selected, input, input_bytes,
            output, output_bytes ? &actual_bytes : nullptr);
        report(result ? LOG_ERR : LOG_NOTICE, "method=%u status=0x%08x reply_bytes=%zu",
            method, result, actual_bytes);
        return !result && actual_bytes == output_bytes;
    }
};

/* Buffer keeps the BO handle, CPU-map token and task mapping together. The
 * caller releases task mappings before returning CPU-map tokens and BO handles
 * to the driver. A failed submission leaves these values for connection close.
 */
struct Buffer {
    uint64_t handle = 0;
    uint64_t iova = 0;
    uint32_t token = 0;
    uint64_t address = 0;
    uint64_t bytes = 0;

    volatile uint32_t *words() const
    {
        return reinterpret_cast<volatile uint32_t *>(address);
    }
};

bool allocate(Connection &connection, uint64_t requested_iova, uint32_t permissions, Buffer *buffer)
{
    NativeAdrenoAbiBoAlloc allocation = {kPageBytes, 12, 0};
    NativeAdrenoAbiBoAllocReply allocated = {};
    if (!connection.call(NativeAdrenoMethodBoAlloc, &allocation, sizeof(allocation), &allocated, sizeof(allocated))) {
        return false;
    }

    buffer->handle = allocated.handle;
    NativeAdrenoAbiBoMap mapping = {buffer->handle, requested_iova, permissions, 0, 2, 0};
    NativeAdrenoAbiBoMapReply mapped = {};
    if (!connection.call(NativeAdrenoMethodBoMap, &mapping, sizeof(mapping), &mapped, sizeof(mapped))) {
        return false;
    }

    buffer->iova = mapped.iova;
    if (!buffer->handle || buffer->iova != requested_iova) {
        report(LOG_ERR, "BO handle or IOVA reply differs from request");
        return false;
    }

    NativeAdrenoAbiBoCpuMap cpu_request = {buffer->handle};
    NativeAdrenoAbiBoCpuMapReply cpu_reply = {};
    if (!connection.call(NativeAdrenoMethodBoCpuMap, &cpu_request, sizeof(cpu_request), &cpu_reply, sizeof(cpu_reply))) {
        return false;
    }

    buffer->token = cpu_reply.token;
    kern_return_t result = connection.map(connection.port, buffer->token, mach_task_self(),
        &buffer->address, &buffer->bytes, kCpuMapFlags);
    report(result ? LOG_ERR : LOG_NOTICE,
        "BO handle=0x%" PRIx64 " iova=0x%" PRIx64 " token=%u CPU_map_status=0x%08x CPU_bytes=%" PRIu64,
        buffer->handle, buffer->iova, buffer->token, result, buffer->bytes);
    return !result && buffer->address && buffer->bytes >= kPageBytes && cpu_reply.bytes >= kPageBytes;
}

bool release(Connection &connection, Buffer *buffer)
{
    if (buffer->address) {
        kern_return_t result = connection.unmap(connection.port, buffer->token, mach_task_self(), buffer->address);
        report(result ? LOG_ERR : LOG_NOTICE, "BO CPU_unmap_status=0x%08x", result);
        if (result) {
            return false;
        }

        buffer->address = 0;
    }

    if (buffer->token) {
        NativeAdrenoAbiBoCpuUnmap request = {buffer->handle, buffer->token, 0};
        if (!connection.call(NativeAdrenoMethodBoCpuUnmap, &request, sizeof(request), nullptr, 0)) {
            return false;
        }

        buffer->token = 0;
    }

    if (buffer->handle) {
        NativeAdrenoAbiBoFree request = {buffer->handle};
        if (!connection.call(NativeAdrenoMethodBoFree, &request, sizeof(request), nullptr, 0)) {
            return false;
        }

        buffer->handle = 0;
    }

    return true;
}

void memory_barrier()
{
    __asm__ volatile("dsb sy" ::: "memory");
}

int run(Connection &connection, const Options &options)
{
    NativeAdrenoAbiParams params = {};
    NativeAdrenoAbiDeviceInfo info = {};
    NativeAdrenoAbiStatus before = {};
    NativeAdrenoAbiTimestamp started = {};
    if (!connection.call(NativeAdrenoMethodQuery, nullptr, 0, &params, sizeof(params)) ||
        !connection.call(NativeAdrenoMethodGetDeviceInfo, nullptr, 0, &info, sizeof(info)) ||
        !connection.call(NativeAdrenoMethodGetStatus, nullptr, 0, &before, sizeof(before)) ||
        !connection.call(NativeAdrenoMethodGetTimestamp, nullptr, 0, &started, sizeof(started))) {
        return 3;
    }

    report(LOG_NOTICE, "ABI=%u input_bits=%u output_bits=%u max_BO=%" PRIu64 " flags=0x%x",
        params.abi_version, params.input_bits, params.output_bits, params.max_bo_bytes, params.flags);
    report(LOG_NOTICE, "chip_id=0x%08x GMEM=%u base=0x%" PRIx64 " UCHE_trap=0x%" PRIx64,
        info.chip_id, info.gmem_bytes, info.gmem_base, info.uche_trap_base);
    report(LOG_NOTICE, "VA_start=0x%" PRIx64 " VA_size=0x%" PRIx64 " bank_bit=%u UBWC=%u swizzle=%u macrotile=%u flags=0x%x",
        info.va_start, info.va_size, info.highest_bank_bit, info.ubwc_version, info.ubwc_swizzle,
        info.macrotile_mode, info.flags);
    report(LOG_NOTICE, "before faults=%" PRIu64 " suspends=%" PRIu64 " timestamp=%" PRIu64,
        before.faults, before.suspends, started.ticks);
    if (params.abi_version != NativeAdrenoAbiVersion || info.chip_id != kExpectedChip || info.gmem_bytes != kExpectedGmem) {
        report(LOG_ERR, "A642L ABI, chip ID or GMEM size differs from required hardware values");
        return 4;
    }

    if (options.stage == Stage::Query) {
        report(LOG_NOTICE, "query completed");
        return 0;
    }

    if ((info.va_start & (kPageBytes - 1)) || info.va_size < 2 * kPageBytes ||
        info.va_start > UINT64_MAX - 2 * kPageBytes || params.max_bo_bytes < kPageBytes) {
        report(LOG_ERR, "driver BO aperture cannot contain the two 4096-byte buffers");
        return 5;
    }

    Buffer commands;
    Buffer target;
    if (!allocate(connection, info.va_start, 1, &commands) ||
        !allocate(connection, info.va_start + kPageBytes, 3, &target)) {
        return 6;
    }

    for (uint32_t word = 0; word < kPageBytes / sizeof(uint32_t); ++word) {
        target.words()[word] = 0xdeadbeef;
    }

    memory_barrier();
    report(LOG_NOTICE, "two BOs allocated and mapped, target_initial=0x%08x", target.words()[0]);
    if (options.stage == Stage::Map) {
        bool released_target = release(connection, &target);
        bool released_commands = release(connection, &commands);
        return released_target && released_commands ? 0 : 7;
    }

    const uint32_t values[] = {0x0adc0de1, 0xfeed0002, 0x600d0003};
    for (uint32_t index = 0; index < 3; ++index) {
        uint32_t header = 0;
        if (AdrenoPM4Type7(0x3d, 3, &header) != AdrenoPM4Success) {
            report(LOG_ERR, "CP_MEM_WRITE packet emission failed");
            return 8;
        }

        uint64_t address = target.iova + index * sizeof(uint32_t);
        commands.words()[index * 4] = header;
        commands.words()[index * 4 + 1] = static_cast<uint32_t>(address);
        commands.words()[index * 4 + 2] = static_cast<uint32_t>(address >> 32);
        commands.words()[index * 4 + 3] = values[index];
    }

    memory_barrier();
    NativeAdrenoAbiQueue queue_request = {};
    NativeAdrenoAbiQueueReply queue_reply = {};
    if (!connection.call(NativeAdrenoMethodCreateQueue, &queue_request, sizeof(queue_request), &queue_reply, sizeof(queue_reply))) {
        return 9;
    }

    NativeAdrenoAbiSubmit submission = {};
    submission.buffer_count = 1;
    submission.queue = queue_reply.queue;
    submission.buffers[0] = {commands.handle, commands.iova, 12 * sizeof(uint32_t)};
    NativeAdrenoAbiSubmitReply submitted = {};
    if (!connection.call(NativeAdrenoMethodSubmit, &submission, sizeof(submission), &submitted, sizeof(submitted))) {
        return 10;
    }

    report(LOG_NOTICE, "queue=%" PRIu64 " submitted_fence=%" PRIu64, queue_reply.queue, submitted.fence);
    if (!submitted.fence) {
        report(LOG_ERR, "driver returned an empty submission fence");
        return 11;
    }

    NativeAdrenoAbiWaitFence wait = {submitted.fence, options.timeout_ns};
    if (!connection.call(NativeAdrenoMethodWaitFence, &wait, sizeof(wait), nullptr, 0)) {
        NativeAdrenoAbiStatus failed = {};
        connection.call(NativeAdrenoMethodGetStatus, nullptr, 0, &failed, sizeof(failed));
        report(LOG_ERR, "fence wait failed, faults=%" PRIu64 " suspends=%" PRIu64 "; closing the GPU connection",
            failed.faults, failed.suspends);
        return 12;
    }

    memory_barrier();
    bool matched = true;
    for (uint32_t index = 0; index < 4; ++index) {
        uint32_t expected = index < 3 ? values[index] : 0xdeadbeef;
        uint32_t observed = target.words()[index];
        report(observed == expected ? LOG_NOTICE : LOG_ERR,
            "readback[%u]=0x%08x expected=0x%08x", index, observed, expected);
        matched = matched && observed == expected;
    }

    NativeAdrenoAbiStatus after = {};
    NativeAdrenoAbiTimestamp finished = {};
    bool status_received = connection.call(NativeAdrenoMethodGetStatus, nullptr, 0, &after, sizeof(after));
    bool timestamp_received = connection.call(NativeAdrenoMethodGetTimestamp, nullptr, 0, &finished, sizeof(finished));
    report(LOG_NOTICE, "after faults=%" PRIu64 " suspends=%" PRIu64 " timestamp=%" PRIu64,
        after.faults, after.suspends, finished.ticks);

    NativeAdrenoAbiQueue close_queue = {queue_reply.queue};
    bool queue_closed = connection.call(NativeAdrenoMethodCloseQueue, &close_queue, sizeof(close_queue), nullptr, 0);
    bool target_released = release(connection, &target);
    bool commands_released = release(connection, &commands);
    bool success = matched && status_received && timestamp_received && after.faults == before.faults &&
        after.suspends == before.suspends && queue_closed && target_released && commands_released;
    report(success ? LOG_NOTICE : LOG_ERR, "CP_MEM_WRITE fence and readback result=%s", success ? "completed" : "failed");
    return success ? 0 : 13;
}

}

int main(int argc, char **argv)
{
    Options options;
    if (!parse(argc, argv, &options)) {
        usage(argv[0]);
        return 1;
    }

    openlog("t733-adreno", LOG_PID | LOG_CONS, LOG_DAEMON);
    report(LOG_NOTICE, "starting A642L userspace command");
    Connection connection;
    if (!connection.open(options)) {
        return 2;
    }

    return run(connection, options);
}
