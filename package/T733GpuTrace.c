// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeAdrenoTrace.h"
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

/* TraceIoKit holds the native entry points used by the USB network helper.
 * Its handoff connection can control and read the recorder without allocating
 * BOs or occupying one of the four rendering connections. */
struct TraceIoKit {
    void *library;
    CFMutableDictionaryRef (*matching)(const char *);
    mach_port_t (*service)(mach_port_t, CFDictionaryRef);
    kern_return_t (*open)(mach_port_t, task_port_t, uint32_t, mach_port_t *);
    kern_return_t (*close)(mach_port_t);
    kern_return_t (*release)(mach_port_t);
    kern_return_t (*call)(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);
};

static bool transfer_bytes(int socket, void *bytes, size_t count, bool send_bytes)
{
    size_t completed = 0;
    while (completed < count) {
        ssize_t result = send_bytes ?
            send(socket, (const char *)bytes + completed, count - completed, 0) :
            recv(socket, (char *)bytes + completed, count - completed, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }

        completed += (size_t)result;
    }

    return true;
}

static bool load_iokit(struct TraceIoKit *api)
{
    api->library = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!api->library) {
        return false;
    }

    api->matching = dlsym(api->library, "IOServiceMatching");
    api->service = dlsym(api->library, "IOServiceGetMatchingService");
    api->open = dlsym(api->library, "IOServiceOpen");
    api->close = dlsym(api->library, "IOServiceClose");
    api->release = dlsym(api->library, "IOObjectRelease");
    api->call = dlsym(api->library, "IOConnectCallStructMethod");

    return api->matching && api->service && api->open && api->close && api->release && api->call;
}

static void serve_trace(int socket, const struct TraceIoKit *api)
{
    mach_port_t connection = MACH_PORT_NULL;
    mach_port_t service = api->service(MACH_PORT_NULL, api->matching("NativeAdrenoGpuPublished"));
    if (service) {
        kern_return_t status = api->open(service, mach_task_self(), 0x4148, &connection);
        api->release(service);
        if (status) {
            syslog(LOG_ERR, "GPU trace IOKit open failed: 0x%x", status);
        }
    }

    for (;;) {
        NativeAdrenoTraceTcpHeader request = {0};
        if (!transfer_bytes(socket, &request, sizeof(request), false)) {
            break;
        }
        if (request.magic != NativeAdrenoTraceTcpMagic ||
            request.version != NativeAdrenoTraceVersion || request.status ||
            request.reserved[0] || request.reserved[1]) {
            break;
        }

        size_t input_bytes = 0;
        size_t output_bytes = 0;
        switch (request.method) {
            case NativeAdrenoMethodQueryTrace:
                output_bytes = sizeof(NativeAdrenoTraceStatus);
                break;
            case NativeAdrenoMethodControlTrace:
                input_bytes = sizeof(NativeAdrenoTraceControl);
                output_bytes = sizeof(NativeAdrenoTraceStatus);
                break;
            case NativeAdrenoMethodReadTrace:
                input_bytes = sizeof(NativeAdrenoTraceRead);
                output_bytes = sizeof(NativeAdrenoTraceChunk);
                break;
            default:
                goto finished;
        }
        if (request.payload_bytes != input_bytes) {
            break;
        }

        union {
            NativeAdrenoTraceControl control;
            NativeAdrenoTraceRead read;
        } input = {0};
        union {
            NativeAdrenoTraceStatus status;
            NativeAdrenoTraceChunk chunk;
        } output = {0};
        if (!transfer_bytes(socket, &input, input_bytes, false)) {
            break;
        }

        kern_return_t result = connection ?
            api->call(connection, request.method, input_bytes ? &input : NULL,
                input_bytes, &output, &output_bytes) : (kern_return_t)0xe00002d8;
        if (result && request.method == NativeAdrenoMethodControlTrace && connection) {
            output_bytes = sizeof(output.status);
            if (api->call(connection, NativeAdrenoMethodQueryTrace, NULL, 0,
                &output.status, &output_bytes)) {
                output_bytes = 0;
            }
        } else if (result) {
            output_bytes = 0;
        }

        NativeAdrenoTraceTcpHeader reply = {0};
        reply.magic = NativeAdrenoTraceTcpMagic;
        reply.version = NativeAdrenoTraceVersion;
        reply.method = request.method;
        reply.request_id = request.request_id;
        reply.payload_bytes = (uint32_t)output_bytes;
        reply.status = (uint32_t)result;
        if (!transfer_bytes(socket, &reply, sizeof(reply), true) ||
            !transfer_bytes(socket, &output, output_bytes, true)) {
            break;
        }
    }

finished:
    if (connection) {
        api->close(connection);
    }
}

static void *trace_server(void *unused)
{
    (void)unused;
    struct TraceIoKit api = {0};
    if (!load_iokit(&api)) {
        syslog(LOG_ERR, "GPU trace IOKit entry points unavailable");
        return NULL;
    }

    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        syslog(LOG_ERR, "GPU trace socket failed: %s", strerror(errno));
        return NULL;
    }
    int enabled = 1;
    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(NativeAdrenoTraceTcpPort);
    inet_pton(AF_INET, "192.168.73.2", &address.sin_addr);
    if (fcntl(listener, F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 1) < 0) {
        syslog(LOG_ERR, "GPU trace listener failed: %s", strerror(errno));
        close(listener);
        return NULL;
    }

    struct in_addr allowed_peer;
    inet_pton(AF_INET, "192.168.73.1", &allowed_peer);
    syslog(LOG_NOTICE, "GPU trace TCP ready on 192.168.73.2:%u", NativeAdrenoTraceTcpPort);
    for (;;) {
        struct sockaddr_in peer = {0};
        socklen_t peer_bytes = sizeof(peer);
        int socket = accept(listener, (struct sockaddr *)&peer, &peer_bytes);
        if (socket < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            break;
        }
        if (peer_bytes != sizeof(peer) || peer.sin_family != AF_INET ||
            peer.sin_addr.s_addr != allowed_peer.s_addr) {
            close(socket);
            continue;
        }

        struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
        if (fcntl(socket, F_SETFD, FD_CLOEXEC) == 0 &&
            setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0 &&
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0 &&
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0) {
            serve_trace(socket, &api);
        }
        close(socket);
    }
    close(listener);

    return NULL;
}

void t733_gpu_trace_start(void)
{
    pthread_t thread;
    int result = pthread_create(&thread, NULL, trace_server, NULL);
    if (result) {
        syslog(LOG_ERR, "GPU trace thread failed: %s", strerror(result));
        return;
    }

    pthread_detach(thread);
}
