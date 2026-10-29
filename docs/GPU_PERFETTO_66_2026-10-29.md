# A642L Perfetto capture

This capture connects Metal workload metadata, Turnip GPU timestamps and the
Adreno6xx hardware counter window. `system-gpu-perfetto-66c` completed all nine
build stages, was written through MSC and booted. Its first five-second capture
contains hardware measurements and exposed producer accounting defects. The
66c capture is incomplete. The corrected 66e package completed all nine build
stages, FAT deployment and a second capture. Pairing and timestamp validity
passed; the five-second event window overflowed. Streaming is implemented for
[67](GPU_PERFETTO_STREAM_67_2026-10-30.md).

## Records and ownership

Metal records command buffer and encoder identities, render pass dimensions,
attachment actions, texture bindings, pipeline identities and draw arguments.
Turnip assigns an identity at each Vulkan command buffer begin, records secondary
execution and associates submitted command buffers with the global completion
receipt. Multiple Metal command buffers can belong to one Vulkan submission.

Mesa `u_trace` supplies timestamps written by the GPU. Its worker waits for the
submission fence before reading the timestamp buffers. The selected points cover
render passes, binning, draw command streams, compute, GMEM transfers and cache
commands. Timestamp and timestamp-copy command sizes are part of the capture.

Adreno6xx retains the 64a hardware access protocol: CPU time reads use GMU AO,
counter access holds the performance OOB request until pending GPU jobs retire,
and the worker stores 29 counter values with validity bits. Each counter read
also stores enclosing CPU monotonic times to align GPU stages with Metal events.

`NativeAdrenoTrace.h` defines a 176-byte event, eight-event append batches, 4096
counter samples, 4096 jobs and 131072 user events. Each producer registers with the
capture generation and ends only after submitted GPU work, its timestamps and
metadata have drained. Stopping closes admission of new submissions; recorded
commands not yet submitted are counted separately. Sealing requires hardware retirement,
released OOB ownership and no remaining producers. Overflow and interrupted
producers remain explicit in the saved status.

The debug network helper serves the binary API on `192.168.73.2:1235` for its USB
host `192.168.73.1`. The host downloads a sealed capture in bounded chunks. The
USB console continues receiving boot and fault logs independently.

## Host commands

From the workspace root, install the host dependency into the existing virtual
environment:

```sh
.venv/bin/python -m pip install --cache-dir downloads/pip-cache \
  -r sources/UniXNU-gts7fewifi/tools/gpu/requirements-perfetto.txt
```

The status command performs no register reads and does not start a capture:

```sh
.venv/bin/python sources/UniXNU-gts7fewifi/tools/gpu/gpu-trace.py status
```

Capture a bounded operation window and save native Perfetto protobuf packets:

```sh
.venv/bin/python sources/UniXNU-gts7fewifi/tools/gpu/gpu-trace.py capture \
  --seconds 5 --output logs/gpu-perfetto-66-runtime
```

