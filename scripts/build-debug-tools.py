#!/usr/bin/env python3
"""Build the USB address helper and package its launchd job for the boot addon.

The helper runs a development-signed copy of the image's debugserver. This
script preserves its executable code and original entitlements, adds the
task-port entitlement, and signs the helper and copied debugger. Their signing
identities form a separate trust-cache module alongside the supplied Apple
modules. UniXNU's payload command carries the addon and trust bundle; the
kernel installs the files before launchd starts.
The wallpaper directory supplies the original J120 CA document and converted
PNG images for the kernel's existing wallpaper mount.
"""

import argparse
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zlib


DEVICE = Path(__file__).resolve().parents[1]

# AMFI 21H461 category 5 accepts platform code on Data and requires launchd
# or the system mbfloagent as its parent (self DER 0xab91c1, parent 0xab9955).
HELPER_CONSTRAINT_CATEGORY = 5


def run(command):
    subprocess.run(list(map(str, command)), check=True)


def padded(data):
    return data + bytes(-len(data) % 8)


def trust_entry(binary, category):
    signature = subprocess.run(
        ["codesign", "--display", "--verbose=4", str(binary)],
        capture_output=True, text=True, check=True).stderr
    identity = re.search(r"^CDHash=([0-9a-f]{40})$", signature, re.M)
    if identity is None:
        raise SystemExit(f"codesign did not report CodeDirectory identity: {binary}")
    return bytes.fromhex(identity[1]) + bytes([2, 0, category, 0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--trust-cache", type=Path, required=True)
    parser.add_argument("--debugserver", type=Path, required=True,
                        help="Unmodified debugserver from the target OS image")
    parser.add_argument("--wallpaper", type=Path, required=True,
                        help="J120 overlay produced by make-wallpaper-overlay.py")
    args = parser.parse_args()
    if not args.debugserver.is_file():
        raise SystemExit(f"Missing original target debugserver input: {args.debugserver}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = subprocess.check_output(
        ["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    binary = output / "t733-debug-network"
    run(["xcrun", "clang", "-target", "arm64-apple-ios17.0", "-isysroot", sdk,
         "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fstack-protector-strong",
         DEVICE / "package/T733DebugNetwork.c", "-framework", "CoreFoundation",
         "-o", binary])
    entitlements = output / "entitlements.plist"
    entitlements.write_bytes(plistlib.dumps({
        "com.apple.private.security.no-container": True,
        "com.apple.private.lockdown.finegrained-set": [
            "com.apple.mobile.wireless_lockdown/EnableWifiConnections",
        ],
        "com.apple.private.lockdown.finegrained-get": [
            "com.apple.mobile.wireless_lockdown/EnableWifiConnections",
        ],
    }))
    run(["codesign", "--force", "--sign", "-", "--pagesize", "4096",
         "--identifier", "org.unixnu.t733.debug-network",
         "--entitlements", entitlements, binary])
    debugger = output / "t733-debugserver"
    debugger_entitlements = output / "debugserver-entitlements.plist"
    original_entitlements = subprocess.check_output(
        ["codesign", "--display", "--entitlements", ":-", str(args.debugserver)],
        stderr=subprocess.PIPE)
    debugger_permissions = plistlib.loads(original_entitlements)
    debugger_permissions["task_for_pid-allow"] = True
    debugger_entitlements.write_bytes(plistlib.dumps(debugger_permissions))
    shutil.copyfile(args.debugserver, debugger)
    debugger.chmod(0o755)
    run(["codesign", "--force", "--sign", "-", "--pagesize", "4096",
         "--identifier", "org.unixnu.t733.debugserver",
         "--entitlements", debugger_entitlements, debugger])

    gpu_output = output / "gpu"
    work = Path(os.environ["T733_RESOLVED_TREE"])
    core = Path(os.environ["UNIXNU_BASE"])
    run([sys.executable, DEVICE / "tools/gpu/build-userspace.py",
         "--output", gpu_output, "--abi-directory", work / "drivers/Adreno6xx",
         "--pm4-directory", work / "kits/AdrenoPM4", "--core", core,
         "--turnip-library", core / "build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib"])
    gpu_binary = gpu_output / "t733-adreno"
    turnip_binary = gpu_output / "t733-turnip"
    turnip_library = gpu_output / "libvulkan_freedreno.dylib"

    original = args.trust_cache.read_bytes()
    count = struct.unpack_from("<I", original)[0]
    offsets = struct.unpack_from(f"<{count}I", original, 4)
    modules = [original[start:end] for start, end in
               zip(offsets, (*offsets[1:], len(original)))]
    entries = sorted([
        trust_entry(binary, HELPER_CONSTRAINT_CATEGORY),
        trust_entry(debugger, HELPER_CONSTRAINT_CATEGORY),
        trust_entry(gpu_binary, 0),
        trust_entry(turnip_binary, 0),
        trust_entry(turnip_library, 0),
    ], key=lambda entry: entry[:20])
    owner = struct.pack("<I16sI", 2, uuid.uuid4().bytes, len(entries))
    owner += b"".join(entries)
    modules.append(owner)
    cursor = 4 + 4 * len(modules)
    starts = []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    bundle = struct.pack(f"<{len(modules) + 1}I", len(modules), *starts)
    (output / "trust.bundle").write_bytes(bundle + b"".join(modules))

    # The cached system job opens /usr/bin/IOMFB_FDR_Loader. The boot addon
    # mounts its bin directory over /usr/bin before launchd starts.
    job = plistlib.dumps({
        "Label": "com.apple.iomfb_fdr_loader",
        "ProgramArguments": ["/private/var/ngpu/bin/t733-debug-network"],
        "RunAtLoad": True,
        "KeepAlive": True,
        "ThrottleInterval": 5,
        "UserName": "root",
        "LimitLoadToSessionType": "System",
        "ProcessType": "Interactive",
        "StandardOutPath": "/dev/console",
        "StandardErrorPath": "/dev/console",
    })
    files = [
        ("bin/t733-debug-network", 0o755, binary.read_bytes()),
        ("bin/IOMFB_FDR_Loader", 0o755, binary.read_bytes()),
        ("bin/t733-debugserver", 0o755, debugger.read_bytes()),
        ("bin/t733-adreno", 0o755, gpu_binary.read_bytes()),
        ("bin/t733-turnip", 0o755, turnip_binary.read_bytes()),
        ("bin/libvulkan_freedreno.dylib", 0o755, turnip_library.read_bytes()),
        ("daemons/com.apple.iomfb_fdr_loader.plist", 0o644, job),
        ("preferences/backboardd-brightness.plist", 0o644,
         (DEVICE / "package/backboardd-brightness.plist").read_bytes()),
    ]
    for name in ("index.xml", "assetManifest.caml", "main.caml", "J120.png", "J120-Dark.png"):
        files.append(("wallpaper/" + name, 0o644, (args.wallpaper / name).read_bytes()))

    body = bytearray()
    for name, mode, data in files:
        encoded = name.encode("ascii") + b"\0"
        packed = zlib.compress(data, 9)
        body.extend(struct.pack("<IIQQ", len(encoded), mode, len(data), len(packed)))
        body.extend(padded(encoded))
        body.extend(padded(packed))
    header = b"NGPUADD2" + struct.pack("<IIQ", len(files), 0, 24 + len(body))
    (output / "debug.addon").write_bytes(header + body)
    print(f"USB debug helper and addon: {output}; "
          f"helper trust-cache constraint category={HELPER_CONSTRAINT_CATEGORY}")
    print(f"Development debugserver from {args.debugserver}; original entitlements "
          "plus task_for_pid-allow; separate category 5 trust entry")
    print(f"J120 wallpaper document and PNG images from {args.wallpaper}")
    print("backboardd managed preference: SynchronousBrightnessTransactions=true")


if __name__ == "__main__":
    main()
