#!/usr/bin/env python3
"""Download completed GPU capture files through the fixed USB TCP service."""

import argparse
from datetime import datetime, timezone
import errno
import json
import os
from pathlib import Path
import re
import socket
import struct
import sys


MAGIC = 0x52444853
VERSION = 1
HEADER = struct.Struct("<8I")
FILE = struct.Struct("<QQ128s")
LIST_PROCESSES = 1
LIST_FILES = 2
READ_FILE = 3
LIST_SESSIONS = 4
TRIGGER_PM4 = 5
MAX_PROCESSES = 256
MAX_SESSIONS = 256
MAX_FILES = 4100
MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_SESSION_BYTES = 146 * 1024 * 1024
CHUNK_BYTES = 64 * 1024
CAPTURE_ROOT = "/private/var/mobile/Library/Caches/NativeAdrenoShaderCapture"
NAME = re.compile(r"s([1-9][0-9]*)_[a-z0-9_-]+\.(json|txt|metallib|air|spv|rd)")


def receive_exact(connection, count):
    data = bytearray()
    while len(data) < count:
        chunk = connection.recv(min(CHUNK_BYTES, count - len(data)))
        if not chunk:
            raise ConnectionError("Capture connection closed before the reply completed")
        data.extend(chunk)

    return bytes(data)


def decode_file(data):
    size, modified_ns, raw_name = FILE.unpack(data)
    terminator = raw_name.find(b"\0")
    if terminator < 1 or any(raw_name[terminator:]):
        raise ValueError("Invalid capture filename padding")

    name = raw_name[:terminator].decode("ascii")
    match = NAME.fullmatch(name)
    if not match or int(match[1]) > 0xffffffffffffffff or size > MAX_FILE_BYTES:
        raise ValueError("Invalid capture filename or file length")

    return {"name": name, "bytes": size, "modified_ns": modified_ns,
            "session": match[1]}


def encode_file(file):
    return FILE.pack(file["bytes"], file["modified_ns"], file["name"].encode("ascii"))


