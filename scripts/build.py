#!/usr/bin/env python3
"""The device build command runs UniXNU and stores each delivery under a new tag.

The upstream commands own source fetching, tree assembly, compilation,
prelinking and FAT assembly. This command supplies the board configuration,
archives the outputs, and optionally calls the companion firmware packager.
Device writes belong to the deployment tools and are not part of this command.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tomllib


DEVICE = Path(__file__).resolve().parents[1]


def platform_device():
    """The generated device directory supplies a complete platform board to UniXNU."""
    destination = DEVICE / "build/platform-device"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(DEVICE / "board.platform.toml", destination / "board.toml")
    for name in ("drivers", "kits", "patches", "scripts", "package", "configs", "dts", "feeds.toml",
                 "components.toml", "porting.toml", "local.toml", "unixnu.lock", ".cache"):
        source = DEVICE / name
        link = destination / name
        if source.exists() and not link.exists():
            link.symlink_to(source, target_is_directory=source.is_dir())

    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("--base", type=Path, default=os.environ.get("UNIXNU_BASE", DEVICE.parent / "UniXNU"))
    parser.add_argument("--out", type=Path, default=DEVICE / "build/work")
    parser.add_argument("--platform", action="store_true")
    parser.add_argument("--firmware", action="store_true")
    parser.add_argument("--install", action="store_true", help="Package initial userdata, BOOT and recovery")
    parser.add_argument("--uefi", type=Path, default=os.environ.get("UNIXNU_UEFI"))
    parser.add_argument("--stock-recovery", type=Path, default=os.environ.get("T733_STOCK_RECOVERY"))
    parser.add_argument("--stock-boot", type=Path, default=os.environ.get("T733_STOCK_BOOT"),
                        help="Matching BOOT image; with --firmware also packages a BOOT/recovery update")
    parser.add_argument("--root-image", type=Path, help="Signed-policy source userdata image for initial installation")
    parser.add_argument("--system-root", type=Path,
                        default=os.environ.get("T733_SYSTEM_ROOT", DEVICE / ".cache/inputs/os-root"),
                        help="Mounted original OS volume supplying the J120 wallpaper assets")
    parser.add_argument("--recovery-payload", type=Path, help="Small platform FAT embedded in recovery")
    parser.add_argument("--defconfig", type=Path, help="Private system configuration with the supplied Data UUID")
    parser.add_argument("-j", "--jobs", type=int, default=6)
    parser.add_argument("--local-module", action="append", default=[], metavar="KIND:NAME=PATH",
                        help="Use an explicit local component or SoC checkout")
    parser.add_argument("--preserve-kernel-tag", help="Save the previously resolved kernel under this tag")
    args = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.tag):
        parser.error("The tag must contain only ASCII letters, digits, dots, underscores and hyphens")
    if args.preserve_kernel_tag and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.preserve_kernel_tag):
        parser.error("The preserved kernel tag must contain only ASCII letters, digits, dots, underscores and hyphens")
    if args.install and args.platform:
        parser.error("--install requires the system configuration")
    if args.firmware or args.install:
        if not args.uefi or not args.stock_recovery:
            parser.error("Firmware packaging requires --uefi and --stock-recovery")
        if not args.platform and not args.recovery_payload:
            parser.error("System firmware requires --recovery-payload from a platform build")
    if args.install and (not args.stock_boot or not args.root_image):
        parser.error("Initial installation requires --stock-boot and --root-image")

    base = args.base.expanduser().resolve()
    out = args.out.expanduser().resolve()
    inputs = DEVICE / ".cache/inputs"
    release = DEVICE / "build/releases" / args.tag
    release.mkdir(parents=True, exist_ok=False)
    logs = release / "logs"
    logs.mkdir()
    inputs.mkdir(parents=True, exist_ok=True)
    firmware_cache = DEVICE / ".cache/firmware"
    firmware_cache.mkdir(parents=True, exist_ok=True)
    shared_inputs = firmware_cache / "t733-inputs"
    if not shared_inputs.exists():
        shared_inputs.symlink_to("../inputs", target_is_directory=True)
    environment = dict(os.environ, UNIXNU_BASE=str(base), UNIXNU_RECORD_HASHES="0", PYTHON=sys.executable)
    if (DEVICE / "local.toml").is_file():
        environment.setdefault("UNIXNU_HARDWARE_INPUT", str(DEVICE / "local.toml"))
        hardware = tomllib.loads((DEVICE / "local.toml").read_text()).get("hardware", {})
        if hardware.get("unique_chip_id"):
            environment.setdefault("UNIXNU_UNIQUE_CHIP_ID", str(hardware["unique_chip_id"]))
    status = release / "stages.tsv"
    status.write_text("stage\texit_code\tlog\n")

    def run(name, command, env=None):
        command = list(map(str, command))
        log = logs / (name + ".log")
        (logs / (name + ".command.json")).write_text(json.dumps(command, indent=2) + "\n")
        print(f"Starting {name}: {log}", flush=True)
        with log.open("w") as stream:
            result = subprocess.run(command, env=env or environment, stdout=stream, stderr=subprocess.STDOUT)
        with status.open("a") as stream:
            stream.write(f"{name}\t{result.returncode}\t{log}\n")
        if result.returncode:
            print(log.read_text(errors="replace")[-8000:], file=sys.stderr)
            raise SystemExit(result.returncode)

        return log

    def unixnu(name, command, output=out, platform=False):
        device = platform_device() if platform else DEVICE
        options = ["-D", device, "--out", output, "-j", args.jobs]
        for module in args.local_module:
            options.extend(["--local-module", module])

        return run(name, [sys.executable, base / "unixnu", *command, *options])

    def resolved_output(name, output=out, platform=False):
        log = unixnu(name, ["status", "--json"], output, platform)
        return Path(json.loads(log.read_text())["output"])

    cache = inputs / "kernelcache.decompressed"
    if not cache.is_file():
        unixnu("extract-kernelcache", ["ipsw", "fetch", "KernelCache"])
        shutil.copyfile(out / "ipsw/21H461/kernelcache.decompressed", cache)
    environment["UNIXNU_KERNELCACHE"] = str(cache)

    bindings = inputs / "bindings"
    if not args.platform and not (bindings / "kernel.json").is_file():
        binding_out = DEVICE / "build/bindings"
        unixnu("bindings-config", ["defconfig", "gts7fewifi_platform_defconfig"], binding_out, True)
        binding_work = resolved_output("bindings-output", binding_out, True)
        unixnu("bindings-kernel", ["build", "kernel"], binding_out, True)
        bindings.mkdir(parents=True, exist_ok=True)
        for name in ("kernel.macho", "kernel.json"):
            shutil.copyfile(binding_work / "kernel" / name, bindings / name)

    config = "gts7fewifi_platform_defconfig" if args.platform else "gts7fewifi_defconfig"
    if args.defconfig:
        config = args.defconfig.expanduser().resolve()
    unixnu("configure", ["defconfig", config], platform=args.platform)
    work = resolved_output("resolve", platform=args.platform)
    environment["T733_RESOLVED_TREE"] = str(work / "tree")
    current_kernel = work / "kernel"
    if args.preserve_kernel_tag and (current_kernel / "kernel.macho").is_file():
        preserved = out.parent / ("t733-kernel-" + args.preserve_kernel_tag)
        preserved.mkdir()
        for name in ("kernel.macho", "kernel.json"):
            shutil.copyfile(current_kernel / name, preserved / name)

    unixnu("kernel", ["build", "kernel"], platform=args.platform)
    unixnu("menu", ["build", "package"], platform=args.platform)

    for name in ("kernel.macho", "kernel.json"):
        shutil.copyfile(work / "kernel" / name, release / name)
    menu = work / "package/Build/Out/AARCH64-DEBUG/UniXNU.efi"
    shutil.copyfile(menu, release / "UniXNU.efi")
    fat = release / "payload.img"
    payload = ["build", "payload", "--kernel-arg", f"t733-build={args.tag}"]
    if not args.platform:
        debug_tools = release / "debug-tools"
        wallpaper = release / "wallpaper"
        gpu_tree = work / "tree"
        environment["T733_GPU_TREE"] = str(gpu_tree)
        run("turnip", [sys.executable, DEVICE / "tools/gpu/bootstrap-turnip.py",
                       "--core", gpu_tree, "--reference-core", base, "--build",
                       "--abi-directory", gpu_tree / "drivers/Adreno6xx"])
        run("metal", [sys.executable, DEVICE / "tools/gpu/bootstrap-metal.py",
                      "--core", gpu_tree, "--reference-core", base,
                      "--abi-directory", gpu_tree / "drivers/Adreno6xx",
                      "--system-root", args.system_root.expanduser().resolve()])
        run("wallpaper", [sys.executable, base / "scripts/make-wallpaper-overlay.py",
                          args.system_root.expanduser().resolve(), wallpaper])
        run("debug-tools", [sys.executable, DEVICE / "scripts/build-debug-tools.py",
                            "--output", debug_tools, "--trust-cache", inputs / "trust.bundle",
                            "--debugserver", inputs / "debugserver", "--wallpaper", wallpaper,
                            "--metal-plugin", gpu_tree / "build/adreno-metal-plugin/NativeAdrenoMetal.bundle",
                            "--metal-inputs", gpu_tree / "build/metal-inputs"])
        payload.extend(["--trust-cache", debug_tools / "trust.bundle",
                        "--gpu-addon", debug_tools / "debug.addon",
                        "--data-ramdisk", inputs / "data.dbr"])
    unixnu("payload", payload, platform=args.platform)
    shutil.copyfile(work / "payload/payload.img", fat)
    shutil.copyfile(work / "payload/payload.json", release / "payload.json")

    if args.firmware or args.install:
        uefi = args.uefi.expanduser().resolve()
        scripts = uefi / "Resources/Scripts"
        firmware_env = dict(environment, UNIXNU_TREE=str(work / "tree"),
                            T733_STOCK_RECOVERY=str(args.stock_recovery.expanduser().resolve()),
                            T733_USERDATA_PAYLOAD=str(int(not args.platform)))
        run("firmware", ["bash", scripts / "build_gts7fewifi_macos.sh", "DEBUG", args.tag], firmware_env)
        recovery_payload = fat if args.platform else args.recovery_payload.expanduser().resolve()
        run("recovery", ["bash", scripts / "package_gts7fewifi_recovery.sh", recovery_payload, args.tag], firmware_env)
        recovery = uefi / f"Build/gts7fewifiPkg/recovery-packages-{args.tag}"
        for name in ("gts7fewifi-uefi-recovery.tar", "gts7fewifi-stock-recovery.tar"):
            shutil.copyfile(recovery / name, release / name)

        if args.stock_boot:
            run("boot", [sys.executable, scripts / "package_gts7fewifi_boot.py",
                         "--recovery-image", uefi / f"Mu-gts7fewifi-{args.tag}.img",
                         "--stock-boot", args.stock_boot.expanduser().resolve(), "--output-dir", release / "boot"])
            if not args.install:
                run("firmware-package", [sys.executable, DEVICE / "scripts/package-t733-system.py",
                                         "--boot", release / "boot/boot.img",
                                         "--recovery", recovery / "uefi/recovery.img",
                                         "--output", release / "gts7fewifi-uefi.tar"])

        if args.install:
            run("userdata", [sys.executable, scripts / "compose_gts7fewifi_userdata.py",
                             args.root_image.expanduser().resolve(), fat, release / "userdata.img"])
            run("sparse", [sys.executable, DEVICE / "scripts/encode-t733-userdata-sparse.py",
                           release / "userdata.img", release / "userdata.sparse.img"])
            run("install-package", [sys.executable, DEVICE / "scripts/package-t733-system.py",
                                    "--userdata", release / "userdata.sparse.img",
                                    "--boot", release / "boot/boot.img", "--recovery", recovery / "uefi/recovery.img",
                                    "--output", release / "gts7fewifi-unixnu-system.tar"])

    print(f"Build artifacts: {release}")


if __name__ == "__main__":
    main()
