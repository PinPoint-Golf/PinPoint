#!/usr/bin/env bash
#
# setup_release_signing.sh — install the RELEASE credentials (Developer ID cert +
# notarytool profile) on this Mac.  One-time setup; companion to
# docs/implementation/macos_release_runbook.md Part 0 (§0.3a, §0.5).
#
# ┌─ HOW THIS DIFFERS FROM setup_dev_signing.sh ────────────────────────────────┐
# │ setup_dev_signing.sh builds a DEDICATED keychain with its password on disk,  │
# │ so a headless build can sign with a throwaway SELF-SIGNED cert.  That trade  │
# │ is right for a dev cert and WRONG for a Developer ID distribution cert: a    │
# │ password sitting next to the key defeats the point of keeping the cert       │
# │ offline.  So this script puts the release identity in the LOGIN keychain and │
# │ accepts the consequence — release signing runs in the DESKTOP session, not   │
# │ over SSH.  (codesign cannot reach the login keychain from launchd session    │
# │ `Background`; it dies errSecInternalComponent with nothing to prompt with.)  │
# └──────────────────────────────────────────────────────────────────────────────┘
#
# ┌─ IMPORT, NEVER RE-ISSUE ────────────────────────────────────────────────────┐
# │ Always import the BACKED-UP .p12.  Creating a fresh Developer ID cert looks  │
# │ like it works and quietly breaks two things:                                 │
# │   • TCC grants (camera/mic/Bluetooth/speech) are keyed to the app's          │
# │     designated requirement, which names the signing cert — a new cert resets │
# │     every existing user's permissions.                                       │
# │   • Sparkle treats a differently-signed build as a DIFFERENT app, so in-place │
# │     updates stop working for everyone already installed.                     │
# └──────────────────────────────────────────────────────────────────────────────┘
#
# RUN THIS FROM Terminal.app ON THE MACHINE (or a VNC desktop session) — not over
# SSH.  It unlocks the login keychain, which only unlocks by prompting.  Passwords
# are read interactively and are never written to disk, never passed as argv (where
# `ps` would show them), and never echoed.
#
# Usage:
#   tools/setup_release_signing.sh [--p12 <file>] [--skip-notary] [--skip-cert]
#     --p12 <file>    the backed-up Developer ID .p12 (default: ~/certs/pinpoint_developer_id.p12)
#     --skip-cert     only do the notarytool credential step
#     --skip-notary   only do the certificate import step
#
set -euo pipefail

P12="${P12:-$HOME/certs/pinpoint_developer_id.p12}"
NOTARY_PROFILE="${NOTARY_PROFILE:-pinpoint-notary}"
LOGIN_KC="$HOME/Library/Keychains/login.keychain-db"
DO_CERT=1
DO_NOTARY=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --p12)         P12="${2:-}"; shift ;;
        --skip-cert)   DO_CERT=0 ;;
        --skip-notary) DO_NOTARY=0 ;;
        *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

