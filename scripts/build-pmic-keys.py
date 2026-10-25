#!/usr/bin/env python3
"""Build the board PMIC registration, SPMI controller and button driver.

The existing touchscreen archive supplies the shared original IOHIDDevice
bindings. This archive adds the Consumer Control subclass and defers its
metaclass initialization until the original HID family has loaded.
"""

import argparse
import importlib.util
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()
    package = tree / "kits/gts7fewifi"
    output = tree / "build/t733-pmic-keys"
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
    spmi = tree / "drivers/NativeQcomSpmi"
    keys = tree / "drivers/NativePmicKeys"
    includes = [package, spmi, keys, tree / ".cache/xnu-work/osfmk"]
    sources = [
        package / "T733PmicKeys.cpp",
        package / "T733PmicKeysHid.cpp",
        package / "T733PmicKeysStartup.c",
        spmi / "NativeQcomSpmi.cpp",
        keys / "NativePmicKeys.cpp",
    ]
    objects = []
    for source in sources:
        obj = output / (source.stem + ".o")
        builder.compile_source(records[source.suffix], source, obj, includes, output)
        if source.stem == "T733PmicKeysHid":
            late = output / "T733PmicKeysHid.late.o"
            builder.run(["llvm-objcopy", "--remove-section=__DATA,__mod_init_func",
                         "--globalize-symbol=__GLOBAL__sub_I_T733PmicKeysHid.cpp", obj, late],
                        output, "deferred-metaclass")
            objects.append(late)
        else:
            objects.append(obj)
    archive = output / "libT733PmicKeys.a"
    archive.unlink(missing_ok=True)
    builder.run(["xcrun", "ar", "rcs", archive, *objects], output, "archive")
    print(archive)


if __name__ == "__main__":
    main()
