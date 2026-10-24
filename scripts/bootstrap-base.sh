#!/bin/bash
set -euo pipefail

device_root=$(cd "$(dirname "$0")/.." && pwd)
destination=${1:?Usage: bootstrap-base.sh NEW_UNIXNU_DIRECTORY}
revision=$(cat "$device_root/support/UniXNU.rev")

if [ -e "$destination" ]; then
    printf 'Destination already exists: %s\n' "$destination" >&2
    exit 1
fi

git clone --no-checkout https://github.com/RigoLigoRLC/unixnu-core.git "$destination"
git -C "$destination" checkout --detach "$revision"
git -C "$destination" submodule update --init --recursive
printf 'UniXNU base prepared: %s\n' "$destination"
