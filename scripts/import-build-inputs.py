#!/usr/bin/env python3
"""The input importer copies owner-supplied build files into the ignored cache.

Only the signed manifests and public anchors are copied from a policy directory.
The private signing key remains at its original location. Existing inputs are
retained unless the caller explicitly selects --replace.
"""

import argparse
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernelcache", type=Path)
    parser.add_argument("--bindings", type=Path, help="Directory containing kernel.macho and kernel.json")
    parser.add_argument("--policies", type=Path)
    parser.add_argument("--trust-cache", type=Path)
    parser.add_argument("--data-seed", type=Path)
    parser.add_argument("--replace", action="store_true")
    args = parser.parse_args()
    destination = Path(__file__).resolve().parents[1] / ".cache/inputs"
    files = []
    for source, name in ((args.kernelcache, "kernelcache.decompressed"),
                         (args.trust_cache, "trust.bundle"), (args.data_seed, "data.dbr")):
        if source:
            files.append((source, name))
    if args.bindings:
        for name in ("kernel.macho", "kernel.json"):
            files.append((args.bindings / name, "bindings/" + name))
    if args.policies:
        for component in ("os", "systemos"):
            for name in ("manifest.bin", "trusted-key.bin"):
                relative = component + "/" + name
                files.append((args.policies / relative, "policies/" + relative))
    for source, relative in files:
        target = destination / relative
        if target.exists() and not args.replace:
            raise FileExistsError(f"Input already exists: {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source.expanduser().resolve(), target)
        print(target)


if __name__ == "__main__":
    main()