The output directory must be new. `definitions.bin`, `counters.bin`, `jobs.bin`
and `events.bin` preserve device records; `status.json` preserves capacities and
loss counts. `trace.pftrace` opens in [Perfetto UI](https://ui.perfetto.dev).
`trace.summary.json` reports actual counter increments, shader instruction
activity, stage pairing and Metal-to-GPU links. Export returns 2 when required
measurements or links are missing or records were lost; retained raw data remains
available for investigation.

`stop` requests early completion. `download --output NEW_DIRECTORY` retrieves an
already stopped capture once producers finish. `export CAPTURE_DIRECTORY --output
FILE.pftrace` converts saved binary records without touching the device.

GPU slices describe the interval between their hardware timestamp commands.
Counter rates describe adjacent valid hardware samples. Stage overlap, pipeline
drain and instrumentation commands remain visible; the exporter does not label
the residual between stages as removable waiting. Pipeline IDs identify recorded
work; this implementation does not sample shader instruction PCs.

The host uses the official [Perfetto protobuf conversion
API](https://perfetto.dev/docs/getting-started/converting) and
[clock snapshots](https://perfetto.dev/docs/concepts/clock-sync). Source revisions,
build results and device observations are recorded separately below when available.

## Build attempts

66 stopped while compiling Adreno6xx because the process ownership code needed
`<sys/proc.h>`. 66a passed kernel and Turnip compilation; Metal source accounting
rejected two paths because the new runtime patch lacked `diff --git` headers.
The patch contents and the source accounting guard are retained, and all eight
affected paths now have standard patch headers. 66b reached Rust bridge
compilation and stopped at texture trait imports and raw format types. Those
errors were corrected in 66c. Failed release directories and tags remain available.

## 66c device capture

The immutable source tag is `build/system-gpu-perfetto-66c-source-1`. The device
commit is `3f7dd221`, core `b39eac5f`, SM7325 `99e2f020`, AdrenoMetal `5012e857`
and Adreno6xx `4eb93604`. The release directory contains the full revision
manifest and nine successful stages. Deployment wrote 536870912 bytes and
completed sync without backing up the previous FAT; workspace evidence is in
`logs/msc-20261029-232629-system-gpu-perfetto-66c/`.

The first capture is retained in workspace `logs/gpu-perfetto-66c-runtime-1/`.
Its five-second hardware window contains 228 counter samples and 49 valid GPU
jobs without hardware sample or job loss. All 29 counter tracks contain values,
including fragment shader instructions. CP-to-RB duration averages 32.25 ms,
with a 16.80 ms minimum and 47.80 ms maximum. This is a different operation
window from 64a and is not a controlled performance comparison.

The user-event section contains 26047 events, 3259 paired GPU stages and 18977
Metal metadata events, of which 17755 have submission links. The producer
reports 46978 dropped records and one incomplete producer; four stage starts
lack ends and 68 GMEM records have invalid timestamps. GPU stages cover only
the first approximately 0.63 seconds. These records cannot explain the complete
five-second GPU workload. The native binary sections are preserved for the
producer fixes; `trace.summary.json` correctly reports `complete=false`.

## 66e producer repairs and deployment

The GPU timestamp budget now counts records still owned by `u_trace` and returns
them at release. Each accepted stage begin reserves its matching end. The worker
uses a bounded 4096-event queue and batches its transport writes; GPU trace
callbacks wait for queue space instead of discarding completed timestamps.
Stop and submission admission share the submission mutex. Producer completion
waits for admitted submissions to retire, without a fixed three-second cutoff.
Unsubmitted command buffers retain normal retirement and release behavior while
their timestamps are excluded from the stopped capture. GMEM slots copied into
binning or sysmem paths are counted as unexecuted; zero timestamps in an executed
GMEM path remain invalid.

Metal metadata retains every draw while emitting encoder and pipeline identities
only when they change within a capture generation. Pass descriptions are emitted
before the first recorded draw and again with final actions at encoder end.
The event pool is 131072 records, and default capture duration is five seconds.
Final producer records separate budget, queue and append losses from unsubmitted
commands and unexecuted GMEM points. Existing copy payload formats and buffer-copy
byte counts are exported; image-copy resource identities and extents are not yet
recorded.

66d stopped because sandboxed host `sysctl` calls could not provide XNU build
parameters. 66d2 used the same source with build access and passed kernel and
Turnip, then found a missing `traced_pass_generation` initializer in the render
execution replay constructor. That initializer was added and recorded in the
Metal runtime patch. 66e passed all nine stages, including Rust release compilation.

The source tag is `build/system-gpu-perfetto-66e-source-1`: device `ea472086`,
core `41b99918`, SM7325 `49f62160`, AdrenoMetal `166696a1`, Adreno6xx `6eab0141`.
All commits and tags are published. The release directory preserves
`source-revisions.json`, `xnu-patches.list`, `rust-bridge.log` and stage logs.
Deployment wrote 536870912 bytes and completed sync without backing up the old
FAT. Evidence is in workspace
`logs/msc-20261030-000237-system-gpu-perfetto-66e/`.

The 66e capture is in `logs/gpu-perfetto-66e-runtime-1/`. It has 226 hardware
samples, 57 valid jobs, 56070 paired GPU stages and 18318 Metal events. Unmatched
begins/ends, invalid events/jobs, incomplete producers and hardware access errors
are all zero. The five-second operation filled the 131072-event pool: 17581
kernel event losses and 11860 producer losses make this capture incomplete.
These are retained in the status and Perfetto summary. The following streaming
implementation addresses event storage and producer memory budgeting.

Perfetto 59.1 imports 22815 slices/instant events and 6811 counter values, exactly
matching the exporter output including clock uncertainty samples. Its default
BOOTTIME fallback reports one unrelated-clock diagnostic against the explicitly
selected MONOTONIC trace clock; actual timestamp conversion failure counts are
zero. The importer registers that fallback before reading `primary_trace_clock`.
The capture retains its measured GPU/MONOTONIC snapshots without inventing a
BOOTTIME relationship. The corresponding upstream reader sources and inspection
output are retained under workspace `logs/perfetto-66-research/` and the capture
directory.

## 65c shutdown observation

While preparing MSC, the user reported a shutdown stall on 65c. Userspace teardown
finished in 3057 ms; kernel output stopped at `unmount(1) /private/var` while touch
and battery output continued. The reboot thread was waiting in the kernel. UFS
had 379839 submissions and completions, no active requests or errors; RamBlock
also had no active or queued requests. The remaining lock or resource requires
a kernel stack to identify. The saved evidence is in workspace
`logs/shutdown-65c-20261029/`. No 66 package had been deployed at that point.
