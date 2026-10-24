#!/usr/bin/env python3
"""Extract the two IPSW trust-cache modules and package them for DarwinBoot."""

import argparse
import json
from pathlib import Path
import struct


def der_item(data, position):
    if position + 2 > len(data):
        raise ValueError("Truncated IM4P item")

    tag = data[position]
    size = data[position + 1]
    position += 2
    if size & 0x80:
        count = size & 0x7f
        if count == 0 or count > 4 or position + count > len(data):
            raise ValueError("Invalid IM4P item length")

        size = int.from_bytes(data[position:position + count], "big")
        position += count

    end = position + size
    if end > len(data):
        raise ValueError("Truncated IM4P payload")

    return tag, data[position:end], end


def module_from_im4p(path):
    data = path.read_bytes()
    tag, sequence, end = der_item(data, 0)
    if tag != 0x30 or end != len(data):
        raise ValueError("Expected one IM4P sequence")

    fields = []
    position = 0
    while position < len(sequence):
        tag, value, position = der_item(sequence, position)
        fields.append((tag, value))

    if len(fields) != 4 or fields[0] != (0x16, b"IM4P") or fields[3][0] != 4:
        raise ValueError("Expected an IM4P trust-cache payload")

    module = fields[3][1]
    if len(module) < 24 or struct.unpack_from("<I", module)[0] != 2:
        raise ValueError("Expected a version 2 trust-cache module")

    entries = struct.unpack_from("<I", module, 20)[0]
    if len(module) != 24 + entries * 24:
        raise ValueError("Incomplete trust-cache module")

    return module, entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--static", type=Path, required=True)
    parser.add_argument("--system", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    static, static_entries = module_from_im4p(args.static)
    system, system_entries = module_from_im4p(args.system)
    bundle = struct.pack("<3I", 2, 12, 12 + len(static)) + static + system

    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "static-trustcache.bin").write_bytes(static)
    (args.output / "system-trustcache.bin").write_bytes(system)
    destination = args.output / "original-static-system.bundle"
    destination.write_bytes(bundle)
    report = {
        "static_source": str(args.static.resolve()),
        "system_source": str(args.system.resolve()),
        "static_entries": static_entries,
        "system_entries": system_entries,
        "static_bytes": len(static),
        "system_bytes": len(system),
        "bundle_bytes": len(bundle),
        "module_bytes_modified": False,
        "additional_entries": 0,
    }
    (args.output / "sources.json").write_text(json.dumps(report, indent=2) + "\n")
    print(destination)


if __name__ == "__main__":
    main()
