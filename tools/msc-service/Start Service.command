#!/bin/bash
set -euo pipefail
service_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
if [[ "$service_dir" == */Contents/Resources ]]; then
    /usr/bin/open "${service_dir%/Contents/Resources}"
else
    /usr/bin/open "$service_dir/dist/T733 MSC.app"
fi
printf '%s\n' 'Starting T733 MSC for your login user.' 'The service will stay in the background after startup.' 'Use Stop service in the menu or mscctl shutdown to stop.'
exec /usr/bin/sudo /usr/bin/python3 -I "$service_dir/service.py" serve --detach
