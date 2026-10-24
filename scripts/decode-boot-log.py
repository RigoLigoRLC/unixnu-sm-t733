#!/usr/bin/env python3
"""Decode the final 32 MiB SHLOG001 window fetched with mscctl read-log."""
import argparse
import json
from pathlib import Path
import struct


BLOCK = 4096
WINDOW = 32 * 1024 * 1024
HEADER = struct.Struct("<8sIIQQ8I6I32s")
FIELDS = (
    "magic", "version", "header_bytes", "boot_id", "first_lba",
    "window_blocks", "msg_first", "msg_blocks", "msg_bytes",
    "slot_first", "slot_blocks", "slot_count", "slots_written",
    "uptime_s", "rounds", "write_errors", "gpt_open", "gpt_last", "state", "tag",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("window", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.window.stat().st_size != WINDOW:
        raise SystemExit("Input must contain the complete final 32 MiB of userdata")
    data = args.window.read_bytes()
    header = dict(zip(FIELDS, HEADER.unpack_from(data)))
    if header["magic"] != b"SHLOG001" or header["version"] != 1:
        raise SystemExit("No supported SHLOG001 version 1 header")
    if (header["header_bytes"] != HEADER.size or
            header["window_blocks"] != WINDOW // BLOCK or
            header["msg_first"] != 1 or header["msg_blocks"] != 32 or
            header["slot_first"] != 33 or header["slot_blocks"] != 16 or
            header["slot_count"] != 509 or header["msg_bytes"] > 32 * BLOCK):
        raise SystemExit("Log header does not describe the T733 bounded log layout")

    args.output.mkdir(parents=True, exist_ok=False)
    header["magic"] = header["magic"].decode("ascii")
    header["tag"] = header["tag"].split(b"\0", 1)[0].decode(errors="replace")
    (args.output / "header.json").write_text(json.dumps(header, indent=2) + "\n")
    (args.output / "msgbuf.txt").write_bytes(data[BLOCK:BLOCK + header["msg_bytes"]])
    first = max(0, header["slots_written"] - header["slot_count"])
    with (args.output / "console.txt").open("wb") as stream:
        for index in range(first, header["slots_written"]):
            block = header["slot_first"] + (index % header["slot_count"]) * header["slot_blocks"]
            begin = block * BLOCK
            slot = data[begin:begin + header["slot_blocks"] * BLOCK]
            stream.write(slot.split(b"\0", 1)[0])
            stream.write(b"\n")

    panic = data[-BLOCK:]
    if panic[:8] == b"T733PAN1":
        panic_boot, = struct.unpack_from("<Q", panic, 8)
        current = panic_boot == header["boot_id"]
        name = "panic.txt" if current else "panic-previous-boot.txt"
        message = panic[16:].split(b"\0", 1)[0]
        prefix = f"boot_id={panic_boot:#x} current_boot={current}\n".encode()
        (args.output / name).write_bytes(prefix + message)
    print(args.output)


if __name__ == "__main__":
    main()
