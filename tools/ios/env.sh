# Sourced by tools/ios/*.sh: reads signing.local.env (or $HENGE_SIGNING_ENV) and fills in the defaults.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENV_FILE="${HENGE_SIGNING_ENV:-$ROOT/signing.local.env}"
if [ -f "$ENV_FILE" ]; then set -a; . "$ENV_FILE"; set +a; fi
BASE="$(cd "$(dirname "$ENV_FILE")" 2>/dev/null && pwd || echo "$ROOT")"   # relative paths: from the env file's folder
abspath() { case "$1" in ""|/*) printf '%s' "$1" ;; *) printf '%s' "$BASE/$1" ;; esac; }
HENGE_WORK="$(abspath "${HENGE_WORK:-build/ios}")"
HENGE_TOOLS_DIR="$(abspath "${HENGE_TOOLS_DIR:-.signing}")"
HENGE_HELPER_BUNDLE_ID="${HENGE_HELPER_BUNDLE_ID:-${HENGE_BUNDLE_ID:+$HENGE_BUNDLE_ID.JITHelper}}"
HENGE_APP_PROFILE="$(abspath "${HENGE_APP_PROFILE:-}")"
HENGE_HELPER_PROFILE="$(abspath "${HENGE_HELPER_PROFILE:-}")"
HENGE_APP_ENTITLEMENTS="$(abspath "${HENGE_APP_ENTITLEMENTS:-}")"
HENGE_HELPER_ENTITLEMENTS="$(abspath "${HENGE_HELPER_ENTITLEMENTS:-}")"
HENGE_BASE_ENTITLEMENTS="$(abspath "${HENGE_BASE_ENTITLEMENTS:-}")"; HENGE_BASE_ENTITLEMENTS="${HENGE_BASE_ENTITLEMENTS:-$ROOT/ios/Henge/Henge.entitlements}"
need() { for v in "$@"; do [ -n "${!v:-}" ] || { echo "$v is not set: put it in $ENV_FILE (see signing.local.env.example)" >&2; exit 1; }; done; }
