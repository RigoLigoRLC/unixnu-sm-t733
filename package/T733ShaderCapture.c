// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733ShaderCapture.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

static bool transfer_bytes(int connection, void *bytes, size_t count, bool sending)
{
    size_t completed = 0;
    while (completed < count) {
        ssize_t result = sending ?
            send(connection, (char *)bytes + completed, count - completed, 0) :
            recv(connection, (char *)bytes + completed, count - completed, 0);
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

static bool send_reply(int connection, const T733ShaderHeader *request,
    uint32_t status, void *payload, uint32_t payload_bytes)
{
    T733ShaderHeader reply = {
        .magic = T733ShaderMagic,
        .version = T733ShaderVersion,
        .operation = request->operation,
        .request_id = request->request_id,
        .status = status,
        .payload_bytes = payload_bytes,
    };
    if (!transfer_bytes(connection, &reply, sizeof(reply), true)) {
        return false;
    }

    return !payload_bytes || transfer_bytes(connection, payload, payload_bytes, true);
}

static uint32_t process_number(const char *name)
{
    if (*name < '1' || *name > '9') {
        return 0;
    }

    uint64_t number = 0;
    for (size_t index = 0; name[index]; index++) {
        if (index >= 10 || name[index] < '0' || name[index] > '9') {
            return 0;
        }
        number = number * 10 + (unsigned)(name[index] - '0');
        if (number > INT_MAX) {
            return 0;
        }
    }

    return (uint32_t)number;
}

static bool capture_filename(const char *name)
{
    size_t length = strnlen(name, T733ShaderNameBytes);
    if (!length || length >= T733ShaderNameBytes || name[0] != 's') {
        return false;
    }

    size_t index = 1;
    while (index < length && name[index] >= '0' && name[index] <= '9') {
        index++;
    }
    if (index == 1 || index >= length || name[index] != '_') {
        return false;
    }

    index++;
    if (index >= length || name[index] == '.') {
        return false;
    }

    const char *extension = NULL;
    for (; index < length; index++) {
        char byte = name[index];
        if (byte == '.') {
            extension = name + index + 1;
            break;
        }
        if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
              byte == '_' || byte == '-')) {
            return false;
        }
    }
    if (!extension) {
        return false;
    }

    const char *allowed[] = {"json", "txt", "metallib", "air", "spv", "rd"};
    for (size_t suffix = 0; suffix < sizeof(allowed) / sizeof(allowed[0]); suffix++) {
        if (!strcmp(extension, allowed[suffix])) {
            return true;
        }
    }

    return false;
}

static uint64_t capture_session(const char *name)
{
    if (!capture_filename(name) || name[1] == '0') {
        return 0;
    }

    uint64_t session = 0;
    for (size_t index = 1; name[index] != '_'; index++) {
        unsigned digit = (unsigned)(name[index] - '0');
        if (session > (UINT64_MAX - digit) / 10) {
            return 0;
        }
        session = session * 10 + digit;
    }

    return session;
}

static bool describe_file(const struct stat *status, T733ShaderFile *file)
{
    if (!S_ISREG(status->st_mode) || status->st_nlink != 1 || status->st_size < 0 ||
        status->st_size > T733ShaderMaxFileBytes || status->st_mtimespec.tv_sec < 0) {
        return false;
    }

    file->bytes = (uint64_t)status->st_size;
    file->modified_ns = (uint64_t)status->st_mtimespec.tv_sec * 1000000000ull +
        (uint64_t)status->st_mtimespec.tv_nsec;
    return true;
}

static int open_process(int root, uint32_t process_id)
{
    if (!process_id || process_id > INT_MAX) {
        errno = EINVAL;
        return -1;
    }

    char name[16];
    snprintf(name, sizeof(name), "%u", process_id);
    return openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static bool list_processes(int connection, int root, const T733ShaderHeader *request)
{
    int directory = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *stream = directory < 0 ? NULL : fdopendir(directory);
    if (!stream) {
        int error = errno;
        if (directory >= 0) {
            close(directory);
        }
        return send_reply(connection, request, (uint32_t)error, NULL, 0);
    }

    uint32_t processes[T733ShaderMaxProcesses];
    uint32_t count = 0;
    uint32_t error = 0;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        uint32_t process = process_number(entry->d_name);
        if (!process) {
            continue;
        }
        int child = open_process(root, process);
        if (child < 0) {
            continue;
        }
        close(child);
        if (count == T733ShaderMaxProcesses) {
            error = EOVERFLOW;
            break;
        }
        processes[count++] = process;
    }
    closedir(stream);

    return send_reply(connection, request, error, processes,
        error ? 0 : count * (uint32_t)sizeof(processes[0]));
}

