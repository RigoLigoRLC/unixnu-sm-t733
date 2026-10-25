#!/usr/bin/env python3
"""Build the ARM64/iOS A642L diagnostic command and launchd job.

The caller adds the executable to the boot addon and its CodeDirectory identity
to the owner trust module. The job runs the requested stage once after launchd
starts; the command writes its actual ABI replies and GPU results to stdout and
syslog. Build output remains outside the source directory.
"""

import argparse
import json
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess


DEVICE = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--abi-directory", type=Path, required=True)
    parser.add_argument("--pm4-directory", type=Path, required=True)
    parser.add_argument("--stage", choices=("query", "map", "submit"), default="submit")
    parser.add_argument("--turnip-library", type=Path)
    parser.add_argument("--core", type=Path,
                        help="UniXNU checkout containing Mesa, AdrenoMetal and glslang build inputs")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    abi = args.abi_directory.resolve()
    pm4 = args.pm4_directory.resolve()
    sdk = subprocess.check_output(
        ["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    common = ["-target", "arm64-apple-ios17.0", "-isysroot", sdk,
              "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fstack-protector-strong"]
    pm4_object = output / "AdrenoPM4.o"
    subprocess.run(["xcrun", "clang", *common, "-std=c11", "-c",
                    str(pm4 / "AdrenoPM4.c"), "-o", str(pm4_object)], check=True)
    binary = output / "t733-adreno"
    subprocess.run(["xcrun", "clang++", *common, "-std=c++17", "-fno-exceptions",
                    "-fno-rtti", "-I", str(abi), "-I", str(pm4),
                    str(DEVICE / "tools/gpu/t733-adreno.cpp"), str(pm4_object),
                    "-o", str(binary)], check=True)

    entitlements = output / "entitlements.plist"
    entitlements.write_bytes(plistlib.dumps({
        "com.apple.private.security.no-container": True,
        "com.apple.security.iokit-user-client-class": ["AGXDeviceUserClient"],
    }))
    subprocess.run(["codesign", "--force", "--sign", "-", "--pagesize", "4096",
                    "--identifier", "org.unixnu.t733.adreno",
                    "--entitlements", str(entitlements), str(binary)], check=True)

    job = output / "org.unixnu.t733.adreno.plist"
    job.write_bytes(plistlib.dumps({
        "Label": "org.unixnu.t733.adreno",
        "ProgramArguments": ["/private/var/ngpu/bin/t733-adreno", "--stage", args.stage],
        "RunAtLoad": True,
        "KeepAlive": False,
        "UserName": "root",
        "LimitLoadToSessionType": "System",
        "ProcessType": "Interactive",
        "StandardOutPath": "/dev/console",
        "StandardErrorPath": "/dev/console",
    }))
    manifest = {
        "binary": str(binary),
        "job": str(job),
        "addon_binary_path": "bin/t733-adreno",
        "addon_job_path": "daemons/org.unixnu.t733.adreno.plist",
        "trust_constraint_category": 0,
        "stage": args.stage,
        "abi_header": str(abi / "NativeAdrenoABI.h"),
    }
    if args.turnip_library:
        if args.core is None:
            parser.error("--turnip-library requires --core")

        core = args.core.resolve()
        glslang = core / ".cache/mesa-tools/bin/glslangValidator"
        shader_directory = core / "drivers/AdrenoMetal/shaders"
        header_lines = ["// Generated GLSL compiler output.", "#pragma once", "#include <stdint.h>"]
        for source in sorted(shader_directory.glob("smoke_*")):
            destination = output / (source.name + ".spv")
            subprocess.run([str(glslang), "-V", "--target-env", "vulkan1.1",
                            str(source), "-o", str(destination)], check=True)
            data = destination.read_bytes()
            words = struct.unpack(f"<{len(data) // 4}I", data)
            name = "kSpv_" + source.name.replace(".", "_")
            header_lines.append(f"static const uint32_t {name}[] = {{")
            for index in range(0, len(words), 8):
                header_lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
            header_lines.append("};")
        (output / "NativeTurnipShaders.h").write_text("\n".join(header_lines) + "\n")

        turnip_command = output / "t733-turnip"
        subprocess.run(["xcrun", "clang++", *common, "-std=c++17", "-Wno-unused-function",
                        "-I", str(core / "drivers/AdrenoMetal"),
                        "-I", str(core / ".cache/mesa-work/include"), "-I", str(output),
                        str(DEVICE / "tools/gpu/t733-turnip.cpp"), "-o", str(turnip_command)], check=True)
        library = output / "libvulkan_freedreno.dylib"
        shutil.copyfile(args.turnip_library, library)
        for executable, identifier in ((turnip_command, "org.unixnu.t733.turnip-command"),
                                       (library, "org.unixnu.t733.turnip")):
            signing = ["codesign", "--force", "--sign", "-", "--pagesize", "4096",
                       "--identifier", identifier]
            if executable == turnip_command:
                signing.extend(["--entitlements", str(entitlements)])
            subprocess.run([*signing, str(executable)], check=True)

        manifest["turnip_command"] = str(turnip_command)
        manifest["turnip_library"] = str(library)
        manifest["turnip_source"] = str(core / ".cache/mesa-work")

    (output / "userspace.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"A642L userspace command and launchd job: {output}")


if __name__ == "__main__":
    main()
