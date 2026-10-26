#!/usr/bin/env python3
"""Build the SM7325 CPU policy, scheduler adapter and TSENS temperature reader."""

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
    output = tree / "build/t733-cpu-power"
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
    tsens = tree / "drivers/NativeQcomTsens"
    includes = [package, tsens, tree / ".cache/xnu-work/osfmk"]
    sources = [
        package / "T733CpuPower.cpp",
        package / "T733CpuPowerStartup.c",
        package / "T733Thermal.cpp",
        tsens / "NativeQcomTsens.cpp",
    ]
    objects = []
    for source in sources:
        obj = output / (source.stem + ".o")
        builder.compile_source(records[source.suffix], source, obj, includes, output)
        objects.append(obj)
    archive = output / "libT733CpuPower.a"
    archive.unlink(missing_ok=True)
    builder.run(["xcrun", "ar", "rcs", archive, *objects], output, "archive")
    print(archive)


if __name__ == "__main__":
    main()
