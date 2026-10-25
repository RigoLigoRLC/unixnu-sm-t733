#!/usr/bin/env python3
"""Host diagnostics for the T733 ECM link using Apple's lockdown services."""

import argparse
import asyncio
from datetime import datetime, timezone
from importlib.metadata import version
import json
import logging
import os
from pathlib import Path
import plistlib
import sys
import uuid

from pymobiledevice3.lockdown import TcpLockdownClient
from pymobiledevice3.service_connection import ServiceConnection
from pymobiledevice3.services.crash_reports import CrashReportsManager
from pymobiledevice3.services.os_trace import OsTraceService
from pymobiledevice3.services.syslog import SyslogService


HOST = "192.168.73.2"
PORT = 62078
CLIENT_VERSION = "11.43.1"
DEVICE_ROOT = Path(__file__).resolve().parents[1]
PAIRING_DIRECTORY = DEVICE_ROOT / ".cache/ios-diagnostics/pairing"
OUTPUT_DIRECTORY = DEVICE_ROOT / "out/ios-diagnostics"


class WorkspaceLockdownClient(TcpLockdownClient):
    """WorkspaceLockdownClient loads only this workspace's real pairing record.

    The inherited TCP transport and pairing protocol handle device communication.
    This lookup excludes system usbmuxd and unrelated host pairing directories.
    """

    async def fetch_pair_record(self):
        path = PAIRING_DIRECTORY / f"{HOST}.plist"
        if path.exists():
            with path.open("rb") as stream:
                self.pair_record = plistlib.load(stream)


class CopyWarnings(logging.Handler):
    """CopyWarnings retains AFC errors that the library logs while continuing."""

    def __init__(self):
        super().__init__(logging.WARNING)
        self.errors = []

    def emit(self, record):
        message = record.getMessage()
        if "(Ignoring) Error:" in message:
            self.errors.append(message)


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=True)
        stream.write("\n")


async def connect(pair, timeout):
    async with asyncio.timeout(timeout + (60 if pair else 0)):
        service = await ServiceConnection.create_using_tcp(HOST, PORT, keep_alive=True)
        try:
            return await WorkspaceLockdownClient.create(
                service,
                hostname=HOST,
                port=PORT,
                label="t733-ios-diagnostics",
                autopair=pair,
                pair_timeout=60,
                pairing_records_cache_folder=PAIRING_DIRECTORY,
                keep_alive=True,
            )
        except BaseException:
            await service.close()
            raise


async def collect(args, output):
    async with await connect(args.command == "pair", args.timeout) as lockdown:
        status = {
            "address": f"{HOST}:{PORT}",
            "paired": lockdown.paired,
            "client_version": CLIENT_VERSION,
            "product_version": lockdown.all_values.get("ProductVersion"),
            "build_version": lockdown.all_values.get("BuildVersion"),
            "product_type": lockdown.all_values.get("ProductType"),
        }
        write_json(output / "status.json", status)
        print(json.dumps(status, ensure_ascii=True), flush=True)

        if args.command in ("pair", "status"):
            return
        if not lockdown.paired:
            raise RuntimeError("No validated pairing session. Run the pair command first.")

        if args.command == "syslog":
            async with SyslogService(lockdown) as relay:
                with (output / "syslog.raw").open("xb") as stream:
                    try:
                        async with asyncio.timeout(args.seconds):
                            while True:
                                chunk = await relay.service.recv_any(4096)
                                if not chunk:
                                    raise ConnectionError("syslog relay closed the connection")
                                stream.write(chunk)
                                stream.flush()
                    except TimeoutError:
                        pass
            return

        async with asyncio.timeout(args.timeout):
            if args.command == "crashreports":
                warnings = CopyWarnings()
                logger = logging.getLogger("pymobiledevice3.services.afc")
                logger.addHandler(warnings)
                try:
                    async with CrashReportsManager(lockdown) as reports:
                        if args.flush:
                            await reports.flush()
                        destination = output / "crashreports"
                        destination.mkdir()
                        await reports.pull(str(destination), erase=False, progress_bar=False)
                finally:
                    logger.removeHandler(warnings)
                if warnings.errors:
                    raise RuntimeError("Partial crash report copy:\n" + "\n".join(warnings.errors))
            elif args.command == "logarchive":
                async with OsTraceService(lockdown) as trace:
                    with (output / "system-logs.tar").open("xb") as stream:
                        await trace.create_archive(
                            stream,
                            size_limit=args.size_limit,
                            age_limit=args.age_days,
                        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("pair", "status", "syslog", "crashreports", "logarchive"))
    parser.add_argument("--timeout", type=float, default=300, help="Connection or finite operation deadline in seconds")
    parser.add_argument("--seconds", type=float, default=60, help="Duration of syslog capture")
    parser.add_argument("--flush", action="store_true", help="Ask the native crash mover to collect pending reports before copying")
    parser.add_argument("--size-limit", type=int, default=268435456, help="Requested log archive byte limit")
    parser.add_argument("--age-days", type=int, default=1, help="Requested log archive maximum age")
    args = parser.parse_args()
    if args.timeout <= 0 or args.seconds <= 0 or args.size_limit <= 0 or args.age_days <= 0:
        parser.error("timeouts and archive limits must be positive")
    if version("pymobiledevice3") != CLIENT_VERSION:
        parser.error(f"Install requirements-ios-diagnostics.txt; pymobiledevice3 {CLIENT_VERSION} is required")

    os.umask(0o077)
    PAIRING_DIRECTORY.mkdir(parents=True, exist_ok=True, mode=0o700)
    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True, mode=0o700)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = OUTPUT_DIRECTORY / f"{stamp}-{args.command}-{uuid.uuid4().hex[:8]}"
    output.mkdir(mode=0o700)
    print(f"Output: {output}", flush=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(levelname)s %(name)s: %(message)s",
        handlers=[logging.StreamHandler(), logging.FileHandler(output / "client.log", mode="x")],
    )
    try:
        asyncio.run(collect(args, output))
    except KeyboardInterrupt:
        print("Interrupted; partial output retained.", file=sys.stderr)
        return 130
    except Exception as error:
        message = f"{type(error).__name__}: {error!s} (args={error.args!r})"
        with (output / "error.txt").open("x", encoding="utf-8") as stream:
            stream.write(message + "\n")
        print(message, file=sys.stderr)
        return 1
    write_json(output / "complete.json", {"command": args.command, "completed": True})
    print(f"Completed: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
