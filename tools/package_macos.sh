#!/usr/bin/env bash
#
# package_macos.sh — build a relocatable, (optionally signed+notarized) .app inside
# a drag-install .dmg for PinPoint Studio (macOS, x86_64 / Intel for v1; runs under
# Rosetta 2 on Apple Silicon).  Companion to docs/design/macos_update.md and
# docs/implementation/macos_update_impl.md (Stage 1, S1·P0–P1).
#
# ┌─ STATUS ────────────────────────────────────────────────────────────────────┐
# │ The deploy → bundle → relocatable-verify → DMG recipe is exercised on a copy │
# │ of an existing .app (the smoke test).  A from-scratch Release build + clean- │
# │ second-Mac validation are the gates in the impl plan (S1·P0 acceptance).     │
# │ Code signing + notarization live in the (guarded) §5 block, which SKIPS when │
# │ no Developer ID identity is available — so this still emits an UNSIGNED DMG.  │
# └──────────────────────────────────────────────────────────────────────────────┘
#
# What it does:
#   1. Resolve the version from src/Core/version.h (single source of truth).
#   2. Build a Release tree (or reuse one / an already-built .app via --app).
#   3. macdeployqt: bundle Qt frameworks + QML modules + plugins, relinking the
#      app's Homebrew dylibs (OpenCV / FFmpeg / Aravis, all under /usr/local/opt)
#      into Contents/Frameworks with @rpath install names.
#   4. Bundle the deps macdeployqt misses — the @rpath ONNX Runtime dylib (from the
#      build's _deps tree) and, if present, Spinnaker (dlopen'd, so not in NEEDED).
#   4d′. Strip build-machine LC_RPATH entries (Homebrew + build-tree) the linker baked in
#      — a stray /usr/local rpath makes dyld load a duplicate Homebrew dylib (e.g. a second
#      libomp → OMP Error #15 launch crash) ahead of the bundled one.
#   5. VERIFY relocatability: no Mach-O in the bundle may still reference an absolute
#      /usr/local or /opt/homebrew path — as a dependency OR an LC_RPATH (the clean-host gate).
#   6. (§5, guarded) codesign --options runtime + notarytool + stapler.
#   7. Assemble PinPointStudio-<ver>-x86_64.dmg (the .app + an /Applications symlink).
#
# Usage:
#   tools/package_macos.sh [--no-build] [--app <prebuilt.app>] [--no-sign] [--check-only]
#     --no-build        reuse an existing $BUILD_DIR (skip configure+build)
#     --app <path>      package an already-built .app (COPIED first — never mutated);
#                       implies --no-build. Used by the smoke test / CI artifact reuse.
#     --no-sign         force-skip the codesign/notarize block even if a cert exists
#     --check-only      run ONLY the §5 verification gate against an already-staged
#                       bundle ($WORK_DIR/PinPointStudio.app, or --app <path>) and exit.
#                       Seconds instead of a full repackage while debugging the gate.
#
# Key environment variables (override per host):
#   CMAKE_PREFIX       Qt6 prefix (default: ~/Qt/6.11.0/macos)
#   JOBS               build parallelism (default 6 — this Mac OOMs above ~6)
#   SIGN_IDENTITY      "Developer ID Application: …" codesign identity (§5; auto-detected if unset)
#   NOTARY_PROFILE     notarytool keychain profile name (§5; required to notarize)
#
set -euo pipefail

