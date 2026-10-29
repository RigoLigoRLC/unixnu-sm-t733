#!/usr/bin/env python3
"""Capture A642L GPU stages and counters over the tablet's USB network."""

import argparse
import json
from pathlib import Path
import sys
import time

from adreno_trace import (
    Capture, TraceConnection, COMPLETE, SEALED, START, STOP, SEAL, write_json,
)


def export(directory, output):
    from adreno_perfetto import export_capture

    capture = Capture(directory)
    result = export_capture(capture, output)
    print(json.dumps({"trace": str(output), **result}, indent=2), flush=True)
    return 0 if result["complete"] else 2


def await_complete(connection, generation, output, timeout):
    deadline = time.monotonic() + timeout
    last_report = None
    while time.monotonic() < deadline:
        status = connection.status()
        write_json(output / "last-status.json", status)
        if status["generation"] != generation:
            raise RuntimeError("Another capture replaced this generation")

        report = (status["state"], status["producer_count"], status["access_error"])
        if report != last_report:
            print(json.dumps({key: status[key] for key in (
                "generation", "state_name", "sample_count", "job_count", "event_count",
                "producer_count", "pending_jobs", "hardware_owned", "access_error")}), flush=True)
            last_report = report

        if status["state"] == SEALED:
            return status

        if (status["state"] == COMPLETE and not status["producer_count"] and
                not status["pending_jobs"] and not status["hardware_owned"]):
            return connection.control(SEAL, generation)

        time.sleep(0.5)

    raise TimeoutError("The capture has not drained; saved last-status.json and left device records retained")


def collect(args):
    args.output.mkdir(parents=True, exist_ok=False)
    with TraceConnection(args.host, args.port) as connection:
        status = connection.status()
        write_json(args.output / "before-status.json", status)
        if args.command == "capture":
            duration_ms = round(args.seconds * 1000)
            if not 100 <= duration_ms <= 60000:
                raise ValueError("Capture duration must be between 0.1 and 60 seconds")

            status = connection.control(START, duration_ms=duration_ms)
            write_json(args.output / "start-status.json", status)
            print(f"GPU capture {status['generation']} requested for {duration_ms} ms", flush=True)

        generation = status["generation"]
        try:
            status = await_complete(connection, generation, args.output, args.timeout)
        except KeyboardInterrupt:
            print("Stopping capture and waiting for GPU and trace producers", flush=True)
            connection.control(STOP, generation)
            status = await_complete(connection, generation, args.output, args.timeout)
        except TimeoutError:
            if args.command == "capture":
                try:
                    connection.control(STOP, generation)
                except Exception as error:
                    print(f"Capture stop failed: {error}", file=sys.stderr)

            raise

        connection.download(args.output, status)
        print(f"Saved generation {generation} binary capture to {args.output}", flush=True)

    if args.raw_only:
        return 0

    return export(args.output, args.output / "trace.pftrace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.73.2")
    parser.add_argument("--port", type=int, default=1235)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="Read capture status without starting profiling")
    commands.add_parser("stop", help="Stop the current capture without discarding it")
    for name in ("capture", "download"):
        command = commands.add_parser(name)
        command.add_argument("--output", type=Path, required=True, help="New directory for raw records and Perfetto trace")
        command.add_argument("--timeout", type=float, default=120, help="Maximum seconds to wait for GPU and producer drain")
        command.add_argument("--raw-only", action="store_true", help="Save binary records without creating a Perfetto file")
        if name == "capture":
            command.add_argument("--seconds", type=float, default=5)

    command = commands.add_parser("export", help="Create Perfetto tracks from an already saved binary capture")
    command.add_argument("directory", type=Path)
    command.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        if args.command == "export":
            return export(args.directory, args.output)

        if args.command in ("capture", "download"):
            return collect(args)

        with TraceConnection(args.host, args.port) as connection:
            status = connection.status()
            if args.command == "stop":
                status = connection.control(STOP, status["generation"])

            print(json.dumps(status, indent=2))
            return 0
    except (OSError, RuntimeError, ValueError, ImportError, KeyError) as error:
        print(f"GPU trace: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
