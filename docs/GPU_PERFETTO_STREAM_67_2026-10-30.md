# GPU trace streaming

The 67 implementation sends event records to the host during the capture.
The device retains a bounded circular buffer, and the host writes each batch
before acknowledging its sequence. Capture windows remain 0.1 through 60 seconds;
counter and job arrays retain their existing capacities and report any overflow.
Build, deployment and runtime results are recorded separately when available.

## Data movement

`StartStream` is control operation 4. Methods 39 and 40 read and acknowledge
events using a generation and an absolute sequence number. The 1472-byte read
result has a 64-byte header and up to eight 176-byte events. Sequences start at
zero in each generation; next/write values are exclusive. A reader that falls
behind receives the missing sequence interval. The GPU recorder overwrites the
oldest unacknowledged event when full; rendering does not wait for the host.

The TCP helper accepts a batch count from 1 through 128 in header reserved word
0 for method 39. It combines that many complete kernel chunks into one response,
up to 188416 bytes. Each chunk preserves its own sequences and missing interval.
The host acknowledges only after writing the received records. It also reads
committed counter samples and jobs during recording through method 35.

After hardware retirement and producer completion, the host seals the capture,
drains remaining events and renames the raw files from `.partial` to `.bin`.
`device-status.json` preserves device counts. `status.json` also records the
number of events actually saved and any host sequence gaps. `stream-gaps.jsonl`
contains exact missing intervals; `stream-status.json` records received bytes,
batch count and transfer rate. An interrupted host keeps the partial files.

Kernel overwrites before an ACK arrives are reported separately from actual
host sequence gaps: data already received can survive such an overwrite.
The exporter's completeness result uses observed missing events, producer losses,
counter/job losses, timestamp validity and stage pairing.

Turnip uses a 16 MiB budget for retained timestamp/indirect chunks, events,
payloads and reserved allocation overhead. Begins reserve their matching ends.
Source render-pass trace buffers transfer to the submitted trace and retire with
its fence, while cloned source CPU payloads are released. Budget peak and limit
are exported alongside producer loss categories.

## Capture

```sh
.venv/bin/python sources/UniXNU-gts7fewifi/tools/gpu/gpu-trace.py capture \
  --seconds 20 --output logs/gpu-perfetto-67-runtime
```

The output directory must be new. Ctrl-C requests stop and drains submitted GPU
work. `--buffered` selects the older stop-then-download protocol for 66e and
earlier packages. The final `trace.pftrace` opens in Perfetto. Raw records remain
available for offline re-export. Hardware counters and shader instruction
activity remain enabled through the existing GMU AO/performance OOB protocol.