# ── repo + output layout ───────────────────────────────────────────────────────
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# NB: a dedicated release tree — NEVER build/Qt_6_11_0_for_macOS-Debug (that is
# QtCreator's; macdeployqt mutates the .app in place, which would corrupt it).
# Arch-qualified trees. Both DMGs coexist in dist/ by filename, but the build and
# staging trees must not: an x86_64 CMake cache reused for an arm64 build (or the other
# way round) silently produces a bundle whose ORT package, CoreML define and Sparkle feed
# belong to the other architecture.
HOST_ARCH="$(uname -m)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/macos-release-$HOST_ARCH}"
WORK_DIR="${WORK_DIR:-$REPO_ROOT/build/macos-pkg-$HOST_ARCH}"
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
# Qt prefix. Default to the newest ~/Qt/<ver>/macos that actually carries macdeployqt:
# a hardcoded version makes the script die at its own preflight the moment the local Qt
# is bumped (this machine has 6.11.1 only, and the old 6.11.0 default did exactly that).
# CI passes CMAKE_PREFIX explicitly from install-qt-action, so it never sees this default.
if [[ -z "${CMAKE_PREFIX:-}" ]]; then
    for _qt in "$HOME"/Qt/*/macos; do
        [[ -x "$_qt/bin/macdeployqt" ]] && CMAKE_PREFIX="$_qt"
    done
fi
CMAKE_PREFIX="${CMAKE_PREFIX:-$HOME/Qt/6.11.0/macos}"
JOBS="${JOBS:-6}"
DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-13.0}"   # our own code's floor (macOS 13 Ventura)
GH_OWNER="PinPoint-Golf"
GH_REPO="PinPointStudio"

DO_BUILD=1
DO_SIGN=1
DO_CHECK_ONLY=0
PREBUILT_APP=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)  DO_BUILD=0 ;;
        --no-sign)   DO_SIGN=0 ;;
        --check-only) DO_CHECK_ONLY=1 ;;
        --app)       PREBUILT_APP="${2:-}"; DO_BUILD=0; shift ;;
        *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

log()  { printf '\033[1;36m[macpkg]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[macpkg] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# List every Mach-O we relocate: the main executable + all dylibs/Qt plugins in the
# bundle (plugins are .dylib on macOS). Re-evaluated each pass so libs copied in one
# pass get processed in the next. Qt .framework binaries link only Qt/system, never
# Homebrew, so they need no scanning here.
list_macho() {
    find "$STAGE_APP" -type f -name '*.dylib' 2>/dev/null
    [[ -f "$STAGE_APP/Contents/MacOS/PinPointStudio" ]] && echo "$STAGE_APP/Contents/MacOS/PinPointStudio"
}

# Copy a dylib into Frameworks under the name the loader will actually ASK FOR, and
# echo that name. A dylib's LC_ID_DYLIB is what its dependents record, and it need not
# match the file name on disk: ORT ships libonnxruntime.1.26.0.dylib whose id is
# @rpath/libonnxruntime.1.dylib. Copying it under its own filename (and rewriting the id
# to match) leaves the requested name absent from the bundle — dyld then fails before
# main() on a clean Mac. Prefer the id's basename; fall back to the filename for a dylib
# with no id.
bundle_dylib() {
    local src="$1" id base
    id="$(macho_id "$src")"
    base="$(basename "${id:-$src}")"
    [[ -n "$base" ]] || base="$(basename "$src")"
    cp -L "$src" "$FW/$base" || return 1
    chmod u+w "$FW/$base"
    install_name_tool -id "@rpath/$base" "$FW/$base" 2>/dev/null || true
    printf '%s' "$base"
}

# Recursively rewrite every absolute Homebrew dependency in the bundle to @rpath and
# pull the referenced dylib into Frameworks. macdeployqt does this for the top level
# but leaves deep transitive trees half-done (OpenCV → VTK → webp; the Qt sql plugin
# → libmimerapi), so the bundle still points at /usr/local on a clean Mac. Fixpoint:
# re-scan after each pass until a pass copies nothing new.
relocate_closure() {
    local fw="$STAGE_APP/Contents/Frameworks"
    mkdir -p "$fw"
    local pass=0 changed=1
    while [[ "$changed" == 1 ]]; do
        changed=0; pass=$((pass+1))
        local macho dep base copied=0
        while IFS= read -r macho; do
            [[ -f "$macho" ]] || continue
            chmod u+w "$macho" 2>/dev/null || true
            if [[ "$macho" == "$fw/"* ]]; then
                # A lib in Frameworks needs @loader_path so @rpath/<sibling> resolves.
                if ! otool -l "$macho" 2>/dev/null | grep -q 'path @loader_path '; then
                    install_name_tool -add_rpath "@loader_path" "$macho" 2>/dev/null || true
                fi
                # …and its own install id must be @rpath, not the build-machine path
                # macdeployqt left behind (-change can't touch the id; -id can).
                case "$(otool -D "$macho" 2>/dev/null | tail -n +2 | head -1)" in
                    /usr/local/*|/opt/homebrew/*)
                        install_name_tool -id "@rpath/$(basename "$macho")" "$macho" 2>/dev/null || true ;;
                esac
            fi
            while IFS= read -r dep; do
                case "$dep" in
                    /usr/local/*|/opt/homebrew/*)
                        base="$(basename "$dep")"
                        install_name_tool -change "$dep" "@rpath/$base" "$macho" 2>/dev/null || true
                        if [[ ! -f "$fw/$base" && -f "$dep" ]]; then
                            cp -L "$dep" "$fw/$base" && chmod u+w "$fw/$base"
                            install_name_tool -id "@rpath/$base" "$fw/$base" 2>/dev/null || true
                            changed=1; copied=$((copied+1))
                        fi ;;
                esac
            done < <(otool -L "$macho" 2>/dev/null | tail -n +2 | awk '{print $1}')
        done < <(list_macho)
        log "relocate pass $pass — pulled $copied new lib(s) into Frameworks"
        [[ $pass -ge 15 ]] && { log "WARN: relocate did not converge in 15 passes"; break; }
    done
    return 0   # the final [[ pass -ge 15 ]] test leaves $?=1; don't let `set -e` abort
}

# List the LC_RPATH entries of a Mach-O, one per line (path only).
list_rpaths() {
    otool -l "$1" 2>/dev/null \
        | awk '/ cmd LC_RPATH$/{f=1;next} f&&/ path /{sub(/^[[:space:]]*path /,"");sub(/ \(offset [0-9]+\)$/,"");print;f=0}'
}

# Strip build-machine LC_RPATH entries from every bundled Mach-O. macdeployqt adds an
# @executable_path/../Frameworks rpath but does NOT remove the rpaths the linker baked
# in at build time — notably the Homebrew OpenCV lib dir (/usr/local/opt/opencv/lib,
# from `brew --prefix opencv` on CMAKE_PREFIX_PATH) and the build tree's _deps/sparkle-src.
# Those survive into the shipped bundle, and because a stray Homebrew rpath is searched
# BEFORE @executable_path/../Frameworks, dyld resolves @rpath/libopencv_*.dylib to the
# Homebrew copy on any host that has Homebrew OpenCV installed — loading a SECOND
# OpenMP/OpenBLAS stack alongside the bundled one. Two libomp runtimes make
# __kmp_register_library_startup abort before main() (the classic OMP Error #15 launch
# crash). Delete every rpath that points at the build host so only @executable_path/
# @loader_path/@rpath relative entries remain.
strip_build_rpaths() {
    local macho rp stripped=0
    while IFS= read -r macho; do
        [[ -f "$macho" ]] || continue
        chmod u+w "$macho" 2>/dev/null || true
        while IFS= read -r rp; do
            case "$rp" in
                /usr/local/*|/opt/homebrew/*|"$REPO_ROOT"/*)
                    install_name_tool -delete_rpath "$rp" "$macho" 2>/dev/null \
                        && { log "  stripped rpath $rp from ${macho#"$STAGE_APP"/}"; stripped=$((stripped+1)); } || true ;;
            esac
        done < <(list_rpaths "$macho")
    done < <(list_macho)
    log "stripped $stripped build-machine rpath(s)"
}

# The architecture of the STAGED BUNDLE, read off the binary itself rather than assumed
# from the build host. This is what names the DMG, so the name can never disagree with
# the bytes — including on the --app path, where a bundle built elsewhere arrives and the
# host tells you nothing about it.
detect_app_arch() {
    local exe="$STAGE_APP/Contents/MacOS/PinPointStudio" archs
    [[ -f "$exe" ]] || die "no main executable at $exe"
    archs="$(lipo -archs "$exe" 2>/dev/null | tr -s '[:space:]' ' ' | sed 's/ *$//')" \
        || die "lipo could not read $exe"
    case "$archs" in
        arm64|x86_64) printf '%s' "$archs" ;;
        "")  die "could not determine the architecture of $exe" ;;
        *)   die "bundle is universal or unexpected ($archs). One arch per DMG is the design — ONNX Runtime versions diverge per arch, so a fat binary cannot carry both engines coherently. Build host-native." ;;
    esac
}

# The dependencies (LC_LOAD_DYLIB) of a Mach-O, one path per line.
#
# Select on the leading TAB rather than skipping a fixed number of header lines: many Qt
# plugins are UNIVERSAL binaries, and otool -L prints a separate "<path> (architecture
# <arch>):" header for every slice. A `tail -n +2` drops only the first, so the second
# slice's header — an absolute build-machine path — is read as if it were a dependency,
# and the whole PlugIns tree reports as unrelocated. Dependency lines are tab-indented;
# headers never are.
macho_deps() {
    otool -L "$1" 2>/dev/null | awk '/^\t/{print $1}'
}

# A Mach-O's own install id (LC_ID_DYLIB), or empty for an executable or a plugin bundle
# (MH_BUNDLE carries no install id at all — most Qt plugins are in this category).
# NB: otool -D emits one or more header lines ("<path>:", and "<path> (architecture
# arm64):" on some toolchains) before the id, so a fixed `tail -n +2` picks up a header
# instead of the id. Drop every line that ends in ':' — an install name never does.
# The trailing `|| true` is load-bearing under `set -o pipefail`: a plain executable has
# NO install id, so grep matches nothing and exits 1, which would abort the whole script.
macho_id() {
    otool -D "$1" 2>/dev/null | grep -v ':[[:space:]]*$' | head -1 | sed 's/^[[:space:]]*//' || true
}

