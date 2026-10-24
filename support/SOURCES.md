# Source provenance and licenses

Recorded on 2026-10-24 for the SM-T733 device repository. The table below identifies upstream baselines. Eight driver and kit submodules pin the published SM-T733 adaptation commits listed in [repositories.json](repositories.json). The device Git tree records each module commit needed for this release.

## Module origins and published adaptations

These directories were copied from the corresponding paths in the development checkout of [UniXNU](https://github.com/UniXNU/UniXNU), whose base revision was `5fdbd5d08518bc2d5c196264c56a377dd80cf5ca`.

The published core fork is fixed at `63f6920d552daacd050b4af65e45d6df16cfc332` by `UniXNU.rev`. Its official baseline is fixed by `UniXNU.upstream.rev`; `UniXNU.patch` records the changes between those revisions. The UEFI fork is fixed at `799544f93bfffd0e7eae38efb53a08fb996d4af5` by `Mu-Silicium.rev`. Bootstrap clones the adapted core directly.

On 2026-10-24, NativeMobileDisplay was merged with the version fixed by official UniXNU main `1a01c2afa197d975d04da892325c56b6b14aaecb`: `8666a9950c9699b368931d18cac92c0fdb2ce9c0`. This adds GPU swap retirement on acceptance and fixed refresh intervals while retaining the T733 CPU copy, source mapping cache and performance CPU binding. The other six module revisions in this table are unchanged in that official main revision. Synchronization follows the main repository's module revisions.

| Device directory | Upstream repository | Checked-out baseline | Existing license to retain |
| --- | --- | --- | --- |
| `drivers/ArmCpu` | [UniXNU/ArmCpu](https://github.com/UniXNU/ArmCpu) | `1951cc8444b340d7d8879e9418220cf29eb711d7` | Its `LICENSE`, BSD-2-Clause-Patent |
| `drivers/DarwinBoot` | [UniXNU/DarwinBoot](https://github.com/UniXNU/DarwinBoot) | `d099354d0eab34df5088ca1ba89c9c88bc3260bb` | Its `LICENSE`, BSD-2-Clause-Patent |
| `drivers/NativeGeniSpi` | [UniXNU/NativeGeniSpi](https://github.com/UniXNU/NativeGeniSpi) | `b5399b5646c4d7415deec6d887fd51715a97a024` | Its `LICENSE`, BSD-2-Clause-Patent |
| `drivers/NativeMobileDisplay` | [UniXNU/NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay) | `8666a9950c9699b368931d18cac92c0fdb2ce9c0` | Its `LICENSE`, BSD-2-Clause-Patent; retain its existing ArmVirtPlatform contributor notice |
| `drivers/NativeRamBlock` | [UniXNU/NativeRamBlock](https://github.com/UniXNU/NativeRamBlock) | `d4d22d9819b0162eda49f6402235dddc2279410d` | Its `LICENSE`, BSD-2-Clause-Patent |
| `drivers/NativeUfs` | [UniXNU/NativeUfs](https://github.com/UniXNU/NativeUfs) | `af785c1777161a62931425c2faaa536ce0bebd5b` | Its `LICENSE`, BSD-2-Clause, copyright 2026 blu-sharky |
| `kits/NativePrelink` | [UniXNU/NativePrelink](https://github.com/UniXNU/NativePrelink) | `2b6ffb50117eee820b98aca9bd51b5c7dfb2970e` | Its `LICENSE`, BSD-2-Clause-Patent |

`drivers/NativeFocaltechTouch`, `drivers/NativeUsbDebug`, and `kits/EarlyEntry` originated as local additions under that UniXNU checkout. NativeFocaltechTouch is now a published independent submodule; NativeUsbDebug and EarlyEntry remain device source. Device XNU changes reside in `patches/t733-xnu`; the official base supplies `patches/xnu`. The device includes five replacements of individual public patches and lists its additions and optional diagnostics separately. Preserve original source notices inside patches and the licenses of the XNU source files to which they apply. The repository's BSD license does not replace Apple source licenses or licenses of other imported material.

`kits/EarlyEntry` contains the SM-T733 early framebuffer diagnostics. `drivers/NativeFocaltechTouch/Fts8203.c` and `.h` declare BSD-2-Clause-Patent. The protocol research used the [FocalTech FT820x Android source](https://github.com/Bush-cat/android_kernel_samsung_sm7225/tree/lineage-22.2/drivers/input/touchscreen/focaltech/ft820x), whose files carry FocalTech copyright notices and GPL version 2 terms. Those reference notices must be retained with any copied reference code; the SPDX declarations on the new module do not relicense Android source. The research checkout did not record a fixed Git revision for those downloaded reference files.

The ADT encoder and parts of the board workflow use [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng), checked out at `b06f2df186c8967fe0cd791d6598d7d86f320d77`, under BSD-2-Clause-Patent. Keep the existing copyright and SPDX notice in `scripts/make-gts7fewifi-adt.py` and notices in other retained sources.

## NativeUsbDebug source chain and retained rights

The immediate source for the controller and CDC protocol is [xyyyds0106/ios-vermeer-lab](https://github.com/xyyyds0106/ios-vermeer-lab/tree/d68e05df683f863f054b0f9c063c54d2b1342b2a), revision `d68e05df683f863f054b0f9c063c54d2b1342b2a`:

- `xnu/vermeer/vermeer_usb_debug.c` supplies the DWC3 controller and endpoint implementation adapted in `drivers/NativeUsbDebug/native_usb_debug.c`.
- `xnu/vermeer/vermeer_usb_protocol.h` supplies CDC descriptors and request handling adapted in the file of the same name.
- `xnu/vermeer/vermeer_debug.c` and `vermeer_log_ring.h` were the reference for console capture and independent reader positions. The T733 ring adds its own overwrite accounting, publication and panic integration.
- T733 changes supply SM7325 register windows, clock/SMMU handling, DMA allocation, reconnect handling, console commands and diagnostics.

The source was located through [tycykp/UniXNU-OnePlus-Ace5](https://github.com/tycykp/UniXNU-OnePlus-Ace5/tree/3c10f8847cd2073b804ac3272bc60f12bb4bc751), revision `3c10f8847cd2073b804ac3272bc60f12bb4bc751`. Its `ios16/sources.json` and `scripts/prepare-ios16.py` select that ios-vermeer-lab revision. Its `ios16/0073-giulia-complete-boot-log.patch` and terminal documentation were additional references for log capture and process diagnostics.

The [Ace5 root LICENSE at that revision](https://github.com/tycykp/UniXNU-OnePlus-Ace5/blob/3c10f8847cd2073b804ac3272bc60f12bb4bc751/LICENSE) states BSD-2-Clause-Patent, copyright 2026 UniXNU contributors. This is the license declaration of the Ace5 repository; it does not establish a license grant from the separate ios-vermeer-lab author for the directly imported USB code.

The saved, complete Git tree for ios-vermeer-lab at `d68e05df` contains no LICENSE, COPYING or NOTICE file, and the original USB source files have no explicit license declaration. Its [README](https://github.com/xyyyds0106/ios-vermeer-lab/blob/d68e05df683f863f054b0f9c063c54d2b1342b2a/README.md) preserves original authors' copyright and dependency licensing, but supplies no explicit license for its added USB implementation. The USB source also names Linux DWC3 as a protocol reference without identifying a copied Linux source revision.

The derived controller and protocol files, plus the log source and header that used the ring implementation as a reference, record the source URL, full revision and **All Rights Reserved by the original authors for upstream-derived code**. [NativeUsbDebug/NOTICE](../drivers/NativeUsbDebug/NOTICE) records the same source chain and the absence of an explicit upstream license grant. These notices retain the original authors' rights; they do not claim that the authors issued a license or grant new redistribution rights. The device repository's BSD license does not apply to that imported material.

## Inputs and generated files

[proprietary-files.txt](../proprietary-files.txt) lists the separately acquired Samsung and Apple inputs. They are not covered by this repository's source license. Keep their extracted contents, original Apple modules, trust bundles, signed root policies, private signing keys, generated kernels, FAT images and device backups out of the source publication.

Module license files listed above already accompany the copied modules. Preserve those files and all per-file notices when reorganizing the repository. Binary distributions must carry the notices required by the applicable module licenses. This inventory records the source evidence available for this port; it does not grant additional rights over third-party inputs.
