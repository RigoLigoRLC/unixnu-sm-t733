#!/usr/bin/env python3
"""Collect the T733 kernel CDC byte stream and reconnect after USB enumeration.

One collector owns the matching serial port and a control socket per output
directory. Commands from other invocations use that socket, so status queries
never reopen the serial port or toggle DTR. Each connection has its own raw
file; events.jsonl records connection times and raw-file byte offsets.
"""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import select
import signal
import socket
import sys
import time


VID = 0x1209
PID = 0x0001
SERIAL = "T733DBG001"


def timestamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def is_device_command(command):
    if command in ("status", "replay", "ping", "help", "tasks", "cpus"):
        return True

    if not command.startswith("tasks "):
        return False

    name = command[6:]
    return 0 < len(name) <= 56 and all(32 <= ord(char) < 127 for char in name)


def command_client(path, command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(3)
        try:
            client.connect(str(path))
            client.sendall((command + "\n").encode("ascii"))
            reply = bytearray()
            while b"\n" not in reply and len(reply) < 4096:
                block = client.recv(4096)
                if not block:
                    break
                reply.extend(block)
        except (OSError, TimeoutError) as error:
            print(f"collector unavailable: {error}", file=sys.stderr)
            return 1
    print(reply.decode("utf-8", errors="replace").strip())
    try:
        return 0 if json.loads(reply).get("ok") else 1
    except (ValueError, UnicodeError):
        return 1


def collect(args, output, control):
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("Install tools/requirements-usb-console.txt in the project Python environment.", file=sys.stderr)
        return 1

    output.mkdir(parents=True, exist_ok=True)
    lock_file = (output / "collector.lock").open("a+")
    try:
        fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("A collector already owns this output directory.", file=sys.stderr)
        lock_file.close()
        return 1

    running = True
    port = None
    raw = None
    raw_path = None
    received = 0
    last_discovery = 0.0
    last_flush = 0.0
    events = (output / "events.jsonl").open("a", encoding="utf-8", buffering=1)

    def record(kind, **fields):
        entry = {"time": timestamp(), "event": kind, **fields}
        events.write(json.dumps(entry, ensure_ascii=True) + "\n")
        if kind != "receive":
            print(json.dumps(entry, ensure_ascii=True), flush=True)

    def finish_connection(reason):
        nonlocal port, raw, raw_path, received
        if port is not None:
            try:
                port.close()
            except OSError:
                pass
            port = None
        if raw is not None:
            raw.flush()
            os.fsync(raw.fileno())
            raw.close()
            record("disconnected", reason=reason, file=str(raw_path), bytes=received)
            raw = None
        raw_path = None
        received = 0

    def stop_signal(_number, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop_signal)
    signal.signal(signal.SIGTERM, stop_signal)
    control.unlink(missing_ok=True)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(control))
    os.chmod(control, 0o600)
    server.listen(4)
    server.setblocking(False)
    record("collector-start", pid=os.getpid(), output=str(output), serial=SERIAL)

    try:
        while running:
            readable, _, _ = select.select([server], [], [], 0)
            if readable:
                client, _ = server.accept()
                with client:
                    client.settimeout(0.25)
                    try:
                        command = client.recv(128).decode("ascii").strip()
                        if command == "host-status":
                            reply = {"ok": True, "running": True, "connected": port is not None,
                                     "output": str(output), "file": str(raw_path) if raw_path else None,
                                     "bytes": received, "pid": os.getpid()}
                        elif command == "stop":
                            running = False
                            reply = {"ok": True, "stopping": True}
                        elif is_device_command(command):
                            if port is None:
                                reply = {"ok": False, "error": "T733 CDC is disconnected"}
                            else:
                                port.write((command + "\n").encode("ascii"))
                                record("command", command=command)
                                reply = {"ok": True, "sent": command, "response_file": str(raw_path)}
                        else:
                            reply = {"ok": False, "error": "unknown command"}
                        client.sendall((json.dumps(reply) + "\n").encode("utf-8"))
                    except (OSError, ValueError, serial.SerialException) as error:
                        record("control-error", error=str(error))

            now = time.monotonic()
            if port is None and now - last_discovery >= 1:
                last_discovery = now
                candidates = [item for item in list_ports.comports()
                              if item.vid == VID and item.pid == PID and item.serial_number == SERIAL]
                candidates.sort(key=lambda item: (not item.device.startswith("/dev/cu."), item.device))
                if candidates:
                    candidate = candidates[0]
                    try:
                        port = serial.Serial(baudrate=115200, timeout=0.1, write_timeout=0.5,
                                             rtscts=False, dsrdtr=False, exclusive=True)
                        port.dtr = True
                        port.rts = False
                        port.port = candidate.device
                        port.open()
                        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
                        raw_path = output / f"t733-{stamp}.raw"
                        raw = raw_path.open("xb", buffering=0)
                        received = 0
                        record("connected", port=candidate.device, file=str(raw_path), serial=SERIAL)
                        port.write(b"status\n")
                        if args.replay_on_connect:
                            port.write(b"replay\n")
                    except (OSError, serial.SerialException) as error:
                        record("open-error", port=candidate.device, error=str(error))
                        finish_connection(str(error))

            if port is not None:
                try:
                    block = port.read(min(max(port.in_waiting, 1), 65536))
                    if block:
                        raw.write(block)
                        received += len(block)
                        record("receive", file=raw_path.name, offset=received - len(block), size=len(block))
                        if args.echo:
                            sys.stdout.buffer.write(block)
                            sys.stdout.buffer.flush()
                    if now - last_flush >= 1:
                        os.fsync(raw.fileno())
                        last_flush = now
                except (OSError, serial.SerialException) as error:
                    finish_connection(str(error))
            else:
                time.sleep(0.1)
    finally:
        finish_connection("collector stopped")
        record("collector-stop")
        server.close()
        control.unlink(missing_ok=True)
        events.close()
        lock_file.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="Persistent capture directory")
    parser.add_argument("--command", choices=("host-status", "stop", "status", "replay", "ping", "help", "tasks", "cpus"))
    parser.add_argument("--process", help="Exact process name to sample with --command tasks")
    parser.add_argument("--echo", action="store_true", help="Also write raw received bytes to stdout")
    parser.add_argument("--replay-on-connect", action="store_true", help="Replay retained RAM after every connection")
    args = parser.parse_args()
    if args.process is not None:
        if args.command != "tasks" or not is_device_command("tasks " + args.process):
            parser.error("--process requires --command tasks and a printable ASCII name of 1-56 characters.")

        args.command = "tasks " + args.process

    output = args.output.expanduser().resolve()
    control = output / "control.sock"
    if len(os.fsencode(control)) > 103:
        parser.error("The output path is too long for a macOS Unix socket; use a shorter directory.")
    if args.command:
        return command_client(control, args.command)
    return collect(args, output, control)


if __name__ == "__main__":
    raise SystemExit(main())
