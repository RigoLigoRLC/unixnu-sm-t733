"""Perfetto tracks for sealed A642L captures.

Hardware counter samples connect the GPU's 19.2 MHz clock to the tablet's
monotonic clock. GPU stages retain their GPU timestamps. Metal metadata and
submission receipts identify the commands that produced each stage.
"""

from bisect import bisect_left
from collections import defaultdict
import math

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import TrackEvent
from perfetto.trace_builder.proto_builder import StreamingTraceProtoBuilder

from adreno_trace import BEGIN, END, INSTANT, GPU_CLOCK, CPU_CLOCK, INVALID, write_json


SEQUENCE = 1
MONOTONIC = 3
GPU_SEQUENCE_CLOCK = 64
TICK_MASK = (1 << 32) - 1
METAL_ARGUMENTS = {
    1: ("metal_command_buffer_id", "encoder_id", "metal_queue_id", "encoder_kind"),
    2: ("encoder_id", "extent", "layers_samples", "attachment_count"),
    3: ("encoder_id", "texture_id", "extent", "format_actions"),
    4: ("encoder_id", "texture_id", "extent", "format_depth"),
    5: ("encoder_id", "metal_pipeline_id", "vulkan_pipeline", "draw_kind"),
    6: ("encoder_id", "metal_pipeline_id", "element_count", "instance_count"),
    7: ("metal_command_buffer_id", "encoder_id", "encoder_kind", "reserved"),
}
TURNIP_ARGUMENTS = {
    1: "width", 2: "height", 3: "bin_count", 4: "samples", 5: "draw_count",
    6: "tiled_rendering", 7: "estimated_bandwidth", 8: "lrz", 9: "vulkan_format",
    10: "force_load", 11: "fast_path", 12: "unaligned_attachment", 13: "local_size",
    14: "group_count", 15: "indirect", 16: "cache_flush_bits",
    17: "secondary_command_buffer_id", 20: "timestamp_dwords", 21: "timestamp_copy_dwords",
    22: "wait_for_me_dwords", 23: "additional_wait_for_idle_dwords",
    24: "budget_dropped", 25: "queue_dropped", 26: "append_dropped",
    27: "unsubmitted_commands", 28: "unexecuted_gmem_points",
    29: "source_format", 30: "destination_format", 31: "bytes",
    32: "reserved_bytes_high_water", 33: "budget_bytes",
}
COUNTER_NAMES = {
    "core": "GPU core cycles", "sp_busy": "Shader processor busy cycles",
    "sp_alu": "Shader ALU cycles", "vs_alu": "Vertex ALU instructions",
    "fs_alu": "Fragment ALU instructions", "fs_tex": "Fragment texture instructions",
    "vs_tex": "Vertex texture instructions", "sp_stall_tp": "Shader cycles waiting for texture processors",
    "sp_stall_uche": "Shader cycles waiting for UCHE", "sp_stall_rb": "Shader cycles waiting for render backends",
    "tp_busy": "Texture processor busy cycles", "tp_stall_uche": "Texture cycles waiting for UCHE",
    "uche_busy": "UCHE busy cycles", "uche_stall_arbiter": "UCHE cycles waiting for its arbiter",
    "uche_read_tp": "UCHE reads for texture processors", "uche_read_sp": "UCHE reads for shader processors",
    "uche_write_sp": "UCHE writes from shader processors", "uche_gmem_read": "UCHE reads from GMEM",
    "uche_vbif_latency_cycles": "UCHE external memory latency cycles",
    "uche_vbif_latency_samples": "UCHE external memory latency samples",
    "rb_busy": "Render backend busy cycles", "rb_stall_ccu": "Render backend cycles waiting for CCU",
    "ccu_gmem_read": "CCU reads from GMEM", "ccu_gmem_write": "CCU writes to GMEM",
    "cp_busy": "Command processor busy cycles", "cp_wfi_stall": "Command processor cycles waiting for idle",
    "cp_sync_stall": "Command processor synchronization wait cycles",
    "fs_stage_duration": "Fragment shader stage cycles", "vs_stage_duration": "Vertex shader stage cycles",
}


