"""Binary capture transport and records shared by the T733 host commands.

The device retains one bounded capture until every producer has finished and
the host seals it. These readers preserve that capture in separate binary
files before the Perfetto exporter interprets its timestamps and arguments.
The wire definitions are in drivers/Adreno6xx/NativeAdrenoTrace.h.
"""

from dataclasses import dataclass
import json
from pathlib import Path
import socket
import struct


VERSION = 1
MAGIC = 0x43525441
PORT = 1235
QUERY = 33
CONTROL = 34
READ = 35
START = 1
STOP = 2
SEAL = 3
IDLE = 0
RUNNING = 2
COMPLETE = 4
SEALED = 5
GPU_CLOCK = 1
CPU_CLOCK = 2
BEGIN = 1
END = 2
INSTANT = 3
INVALID = 1
HEADER = struct.Struct("<8I")
STATUS = struct.Struct("<4I2Q24I")
CHUNK = struct.Struct("<8I")
DEFINITION = struct.Struct("<48s16s8I")
SAMPLE = struct.Struct("<33Q2I")
JOB = struct.Struct("<2Q8I")
EVENT_HEADER = struct.Struct("<4I4Q4I")
ARGUMENT = struct.Struct("<IIQ")
EVENT_BYTES = 176
STATUS_NAMES = (
    "version", "generation", "state", "access_error", "started_ns", "stopped_ns",
    "counter_count", "sample_count", "job_count", "event_count",
    "sample_dropped", "job_dropped", "event_dropped", "producer_dropped",
    "pending_jobs", "hardware_owned", "gpu_clock_hz", "sample_interval_us",
    "sample_capacity", "job_capacity", "event_capacity", "event_bytes",
    "sample_bytes", "job_bytes", "definition_bytes", "producer_count",
    "producer_failed", "reserved0", "reserved1", "reserved2",
)
STATE_NAMES = {0: "idle", 1: "requested", 2: "running", 3: "draining", 4: "complete", 5: "sealed"}
SECTIONS = (
    (1, "definitions.bin", "counter_count", "definition_bytes"),
    (2, "counters.bin", "sample_count", "sample_bytes"),
    (3, "jobs.bin", "job_count", "job_bytes"),
    (4, "events.bin", "event_count", "event_bytes"),
)


def c_string(value):
    return value.split(b"\0", 1)[0].decode("utf-8", "replace")


def decode_status(payload):
    if len(payload) != STATUS.size:
        raise ValueError(f"Trace status has {len(payload)} bytes; expected {STATUS.size}")

    status = dict(zip(STATUS_NAMES, STATUS.unpack(payload)))
    if status["version"] != VERSION:
        raise ValueError(f"Unsupported trace ABI {status['version']}")

    status["state_name"] = STATE_NAMES.get(status["state"], "unknown")
    return status


class DeviceError(RuntimeError):
    """DeviceError retains the IOReturn from one failed trace request."""

    def __init__(self, method, status):
        self.method = method
        self.status = status
        super().__init__(f"Trace method {method} returned IOReturn 0x{status:08x}")


class TraceConnection:
    """TraceConnection sends bounded requests over the tablet's USB network.

    The existing debug network helper owns the TCP server and the IOKit
    connection. This client only controls capture and reads sealed records;
    it never opens the USB console or reads GPU registers.
    """

    def __init__(self, host="192.168.73.2", port=PORT, timeout=10):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.request_id = 0

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.socket.close()

    def receive(self, count):
        result = bytearray()
        while len(result) < count:
            block = self.socket.recv(count - len(result))
            if not block:
                raise ConnectionError("The tablet closed the trace connection")

            result.extend(block)

        return bytes(result)

    def request(self, method, payload=b""):
        self.request_id += 1
        header = HEADER.pack(MAGIC, VERSION, method, self.request_id, len(payload), 0, 0, 0)
        self.socket.sendall(header + payload)
        magic, version, reply_method, request_id, count, status, _, _ = HEADER.unpack(self.receive(HEADER.size))
        if (magic, version, reply_method, request_id) != (MAGIC, VERSION, method, self.request_id):
            raise ValueError("The tablet returned a mismatched trace response")

        if count > 4096:
            raise ValueError(f"Trace response exceeds the wire limit: {count}")

        reply = self.receive(count)
        if status:
            raise DeviceError(method, status)

        return reply

    def status(self):
        return decode_status(self.request(QUERY))

    def control(self, operation, generation=0, duration_ms=0):
        payload = struct.pack("<4I", VERSION, operation, generation, duration_ms)
        return decode_status(self.request(CONTROL, payload))

    def download(self, output, status):
        if status["state"] != SEALED:
            raise ValueError("The capture must be sealed before downloading")

        output = Path(output)
        output.mkdir(parents=True, exist_ok=True)
        generation = status["generation"]
        for section, filename, count_field, size_field in SECTIONS:
            expected = status[count_field] * status[size_field]
            if expected > 16 * 1024 * 1024:
                raise ValueError(f"{filename} exceeds the capture capacity")

            offset = 0
            partial = output / (filename + ".partial")
            with partial.open("xb") as stream:
                while offset < expected:
                    reply = self.request(READ, struct.pack("<4I", VERSION, generation, section, offset))
                    if len(reply) < CHUNK.size:
                        raise ValueError("Truncated trace chunk header")

                    version, returned_generation, returned_section, returned_offset, count, total, _, _ = CHUNK.unpack_from(reply)
                    if (version, returned_generation, returned_section, returned_offset, total) != (
                            VERSION, generation, section, offset, expected):
                        raise ValueError("The trace changed while downloading")

                    if not 0 < count <= min(1536, expected - offset) or len(reply) < CHUNK.size + count:
                        raise ValueError("Invalid trace chunk length")

                    stream.write(reply[CHUNK.size:CHUNK.size + count])
                    offset += count

            partial.rename(output / filename)

        write_json(output / "status.json", status)


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n")


