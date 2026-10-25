#!/usr/bin/env python3
"""Compile the QCOM DSI command sender and T733 display power coordinator."""
import argparse
import importlib.util
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--sources", type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()
    sources = args.sources.resolve()
    package = tree / "kits/gts7fewifi"
    output = tree / "build/t733-display-power"
    output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("touch_builder", Path(__file__).with_name("build-touch-input.py"))
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    recipes = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
    records = {
        ".c": json.loads((recipes / "bsd/DEVELOPMENT/bsd_init.o.json").read_text().strip().rstrip(",")),
        ".cpp": json.loads((recipes / "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json").read_text().strip().rstrip(",")),
    }
    components = [
        sources / "drivers/NativeQcomDsi/QcomDsi.c",
        sources / "drivers/NativeGeniI2c/NativeGeniI2c.cpp",
        package / "T733Display/DisplayI2c.cpp",
        package / "T733Display/DisplayPower.cpp",
    ]
    objects = []
    for source in components:
        obj = output / (source.stem + ".o")
        builder.compile_source(records[source.suffix], source, obj,
                               [sources / "drivers/NativeQcomDsi", sources / "drivers/NativeGeniI2c",
                                tree / ".cache/xnu-work/osfmk"], output)
        objects.append(obj)
    archive = output / "libT733DisplayPower.a"
    archive.unlink(missing_ok=True)
    builder.run(["xcrun", "ar", "rcs", archive, *objects], output, "archive")
    print(archive)


if __name__ == "__main__":
    main()
