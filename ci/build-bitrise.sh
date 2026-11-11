#!/usr/bin/env bash
# Build Henge's unsigned, ad-hoc-signed IPA on a Bitrise macOS/Xcode stack (ci/build-ipa.sh does the work).
# SideStore or AltStore re-signs the IPA when installing it on the iPad.
set -euo pipefail

HENGE_ROOT="${BITRISE_SOURCE_DIR:?Bitrise must set BITRISE_SOURCE_DIR}"
bash "$HENGE_ROOT/ci/build-ipa.sh" "$(dirname "$HENGE_ROOT")"

mkdir -p "${BITRISE_DEPLOY_DIR:?Bitrise must set BITRISE_DEPLOY_DIR}"
# The IPA is intentionally ad-hoc-signed, without embedded.mobileprovision.
# Bitrise's deploy step treats an .ipa as an installable artifact and rejects it
# without a provisioning profile. Wrap it so it is uploaded as a generic file;
# users extract the IPA and let SideStore or AltStore re-sign it.
( cd "$HOME/Desktop" && zip -0 -q "$BITRISE_DEPLOY_DIR/Henge-color-fix-unsigned.zip" Henge.ipa )
