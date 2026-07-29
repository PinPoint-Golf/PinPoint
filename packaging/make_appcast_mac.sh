#!/usr/bin/env bash
#
# make_appcast_mac.sh — sign the notarized DMG with the offline Sparkle EdDSA key and
# emit the Sparkle appcast (appcast-mac.xml) for a release. macOS sibling of
# packaging/make_appcast.ps1; Stage 1 / S1·P2 of docs/implementation/macos_update_impl.md.
# See also docs/implementation/macos_release_runbook.md and design §4.1/§6.
#
# Sparkle's feed is an appcast XML hosted as a release asset. This script:
#   1. resolves the version (display + monotonic build number) from src/Core/version.h,
#   2. signs the .dmg with the offline EdDSA private key via Sparkle's `sign_update`
#      (the signature Sparkle pins-verifies before installing — design §6),
#   3. writes appcast-mac.xml with the enclosure URL pointing at the SPECIFIC release
#      tag (so the signed bytes match exactly what Sparkle downloads).
#
# The PRIVATE KEY NEVER LEAVES YOUR MACHINE and is never a CI secret — this runs
# locally as the maintainer's signing step. Sign the EXACT bytes you will publish
# (the notarized DMG), not a local rebuild.
#
# Usage:
#   packaging/make_appcast_mac.sh --tag v0.1-alpha3 [options]
#     --tag <tag>         REQUIRED. git tag of the release; builds the enclosure URL
#                         https://github.com/<repo>/releases/download/<tag>/<dmg>
#     --arch <arch>       x86_64 | arm64 (default: this host). Selects the DMG to sign,
#                         the feed the enclosure must point at, and the appcast filename.
#                         ONE ITEM PER FEED — Sparkle has no arch attribute, so the two
#                         architectures are separated by being different files, never by
#                         negotiation inside one feed (design §4.1).
#     --dmg <path>        the DMG to sign (default: newest dist/PinPointStudio-*-<arch>.dmg)
#     --key-file <path>   offline EdDSA private key from `generate_keys -x` (default:
#                         use the key in the login Keychain, sign_update's default)
#     --notes-url <url>   optional "what's new" link shown in Sparkle's window
#     --tool <sign_update> path to Sparkle's sign_update (default: $SPARKLE_BIN, PATH, _deps)
#     --repo <owner/name> default PinPoint-Golf/PinPointStudio
#     --out <path>        output appcast (default: alongside the DMG)
#     --min-os <ver>      sparkle:minimumSystemVersion (default: read
#                         LSMinimumSystemVersion from the app inside the DMG, which
#                         package_macos.sh §4f stamped with the bundle's real floor)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="PinPoint-Golf/PinPointStudio"
TAG="" DMG="" KEY_FILE="" NOTES_URL="" TOOL="${SPARKLE_BIN:+$SPARKLE_BIN/sign_update}" OUT="" MIN_OS=""
ARCH="$(uname -m)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag)       TAG="$2"; shift ;;
        --dmg)       DMG="$2"; shift ;;
        --key-file)  KEY_FILE="$2"; shift ;;
        --notes-url) NOTES_URL="$2"; shift ;;
        --tool)      TOOL="$2"; shift ;;
        --repo)      REPO="$2"; shift ;;
        --out)       OUT="$2"; shift ;;
        --min-os)    MIN_OS="$2"; shift ;;
        --arch)      ARCH="$2"; shift ;;
        *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

die() { printf '\033[1;31m[appcast-mac] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
log() { printf '\033[1;36m[appcast-mac]\033[0m %s\n' "$*"; }

[[ -n "$TAG" ]] || die "--tag is required (the git tag of the release)"

# ── locate Sparkle's sign_update ──────────────────────────────────────────────────
if [[ -z "$TOOL" ]]; then
    if command -v sign_update >/dev/null 2>&1; then
        TOOL="$(command -v sign_update)"
    else
        TOOL="$(find "$REPO_ROOT/build" -name sign_update -type f 2>/dev/null | head -1)"
    fi
fi
[[ -n "$TOOL" && -x "$TOOL" ]] || die "sign_update not found — set \$SPARKLE_BIN, pass --tool, or build once so CMake fetches Sparkle (Stage 2). It ships in the Sparkle distribution's bin/."
[[ -z "$KEY_FILE" || -f "$KEY_FILE" ]] || die "key file not found: $KEY_FILE"

# ── DMG (the enclosure) ───────────────────────────────────────────────────────────
# The default glob is ARCH-SPECIFIC, deliberately not "newest DMG of either arch".
# Signing one arch's DMG while writing the other arch's appcast is the exact mistake
# that ships an unlaunchable app, and "newest" makes it a coin-flip on release day.
case "$ARCH" in
    x86_64|arm64) ;;
    *) die "--arch must be x86_64 or arm64 (got: $ARCH)" ;;
