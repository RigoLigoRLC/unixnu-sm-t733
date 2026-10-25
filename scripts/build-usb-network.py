#!/usr/bin/env python3
"""Build the T733 USB Ethernet ifnet adapter with the XNU BSD compiler recipe."""

import argparse
import importlib.util
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()
    output = tree / "build/native-usb-network"
    output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location(
        "battery_build", Path(__file__).with_name("build-battery.py"))
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    recipe = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json"
    record = json.loads(recipe.read_text().strip().rstrip(","))
    source = tree / "drivers/NativeUsbNetwork/native_usb_network.c"
    obj = output / "native_usb_network.o"
    includes = [source.parent, tree / "drivers/NativeUsbDebug", tree / ".cache/xnu-work/osfmk"]
    builder.compile_source(record, source, obj, includes, output)
    archive = output / "libNativeUsbNetwork.a"
    archive.unlink(missing_ok=True)
    builder.run(["xcrun", "ar", "rcs", archive, obj], output, "archive")
    print(archive)


if __name__ == "__main__":
    main()
