// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <arpa/inet.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <notify.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/sockio.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static pid_t gpu_diagnostic_pid;
static pid_t tools_pid;
static volatile sig_atomic_t stopping;
static volatile sig_atomic_t debugger_pid;
static const char *display_policy_state = "not-requested";

/* The display policy version belongs to the installed addon. A changed version
 * removes only MobileGestalt's persisted answers. The boot timestamp records
 * whether consumers still need a restart to discard their in-memory answers.
 */
static void refresh_display_policy_cache(void)
{
    static const char version[] = "21H461-srgb-bgra8-1";
    static const char state_path[] = "/private/var/db/t733-display-policy";
    FILE *request = fopen("/private/var/ngpu/addons/NativeAdrenoMetal.display-policy", "r");
    if (!request) {
        return;
    }

    char requested_version[64] = {0};
    char *line = fgets(requested_version, sizeof(requested_version), request);
    fclose(request);
    requested_version[strcspn(requested_version, "\r\n")] = 0;
    if (!line || strcmp(requested_version, version) != 0) {
        display_policy_state = "unsupported-version";
        syslog(LOG_ERR, "display policy addon version is unsupported");
        return;
    }

    struct timeval boot = {0};
    size_t boot_size = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &boot_size, NULL, 0) < 0) {
        display_policy_state = "boot-time-error";
        syslog(LOG_ERR, "display policy boot time failed: %s", strerror(errno));
        return;
    }

    int previous = open(state_path, O_RDONLY | O_NOFOLLOW);
    char previous_state[128] = {0};
    if (previous >= 0) {
        ssize_t count = read(previous, previous_state, sizeof(previous_state) - 1);
        close(previous);
        if (count < 0) {
            previous_state[0] = 0;
        }
    }

    char previous_version[64] = {0};
    long long previous_boot = 0;
    if (sscanf(previous_state, "%63s %lld", previous_version, &previous_boot) == 2 &&
        strcmp(previous_version, version) == 0) {
        display_policy_state = previous_boot == (long long)boot.tv_sec ?
            "awaiting-restart" : "cache-refreshed";
        syslog(LOG_NOTICE, "display policy %s cache state=%s", version, display_policy_state);
        return;
    }

    static const char *const cache_paths[] = {
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist",
        "/private/var/mobile/Library/Preferences/com.apple.MobileGestalt.plist",
        "/private/var/mobile/Library/Preferences/com.apple.mobilegestalt.plist",
        "/private/var/mobile/Library/Caches/com.apple.MobileGestalt.plist",
    };
    unsigned removed = 0;
    for (size_t index = 0; index < sizeof(cache_paths) / sizeof(cache_paths[0]); ++index) {
        if (unlink(cache_paths[index]) == 0) {
            ++removed;
        } else if (errno != ENOENT) {
            display_policy_state = "cache-removal-error";
            syslog(LOG_ERR, "display policy cache removal failed for %s: %s",
                cache_paths[index], strerror(errno));
            return;
        }
    }

    char temporary[160];
    snprintf(temporary, sizeof(temporary), "%s.%d", state_path, getpid());
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        display_policy_state = "version-write-error";
        syslog(LOG_ERR, "display policy version open failed: %s", strerror(errno));
        return;
    }

    char state[128];
    int length = snprintf(state, sizeof(state), "%s %lld\n", version, (long long)boot.tv_sec);
    size_t written = 0;
    while (written < (size_t)length) {
        ssize_t count = write(descriptor, state + written, (size_t)length - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        written += (size_t)count;
    }

    int flush_error = fsync(descriptor);
    close(descriptor);
    if (written != (size_t)length || flush_error < 0 || rename(temporary, state_path) < 0) {
        unlink(temporary);
        display_policy_state = "version-write-error";
        syslog(LOG_ERR, "display policy version save failed: %s", strerror(errno));
        return;
    }

    display_policy_state = "awaiting-restart";
    syslog(LOG_NOTICE, "display policy %s removed %u MobileGestalt caches; restart required for existing consumers",
        version, removed);
}