# Expand a Mach-O's LC_RPATH entries into concrete directories, one per line:
# @loader_path → the file's own directory, @executable_path → Contents/MacOS.
expand_rpaths() {
    local macho="$1" dir="$2" rp exedir="$STAGE_APP/Contents/MacOS"
    while IFS= read -r rp; do
        [[ -n "$rp" ]] || continue
        case "$rp" in
            @loader_path*)     rp="$dir${rp#@loader_path}" ;;
            @executable_path*) rp="$exedir${rp#@executable_path}" ;;
        esac
        printf '%s\n' "$rp"
    done <<< "$(list_rpaths "$macho")"
}

# Does <suffix> exist under any of the newline-separated roots?
resolve_in_roots() {
    local roots="$1" suffix="$2" r
    while IFS= read -r r; do
        [[ -n "$r" ]] || continue
        [[ -e "$r/$suffix" ]] && return 0
    done <<< "$roots"
    return 1
}

# Every Mach-O we ship: the dylibs and main binary from list_macho(), plus the binaries
# inside bundled .framework bundles (Qt, Sparkle), which list_macho() skips because they
# carry no .dylib suffix.
list_all_macho() {
    list_macho
    find "$STAGE_APP/Contents/Frameworks" -type f -perm -u+x -path '*.framework/Versions/*' \
         ! -name '*.dylib' ! -name '*.prl' 2>/dev/null
}

# The bundle's REAL minimum macOS: the HIGHEST LC_BUILD_VERSION minos across everything
# we ship. An app can only run where its whole closure runs, so this — not our own
# CMAKE_OSX_DEPLOYMENT_TARGET — is the number users must be told.
#
# It is a computed value rather than a constant because Homebrew bottles are built for
# the PACKAGING HOST's macOS: OpenCV/FFmpeg/Aravis/glib bottles on a macOS 26 host all
# carry minos 26.0, so a DMG built there requires macOS 26 no matter what we pin. Pinning
# alone therefore cannot make the advertised floor true; deriving it can.
bundle_min_os() {
    local macho v
    {
        while IFS= read -r macho; do
            [[ -f "$macho" ]] || continue
            otool -l "$macho" 2>/dev/null | awk '
                /LC_BUILD_VERSION/   {b=1} b&&/ *minos /   {print $2; b=0}
                /LC_VERSION_MIN_MAC/ {m=1} m&&/ *version / {print $2; m=0}'
        done < <(list_all_macho)
    } | sort -V | tail -1
}

