#!/usr/bin/env python3
"""The sparse encoder preserves userdata bytes using Android RAW and FILL chunks.

The input is the complete SHNGROOT image with its appended boot FAT. Each chunk
covers at most 1 MiB of that image. Zero-filled chunks become explicit FILL writes;
all other chunks retain their original bytes. The bootloader expands these chunks
while writing the existing userdata partition.
"""

import argparse
from pathlib import Path
import struct


BLOCK_SIZE = 4096
CHUNK_BYTES = 1024 * 1024
SPARSE_MAGIC = 0xED26FF3A
RAW_CHUNK = 0xCAC1
FILL_CHUNK = 0xCAC2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    size = args.source.stat().st_size
    if size == 0 or size % BLOCK_SIZE:
        parser.error("userdata length must be a positive multiple of 4096")

    chunks = (size + CHUNK_BYTES - 1) // CHUNK_BYTES
    zeroes = bytes(CHUNK_BYTES)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.source.open("rb") as source, args.output.open("xb") as output:
        output.write(struct.pack("<IHHHHIIII", SPARSE_MAGIC, 1, 0, 28, 12,
                                 BLOCK_SIZE, size // BLOCK_SIZE, chunks, 0))
        remaining = size
        while remaining:
            length = min(remaining, CHUNK_BYTES)
            data = source.read(length)
            if len(data) != length:
                raise OSError("userdata input ended before its recorded length")

            if data == zeroes[:length]:
                output.write(struct.pack("<HHII", FILL_CHUNK, 0, length // BLOCK_SIZE, 16))
                output.write(bytes(4))
            else:
                output.write(struct.pack("<HHII", RAW_CHUNK, 0, length // BLOCK_SIZE, 12 + length))
                output.write(data)

            remaining -= length

    print(f"Sparse userdata: {args.output}")
    print(f"Expanded bytes: {size}; encoded bytes: {args.output.stat().st_size}")


if __name__ == "__main__":
    main()
