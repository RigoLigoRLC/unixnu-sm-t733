// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <arpa/inet.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static pid_t gpu_diagnostic_pid;
static unsigned gpu_diagnostic_phase;

static void start_turnip_diagnostic(void);

static void reap_gpu_diagnostic(void)
{
    if (!gpu_diagnostic_pid) {
        return;
    }

    int status = 0;
    pid_t result = waitpid(gpu_diagnostic_pid, &status, WNOHANG);
    if (result == gpu_diagnostic_pid) {
        int start_turnip = gpu_diagnostic_phase == 1 && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0;
        if (WIFEXITED(status)) {
            syslog(LOG_NOTICE, "GPU diagnostic pid=%d exited status=%d",
                gpu_diagnostic_pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            syslog(LOG_ERR, "GPU diagnostic pid=%d terminated signal=%d",
                gpu_diagnostic_pid, WTERMSIG(status));
        }

        gpu_diagnostic_pid = 0;
        gpu_diagnostic_phase = 0;
        if (start_turnip) {
            start_turnip_diagnostic();
        }
    } else if (result < 0 && errno == ECHILD) {
        gpu_diagnostic_pid = 0;
        gpu_diagnostic_phase = 0;
    }
}

static void start_gpu_diagnostic(void)
{
    const char *path = "/private/var/ngpu/bin/t733-adreno";
    if (access(path, X_OK) < 0) {
        return;
    }

    char *arguments[] = {"t733-adreno", "--stage", "submit",
        "--wait-seconds", "120", NULL};
    int error = posix_spawn(&gpu_diagnostic_pid, path, NULL, NULL, arguments, environ);
    if (error) {
        gpu_diagnostic_pid = 0;
        syslog(LOG_ERR, "GPU diagnostic spawn failed: %s", strerror(error));
        return;
    }

    gpu_diagnostic_phase = 1;
    syslog(LOG_NOTICE, "GPU diagnostic pid=%d started independently", gpu_diagnostic_pid);
}

static void start_turnip_diagnostic(void)
{
    const char *path = "/private/var/ngpu/bin/t733-turnip";
    if (access(path, X_OK) < 0) {
        return;
    }

    char *arguments[] = {"t733-turnip", NULL};
    int error = posix_spawn(&gpu_diagnostic_pid, path, NULL, NULL, arguments, environ);
    if (error) {
        gpu_diagnostic_pid = 0;
        syslog(LOG_ERR, "Turnip diagnostic spawn failed: %s", strerror(error));
        return;
    }

    gpu_diagnostic_phase = 2;
    syslog(LOG_NOTICE, "Turnip diagnostic pid=%d started after CP readback succeeded", gpu_diagnostic_pid);
}

/* The launchd helper assigns the USB Ethernet address and enables the normal
 * lockdown network connection setting through its local socket API. It then
 * accepts one TCP connection and replaces itself with debugserver using the fd.
 * This keeps launchd as debugserver's parent,
 * as required by its launch constraint. NativeUsbNetwork owns unet0 and its
 * link state. The cached RunAtLoad job starts one debugging session per boot.
 */

/* LockdownApi contains the exported functions from the system's liblockdown.
 * The opaque connection and its socket remain owned by liblockdown.
 */
struct LockdownApi {
    void *(*connect)(void);
    int (*get_socket)(void *connection);
    int (*set_value)(void *connection, CFStringRef domain, CFStringRef key,
        CFTypeRef value);
    CFTypeRef (*copy_value)(void *connection, CFStringRef domain, CFStringRef key);
    void (*disconnect)(void *connection);
};

static void enable_lockdown_connections(void)
{
    void *library = dlopen("/usr/lib/liblockdown.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        syslog(LOG_ERR, "lockdown library unavailable: %s", dlerror());
        return;
    }

    struct LockdownApi api = {
        .connect = (void *(*)(void))dlsym(library, "lockdown_connect"),
        .get_socket = (int (*)(void *))dlsym(library, "lockdown_get_socket"),
        .set_value = (int (*)(void *, CFStringRef, CFStringRef, CFTypeRef))
            dlsym(library, "lockdown_set_value"),
        .copy_value = (CFTypeRef (*)(void *, CFStringRef, CFStringRef))
            dlsym(library, "lockdown_copy_value"),
        .disconnect = (void (*)(void *))dlsym(library, "lockdown_disconnect"),
    };
    if (!api.connect || !api.get_socket || !api.set_value || !api.copy_value ||
        !api.disconnect) {
        syslog(LOG_ERR, "lockdown required local API export unavailable");
        dlclose(library);
        return;
    }

    CFStringRef domain = CFSTR("com.apple.mobile.wireless_lockdown");
    CFStringRef key = CFSTR("EnableWifiConnections");
    struct timespec started;
    if (clock_gettime(CLOCK_MONOTONIC, &started) < 0) {
        syslog(LOG_ERR, "lockdown retry clock failed: errno=%d", errno);
        dlclose(library);
        return;
    }

    int confirmed = 0;
    unsigned attempts = 0;
    for (unsigned attempt = 1; attempt <= 45; ++attempt) {
        reap_gpu_diagnostic();

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 ||
            now.tv_sec - started.tv_sec >= 90) {
            break;
        }

        attempts = attempt;
        errno = 0;
        void *connection = api.connect();
        int connection_error = connection ? 0 : errno;
        int set_result = -1;
        int set_error = 0;
        int read_error = 0;
        CFTypeID value_type = 0;
        int enabled = -1;
        if (connection) {
            int descriptor = api.get_socket(connection);
            struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
            int no_sigpipe = 1;
            if (descriptor < 0) {
                connection_error = EBADF;
            } else if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO,
                &timeout, sizeof(timeout)) < 0 ||
                setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO,
                &timeout, sizeof(timeout)) < 0 ||
                setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE,
                &no_sigpipe, sizeof(no_sigpipe)) < 0) {
                connection_error = errno;
            }

            if (!connection_error) {
                errno = 0;
                set_result = api.set_value(connection, domain, key, kCFBooleanTrue);
                set_error = errno;

                /* SetValue can return zero without a response Error object.
                 * Only a successfully read Boolean confirms the stored value.
                 */
                errno = 0;
                CFTypeRef value = api.copy_value(connection, domain, key);
                read_error = errno;
                if (value) {
                    value_type = CFGetTypeID(value);
                    if (value_type == CFBooleanGetTypeID()) {
                        enabled = CFBooleanGetValue((CFBooleanRef)value) ? 1 : 0;
                    }
                    CFRelease(value);
                }
                confirmed = set_result == 0 && enabled == 1;
            }
            api.disconnect(connection);
        }

        syslog(confirmed ? LOG_NOTICE : LOG_WARNING,
            "lockdown EnableWifiConnections attempt=%u connection_errno=%d "
            "set_result=%d set_errno=%d read_errno=%d read_type=%lu enabled=%d confirmed=%d",
            attempt, connection_error, set_result, set_error, read_error,
            (unsigned long)value_type, enabled, confirmed);
        if (confirmed) {
            break;
        }
        sleep(2);
    }

    if (!confirmed) {
        syslog(LOG_ERR, "lockdown EnableWifiConnections unconfirmed after %u attempts; "
            "continuing to debug TCP listener", attempts);
    }
    dlclose(library);
}