# The clean-host gate. Two independent checks over every bundled Mach-O:
#
#  (a) No absolute build-machine path may survive — neither as a dependency
#      (LC_LOAD_DYLIB) nor as a search path (LC_RPATH). A bad dependency crashes with
#      "image not found" on a stock Mac; a bad rpath silently loads a Homebrew copy of a
#      dylib ahead of the bundled one (the OMP Error #15 double-libomp launch crash).
#
#  (b) Every @rpath dependency must actually RESOLVE inside the bundle. Check (a) is
#      blind here by construction: it only rejects absolute paths, so a dylib that was
#      correctly rewritten to @rpath but never copied in — or copied in under a name
#      nobody asks for — looks perfectly clean to it and reaches notarization. Both
#      ONNX Runtime bugs fixed in §4a/§4a′ were exactly this shape.
#
# Skip each Mach-O's own LC_ID_DYLIB: a dylib lists its own id first in otool -L output,
# and it is a declaration, not a dependency to satisfy.
verify_bundle() {
    log "verifying relocatability (no /usr/local or /opt/homebrew deps or rpaths)"
    local bad=0 macho rp dep self suffix
    while IFS= read -r macho; do
        [[ -f "$macho" ]] || continue
        self="$(macho_id "$macho")"
        # Any ABSOLUTE dependency outside /usr/lib and /System is a build-machine path.
        # Allow-list rather than deny-list: /usr/local and /opt/homebrew are the common
        # cases, but Qt's sqldrivers plugins also link things like
        # /Applications/Postgres.app/…/libpq.5.dylib, which a deny-list of Homebrew
        # prefixes waves straight through to notarization. /usr/lib and /System are the
        # only trees guaranteed to exist on a stock Mac.
        #
        # Skip the file's own LC_ID_DYLIB, which otool -L prints ahead of the real
        # dependencies. macdeployqt leaves many Qt plugins with an absolute id; that is
        # a declaration nothing resolves at load time, and flagging it would report the
        # entire PlugIns tree as broken. What matters is what OTHER binaries link, and
        # those show up as their own dependency lines.
        while IFS= read -r dep; do
            [[ -n "$self" && "$dep" == "$self" ]] && continue
            case "$dep" in
                /usr/lib/*|/System/*) ;;
                /*) printf '\033[1;31m  UNRELOCATED DEP:\033[0m %s → %s\n' "${macho#"$STAGE_APP"/}" "$dep"
                    bad=1 ;;
            esac
        done <<< "$(macho_deps "$macho")"
        while IFS= read -r rp; do
            case "$rp" in
                /usr/local/*|/opt/homebrew/*|"$REPO_ROOT"/*)
                    printf '\033[1;31m  STRAY RPATH:\033[0m %s → %s\n' "${macho#"$STAGE_APP"/}" "$rp"
                    bad=1 ;;
            esac
        done < <(list_rpaths "$macho")
    done < <(list_macho)

    # dyld substitutes @rpath using the LC_RPATHs of the whole loader chain, so the MAIN
    # EXECUTABLE's rpaths apply to every image the process loads — expand them once and
    # add them to every file's candidate roots.
    log "verifying @rpath dependencies resolve inside the bundle"
    local exe="$STAGE_APP/Contents/MacOS/PinPointStudio" exe_roots="" roots
    [[ -f "$exe" ]] && exe_roots="$(expand_rpaths "$exe" "$(dirname "$exe")")"
    while IFS= read -r macho; do
        [[ -f "$macho" ]] || continue
        self="$(macho_id "$macho")"
        roots="$(expand_rpaths "$macho" "$(dirname "$macho")")"   # once per file, not per dep
        [[ "$macho" != "$exe" ]] && roots="$roots
$exe_roots"
        while IFS= read -r dep; do
            case "$dep" in
                @rpath/*) ;;
                *) continue ;;
            esac
            [[ -n "$self" && "$dep" == "$self" ]] && continue
            suffix="${dep#@rpath/}"
            if ! resolve_in_roots "$roots" "$suffix"; then
                printf '\033[1;31m  UNRESOLVED @rpath:\033[0m %s → %s\n' "${macho#"$STAGE_APP"/}" "$dep"
                bad=1
            fi
        done <<< "$(macho_deps "$macho")"
    done < <(list_macho)

    if [[ "$bad" == 1 ]]; then
        die "bundle verification FAILED (see above) — this app would not launch on a clean Mac"
    fi
    log "bundle OK — self-contained, and every @rpath dependency resolves"
}

MACDEPLOYQT="${MACDEPLOYQT:-$CMAKE_PREFIX/bin/macdeployqt}"

# ── 0. tool preflight ───────────────────────────────────────────────────────────
for t in hdiutil otool install_name_tool ditto lipo; do
    have "$t" || die "required tool '$t' not on PATH"
done

# ── 0a. --check-only: verify an already-staged bundle and stop ────────────────────
# Deliberately ahead of the macdeployqt check: verification reads an existing bundle
# with otool alone, and requiring a Qt install to re-run the gate would make the fast
# debug loop depend on the slow one.
if [[ "$DO_CHECK_ONLY" == 1 ]]; then
    STAGE_APP="${PREBUILT_APP:-$WORK_DIR/PinPointStudio.app}"
    [[ -d "$STAGE_APP" ]] || die "--check-only: no bundle at $STAGE_APP (stage one first, or pass --app <path>)"
    STAGE_APP="$(cd "$STAGE_APP" && pwd)"
    log "--check-only: verifying $STAGE_APP"
    verify_bundle
    exit 0
fi

[[ -x "$MACDEPLOYQT" ]] || die "macdeployqt not found at $MACDEPLOYQT (set CMAKE_PREFIX)"

# ── 1. version (single source of truth: src/Core/version.h) ──────────────────────
ver_h="$REPO_ROOT/src/Core/version.h"
maj=$(sed -n 's/^#define PINPOINT_VERSION_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' "$ver_h")
min=$(sed -n 's/^#define PINPOINT_VERSION_MINOR[[:space:]]*\([0-9]*\).*/\1/p' "$ver_h")
pfx=$(sed -n 's/^#define PINPOINT_VERSION_POSTFIX[[:space:]]*"\([^"]*\)".*/\1/p' "$ver_h")
VERSION="v${maj}.${min}${pfx}"
[[ -n "$maj" && -n "$min" ]] || die "could not parse version from $ver_h"
# DMG_NAME is deliberately NOT set here — it is derived from the built binary in §2a,
# once the .app exists. It used to be hardcoded "-x86_64", which on an Apple Silicon host
# wrote an arm64 app into an x86_64-labelled DMG with no complaint.
log "version: $VERSION   target: $GH_OWNER/$GH_REPO"

# ── 2. obtain the .app (build Release, reuse, or copy a prebuilt one) ─────────────
mkdir -p "$WORK_DIR"
STAGE_APP="$WORK_DIR/PinPointStudio.app"
rm -rf "$STAGE_APP"

if [[ -n "$PREBUILT_APP" ]]; then
    [[ -d "$PREBUILT_APP" ]] || die "--app path is not a bundle: $PREBUILT_APP"
    log "copying prebuilt bundle (source is never mutated): $PREBUILT_APP"
    cp -a "$PREBUILT_APP" "$STAGE_APP"
else
    if [[ "$DO_BUILD" == 1 ]]; then
        log "configuring Release in $BUILD_DIR (PINPOINT_INSTALLED=ON)"
        # CMAKE_OSX_DEPLOYMENT_TARGET pins OUR code's floor. Nothing set it before, so
        # clang defaulted to the build host's OS — a 26.x binary that cannot start on
        # anything older, while the appcast advertised 12.0.0. NB this pins our code
        # only: the bundle's real floor is the MAX over everything we ship (§4f), and
        # Homebrew bottles are built for the packaging host's macOS.
        cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX" \
              -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
              -DPINPOINT_INSTALLED=ON
        log "building (-j$JOBS)"
        cmake --build "$BUILD_DIR" --parallel "$JOBS"
    else
        log "skipping build — reusing $BUILD_DIR (--no-build)"
    fi
    SRC_APP="$BUILD_DIR/PinPointStudio.app"
    [[ -d "$SRC_APP" ]] || die "no .app at $SRC_APP (build failed, or pass --app)"
    cp -a "$SRC_APP" "$STAGE_APP"
fi

# ── 2a. name the DMG after the bytes, and check the feed matches them ─────────────
APP_ARCH="$(detect_app_arch)"
DMG_NAME="PinPointStudio-${VERSION}-${APP_ARCH}.dmg"
log "bundle arch: $APP_ARCH   dmg: $DMG_NAME"

# Cross-check the Sparkle feed baked into this bundle against the arch actually built.
# CMakeLists.txt picks the feed from CMAKE_SYSTEM_PROCESSOR at CONFIGURE time, so a stale
# cache — or a build tree reused across architectures — bakes in the other arch's feed
# while emitting this arch's binary. The result is an app that polls the wrong appcast and
# installs the other architecture's DMG over itself on the very next update check. That is
# unrecoverable for the user, so it must never leave this machine.
case "$APP_ARCH" in
    arm64)  want_feed="appcast-mac-arm64.xml" ;;
    x86_64) want_feed="appcast-mac.xml" ;;
