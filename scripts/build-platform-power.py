#!/usr/bin/env python3
"""Build the board provider and bind the original 21H461 ApplePMGR methods."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def run(command, output, name, cwd=None):
    command = list(map(str, command))
    (output / (name + ".command.json")).write_text(json.dumps(command, indent=2) + "\n")
    with (output / (name + ".log")).open("w") as stream:
        result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit((output / (name + ".log")).read_text()[-6000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--sources", type=Path,
                        help="Override the assembled tree's source modules")
    parser.add_argument("--kernelcache", type=Path, required=True)
    parser.add_argument("--layout", type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()
    package = tree / "kits/gts7fewifi"
    sources = args.sources.resolve() if args.sources else tree
    output = tree / "build/sheng-platform-power"
    output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(sources / "kits/NativePrelink"))
    import native_prelink as np

    data = args.kernelcache.read_bytes()
    image = np.macho(data)
    info = next(segment for segment in image["segments"] if segment["name"] == "__PRELINK_INFO")
    offset = np.backed_offset(image["segments"], info["va"], info["filesize"], len(data))
    metadata = np.unserialize_xml(data[offset:offset + info["filesize"]])
    module = next(item for item in metadata["_PrelinkInfoDictionary"]
                  if item["CFBundleIdentifier"] == "com.apple.driver.ApplePMGR")
    offset = np.backed_offset(image["segments"], module["_PrelinkExecutableLoadAddr"], 32, len(data))
    segments = np.macho(data, offset)["segments"]
    slide = json.loads(args.layout.read_text())["slide"]

    # These 21H461 addresses name original class methods and the nub factory.
    original = {
        "__ZN9ApplePMGRC2EPK11OSMetaClass": 0xfffffff0059f003c,
        "__ZN9ApplePMGRD2Ev": 0xfffffff0059f019c,
        "__ZN9ApplePMGR10gMetaClassE": 0xfffffff007adf490,
        "__ZN9ApplePMGR7readAonEv": 0xfffffff005a20fbc,
        "__ZN9ApplePMGR8writeAonEy": 0xfffffff005a20fc4,
        "_sheng_original_pmgr_nub_create": 0xfffffff005a22568,
    }
    bindings = {}
    assembly = []
    for name, address in original.items():
        if not any(segment["va"] <= address < segment["va"] + segment["size"]
                   for segment in segments if segment["name"] != "__LINKEDIT"):
            raise ValueError("PMGR address is outside the supplied original module: " + name)
        target = address + slide
        bindings[name] = hex(target)
        assembly.extend([".globl " + name, ".set " + name + ", " + hex(target)])
    (output / "original-bindings.S").write_text("\n".join(assembly) + "\n")

    header = [
        "#pragma once", "#include <IOKit/IOPMGR.h>",
        "// ApplePMGR preserves the original provider size and virtual method order.",
        "class ApplePMGR : public IOPMGR {",
        "    OSDeclareAbstractStructors(ApplePMGR)",
        "    unsigned char originalPrivate[0x4b8a8 - sizeof(IOPMGR)];", "public:",
    ]
    guards = []
    for slot in range(177, 340):
        if slot == 320:
            header.append("    virtual uint32_t readAon();")
        elif slot == 321:
            header.append("    virtual void writeAon(uint64_t);")
        else:
            header.append(f"    virtual void opaque{slot}() = 0;")
            guards.extend([
                f"    void opaque{slot}() override", "    {",
                f"        unavailable({slot});", "    }",
            ])
    header.extend(["};", 'static_assert(sizeof(ApplePMGR) == 0x4b8a8, "Original provider storage");'])
    (output / "OriginalPMGRABI.h").write_text("\n".join(header) + "\n")
    (output / "GuardedPMGRSlots.inc").write_text("\n".join(guards) + "\n")

    recipe = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
    record = json.loads(recipe.read_text().strip().rstrip(","))
    source = package / "ShengPlatformPower.cpp"
    obj = output / "ShengPlatformPower.o"
    command = []
    arguments = record["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(source)
        elif value == "-o":
            command.extend(["-o", obj])
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
        "-I", output, "-I", tree / ".cache/xnu-work/osfmk",
    ])
    run(command, output, "compile", record["directory"])
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    run([
        "xcrun", "clang", "-target", "arm64-apple-ios17.0", "-isysroot", sdk,
        "-c", output / "original-bindings.S", "-o", output / "original-bindings.o",
    ], output, "binding")
    late = output / "ShengPlatformPower.late.o"
    run([
        "llvm-objcopy", "--remove-section=__DATA,__mod_init_func",
        "--globalize-symbol=__GLOBAL__sub_I_ShengPlatformPower.cpp", obj, late,
    ], output, "registration")
    archive = output / "libShengPlatformPower.a"
    archive.unlink(missing_ok=True)
    run(["xcrun", "ar", "rcs", archive, late, output / "original-bindings.o"], output, "archive")
    (output / "inputs.json").write_text(json.dumps({
        "source": str(source), "compile_recipe": str(recipe),
        "kernelcache": str(args.kernelcache.resolve()), "slide": slide,
        "bindings": bindings, "archive": str(archive),
    }, indent=2) + "\n")
    print(archive)


if __name__ == "__main__":
    main()
