#!/usr/bin/env python3
"""Capture A642L GPU stages and counters over the tablet's USB network."""

import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import signal
import sys
import time

from adreno_trace import (
    Capture, TraceConnection, COMPLETE, SEALED, START, START_STREAM, STOP, SEAL,
    SECTIONS, EVENT_BYTES, write_json,
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


def collect_stream(connection, status, args):
    """The host writes each event batch before acknowledging its ring sequence.

    Counter, job and DVFS sections append to their files while recording continues.
    Once GPU work and producers finish, sealing fixes the final section lengths;
    the remaining stream records are drained before the files receive final names.
    """
    generation = status["generation"]
    offsets = {section: 0 for section, _, _, _ in SECTIONS}
    sequence = 0
    acknowledged_sequence = 0
    saved_events = 0
    missing_events = 0
    batches = 0
    stop_requested = False
    stop_sent = False
    started = time.monotonic()
    deadline = started + args.seconds + args.timeout
    last_poll = 0
    last_report = 0
    statistics = {}

    def request_stop(_signal, _frame):
        nonlocal stop_requested
        stop_requested = True

    old_handler = signal.signal(signal.SIGINT, request_stop)
    try:
        with ExitStack() as stack:
            streams = {
                section: stack.enter_context((args.output / (filename + ".partial")).open("xb"))
                for section, filename, _, _ in SECTIONS
            }
            gaps = stack.enter_context((args.output / "stream-gaps.jsonl").open("x"))
            while True:
                if stop_requested and not stop_sent:
                    connection.control(STOP, generation)
                    stop_sent = True
                    print("Stopping stream and draining submitted GPU work", flush=True)

                chunks = connection.stream_events(generation, sequence)
                batches += 1
                for chunk in chunks:
                    if chunk["first"] > sequence:
                        gaps.write(json.dumps({"begin": sequence, "end": chunk["first"]}) + "\n")
                        gaps.flush()
                        missing_events += chunk["first"] - sequence

                    streams[4].write(chunk["data"])
                    saved_events += len(chunk["data"]) // EVENT_BYTES
                    offsets[4] += len(chunk["data"])
                    sequence = chunk["next"]

                streams[4].flush()
                if sequence != acknowledged_sequence:
                    connection.acknowledge_events(generation, sequence)
                    acknowledged_sequence = sequence
                now = time.monotonic()
                caught_up = sequence == chunks[-1]["written"]
                if now - last_poll >= 0.25 or status["state"] == SEALED:
                    status = connection.status()
                    if status["generation"] != generation:
                        raise RuntimeError("Another capture replaced the streaming generation")

                    write_json(args.output / "last-status.json", status)
                    last_poll = now
                    for section, _, count_field, size_field in SECTIONS:
                        if section == 4:
                            continue

                        expected = status[count_field] * status[size_field]
                        while offsets[section] < expected:
                            data, available = connection.read_section(generation, section, offsets[section])
                            if not data or available < expected:
                                raise ValueError("A committed trace section became unavailable")

                            streams[section].write(data)
                            offsets[section] += len(data)

                        streams[section].flush()

                    if (status["state"] == COMPLETE and not status["producer_count"] and
                            not status["pending_jobs"] and not status["hardware_owned"]):
                        status = connection.control(SEAL, generation)

                statistics = {
                    "generation": generation, "events_saved": saved_events,
                    "next_sequence": sequence, "missing_events": missing_events,
                    "batches": batches, "bytes_saved": sum(offsets.values()),
                    "elapsed_seconds": round(now - started, 6),
                    "mean_bytes_per_second": round(sum(offsets.values()) / max(now - started, 0.001)),
                }
                if now - last_report >= 1 or status["state"] == SEALED:
                    write_json(args.output / "stream-status.json", statistics)
                    print(json.dumps({"state": status["state_name"], **statistics}), flush=True)
                    last_report = now

                if status["state"] == SEALED and sequence == status["event_count"]:
                    break

                if now >= deadline:
                    connection.control(STOP, generation)
                    raise TimeoutError("Stream drain timed out; received data remains in .partial files")

                if caught_up:
                    time.sleep(0.005)
    finally:
        signal.signal(signal.SIGINT, old_handler)

    for section, filename, count_field, size_field in SECTIONS:
        expected = saved_events * EVENT_BYTES if section == 4 else status[count_field] * status[size_field]
        if offsets[section] != expected:
            raise ValueError(f"Streaming {filename} has {offsets[section]} bytes; expected {expected}")

        (args.output / (filename + ".partial")).rename(args.output / filename)

    write_json(args.output / "device-status.json", status)
    saved_status = {**status, "device_event_count": status["event_count"],
                    "event_count": saved_events, "host_missing_events": missing_events}
    write_json(args.output / "status.json", saved_status)
    write_json(args.output / "stream-status.json", statistics)
    print(f"Saved streaming generation {generation} to {args.output}", flush=True)


def collect(args):
    args.output.mkdir(parents=True, exist_ok=False)
    with TraceConnection(args.host, args.port) as connection:
        status = connection.status()
        write_json(args.output / "before-status.json", status)
        if args.command == "capture":
            duration_ms = round(args.seconds * 1000)
            if not 100 <= duration_ms <= 60000:
                raise ValueError("Capture duration must be between 0.1 and 60 seconds")

            operation = START if args.buffered else START_STREAM
            status = connection.control(operation, duration_ms=duration_ms)
            write_json(args.output / "start-status.json", status)
            print(f"GPU capture {status['generation']} requested for {duration_ms} ms", flush=True)

        generation = status["generation"]
        if status.get("streaming"):
            if args.command != "capture":
                raise ValueError("Streaming data is saved in the directory used by its capture command")

            collect_stream(connection, status, args)
            if args.raw_only:
                return 0

            return export(args.output, args.output / "trace.pftrace")

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
            command.add_argument("--buffered", action="store_true", help="Use the older stop-then-download protocol")

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
