# Device repository instructions

This directory is the source for UniXNU-SM-T733. The repository layout follows
UniXNU-sheng; read README.md, docs/BUILDING.md and docs/REPOSITORY_LAYOUT.md.

The published core fork is pinned by support/UniXNU.rev; its official baseline
is recorded in support/UniXNU.upstream.rev. The UEFI fork is pinned by
support/Mu-Silicium.rev. The 2026-10-24 synchronization
and remaining runtime adaptations are recorded in docs/UPSTREAM_AUDIT.md.
Read docs/XNU_PATCH_LAYOUT.md before changing XNU patches or their selection.
The GitHub repository list is in docs/GITHUB_REPOSITORIES.md and
support/repositories.json. All repository names and adaptation branches use
the unixnu- prefix. The adaptation and default branch is unixnu-sm-t733;
repository descriptions identify SM-T733 and the module purpose.
The device adaptations are published. Eight driver and kit directories are
Git submodules pinned to their published adaptation commits. Initialize them
with git submodule update --init --recursive after cloning or updating.

- Drivers and kits in this repository override same-name upstream modules.
  Edit these copies. Do not edit a second copy in the adjacent UniXNU checkout
  and leave this repository unchanged.
- XNU device patches belong in patches/t733-xnu. Keep the official patches/xnu
  module in the assembled tree. The device manifests select additions and
  optional diagnostics; same-name replacements override individual patches.
  Board services belong in package.
  Shared UniXNU payload, build and menu changes are recorded in support/UniXNU.patch,
  against support/UniXNU.upstream.rev. Bootstrap checks out the adapted core
  commit directly; the patch is a source comparison record.
- The system configuration is board.toml with gts7fewifi_defconfig. The platform
  configuration uses board.platform.toml and gts7fewifi_platform_defconfig.
  Both are complete boards. scripts/build.py supplies the platform board through
  an ignored device directory and the official -D option.
- Build and packaging have one owner: scripts/build.sh TAG. Driver development
  may run in parallel; dependent compilation and packaging remain in that command.
  Use official build kernel, build package and build payload. Prelinking belongs
  to build kernel; do not duplicate FAT assembly in a device staging script.
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
