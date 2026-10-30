#!/usr/bin/env python3
"""Analyze saved GPU stages, command counts and render-pass operations."""

import argparse
from bisect import bisect_right
from collections import defaultdict
import math
from pathlib import Path
import statistics

from adreno_trace import BEGIN, END, GPU_CLOCK, INVALID, Capture, write_json
from adreno_perfetto import ClockAlignment, RenderPassWork, event_arguments


def duration_statistics(values):
    if not values:
        return {"count": 0}

    ordered = sorted(values)
    return {
        "count": len(ordered),
        "summed_stage_duration_ms": sum(ordered) / 1_000_000,
        "mean_ms": statistics.mean(ordered) / 1_000_000,
        "median_ms": statistics.median(ordered) / 1_000_000,
        "p95_ms": ordered[math.ceil(len(ordered) * 0.95) - 1] / 1_000_000,
        "maximum_ms": ordered[-1] / 1_000_000,
    }


def pass_key(event, arguments):
    return (event.process_id, event.command_buffer_id, event.submission_id, arguments.get("pass_id", 0))


def pass_identity(key):
    return dict(zip(("process_id", "command_buffer_id", "submission_id", "pass_id"), key))


def pair_stages(capture):
    pending = defaultdict(list)
    stages = []
    coverage = {"invalid_endpoints": 0, "unmatched_begin": 0, "unmatched_end": 0}
    for event in capture.events:
        if event.source != 2 or event.point not in (1, 11) or event.phase not in (BEGIN, END):
            continue

        if event.flags & INVALID or event.clock_domain != GPU_CLOCK:
            coverage["invalid_endpoints"] += 1
            continue

        key = (event.process_id, event.queue_id, event.submission_id, event.command_buffer_id,
               event.point, event.name)
        if event.phase == BEGIN:
            pending[key].append(event)
        elif not pending[key]:
            coverage["unmatched_end"] += 1
        else:
            start = pending[key].pop()
            if event.timestamp_ns < start.timestamp_ns:
                coverage["invalid_endpoints"] += 1
                continue

            arguments = {**event_arguments(start), **event_arguments(event)}
            stages.append((start, event, arguments))

    coverage["unmatched_begin"] = sum(len(items) for items in pending.values())
    return sorted(stages, key=lambda item: item[0].timestamp_ns), coverage


class PerformanceWindows:
    """PerformanceWindows associates GPU stages with acknowledged GPU/DDR votes.

    The saved counter clock pairs convert each stage endpoint to CPU time.
    A stage is covered when both endpoints lie inside the performance sample
    range. Its starting vote and every following vote through its end must
    acknowledge 550 MHz and DDR index 9; fixed mode additionally requires the
    fixed-frequency flag in each of those records.
    """

    def __init__(self, capture):
        self.clock = ClockAlignment(capture)
        self.records = sorted(capture.performance, key=lambda item: item["timestamp_ns"])
        self.times = [item["timestamp_ns"] for item in self.records]

    def classify(self, start, end):
        begin_ns = self.clock.cpu_time(start.timestamp_ns)
        end_ns = self.clock.cpu_time(end.timestamp_ns)
        result = {"cpu_begin_ns": begin_ns, "cpu_end_ns": end_ns,
                  "acknowledged_550": False, "fixed_550": False}
        first = bisect_right(self.times, begin_ns) - 1
        if first < 0 or not self.times or end_ns > self.times[-1] or end_ns < begin_ns:
            result["reason"] = "stage endpoints extend beyond recorded performance coverage"
            return result

        records = self.records[first:bisect_right(self.times, end_ns)]
        if any(item["acknowledged_hz"] != 550000000 or item["bandwidth_index"] != 9 or
               item["vote_status"] for item in records):
            result["reason"] = "stage includes another GPU/DDR vote or an unsuccessful vote"
            return result

        result["acknowledged_550"] = True
        result["fixed_550"] = all(item["flags"] & 4 for item in records)
        result["reason"] = "fixed 550 MHz and DDR 9" if result["fixed_550"] else "governor at 550 MHz and DDR 9"
        return result


def signature(event, arguments):
    fields = ("width", "height", "samples", "draw_count", "source_format", "destination_format",
              "bytes", "copy_path_mask", "copy_rows", "copy_batched_draws")
    result = {"stage": event.name, "point": event.point}
    for name in fields:
        if name in arguments:
            result[name] = arguments[name]

    if event.point == 1:
        result["render_mode"] = arguments.get("actual_render_mode",
            "gmem" if arguments.get("tiled_rendering") else "sysmem")

    return result


