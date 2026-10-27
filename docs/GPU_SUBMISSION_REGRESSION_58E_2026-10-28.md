# GPU submission regression and build 58e

`system-gpu-submit-58e` completed all nine build stages and was deployed through
MSC on 2026-10-28. The service wrote 536870912 bytes and completed sync. Data was
preserved. Passive USB capture was restarted before requesting boot and repeated
Settings interaction. Measured interaction and the subsequent 58f deployment
are recorded below.

## Measured regression in 58d

Both versions have 512 completed backboardd submissions from two operation-period
windows. These measurements describe submissions, and a displayed frame may use
several submissions. Initialization allocations are outside these windows.

| CPU submission time | 57c | 58d |
| --- | ---: | ---: |
| Mean | 3.980875 ms | 25.437125 ms |
| Median | 3.6075 ms | 22.854 ms |
| p95 | 6.6352 ms | 68.4443 ms |

The mean increased 6.3898 times and the median increased 6.3351 times. Time from
submission entry to observed fence completion increased from mean 37.5563 ms and
median 34.7885 ms to mean 77.1237 ms and median 80.3515 ms. This interval includes
CPU preparation, queueing, GPU work and host observation delay.

58d sequences 436-691 and 1085-1340 cover system timestamps 104.12-301.03 seconds
with a gap between the saved windows. Among these samples, 236 submissions have
no preceding recorded submission awaiting completion observation: their CPU mean
is 2.2170 ms and median is 1.9745 ms. The other 276 have one such preceding
submission: mean 45.2920 ms and median 49.0305 ms. This split agrees with the
source path that compares resources against unfinished jobs.

Evidence is in workspace `logs/gpu-submit-58d-runtime/`: `interaction-1.raw`,
`interaction-2.raw`, `submission-costs.csv`, `submission-costs-summary.json` and
`comparison-57c.json`. The 57c input is
`logs/gpu-concurrency-57c-runtime/submission-costs.csv`.

## Causes and implementation

58d collected every ACTIVE internal BO on each submission and advanced its last
fence, even when the current commands did not reference it. The saved submissions
contain mean 1454 resources, maximum 3991. The kernel then compared current
resources against unfinished jobs using whole-BO physical spans. This multiplied
resource searches and delayed reuse of unrelated command storage.

58e records command-stream ranges when commands, draw states and substreams are
assembled, and propagates them through secondary command buffers and appended
command streams. Shader storage, private memory, pipeline state, global/tessellation
storage, scratch, query/event, trace/autotune and visibility storage are included
where their addresses are emitted. `darwin_lookup_bo_locked` locates an ACTIVE BO
through the IOVA red-black tree. Submission deduplicates lifetime references by
handle and advances the last fence only for selected BOs. Explicit BDA/UAB and
other incomplete-tracking fallbacks retain their wider resource collection.

The kernel now keeps physical span intervals in an augmented libkern red-black
tree. Mapping allocates its node array once, installs entries after successful
GPU mapping, and removes them during unmapping. Each job indexes its actual
resource ranges by handle. Conflict queries intersect those ranges at cache-line
boundaries and require synchronization when a writer shares cached memory with
an unfinished job. The eight job indexes consume 64 KiB in total. Query limits
return Busy and let older jobs retire before retrying.

Source entries are [VM interval lookup](../drivers/Adreno6xx/NativeAdrenoVm.cpp),
[job conflict lookup and timing](../drivers/Adreno6xx/NativeAdrenoSubmit.cpp) and
[cache range iteration](../drivers/Adreno6xx/NativeAdrenoCpu.cpp). Cache iteration
now finds the initial logical span by binary search. The Mesa implementation is
maintained in core `patches/mesa/turnip-resource-submit-v2.patch`, registered in
`scripts/data/adreno-compiler-inputs.json`; `.cache/mesa-work` is its build tree.

The second 58d operation interval had 10647 BO requests and 4639 hits, a 43.57%
reuse rate. Of 6008 misses, 3480 found compatible BOs still waiting for fences.
The old NoSpace path cleared the cache and waited for all pending BOs. Its
`trim_waiters` path also destroyed completed cacheable BOs without counting those
evictions. 58e reclaims one object for mapping-slot pressure or the requested
bytes for memory/address-space pressure, then retries the allocation. It retains
a bounded deadline and worker ownership of physical destruction.

Freedreno already separates ringbuffer storage from ordinary BO caching in
`src/freedreno/drm/freedreno_bo.c:175` and initializes both caches in
`freedreno_device.c:98`. 58e follows this usage separation with a command-stream
allocation flag and two LRU lists. The existing 128 MiB total budget remains;
ordinary BOs are limited to 64 MiB to retain room for command streams. This budget
split is a platform policy. Existing size buckets and compatibility requirements
remain in effect. All actual cache evictions and successful destruction count
toward the diagnostics.

## Measurement fields and remaining work

