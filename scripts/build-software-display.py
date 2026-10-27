#!/usr/bin/env python3
"""Build T733 CPU display and key-state services against original 21H461 classes.

The builder reads the original Legacy dispatch table and generates absolute
bindings using the current prelink slide. The current XNU compile recipe builds
NativeLegacyDisplay, NativeKeyStoreEvents, and the T733 registration code. Static
constructors run when their original families have loaded during kernel startup.
"""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

REFERENCE_SLIDE = -0x1000000
ABSTRACT = {211, 213, 245, 246, 306, 307, 337, 338, 345}


def run(command, output, name, cwd=None):
    command = list(map(str, command))
    (output / (name + ".command.json")).write_text(json.dumps(command, indent=2) + "\n")
    log = output / (name + ".log")
    with log.open("w") as stream:
        result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(log.read_text()[-8000:])


def symbols(np, data):
    image = np.macho(data)
    table = next(offset for command, offset, _ in image["commands"] if command == 2)
    start, count, strings, _ = struct.unpack_from("<4I", data, table + 8)
    result = []
    for index in range(count):
        name, kind, _, _, address = struct.unpack_from("<IBBHQ", data, start + index * 16)
        end = data.index(b"\0", strings + name)
        result.append((data[strings + name:end].decode(), address, kind))
    return image, result


