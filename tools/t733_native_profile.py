"""Native debugserver counters collected while the attached process runs.

Protocol and field definitions come from LLVM debugserver's RNBRemote.cpp,
MachProcess.mm, MachTask.mm and DNBDefs.h. The direct RSP connection preserves
64-bit thread counters before LLDB's thread-index conversion and filtering.
"""

import csv
from datetime import datetime, timezone
import json
from pathlib import Path
import socket
import time
import uuid


SCAN_TYPE = 0x6F  # Host/task/thread CPU, thread names, host/task memory.
END = "--end--;"


class Remote:
    """Remote owns one debugserver connection and preserves incomplete frames.

    Requests run only while the inferior is stopped. During execution the
    caller consumes asynchronous profile frames until its deadline, then sends
    one interrupt before disabling profiling and detaching.
    """

    def __init__(self, connection, capture):
        self.connection = connection
        self.capture = capture
        self.buffer = bytearray()
        self.noack = False
        self.last_frame = b""
        self.retries = 0

    def send(self, payload):
        encoded = payload.encode("ascii")
        self.last_frame = b"$" + encoded + b"#" + f"{sum(encoded) & 255:02x}".encode()
        self.retries = 0
        self.connection.sendall(self.last_frame)

    def packet(self, deadline):
        while True:
            while self.buffer and self.buffer[0] in b"+-":
                acknowledgement = self.buffer.pop(0)
                if acknowledgement == ord("-"):
                    self.retries += 1
                    if self.noack or self.retries > 3:
                        raise RuntimeError("debugserver rejected an RSP packet")
                    self.connection.sendall(self.last_frame)

            if self.buffer and self.buffer[0] != ord("$"):
                raise RuntimeError("Unexpected debugserver framing")
            end = self.buffer.find(b"#", 1)
            if end >= 0 and len(self.buffer) >= end + 3:
                encoded = bytes(self.buffer[1:end])
                checksum = int(self.buffer[end + 1:end + 3], 16)
                del self.buffer[:end + 3]
                if not self.noack and sum(encoded) & 255 != checksum:
                    self.connection.sendall(b"-")
                    continue
                if not self.noack:
                    self.connection.sendall(b"+")

                decoded = bytearray()
                index = 0
                while index < len(encoded):
                    byte = encoded[index]
                    if byte == ord("}"):
                        index += 1
                        if index == len(encoded):
                            raise RuntimeError("Incomplete RSP escape")
                        decoded.append(encoded[index] ^ 0x20)
                    elif byte == ord("*"):
                        index += 1
                        if not decoded or index == len(encoded) or encoded[index] < 29:
                            raise RuntimeError("Invalid RSP run length")
                        decoded.extend([decoded[-1]] * (encoded[index] - 29))
                    else:
                        decoded.append(byte)
                    index += 1
                return decoded.decode("ascii")

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Timed out waiting for debugserver")
            self.connection.settimeout(remaining)
            chunk = self.connection.recv(65536)
            if not chunk:
                raise RuntimeError("debugserver disconnected")
            self.buffer.extend(chunk)
            if len(self.buffer) > 4 * 1024 * 1024:
                raise RuntimeError("debugserver packet exceeded 4 MiB")

    def response(self, timeout=15):
        deadline = time.monotonic() + timeout
        while True:
            packet = self.packet(deadline)
            if packet.startswith("A"):
                self.capture.add(packet[1:])
            elif packet.startswith("O") and packet != "OK":
                self.capture.output.write(bytes.fromhex(packet[1:]))
                self.capture.output.flush()
            else:
                return packet

    def request(self, payload, expected=None, timeout=15):
        self.send(payload)
        response = self.response(timeout)
        if response.startswith("E") or not response:
            raise RuntimeError(f"{payload}: debugserver response {response!r}")
        if expected is not None and response != expected:
            raise RuntimeError(f"{payload}: expected {expected!r}, received {response!r}")
        return response