def command_totals():
    return {"emitted_packets": 0, "estimated_executions": 0,
            "conditional_execution_upper_bound": 0, "emitted_packets_without_execution_estimate": 0,
            "records": 0}


def add_commands(total, arguments):
    emitted = arguments.get("emitted_count", 0)
    total["records"] += 1
    total["emitted_packets"] += emitted
    if not arguments.get("execution_estimate_available"):
        total["emitted_packets_without_execution_estimate"] += emitted
    elif arguments.get("execution_estimate_is_upper_bound"):
        total["conditional_execution_upper_bound"] += arguments["estimated_executions"]
    else:
        total["estimated_executions"] += arguments["estimated_executions"]


def sys_wfi_counter(capture):
    definition = next((item for item in capture.definitions if item["name"] == "cp_sys_wfi_stall"), None)
    result = {"definition_present": definition is not None, "valid_intervals": 0,
              "nonzero_intervals": 0, "counter_resets": 0, "cycles": 0, "measurements_populated": False}
    if definition is None:
        return result

    index = definition["index"]
    for previous, current in zip(capture.samples, capture.samples[1:]):
        if not (previous.valid_mask & current.valid_mask & (1 << index)):
            continue

        delta = current.values[index] - previous.values[index]
        if delta < 0:
            result["counter_resets"] += 1
            continue

        result["valid_intervals"] += 1
        result["nonzero_intervals"] += delta > 0
        result["cycles"] += delta

    result["measurements_populated"] = result["valid_intervals"] > 0
    return result


