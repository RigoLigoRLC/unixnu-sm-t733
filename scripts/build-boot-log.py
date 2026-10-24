#!/usr/bin/env python3
"""Compile the T733 persistent kernel log using the active XNU C recipe."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()
    package = tree / "kits/gts7fewifi"
    output = tree / "build/t733-boot-log"
    output.mkdir(parents=True, exist_ok=True)
    recipe = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json"
    record = json.loads(recipe.read_text().strip().rstrip(","))
    source = package / "BootLog.c"
    obj = output / "BootLog.o"
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
        elif value not in ("-MD", "-MP"):
            command.append(value)
        index += 1
    command.extend(["-Wno-missing-include-dirs", "-I", str(tree / ".cache/xnu-work/osfmk")])
    (output / "compile.command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / "compile.log").open("w") as stream:
        result = subprocess.run(command, cwd=record["directory"], stdout=stream,
                                stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit((output / "compile.log").read_text()[-6000:])
    policy_source = package / "BootWatchdogPolicy.c"
    policy_obj = output / "BootWatchdogPolicy.o"
    policy_command = [str(policy_source) if value == str(source) else
                      str(policy_obj) if value == str(obj) else value for value in command]
    (output / "policy.command.json").write_text(json.dumps(policy_command, indent=2) + "\n")
    with (output / "policy.log").open("w") as stream:
        result = subprocess.run(policy_command, cwd=record["directory"], stdout=stream,
                                stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit((output / "policy.log").read_text()[-6000:])
    archive = output / "libT733BootLog.a"
    archive.unlink(missing_ok=True)
    subprocess.run(["xcrun", "ar", "rcs", str(archive), str(obj), str(policy_obj)], check=True)
    print(archive)


if __name__ == "__main__":
    main()
