#!/usr/bin/env python3
"""Build the T733 SM5714 reader and native IOPMPowerSource service."""

import argparse
import json
from pathlib import Path
import subprocess


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()
    package = tree / "kits/gts7fewifi"
    output = tree / "build/t733-battery"
    output.mkdir(parents=True, exist_ok=True)
    recipes = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
    records = {
        ".cpp": json.loads((recipes / "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json").read_text().strip().rstrip(",")),
        ".c": json.loads((recipes / "bsd/DEVELOPMENT/bsd_init.o.json").read_text().strip().rstrip(",")),
    }
    includes = [package, tree / ".cache/xnu-work/osfmk"]
    objects = []
    for name in ("T733Battery.cpp", "T733BatteryStartup.c", "T733FuelGauge.cpp"):
        source = package / name
        obj = output / (source.stem + ".o")
        compile_source(records[source.suffix], source, obj, includes, output)
        if source.stem == "T733Battery":
            late = output / "T733Battery.late.o"
            run(["llvm-objcopy", "--remove-section=__DATA,__mod_init_func",
                 "--globalize-symbol=__GLOBAL__sub_I_T733Battery.cpp", obj, late],
                output, "deferred-metaclass")
            objects.append(late)
        else:
            objects.append(obj)

    archive = output / "libT733Battery.a"
    archive.unlink(missing_ok=True)
    run(["xcrun", "ar", "rcs", archive, *objects], output, "archive")
    print(archive)


if __name__ == "__main__":
    main()