def analyze(directory):
    capture = Capture(directory)
    windows = PerformanceWindows(capture)
    pass_work = RenderPassWork(capture.events)
    stages, coverage = pair_stages(capture)
    durations = {"all": defaultdict(list), "acknowledged_550": defaultdict(list), "fixed_550": defaultdict(list)}
    signatures = {}
    stage_rows = []
    pass_rows = defaultdict(dict)
    wfi_causes = defaultdict(command_totals)
    wfi_passes = defaultdict(command_totals)
    event_writes = defaultdict(command_totals)
    split_pairs = []
    metadata_dropped = 0
    metal_batches = []

    for event in capture.events:
        if event.flags & INVALID:
            continue

        arguments = event_arguments(event)
        key = pass_key(event, arguments)
        if event.source == 2 and event.point == 65540:
            if arguments["command_opcode"] == 0x26:
                cause = (event.name, arguments["command_source_line"], arguments["command_stream_description"],
                         arguments["command_execution_is_conditional"])
                add_commands(wfi_causes[cause], arguments)
                add_commands(wfi_passes[key], arguments)
            elif arguments["command_opcode"] == 0x46:
                add_commands(event_writes[arguments["command_event_name"]], arguments)
        elif event.source == 2 and event.point in (65541, 65542):
            row = pass_rows[key]
            if not row:
                row.update({**pass_identity(key), "attachments": []})

            if event.point == 65541:
                row.update({name: value for name, value in arguments.items()
                            if name not in ("source", "point", "flags", "vulkan_command_buffer_id")})
            else:
                row["attachments"].append(arguments)
        elif event.source == 2 and event.point == 65543:
            metadata_dropped += arguments.get("metadata_dropped", 0)
        elif event.source == 1 and event.point == 17:
            metal_batches.append({"timestamp_ns": event.timestamp_ns, **arguments})

    for start, end, arguments in stages:
        arguments.update(pass_work.describe(start, arguments))
        selected = windows.classify(start, end)
        description = signature(start, arguments)
        group = tuple(sorted(description.items()))
        signatures[group] = description
        duration = end.timestamp_ns - start.timestamp_ns
        durations["all"][group].append(duration)
        for selection in ("acknowledged_550", "fixed_550"):
            if selected[selection]:
                durations[selection][group].append(duration)

        row = {**pass_identity(pass_key(start, arguments)), "completion_id": start.completion_id,
               "stage": start.name, "gpu_begin_ns": start.timestamp_ns, "gpu_end_ns": end.timestamp_ns,
               "duration_ns": duration, **selected, "description": description}
        stage_rows.append(row)
        if start.point == 1 and arguments.get("pass_id"):
            pass_rows[pass_key(start, arguments)].update(row)

    previous_pass = {}
    for key, row in sorted(pass_rows.items(), key=lambda item: item[1].get("gpu_begin_ns", 0)):
        row.setdefault("attachments", [])
        row["wait_for_idle_commands"] = wfi_passes.get(key, command_totals())
        encoder = row.get("encoder_id")
        if not encoder or "gpu_begin_ns" not in row:
            continue

        encoder_key = (row["process_id"], row["submission_id"], encoder)
        previous = previous_pass.get(encoder_key)
        if previous is not None:
            copies = [stage for stage in stage_rows if stage["stage"] == "copy_buffer_to_image" and
                      stage["process_id"] == row["process_id"] and
                      stage["submission_id"] == row["submission_id"] and
                      stage["gpu_begin_ns"] >= previous["gpu_end_ns"] and
                      stage["gpu_end_ns"] <= row["gpu_begin_ns"]]
            split_pairs.append({
                "process_id": row["process_id"], "submission_id": row["submission_id"], "encoder_id": encoder,
                "previous_pass_id": previous["pass_id"], "next_pass_id": row["pass_id"],
                "previous_instance": previous.get("internal_pass_index"),
                "next_instance": row.get("internal_pass_index"),
                "next_split_reason_bits": row.get("split_reason_bits"),
                "next_split_reasons": row.get("split_reasons"),
                "gap_ns": row["gpu_begin_ns"] - previous["gpu_end_ns"],
                "intervening_copy_count": len(copies),
                "intervening_copy_stage_duration_ns": sum(stage["duration_ns"] for stage in copies),
                "both_fixed_550": previous.get("fixed_550", False) and row.get("fixed_550", False),
            })
        previous_pass[encoder_key] = row

    levels = defaultdict(int)
    for record in capture.performance:
        levels[(record["acknowledged_hz"], record["bandwidth_index"], bool(record["flags"] & 4))] += 1

    return {
        "directory": str(directory.resolve()), "generation": capture.status["generation"],
        "requested_frequency_hz": capture.status.get("capture_frequency_hz", 0),
        "capture_flags": capture.status.get("capture_flags", 0),
        "losses": {name: capture.status.get(name, 0) for name in (
            "sample_dropped", "job_dropped", "event_dropped", "producer_dropped", "producer_failed", "host_missing_events")},
        "metadata_dropped": metadata_dropped, "stage_endpoint_coverage": coverage,
        "system_wait_for_idle_counter": sys_wfi_counter(capture),
        "gpu_ddr_samples": [{"frequency_hz": hz, "bandwidth_index": ddr, "fixed": fixed, "records": count}
                            for (hz, ddr, fixed), count in sorted(levels.items())],
        "thermal_limited_records": sum(bool(item["flags"] & 32) for item in capture.performance),
        "vote_failures": sum(bool(item["vote_status"]) for item in capture.performance),
        "stage_durations": {selection: [{"description": signatures[group], **duration_statistics(values)}
                                        for group, values in groups.items()]
                            for selection, groups in durations.items()},
        "wait_for_idle_by_source": [{"function": cause[0], "line": cause[1], "stream": cause[2],
                                     "conditional": cause[3], **counts} for cause, counts in wfi_causes.items()],
        "wait_for_idle_by_pass": [{**pass_identity(key), **counts} for key, counts in wfi_passes.items()],
        "event_write_counts": [{"event": name, **counts} for name, counts in sorted(event_writes.items())],
        "passes": list(pass_rows.values()), "adjacent_passes_in_same_encoder": split_pairs,
        "metal_copy_batches": metal_batches, "gpu_stages": stage_rows,
    }


def compare(current, baseline):
    prior = {tuple(sorted(row["description"].items())): row
             for row in baseline["stage_durations"]["acknowledged_550"]}
    comparisons = []
    for row in current["stage_durations"]["fixed_550"]:
        before = prior.get(tuple(sorted(row["description"].items())))
        if before is None:
            continue

        comparisons.append({"description": row["description"], "current": row, "baseline": before,
                            "mean_change_percent": 100 * (row["mean_ms"] / before["mean_ms"] - 1)
                            if before["mean_ms"] else None})

    return {"current_selection": "fixed 550 MHz and DDR 9",
            "baseline_selection": "acknowledged 550 MHz and DDR 9; governor samples are included",
            "matched_stage_groups": comparisons}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    result = analyze(args.directory)
    if args.baseline:
        baseline = analyze(args.baseline)
        result["baseline"] = baseline
        result["comparison"] = compare(result, baseline)

    write_json(args.output, result)
    print(f"Saved GPU overhead analysis to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