esac
baked_feed="$(/usr/libexec/PlistBuddy -c 'Print :SUFeedURL' "$STAGE_APP/Contents/Info.plist" 2>/dev/null || true)"
[[ -n "$baked_feed" ]] || die "no SUFeedURL in the staged Info.plist — the bundle cannot update itself"
[[ "$baked_feed" == *"/$want_feed" ]] || die "FEED/ARCH MISMATCH: a $APP_ARCH bundle has SUFeedURL '$baked_feed', expected one ending in '$want_feed'. Delete $BUILD_DIR and re-configure — a stale CMake cache produces exactly this."
log "feed check OK: $APP_ARCH → $want_feed"

# ── 3. macdeployqt: Qt frameworks + QML + plugins + Homebrew dylib relocation ─────
# macdeployqt copies the app's externally-linked dylibs (incl. /usr/local/opt
# OpenCV/FFmpeg/Aravis) into Contents/Frameworks and rewrites their install names to
# @rpath, adding an @executable_path/../Frameworks LC_RPATH to the main binary.
log "running macdeployqt"
"$MACDEPLOYQT" "$STAGE_APP" -qmldir="$REPO_ROOT/src" -verbose=1

FW="$STAGE_APP/Contents/Frameworks"
MACOS="$STAGE_APP/Contents/MacOS"
mkdir -p "$FW"

# ── 4. bundle the deps macdeployqt misses ─────────────────────────────────────────
# 4a. ONNX Runtime — linked as @rpath/libonnxruntime.<ver>.dylib (so macdeployqt may
#     not resolve it). Copy from the build's _deps tree into Frameworks; the
#     @executable_path/../Frameworks rpath macdeployqt added then resolves it.
#     The guard glob is libonnxruntime.*.dylib (with the dot) — NOT libonnxruntime*,
#     which 4a′ below would satisfy with libonnxruntime-genai.dylib and skip ORT proper.
if ! ls "$FW"/libonnxruntime.*.dylib >/dev/null 2>&1; then
    ort=""
    for cand in "$BUILD_DIR"/_deps/onnxruntime-*/lib/libonnxruntime.*.dylib \
                "$REPO_ROOT"/build/*/_deps/onnxruntime-*/lib/libonnxruntime.*.dylib; do
        [[ -f "$cand" ]] && { ort="$cand"; break; }
    done
    if [[ -n "$ort" ]]; then
        ort_base="$(bundle_dylib "$ort")" || die "failed to bundle ONNX Runtime from $ort"
        log "bundling ONNX Runtime: $ort_base (source $(basename "$ort"))"
    else
        log "WARN: ONNX Runtime dylib not found under _deps — GPU/CPU inference will fail to load"
    fi
