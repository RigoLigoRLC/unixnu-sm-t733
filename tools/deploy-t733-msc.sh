#!/bin/bash
set -euo pipefail

workspace_root=$(cd "$(dirname "$0")/.." && pwd)
artifact_tag=${1:-}
backup_previous=0

if [ "$#" -eq 2 ] && [ "$2" = "--backup" ]; then
    backup_previous=1
elif [ "$#" -ne 1 ]; then
    printf 'Usage: %s BUILD_TAG [--backup]\n' "$0" >&2
    exit 2
fi

if [[ ! "$artifact_tag" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
    printf 'Usage: %s BUILD_TAG [--backup]\n' "$0" >&2
    exit 2
fi

release="$workspace_root/build/releases/$artifact_tag"
payload="$release/payload.img"
stages="$release/stages.tsv"
client="$workspace_root/tools/msc-service/mscctl"
usb_dir="$workspace_root/logs/t733-usb-live"
session="$(date +%Y%m%d-%H%M%S)-$artifact_tag"
log_dir="$workspace_root/logs/msc-$session"
backup_dir="$workspace_root/backups/msc-$session"

if [ ! -f "$payload" ] || [ ! -f "$stages" ] ||
    ! awk -F '\t' '$1 == "payload" && $2 == 0 { completed = 1 } END { exit !completed }' "$stages"; then
    printf 'The requested FAT build has not completed: %s\n' "$artifact_tag" >&2
    exit 1
fi

mkdir -p "$log_dir"
"$client" status | tee "$log_dir/service.jsonl"
"$client" list | tee "$log_dir/device.jsonl"

"${PYTHON:-python3}" "$workspace_root/tools/t733-usb-console.py" \
    --output "$usb_dir" --background | tee "$log_dir/collector-start.txt"

# The service selects the connected tablet again for each operation.
if [ "$backup_previous" -eq 1 ]; then
    mkdir -p "$backup_dir"
    "$client" read-fat "$backup_dir/payload-before.img" | tee "$log_dir/read-fat.jsonl"
fi
"$client" write-fat "$payload" | tee "$log_dir/write-fat.jsonl"

printf 'FAT deployment completed: %s\n' "$payload"
if [ "$backup_previous" -eq 1 ]; then
    printf 'Previous FAT: %s/payload-before.img\n' "$backup_dir"
fi
printf 'Deployment log: %s\n' "$log_dir"
printf 'USB capture directory: %s\n' "$usb_dir"
printf 'The write and sync have completed. Fully reboot the tablet before opening UniXNU.\n'
