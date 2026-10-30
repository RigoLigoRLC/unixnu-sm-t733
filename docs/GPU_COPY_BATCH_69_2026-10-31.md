# A6xx buffer-to-image copy batching and blocked-thread snapshots

The 68b capture identified a 2732 by 2732 BGRA buffer with a 10928-byte row
pitch. A6xx sampling requires a 64-byte-aligned pitch, so the existing Turnip
fallback emits a separate blit for each row. The complete jobs in the later
67 capture segment spent an average 10.678 ms copying. The 68b capture has
resource metadata but no GPU job timings; it does not establish that this
particular resource accounts for that duration.

Core commit `e5e477192346a5cf05163f06786c8715980faabf` adds
`turnip-unaligned-buffer-image-batch.patch` to the compiler input manifest.
The A6xx path batches up to 256 rectangles using Turnip's existing multi-blit
shader. Each rectangle samples a row or the portion of a row that fits within
a 16384-texel-wide aligned source view. The source buffer contents remain in
place. The destination retains its existing fdl layout and CCU transitions.
The command buffer owns the coordinate storage until its normal retirement.
Unsupported formats and layouts use the existing copy path.

Each copy END event reports the following values across all regions and layers:

| Key | Exported name | Meaning |
| --- | --- | --- |
| 31 | bytes | Copied texel bytes, excluding row padding |
| 5 | draw_count | Emitted CP_BLIT packets plus RECTLIST draws |
| 37 | copy_rows | Total copied rows |
| 38 | copy_batched_draws | Multi-blit RECTLIST draws included in draw_count |

BEGIN events retain the source buffer, destination image and copy operation ID.
The public argument definitions are in Adreno6xx commit
`e6ca93c76b4ece1e92b0360ec6ade87a6dcec7be`. Device commit `7dea3bf` names the
new arguments in Perfetto exports. Actual performance remains to be measured
on the device using a 60-second capture with populated hardware counters.

Device commit `a5e3501` adds bounded blocked-thread kernel stacks, BSD wait
descriptions and thread pagination to the existing USB tasks command.
NativeUfs commit `06b3a102e78c3129b4645416a52ac23f5523f3cb` records request
owners and phases for the existing UFS snapshot. Device commit `71d2a05`
registers `native-zone-expander-snapshot.patch` in `diagnostics.list`; the
snapshot reports zone allocation and expansion state through the UFS command.
These additions collect missing evidence for the separate shutdown and
runtime freeze investigations. They do not change request admission, lease
release or shutdown behavior.

The build's `source-revisions.json` and immutable source tag identify the full
set of selected sources. Freeze evidence remains in the workspace under
`logs/gpu-68b-freeze-1/`; the earlier shutdown evidence is under
`logs/shutdown-65c-20261029/`.