def event_arguments(event):
    arguments = {
        "source": event.source, "point": event.point, "process_id": event.process_id,
        "queue_id": event.queue_id, "submission_id": event.submission_id,
        "completion_id": event.completion_id, "vulkan_command_buffer_id": event.command_buffer_id,
        "flags": event.flags,
    }
    names = METAL_ARGUMENTS.get(event.point, ()) if event.source == 1 else ()
    for key, kind, value in event.arguments:
        index = key - event.point * 16
        if 0 <= index < len(names):
            name = names[index]
        elif event.source == 2:
            name = TURNIP_ARGUMENTS.get(key, f"argument_{key}")
        else:
            name = f"argument_{key}"
        arguments[name] = value

    if "extent" in arguments:
        extent = arguments["extent"]
        arguments["width"] = extent & 0xffffffff
        arguments["height"] = extent >> 32

    if "layers_samples" in arguments:
        value = arguments["layers_samples"]
        arguments["layers"] = value & 0xffffffff
        arguments["samples"] = value >> 32
        arguments["pass_description"] = "final" if event.flags >> 16 else "initial"

    if "format_actions" in arguments:
        value = arguments["format_actions"]
        arguments["metal_pixel_format"] = value & 0xffffffff
        arguments["load_action"] = (value >> 32) & 0xff
        arguments["store_action"] = (value >> 40) & 0xff
        arguments["attachment_slot"] = (event.flags >> 16) & 0x7fff
        arguments["attachment_actions"] = "final" if event.flags & 0x80000000 else "initial"

    if "format_depth" in arguments:
        value = arguments["format_depth"]
        arguments["metal_pixel_format"] = value & 0xffffffff
        arguments["depth"] = value >> 32
        arguments["shader_stage"] = (event.flags >> 16) & 0xff
        arguments["texture_slot"] = event.flags >> 24

    return arguments


class MetalWork:
    """MetalWork connects encoder metadata to each recorded Vulkan command.

    Encoder mappings associate pass descriptions with primary commands.
    Turnip's secondary execution records connect deferred draws to their
    primary commands. GPU stage annotations summarize this recorded work;
    the original metadata events retain every binding and draw argument.
    """

    def __init__(self, events):
        self.command_encoders = defaultdict(set)
        self.encoder_commands = defaultdict(set)
        self.encoder_metadata = defaultdict(list)
        self.command_metadata = defaultdict(list)
        self.secondary_commands = defaultdict(list)
        self.primary_commands = defaultdict(set)
        self.submissions = defaultdict(list)
        for event in events:
            if event.flags & INVALID:
                continue

            command = (event.process_id, event.command_buffer_id)
            args = event_arguments(event)
            if event.source == 2 and event.point == 65536:
                self.submissions[command].append((event.submission_id, event.completion_id))
            elif event.source == 2 and event.point == 65537:
                secondary = args.get("secondary_command_buffer_id")
                if secondary:
                    child = (event.process_id, secondary)
                    self.secondary_commands[command].append(child)
                    self.primary_commands[child].add(command)
            elif event.source == 1:
                encoder_id = args.get("encoder_id")
                if encoder_id:
                    encoder = (event.process_id, encoder_id)
                    self.encoder_metadata[encoder].append((event, args))
                    if event.command_buffer_id:
                        self.command_encoders[command].add(encoder)
                        self.encoder_commands[encoder].add(command)

                if event.command_buffer_id:
                    self.command_metadata[command].append((event, args))

    def commands_for(self, event, args):
        if event.command_buffer_id:
            return {(event.process_id, event.command_buffer_id)}

        return self.encoder_commands.get((event.process_id, args.get("encoder_id", 0)), set())

    def completions_for(self, command):
        pending = [command]
        visited = set()
        completions = set()
        while pending:
            current = pending.pop()
            if current in visited:
                continue

            visited.add(current)
            completions.update(completion for _, completion in self.submissions.get(current, ()) if completion)
            pending.extend(self.primary_commands.get(current, ()))

        return completions

    def describe(self, process_id, command_id):
        pending = [(process_id, command_id)]
        visited = set()
        encoders = set()
        pipelines = set()
        metal_commands = set()
        textures = set()
        draws = 0
        extents = set()
        while pending:
            command = pending.pop()
            if command in visited:
                continue

            visited.add(command)
            pending.extend(self.secondary_commands.get(command, ()))
            encoders.update(self.command_encoders.get(command, ()))
            for event, args in self.command_metadata.get(command, ()):
                if event.point == 6:
                    draws += 1
                if event.point == 5:
                    pipelines.add(args.get("vulkan_pipeline", 0))

        for encoder in encoders:
            for event, args in self.encoder_metadata[encoder]:
                if args.get("metal_command_buffer_id"):
                    metal_commands.add(args["metal_command_buffer_id"])
                if event.point == 2:
                    extents.add((args.get("width", 0), args.get("height", 0)))
                if event.point in (3, 4) and args.get("texture_id"):
                    textures.add(args["texture_id"])

        return {
            "metal_command_buffer_ids": ",".join(str(value) for value in sorted(metal_commands)),
            "metal_encoder_ids": ",".join(str(value[1]) for value in sorted(encoders)),
            "recorded_draws": draws,
            "vulkan_pipeline_ids": ",".join(hex(value) for value in sorted(pipelines)),
            "texture_ids": ",".join(str(value) for value in sorted(textures)),
            "render_pass_extents": ",".join(f"{width}x{height}" for width, height in sorted(extents)),
        }