log() { printf '\033[1;36m[relsign]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[relsign] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ── refuse to run headless, where this cannot work ────────────────────────────
# `security unlock-keychain` and codesign both need the Aqua session. Failing here
# with an explanation beats failing later inside codesign with errSecInternalComponent.
if [[ "$(launchctl managername 2>/dev/null || echo unknown)" != "Aqua" ]]; then
    die "this is not a desktop session (launchd session: $(launchctl managername 2>/dev/null || echo unknown)).
       The login keychain unlocks only by prompting, so run this from Terminal.app on the
       Mac itself (screen share / VNC is fine). See runbook §0.3a."
fi

# ── 1. Developer ID certificate + private key ─────────────────────────────────
if [[ "$DO_CERT" == 1 ]]; then
    [[ -f "$P12" ]] || die "no .p12 at $P12 (pass --p12 <file>)"

    log "importing $P12 into the LOGIN keychain"
    printf 'Password for the .p12 (from the 0.3 backup): '
    read -r -s P12_PASS; echo
    [[ -n "$P12_PASS" ]] || die "empty password"

    # -T grants these tools access without a prompt each time they use the key.
    if ! security import "$P12" -k "$LOGIN_KC" -P "$P12_PASS" \
             -T /usr/bin/codesign -T /usr/bin/security -T /usr/bin/productsign 2>/tmp/relsign.err; then
        if grep -q "already exists" /tmp/relsign.err; then
            log "identity already present in the login keychain — continuing"
        else
            sed 's/^/    /' /tmp/relsign.err >&2
            die "import failed (wrong .p12 password?)"
        fi
    fi
    rm -f /tmp/relsign.err

    # Without this, codesign prompts "wants to use your confidential information"
    # on EVERY signature — which is what turns a release into a clicking marathon.
    log "authorising codesign to use the key without prompting"
    printf 'Your macOS LOGIN password (for set-key-partition-list): '
    read -r -s LOGIN_PASS; echo
    security set-key-partition-list -S apple-tool:,apple:,codesign: \
        -s -k "$LOGIN_PASS" "$LOGIN_KC" >/dev/null 2>&1 \
        || die "set-key-partition-list failed (wrong login password?)"
    unset LOGIN_PASS P12_PASS

    # The .p12 carries the leaf only, not the issuing CA. Without the "Developer ID
    # Certification Authority (G2)" INTERMEDIATE, the leaf cannot chain to Apple Root, so
    # codesign reports it untrusted and `find-identity -v` lists ZERO valid identities —
    # the classic "1 identity imported / 0 valid identities" split. Install it if absent.
    if ! security find-certificate -a -c "Developer ID Certification Authority" 2>/dev/null \
            | grep -q keychain; then
        log "installing the Developer ID G2 intermediate CA (absent — the leaf can't chain without it)"
        g2="$(mktemp -t DeveloperIDG2CA).cer"
        if curl -fsSL -o "$g2" https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer; then
            # Sanity-check it really is the G2 CA before trusting the download.
            if openssl x509 -inform DER -in "$g2" -noout -subject 2>/dev/null \
                 | grep -q "Developer ID Certification Authority"; then
                security import "$g2" -k "$LOGIN_KC" 2>&1 | sed 's/^/    /'
            else
                log "WARN: downloaded intermediate did not look like the G2 CA — skipping"
            fi
            rm -f "$g2"
        else
            log "WARN: could not download the G2 intermediate from apple.com."
            log "WARN: get it from https://www.apple.com/certificateauthority/ (Developer ID - G2)"
            log "WARN: and import it, or the identity below will show 0 valid."
        fi
    fi

    log "verifying the identity is visible to codesign"
    security find-identity -v -p codesigning | sed 's/^/    /'
    security find-identity -v -p codesigning | grep -q "Developer ID Application" \
        || die "no VALID 'Developer ID Application' identity after import — if it imported but
       shows 0 valid, the G2 intermediate is still missing (see the WARN lines above)."
fi

# ── 2. notarytool credentials ─────────────────────────────────────────────────
# Stored as a keychain profile so the release script never sees the password.
# The app-specific password comes from appleid.apple.com → Sign-In and Security.
if [[ "$DO_NOTARY" == 1 ]]; then
    TEAM_ID="$(security find-identity -v -p codesigning \
                 | sed -n 's/.*Developer ID Application: .*(\([A-Z0-9]*\)).*/\1/p' | head -1)"
    [[ -n "$TEAM_ID" ]] || die "could not read the Team ID from the installed identity"
    log "Team ID: $TEAM_ID"

    printf 'Apple ID (developer account email): '
    read -r APPLE_ID
    [[ -n "$APPLE_ID" ]] || die "empty Apple ID"

    log "storing notarytool profile '$NOTARY_PROFILE' (paste the APP-SPECIFIC password, not your Apple ID password)"
    xcrun notarytool store-credentials "$NOTARY_PROFILE" \
        --apple-id "$APPLE_ID" --team-id "$TEAM_ID" \
        || die "store-credentials failed"
fi

log "done. Verify with:"
log "  security find-identity -v -p codesigning"
log "  xcrun notarytool history --keychain-profile $NOTARY_PROFILE"
log ""
log "The Sparkle EdDSA key is NOT stored in a keychain — pass it explicitly:"
log "  packaging/make_appcast_mac.sh --key-file ~/certs/pinpoint_release_mac_eddsa_PRIVATE.pem …"
