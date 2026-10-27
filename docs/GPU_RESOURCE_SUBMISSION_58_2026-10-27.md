# GPU resource submission, build 58b

`system-gpu-submit-58b` completed all nine build stages and was written to SM-T733
through MSC on 2026-10-27. The service wrote 536870912 bytes and completed sync.
Data was preserved. Passive USB capture was running before the boot request.
Operation-period measurements are pending the user's boot and interaction.

## Implementation

This implements the [resource submission design](GPU_RESOURCE_SUBMISSION_DESIGN_57C_2026-10-27.md)
and addresses the allocation failures in the [two-frame incident](GPU_TWO_FRAME_FLICKER_57C_2026-10-27.md).
Official upstream references were refreshed. The core updates only changed device
records; the kernel adopts Adreno upstream `5c8c145`'s removal of full TLB
invalidation on ordinary submissions. Mapping changes retain their synchronization.

- Mesa collects descriptor, attachment, vertex/index/indirect, transfer/query and
  secondary-command resources, and retains internal command/shader/scratch BOs.
- Method 20 negotiates SubmitV2 and kernel cache maintenance; 21 accepts one bounded
  packet containing IBs and resource ranges; 22 reports shared mapping budgets.
- Cache ranges remain separate from BO lifetime references. Only adjacent or
  overlapping ranges with the same handle/access are merged. Maintenance covers
  intersecting cache lines; each job retains each mapping once.
- GPU-write resources are invalidated before WaitFence succeeds. Cached physical
  aliases involving a writer wait for ownership transfer; read/read may overlap.
  Owned WC resources share a barrier.
- Metal skips duplicate IOSurface flushes only after the capability query succeeds.
  `didModifyRange:` reaches the runtime; failed command buffers do not present drawables.
- Ready BO caches follow shared mapping budgets. Pressure triggers bounded reclaim
  and retry. Alias Busy waits use a monotonic five-second deadline; deferred frees
  preserve handle, IOVA and completed CPU-unmap state.
- Command allocation failures propagate through substreams, shader/pipeline creation,
  command assembly and queue submission. Further writes and invalid IB submission stop.

Tracked ranges cover VkBuffer/VkImage allocations within heaps. BDA, descriptor
buffers and actual mutable descriptor bindings use an explicit AllMapped fallback.
Unknown cached imports still require maintenance on use. The bounded diagnostic
ring records fallback reasons and maintained bytes; S and R rows join by sequence/fence.

## Sources and delivery

All Mesa edits are recorded in core `patches/mesa/turnip-resource-submit-v2.patch`,
appended to `scripts/data/adreno-compiler-inputs.json`. The successful Turnip build
consumed the recorded patch sequence against pinned Mesa. No cache-only changes
were used to produce the image.

| Repository | Build commit |
| --- | --- |
| Device | `968e3364f13619b2dea1314004966c3a17a890f5` |
| Core | `2670fef131f91e8e8292070004266f2e58ae9058` |
| SoC | `f3512bb66949282d4e63fdb3f9bc313d5a69dc04` |
| Adreno6xx | `8a3c8db9563e212986b8b993bdee327e72c4add5` |
| AdrenoMetal | `fd02456764c399ab5a56dfdd603fbc887a08da92` |
| ArmSmmu4K | `d92ebe7ee91e2412a40490f9f78a4a927876229a` |
| NativeMobileDisplay | `0090571635e84144b09a1877e088d19c055a7979` |

Changed repositories have immutable tag `build/system-gpu-submit-58b-source-1`.
Source trees were clean before building. No tests were added or run; remote
publication was not requested.

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-submit-58b --system --payload-only \
  --local-module soc:sm7325="$PWD/sources/unixnu-sm7325-common"
bash tools/deploy-t733-msc.sh system-gpu-submit-58b
```

Records are in `build/releases/system-gpu-submit-58b/`: `stages.tsv`,
`source-revisions.json` and stage commands/logs. Attempt 58 failed on kernel enum
arithmetic; 58a passed the kernel and failed on a missing command-buffer declaration
in Turnip. Both fixes were committed before building 58b; earlier tags/logs remain.

Workspace deployment evidence is in
`logs/msc-20261027-232740-system-gpu-submit-58b/write-fat.jsonl`; the previous FAT is
`backups/msc-20261027-232740-system-gpu-submit-58b/payload-before.img`.
USB raw captures and status are under `logs/t733-usb-live/`.

## Operation-period comparison

The 57c baseline has 512 completed interaction submissions: CPU submission mean
3.981 ms/p95 6.635 ms; cache publication mean 3.775 ms/p95 6.394 ms. Data is in
`logs/gpu-concurrency-57c-runtime/`. Compare 58b after user interaction, keeping
initial allocations separate. CPU fence-observation intervals are not GPU
utilization. GPU hardware profiling remains outside this CPU-submission iteration.
