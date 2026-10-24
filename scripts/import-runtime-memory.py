#!/usr/bin/env python3
"""The runtime memory importer records DRAM and reserved nodes from a device FDT.

The command decodes the FDT structure and reservation table, then writes a JSON
record of memory intervals and reserved-memory properties. UEFI consumes these
physical intervals when defining its memory map; DarwinBoot allocates its XNU
window from the memory descriptors supplied by UEFI.
"""

import argparse
import json
from pathlib import Path
import struct


def read_nodes(data):
    fields = struct.unpack_from(">10I", data)
    if fields[0] != 0xD00DFEED:
        raise ValueError("Input is not an FDT")

    offset = fields[2]
    strings = fields[3]
    nodes = {}
    stack = []

    while True:
        token = struct.unpack_from(">I", data, offset)[0]
        offset += 4

        if token == 1:
            end = data.index(0, offset)
            stack.append(data[offset:end].decode("ascii"))
            offset = (end + 4) & ~3
            path = "/".join(stack) or "/"
            nodes[path] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name_offset = struct.unpack_from(">II", data, offset)
            offset += 8
            name_start = strings + name_offset
            name_end = data.index(0, name_start)
            name = data[name_start:name_end].decode("ascii")
            path = "/".join(stack) or "/"
            nodes[path][name] = data[offset:offset + length]
            offset = (offset + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            break
        else:
            raise ValueError(f"Unexpected FDT structure token {token}")

    reservations = []
    offset = fields[4]
    while True:
        base, size = struct.unpack_from(">QQ", data, offset)
        offset += 16
        if base == 0 and size == 0:
            break

        reservations.append(interval(base, size))

    return nodes, reservations


def interval(base, size):
    return {
        "base": hex(base),
        "size": hex(size),
        "end_exclusive": hex(base + size),
    }


def regions(raw, address_cells, size_cells):
    width = (address_cells + size_cells) * 4
    result = []

    for offset in range(0, len(raw), width):
        split = offset + address_cells * 4
        base = int.from_bytes(raw[offset:split], "big")
        size = int.from_bytes(raw[split:offset + width], "big")
        result.append(interval(base, size))

    return result


def cell_count(properties, name):
    return int.from_bytes(properties[name], "big")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    nodes, reservations = read_nodes(args.input.read_bytes())
    root = nodes["/"]
    address_cells = cell_count(root, "#address-cells")
    size_cells = cell_count(root, "#size-cells")
    memory = []

    for path, properties in nodes.items():
        if properties.get("device_type") == b"memory\0":
            memory.extend(regions(properties["reg"], address_cells, size_cells))

    reserved_parent = nodes["/reserved-memory"]
    address_cells = cell_count(reserved_parent, "#address-cells")
    size_cells = cell_count(reserved_parent, "#size-cells")
    reserved = []

    for path, properties in nodes.items():
        if not path.startswith("/reserved-memory/") or path.count("/") != 2:
            continue

        entry = {
            "path": path,
            "status": properties.get("status", b"okay\0").rstrip(b"\0").decode("ascii"),
            "no_map": "no-map" in properties,
            "reusable": "reusable" in properties,
        }
        for name in ("reg", "alloc-ranges"):
            if name in properties:
                entry[name] = regions(properties[name], address_cells, size_cells)

        for name in ("size", "non_gki_size", "alignment"):
            if name in properties:
                entry[name] = hex(int.from_bytes(properties[name], "big"))

        reserved.append(entry)

    base = min(int(item["base"], 16) for item in memory)
    end = max(int(item["end_exclusive"], 16) for item in memory)
    result = {
        "source": str(args.input),
        "dram_envelope": interval(base, end - base),
        "memory_reg_total_bytes": hex(sum(int(item["size"], 16) for item in memory)),
        "memory": memory,
        "fdt_reservations": reservations,
        "reserved_memory": reserved,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Runtime memory records written: {args.output}")


if __name__ == "__main__":
    main()