static int configure_address(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return errno;
    }

    struct ifreq flags = {0};
    strlcpy(flags.ifr_name, "unet0", sizeof(flags.ifr_name));
    if (ioctl(fd, SIOCGIFFLAGS, &flags) < 0) {
        int error = errno;
        close(fd);
        return error;
    }

    struct ifaliasreq address = {0};
    strlcpy(address.ifra_name, "unet0", sizeof(address.ifra_name));
    struct sockaddr_in *ip = (struct sockaddr_in *)&address.ifra_addr;
    struct sockaddr_in *mask = (struct sockaddr_in *)&address.ifra_mask;
    struct sockaddr_in *broadcast = (struct sockaddr_in *)&address.ifra_broadaddr;
    ip->sin_len = mask->sin_len = broadcast->sin_len = sizeof(*ip);
    ip->sin_family = mask->sin_family = broadcast->sin_family = AF_INET;
    inet_pton(AF_INET, "192.168.73.2", &ip->sin_addr);
    inet_pton(AF_INET, "255.255.255.252", &mask->sin_addr);
    inet_pton(AF_INET, "192.168.73.3", &broadcast->sin_addr);
    if (ioctl(fd, SIOCAIFADDR, &address) < 0 && errno != EEXIST) {
        int error = errno;
        close(fd);
        return error;
    }

    flags.ifr_flags |= IFF_UP;
    int error = ioctl(fd, SIOCSIFFLAGS, &flags) < 0 ? errno : 0;
    close(fd);
    return error;
}

