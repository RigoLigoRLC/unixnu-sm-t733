#!/usr/bin/env python3
"""Build the A6xx driver, shared SMMU/PM4 code and private A660 firmware inputs."""

import argparse
import importlib.util
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--driver", type=Path)
    parser.add_argument("--package", type=Path)
    args = parser.parse_args()
    tree = args.tree.resolve()
    driver = (args.driver or tree / "drivers/Adreno6xx").resolve()
    package = (args.package or tree / "kits/gts7fewifi").resolve()
    output = tree / "build/adreno6xx"
    output.mkdir(parents=True, exist_ok=True)

    spec = importlib.util.spec_from_file_location(
        "battery_build", Path(__file__).with_name("build-battery.py"))
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    recipes = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
    records = {
        ".cpp": json.loads((recipes / "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json").read_text().strip().rstrip(",")),
        ".c": json.loads((recipes / "bsd/DEVELOPMENT/bsd_init.o.json").read_text().strip().rstrip(",")),
    }
    includes = [driver, tree / "kits/ArmSmmu4K", tree / "kits/AdrenoPM4",
                tree / "kits/AdrenoFirmware", tree / ".cache/xnu-work/osfmk"]
    driver_sources = [
        "NativeAdrenoService.cpp", "NativeAdrenoA6xxHwInit.cpp", "NativeAdrenoA6xxZap.cpp",
        "NativeAdrenoA6xxPower.cpp", "NativeAdrenoA6xxGmu.cpp",
        "NativeAdrenoMemory.cpp", "NativeAdrenoCpu.cpp", "NativeAdrenoSmmuMemory.cpp",
        "NativeAdrenoSmmuContext.cpp", "NativeAdrenoVm.cpp", "NativeAdrenoGmuMemory.cpp",
        "NativeAdrenoGmuContext.cpp", "NativeAdrenoGpuContext.cpp", "NativeAdrenoRing.cpp",
        "NativeAdrenoCpInit.cpp", "NativeAdrenoIrq.cpp", "NativeAdrenoSubmit.cpp",
        "NativeAdrenoTimer.cpp", "NativeAdrenoKernelTime.cpp", "NativeAdrenoUserClient.cpp",
        "NativeAdrenoClient.cpp", "NativeAdrenoPublish.cpp",
    ]
    sources = [driver / name for name in driver_sources]
    sources.extend(tree / "kits/ArmSmmu4K" / (name + ".c") for name in (
        "ArmSmmu4K", "ArmSmmu4KAudit", "ArmSmmu4KContext", "ArmSmmu4KLive",
        "ArmSmmu4KMmio", "ArmSmmu4KProgram", "ArmSmmu4KStreams", "ArmSmmu4KTableMemory"))
    sources.extend([
        tree / "kits/AdrenoPM4/AdrenoPM4.c",
        package / "T733GpuResources.cpp", package / "T733GpuStartup.c",
    ])
    objects = []
    for source in sources:
        obj = output / (source.stem + ".o")
        builder.compile_source(records[source.suffix], source, obj, includes, output)
        if source.stem == "NativeAdrenoPublish":
            late = output / "NativeAdrenoPublish.late.o"
            builder.run(["llvm-objcopy", "--remove-section=__DATA,__mod_init_func",
                         "--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoPublish.cpp", obj, late],
                        output, "deferred-metaclass")
            objects.append(late)
        else:
            objects.append(obj)

    vendor = tree / ".cache/firmware/vendor/T733ZCS9DYE1"
    firmware = [
        ("native_adreno_firmware_image", vendor / "vendor/firmware/a660_gmu.bin"),
        ("native_adreno_sqe_firmware_image", vendor / "vendor/firmware/a660_sqe.fw"),
        ("native_adreno_zap_metadata_image", vendor / "apnhlos/image/a660_zap.mdt"),
        ("native_adreno_zap_shader_image", vendor / "apnhlos/image/a660_zap.b02"),
    ]
    assembly = []
    for symbol, source in firmware:
        if not source.is_file():
            raise SystemExit(f"Missing private GPU firmware input: {source}")

        assembly.extend([
            ".section __TEXT,__const", ".balign 16", f".globl _{symbol}",
            f"_{symbol}:", f'.incbin "{source.resolve()}"',
            ".balign 8", f".globl _{symbol}_size", f"_{symbol}_size:",
            f".quad {source.stat().st_size}",
        ])
    image = output / "A660Firmware.S"
    image.write_text("\n".join(assembly) + "\n")
    image_object = output / "A660Firmware.o"
    builder.run(["xcrun", "clang", "-target", "arm64-apple-ios17.0", "-c",
                 image, "-o", image_object], output, "firmware")
    objects.append(image_object)

    archive = output / "libAdreno6xx.a"
    archive.unlink(missing_ok=True)
    builder.run(["xcrun", "ar", "rcs", archive, *objects], output, "archive")
    print(archive)


if __name__ == "__main__":
    main()