static bool list_sessions(int connection, int root, const T733ShaderHeader *request,
    uint32_t process_id)
{
    int directory = open_process(root, process_id);
    DIR *stream = directory < 0 ? NULL : fdopendir(directory);
    if (!stream) {
        int error = errno;
        if (directory >= 0) {
            close(directory);
        }
        return send_reply(connection, request, (uint32_t)error, NULL, 0);
    }

    uint64_t sessions[T733ShaderMaxSessions];
    uint32_t count = 0;
    uint32_t error = 0;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        uint64_t session = capture_session(entry->d_name);
        if (!session) {
            continue;
        }

        struct stat status;
        T733ShaderFile file = {0};
        if (fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) < 0 ||
            !describe_file(&status, &file)) {
            continue;
        }

        uint32_t index = 0;
        while (index < count && sessions[index] != session) {
            index++;
        }
        if (index < count) {
            continue;
        }
        if (count == T733ShaderMaxSessions) {
            error = EOVERFLOW;
            break;
        }

        sessions[count++] = session;
    }
    closedir(stream);

    return send_reply(connection, request, error, sessions,
        error ? 0 : count * (uint32_t)sizeof(sessions[0]));
}

static bool list_files(int connection, int root, const T733ShaderHeader *request,
    const T733ShaderSession *selection)
{
    if (selection->reserved || !selection->session) {
        return send_reply(connection, request, EINVAL, NULL, 0);
    }

    int directory = open_process(root, selection->process_id);
    DIR *stream = directory < 0 ? NULL : fdopendir(directory);
    if (!stream) {
        int error = errno;
        if (directory >= 0) {
            close(directory);
        }
        return send_reply(connection, request, (uint32_t)error, NULL, 0);
    }

    T733ShaderFile *files = calloc(T733ShaderMaxFiles, sizeof(*files));
    if (!files) {
        closedir(stream);
        return send_reply(connection, request, ENOMEM, NULL, 0);
    }

    uint32_t count = 0;
    uint64_t bytes = 0;
    uint32_t error = 0;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        if (capture_session(entry->d_name) != selection->session) {
            continue;
        }
        struct stat status;
        T733ShaderFile file = {0};
        if (fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) < 0 ||
            !describe_file(&status, &file)) {
            continue;
        }
        if (count == T733ShaderMaxFiles || bytes + file.bytes > T733ShaderMaxSessionBytes) {
            error = EOVERFLOW;
            break;
        }

        memcpy(file.name, entry->d_name, strlen(entry->d_name) + 1);
        files[count++] = file;
        bytes += file.bytes;
    }
    closedir(stream);

    bool sent = send_reply(connection, request, error, files,
        error ? 0 : count * (uint32_t)sizeof(*files));
    free(files);
    return sent;
}

static bool read_file(int connection, int root, const T733ShaderHeader *request,
    const T733ShaderRead *read_request)
{
    if (read_request->reserved || !capture_session(read_request->file.name) ||
        read_request->file.bytes > T733ShaderMaxFileBytes) {
        return send_reply(connection, request, EINVAL, NULL, 0);
    }

    int directory = open_process(root, read_request->process_id);
    int file = directory < 0 ? -1 : openat(directory, read_request->file.name,
        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int error = errno;
    if (directory >= 0) {
        close(directory);
    }
    if (file < 0) {
        return send_reply(connection, request, (uint32_t)error, NULL, 0);
    }

    struct stat before;
    T733ShaderFile descriptor = {0};
    if (fstat(file, &before) < 0 || !describe_file(&before, &descriptor) ||
        descriptor.bytes != read_request->file.bytes ||
        descriptor.modified_ns != read_request->file.modified_ns) {
        close(file);
        return send_reply(connection, request, EAGAIN, NULL, 0);
    }

    memcpy(descriptor.name, read_request->file.name, sizeof(descriptor.name));
    bool sent = send_reply(connection, request, 0, &descriptor, sizeof(descriptor));
    unsigned char buffer[T733ShaderChunkBytes];
    uint64_t completed = 0;
    while (sent && completed < descriptor.bytes) {
        size_t count = (size_t)(descriptor.bytes - completed);
        if (count > sizeof(buffer)) {
            count = sizeof(buffer);
        }
        ssize_t got = pread(file, buffer, count, (off_t)completed);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            sent = false;
            break;
        }

        sent = transfer_bytes(connection, buffer, (size_t)got, true);
        completed += (uint64_t)got;
    }

    struct stat after;
    T733ShaderFile current = {0};
    bool unchanged = fstat(file, &after) == 0 && describe_file(&after, &current) &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        descriptor.bytes == current.bytes && descriptor.modified_ns == current.modified_ns;
    close(file);

    return sent && send_reply(connection, request, unchanged ? 0 : EAGAIN, NULL, 0);
}