def parse_sample(raw):
    fields = {}
    threads = []
    current = None
    for item in raw.split(";"):
        if not item:
            continue
        name, separator, value = item.partition(":")
        if not separator:
            raise ValueError("Malformed profile field: " + item)
        if name == "thread_used_id":
            current = {"id": f"0x{int(value, 16):x}"}
            threads.append(current)
        elif name == "thread_used_usec" and current is not None:
            current["used_usec"] = int(value, 10)
        elif name == "thread_used_name" and current is not None:
            current["name_hex"] = value
            try:
                current["name"] = bytes.fromhex(value).decode("utf-8", errors="replace")
            except ValueError:
                current["name"] = None
        else:
            fields[name] = int(value, 10) if value.isdecimal() else value
    return {"fields": fields, "threads": threads}


class Capture:
    """Capture stores raw server records and derives intervals from device time.

    Host tick percentages describe all CPUs together. Thread percentages use
    cumulative user plus system microseconds, with 100 percent equal to one
    fully occupied core. Sampling different threads is not atomic.
    """

    def __init__(self, directory):
        self.directory = directory
        self.raw = (directory / "profile-raw.jsonl").open("x")
        self.output = (directory / "inferior-output.bin").open("xb")
        self.partial = ""
        self.samples = []

    def add(self, data, phase="running"):
        self.partial += data
        if len(self.partial) > 4 * 1024 * 1024:
            raise RuntimeError("Profile record exceeded 4 MiB")
        while END in self.partial:
            record, self.partial = self.partial.split(END, 1)
            received = time.monotonic()
            self.raw.write(json.dumps({"received_monotonic": received, "phase": phase,
                                       "raw": record + END}) + "\n")
            self.raw.flush()
            sample = parse_sample(record)
            sample.update({"received_monotonic": received, "phase": phase,
                           "index": len(self.samples)})
            self.samples.append(sample)

    def finish(self, metadata):
        previous = None
        for sample in self.samples:
            fields = sample["fields"]
            sample["derived"] = {}
            if sample["phase"] != "running":
                continue
            if previous is not None:
                prior = previous["fields"]
                ticks = ["host_user_ticks", "host_sys_ticks", "host_idle_ticks"]
                if all(isinstance(fields.get(key), int) and isinstance(prior.get(key), int)
                       for key in ticks):
                    deltas = [(fields[key] - prior[key]) & 0xFFFFFFFF for key in ticks]
                    if sum(deltas):
                        sample["derived"]["host_busy_percent"] = 100 * sum(deltas[:2]) / sum(deltas)
                elapsed = fields.get("elapsed_usec", 0) - prior.get("elapsed_usec", 0)
                if elapsed > 0:
                    sample["derived"]["interval_usec"] = elapsed
                    old_threads = {thread["id"]: thread for thread in previous["threads"]}
                    for thread in sample["threads"]:
                        old = old_threads.get(thread["id"], {})
                        if "used_usec" in thread and "used_usec" in old:
                            delta = thread["used_usec"] - old["used_usec"]
                            if delta >= 0:
                                thread["cpu_percent_one_core"] = 100 * delta / elapsed
            previous = sample

        metadata.update({"sample_count": len(self.samples), "samples": self.samples,
                         "partial_record": self.partial,
                         "units": {"elapsed_usec": "device wall clock microseconds",
                                   "task_used_usec": "TASK_BASIC_INFO user+system microseconds",
                                   "thread_used_usec": "THREAD_BASIC_INFO user+system microseconds",
                                   "phys_footprint": "process bytes; server may return zero on Mach API failure",
                                   "used": "host used bytes, not process resident memory",
                                   "total": "host physical bytes",
                                   "host_busy_percent": "100*(delta user+system)/(delta user+system+idle)",
                                   "cpu_percent_one_core": "100*delta thread usec/delta device elapsed usec"},
                         "limitations": ["Host packet omits nice ticks and per-core counters.",
                                         "Thread snapshots are non-atomic; new threads require two observations.",
                                         "No process CPU percentage is inferred from task_used_usec alone.",
                                         "Memory API failures are not separately flagged by debugserver."]})
        with (self.directory / "profile.json").open("x") as output:
            json.dump(metadata, output, indent=2)
        names = sorted({key for sample in self.samples for key in sample["fields"]})
        with (self.directory / "samples.csv").open("x", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=["index", "phase", "received_monotonic",
                                                       "interval_usec", "host_busy_percent", *names])
            writer.writeheader()
            for sample in self.samples:
                writer.writerow({"index": sample["index"], "phase": sample["phase"],
                                 "received_monotonic": sample["received_monotonic"],
                                 **sample["fields"], **sample["derived"]})
        with (self.directory / "threads.csv").open("x", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=["sample", "elapsed_usec", "id", "name",
                                                       "name_hex", "used_usec", "cpu_percent_one_core"])
            writer.writeheader()
            for sample in self.samples:
                for thread in sample["threads"]:
                    writer.writerow({"sample": sample["index"],
                                     "elapsed_usec": sample["fields"].get("elapsed_usec"), **thread})
        self.raw.close()
        self.output.close()


