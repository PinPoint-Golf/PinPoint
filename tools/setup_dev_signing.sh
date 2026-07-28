#!/usr/bin/env bash
#
# setup_dev_signing.sh — put the local dev code-signing identity somewhere a build
# with no desktop session can actually reach it (macOS).  Companion to
# cmake/DevCodesign.cmake, which unlocks what this creates and does the signing.
#
# ┌─ WHY THIS EXISTS ───────────────────────────────────────────────────────────┐
# │ macOS keys camera / microphone / speech / Bluetooth grants (TCC) to the      │
# │ binary's CODE SIGNATURE.  Unsigned builds have no durable identity and       │
# │ ad-hoc-signed ones get a fresh cdhash every rebuild, so a grant made in      │
# │ System Settings never applies to the next build — the app reads as           │
# │ "enabled" there while every entitled API returns Denied.  Signing with a     │
# │ stable self-signed cert gives TCC something constant to match.               │
# │                                                                              │
# │ The cert cannot live in the LOGIN keychain, which is the obvious place for   │
# │ it.  That keychain unlocks only by PROMPTING, so a build driven over SSH —   │
# │ an agent, a remote shell, CI — is in launchd session `Background` rather     │
# │ than `Aqua`, has nothing to prompt with, and codesign dies                   │
# │ `errSecInternalComponent`.  A DEDICATED keychain whose password is on disk   │
# │ unlocks with no interaction, which is why every macOS CI system builds one   │
# │ (fastlane ships it as create_keychain / setup_ci).                           │
# └──────────────────────────────────────────────────────────────────────────────┘
#
# Usage:
#   tools/setup_dev_signing.sh                 # generate a fresh identity
#   tools/setup_dev_signing.sh --p12 <file>    # adopt an existing one (see below)
#
# `--p12` matters when a signed build has ALREADY been granted permissions: TCC
# keys those grants to the certificate, so a new cert resets them and the app
# re-prompts.  Exporting the old identity and adopting it here keeps them.  The
# export itself needs a real desktop session — exporting a private key always
# demands GUI approval, and no ACL or partition list covers it — so run that part
# from Terminal.app on the machine itself:
#
#     security export -t identities -f pkcs12 -P <pass> -o /tmp/pp.p12
#
# Idempotent: re-running replaces the keychain and its contents.
#
set -euo pipefail

IDENTITY="${PINPOINT_DEV_CODESIGN_IDENTITY:-PinPoint Dev}"
KC_NAME="pinpoint-dev.keychain"
KC="$HOME/Library/Keychains/pinpoint-dev.keychain-db"
LOGIN_KC="$HOME/Library/Keychains/login.keychain-db"
PASSDIR="$HOME/.config/pinpoint"
PASSFILE="$PASSDIR/dev-keychain.pass"
P12_IN=""
P12_PASS="pinpoint"

