#!/usr/bin/env python3
"""Build the T733 SPI touchscreen with the original 21H461 IOHIDDevice methods.

The active XNU C and C++ recipes compile the board driver and FT8203 protocol.
The original kernelcache supplies the IOHIDDevice dispatch table. Relocations
in the compiled subclass associate each inherited method with its table slot;
the builder emits bindings at the configured prelink slide. The board worker
registers the subclass after the original HID families have loaded.
"""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys


def run(command, output, name, cwd=None):
    command = list(map(str, command))
    (output / (name + ".command.json")).write_text(json.dumps(command, indent=2) + "\n")
    log = output / (name + ".log")
    with log.open("w") as stream:
        result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(log.read_text()[-10000:])


def compile_source(record, source, destination, includes, output):
    command = []
    arguments = record["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(source)
        elif value == "-o":
            command.extend(["-o", destination])
            index += 1
        elif value in ("-MF", "-MJ"):
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif value not in ("-MD", "-MP") and not value.startswith("-W"):
            command.append(value)
        index += 1
    command.extend([
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-Wno-missing-include-dirs", "-Wno-c99-designator",
    ])
    if source.suffix == ".cpp":
        command.extend([
            "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
            "-Wno-unnecessary-virtual-specifier",
        ])
    for directory in includes:
        command.extend(["-I", directory])
    run(command, output, source.stem, record["directory"])


def object_symbols(np, data):
    image = np.macho(data)
    table = next(offset for command, offset, _ in image["commands"] if command == 2)
    start, count, strings, _ = struct.unpack_from("<4I", data, table + 8)
    names = []
    for index in range(count):
        name, kind, _, _, address = struct.unpack_from("<IBBHQ", data, start + index * 16)
        end = data.index(b"\0", strings + name)
        names.append((data[strings + name:end].decode(), address, kind))
    return image, names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--sources", type=Path,
                        help="Override the assembled tree's source modules")
    parser.add_argument("--kernelcache", type=Path, required=True)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--firmware", type=Path)
    args = parser.parse_args()
    tree = args.tree.resolve()
    package = tree / "kits/gts7fewifi"
    sources = args.sources.resolve() if args.sources else tree
    output = tree / "build/t733-touch-input"
    output.mkdir(parents=True, exist_ok=True)
    firmware = (args.firmware or tree /
                ".cache/firmware/vendor/T733ZCS9DYE1/vendor/firmware/tsp_focaltech/ft8203_gts7xllite.bin").resolve()
    if not firmware.is_file():
        raise SystemExit("FT8203 firmware is missing: " + str(firmware))

    sys.path.insert(0, str(sources / "kits/NativePrelink"))
    import native_prelink as np

    original = args.kernelcache.read_bytes()
    image = np.macho(original)
    slide = json.loads(args.layout.read_text())["slide"]

    def read(address, length):
        offset = np.backed_offset(image["segments"], address, length, len(original))
        return original[offset:offset + length]

    # IOHIDDevice C2 stores the address computed by this ADRP/ADD pair in self.
    constructor = 0xfffffff005ad94f8
    pc = constructor + 12
    adrp, add, store = struct.unpack("<3I", read(pc, 12))
    if adrp & 0x9f00001f != 0x90000008 or add & 0xffc003ff != 0x91000108 or store != 0xf9000008:
        raise ValueError("The supplied kernelcache has a different IOHIDDevice constructor")
    immediate = ((adrp >> 5) & 0x7ffff) * 4 + ((adrp >> 29) & 3)
    if immediate & (1 << 20):
        immediate -= 1 << 21
    vptr = (pc & ~4095) + (immediate << 12) + ((add >> 10) & 4095)
    methods = struct.unpack("<207Q", read(vptr, 207 * 8))

    recipes = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
    cpp_recipe = recipes / "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
    c_recipe = recipes / "bsd/DEVELOPMENT/bsd_init.o.json"
    records = {
        ".cpp": json.loads(cpp_recipe.read_text().strip().rstrip(",")),
        ".c": json.loads(c_recipe.read_text().strip().rstrip(",")),
    }
    includes = [
        sources / "drivers/NativeGeniSpi", sources / "drivers/NativeFocaltechTouch",
        package, tree / ".cache/xnu-work/osfmk",
    ]
    components = [
        package / "T733Touch.cpp", package / "T733TouchHid.cpp",
        package / "T733TouchStartup.c",
        sources / "drivers/NativeFocaltechTouch/Fts8203.c",
        sources / "drivers/NativeGeniSpi/GeniSpi.c",
    ]
    objects = []
    for source in components:
        obj = output / (source.stem + ".o")
        compile_source(records[source.suffix], source, obj, includes, output)
        if source.stem == "T733TouchHid":
            late = output / "T733TouchHid.late.o"
            run(["llvm-objcopy", "--remove-section=__DATA,__mod_init_func",
                 "--globalize-symbol=__GLOBAL__sub_I_T733TouchHid.cpp", obj, late],
                output, "deferred-metaclass")
            objects.append(late)
        else:
            objects.append(obj)

    raw = (output / "T733TouchHid.o").read_bytes()
    compiled, names = object_symbols(np, raw)
    vtable = next(address for name, address, _ in names if name == "__ZTV12T733TouchHid")
    imported = {}
    for segment in compiled["segments"]:
        for section in segment["sections"]:
            if not section["va"] <= vtable < section["va"] + section["size"]:
                continue
            relocations, count = struct.unpack_from("<2I", raw, section["command_offset"] + 56)
            for index in range(count):
                offset, bits = struct.unpack_from("<2I", raw, relocations + index * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    imported[section["va"] + offset] = names[bits & 0xffffff][0]

    bindings = {
        "__ZN11IOHIDDeviceC2EPK11OSMetaClass": constructor + slide,
        "__ZN11IOHIDDeviceD2Ev": 0xfffffff005ad9518 + slide,
        "__ZN11IOHIDDevice10gMetaClassE": 0xfffffff007ae6fa0 + slide,
        "__ZTV11IOHIDDevice": vptr - 16 + slide,
    }
    for slot in range(207):
        name = imported.get(vtable + 16 + slot * 8)
        if name is None:
            raise ValueError(f"Missing compiled IOHIDDevice dispatch entry {slot}")
        if "11IOHIDDevice" in name:
            bindings[name] = methods[slot] + slide
    assembly = output / "original-bindings.S"
    assembly.write_text("".join(f".globl {name}\n.set {name}, {address:#x}\n"
                                for name, address in sorted(bindings.items())))
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    base = ["xcrun", "clang", "-target", "arm64-apple-ios17.0", "-isysroot", sdk]
    binding = output / "original-bindings.o"
    run([*base, "-c", assembly, "-o", binding], output, "bindings")
    firmware_obj = output / "T733TouchFirmware.o"
    run([*base, '-DT733_TOUCH_FIRMWARE_PATH="' + str(firmware) + '"',
         "-c", package / "T733TouchFirmware.S", "-o", firmware_obj], output, "firmware")
    archive = output / "libT733Touch.a"
    archive.unlink(missing_ok=True)
    run(["xcrun", "ar", "rcs", archive, *objects, binding, firmware_obj], output, "archive")
    (output / "build-report.json").write_text(json.dumps({
        "archive": str(archive), "sources": list(map(str, components)),
        "kernelcache": str(args.kernelcache.resolve()), "firmware": str(firmware),
        "firmware_bytes": firmware.stat().st_size, "slide": slide,
        "original_vptr": hex(vptr), "virtual_entries": 207,
        "bindings": {name: hex(address) for name, address in bindings.items()},
        "hashes_recorded": False,
    }, indent=2) + "\n")
    print(archive)


if __name__ == "__main__":
    main()