fi

# 4a′. ONNX Runtime GenAI (local LLM engine) — a SEPARATE FetchContent download from ORT
#     proper, unpacked under _deps/onnxruntime_genai-*/ (note the underscore), so 4a's
#     glob can never reach it. The app hard-links @rpath/libonnxruntime-genai.dylib
#     whenever HAVE_ORTGENAI is defined, so a missing copy is a dyld failure before
#     main(). Its LINK-time closure is just itself + system frameworks, but it also
#     dlopen()s ORT proper at RUNTIME (see the alias below) — that edge is in no NEEDED
#     list, so neither macdeployqt nor §5's verifier can see it. Intel forces
#     WITH_ORTGENAI OFF (no Intel macOS prebuilt after v0.11.4), so there is nothing to
#     find there and this block is a clean no-op — which is why only arm64 hits this.
if ! ls "$FW"/libonnxruntime-genai*.dylib >/dev/null 2>&1; then
    genai=""
    for cand in "$BUILD_DIR"/_deps/onnxruntime_genai-*/lib/libonnxruntime-genai*.dylib \
                "$REPO_ROOT"/build/*/_deps/onnxruntime_genai-*/lib/libonnxruntime-genai*.dylib; do
        [[ -f "$cand" ]] && { genai="$cand"; break; }
    done
    if [[ -n "$genai" ]]; then
        genai_base="$(bundle_dylib "$genai")" || die "failed to bundle ORT-GenAI from $genai"
        log "bundling ONNX Runtime GenAI: $genai_base"
    else
        log "ORT-GenAI dylib not found under _deps — expected on Intel (local LLM falls back to Gemini)"
    fi
fi

# 4a″. GenAI's RUNTIME dependency on ORT proper. GenAI dlopen()s the UNVERSIONED name
#     "libonnxruntime.dylib" from @loader_path, but 4a bundles ORT under its VERSIONED
#     name only (libonnxruntime.1.dylib). Without this alias the dlopen finds nothing,
#     GenAI's static initializer throws std::runtime_error ("Failed to load onnxruntime"),
#     and dyld SIGABRTs the app before main() — the bundle looks perfect and never opens.
#
#     This is deliberately OUTSIDE 4a′: GenAI is a NEEDED dep of the app binary, so
#     macdeployqt usually copies it in first, which makes 4a′'s guard false and skips that
#     whole block. Keying off "is GenAI in the bundle" — no matter who put it there — is
#     what makes this fire. §5 cannot cover it either, because a dlopen is not a NEEDED
#     entry, so this alias is the only thing standing between us and a DOA arm64 DMG.
if ls "$FW"/libonnxruntime-genai*.dylib >/dev/null 2>&1 && [[ ! -e "$FW/libonnxruntime.dylib" ]]; then
    ort_versioned="$(cd "$FW" && ls libonnxruntime.*.dylib 2>/dev/null | head -1)"
    [[ -n "$ort_versioned" ]] || die "ORT-GenAI is bundled but no libonnxruntime.*.dylib is — the app would abort in dyld at launch"
    ln -sf "$ort_versioned" "$FW/libonnxruntime.dylib"
    log "aliasing libonnxruntime.dylib → $ort_versioned (ORT-GenAI dlopen target)"
fi

# 4b. Spinnaker (proprietary, dlopen'd → not in NEEDED, so macdeployqt skips it).
#     Its EULA forbids redistribution, so do NOT bundle it in a shipped package. The
#     Windows build delay-loads a user-installed SDK at runtime; on macOS HAVE_SPINNAKER
#     is not defined so no Spinnaker code is present. The SPINNAKER_DIR hook is retained
#     only for private/local rigs where you have accepted those terms.
if [[ -n "${SPINNAKER_DIR:-}" ]]; then
    log "bundling Spinnaker SDK from $SPINNAKER_DIR (LOCAL/private only — EULA forbids redistribution!)"
    for s in "$SPINNAKER_DIR"/lib/libSpinnaker*.dylib; do
        [[ -f "$s" ]] && cp -L "$s" "$FW/" || true
    done
else
    log "Spinnaker not bundled (SPINNAKER_DIR unset) — industrial cameras degrade to absent"
fi

# 4c. yt-dlp (Film tab shells out to it). CMake stages yt-dlp_macos next to the build
#     binary; carry it into the bundle if present.
for y in "$BUILD_DIR/yt-dlp_macos" "$MACOS/yt-dlp_macos" "$REPO_ROOT/tools/yt-dlp_macos"; do
    [[ -f "$y" && ! -f "$MACOS/yt-dlp_macos" ]] && { cp "$y" "$MACOS/yt-dlp_macos"; chmod +x "$MACOS/yt-dlp_macos"; log "bundled yt-dlp_macos"; }
done

# 4d. Recursively relocate the Homebrew closure macdeployqt left half-done.
log "relocating Homebrew dependency closure (OpenCV/FFmpeg/Aravis + transitive deps)"
relocate_closure

# 4d′. Strip build-machine LC_RPATH entries the linker/macdeployqt left behind. Without
#      this a stray /usr/local/opt/opencv/lib rpath makes dyld load the Homebrew OpenCV
#      (→ Homebrew OpenBLAS → a 2nd libomp) on hosts that have Homebrew OpenCV, which
#      aborts at launch with OMP Error #15. See strip_build_rpaths above.
log "stripping build-machine rpaths (Homebrew + build-tree leftovers)"
strip_build_rpaths

