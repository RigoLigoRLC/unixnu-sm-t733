#!/usr/bin/env python3
"""The vendor importer copies the board's selected files from extracted backups.

package/firmware-inputs.toml names each firmware or board configuration file.
The command reads the extraction inventory, copies those files into the board's
firmware cache, and records their source image, path, and extraction byte count.
Driver loading and firmware submission remain separate from this file import.
"""

import argparse
import json
from pathlib import Path
import shutil
import tomllib


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=ROOT.parents[1])
    parser.add_argument("--extraction", type=Path,
                        help="Directory containing the extracted partition files")
    parser.add_argument("--inventory", type=Path,
                        help="Extraction inventory; defaults to extraction/inventory.json")
    parser.add_argument("--destination", type=Path,
                        help="Override the device firmware cache destination")
    args = parser.parse_args()
    workspace = args.workspace.resolve()

    with (ROOT / "package/firmware-inputs.toml").open("rb") as source:
        inputs = tomllib.load(source)["vendor_inputs"]

    extraction = (args.extraction or workspace / inputs["extraction"]).resolve()
    if args.inventory:
        inventory_path = args.inventory.resolve()
    elif args.extraction:
        inventory_path = extraction / "inventory.json"
    else:
        inventory_path = workspace / inputs["inventory"]
    inventory = json.loads(inventory_path.read_text())
    entries = {}
    for item in inventory:
        if item.get("extracted") and item.get("type") == "file":
            entries[item["partition"] + item["path"]] = item

    destination = (args.destination or ROOT / ".cache/firmware" / inputs["cache"]).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    records = []

    for role, name in inputs["files"].items():
        item = entries[name]
        source = extraction / name
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        records.append({
            "role": role,
            "cache_path": name,
            "partition": item["partition"],
            "image": item["image"],
            "image_path": item["path"],
            "extraction_bytes": item["bytes"],
            "extracted_file": str(source),
        })

    record = {
        "inventory": str(inventory_path),
        "extraction": str(extraction),
        "files": records,
    }
    if args.extraction is None and args.inventory is None:
        record["super_backup"] = str(workspace / inputs["super_backup"])
        record["apnhlos_backup"] = str(workspace / inputs["apnhlos_backup"])
    (destination / "sources.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"Imported {len(records)} device firmware and configuration files: {destination}")


if __name__ == "__main__":
    main()
