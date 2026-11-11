#!/usr/bin/env bash
# Makes a self-signed code-signing identity for the Mac helper tools (henge-agent and "Install Henge Tools"),
# in a keychain file of its own, so no Apple account (and no one's name) signs them.
# macOS remembers the helper's Accessibility permission by its signer: with the same identity on every build,
# a new build keeps the permission. Signed ad hoc instead (the default), each build has to be allowed again.
# Usage: tools/ios/make-tools-identity.sh [DIR]   (default: HENGE_TOOLS_DIR from signing.local.env, else .signing/)
# Writes DIR/henge-tools.keychain-db and DIR/henge-tools.password. Keep both: a new identity means allowing
# Accessibility once more on every Mac. Never commit them (.signing/ is in .gitignore).
set -euo pipefail
. "$(dirname "$0")/env.sh"   # HENGE_TOOLS_DIR, as build.sh reads it
DIR="${1:-$HENGE_TOOLS_DIR}"
NAME="Henge Tools (self-signed)"
if [ -f "$DIR/henge-tools.keychain-db" ]; then echo "already made: $DIR/henge-tools.keychain-db"; exit 0; fi
mkdir -p "$DIR"; chmod 700 "$DIR"; DIR="$(cd "$DIR" && pwd)"   # security puts relative paths in ~/Library/Keychains
KC="$DIR/henge-tools.keychain-db"
PW="$DIR/henge-tools.password"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
head -c 24 /dev/urandom | base64 | tr -d '/+=' > "$PW"; chmod 600 "$PW"
cat > "$T/cert.cnf" <<CNF
[req]
distinguished_name = dn
prompt = no
x509_extensions = ext
[dn]
CN = $NAME
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
CNF
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 7300 -config "$T/cert.cnf" \
    -keyout "$T/key.pem" -out "$T/cert.pem" 2>/dev/null
/usr/bin/openssl pkcs12 -export -inkey "$T/key.pem" -in "$T/cert.pem" -name "$NAME" \
    -out "$T/id.p12" -passout pass:henge
security create-keychain -p "$(cat "$PW")" "$KC"
security set-keychain-settings "$KC"                      # no auto-lock timeout
security unlock-keychain -p "$(cat "$PW")" "$KC"
security import "$T/id.p12" -k "$KC" -P henge -T /usr/bin/codesign >/dev/null
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$(cat "$PW")" "$KC" >/dev/null
echo "made: $KC"
security find-identity -p codesigning "$KC" | sed -n 's/^ *1) //p'
