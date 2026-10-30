// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T733_SHADER_CAPTURE_H
#define T733_SHADER_CAPTURE_H

#include <stdint.h>

#define T733_SHADER_CAPTURE_ROOT "/private/var/mobile/Library/Caches/NativeAdrenoShaderCapture"

enum {
    T733ShaderMagic = 0x52444853,
    T733ShaderVersion = 1,
    T733ShaderPort = 1236,
    T733ShaderListProcesses = 1,
    T733ShaderListFiles = 2,
    T733ShaderReadFile = 3,
    T733ShaderListSessions = 4,
    T733ShaderNameBytes = 128,
    T733ShaderMaxProcesses = 256,
    T733ShaderMaxSessions = 256,
    T733ShaderMaxFiles = 4100,
    T733ShaderMaxFileBytes = 16 * 1024 * 1024,
    T733ShaderMaxSessionBytes = 144 * 1024 * 1024,
    T733ShaderChunkBytes = 64 * 1024,
};

/* T733ShaderHeader describes one request or reply in little-endian words.
 * A file reply carries its descriptor, exactly descriptor.bytes file bytes,
 * then another header with the final status and no payload. */
typedef struct T733ShaderHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t operation;
    uint32_t request_id;
    uint32_t status;
    uint32_t payload_bytes;
    uint32_t reserved[2];
} T733ShaderHeader;

/* T733ShaderFile describes a completed capture file inside one numeric PID directory. */
typedef struct T733ShaderFile {
    uint64_t bytes;
    uint64_t modified_ns;
    char name[T733ShaderNameBytes];
} T733ShaderFile;

/* T733ShaderSession selects one capture lifetime within a numeric PID directory. */
typedef struct T733ShaderSession {
    uint32_t process_id;
    uint32_t reserved;
    uint64_t session;
} T733ShaderSession;

/* T733ShaderRead names the exact file version previously returned by the directory listing. */
typedef struct T733ShaderRead {
    uint32_t process_id;
    uint32_t reserved;
    T733ShaderFile file;
} T733ShaderRead;

void t733_shader_capture_start(void);

#endif
