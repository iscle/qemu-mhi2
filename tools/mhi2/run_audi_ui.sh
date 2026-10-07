#!/usr/bin/env bash
# Run the prepared Audi A3 media with its center-console controller.
set -euo pipefail
exec bash "$(dirname "$0")/run_ui.sh" --firmware audi-a3 "$@"
