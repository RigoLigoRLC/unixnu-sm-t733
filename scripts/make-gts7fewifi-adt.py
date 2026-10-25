#!/usr/bin/env python3
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The SM-T733 ADT generator encodes XNU platform properties and SM7325 resources.

CPU affinities, GIC registers, the timer frequency, and PSCI come from yupik.dtsi.
The caller supplies the DRAM address span and any requested contiguous allocation
from the device memory records. DarwinBoot allocates that memory, supplies the framebuffer,
and replaces memory-map placeholders with the loaded sidecar addresses.
"""

import argparse
import os
from pathlib import Path
import struct
import tomllib
import uuid
import zlib


ROOT = Path(__file__).resolve().parents[1]
DRAM_BASE = 0x80000000
GIC_DIST = 0x17A00000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x17A60000
GIC_REDIST_SIZE = 0x100000
GIC_REDIST_STRIDE = 0x20000
TIMEBASE = 19200000
CPU_AFFINITIES = tuple(range(0, 0x800, 0x100))


def node(name, properties=(), children=()):
    """Encode a node header, padded properties, and its already encoded children."""
    properties = [("name", name.encode("ascii") + b"\0"), *properties]
    result = bytearray(struct.pack("<II", len(properties), len(children)))

    for key, data in properties:
        result.extend(struct.pack("<32sI", key.encode("ascii"), len(data)))
        result.extend(data)
        result.extend(bytes(-len(data) % 4))

    for child in children:
        result.extend(child)

    return bytes(result)


def empty_nvram_proxy():
    """Create the volatile CHRP bank read by IODTNVRAM from /chosen."""
    def header(name, size):
        raw = bytearray(struct.pack("<BBH12s", 0x70, 0, size // 16, name))
        checksum = sum(raw)

        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)

        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b"nvram", 32)
    image[32:48] = header(b"common", 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b"free", len(image) - free_at)
    struct.pack_into("<I", image, 16, zlib.adler32(image[20:]))

    return [
        ("nvram-bank-size", struct.pack("<I", len(image))),
        ("nvram-bank-count", struct.pack("<I", 1)),
        ("nvram-current-bank", struct.pack("<I", 0)),
        ("nvram-proxy-data", bytes(image)),
    ]


def integer(value):
    if isinstance(value, int):
        return value

    return int(value, 0)


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hardware-input", type=Path)
    parser.add_argument("--dram-size", default="required")
    parser.add_argument("--owned-memory-size",
                        help="Exact contiguous allocation in bytes, or auto for the largest available UEFI range")
    parser.add_argument("--cpu-regs", default="0")
    parser.add_argument("--psci-method", choices=("smc",), default="smc")
    parser.add_argument("--unique-chip-id")
    parser.add_argument("--chip-id")
    parser.add_argument("--root-match-volume-uuid", type=uuid.UUID)
    disks = parser.add_mutually_exclusive_group()
    disks.add_argument("--data-ramdisk", action="store_true")
    disks.add_argument("--packed-data-ramdisk", action="store_true")
    parser.add_argument("--persistent-data", action="store_true")
    parser.add_argument("--ipad-identity", action="store_true")
    parser.add_argument("--trust-cache", action="store_true")
    parser.add_argument("--gpu-addon", action="store_true")
    parser.add_argument("--text-fixup", action="append", default=[],
                        help="Signed-page replacement: filename,offset,original,replacement in hex")
    args = parser.parse_args()

    hardware = {}
    hardware_path = args.hardware_input
    if hardware_path is None and os.environ.get("UNIXNU_HARDWARE_INPUT"):
        hardware_path = Path(os.environ["UNIXNU_HARDWARE_INPUT"])

    if hardware_path is None and (ROOT / "local.toml").is_file():
        hardware_path = ROOT / "local.toml"

    if hardware_path is not None:
        with hardware_path.open("rb") as source:
            hardware = tomllib.load(source).get("hardware", {})

    if args.dram_size == "required":
        args.dram_size = os.environ.get("UNIXNU_DRAM_SIZE") or hardware.get("dram_size")

    if args.dram_size is None:
        parser.error("DRAM size is required from the device memory map; set --dram-size, UNIXNU_DRAM_SIZE, or hardware.dram_size")

    args.owned_memory_size = args.owned_memory_size or hardware.get("owned_memory_size")
    args.unique_chip_id = args.unique_chip_id or os.environ.get("UNIXNU_UNIQUE_CHIP_ID") or hardware.get("unique_chip_id")
    args.chip_id = args.chip_id or hardware.get("chip_id")

    try:
        args.dram_size = integer(args.dram_size)
        args.cpu_regs = [integer(value) for value in args.cpu_regs.split(",")]

        if args.owned_memory_size not in (None, "auto"):
            args.owned_memory_size = integer(args.owned_memory_size)

        if args.unique_chip_id is not None and args.chip_id is not None:
            args.unique_chip_id = int(str(args.unique_chip_id), 16)
            args.chip_id = int(str(args.chip_id), 16)
    except ValueError as error:
        parser.error(str(error))

    if not 0 < args.dram_size < (1 << 64) - DRAM_BASE:
        parser.error("DRAM size must fit the 64-bit physical address range")

    if args.owned_memory_size == "auto":
        if args.dram_size < 0x40000000:
            parser.error("Automatic memory allocation requires at least 1 GiB of DRAM")
    elif args.owned_memory_size is not None:
        if not 0x10000000 <= args.owned_memory_size <= min(0x200000000, args.dram_size):
            parser.error("Owned memory must be 256 MiB to 8 GiB and fit DRAM")

        if args.owned_memory_size % 0x200000:
            parser.error("Owned memory must be aligned to 2 MiB")

    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error("unique-chip-id and chip-id must be supplied together")

    if args.unique_chip_id is not None:
        if not 0 < args.unique_chip_id < 1 << 64 or not 0 < args.chip_id < 1 << 32:
            parser.error("Identity seeds must be nonzero and fit 64 and 32 bits")

    if not args.cpu_regs or args.cpu_regs[0] != 0:
        parser.error("The boot CPU affinity must be first and equal to 0")

    if len(set(args.cpu_regs)) != len(args.cpu_regs) or any(value not in CPU_AFFINITIES for value in args.cpu_regs):
        parser.error("CPU affinities must be distinct entries from 0,0x100,...,0x700")

    if args.persistent_data and not (args.data_ramdisk or args.packed_data_ramdisk):
        parser.error("persistent-data requires a Data image")

    return args


def make_tree(args):
    chosen = [
        ("random-seed", os.urandom(64)),
        ("osenvironment", b"main\0"),
        ("dram-base", struct.pack("<Q", DRAM_BASE)),
        ("dram-size", struct.pack("<Q", args.dram_size)),
        ("crypto-hash-method", b"sha2-384\0"),
        ("amfi-only-platform-code", struct.pack("<I", 1)),
        *empty_nvram_proxy(),
    ]

    if args.owned_memory_size == "auto":
        maximum = min(args.dram_size, 0x200000000) & ~0x1fffff
        chosen.append(("darwinboot-memory-range", struct.pack("<QQ", 0x40000000, maximum)))
    elif args.owned_memory_size is not None:
        chosen.append(("darwinboot-memory-size", struct.pack("<Q", args.owned_memory_size)))

    if args.text_fixup:
        chosen.append(("text-fixups", b"".join(
            entry.encode("ascii") + b"\0" for entry in args.text_fixup
        )))

    if args.unique_chip_id is not None:
        chosen.extend([
            ("unique-chip-id", struct.pack("<Q", args.unique_chip_id)),
            ("chip-id", struct.pack("<I", args.chip_id)),
        ])

    if args.root_match_volume_uuid is not None:
        matching = (
            "<dict><key>IOProviderClass</key><string>IOMedia</string>"
            "<key>IOPropertyMatch</key><dict><key>UUID</key><string>"
            + str(args.root_match_volume_uuid).upper()
            + "</string></dict></dict>\0"
        )
        chosen.append(("root-matching", matching.encode("ascii")))

    memory_map = []
    chosen.append(("native-usb-debug-config", struct.pack("<16I",
        1, 0x0a600000, 0x000e000, 0x0a6f8800, 0x088e3000, 0x088e2000,
        0x0010f000, 0x1000, 0x15000000, 0x100000, 0xe0, 0x80, 3, 0, 0, 0)))
    if args.ipad_identity:
        chosen.extend([
            ("certificate-production-status", struct.pack("<I", 0)),
            ("effective-production-status-ap", struct.pack("<I", 0)),
            ("certificate-security-mode", struct.pack("<I", 1)),
            ("effective-security-mode-sep", struct.pack("<I", 1)),
            ("display-scale", struct.pack("<I", 2)),
        ])
    if args.data_ramdisk or args.packed_data_ramdisk:
        name = "PackedRAMDisk" if args.packed_data_ramdisk else "RAMDisk"
        memory_map.append((name, bytes(16)))

        if not args.persistent_data:
            chosen.append(("ephemeral-storage", struct.pack("<I", 1)))

    if args.trust_cache:
        memory_map.append(("TrustCache", bytes(16)))

    if args.gpu_addon:
        memory_map.append(("GpuAddon", bytes(16)))

    cpus = []
    for index, affinity in enumerate(args.cpu_regs):
        cpus.append(node(f"cpu{index}", [
            ("state", b"running\0" if index == 0 else b"waiting\0"),
            ("reg", struct.pack("<I", affinity)),
            ("cluster-type", struct.pack("<I", ord("E") if index < 4 else ord("P"))),
            ("die-cluster-id", struct.pack("<I", 0 if index < 4 else (1 if index < 7 else 2))),
            ("cluster-core-id", struct.pack("<I", index if index < 4 else (index - 4 if index < 7 else 0))),
            ("device_type", b"cpu\0"),
            ("compatible", b"qcom,kryo\0"),
            ("timebase-frequency", struct.pack("<I", TIMEBASE)),
        ]))

    children = [
        node("chosen", chosen, [node("memory-map", memory_map), node("manifest-properties")]),
        node("cpus", children=cpus),
        node("psci", [("compatible", b"arm,psci-1.0\0"), ("method", args.psci_method.encode("ascii") + b"\0")]),
        node("defaults", [
            ("sleep-disabled", struct.pack("<I", 1)),
            # PE_get_default supplies XNU's small vnode budget before its build-time cap.
            ("kern.maxvnodes", struct.pack("<I", 10240)),
        ]),
        node("options"),
        node("product", [
            ("product-name", b"iPad Pro (12.9-inch) (2nd generation)\0" if args.ipad_identity else b"Samsung Galaxy Tab S7 FE Wi-Fi\0"),
            ("model", b"SM-T733\0"),
            *([
                ("product-description", b"iPad Pro (12.9-inch) (2nd generation)\0"),
                ("artwork-device-idiom", b"pad\0"),
                ("artwork-device-subtype", struct.pack("<I", 2732)),
                ("artwork-scale-factor", struct.pack("<I", 2)),
                ("compatible-device-fallback", b"iPad6,7\0"),
                ("thin-bezel", struct.pack("<I", 1)),
            ] if args.ipad_identity else []),
        ]),
        node("arm-io", [
            ("device_type", b"arm-io\0"),
            ("#address-cells", struct.pack("<I", 2)),
            ("#size-cells", struct.pack("<I", 2)),
            ("ranges", struct.pack("<QQQ", 0, 0, 0x20000000)),
        ], [
            node("intc", [
                ("compatible", b"arm,gic-v3\0"),
                ("reg", struct.pack("<4Q", GIC_DIST, GIC_DIST_SIZE, GIC_REDIST, GIC_REDIST_SIZE)),
                ("redistributor-stride", struct.pack("<Q", GIC_REDIST_STRIDE)),
            ]),
            node("ufshc", [
                ("compatible", b"qcom,sm7325-ufshc\0"),
                ("reg", struct.pack("<QQ", 0x01D84000, 0x3000)),
                ("native-ufs-smmu-reg", struct.pack("<QQ", 0x15000000, 0x100000)),
                ("native-ufs-stream-id", struct.pack("<I", 0x80)),
                ("native-ufs-interrupt", struct.pack("<I", 297)),
            ]),
        ]),
    ]

    if args.ipad_identity:
        children.append(node("buttons", [
            ("compatible", b"buttons\0"),
            ("device_type", b"buttons\0"),
            ("button-names", b"hold\0voldown\0volup\0"),
            ("home-button-type", struct.pack("<I", 2)),
        ]))

    return node("device-tree", [
        ("model", b"iPad7,1\0" if args.ipad_identity else b"SM-T733\0"),
        *([("target-type", b"J120\0")] if args.ipad_identity else []),
        ("compatible", b"samsung,gts7fewifi\0qcom,sm7325\0qcom,yupik\0"),
        ("#address-cells", struct.pack("<I", 2)),
        ("#size-cells", struct.pack("<I", 2)),
    ], children)


def main():
    args = parse_arguments()
    tree = make_tree(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f"SM-T733 ADT written: {args.output} ({len(tree)} bytes)")


if __name__ == "__main__":
    main()