class CaptureClient:
    """CaptureClient lists completed files and streams each selected version to disk.

    The server supplies a file length and modification time for each filename.
    A download uses that descriptor in the request, receives bounded chunks,
    and publishes its local file after the server confirms the final descriptor.
    """

    def __init__(self, connection):
        self.connection = connection
        self.request_id = 0

    def send(self, operation, payload=b""):
        self.request_id += 1
        self.connection.sendall(HEADER.pack(
            MAGIC, VERSION, operation, self.request_id, 0, len(payload), 0, 0) + payload)

    def reply(self, operation, maximum):
        values = HEADER.unpack(receive_exact(self.connection, HEADER.size))
        magic, version, actual, request_id, status, length, reserved0, reserved1 = values
        if (magic != MAGIC or version != VERSION or actual != operation or
                request_id != self.request_id or reserved0 or reserved1 or length > maximum):
            raise ValueError("Invalid capture reply header")
        if status:
            if length:
                raise ValueError("Error reply contains a payload")
            raise OSError(status, os.strerror(status))

        return receive_exact(self.connection, length)

    def processes(self):
        self.send(LIST_PROCESSES)
        data = self.reply(LIST_PROCESSES, MAX_PROCESSES * 4)
        if len(data) % 4:
            raise ValueError("Invalid process list length")

        processes = [entry[0] for entry in struct.iter_unpack("<I", data)]
        if any(pid < 1 or pid > 0x7fffffff for pid in processes) or len(set(processes)) != len(processes):
            raise ValueError("Invalid process list")

        return sorted(processes)

    def sessions(self, pid):
        self.send(LIST_SESSIONS, struct.pack("<I", pid))
        data = self.reply(LIST_SESSIONS, MAX_SESSIONS * 8)
        if len(data) % 8:
            raise ValueError("Invalid session list length")

        sessions = [entry[0] for entry in struct.iter_unpack("<Q", data)]
        if not all(sessions) or len(set(sessions)) != len(sessions):
            raise ValueError("Invalid session list")

        return sorted(sessions)

    def files(self, pid, session):
        self.send(LIST_FILES, struct.pack("<IIQ", pid, 0, session))
        data = self.reply(LIST_FILES, MAX_FILES * FILE.size)
        if len(data) % FILE.size:
            raise ValueError("Invalid capture file list length")

        files = [decode_file(data[offset:offset + FILE.size])
                 for offset in range(0, len(data), FILE.size)]
        if (sum(file["bytes"] for file in files) > MAX_SESSION_BYTES or
                len({file["name"] for file in files}) != len(files) or
                any(file["session"] != str(session) for file in files)):
            raise ValueError("Capture directory exceeds limits or repeats a filename")

        return sorted(files, key=lambda file: file["name"])

    def download(self, pid, file, destination):
        descriptor = encode_file(file)
        self.send(READ_FILE, struct.pack("<II", pid, 0) + descriptor)
        if self.reply(READ_FILE, FILE.size) != descriptor:
            raise ValueError("Capture file changed before download")

        partial = destination.with_name(destination.name + ".part")
        try:
            with partial.open("xb") as output:
                remaining = file["bytes"]
                while remaining:
                    chunk = receive_exact(self.connection, min(CHUNK_BYTES, remaining))
                    output.write(chunk)
                    remaining -= len(chunk)

            self.reply(READ_FILE, 0)
            partial.rename(destination)
        except BaseException:
            partial.unlink(missing_ok=True)
            raise

    def trigger_pm4(self, pid, session, target):
        self.send(TRIGGER_PM4, struct.pack("<IIQQ", pid, 0, session, target))
        self.reply(TRIGGER_PM4, 0)

    def download_latest(self, pid, file, destination):
        for attempt in range(3):
            try:
                self.download(pid, file, destination)
                return file
            except OSError as error:
                if error.errno != errno.EAGAIN or attempt == 2:
                    raise

                current = self.files(pid, int(file["session"]))
                replacement = next((entry for entry in current if entry["name"] == file["name"]), None)
                if replacement is None:
                    raise FileNotFoundError(file["name"]) from error
                file = replacement


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.73.2")
    parser.add_argument("--port", type=int, default=1236)
    parser.add_argument("--pid", type=int, action="append", help="Select a process; repeat as needed")
    parser.add_argument("--session", type=int, help="Select one decimal capture session")
    parser.add_argument("--list", action="store_true", help="Print file descriptors without downloading")
    parser.add_argument("--trigger-pm4", type=int, metavar="SUBMISSION",
                        help="Request one PM4 submission; 0 selects the next nonempty submission")
    parser.add_argument("--output", type=Path, help="New directory for original files and download metadata")
    args = parser.parse_args()
    if not args.list and args.trigger_pm4 is None and args.output is None:
        parser.error("--output is required for downloading")
    if args.pid and any(pid < 1 or pid > 0x7fffffff for pid in args.pid):
        parser.error("--pid must be between 1 and 2147483647")
    if args.session is not None and not 1 <= args.session <= 0xffffffffffffffff:
        parser.error("--session must be between 1 and 18446744073709551615")
    if args.trigger_pm4 is not None:
        if args.pid is None or len(args.pid) != 1 or args.session is None:
            parser.error("--trigger-pm4 requires exactly one --pid and one --session")
        if args.list or args.output is not None:
            parser.error("--trigger-pm4 is a separate operation from listing or downloading")
        if not 0 <= args.trigger_pm4 <= 0xffffffffffffffff:
            parser.error("--trigger-pm4 must be between 0 and 18446744073709551615")

        with socket.create_connection((args.host, args.port), timeout=15) as connection:
            CaptureClient(connection).trigger_pm4(args.pid[0], args.session, args.trigger_pm4)
        print(json.dumps({
            "request_published": True,
            "pid": args.pid[0],
            "session": str(args.session),
            "target_submit": args.trigger_pm4,
            "selection": "next_nonempty_after_arming" if args.trigger_pm4 == 0 else "process_pm4_serial",
            "request": f"s{args.session}_pm4_request.txt",
            "status": f"s{args.session}_pm4_status.json",
            "requested_utc": datetime.now(timezone.utc).isoformat(),
        }, indent=2))
        return

    manifest = {
        "protocol_version": VERSION,
        "host": args.host,
        "port": args.port,
        "capture_root": CAPTURE_ROOT,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "complete": False,
        "files": [],
    }
    if not args.list:
        args.output.mkdir(parents=True, exist_ok=False)

    try:
        with socket.create_connection((args.host, args.port), timeout=15) as connection:
            client = CaptureClient(connection)
            pids = sorted(set(args.pid)) if args.pid else client.processes()
            for pid in pids:
                sessions = [args.session] if args.session is not None else client.sessions(pid)
                for session in sessions:
                    files = client.files(pid, session)
                    if not args.list and files:
                        (args.output / str(pid)).mkdir(exist_ok=True)

                    for file in files:
                        entry = {"pid": pid, **file}
                        if not args.list:
                            relative = Path(str(pid)) / file["name"]
                            file = client.download_latest(pid, file, args.output / relative)
                            entry.update(file)
                            entry["path"] = relative.as_posix()
                            print(f"{pid}/{file['name']}: {file['bytes']} bytes", flush=True)
                        manifest["files"].append(entry)

        manifest["complete"] = True
    except BaseException as error:
        manifest["error"] = str(error)
        raise
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        text = json.dumps(manifest, indent=2) + "\n"
        if args.list:
            print(text, end="")
        else:
            (args.output / "download-manifest.json").write_text(text, encoding="utf-8")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"Capture export failed: {error}", file=sys.stderr)
        raise SystemExit(1)
