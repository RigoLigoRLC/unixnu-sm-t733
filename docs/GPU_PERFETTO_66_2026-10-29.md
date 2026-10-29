# A642L Perfetto capture

This capture connects Metal workload metadata, Turnip GPU timestamps and the
Adreno6xx hardware counter window. The implementation is being integrated for
`system-gpu-perfetto-66`; no runtime capture from this package has been recorded.

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
counter samples, 4096 jobs and 32768 user events. Each producer registers with the
capture generation and ends only after its pending timestamps and metadata have
been appended. Stopping ends recording; sealing requires hardware retirement,
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
  --seconds 20 --output logs/gpu-perfetto-66-runtime
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