log() { printf '  %s\n' "$*"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --p12)      P12_IN="$2"; shift 2 ;;
        --p12-pass) P12_PASS="$2"; shift 2 ;;
        -h|--help)  sed -n '2,40p' "$0"; exit 0 ;;
        *)          echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || { echo "macOS only" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ── 1. The identity ──────────────────────────────────────────────────────────
# Either adopt the .p12 handed in, or mint one.  The generated cert needs
# codeSigning as an EKU and is a self-signed ROOT; it will always list as
# CSSMERR_TP_NOT_TRUSTED, which is fine — codesign signs with an untrusted cert
# regardless and TCC keys the grant to the signature either way.  DevCodesign.cmake
# deliberately omits `security find-identity -v` for exactly this reason.
if [[ -n "$P12_IN" ]]; then
    [[ -f "$P12_IN" ]] || { echo "no such file: $P12_IN" >&2; exit 1; }
    cp "$P12_IN" "$WORK/id.p12"
    log "adopting identity from $P12_IN"
else
    cat > "$WORK/ext.cnf" <<EOF
[req]
distinguished_name = dn
prompt             = no
x509_extensions    = v3
[dn]
CN = $IDENTITY
[v3]
basicConstraints     = critical,CA:true
keyUsage             = critical,digitalSignature
extendedKeyUsage     = critical,codeSigning
subjectKeyIdentifier = hash
EOF
    openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
        -keyout "$WORK/k.pem" -out "$WORK/c.pem" -config "$WORK/ext.cnf" 2>/dev/null
    # -legacy: the Security framework cannot read OpenSSL 3's default AES-256-CBC
    # PKCS#12 encryption, and `security import` fails with an opaque error.
    openssl pkcs12 -export -legacy -out "$WORK/id.p12" -inkey "$WORK/k.pem" \
        -in "$WORK/c.pem" -name "$IDENTITY" -passout "pass:$P12_PASS" 2>/dev/null
    P12_PASS="$P12_PASS"
    log "generated a fresh '$IDENTITY' (self-signed, 10 years)"
fi

# ── 2. The keychain ──────────────────────────────────────────────────────────
security delete-keychain "$KC" 2>/dev/null || true
mkdir -p "$PASSDIR"; chmod 700 "$PASSDIR"
KP=$(openssl rand -hex 24)
printf '%s' "$KP" > "$PASSFILE"; chmod 600 "$PASSFILE"

security create-keychain -p "$KP" "$KC_NAME"
# No -t and no -l: never time out, never lock on sleep.  It still locks on reboot,
# which is what DevCodesign.cmake's unlock step is for.
security set-keychain-settings "$KC"
security unlock-keychain -p "$KP" "$KC"
security import "$WORK/id.p12" -k "$KC" -P "$P12_PASS" -T /usr/bin/codesign >/dev/null

# Without this every signing attempt raises an approval dialog — which is fatal
# with no desktop session to raise it on.  The partition list is the ACL that
# says which callers may use the key without asking.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KP" "$KC" >/dev/null 2>&1
log "keychain: $KC"

# ── 3. Search list ───────────────────────────────────────────────────────────
# codesign resolves `-s <name>` through the user's keychain search list, IN ORDER
# (its --keychain flag does not affect identity lookup at all — it just reports
# "no identity found").  This one goes FIRST so a leftover copy of the same
# identity elsewhere cannot shadow it.  We APPEND rather than replace, and never
# touch the default keychain: clobbering the host's keychain configuration is a
# well-known misfeature of the CI helpers this is modelled on.
OTHERS=$(security list-keychains -d user | sed -e 's/^[[:space:]]*"//' -e 's/"$//' | grep -v "pinpoint-dev.keychain" || true)
# shellcheck disable=SC2086
security list-keychains -d user -s "$KC" $OTHERS
log "search list: $(security list-keychains -d user | tr -d ' "' | tr '\n' ' ')"

# ── 4. Retire the login-keychain copy ────────────────────────────────────────
# A duplicate under the same name in login.keychain-db is not harmless: if the
# search order is ever reset, codesign finds the unreachable copy first and fails
# with errSecInternalComponent, which reads like the setup is broken when it is
# merely shadowed.  Best-effort — it needs the login keychain unlocked.
HASH=$(security find-identity -p codesigning "$KC" | sed -n 's/.*) \([0-9A-F]\{40\}\) .*/\1/p' | head -1)
if [[ -n "$HASH" ]] && security find-identity -p codesigning "$LOGIN_KC" 2>/dev/null | grep -q "$HASH"; then
    security delete-identity -Z "$HASH" "$LOGIN_KC" >/dev/null 2>&1 \
        && log "removed the duplicate from login.keychain-db" \
        || log "NOTE: a copy remains in login.keychain-db (needs a desktop session to remove)"
fi

# ── 5. Prove it ──────────────────────────────────────────────────────────────
# The whole point is signing without interaction, so verify that rather than
# assuming it: a setup that works only from the session that created it is the
# failure this script exists to prevent.
cp /bin/echo "$WORK/probe"
if codesign -f -s "$IDENTITY" "$WORK/probe" >/dev/null 2>&1 \
   && codesign -dvvv "$WORK/probe" 2>&1 | grep -q "^Authority=$IDENTITY"; then
    log "VERIFIED: signed a probe binary non-interactively as '$IDENTITY'"
else
    echo "FAILED: '$IDENTITY' is installed but codesign could not use it" >&2
    exit 1
fi

echo
echo "Done. Builds now sign with '$IDENTITY' from any session, desktop or not."
[[ -n "$P12_IN" ]] || echo "This is a NEW cert — relaunch the app and re-grant its permissions once."