esac
if [[ -z "$DMG" ]]; then
    DMG="$(ls -t "$REPO_ROOT"/dist/PinPointStudio-*-"$ARCH".dmg 2>/dev/null | head -1 || true)"
fi
[[ -n "$DMG" && -f "$DMG" ]] || die "no ${ARCH} DMG found in dist/ (build one with tools/package_macos.sh, or pass --dmg)"
DMG="$(cd "$(dirname "$DMG")" && pwd)/$(basename "$DMG")"   # absolute
DMG_NAME="$(basename "$DMG")"
LENGTH="$(stat -f%z "$DMG")"

# ── inspect the ACTUAL BYTES being signed ─────────────────────────────────────────
# The filename is hearsay — in the runbook's Path A these bytes were built by CI and
# downloaded, so nothing local has ever checked that the file called "-arm64.dmg" holds
# an arm64 app pointing at the arm64 feed. This is the LAST gate before an EdDSA
# signature blesses them, and Sparkle installs in place: a wrong-arch or wrong-feed
# enclosure does not fail to download, it replaces a working install with a dead one.
#
# Mount once and take all three facts from inside: the slice (lipo), the feed the app
# will actually poll (SUFeedURL), and the floor to advertise (LSMinimumSystemVersion,
# stamped by package_macos.sh §4f with the max minos over the whole shipped closure).
mnt="$(mktemp -d)"
cleanup_mnt() { hdiutil detach "$mnt" -quiet >/dev/null 2>&1 || true; rmdir "$mnt" 2>/dev/null || true; }
trap cleanup_mnt EXIT
hdiutil attach "$DMG" -readonly -nobrowse -mountpoint "$mnt" >/dev/null 2>&1 \
    || die "could not mount $DMG_NAME for verification"
app="$(find "$mnt" -maxdepth 1 -name '*.app' -print -quit)"
[[ -n "$app" ]] || die "no .app inside $DMG_NAME"

dmg_arch="$(lipo -archs "$app/Contents/MacOS/PinPointStudio" 2>/dev/null | tr -s ' ')"
[[ "$dmg_arch" == "$ARCH" ]] \
    || die "ARCH MISMATCH: $DMG_NAME contains a '$dmg_arch' app but this appcast is for '$ARCH'. Signing it would offer $ARCH users a binary that cannot launch."

dmg_feed="$(/usr/libexec/PlistBuddy -c 'Print :SUFeedURL' "$app/Contents/Info.plist" 2>/dev/null || true)"
case "$ARCH" in
    arm64)  want_feed="appcast-mac-arm64.xml" ;;
    x86_64) want_feed="appcast-mac.xml" ;;
esac
[[ "$dmg_feed" == *"/$want_feed" ]] \
    || die "FEED MISMATCH: the $ARCH app in $DMG_NAME polls '$dmg_feed', expected one ending in '$want_feed'. A stale CMake cache produces exactly this, and it makes the app pull the other arch's updates over itself."

if [[ -z "$MIN_OS" ]]; then
    MIN_OS="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$app/Contents/Info.plist" 2>/dev/null || true)"
    [[ -n "$MIN_OS" ]] || die "could not read LSMinimumSystemVersion from $DMG_NAME — repackage with the current tools/package_macos.sh (it stamps §4f), or pass --min-os <ver>"
    log "minimum macOS (read from the DMG): $MIN_OS"
else
    log "minimum macOS (--min-os): $MIN_OS"
fi
cleanup_mnt
trap - EXIT
log "verified enclosure: $DMG_NAME — arch=$dmg_arch feed=$want_feed minOS=$MIN_OS"

