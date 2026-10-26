#!/usr/bin/env python3
"""Build the device settings application for the existing boot addon packager."""

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import zlib


DEVICE = Path(__file__).resolve().parents[1]


def write_icon(path, size):
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            red, green, blue = 24, 62, 102
            for center_y, knob_x in ((0.36, 0.64), (0.64, 0.36)):
                local_x = x / size
                local_y = y / size
                closest_x = min(max(local_x, 0.32), 0.68)
                distance = (local_x - closest_x) ** 2 + (local_y - center_y) ** 2
                if distance < 0.105 ** 2:
                    red, green, blue = 65, 155, 200

                knob_distance = (local_x - knob_x) ** 2 + (local_y - center_y) ** 2
                if knob_distance < 0.077 ** 2:
                    red, green, blue = 245, 249, 252

            rows.extend((red, green, blue))

    def chunk(name, data):
        checksum = zlib.crc32(name + data)

        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", checksum)

    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = DEVICE / "package/T733Tools"
    application = args.output.resolve() / "T733Tools.app"
    application.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / "Info.plist", application / "Info.plist")
    shutil.copyfile(source / "entitlements.plist", application / "Entitlements.plist")
    write_icon(application / "AppIcon76x76@2x.png", 152)
    write_icon(application / "AppIcon83.5x83.5@2x.png", 167)

    sdk = subprocess.check_output(
        ["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    subprocess.run([
        "xcrun", "clang", "-target", "arm64-apple-ios17.0", "-isysroot", sdk,
        "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fobjc-arc", "-fblocks",
        "-fstack-protector-strong", "-Wno-deprecated-declarations",
        source / "T733Tools.m", source / "T733ToolsSettings.m",
        source / "T733ToolsPairing.m",
        "-framework", "UIKit", "-framework", "Foundation",
        "-framework", "CoreFoundation", "-o", application / "T733Tools",
    ], check=True)
    subprocess.run([
        "codesign", "--force", "--sign", "-", "--pagesize", "4096",
        "--identifier", "org.unixnu.t733.tools", "--entitlements",
        source / "entitlements.plist", application,
    ], check=True)

    print(f"T733 Tools system application: {application}")


if __name__ == "__main__":
    main()
