#!/usr/bin/env python3
"""Copy the OS windows from a SHNGROOT image and sign their block policies."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def copy_window(image, offset, size, destination):
    partial = destination.with_name(destination.name + ".incomplete")
    remaining = size
    with image.open("rb") as source, partial.open("xb") as target:
        source.seek(offset)
        while remaining:
            chunk = source.read(min(8 * 1024 * 1024, remaining))
            if not chunk:
                raise ValueError("Root image ended inside " + destination.name)

            target.write(chunk)
            remaining -= len(chunk)

    partial.rename(destination)


def run(command, log):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root-image", type=Path, required=True)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--volumes", type=Path, required=True)
    parser.add_argument("--manifest-tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    layout = json.loads(args.layout.read_text())
    windows = {item["name"]: item for item in layout["windows"]}
    volumes = {item["component"]: item for item in json.loads(args.volumes.read_text())}
    image_bytes = args.root_image.stat().st_size
    for name in ("OS", "SystemOS"):
        window = windows[name]
        if window["offset"] < 4096 or window["size"] <= 0:
            raise ValueError("Invalid root window: " + name)

        if window["offset"] + window["size"] > image_bytes:
            raise ValueError("Root window exceeds image: " + name)

    os.umask(0o077)
    output = args.output.resolve()
    output.mkdir(parents=True, mode=0o700, exist_ok=False)
    keys = output / "keys"
    keys.mkdir(mode=0o700)
    private_key = keys / "owner.pem"
    public_key = keys / "owner-public.pem"
    run(["openssl", "genrsa", "-out", str(private_key), "3072"], output / "key-generation.log")
    private_key.chmod(0o600)
    run(["openssl", "rsa", "-in", str(private_key), "-pubout", "-out", str(public_key)],
        output / "public-key.log")

    report = {
        "root_image": str(args.root_image.resolve()),
        "layout": str(args.layout.resolve()),
        "volumes": str(args.volumes.resolve()),
        "private_key": str(private_key),
        "public_key": str(public_key),
        "generation": 1,
        "status": "preparing",
        "policies": [],
    }
    report_path = output / "sources.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")

    for name, component in (("OS", "os"), ("SystemOS", "systemos")):
        window = windows[name]
        raw = output / (component + ".raw")
        policy = output / component
        copy_window(args.root_image, window["offset"], window["size"], raw)
        print(name + ": copied " + str(window["size"]) + " bytes", flush=True)

        volume_uuid = volumes[component]["VolumeUUID"]
        run([
            sys.executable, str(args.manifest_tool.resolve()),
            "--image", str(raw),
            "--private-key", str(private_key),
            "--public-key", str(public_key),
            "--volume-uuid", volume_uuid,
            "--generation", "1",
            "--output", str(policy),
        ], output / (component + "-policy.log"))
        report["policies"].append({
            "component": name,
            "source_offset": window["offset"],
            "image_bytes": window["size"],
            "raw_image": str(raw),
            "volume_uuid": volume_uuid,
            "manifest": str(policy / "manifest.bin"),
            "public_anchor": str(policy / "trusted-key.bin"),
            "status": "complete",
        })
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(name + ": signed policy complete", flush=True)

    report["status"] = "complete"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(report_path)


if __name__ == "__main__":
    main()