def collect(args, device_ip, port):
    destination = args.profile_output
    if destination is None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        destination = Path("out") / ("t733-profile-" + stamp + "-" + uuid.uuid4().hex[:8])
    destination = destination.expanduser().resolve()
    destination.mkdir(parents=True, exist_ok=False)
    capture = Capture(destination)
    metadata = {"protocol": "debugserver QSetEnableAsyncProfiling", "scan_type": hex(SCAN_TYPE),
                "requested_seconds": args.profile_seconds, "interval_seconds": args.profile_interval,
                "requested_pid": args.pid, "requested_name": args.attach, "complete": False,
                "detached": False, "errors": []}
    remote = None
    attached = False
    stopped = False
    enabled = False
    failure = None
    print("Native running-counter profile: " + str(destination), flush=True)
    try:
        connection = socket.create_connection((device_ip, port), timeout=15)
        remote = Remote(connection, capture)
        connection.sendall(b"+")
        remote.request("QStartNoAckMode", "OK")
        remote.noack = True
        remote.request("QSetDetachOnError:1", "OK")
        attach = f"vAttach;{args.pid:x}" if args.pid else "vAttachName;" + args.attach.encode().hex()
        reply = remote.request(attach, timeout=40)
        if reply[:1] not in ("T", "S"):
            raise RuntimeError("Attach did not stop the requested process: " + reply)
        attached = stopped = True
        metadata["attach_reply"] = reply
        baseline = remote.request(f"qGetProfileData;scan_type:0x{SCAN_TYPE:x};")
        if END not in baseline:
            raise RuntimeError("debugserver returned no native profile counters: " + baseline)
        capture.add(baseline, "attached")
        interval = args.profile_interval * 1000000
        remote.request(f"QSetEnableAsyncProfiling;enable:1;interval_usec:{interval};scan_type:0x{SCAN_TYPE:x};", "OK")
        enabled = True
        remote.send("c")
        stopped = False
        start = time.monotonic()
        deadline = start + args.profile_seconds
        metadata["running_started_monotonic"] = start
        while time.monotonic() < deadline:
            try:
                reply = remote.response(deadline - time.monotonic())
            except TimeoutError:
                break
            if reply[:1] in ("T", "S"):
                stopped = True
                raise RuntimeError("Inferior stopped during profiling: " + reply)
            if reply[:1] in ("W", "X"):
                attached = False
                raise RuntimeError("Inferior exited during profiling: " + reply)
            raise RuntimeError("Unexpected running response: " + reply)
        metadata["running_finished_monotonic"] = time.monotonic()
        if sum(sample["phase"] == "running" for sample in capture.samples) < 2:
            raise RuntimeError("Fewer than two running samples; CPU intervals are unavailable")
        metadata["complete"] = True
    except (OSError, RuntimeError, ValueError, KeyboardInterrupt) as error:
        failure = error
        metadata["errors"].append(str(error) or type(error).__name__)
    finally:
        if remote is not None:
            try:
                if attached:
                    if not stopped:
                        remote.connection.sendall(b"\x03")
                        reply = remote.response(15)
                        stopped = reply[:1] in ("T", "S")
                        if not stopped:
                            raise RuntimeError("Final interrupt did not stop inferior: " + reply)
                    if enabled:
                        remote.request("QSetEnableAsyncProfiling;enable:0;", "OK")
                    remote.request("D", "OK")
                    metadata["detached"] = True
            except (OSError, RuntimeError, ValueError) as error:
                metadata["errors"].append("cleanup: " + str(error))
                metadata["complete"] = False
                failure = failure or error
            finally:
                remote.connection.close()
        capture.finish(metadata)
    print("Profile saved: " + str(destination), flush=True)
    if failure is not None:
        raise RuntimeError("Native profile incomplete: " + str(failure))