def compile_source(record, tree, source, destination, output):
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
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-c99-designator",
        "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
        "-Wno-unnecessary-virtual-specifier", "-Wno-missing-include-dirs",
        "-I", output, "-I", tree / ".cache/xnu-work/osfmk",
    ])
    if source.stem == "NativeLegacyDisplay":
        command.append("-DNATIVE_T733_FIRMWARE_DISPLAY=1")
    run(command, output, source.stem, record["directory"])


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
    output = tree / "build/native-legacy-display"
    output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(sources / "kits/NativePrelink"))
    import native_prelink as np

    original = args.kernelcache.read_bytes()
    image = np.macho(original)
    slide = json.loads(args.layout.read_text())["slide"]

    def read_reference(address, length):
        address -= REFERENCE_SLIDE
        start = np.backed_offset(image["segments"], address, length, len(original))
        return original[start:start + length]

    # The original constructor computes its dispatch pointer with ADRP/ADD/ADD.
    pc = 0xfffffff0057865d8
    adrp, add, header = struct.unpack("<3I", read_reference(pc, 12))
    if adrp & 0x9f00001f != 0x90000008 or add & 0xffc003ff != 0x91000108 or header != 0x91004108:
        raise ValueError("21H461 Legacy constructor does not contain the expected dispatch address")
    immediate = ((adrp >> 5) & 0x7ffff) * 4 + ((adrp >> 29) & 3)
    if immediate & (1 << 20):
        immediate -= 1 << 21
    vptr = (pc & ~4095) + (immediate << 12) + ((add >> 10) & 4095) + 16
    methods = struct.unpack("<358Q", read_reference(vptr, 358 * 8))

    declarations = {
        178: "IOReturn submitSwap(const void *, void *, void *)",
        184: "bool isMainDisplay()", 188: "IOReturn getDotPitch(uint32_t *)",
        211: "IOReturn setFrameInfoNotificationMask(uint32_t, OriginalLegacyFrameInfoSubscription *, uint32_t) = 0",
        213: "IOReturn supportedFrameInfo(uint32_t, uint32_t *, char *, uint32_t *) = 0",
        249: "void getDisplayDimensions(uint32_t *, uint32_t *)",
        288: "bool startHardware()", 290: "void kickSwapQueue()",
        295: "void completeCurrentSwap()", 296: "bool validateSwap(const OriginalLegacySwapRequest *)",
        298: "IOReturn applyDisplayState()",
        301: "bool getRefreshAttributes(uint64_t *, bool)",
        306: "void frameInfoSubscriptionChanged(OriginalLegacyFrameInfoSubscription *, bool) = 0",
        307: "IOReturn sendFrameInfo(OriginalLegacyFrameInfoSubscription *, const OriginalLegacyFrameReport *) = 0",
        336: "bool usesSharedFrameInfoQueue()",
        337: "IOReturn collectCompletionReports(uint32_t, OriginalLegacyFrameReport *, uint32_t *, uint32_t) = 0",
        345: "bool isTimingEnabled() = 0",
    }
    virtuals = []
    for slot in range(168, 358):
        declaration = declarations.get(slot)
        if declaration is None:
            declaration = f"void opaque{slot}()"
            if slot in ABSTRACT:
                declaration = "__attribute__((noreturn)) " + declaration + " = 0"
        virtuals.append("    virtual " + declaration + ";")
    template = package / "T733Display/OriginalLegacyFramebufferABI.h.in"
    (output / "OriginalLegacyFramebufferABI.h").write_text(
        template.read_text().replace("@VIRTUAL_METHODS@", "\n".join(virtuals)))
    (output / "OriginalAppleARMBacklightABI.h").write_text(
        (package / "T733Display/OriginalAppleARMBacklightABI.h.in").read_text())

    backlight_constructor = 0xfffffff0059abc1c
    backlight_destructor = 0xfffffff0059abc84
    backlight_metaclass = 0xfffffff007ade638
    backlight_vptr = 0xfffffff006ccbd18
    backlight_methods = struct.unpack("<169Q",
        read_reference(backlight_vptr + REFERENCE_SLIDE, 169 * 8))

    recipe = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
    record = json.loads(recipe.read_text().strip().rstrip(","))
    components = [
        sources / "drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp",
        sources / "drivers/NativeMobileDisplay/NativeDpuScanout.cpp",
        sources / "drivers/NativeMobileDisplay/NativeDpuSurface.cpp",
        sources / "drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp",
        package / "T733Display/DisplayRegistration.cpp",
        package / "T733Display/KeyStoreRegistration.cpp",
    ]
    objects = []
    for source in components:
        obj = output / (source.stem + ".o")
        compile_source(record, tree, source, obj, output)
        if source.stem in {"NativeLegacyDisplay", "NativeKeyStoreEvents"}:
            late = output / (source.stem + ".late.o")
            run(["llvm-objcopy", "--remove-section=__DATA,__mod_init_func",
                 "--globalize-symbol=__GLOBAL__sub_I_" + source.name, obj, late],
                output, source.stem + "-registration")
            objects.append(late)
        else:
            objects.append(obj)

    raw = (output / "NativeLegacyDisplay.o").read_bytes()
    compiled_image, names = symbols(np, raw)
    vtable = next(address for name, address, _ in names if name == "__ZTV19NativeLegacyDisplay")
    backlight_vtable = next(address for name, address, _ in names
                           if name == "__ZTV19NativeT733Backlight")
    imported = {}
    for segment in compiled_image["segments"]:
        for section in segment["sections"]:
            if not any(section["va"] <= table < section["va"] + section["size"]
                       for table in (vtable, backlight_vtable)):
                continue
            relocations, count = struct.unpack_from("<2I", raw, section["command_offset"] + 56)
            for index in range(count):
                offset, bits = struct.unpack_from("<2I", raw, relocations + index * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    imported[section["va"] + offset] = names[bits & 0xffffff][0]

    def relocated(address):
        return address - REFERENCE_SLIDE + slide

    bindings = {
        "__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass": relocated(0xfffffff0057865cc),
        "__ZN25IOMobileFramebufferLegacyD2Ev": relocated(0xfffffff005786618),
        "__ZN25IOMobileFramebufferLegacy10gMetaClassE": relocated(0xfffffff006b35940),
        "__ZTV25IOMobileFramebufferLegacy": relocated(vptr - 16),
        "__ZN17AppleARMBacklightC2EPK11OSMetaClass": backlight_constructor + slide,
        "__ZN17AppleARMBacklightD2Ev": backlight_destructor + slide,
        "__ZN17AppleARMBacklight10gMetaClassE": backlight_metaclass + slide,
        "__ZTV17AppleARMBacklight": backlight_vptr - 16 + slide,
        "_native_legacy_vsync_notify": relocated(0xfffffff005796720),
        "_native_legacy_frame_info_notify": relocated(0xfffffff005796934),
        "_native_legacy_set_refresh_properties": relocated(0xfffffff00579ebb8),
        "_native_legacy_set_properties": methods[44] + slide,
        "_native_legacy_surface_increment_use": relocated(0xfffffff004b9f750),
        "_native_legacy_surface_decrement_use": relocated(0xfffffff004b9f80c),
        "_native_legacy_surface_fence_complete": relocated(0xfffffff004bb33f0),
    }
    for slot in range(358):
        name = imported.get(vtable + 16 + slot * 8)
        if name is None:
            raise ValueError(f"Missing compiled dispatch entry {slot}")
        if "25IOMobileFramebufferLegacy" in name:
            bindings[name] = methods[slot] + slide

    for slot in range(169):
        name = imported.get(backlight_vtable + 16 + slot * 8)
        if name is None:
            raise ValueError(f"Missing compiled backlight dispatch entry {slot}")
        if "17AppleARMBacklight" in name:
            bindings[name] = backlight_methods[slot] + slide

    for name, address, field, wide in [
        ("descriptor", 0xfffffff004b9ead0, 48, True),
        ("width", 0xfffffff004b9ebbc, 88, True),
        ("height", 0xfffffff004b9ebc4, 96, True),
        ("stride", 0xfffffff004b9ebd4, 104, True),
        ("allocation", 0xfffffff004b9ebfc, 144, True),
        ("format", 0xfffffff004b9ebac, 128, False),
        ("planes", 0xfffffff004b9ebf4, 152, False),
    ]:
        load = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
        if read_reference(address, 8) != struct.pack("<2I", load, 0xd65f03c0):
            raise ValueError("Original IOSurface getter does not match field: " + name)
        bindings["_native_legacy_surface_" + name] = relocated(address)

    assembly = output / "original-bindings.S"
    assembly.write_text("".join(f".globl {name}\n.set {name}, {address:#x}\n"
                                for name, address in sorted(bindings.items())))
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    binding = output / "original-bindings.o"
    run(["xcrun", "clang", "-target", "arm64-apple-ios17.0", "-isysroot", sdk,
         "-c", assembly, "-o", binding], output, "binding")
    archive = output / "libNativeLegacyDisplay.late.a"
    archive.unlink(missing_ok=True)
    run(["xcrun", "ar", "rcs", archive, *objects, binding], output, "archive")
    (output / "build-report.json").write_text(json.dumps({
        "archive": str(archive), "kernelcache": str(args.kernelcache.resolve()),
        "slide": slide, "sources": [str(source) for source in components],
        "bindings": {name: hex(address) for name, address in bindings.items()},
        "virtual_entries": 358, "framebuffer_width": 1600, "framebuffer_height": 2560,
        "backlight_base_extent": 0x370, "backlight_virtual_entries": 169,
        "hashes_recorded": False,
    }, indent=2) + "\n")
    print(archive)


if __name__ == "__main__":
    main()
