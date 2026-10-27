#!/usr/bin/env python3
"""Export display frames and their direct GPU writers from saved USB logs.

Each input file represents one USB connection. Repeated diagnostic dumps are
joined by the display record sequence within that file. Native validity bits
control which durations can be calculated. The exporter preserves individual
frames and missing records instead of resampling submissions into frames.
"""

import argparse
import csv
import json
import math
from pathlib import Path
import re


FRAME_PREFIX = "T733 frame-profile:"
WRITE_PREFIX = "T733 frame-write:"
SUMMARY_PREFIX = "T733 frame-profile-summary:"
DURATIONS = (
    "generation_observed_us",
    "generation_gpu_observed_us",
    "global_wait_after_surface_ready_us",
    "display_wait_lower_us",
    "display_wait_upper_us",
    "composition_to_display_lower_us",
    "composition_to_display_upper_us",
    "display_interval_lower_us",
    "display_interval_upper_us",
)


def read_fields(text):
    result = {}
    for key, value in re.findall(r"([a-z_]+)=([^\s]+)", text):
        try:
            result[key] = int(
                value, 16 if value.startswith("0x") or key in ("status", "gpu_status") else 10
            )
        except ValueError:
            result[key] = value

    return result


def duration(row, start, end, flags):
    if row.get("valid", 0) & flags != flags:
        return None

    first = row.get(start)
    last = row.get(end)
    if not isinstance(first, int) or not isinstance(last, int) or last < first:
        return None

    return last - first


def read_capture(path):
    frames = {}
    frame_parts = {}
    writers = {}
    snapshots = []
    for line in path.read_text(errors="replace").splitlines():
        if FRAME_PREFIX in line:
            row = read_fields(line.split(FRAME_PREFIX, 1)[1])
            if isinstance(row.get("sequence"), int):
                sequence = row["sequence"]
                frames.setdefault(sequence, {}).update(row)
                if isinstance(row.get("part"), int):
                    frame_parts.setdefault(sequence, set()).add(row["part"])
        elif WRITE_PREFIX in line:
            row = read_fields(line.split(WRITE_PREFIX, 1)[1])
            if "sequence" in row and "index" in row:
                writers[(row["sequence"], row["index"])] = row
        elif SUMMARY_PREFIX in line:
            snapshots.append(read_fields(line.split(SUMMARY_PREFIX, 1)[1]))

    rows = []
    previous = None
    for sequence in sorted(frames):
        row = {"capture": str(path), **frames[sequence]}
        expected_parts = row.get("parts", 0)
        row["frame_record_complete"] = (
            expected_parts > 0
            and set(range(expected_parts)).issubset(frame_parts.get(sequence, set()))
        )
        if not row["frame_record_complete"]:
            row["valid"] = 0

        row["generation_observed_us"] = duration(
            row, "composition_start_us", "read_fence_active_us", 1 | 16
        )
        row["generation_gpu_observed_us"] = None
        # Output truncation limits the fence list, not the accumulated times.
        if (row.get("gpu_flags", 0) & ~32 == 0
                and row.get("submissions", 0) > 0
                and row.get("fallback_submissions", 1) == 0
                and row.get("gpu_complete_observed_us", 0)
                >= row.get("last_submit_us", 0)
                >= row.get("composition_start_us", 0)):
            row["generation_gpu_observed_us"] = duration(
                row, "composition_start_us", "gpu_complete_observed_us", 1 | 64
            )

        row["global_wait_after_surface_ready_us"] = duration(
            row, "read_fence_active_us", "producer_ready_us", 16 | 32
        )
        for bound in ("lower", "upper"):
            row[f"composition_to_display_{bound}_us"] = duration(
                row, "composition_start_us", f"display_{bound}_us", 1 | 128
            )
            row[f"display_wait_{bound}_us"] = duration(
                row, "read_fence_active_us", f"display_{bound}_us", 16 | 128
            )

        if row.get("display_wait_upper_us") is not None:
            row["display_wait_lower_us"] = max(
                0, row["display_lower_us"] - row["read_fence_active_us"]
            )

        row["display_interval_lower_us"] = None
        row["display_interval_upper_us"] = None
        if (previous is not None and previous["sequence"] + 1 == sequence
                and previous.get("valid", 0) & 128 and row.get("valid", 0) & 128
                and row["display_upper_us"] >= previous["display_upper_us"]):
            row["display_interval_lower_us"] = max(
                0, row["display_lower_us"] - previous["display_upper_us"]
            )
            row["display_interval_upper_us"] = (
                row["display_upper_us"] - previous["display_lower_us"]
            )

        row["writers"] = [
            writers[(sequence, index)]
            for index in range(row.get("stored_submissions", 0))
            if (sequence, index) in writers
        ]
        row["writer_records_complete"] = (
            len(row["writers"]) == row.get("stored_submissions", 0)
        )
        rows.append(row)
        previous = row

    return rows, snapshots


def summarize(rows):
    timings = {}
    for name in DURATIONS:
        values = sorted(row[name] for row in rows if row.get(name) is not None)
        if not values:
            continue

        middle = len(values) // 2
        median = values[middle] if len(values) % 2 else (
            values[middle - 1] + values[middle]
        ) / 2
        timings[name] = {
            "count": len(values),
            "mean": sum(values) / len(values),
            "median": median,
            "p95": values[math.ceil(len(values) * 0.95) - 1],
            "max": values[-1],
        }

    return {
        "frames": len(rows),
        "timings": timings,
        "generation_boundary": "CA begin_update completed to native surface read-fence observed active",
        "gpu_membership": "Direct writes to the displayed surface; intermediate render targets are not enumerated",
        "presentation_boundary": "DPU switch observed between display_lower_us and display_upper_us",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    rows = []
    snapshots = []
    for path in args.logs:
        capture_rows, capture_snapshots = read_capture(path)
        rows.extend(capture_rows)
        snapshots.append({"capture": str(path), "snapshots": capture_snapshots})

    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "frames.jsonl").open("w") as stream:
        for row in rows:
            stream.write(json.dumps(row) + "\n")

    if rows:
        fields = list(dict.fromkeys(key for row in rows for key in row))
        with (args.output / "frames.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for row in rows:
                writer.writerow({**row, "writers": json.dumps(row["writers"])})

    summary = {**summarize(rows), "captures": snapshots}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"frames": len(rows), "output": str(args.output)}))


if __name__ == "__main__":
    main()