class ClockAlignment:
    """ClockAlignment pairs GPU readings with their enclosing CPU readings.

    The CPU midpoint and GPU midpoint form each synchronization sample. The
    enclosing read spans remain available as an uncertainty bound. The kernel
    already owns the GMU performance request while obtaining these samples.
    """

    def __init__(self, capture):
        self.hz = capture.status["gpu_clock_hz"]
        if not self.hz:
            raise ValueError("The capture has no GPU clock frequency")

        self.pairs = []
        for sample in capture.samples:
            if (sample.cpu_end_ns < sample.cpu_begin_ns or
                    sample.gpu_end_ticks < sample.gpu_begin_ticks or
                    sample.cpu_end_ns - sample.cpu_begin_ns > 1_000_000_000):
                continue

            cpu = (sample.cpu_begin_ns + sample.cpu_end_ns) // 2
            gpu = self.nanoseconds((sample.gpu_begin_ticks + sample.gpu_end_ticks) // 2)
            uncertainty = (sample.cpu_end_ns - sample.cpu_begin_ns +
                           self.nanoseconds(sample.gpu_end_ticks - sample.gpu_begin_ticks)) // 2
            self.pairs.append((cpu, gpu, uncertainty))

        self.pairs.sort()
        if not self.pairs:
            raise ValueError("No valid hardware clock synchronization samples were saved")

        self.cpu_times = [pair[0] for pair in self.pairs]
        self.gpu_pairs = sorted(self.pairs, key=lambda pair: pair[1])
        self.gpu_times = [pair[1] for pair in self.gpu_pairs]

    def nanoseconds(self, ticks):
        return ticks * 1_000_000_000 // self.hz

    @staticmethod
    def nearest(values, value):
        index = bisect_left(values, value)
        if index == len(values):
            return index - 1

        if index and value - values[index - 1] <= values[index] - value:
            return index - 1

        return index

    def cpu_time(self, gpu_ns):
        cpu, gpu, _ = self.gpu_pairs[self.nearest(self.gpu_times, gpu_ns)]
        return cpu + gpu_ns - gpu

    def job_ticks(self, job):
        cpu, gpu, _ = self.pairs[self.nearest(self.cpu_times, job["retired_ns"])]
        estimated_ns = gpu + job["retired_ns"] - cpu
        estimated_ticks = estimated_ns * self.hz // 1_000_000_000
        rb = (estimated_ticks & ~TICK_MASK) | job["rb_ticks"]
        if rb - estimated_ticks > 1 << 31:
            rb -= 1 << 32
        elif estimated_ticks - rb > 1 << 31:
            rb += 1 << 32

        cp = rb - ((job["rb_ticks"] - job["cp_ticks"]) & TICK_MASK)
        issue = cp - ((job["cp_ticks"] - job["issue_ticks"]) & TICK_MASK)
        return issue, cp, rb


class PerfettoCapture:
    """PerfettoCapture writes named tracks, GPU slices and counter readings.

    Each overlapping interval gets a separate lane so Perfetto's slice stack
    cannot merge unrelated submissions. The original binary records stay next
    to the trace; metadata-only events never acquire a fabricated GPU duration.
    """

    def __init__(self, capture, stream):
        self.capture = capture
        self.clock = ClockAlignment(capture)
        self.work = MetalWork(capture.events)
        self.builder = StreamingTraceProtoBuilder(stream)
        self.tracks = {}
        self.pending = []
        self.lanes = defaultdict(list)
        self.serial = 0
        self.summary = {"generation": capture.status["generation"], "gpu_jobs": 0,
                        "gpu_stages": 0, "metal_events": 0, "metal_events_linked": 0, "counter_values": 0,
                        "unmatched_begin": 0, "unmatched_end": 0, "invalid_events": 0,
                        "invalid_jobs": 0, "counter_resets": 0, "counter_activity": {},
                        "clock_samples": len(self.clock.pairs),
                        "clock_uncertainty_max_ns": max(pair[2] for pair in self.clock.pairs)}

    def track(self, name, parent=None, unit=None):
        key = (name, parent, unit)
        if key in self.tracks:
            return self.tracks[key]

        identifier = len(self.tracks) + 1
        self.tracks[key] = identifier
        packet = self.builder.create_packet()
        packet.trusted_packet_sequence_id = SEQUENCE
        packet.timestamp = self.capture.status["started_ns"]
        packet.timestamp_clock_id = MONOTONIC
        descriptor = packet.track_descriptor
        descriptor.uuid = identifier
        descriptor.name = name
        if parent is not None:
            descriptor.parent_uuid = parent

        if unit is not None:
            descriptor.counter.unit_name = unit
            descriptor.counter.is_incremental = False

        self.builder.write_packet(packet)
        return identifier

    def emit(self, track, timestamp, clock_domain, phase, name="", arguments=None, counter=None, flow=None):
        cpu_time = self.clock.cpu_time(timestamp) if clock_domain == GPU_CLOCK else timestamp
        self.serial += 1
        self.pending.append((cpu_time, self.serial, track, timestamp, clock_domain,
                             phase, name, arguments or {}, counter, flow))

    def slice(self, parent, name, start, end, arguments, flow=None):
        if end < start:
            self.summary["invalid_events"] += 1
            return

        lanes = self.lanes[(parent, name)]
        for index, available in enumerate(lanes):
            if available <= start:
                lanes[index] = end
                break
        else:
            index = len(lanes)
            lanes.append(end)

        track_name = name if not index else f"{name} lane {index + 1}"
        track = self.track(track_name, parent)
        self.emit(track, start, GPU_CLOCK, BEGIN, name, arguments, flow=flow)
        self.emit(track, end, GPU_CLOCK, END)

    def clocks(self):
        for cpu, gpu, _ in self.clock.pairs:
            packet = self.builder.create_packet()
            packet.trusted_packet_sequence_id = SEQUENCE
            packet.timestamp = cpu
            packet.timestamp_clock_id = MONOTONIC
            snapshot = packet.clock_snapshot
            snapshot.primary_trace_clock = MONOTONIC
            reading = snapshot.clocks.add()
            reading.clock_id = MONOTONIC
            reading.timestamp = cpu
            reading = snapshot.clocks.add()
            reading.clock_id = GPU_SEQUENCE_CLOCK
            reading.timestamp = gpu
            self.builder.write_packet(packet)

    def hardware(self):
        parent = self.track("A642L GPU")
        for job in sorted(self.capture.jobs, key=lambda row: row["completion"]):
            if (job["generation"] != self.capture.status["generation"] or
                    not job["valid_flags"] & 2):
                self.summary["invalid_jobs"] += 1
                continue

            issue, cp, rb = self.clock.job_ticks(job)
            if not 0 <= rb - cp <= 65 * self.clock.hz:
                self.summary["invalid_jobs"] += 1
                continue

            start, end = self.clock.nanoseconds(cp), self.clock.nanoseconds(rb)
            self.slice(parent, "Command processor start to render backend completion", start, end,
                       job, flow=job["completion"] or None)
            self.summary["gpu_jobs"] += 1
            if job["valid_flags"] & 1 and 0 <= cp - issue <= 65 * self.clock.hz:
                self.slice(parent, f"Queue {job['queue']} submitted work awaiting command processor",
                           self.clock.nanoseconds(issue), start, job)

        counters_parent = self.track("A642L hardware counters")
        previous = None
        for sample in self.capture.samples:
            if sample.gpu_end_ticks < sample.gpu_begin_ticks:
                previous = None
                continue

            timestamp = self.clock.nanoseconds((sample.gpu_begin_ticks + sample.gpu_end_ticks) // 2)
            if previous is not None:
                prior, prior_time = previous
                duration = timestamp - prior_time
                if duration > 0:
                    for definition in self.capture.definitions:
                        index = definition["index"]
                        if index >= len(sample.values) or not (sample.valid_mask & prior.valid_mask & (1 << index)):
                            continue

                        difference = sample.values[index] - prior.values[index]
                        if difference < 0:
                            self.summary["counter_resets"] += 1
                            continue

                        name = definition["name"]
                        unit = f"{definition['unit']}/ms"
                        track = self.track(COUNTER_NAMES.get(name, name), counters_parent, unit)
                        arguments = {**definition, "interval_ns": duration, "delta": difference,
                                     "raw_value": sample.values[index]}
                        self.emit(track, timestamp, GPU_CLOCK, 4, arguments=arguments,
                                  counter=difference * 1_000_000 / duration)
                        self.summary["counter_values"] += 1
                        self.summary["counter_activity"][name] = self.summary["counter_activity"].get(name, 0) + difference

            previous = (sample, timestamp)

        clock_track = self.track("CPU to GPU clock alignment uncertainty", unit="ns")
        for cpu, _, uncertainty in self.clock.pairs:
            self.emit(clock_track, cpu, CPU_CLOCK, 4, counter=uncertainty)

    def user_events(self):
        stacks = defaultdict(list)
        events = sorted(enumerate(self.capture.events), key=lambda pair: (
            self.clock.cpu_time(pair[1].timestamp_ns) if pair[1].clock_domain == GPU_CLOCK else pair[1].timestamp_ns,
            pair[0]))
        slices = []
        for _, event in events:
            if event.flags & INVALID or event.clock_domain not in (GPU_CLOCK, CPU_CLOCK):
                self.summary["invalid_events"] += 1
                continue

            key = (event.source, event.process_id, event.queue_id, event.point,
                   event.submission_id, event.command_buffer_id, event.name)
            arguments = event_arguments(event)
            if event.source == 1:
                parent = self.track(f"Metal process {event.process_id}")
                track = self.track("Recorded Metal work", parent)
                completions = set()
                for command in self.work.commands_for(event, arguments):
                    completions.update(self.work.completions_for(command))

                if event.completion_id:
                    completions.add(event.completion_id)

                if completions:
                    self.summary["metal_events_linked"] += 1
                    arguments["gpu_completion_ids"] = ",".join(str(value) for value in sorted(completions))

                self.emit(track, event.timestamp_ns, event.clock_domain, INSTANT, event.name,
                          arguments, flow=min(completions) if completions else None)
                self.summary["metal_events"] += 1
            elif event.phase == BEGIN:
                stacks[key].append(event)
            elif event.phase == END:
                if stacks[key]:
                    start = stacks[key].pop()
                    slices.append((start, event, {**event_arguments(start), **arguments}))
                else:
                    self.summary["unmatched_end"] += 1
            else:
                parent = self.track(f"Turnip process {event.process_id} queue {event.queue_id}")
                self.emit(parent, event.timestamp_ns, event.clock_domain, INSTANT, event.name,
                          arguments, flow=event.completion_id or None)

        self.summary["unmatched_begin"] = sum(len(stack) for stack in stacks.values())
        for start, end, arguments in sorted(slices, key=lambda item: item[0].timestamp_ns):
            parent = self.track(f"Turnip process {start.process_id} queue {start.queue_id}")
            if start.clock_domain != GPU_CLOCK or end.clock_domain != GPU_CLOCK:
                self.summary["invalid_events"] += 1
                continue

            arguments.update(self.work.describe(start.process_id, start.command_buffer_id))
            self.slice(parent, start.name, start.timestamp_ns, end.timestamp_ns, arguments,
                       flow=start.completion_id or None)
            self.summary["gpu_stages"] += 1

    @staticmethod
    def annotation(event, name, value):
        if isinstance(value, float) and not math.isfinite(value):
            return

        annotation = event.debug_annotations.add()
        annotation.name = name
        if isinstance(value, bool):
            annotation.bool_value = value
        elif isinstance(value, int):
            if value >= 0:
                annotation.uint_value = value
            else:
                annotation.int_value = value
        elif isinstance(value, float):
            annotation.double_value = value
        else:
            annotation.string_value = str(value)

    def write(self):
        self.clocks()
        self.hardware()
        self.user_events()
        status_track = self.track("Capture status")
        self.emit(status_track, self.capture.status["started_ns"], CPU_CLOCK, INSTANT,
                  "Bounded GPU capture", self.capture.status)
        for _, _, track, timestamp, domain, phase, name, arguments, counter, flow in sorted(self.pending):
            packet = self.builder.create_packet()
            packet.trusted_packet_sequence_id = SEQUENCE
            packet.timestamp = timestamp
            packet.timestamp_clock_id = GPU_SEQUENCE_CLOCK if domain == GPU_CLOCK else MONOTONIC
            event = packet.track_event
            event.track_uuid = track
            event.type = phase
            if name:
                event.name = name

            if counter is not None:
                event.double_counter_value = float(counter)

            if flow:
                event.flow_ids.append(flow)

            for key, value in arguments.items():
                self.annotation(event, key, value)

            self.builder.write_packet(packet)

        lost = sum(self.capture.status[field] for field in (
            "sample_dropped", "job_dropped", "producer_dropped"))
        missing_events = self.capture.status.get("host_missing_events", 0)
        if self.capture.status.get("streaming"):
            lost += missing_events
            self.summary["events_overwritten_before_ack"] = self.capture.status["event_dropped"]
        else:
            lost += self.capture.status["event_dropped"]
        activity = self.summary["counter_activity"]
        self.summary["hardware_counters_populated"] = self.summary["counter_values"] > 0 and any(activity.values())
        self.summary["shader_activity_populated"] = any(activity.get(name, 0) > 0 for name in (
            "vs_alu", "fs_alu", "fs_tex", "vs_tex"))
        self.summary["dropped_records"] = lost
        self.summary["incomplete_producers"] = self.capture.status["producer_failed"]
        self.summary["host_missing_events"] = missing_events
        self.summary["complete"] = bool(
            self.summary["gpu_jobs"] and self.summary["gpu_stages"] and self.summary["metal_events_linked"] and
            self.summary["hardware_counters_populated"] and self.summary["shader_activity_populated"] and
            not lost and not self.summary["incomplete_producers"] and not self.summary["host_missing_events"] and
            not self.capture.status["access_error"] and
            not self.summary["unmatched_begin"] and not self.summary["unmatched_end"] and
            not self.summary["invalid_events"] and not self.summary["invalid_jobs"])
        return self.summary


def export_capture(capture, output):
    with output.open("xb") as stream:
        result = PerfettoCapture(capture, stream).write()

    write_json(output.with_suffix(".summary.json"), result)
    return result