# 4e. Sparkle.framework — the macOS in-app update engine (docs/design/macos_update.md).
#     macdeployqt does NOT reliably relocate a framework that carries nested helpers
#     (Autoupdate, Updater.app, XPCServices), so copy the WHOLE framework verbatim AFTER
#     macdeployqt, preserving its Versions/ symlink structure. The main binary's
#     @executable_path/../Frameworks rpath (added by macdeployqt) resolves Sparkle's
#     @rpath/Sparkle.framework/Versions/B/Sparkle install name; the nested helpers are
#     signed bottom-up in §6. CMake fetched it to _deps/sparkle-src/.
if [[ ! -d "$FW/Sparkle.framework" ]]; then
    sparkle_fw=""
    for cand in "$BUILD_DIR"/_deps/sparkle-src/Sparkle.framework \
                "$REPO_ROOT"/build/*/_deps/sparkle-src/Sparkle.framework; do
        [[ -d "$cand" ]] && { sparkle_fw="$cand"; break; }
    done
    if [[ -n "$sparkle_fw" ]]; then
        log "bundling Sparkle.framework (+ Autoupdate/Updater.app/XPCServices helpers)"
        ditto "$sparkle_fw" "$FW/Sparkle.framework"
    else
        log "WARN: Sparkle.framework not found under _deps — in-app update will be inert"
    fi
fi

# ── 4e. drop Qt SQL driver plugins we can never load ──────────────────────────────
# macdeployqt copies the whole sqldrivers/ directory whenever QtSql is present anywhere
# in the dependency graph, regardless of use. PinPoint uses no SQL at all — no
# QSqlDatabase, no QtSql link, not even QML LocalStorage — and the ODBC / Mimer /
# PostgreSQL drivers each link a third-party client library that is NOT in the bundle
# (libiodbc, libmimerapi, libpq from a local Postgres.app install). Every one of them is
# an unloadable plugin carrying an unresolvable dependency into notarization.
#
# Delete the whole directory rather than the three broken ones: keeping libqsqlite alone
# would ship a driver for a database the app never opens.
if [[ -d "$STAGE_APP/Contents/PlugIns/sqldrivers" ]]; then
    rm -rf "$STAGE_APP/Contents/PlugIns/sqldrivers"
    log "removed unused Qt sqldrivers plugins (no SQL usage in this app)"
fi

# ── 4f. stamp the bundle's REAL minimum macOS into Info.plist ─────────────────────
# LSMinimumSystemVersion becomes the single source of truth that make_appcast_mac.sh
# reads, so the Sparkle feed cannot advertise a floor the bundle does not meet. That
# mismatch is not cosmetic: Sparkle offers any update whose minimumSystemVersion the
# user's Mac satisfies, then installs it IN PLACE — so an over-optimistic number does
# not decline gracefully, it replaces a working app with one that cannot launch.
MIN_OS="$(bundle_min_os)"
if [[ -n "$MIN_OS" ]]; then
    PB=/usr/libexec/PlistBuddy
    "$PB" -c "Set :LSMinimumSystemVersion $MIN_OS" "$STAGE_APP/Contents/Info.plist" 2>/dev/null \
        || "$PB" -c "Add :LSMinimumSystemVersion string $MIN_OS" "$STAGE_APP/Contents/Info.plist"
    log "bundle minimum macOS: $MIN_OS (stamped as LSMinimumSystemVersion)"
    # Compare against our own pin: if the closure floor is higher, something we did not
    # compile set it — in practice the packaging host's Homebrew bottles.
    if [[ "$(printf '%s\n%s\n' "$MIN_OS" "$DEPLOYMENT_TARGET" | sort -V | tail -1)" != "$DEPLOYMENT_TARGET" ]]; then
        log "WARN: bundle floor $MIN_OS EXCEEDS the $DEPLOYMENT_TARGET target — this DMG will not"
        log "WARN: run on macOS < $MIN_OS. Homebrew bottles are built for the packaging host's"
        log "WARN: macOS, so packaging on an older host is what lowers this."
    fi
else
    log "WARN: could not determine bundle minimum macOS — appcast will fall back to its default"
fi

# ── 5. VERIFY the bundle — the clean-host gate ────────────────────────────────────
verify_bundle

# ── 6. code signing + notarization (cred-guarded) ─────────────────────────────────
# Developer ID codesign (Hardened Runtime) → notarytool → staple. Each stage SKIPS
# with a clear message when its credential is absent, so auto/dev runs still emit an
# unsigned DMG. Real signing needs SIGN_IDENTITY (a "Developer ID Application" cert in
# the keychain) and NOTARY_PROFILE (a `notarytool store-credentials` profile). The DMG
# itself is signed+stapled in §7 after it is built (the notarization unit on macOS is
# the disk image the user downloads). docs/design/macos_update.md §6.
# Hardened Runtime entitlements (required for notarization). Kept MINIMAL and
# comment-free — codesign's AMFI parser rejects XML comments (an XML comment may not
# contain a double hyphen, which any "--flag" rationale would). Rationale instead lives
# here:
#   - disable-library-validation: PinPoint bundles many third-party dylibs (ORT, OpenCV,
#     FFmpeg, Aravis, Qt). Without it the process is killed when it loads a library not
#     signed by our Team ID. Re-signing every nested binary could let us drop this later.
#   - allow-jit / allow-unsigned-executable-memory / allow-dyld-environment-variables are
#     deliberately OMITTED until a launch crash proves one is needed (add the minimum).
# Camera/mic/Bluetooth/Speech are Info.plist NSxxxUsageDescription strings, not here.
ENTITLEMENTS="$REPO_ROOT/packaging/macos/entitlements.plist"

detect_identity() {   # echo a Developer ID Application identity if one exists, else nothing
    [[ -n "${SIGN_IDENTITY:-}" ]] && { echo "$SIGN_IDENTITY"; return; }
    security find-identity -v -p codesigning 2>/dev/null \
        | sed -n 's/.*"\(Developer ID Application:[^"]*\)".*/\1/p' | head -1
}

