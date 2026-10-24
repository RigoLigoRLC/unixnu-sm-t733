#!/usr/bin/env python3
"""Compile the RAM Data disk or software AES provider using the current XNU recipe."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def run(command, log, cwd=None):
    log.with_suffix(".command.json").write_text(json.dumps(list(map(str, command)), indent=2) + "\n")
    with log.open("w") as stream:
        result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)

    if result.returncode:
        raise SystemExit(log.read_text()[-6000:])


def aes_bindings(sources, kernelcache, slide, output):
    sys.path.insert(0, str(sources / "kits/NativePrelink"))
    import native_prelink as np

    data = kernelcache.read_bytes()
    image = np.macho(data)
    info = next(segment for segment in image["segments"] if segment["name"] == "__PRELINK_INFO")
    offset = np.backed_offset(image["segments"], info["va"], info["filesize"], len(data))
    metadata = np.unserialize_xml(data[offset:offset + info["filesize"]])
    module = next(item for item in metadata["_PrelinkInfoDictionary"]
                  if item["CFBundleIdentifier"] == "com.apple.iokit.IOCryptoAcceleratorFamily")
    header = np.backed_offset(image["segments"], module["_PrelinkExecutableLoadAddr"], 32, len(data))
    segments = np.macho(data, header)["segments"]
    recorded = sources / "scripts/data/native-aes-original-bindings.S"
    definitions = re.findall(r"^\.set\s+(\S+),\s*(0x[0-9a-fA-F]+)$", recorded.read_text(), re.MULTILINE)
    if not definitions:
        raise ValueError("Missing original AES binding definitions")

    lines = []
    bindings = {}
    for name, address in definitions:
        original = int(address, 16) + 0x1000000
        if not any(segment["va"] <= original < segment["va"] + segment["size"]
                   for segment in segments if segment["name"] != "__LINKEDIT"):
            raise ValueError("AES binding is outside the supplied original module: " + name)

        target = original + slide
        lines.extend([".globl " + name, ".set " + name + ", " + hex(target)])
        bindings[name] = hex(target)

    assembly = output / "original-bindings.S"
    assembly.write_text("\n".join(lines) + "\n")
    return assembly, bindings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", choices=("NativeRamBlock", "NativeAES"), required=True)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--sources", type=Path,
                        help="Override the assembled tree's source modules")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--layout", type=Path)
    parser.add_argument("--kernelcache", type=Path)
    parser.add_argument("--slide", type=lambda value: int(value, 0), default=-0x1000000)
    parser.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    if args.component == "NativeRamBlock" and not args.compile_only and not (args.kernel and args.layout):
        parser.error("NativeRamBlock binding requires --kernel and --layout")

    if args.component == "NativeAES" and not args.compile_only and args.kernelcache is None:
        parser.error("NativeAES binding requires --kernelcache")

    tree = args.tree.resolve()
    sources = args.sources.resolve() if args.sources else tree
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    recipe = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
    record = json.loads(recipe.read_text().strip().rstrip(","))
    source = sources / "drivers" / args.component / (args.component + ".cpp")
    obj = output / (args.component + ".o")

    command = []
    arguments = record["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command.extend(["-o", str(obj)])
            index += 1
        elif value in ("-MF", "-MJ"):
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif value not in ("-MD", "-MP") and not value.startswith("-W"):
            command.append(value)

        index += 1

    command.extend([
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-c99-designator",
        "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
        "-Wno-unnecessary-virtual-specifier", "-Wno-missing-include-dirs",
        "-I", str(tree / ".cache/xnu-work/osfmk"),
        "-I", str(tree / "build/storage-abi/include"),
    ])
    run(command, output / "compile.log", record["directory"])
    if args.compile_only:
        print(obj)
        return

    if args.component == "NativeRamBlock":
        run([
            sys.executable, str(sources / "scripts/build-native-storage-bindings.py"),
            "--kernel", str(args.kernel.resolve()), "--layout", str(args.layout.resolve()),
            "--object", str(obj), "--class-name", "NativeRamBlock",
            "--archive-name", "NativeRamBlock", "--output", str(output),
        ], output / "binding.log")
    else:
        slide = json.loads(args.layout.read_text())["slide"] if args.layout else args.slide
        assembly, targets = aes_bindings(sources, args.kernelcache.resolve(), slide, output)
        bindings = output / "original-bindings.o"
        sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
        run([
            "xcrun", "clang", "-target", "arm64-apple-ios17.0", "-isysroot", sdk,
            "-c", str(assembly), "-o", str(bindings),
        ], output / "binding.log")
        archive = output / "libNativeAES.a"
        archive.unlink(missing_ok=True)
        subprocess.run(["xcrun", "ar", "rcs", str(archive), str(obj), str(bindings)], check=True)
        (output / "inputs.json").write_text(json.dumps({
            "compile_recipe": str(recipe), "source": str(source), "bindings": str(assembly),
            "original_build": "21H461", "original_device_class": "j120ap",
            "kernelcache": str(args.kernelcache.resolve()),
            "prelink_slide": slide, "archive": str(archive), "targets": targets,
        }, indent=2) + "\n")

    print(output / ("lib" + args.component + ".a"))


if __name__ == "__main__":
    main()
