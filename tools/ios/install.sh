#!/usr/bin/env bash
# Installs the signed app (from sign.sh) on HENGE_DEVICE over USB or the network.
# Installing replaces the running app: a Mac that is running in Henge at that moment is stopped.
# Usage: tools/ios/install.sh
set -euo pipefail
. "$(dirname "$0")/env.sh"
need HENGE_DEVICE
OUT="$HENGE_WORK/signed/Henge.app"
[ -d "$OUT" ] || { echo "no $OUT: run tools/ios/sign.sh first" >&2; exit 1; }
xcrun devicectl device install app --device "$HENGE_DEVICE" "$OUT"