static void publish_debug_status(const char *state)
{
    const char *path = "/private/var/run/t733-debug-network.json";
    char temporary[128];
    snprintf(temporary, sizeof(temporary), "%s.%d", path, getpid());
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (descriptor < 0) {
        syslog(LOG_ERR, "debug status file open failed: %s", strerror(errno));
        return;
    }

    char content[320];
    int length = snprintf(content, sizeof(content),
        "{\"state\":\"%s\",\"supervisor_pid\":%d,\"debugger_pid\":%d,"
        "\"address\":\"192.168.73.2\",\"port\":1234,\"display_policy\":\"%s\"}\n",
        state, getpid(), (int)debugger_pid, display_policy_state);
    size_t written = 0;
    while (written < (size_t)length) {
        ssize_t count = write(descriptor, content + written, (size_t)length - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        written += (size_t)count;
    }

    fchmod(descriptor, 0644);
    close(descriptor);
    if (written != (size_t)length || rename(temporary, path) < 0) {
        unlink(temporary);
    }
}

static void reap_background_process(pid_t *child, const char *name)
{
    if (!*child) {
        return;
    }

    int status = 0;
    pid_t result = waitpid(*child, &status, WNOHANG);
    if (result == *child) {
        if (WIFEXITED(status)) {
            syslog(LOG_NOTICE, "%s pid=%d exited status=%d",
                name, *child, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            syslog(LOG_ERR, "%s pid=%d terminated signal=%d",
                name, *child, WTERMSIG(status));
        }

        *child = 0;
    } else if (result < 0 && errno == ECHILD) {
        *child = 0;
    }
}

static void reap_background_processes(void)
{
    reap_background_process(&gpu_diagnostic_pid, "GPU diagnostic");
    reap_background_process(&tools_pid, "T733 Tools startup");
}

static void stop_service(int signal_number)
{
    (void)signal_number;
    stopping = 1;
    if (debugger_pid > 0) {
        kill((pid_t)debugger_pid, SIGTERM);
    }
}

static void start_tools(void)
{
    const char *path = "/private/var/ngpu/apps/T733Tools.app/T733Tools";
    char *arguments[] = {"T733Tools", "--boot-apply", NULL};
    int error = posix_spawn(&tools_pid, path, NULL, NULL, arguments, environ);
    if (error) {
        tools_pid = 0;
        syslog(LOG_ERR, "T733 Tools startup failed: %s", strerror(error));
    }
}

static void start_gpu_diagnostic(void)
{
    const char *path = "/private/var/ngpu/bin/t733-adreno";
    if (access(path, X_OK) < 0) {
        return;
    }

    char *arguments[] = {"t733-adreno", "--stage", "query",
        "--wait-seconds", "120", NULL};
    int error = posix_spawn(&gpu_diagnostic_pid, path, NULL, NULL, arguments, environ);
    if (error) {
        gpu_diagnostic_pid = 0;
        syslog(LOG_ERR, "GPU diagnostic spawn failed: %s", strerror(error));
        return;
    }

    syslog(LOG_NOTICE, "GPU diagnostic pid=%d started independently", gpu_diagnostic_pid);
}

/* The launchd helper assigns the USB Ethernet address and enables the normal
 * lockdown network connection setting through its local socket API. After
 * confirming that setting, it posts the native Bonjour pairing service event.
 * It owns the listening socket and launches one native debugserver per accepted
 * connection. A session's exit returns to accept without repeating hardware
 * initialization. The helper retains its launchd parent; the development-signed
 * debugger uses its own category-0 trust entry. NativeUsbNetwork owns unet0 and
 * its link state. The cached RunAtLoad job starts this supervisor once per boot.
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
        reap_background_processes();
        if (stopping) {
            break;
        }

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

    if (confirmed && !stopping) {
        const char *notification = "com.apple.mobile.lockdown.BonjourPairingServiceChanged";
        uint32_t result = notify_post(notification);
        syslog(result == NOTIFY_STATUS_OK ? LOG_NOTICE : LOG_ERR,
            "lockdown pairing discovery notification=%s result=%u", notification, result);
    }
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

static int create_debug_listener(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        syslog(LOG_ERR, "debug TCP socket failed: errno=%d %s", errno, strerror(errno));
        return -1;
    }

    const char *operation = "FD_CLOEXEC";
    if (fcntl(listener, F_SETFD, FD_CLOEXEC) < 0) {
        goto failed;
    }

    operation = "SO_REUSEADDR";
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

    return listener;

failed:
    {
        int error = errno;
        syslog(LOG_ERR, "debug TCP %s failed: errno=%d %s", operation, error,
            strerror(error));
        close(listener);
        return -1;
    }
}

static int accept_debugger(int listener)
{
    publish_debug_status("listening");
    syslog(LOG_NOTICE, "debug TCP ready on 192.168.73.2:1234; accepting 192.168.73.1");
    struct in_addr allowed_peer;
    inet_pton(AF_INET, "192.168.73.1", &allowed_peer);
    while (!stopping) {
        reap_background_processes();

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        struct timeval poll_interval = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(listener + 1, &readable, NULL, NULL, &poll_interval);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }

        if (ready < 0) {
            syslog(LOG_ERR, "debug TCP select failed: %s", strerror(errno));
            return -1;
        }

        struct sockaddr_in peer = {0};
        socklen_t peer_length = sizeof(peer);
        int connection = accept(listener, (struct sockaddr *)&peer, &peer_length);
        if (connection < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }

            syslog(LOG_ERR, "debug TCP accept failed: %s", strerror(errno));
            return -1;
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

        int enabled = 1;
        if (!error && setsockopt(connection, IPPROTO_TCP, TCP_NODELAY,
            &enabled, sizeof(enabled)) < 0) {
            error = errno;
        }

        if (!error && setsockopt(connection, SOL_SOCKET, SO_KEEPALIVE,
            &enabled, sizeof(enabled)) < 0) {
            error = errno;
        }

        int idle_seconds = 10;
        int probe_seconds = 3;
        int probe_count = 3;
        if (!error && (setsockopt(connection, IPPROTO_TCP, TCP_KEEPALIVE,
            &idle_seconds, sizeof(idle_seconds)) < 0 ||
            setsockopt(connection, IPPROTO_TCP, TCP_KEEPINTVL,
            &probe_seconds, sizeof(probe_seconds)) < 0 ||
            setsockopt(connection, IPPROTO_TCP, TCP_KEEPCNT,
            &probe_count, sizeof(probe_count)) < 0)) {
            error = errno;
        }

        if (error) {
            syslog(LOG_ERR, "debug TCP connected socket setup failed: errno=%d %s",
                error, strerror(error));
            close(connection);
            continue;
        }

        return connection;
    }

    return -1;
}

static void run_debug_session(int connection)
{
    char descriptor_argument[32];
    snprintf(descriptor_argument, sizeof(descriptor_argument), "--fd=%d", connection);
    char *arguments[] = {"debugserver", "--log-file=stderr", "--log-flags=0x80000",
        descriptor_argument, NULL};

    pid_t child = 0;
    int error = posix_spawn(&child, "/private/var/ngpu/bin/t733-debugserver",
        NULL, NULL, arguments, environ);
    close(connection);
    if (error) {
        syslog(LOG_ERR, "debugserver spawn failed: %s", strerror(error));
        return;
    }

    debugger_pid = child;
    publish_debug_status("session");
    syslog(LOG_NOTICE, "debug session pid=%d started", child);
    if (stopping) {
        kill(child, SIGTERM);
    }

    int status = 0;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);

    debugger_pid = 0;
    if (result < 0) {
        syslog(LOG_ERR, "debug session pid=%d wait failed: %s", child, strerror(errno));
    } else if (WIFEXITED(status)) {
        syslog(LOG_NOTICE, "debug session pid=%d exited status=%d; listener retained",
            child, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        syslog(LOG_WARNING, "debug session pid=%d terminated signal=%d; listener retained",
            child, WTERMSIG(status));
    }
}

int main(void)
{
    connect_console();
    openlog("T733DebugNetwork", LOG_PID | LOG_PERROR, LOG_DAEMON);
    refresh_display_policy_cache();
    publish_debug_status("starting");
    struct sigaction action = {0};
    action.sa_handler = stop_service;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

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
    start_tools();
    enable_lockdown_connections();
    while (!stopping) {
        int listener = create_debug_listener();
        if (listener < 0) {
            sleep(1);
            continue;
        }

        while (!stopping) {
            int connection = accept_debugger(listener);
            if (connection < 0) {
                break;
            }
            run_debug_session(connection);
        }
        close(listener);
    }

    publish_debug_status("stopped");
    return 0;
}