@dataclass
class CounterSample:
    """CounterSample contains one bounded CPU read of the hardware counters."""

    cpu_begin_ns: int
    cpu_end_ns: int
    gpu_begin_ticks: int
    gpu_end_ticks: int
    values: tuple
    valid_mask: int


@dataclass
class TraceEvent:
    """TraceEvent identifies a GPU stage or Metal command and its clock."""

    source: int
    point: int
    phase: int
    clock_domain: int
    timestamp_ns: int
    submission_id: int
    completion_id: int
    command_buffer_id: int
    process_id: int
    queue_id: int
    argument_count: int
    flags: int
    arguments: list
    name: str


def read_records(directory, filename, record_bytes, expected):
    data = (directory / filename).read_bytes()
    if len(data) != record_bytes * expected:
        raise ValueError(f"{filename} has {len(data)} bytes; expected {record_bytes * expected}")

    return (data[offset:offset + record_bytes] for offset in range(0, len(data), record_bytes))


class Capture:
    """Capture reads the sealed binary sections and preserves validity fields.

    Counter definitions supply the hardware event names and units. Job records
    supply the completion receipt and low 32-bit timestamps. The exporter owns
    timestamp extension, clock alignment and matching events to those jobs.
    """

    def __init__(self, directory):
        directory = Path(directory)
        self.status = json.loads((directory / "status.json").read_text())
        if self.status["version"] != VERSION or self.status["state"] != SEALED:
            raise ValueError("The saved capture is not a sealed ABI v1 capture")

        layouts = {"event_bytes": EVENT_BYTES, "sample_bytes": SAMPLE.size,
                   "job_bytes": JOB.size, "definition_bytes": DEFINITION.size}
        for field, expected in layouts.items():
            if self.status[field] != expected:
                raise ValueError(f"Unsupported {field}: {self.status[field]}")

        self.definitions = []
        for raw in read_records(directory, "definitions.bin", DEFINITION.size, self.status["counter_count"]):
            name, unit, index, selector, event, low_register, load_bit, *_ = DEFINITION.unpack(raw)
            self.definitions.append({"name": c_string(name), "unit": c_string(unit), "index": index,
                                     "selector": selector, "event": event, "low_register": low_register,
                                     "load_bit": load_bit})

        self.samples = []
        for raw in read_records(directory, "counters.bin", SAMPLE.size, self.status["sample_count"]):
            values = SAMPLE.unpack(raw)
            self.samples.append(CounterSample(*values[:4], values[4:33], values[33]))

        self.jobs = []
        for raw in read_records(directory, "jobs.bin", JOB.size, self.status["job_count"]):
            names = ("completion", "retired_ns", "fence", "queue", "issue_ticks", "cp_ticks",
                     "rb_ticks", "valid_flags", "generation", "reserved")
            self.jobs.append(dict(zip(names, JOB.unpack(raw))))

        self.events = []
        for raw in read_records(directory, "events.bin", EVENT_BYTES, self.status["event_count"]):
            fields = EVENT_HEADER.unpack_from(raw)
            count = fields[10]
            if count > 4:
                raise ValueError("Trace event has more than four arguments")

            arguments = []
            for index in range(count):
                key, kind, value = ARGUMENT.unpack_from(raw, EVENT_HEADER.size + index * ARGUMENT.size)
                if kind == 2 and value & (1 << 63):
                    value -= 1 << 64
                elif kind == 3:
                    value = struct.unpack("<d", struct.pack("<Q", value))[0]

                arguments.append((key, kind, value))

            self.events.append(TraceEvent(*fields, arguments, c_string(raw[128:176])))