static void serve_capture(int connection, int root)
{
    for (;;) {
        T733ShaderHeader request = {0};
        if (!transfer_bytes(connection, &request, sizeof(request), false) ||
            request.magic != T733ShaderMagic || request.version != T733ShaderVersion ||
            request.status || request.reserved[0] || request.reserved[1]) {
            return;
        }

        bool sent;
        if (request.operation == T733ShaderListProcesses && request.payload_bytes == 0) {
            sent = list_processes(connection, root, &request);
        } else if (request.operation == T733ShaderListSessions && request.payload_bytes == sizeof(uint32_t)) {
            uint32_t process_id;
            if (!transfer_bytes(connection, &process_id, sizeof(process_id), false)) {
                return;
            }
            sent = list_sessions(connection, root, &request, process_id);
        } else if (request.operation == T733ShaderListFiles && request.payload_bytes == sizeof(T733ShaderSession)) {
            T733ShaderSession selection;
            if (!transfer_bytes(connection, &selection, sizeof(selection), false)) {
                return;
            }
            sent = list_files(connection, root, &request, &selection);
        } else if (request.operation == T733ShaderReadFile && request.payload_bytes == sizeof(T733ShaderRead)) {
            T733ShaderRead read_request;
            if (!transfer_bytes(connection, &read_request, sizeof(read_request), false)) {
                return;
            }
            sent = read_file(connection, root, &request, &read_request);
        } else {
            return;
        }
        if (!sent) {
            return;
        }
    }
}

static void *capture_server(void *unused)
{
    (void)unused;
    if (mkdir(T733_SHADER_CAPTURE_ROOT, 0777) < 0 && errno != EEXIST) {
        syslog(LOG_ERR, "Shader capture directory failed: %s", strerror(errno));
        return NULL;
    }
    int root = open(T733_SHADER_CAPTURE_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) {
        return NULL;
    }
    struct passwd *mobile = getpwnam("mobile");
    if (!mobile || fchown(root, mobile->pw_uid, mobile->pw_gid) < 0 || fchmod(root, 0777) < 0) {
        syslog(LOG_ERR, "Shader capture directory ownership failed: %s", strerror(errno));
        close(root);
        return NULL;
    }

    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int enabled = 1;
    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(T733ShaderPort);
    inet_pton(AF_INET, "192.168.73.2", &address.sin_addr);
    if (listener < 0 || fcntl(listener, F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 1) < 0) {
        syslog(LOG_ERR, "Shader capture listener failed: %s", strerror(errno));
        if (listener >= 0) {
            close(listener);
        }
        close(root);
        return NULL;
    }

    struct in_addr allowed_peer;
    inet_pton(AF_INET, "192.168.73.1", &allowed_peer);
    syslog(LOG_NOTICE, "Shader capture TCP ready on 192.168.73.2:%u", T733ShaderPort);
    for (;;) {
        struct sockaddr_in peer = {0};
        socklen_t peer_bytes = sizeof(peer);
        int connection = accept(listener, (struct sockaddr *)&peer, &peer_bytes);
        if (connection < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            break;
        }
        if (peer_bytes != sizeof(peer) || peer.sin_family != AF_INET ||
            peer.sin_addr.s_addr != allowed_peer.s_addr) {
            close(connection);
            continue;
        }

        struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
        if (fcntl(connection, F_SETFD, FD_CLOEXEC) == 0 &&
            setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0 &&
            setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0 &&
            setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
            setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0) {
            serve_capture(connection, root);
        }
        close(connection);
    }
    close(listener);
    close(root);

    return NULL;
}

void t733_shader_capture_start(void)
{
    pthread_t thread;
    int result = pthread_create(&thread, NULL, capture_server, NULL);
    if (result) {
        syslog(LOG_ERR, "Shader capture thread failed: %s", strerror(result));
        return;
    }

    pthread_detach(thread);
}
