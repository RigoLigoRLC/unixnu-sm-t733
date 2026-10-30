# PM4 command decoding

`shader-capture.py --pid PID --session SESSION --trigger-pm4 0` requests one
nonempty submission after the producer arms. Run it during the trace window,
then download that session with `shader-capture.py`. The producer writes a
16 MiB maximum Mesa RD file and a JSON manifest with the same basename. The
request and result procedures are described in `SHADER_CAPTURE.md`.

`decode-pm4.py` reads those two files, reports saved command-address coverage,
and invokes Mesa's cffdump for register and packet names. For the prepared host
tool in this workspace:

```sh
.venv/bin/python sources/UniXNU-gts7fewifi/tools/gpu/decode-pm4.py \
  /absolute/capture/PID/sSESSION_pm4-SERIAL.rd \
  --cffdump logs/gpu-pm4-implementation/cffdump-build/src/freedreno/decode/cffdump \
  --output /absolute/path/new-decoded-directory
```

Output includes `commands.txt`, `decoder-stderr.txt`, `decode-report.json`, and
`summary.txt`. The output directory must be new. The default decoder invocation
uses `--no-color --no-pager --once`, which displays each command stream once per
draw mode. `--expand-ibs` expands repeated IB calls. Those are encoded replay
paths; predicates, visibility and hardware execution counts require the linked
runtime capture. Output is limited to 64 MiB per text file and 120 seconds by
default. `--coverage-only` emits the report without invoking cffdump.

Join the manifest's `completion_receipt` and `command_buffer_recording_ids` with
the same process's saved trace events. `pm4_submit_serial` identifies the capture
request within the process, while `mesa_submit_count` is scoped to its Vulkan
device. A successful decoder exit records text production. Command coverage
also requires all recorded top-level/nested IB and draw-state references to be
saved, no metadata loss, no malformed packet region, and no traversal limit.
The report lists each missing reference with its referring packet address and
producer reason.

The producer copies explicitly referenced command ranges from retained CPU
mappings before kernel submission. It follows ordinary command-stream BOs and
referenced commands inside internal read-only state allocations. Application
textures and ordinary resource bytes are excluded. External CP_LOAD_STATE6
addresses have address-only coverage: a present address does not establish the
complete state data extent. GPU-writable command allocations are listed because
their bytes describe the snapshot before execution.

The host tool uses this workspace's patched Mesa source, Lua 5.4.6 built as a
static library, and the macOS libarchive library with public libarchive headers.
These host dependencies and build outputs stay in
`logs/gpu-pm4-implementation/`. Its Meson configuration disables Vulkan/Gallium
drivers and tests, selects only the Freedreno tools, and ninja builds only
`src/freedreno/decode/cffdump`. Register XML comes from that Mesa source tree.