codesign_bundle() {
    local identity; identity="$(detect_identity)"
    if [[ -z "$identity" ]]; then
        log "SKIP codesign — no 'Developer ID Application' identity in the keychain (set up the cert, or this stays UNSIGNED)"
        return 1
    fi
    [[ -f "$ENTITLEMENTS" ]] || die "entitlements not found: $ENTITLEMENTS"
    log "codesigning with: $identity"
    # Sparkle ships nested code the generic dylib/framework sweep below does NOT descend
    # into — XPC services, Updater.app, and the Autoupdate helper. They must be signed
    # bottom-up BEFORE the framework is sealed (the framework seal references them), or
    # notarization rejects the unsigned nested code. --options runtime (Hardened
    # Runtime), no entitlements (only the main app carries those). docs/design/macos_update.md §6.
    sparkle_ver="$STAGE_APP/Contents/Frameworks/Sparkle.framework/Versions/B"
    if [[ -d "$sparkle_ver" ]]; then
        log "signing Sparkle helpers (XPCServices, Updater.app, Autoupdate)"
        for item in \
            "$sparkle_ver/XPCServices/Downloader.xpc" \
            "$sparkle_ver/XPCServices/Installer.xpc" \
            "$sparkle_ver/Updater.app" \
            "$sparkle_ver/Autoupdate"; do
            [[ -e "$item" ]] || continue
            codesign --force --options runtime --timestamp \
                     --sign "$identity" "$item" || die "codesign failed: ${item#"$STAGE_APP"/}"
        done
    fi
    # Sign inside-out: every nested dylib/framework/helper/plugin first, the .app last.
    # --options runtime (Hardened Runtime) + --timestamp are notarization prerequisites.
    while IFS= read -r f; do
        codesign --force --options runtime --timestamp \
                 --sign "$identity" "$f" 2>/dev/null || die "codesign failed: ${f#"$STAGE_APP"/}"
    done < <( { find "$STAGE_APP/Contents/Frameworks" -type f \( -name '*.dylib' -o -name '*.framework' \) 2>/dev/null
               find "$STAGE_APP/Contents/Frameworks" -type d -name '*.framework' 2>/dev/null
               find "$STAGE_APP/Contents/PlugIns" "$STAGE_APP/Contents/Resources" -type f -name '*.dylib' 2>/dev/null
               [[ -f "$STAGE_APP/Contents/MacOS/yt-dlp_macos" ]] && echo "$STAGE_APP/Contents/MacOS/yt-dlp_macos"; } | sort -u )
    # The app last, with entitlements — its seal covers the now-signed contents.
    codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" \
             --sign "$identity" "$STAGE_APP" || die "codesign failed: app bundle"
    codesign --verify --deep --strict --verbose=1 "$STAGE_APP" || die "codesign verification failed"
    log "codesign OK"
    return 0
}

# Notarize + staple a built artifact (the DMG). Returns non-zero (skips) without creds.
notarize_and_staple() {
    local artifact="$1"
    if [[ -z "${NOTARY_PROFILE:-}" ]]; then
        log "SKIP notarize — NOTARY_PROFILE unset (run: xcrun notarytool store-credentials)"
        return 1
    fi
    log "submitting $(basename "$artifact") to notarytool (waits for Apple)…"
    xcrun notarytool submit "$artifact" --keychain-profile "$NOTARY_PROFILE" --wait \
        || die "notarization failed (see: xcrun notarytool log)"
    log "stapling notarization ticket"
    xcrun stapler staple "$artifact" || die "stapler failed"
    xcrun stapler validate "$artifact" || die "stapler validate failed"
    log "notarize + staple OK"
}

SIGNED=0
if [[ "$DO_SIGN" == 1 ]]; then
    codesign_bundle && SIGNED=1
else
    log "signing skipped (--no-sign)"
fi

# ── 7. assemble the drag-install DMG ──────────────────────────────────────────────
mkdir -p "$DIST_DIR"
out="$DIST_DIR/$DMG_NAME"
rm -f "$out"
stage="$WORK_DIR/dmg-stage"
rm -rf "$stage"; mkdir -p "$stage"
cp -a "$STAGE_APP" "$stage/PinPointStudio.app"
ln -s /Applications "$stage/Applications"     # drag-to-install affordance
log "building DMG → $out"
hdiutil create -volname "PinPoint Studio $VERSION" \
    -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$out" >/dev/null
rm -rf "$stage"

# Sign + notarize the DMG itself (the artifact the user downloads). Both skip cleanly
# without creds, leaving a valid unsigned DMG.
if [[ "$SIGNED" == 1 ]]; then
    identity="$(detect_identity)"
    log "codesigning the DMG"
    codesign --force --timestamp --sign "$identity" "$out" || die "DMG codesign failed"
    notarize_and_staple "$out" || log "DMG left un-notarized (no NOTARY_PROFILE)"
fi

log "done."
[[ "$SIGNED" == 1 ]] && log "bundle: SIGNED (Developer ID)" || log "bundle: UNSIGNED (no cert — dev/auto build)"
ls -lh "$out"
cat <<EOF

Next (S1·P1/P2, maintainer gates): codesign + notarize this .app, generate the
EdDSA-signed appcast-mac.xml (packaging/make_appcast_mac.sh), and publish both to a
GitHub Release tagged '${VERSION}' on ${GH_OWNER}/${GH_REPO}
(docs/implementation/macos_release_runbook.md).
EOF
