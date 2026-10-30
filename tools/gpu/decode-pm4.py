#!/usr/bin/env python3
"""Decode one saved RD submission and report command-address coverage."""

import argparse
from collections import Counter
import json
from pathlib import Path
import resource
import shutil
import struct
import subprocess


def read_rd(path):
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("RD exceeds the producer's 16 MiB bound")
    data = path.read_bytes()

    at = 0
    address = None
    ranges = []
    commands = []
    sections = Counter()
    while at < len(data):
        if len(data) - at < 8:
            raise ValueError(f"Incomplete RD header at {at}")
        kind, size = struct.unpack_from("<II", data, at)
        at += 8
        if size > len(data) - at:
            raise ValueError(f"Incomplete RD section at {at}")
        sections[kind] += 1
        if kind == 3 and size == 12:
            low, length, high = struct.unpack_from("<III", data, at)
            address = (low | high << 32, length)
        elif kind == 12:
            if address is None or address[1] != size:
                raise ValueError(f"RD contents lack a matching address at {at}")
            ranges.append({"address": address[0], "bytes": size, "rd_offset": at})
            address = None
        elif kind == 6 and size == 12:
            low, dwords, high = struct.unpack_from("<III", data, at)
            commands.append({"address": low | high << 32, "bytes": dwords * 4})
        at += size

    return {"bytes": len(data), "sections": dict(sections), "ranges": ranges,
            "top_level_commands": commands}


def contains(ranges, address, size):
    return any(item["address"] <= address and
               address + size <= item["address"] + item["bytes"]
               for item in ranges)


def coverage(manifest, rd):
    missing = []
    external = []
    kinds = Counter()
    for entry in manifest.get("references", []):
        record = dict(entry)
        record["present_in_rd"] = contains(rd["ranges"], entry["address"], entry["bytes"])
        kinds[entry["kind"]] += 1
        if entry["kind"] == 3:
            external.append(record)
        elif entry["coverage"] != "saved" or not record["present_in_rd"]:
            missing.append(record)

    missing_top = [item for item in rd["top_level_commands"]
                   if not contains(rd["ranges"], item["address"], item["bytes"])]
    expected_top = kinds[0]
    limits = manifest.get("metadata_dropped", 0)
    malformed = manifest.get("malformed_packets", 0)
    resource_classes = Counter(item["resource_class"]
                               for item in manifest.get("submitted_resources", []))
    writable_commands = [item for item in manifest.get("submitted_resources", [])
                         if item["allocation_flags"] & 128 and
                         not item["allocation_flags"] & 2 and
                         any(item["address"] <= saved["address"] < item["address"] + item["bytes"]
                             for saved in rd["ranges"])]
    complete = bool(expected_top) and not (missing or missing_top or limits or malformed)
    complete = complete and expected_top == len(rd["top_level_commands"])
    return {
        "recorded_command_references_covered": complete,
        "reference_counts_by_kind": dict(kinds),
        "missing_command_references": missing,
        "missing_top_level_rd_commands": missing_top,
        "metadata_dropped": limits,
        "malformed_packets": malformed,
        "external_state_address_references": external,
        "external_state_full_extent_captured": False,
        "submitted_resources_by_class": dict(resource_classes),
        "gpu_writable_command_allocations": writable_commands,
        "execution_counts_measured": False,
        "snapshot_time": "CPU bytes before kernel submission",
        "resource_contents": "Only referenced command ranges; texture and ordinary resource contents excluded",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rd", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--cffdump", default=shutil.which("cffdump"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--coverage-only", action="store_true")
    parser.add_argument("--expand-ibs", action="store_true",
                        help="Ask cffdump to expand repeated IB calls; this does not measure GPU executions")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--text-limit-mib", type=int, default=64)
    args = parser.parse_args()
    if not args.coverage_only and not args.cffdump:
        parser.error("cffdump is required; pass --cffdump PATH or --coverage-only")
    if args.timeout <= 0 or not 1 <= args.text_limit_mib <= 256:
        parser.error("timeout must be positive and text limit must be 1..256 MiB")

    manifest_path = args.manifest or args.rd.with_suffix(".json")
    manifest = json.loads(manifest_path.read_text())
    rd = read_rd(args.rd)
    report = {
        "version": 1,
        "rd_path": str(args.rd.resolve()),
        "manifest_path": str(manifest_path.resolve()),
        "identity": {key: manifest.get(key) for key in (
            "pid", "session_id", "pm4_submit_serial", "mesa_submit_count", "device_index",
            "queue_id", "completion_receipt", "submit_result", "command_buffer_recording_ids")},
        "producer_reason": manifest.get("reason"),
        "rd": rd,
        "coverage": coverage(manifest, rd),
    }
    args.output.mkdir(parents=True, exist_ok=False)
    if not args.coverage_only:
        command = [str(Path(args.cffdump).resolve()), "--no-color", "--no-pager"]
        if not args.expand_ibs:
            command.append("--once")
        command.append(str(args.rd.resolve()))
        report["decoder"] = {"command": command, "ib_expansion": args.expand_ibs}

        def limit_output():
            size = args.text_limit_mib * 1024 * 1024
            resource.setrlimit(resource.RLIMIT_FSIZE, (size, size))

        with (args.output / "commands.txt").open("wb") as output, \
                (args.output / "decoder-stderr.txt").open("wb") as errors:
            try:
                result = subprocess.run(command, stdout=output, stderr=errors,
                                        timeout=args.timeout, preexec_fn=limit_output, check=False)
                report["decoder"]["returncode"] = result.returncode
            except subprocess.TimeoutExpired:
                report["decoder"]["timed_out"] = True
            except OSError as error:
                report["decoder"]["error"] = str(error)

    (args.output / "decode-report.json").write_text(json.dumps(report, indent=2) + "\n")
    summary = report["coverage"]
    lines = [
        f"Native completion: {manifest.get('completion_receipt')}",
        f"PM4 serial: {manifest.get('pm4_submit_serial')}; Mesa submit: {manifest.get('mesa_submit_count')}",
        f"Recorded command references covered: {summary['recorded_command_references_covered']}",
        f"Missing command references: {len(summary['missing_command_references'])}",
        f"Metadata dropped: {summary['metadata_dropped']}; malformed packets: {summary['malformed_packets']}",
        f"External state addresses with unresolved full extents: {len(summary['external_state_address_references'])}",
        f"GPU-writable command allocations: {len(summary['gpu_writable_command_allocations'])}",
        "The decoder shows packet references and conditional replay paths, not measured GPU execution counts.",
    ]
    if "decoder" in report:
        lines.append("Decoder result: " + json.dumps(report["decoder"]))
    (args.output / "summary.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