static void connect_console(void)
{
    int console = open("/dev/console", O_RDWR);
    if (console < 0) {
        return;
    }

    for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
        if (dup2(console, descriptor) < 0) {
            break;
        }
    }

    if (console > STDERR_FILENO) {
        close(console);
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static int accept_debugger(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        syslog(LOG_ERR, "debug TCP socket failed: errno=%d %s", errno, strerror(errno));
        return -1;
    }

    const char *operation = "SO_REUSEADDR";
    int enabled = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        goto failed;
    }

    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(1234);
    inet_pton(AF_INET, "192.168.73.2", &address.sin_addr);
    operation = "bind 192.168.73.2:1234";
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0) {
        goto failed;
    }

    operation = "listen";
    if (listen(listener, 1) < 0) {
        goto failed;
    }

    syslog(LOG_NOTICE, "debug TCP listening on 192.168.73.2:1234; accepting 192.168.73.1");
    struct in_addr allowed_peer;
    inet_pton(AF_INET, "192.168.73.1", &allowed_peer);
    for (;;) {
        reap_gpu_diagnostic();

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        struct timeval poll_interval = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(listener + 1, &readable, NULL, NULL, &poll_interval);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }

        if (ready < 0) {
            operation = "select";
            goto failed;
        }

        struct sockaddr_in peer = {0};
        socklen_t peer_length = sizeof(peer);
        int connection = accept(listener, (struct sockaddr *)&peer, &peer_length);
        if (connection < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }

            operation = "accept";
            goto failed;
        }

        if (peer_length != sizeof(peer) || peer.sin_family != AF_INET ||
            peer.sin_addr.s_addr != allowed_peer.s_addr) {
            syslog(LOG_WARNING, "debug TCP rejected peer outside 192.168.73.1");
            close(connection);
            continue;
        }

        int descriptor_flags = fcntl(connection, F_GETFD);
        int error = descriptor_flags < 0 ? errno : 0;
        if (!error && fcntl(connection, F_SETFD, descriptor_flags & ~FD_CLOEXEC) < 0) {
            error = errno;
        }

        if (!error && setsockopt(connection, IPPROTO_TCP, TCP_NODELAY,
            &enabled, sizeof(enabled)) < 0) {
            error = errno;
        }

        if (error) {
            syslog(LOG_ERR, "debug TCP connected socket setup failed: errno=%d %s",
                error, strerror(error));
            close(connection);
            close(listener);
            return -1;
        }

        close(listener);
        return connection;
    }

failed:
    {
        int error = errno;
        syslog(LOG_ERR, "debug TCP %s failed: errno=%d %s", operation, error,
            strerror(error));
        close(listener);
        return -1;
    }
}

int main(void)
{
    connect_console();
    openlog("T733DebugNetwork", LOG_PID | LOG_PERROR, LOG_DAEMON);

    int error = ENXIO;
    for (unsigned attempt = 0; attempt < 60; ++attempt) {
        error = configure_address();
        if (!error) {
            break;
        }
        sleep(1);
    }

    if (error) {
        syslog(LOG_ERR, "unet0 address configuration failed: %s", strerror(error));
        return 1;
    }

    int developer_mode = 0;
    size_t developer_mode_size = sizeof(developer_mode);
    int developer_error = 0;
    if (sysctlbyname("security.mac.amfi.developer_mode_status", &developer_mode,
        &developer_mode_size, NULL, 0) < 0) {
        developer_error = errno;
    }

    syslog(LOG_NOTICE, "unet0 192.168.73.2/30 ready; developer mode=%d query error=%d console=%d",
        developer_mode, developer_error, isatty(STDIN_FILENO));
    start_gpu_diagnostic();
    enable_lockdown_connections();
    int connection = accept_debugger();
    if (connection < 0) {
        return 1;
    }

    char descriptor_argument[32];
    snprintf(descriptor_argument, sizeof(descriptor_argument), "--fd=%d", connection);
    syslog(LOG_NOTICE, "starting development-signed debugserver with connected fd=%d", connection);
    char *arguments[] = {"debugserver", "--log-file=stderr", "--log-flags=0x80000",
        descriptor_argument, NULL};
    reap_gpu_diagnostic();
    if (gpu_diagnostic_pid) {
        // The inherited ignored disposition lets the kernel reap a diagnostic
        // that finishes after this process becomes debugserver.
        signal(SIGCHLD, SIG_IGN);
    }

    execv("/private/var/ngpu/bin/t733-debugserver", arguments);
    syslog(LOG_ERR, "debugserver exec failed: %s", strerror(errno));
    close(connection);
    return 1;
}
