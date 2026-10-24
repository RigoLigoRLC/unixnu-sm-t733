#!/usr/bin/env python3
"""The system tar supplies userdata and both Samsung boot entries to samloader."""

import argparse
from pathlib import Path
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--userdata", type=Path, required=True)
    parser.add_argument("--boot", type=Path, required=True)
    parser.add_argument("--recovery", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    # GNU numeric fields accommodate the userdata member exceeding 8 GiB.
    with tarfile.open(args.output, "x", format=tarfile.GNU_FORMAT) as archive:
        for name in ("userdata", "boot", "recovery"):
            archive.add(getattr(args, name), arcname=f"{name}.img", recursive=False)

    print(f"System package: {args.output}")


if __name__ == "__main__":
    main()
