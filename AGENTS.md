# Device repository instructions

Current GPU source: CPU cache-maintenance removal is implemented and
`system-gpu-coherent-60a` completed all nine build stages and was deployed by MSC
on 2026-10-28 with Data preserved; reboot and runtime results are pending.
The implementation, recovery commit and fixed build versions are recorded in
`docs/GPU_CPU_CACHE_REMOVAL_60.md`.
Per-frame timing source `system-gpu-frame-profile-61` completed all nine build
stages and awaits MSC deployment. Fields, limits and capture commands are in
`docs/GPU_FRAME_PROFILE_61.md`; fence reference findings are in
`docs/GPU_FENCE_WAIT_AUDIT_2026-10-28.md`.

This directory is the source for UniXNU-SM-T733. The repository layout follows
UniXNU-sheng; read README.md, docs/BUILDING.md and docs/REPOSITORY_LAYOUT.md.

The published core fork is pinned by support/UniXNU.rev; its official baseline
is recorded in support/UniXNU.upstream.rev. The UEFI fork is pinned by
support/Mu-Silicium.rev. feeds.toml pins the public unixnu-sm7325-common profile.
Read docs/BUILDING.md before changing patch selection or build inputs.
The GitHub repository list is in docs/GITHUB_REPOSITORIES.md and
support/repositories.json. All repository names and adaptation branches use
the unixnu- prefix. The adaptation and default branch is unixnu-sm-t733;
repository descriptions identify SM-T733 and the module purpose.
The device adaptations are published. Thirteen driver and kit directories are
Git submodules pinned to their published adaptation commits. Initialize them
with git submodule update --init --recursive after cloning or updating.

system-format2-48f completed seven build stages, full FAT deployment and boot.
Its USB observations and exact log paths are in
docs/archive/FORMAT2_2026-10-25.md; earlier memory and power observations retain
their original build identities.

- Drivers and kits in this repository override same-name upstream modules.
  Edit these copies. Do not edit a second copy in the adjacent UniXNU checkout
  and leave this repository unchanged.
- XNU device patches belong in patches/t733-xnu. Keep the official patches/xnu
  module in the assembled tree. Format 2 boards select ordered ARM64, SM7325,
  OS and device series; the ARM64 series names the five device replacements
  directly. The boards also explicitly select diagnostics.list while early
  markers remain disabled. Every new patch must update its selected series
  together with its selection manifest:
  additions.list for functional patches, diagnostics.list or early-entry.list
  for those optional groups. Directory presence selects no new patch. Agents
  must name the manifest they changed. Before handing a build to deployment,
  read that build's .cache/xnu-patch-selection.json or .cache/xnu-patches.list
  to confirm that the build selected and applied each required patch. A missing
  entry requires a corrected build before deployment. This is part of build
  delivery and requires neither additional tests nor confirmation hashes.
  Hardware drivers belong in independent public repositories referenced by
  drivers/<Name> submodules, each carrying the unixnu topic. Board configuration
  and component registration belong in package. Follow docs/UPSTREAMING.md for
  the pending extraction of existing hardware code from package. Community
  device drivers remain maintainer-owned; official-device and common-stack
  changes may be contributed to UniXNU.
  Shared UniXNU payload, build and menu changes are recorded in support/UniXNU.patch,
  against support/UniXNU.upstream.rev. Bootstrap checks out the adapted core
  commit directly; the patch is a source comparison record.
- The system configuration is board.toml with gts7fewifi_defconfig. The platform
  configuration uses board.platform.toml and gts7fewifi_platform_defconfig.
  Both are complete format 2 boards. The pinned SM7325 profile owns shared DTSI,
  configuration and kernel patches; dts/board.dts owns the device memory span.
  components.toml retains community component sources and board archive recipes.
  porting.toml separates the existing implementation from new build/runtime results.
  scripts/build.py supplies the platform board through
  an ignored device directory and the official -D option.
  The resolved output is out/DEVICE/OS_PROFILE/CONFIG, read from status --json.
  Public boards retain hexadecimal payload chip_id 733 for platform UUID input.
  With personalization_from_ipsw enabled, chosen chip-id, board-id and
  security-domain come from the selected OS identity. The original chip value
  is carried in unixnu,platform-uuid-chip-id and consumed only by UUID generation.
  local.toml keeps unique_chip_id private; build.py passes it through
  UNIXNU_UNIQUE_CHIP_ID. Preserve this identity and the software keystore key.
- Build and packaging have one owner: scripts/build.sh TAG. Driver development
  may run in parallel; dependent compilation and packaging remain in that command.
  Use official build kernel, build package and build payload. Prelinking belongs
  to build kernel; do not duplicate FAT assembly in a device staging script.
- Every build requires committed source. Commit each changed module, including
  new files, then commit its parent gitlink and component revision together.
  Pin the core and SoC commits before building from clean working trees. Record
  all selected commits and the build command with an immutable build tag.
  Private inputs and keys remain ignored. Never assign today's commits to an
  older binary whose uncommitted source was not preserved.
- Use the existing unixnu-sm-t733 adaptation branch. Do not create checkpoint
  branches for individual builds; ordinary commits and build tags record them.
- Never calculate file hashes for confirmation. Keep UNIXNU_RECORD_HASHES=0.
  Do not add or run tests unless the user requests them.
- Preserve the fixed Data seed, UUID and package/data-layout.toml during ordinary
  FAT updates. Do not execute device writes as part of a build.
- Keep firmware, IPSWs, extracted Apple code, images, signing keys, local identity,
  captures and build products in ignored directories.
- Keep each module's license. NativeUsbDebug/NOTICE and support/SOURCES.md record
  the original authors' retained rights and absence of an explicit upstream
  license grant; do not assign it another module's license.
- Record actual hardware progress in docs/DRIVERS.md. Mark complete items with
  checked boxes and strikethrough, including the date and relevant source.
