#!/bin/bash
set -euo pipefail
service_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
app="$service_dir/dist/T733 MSC.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$service_dir/build-cache"
/usr/bin/swiftc -module-cache-path "$service_dir/build-cache" -O -framework AppKit \
    "$service_dir/MenuApp.swift" -o "$app/Contents/MacOS/T733MSC"
cp "$service_dir/service.py" "$service_dir/mscctl" "$service_dir/Start Service.command" "$app/Contents/Resources/"
cp "$service_dir/Info.plist" "$app/Contents/Info.plist"
chmod +x "$service_dir/mscctl" "$service_dir/Start Service.command" \
    "$app/Contents/Resources/mscctl" "$app/Contents/Resources/Start Service.command"
/usr/bin/codesign --force --sign - "$app"
printf 'Built %s\n' "$app"