`S.submit_cpu` retains its previous entry/exit boundaries. In 58e, `cache` measures
the actual cache-maintenance loop and final barrier; `publish` measures the other
preparation, reference and conflict work. A separate bounded `A` record reports
`alias_us`, `queries` and `hits`, matched by sequence and fence. `alias_us` is part
of `publish`, so graphs must not add it twice. Aggregate alias statistics include
Busy attempts caused by query limits. Normal rendering does not print each event.

The active cached IOSurface ranges still require maintenance. Image use currently
records the complete image allocation, and imports have no producer CPU-write
notification that would allow maintenance to be skipped. The 58d mean of about
63.3 MB per submission may therefore remain if those images are actually used.
The next saved interaction must establish the reduction in lookup cost, selected
resource count, BO reuse rate and remaining cache-maintenance cost. GPU execution
and pipeline work remain separate follow-up items.

## Build and deployment

| Source | Commit |
| --- | --- |
| Device | `e0dd0d1f0bc00bdaea892ca4470f8da1d81284b9` |
| Core | `59a43917d5d12dd13ca14c9859d4347e8c9529ff` |
| SM7325 | `e0842b2894b5634f02611b55a773e81e6178a0df` |
| Adreno6xx | `ae7a8d2a693cf1259fc8c192828656567d8aa59c` |
| AdrenoMetal | `fd02456764c399ab5a56dfdd603fbc887a08da92` |

All build inputs were committed and clean before compilation. The immutable tag
is `build/system-gpu-submit-58e-source-1`. No tests were added or run.

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-submit-58e --system --payload-only \
  --local-module soc:sm7325="$PWD/sources/unixnu-sm7325-common"
bash tools/deploy-t733-msc.sh system-gpu-submit-58e
```

The release directory is `build/releases/system-gpu-submit-58e/`, containing
`payload.img`, `stages.tsv`, `source-revisions.json` and stage logs. Workspace
deployment evidence is `logs/msc-20261028-001209-system-gpu-submit-58e/`.
USB status, raw logs and disconnect tails remain in `logs/t733-usb-live/`.

## 58e interaction and the remaining per-page query

The two saved 58e windows contain 512 submissions. CPU time is mean 4.002514 ms,
median 1.8595 ms and p95 13.8758 ms. Physical alias queries still account for
mean 2.0977 ms and p95 12.0044 ms. The interval tree made individual lookups faster,
but the outer loop still queried every physical span, mostly finding the same BO.
Slow submissions made about 31000 queries. Replacing only the lookup structure
therefore left a repeated page walk in the submission path.

The separate BO interval q26-q80 covers 83.966038-192.843844 seconds: 19430 requests,
18948 hits, 97.5193% reuse. Four command-stream classes had 18305 requests and
18170 hits, 99.2625% reuse. Descriptor pools account for 329 of the remaining 482
misses; their ordinary-cache placement competes with 64 MiB allocations. The
counts do not yet attribute evictions by usage. Mapping failures did not increase.
Puts minus hits and evictions exactly match the change in cached objects, and
evictions plus uncached completed objects exactly match successful destruction.

The data, chart and BO interval are in `logs/gpu-submit-58e-runtime/`:
`submission-costs-summary.json`, `submission-costs.png` and
`bo-interaction-delta.json`. CPU sample windows and the longer BO interval are
reported separately.

## 58f deployment and operation measurements

58f establishes physical alias relationships while mapping. Each relationship
stores offsets in the two BOs and a byte length; adjacent relationships with the
same offset translation are merged. Submission checks its own mapping and these
stored relationships. An unaliased BO requires no physical-page traversal during
submission. Same-BO repeated physical pages remain represented. Unmapping removes
both ends after outstanding job references have retired.

All nine stages completed and MSC wrote 536870912 bytes with sync. The immutable
tag is `build/system-gpu-submit-58f-source-1`. Build source: device
`3f33c303d9a4b4c49f7cc6a0712f7e4de2a61331`, core
`59a43917d5d12dd13ca14c9859d4347e8c9529ff`, SoC
`1474962f4f0b0000fe77ab5e428f29103db174ea`, Adreno6xx
`6c92ba91d3a684814cd2e4a0127e647ba6e6635c`, AdrenoMetal
`fd02456764c399ab5a56dfdd603fbc887a08da92`.

The command is the 58e command above with tag `system-gpu-submit-58f`. The release
directory contains the exact command, revisions and stage results. Deployment
evidence is `logs/msc-20261028-002619-system-gpu-submit-58f/`. Passive collector
was restarted before boot.

Saved operation sequences 468-723 cover 111.233157-128.800362 seconds. CPU submission
mean is 1.838656 ms, median 1.755 ms, p95 2.33175 ms and maximum 2.766 ms. Alias
query maximum is 29 us; cache maintenance mean is 1.814883 ms. Other submission
work totals mean 23.773 us. These results establish that the per-page query
spike was removed; repeated cache maintenance remains the main CPU cost.

Raw data and statistics are in `logs/gpu-submit-58f-runtime/`. `aliases version=2`
counts mapping/alias intervals rather than physical-tree lookups. Current source
work on directional CVAC/IVAC and per-BO hardware coherency is not part of 58f.
