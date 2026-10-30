# GPU capture file download

The debug addon contains `NativeAdrenoMetal.shadercapture`. Metal device creation
uses this marker to enable bounded shader capture. The compiler writes AIR,
SPIR-V, NIR, A6xx ISA and their metadata under:

```text
/private/var/mobile/Library/Caches/NativeAdrenoShaderCapture/<pid>/s<session>_<name>.<extension>
```

The decimal session is the process capture start time from `CLOCK_MONOTONIC` in
nanoseconds. It distinguishes files after a PID is reused. Completed files appear
through an atomic rename. The root directory has mobile ownership and mode 0777;
each producer owns its PID directory with mode 0700.

The helper listens on USB address `192.168.73.2:1236` and accepts
`192.168.73.1`. It opens numeric PID directories through the fixed root directory
descriptor, rejects symbolic links and hard-linked files, and accepts only
`s[1-9][0-9]*_[a-z0-9_-]+.(json|txt|metallib|air|spv|rd)` names.

From the device source repository:

```sh
python3 tools/gpu/shader-capture.py --list
python3 tools/gpu/shader-capture.py --pid 56 --session SESSION --output /absolute/path/new-capture
```

Replace `SESSION` with a decimal session shown by `--list`. Omitting `--pid` or
`--session` downloads all listed processes or sessions. The output directory must
be new. It contains each original file under its numeric PID and a
`download-manifest.json` with remote size, modification time, session and local
path. Interrupted downloads retain a manifest with `complete: false`; a file
becomes visible under its final local name only after the server's final status.

The server limits each file to 16 MiB and transfers it in 64 KiB chunks. Each
session listing permits 4100 files and 145 MiB, including the shader producer's
128 MiB/4096-file budget, one 16 MiB PM4 export and 1 MiB for request/status metadata.
Directory enumeration returns at most
256 processes and 256 sessions per process. Old sessions remain readable.

The wire protocol uses little-endian integers. Each request and reply starts with
eight `uint32_t` words: magic `0x52444853`, version `1`, operation, request ID,
POSIX error status, payload bytes, zero, zero. Requests have status zero.

| Operation | Request | Reply payload |
| --- | --- | --- |
| 1: list processes | Empty | Array of `uint32_t` PIDs |
| 2: list files | PID `uint32_t`, zero `uint32_t`, session `uint64_t` | Array of file descriptors |
| 3: read file | PID `uint32_t`, zero `uint32_t`, file descriptor | Same descriptor, then exactly its file bytes, then a final header with no payload |
| 4: list sessions | PID `uint32_t` | Array of `uint64_t` sessions |
| 5: request one PM4 submission | PID `uint32_t`, zero `uint32_t`, session `uint64_t`, submission `uint64_t` | Empty |

A file descriptor is size `uint64_t`, modification time in nanoseconds
`uint64_t`, and a NUL-terminated, zero-padded 128-byte ASCII filename. The read
request names the exact version from the directory listing. The helper checks
size and modification time before reading, then checks them and inode identity
after streaming. Its final status is `EAGAIN` if the open file changed. The host
refreshes that file's descriptor and retries at most three times, allowing an
active PM4 status file to be downloaded while the worker publishes its state.

The transport performs filesystem work in its own helper thread. Shader
compilation owns file production; command recording and GPU submission continue
independently of downloads.

## One PM4 submission

The PM4 producer publishes `s<session>_pm4_status.json` with its current state and
`last_submit`. The submission number is a process-wide PM4 serial. The result
metadata separately names the Mesa submission, native completion and command
recording IDs.

After startup, wait for shader export creation to settle. Begin a 60-second trace
for the same process, publish the PM4 request during that trace window, and
download shader and PM4 files after the window finishes. The PM4 result's native
completion and command recording IDs then refer to records in that saved trace.

```sh
python3 tools/gpu/shader-capture.py --pid 56 --session SESSION --trigger-pm4 0
```

Zero requests the next nonempty submission after the producer arms the capture.
A nonzero value selects a future process-wide PM4 serial. The helper requires an
existing status file for the same PID/session and atomically publishes exactly:

```text
1 <session> <target_submit>
```

The only writable request name is `s<session>_pm4_request.txt`. A temporary file
is linked into that name after all bytes are written; an existing request causes
`EEXIST`. The producer retains the request and accepts only its own current
session. The server's success reply confirms request publication. Download the
status and the resulting `s<session>_pm4-<serial>.rd` and `.json` files through the
same file download operation after capture completes.