# ── version (single source of truth: src/Core/version.h) ──────────────────────────
ver_h="$REPO_ROOT/src/Core/version.h"
maj=$(sed -n 's/^#define PINPOINT_VERSION_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' "$ver_h")
min=$(sed -n 's/^#define PINPOINT_VERSION_MINOR[[:space:]]*\([0-9]*\).*/\1/p' "$ver_h")
pfx=$(sed -n 's/^#define PINPOINT_VERSION_POSTFIX[[:space:]]*"\([^"]*\)".*/\1/p' "$ver_h")
build=$(sed -n 's/^#define PINPOINT_VERSION_BUILD[[:space:]]*\([0-9]*\).*/\1/p' "$ver_h")
[[ -n "$maj" && -n "$min" && -n "$build" ]] || die "could not parse version from $ver_h"
SHORT="v${maj}.${min}${pfx}"   # display (sparkle:shortVersionString / CFBundleShortVersionString)
# $build is the monotonic compare key (sparkle:version) — matches CFBundleVersion.

# ── sign the EXACT DMG bytes ──────────────────────────────────────────────────────
log "signing $DMG_NAME with EdDSA key (sign_update)…"
key_args=()
[[ -n "$KEY_FILE" ]] && key_args=(-f "$KEY_FILE")
# sign_update prints e.g.:  sparkle:edSignature="…" length="12345"
# NB: ${arr[@]+"${arr[@]}"} guards the empty-array expansion under `set -u` on the
# bash 3.2 that ships with macOS (a bare "${key_args[@]}" trips "unbound variable").
sigline="$("$TOOL" ${key_args[@]+"${key_args[@]}"} "$DMG")"
ED_SIG="$(sed -n 's/.*sparkle:edSignature="\([^"]*\)".*/\1/p' <<<"$sigline")"
sig_len="$(sed -n 's/.*length="\([0-9]*\)".*/\1/p' <<<"$sigline")"
[[ -n "$ED_SIG" ]] || die "sign_update produced no signature (is the private key available? output: $sigline)"
[[ -n "$sig_len" ]] && LENGTH="$sig_len"   # prefer the length sign_update reports

ENCLOSURE_URL="https://github.com/${REPO}/releases/download/${TAG}/${DMG_NAME}"
PUBDATE="$(date -u +'%a, %d %b %Y %H:%M:%S +0000')"
NOTES_ELEMENT=""
[[ -n "$NOTES_URL" ]] && NOTES_ELEMENT="      <sparkle:releaseNotesLink>${NOTES_URL}</sparkle:releaseNotesLink>
"

# ── emit appcast-mac.xml ──────────────────────────────────────────────────────────
# x86_64 keeps the name "appcast-mac.xml" FOREVER — it is the URL baked into every
# Intel install in the field (CMakeLists.txt, PP_SU_FEED_URL). arm64 gets its own file.
[[ -n "$OUT" ]] || OUT="$(dirname "$DMG")/$want_feed"
cat > "$OUT" <<XML
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>PinPoint Studio</title>
    <description>PinPoint Studio updates</description>
    <language>en</language>
    <item>
      <title>PinPoint Studio ${SHORT}</title>
${NOTES_ELEMENT}      <pubDate>${PUBDATE}</pubDate>
      <sparkle:minimumSystemVersion>${MIN_OS}</sparkle:minimumSystemVersion>
      <enclosure
        url="${ENCLOSURE_URL}"
        sparkle:version="${build}"
        sparkle:shortVersionString="${SHORT}"
        sparkle:os="macos"
        length="${LENGTH}"
        type="application/octet-stream"
        sparkle:edSignature="${ED_SIG}" />
    </item>
  </channel>
</rss>
XML

log "appcast written: $OUT"
log "  version (compare): $build   short: $SHORT"
log "  enclosure:         $ENCLOSURE_URL"
log "  length:            $LENGTH bytes"
cat <<EOF

Next: upload BOTH ${DMG_NAME} and appcast-mac.xml to release ${TAG}, then publish it
non-prerelease so releases/latest/download/appcast-mac.xml resolves (design §4.1).
Verify the signature first:  "$TOOL" --verify ... (or sparkle's generate_appcast).
EOF
