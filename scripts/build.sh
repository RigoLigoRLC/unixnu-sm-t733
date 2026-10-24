#!/bin/bash
set -euo pipefail

device_root=$(cd "$(dirname "$0")/.." && pwd)
exec "${PYTHON:-python3}" "$device_root/scripts/build.py" "$@"
