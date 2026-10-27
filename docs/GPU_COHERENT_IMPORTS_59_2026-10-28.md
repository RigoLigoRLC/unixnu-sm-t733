# A642L coherent imports, build 59

`system-gpu-coherent-59` completed all nine stages and was deployed on 2026-10-28.
MSC wrote 536870912 bytes and completed sync. Data was preserved. Passive USB
capture was restarted before requesting boot and Settings interaction. Runtime
measurements are pending.

58f removed submission-time physical-page alias queries. Its saved 256 operation
submissions took mean 1.838656 ms, of which 1.814883 ms was CPU cache maintenance.
The remaining repeated work covered about 63 MB of imported memory per submission.
The evidence is in [the preceding report](GPU_SUBMISSION_REGRESSION_58E_2026-10-28.md)
and workspace `logs/gpu-submit-58f-runtime/`.

## Reference procedure and implementation

The Samsung SM7325 source in workspace
`references/android_kernel_samsung_sm7325/` contains the complete reference chain:

1. `arch/arm64/boot/dts/vendor/qcom/yupik-gpu.dtsi:20` selects A642L.
2. `drivers/gpu/msm/adreno-gpulist.h:1723` declares `ADRENO_IOCOHERENT` for this GPU;
   `adreno.c:1493` enables `KGSL_MMU_IO_COHERENT`.
3. `kgsl_sharedmem.c` permits `KGSL_MEMFLAGS_IOCOHERENT` for cached allocations and
   excludes WC/uncached modes. Import handling records the exporter/VMA cachemode.
4. `kgsl_iommu.c:1687` translates the flag to `IOMMU_CACHE`.
5. `drivers/iommu/io-pgtable-arm.c:591` selects MAIR `0xff`; its leaf descriptors
   use Outer Shareable at line 392.

The v500 path takes data attributes from the SMMU PTE. LLCC SCID hints and the
provider's coherent page-table-walk property have separate roles. This change
preserves existing page-table-walk configuration, page-table maintenance and GPU
cache events.

The corresponding local procedure is:

- After successful GPU initialization, submission-queue and timer setup, Service
  enables cached-import support. VM checks chip ID `0x06030500`, attached healthy
  state and its initialized SMMU context.
- `importTask` pins and retains the original backing, gathers physical spans and
  classifies CPU attributes once. Every page must return ARM
  `VM_WIMG_COPYBACK` (`2`) after masking. Unknown, non-WB and mixed imports remain
  eligible only for the software-maintained path.
- VM gives qualified imports Normal WB/WA, Outer Shareable leaf mappings and
  saves the actual `io_coherent` property on each mapping. User-supplied map flags
  cannot bypass the backing check.
- Submission and retirement read this saved property. Coherent mappings use the
  existing barriers and fence ordering while skipping explicit CPU clean and
  invalidate. No page-attribute scan occurs during submission or retirement.
- Unmapping releases the mapping and backing through existing retirement rules.
  Physical aliases still pass the cacheability/shareability consistency checks.

Code entries: [CPU attribute classification](../drivers/Adreno6xx/NativeAdrenoCpu.cpp),
[import lifetime](../drivers/Adreno6xx/NativeAdrenoMemory.cpp),
[mapping and cache transfer](../drivers/Adreno6xx/NativeAdrenoVm.cpp),
[capability negotiation](../drivers/Adreno6xx/NativeAdrenoUserClient.cpp) and
[initialization](../drivers/Adreno6xx/NativeAdrenoService.cpp).

The new capability is specific to cached imports. The old capability for creating
HOST_CACHED allocations remains disabled, preserving the existing owned-WC
allocation contract. Global coherence skips and `importedUntouched` skips were
removed; the mapping's actual attributes determine maintenance.

## CPU attributes and access direction

The pinned allocation keeps its CPU cachemode while GPU mappings use it, following
the reference driver's allocation contract. `IOMemoryDescriptor::prepare()` pins
pages; it does not prevent an independent `MAP_MEM_ONLY` operation from changing
cachemode. Current KMD import CPU-view and make-write-combined paths reject such
changes, and ordinary display/import/retire paths do not request them. No new
global XNU attribute-holding mechanism was added.

For the software-maintained fallback, publication now uses CVAC, and acquisition
after a known GPU-write range uses IVAC. This follows
[Linux ARM64 DMA synchronization](https://github.com/torvalds/linux/blob/master/arch/arm64/mm/dma-mapping.c)
and its [cache instruction helpers](https://github.com/torvalds/linux/blob/master/arch/arm64/mm/cache.S).
The caller owns intersecting whole cache lines during device use; publication
cleans them before the GPU can write. Legacy acquisition without a submitted-range
contract retains CIVAC. WC barriers remain in place.

The existing XNU modified bit is shared VM state: wired mappings can prevent it
from being cleared, pageout also clears it, and the physical aperture is outside
ordinary user-PV fault tracking. It is not a private GPU dirty-page record.
IOSurface lock/seed also does not prove a CPU store: saved backboardd disassembly
places RenderDisplay inside a lock/unlock pair. Build 59 therefore uses the
hardware-coherent allocation path to eliminate repeated maintenance for qualified
imports. Unknown imports keep software maintenance.

## Operation measurements

The `gpu` query prints bounded summaries:

- `coherence`: cumulative/live coherent mappings and bytes, bytes whose explicit
  clean or acquisition was skipped.
- `coherence_fallback`: owned, platform, unknown, non-writeback and mixed reasons.
- Existing S/R/A records retain submission timing, resource ranges and alias work.

The runtime directory is `logs/gpu-coherent-59-runtime/`. The first operation
capture must show actual coherent mappings and skipped-maintenance bytes, together
with reduced `cache` and total submission time. An advertised capability alone
does not establish that the displayed surfaces took the new path.

## Build and deployment identity

| Source | Commit |
| --- | --- |
| Device | `600227b55fe68536036fce4328d10c1ffbc9dc38` |
| Core | `59a43917d5d12dd13ca14c9859d4347e8c9529ff` |
| SM7325 | `09a6f33684a4a4ec5130f311258d400f17a8a417` |
| Adreno6xx | `57d90a1c226e2a1e7ef2371a4aae7ff58d4d2b8f` |
| AdrenoMetal | `fd02456764c399ab5a56dfdd603fbc887a08da92` |

All source was committed and clean before building. The immutable tag is
`build/system-gpu-coherent-59-source-1`. No tests were added or run.

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-coherent-59 --system --payload-only \
  --local-module soc:sm7325="$PWD/sources/unixnu-sm7325-common"
bash tools/deploy-t733-msc.sh system-gpu-coherent-59
```

The release is `build/releases/system-gpu-coherent-59/`, with `payload.img`,
`stages.tsv`, `source-revisions.json` and logs. Deployment evidence is in workspace
`logs/msc-20261028-004805-system-gpu-coherent-59/`.
