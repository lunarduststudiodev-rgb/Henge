#!/usr/bin/env bash
# Signs the app build.sh made ($HENGE_WORK/Henge.app) with your certificate and provisioning profiles
# (signing.local.env), then installs it with install.sh.
# Usage: tools/ios/sign.sh [--no-install] [APP]   (APP: another unsigned Henge.app to sign)
set -euo pipefail
. "$(dirname "$0")/env.sh"
NO_INSTALL=0; [ "${1:-}" = "--no-install" ] && { NO_INSTALL=1; shift; }
APP_SRC="${1:-$HENGE_WORK/Henge.app}"
need HENGE_TEAM_ID HENGE_SIGN_IDENTITY HENGE_BUNDLE_ID HENGE_APP_PROFILE HENGE_HELPER_PROFILE
[ -d "$APP_SRC" ] || { echo "no $APP_SRC: run tools/ios/build.sh first" >&2; exit 1; }
OUT="$HENGE_WORK/signed/Henge.app"
rm -rf "$HENGE_WORK/signed"; mkdir -p "$HENGE_WORK/signed"
ditto "$APP_SRC" "$OUT"
EX="$OUT/PlugIns/HengeJITHelper.appex"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

# Entitlements: the given files, or Henge's (ios/Henge/Henge.entitlements) cut down to what the profile allows,
# plus the team's identifiers.
ents() { # PROFILE BUNDLE BASE OUT
    security cms -D -i "$1" > "$T/profile.plist"
    /usr/bin/python3 -I - "$T/profile.plist" "$HENGE_TEAM_ID" "$2" "$3" "$4" <<'PY'
import plistlib, sys
prof, team, bundle, base, out = sys.argv[1:]
allowed = plistlib.load(open(prof, 'rb'))['Entitlements']
want = plistlib.load(open(base, 'rb')) if base else {}
e = {k: v for k, v in want.items() if k in allowed}
missing = sorted(set(want) - set(e))
if missing: print('  the profile does not allow, so left out:', ', '.join(missing))
e['application-identifier'] = f'{team}.{bundle}'
e['com.apple.developer.team-identifier'] = team
e['keychain-access-groups'] = [f'{team}.{bundle}']
if 'get-task-allow' in allowed: e['get-task-allow'] = allowed['get-task-allow']
plistlib.dump(e, open(out, 'wb'))
PY
}
APP_ENTS="$HENGE_APP_ENTITLEMENTS"; HELPER_ENTS="$HENGE_HELPER_ENTITLEMENTS"
[ -n "$APP_ENTS" ] || { APP_ENTS="$T/app.plist"; ents "$HENGE_APP_PROFILE" "$HENGE_BUNDLE_ID" "$HENGE_BASE_ENTITLEMENTS" "$APP_ENTS"; }
[ -n "$HELPER_ENTS" ] || { HELPER_ENTS="$T/helper.plist"; ents "$HENGE_HELPER_PROFILE" "$HENGE_HELPER_BUNDLE_ID" "" "$HELPER_ENTS"; }

plutil -replace CFBundleIdentifier -string "$HENGE_BUNDLE_ID" "$OUT/Info.plist"
plutil -replace CFBundleIdentifier -string "$HENGE_HELPER_BUNDLE_ID" "$EX/Info.plist"
cp "$HENGE_APP_PROFILE" "$OUT/embedded.mobileprovision"
cp "$HENGE_HELPER_PROFILE" "$EX/embedded.mobileprovision"

sign() { codesign -f -s "$HENGE_SIGN_IDENTITY" --generate-entitlement-der --timestamp=none "$@"; }
for fw in "$OUT"/Frameworks/*.framework; do [ -d "$fw" ] && sign "$fw"; done
sign --entitlements "$HELPER_ENTS" "$EX"
sign --entitlements "$APP_ENTS" "$OUT"
codesign --verify --deep --strict "$OUT"
echo "memory entitlements:"; codesign -d --entitlements - --xml "$OUT" 2>/dev/null | plutil -p - | grep -E 'memory|virtual' || echo "  none"
echo "signed: $OUT"
[ "$NO_INSTALL" = 1 ] && exit 0
"$(dirname "$0")/install.sh"
