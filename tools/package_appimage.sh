#!/usr/bin/env bash
#
# package_appimage.sh — build a relocatable, signed, self-updating AppImage for
# PinPoint Studio (Linux x86_64).  Companion to docs/design/linux_update.md and
# docs/implementation/linux_update_impl.md (Phase P0).
#
# ┌─ STATUS ────────────────────────────────────────────────────────────────────┐
# │ Scaffolding.  This script encodes the *correct* packaging recipe but has NOT │
# │ yet been run end-to-end on a clean machine (the dev host lacks linuxdeploy / │
# │ appimagetool / appimageupdatetool and only has a Debug build).  Validate on  │
# │ a clean Ubuntu LTS VM before relying on it for a release (impl plan P0).     │
# └──────────────────────────────────────────────────────────────────────────────┘
#
# What it does:
#   1. Resolve the version from src/Core/version.h (single source of truth).
#   2. Configure + build a Release tree.
#   3. Assemble an AppDir (binary, .desktop, icons) and bundle Qt + QML via
#      linuxdeploy + linuxdeploy-plugin-qt.
#   4. Bundle the heavy native deps not present on a stock host (ORT, x264 FFmpeg,
#      OpenCV, Aravis, espeak-ng, CUDA/cuDNN, Spinnaker) and the runtime tools the
#      app shells out to (appimageupdatetool, yt-dlp).
#   5. Seal with appimagetool, embedding the gh-releases-zsync update information
#      and a GPG signature; emit the companion .zsync and a detached .sig.
#
# Usage:
#   tools/package_appimage.sh [--no-sign] [--no-build]
#
# Key environment variables (override per host):
#   SIGN_KEY        GPG key id/fingerprint for the release signature (required unless --no-sign)
#   GPG_PASSPHRASE  passphrase for the signing key (required for non-interactive/CI
#                   signing when the key is passphrase-protected; omit to use the agent)
#   CMAKE_PREFIX    Qt6 prefix (e.g. ~/Qt/6.11.1/gcc_64); else relies on PATH/env
#   CUDA_LIB_DIR    dir with CUDA 12 runtime .so's to bundle (e.g. /usr/local/cuda-12/lib64)
#   CUDNN_LIB_DIR   dir with cuDNN 9 .so's to bundle
#   SPINNAKER_DIR   Spinnaker SDK root (optional; load-if-present otherwise)
#   JOBS            build parallelism (default 4 — this host OOMs above ~4)
#
set -euo pipefail

# ── repo + output layout ───────────────────────────────────────────────────────
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/appimage-release}"
APPDIR="${APPDIR:-$BUILD_DIR/AppDir}"
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
JOBS="${JOBS:-4}"
GH_OWNER="PinPoint-Golf"
GH_REPO="PinPointStudio"

DO_SIGN=1
DO_BUILD=1
for arg in "$@"; do
    case "$arg" in
        --no-sign)  DO_SIGN=0 ;;
        --no-build) DO_BUILD=0 ;;   # reuse an existing BUILD_DIR (skip configure+build)
        *) die "unknown option: $arg (use --no-sign / --no-build)" ;;
    esac
done

# Run the AppImage-format tools (linuxdeploy/appimagetool/…) via extract-and-run so
# packaging works on hosts/CI without FUSE. Does NOT affect the produced AppImage.
export APPIMAGE_EXTRACT_AND_RUN="${APPIMAGE_EXTRACT_AND_RUN:-1}"

log()  { printf '\033[1;36m[appimage]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[appimage] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# ── 0. tool preflight ───────────────────────────────────────────────────────────
for t in cmake linuxdeploy appimagetool; do
    have "$t" || die "required tool '$t' not on PATH (see docs/implementation/linux_update_impl.md P0)"
done
have linuxdeploy-plugin-qt || die "linuxdeploy-plugin-qt not on PATH"
have appimageupdatetool || log "WARN: appimageupdatetool not found — it must be bundled for in-app updates to work"
have zsyncmake || log "WARN: zsyncmake not found — appimagetool -u cannot emit the .zsync delta file"

# ── 1. version (single source of truth: src/Core/version.h) ──────────────────────
ver_h="$REPO_ROOT/src/Core/version.h"
maj=$(sed -n 's/^#define PINPOINT_VERSION_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' "$ver_h")
min=$(sed -n 's/^#define PINPOINT_VERSION_MINOR[[:space:]]*\([0-9]*\).*/\1/p' "$ver_h")
pfx=$(sed -n 's/^#define PINPOINT_VERSION_POSTFIX[[:space:]]*"\([^"]*\)".*/\1/p' "$ver_h")
VERSION="v${maj}.${min}${pfx}"
[[ -n "$maj" && -n "$min" ]] || die "could not parse version from $ver_h"
log "version: $VERSION   target: $GH_OWNER/$GH_REPO"

APPIMAGE_NAME="PinPointStudio-${VERSION}-x86_64.AppImage"
UPDATE_INFO="gh-releases-zsync|${GH_OWNER}|${GH_REPO}|latest|PinPointStudio-*-x86_64.AppImage.zsync"

# ── 2. configure + build Release ─────────────────────────────────────────────────
if [[ "$DO_BUILD" == 1 ]]; then
    log "configuring Release in $BUILD_DIR"
    cmake_args=(-S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DPP_SHIPPING_BUILD=ON)
    [[ -n "${CMAKE_PREFIX:-}" ]] && cmake_args+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX")
    cmake "${cmake_args[@]}"
    log "building (-j$JOBS)"
    cmake --build "$BUILD_DIR" --parallel "$JOBS"
else
    log "skipping build — packaging existing $BUILD_DIR (--no-build)"
fi

BIN="$BUILD_DIR/PinPointStudio"
[[ -x "$BIN" ]] || die "no binary at $BIN (configure CMAKE_PREFIX / drop --no-build?)"

# ── 3. AppDir skeleton ───────────────────────────────────────────────────────────
log "assembling AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps"
cp "$BIN" "$APPDIR/usr/bin/PinPointStudio"
cp "$REPO_ROOT/PinPointStudio.desktop"                       "$APPDIR/usr/share/applications/PinPointStudio.desktop"
cp "$REPO_ROOT/src/Resources/icons/pinpointstudio_256.png"   "$APPDIR/usr/share/icons/hicolor/256x256/apps/pinpointstudio.png"
cp "$REPO_ROOT/src/Resources/icons/pinpointstudio.svg"       "$APPDIR/usr/share/icons/hicolor/scalable/apps/pinpointstudio.svg" 2>/dev/null || true
cp "$REPO_ROOT/src/Resources/icons/pinpointstudio_256.png"   "$APPDIR/pinpointstudio.png"   # AppDir-root icon

# ── 4. bundle Qt + QML via linuxdeploy ───────────────────────────────────────────
# linuxdeploy-plugin-qt discovers QML imports from the binary + qml sources.
log "running linuxdeploy (Qt plugin)"
export QML_SOURCES_PATHS="$REPO_ROOT/src/Gui"

# linuxdeploy-plugin-qt needs qmake; derive from CMAKE_PREFIX if not already set.
[[ -z "${QMAKE:-}" && -n "${CMAKE_PREFIX:-}" && -x "$CMAKE_PREFIX/bin/qmake" ]] && export QMAKE="$CMAKE_PREFIX/bin/qmake"

# Qt-installer image plugins on newer distros (e.g. Ubuntu 26.04) link an older
# libtiff soname (libqtiff.so → libtiff.so.5) the host no longer ships. We never
# read TIFFs, but linuxdeploy errors on the unresolved dep — shim the missing
# soname to the present libtiff.so.6 so deployment proceeds (inert at runtime).
if ! ldconfig -p | grep -q 'libtiff\.so\.5'; then
    _tiff6=$(ldconfig -p | sed -n 's/.* => \(.*libtiff\.so\.6[^ ]*\)$/\1/p' | head -1)
    if [[ -n "$_tiff6" ]]; then
        mkdir -p "$BUILD_DIR/.deploy-shims"
        ln -sf "$_tiff6" "$BUILD_DIR/.deploy-shims/libtiff.so.5"
        export LD_LIBRARY_PATH="$BUILD_DIR/.deploy-shims:${LD_LIBRARY_PATH:-}"
        log "shimmed libtiff.so.5 → $_tiff6 for deployment (TIFF unused at runtime)"
    fi
fi
# Bundle extra platform plugins alongside the default xcb:
#   wayland   → run natively on Wayland sessions (no "could not find plugin wayland")
#   offscreen → headless runs (CI smoke tests, servers)
# xcb remains the X11 / XWayland fallback.
export EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so"
linuxdeploy --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/PinPointStudio.desktop" \
    --icon-file "$APPDIR/pinpointstudio.png" \
    --plugin qt

# ── 5. bundle the heavy native deps + runtime tools ──────────────────────────────
# These are NOT on a stock host and linuxdeploy will not all find them by itself.
# Paths are host-specific; adjust per build machine. Marked TODO where they must be
# resolved against the actual SDK install. (impl plan P0 acceptance.)
copy_lib() { # copy_lib <glob...>  → into usr/lib, following symlinks
    local f; for f in "$@"; do [[ -e "$f" ]] && cp -Lv "$f" "$APPDIR/usr/lib/" || log "WARN: missing lib $f"; done
}

# Wayland: linuxdeploy bundles the platform plugin (libqwayland.so via
# EXTRA_PLATFORM_PLUGINS) but not its companion plugin dirs. Bundle them so native
# Wayland actually works (shell-integration = xdg-shell etc.; decoration =
# client-side titlebars; graphics-integration = EGL/dmabuf), rpath them to the
# bundled Qt libs, and bundle the libwayland client libs they depend on.
if [[ -n "${QMAKE:-}" ]]; then
    _qtp=$("$QMAKE" -query QT_INSTALL_PLUGINS 2>/dev/null)
    for sub in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
        if [[ -d "$_qtp/$sub" ]]; then
            mkdir -p "$APPDIR/usr/plugins/$sub"
            cp -vn "$_qtp/$sub"/*.so "$APPDIR/usr/plugins/$sub/" 2>/dev/null || true
            if command -v patchelf >/dev/null; then
                for _so in "$APPDIR/usr/plugins/$sub"/*.so; do
                    [[ -e "$_so" ]] && patchelf --set-rpath '$ORIGIN/../../lib' "$_so" 2>/dev/null || true
                done
            fi
        fi
    done
    copy_lib /usr/lib/x86_64-linux-gnu/libwayland-client.so* \
             /usr/lib/x86_64-linux-gnu/libwayland-egl.so* \
             /usr/lib/x86_64-linux-gnu/libwayland-cursor.so*
fi

log "bundling runtime tools (appimageupdatetool, yt-dlp)"
# appimageupdatetool drives the in-app delta update (design §3). REQUIRED.
if have appimageupdatetool; then cp -Lv "$(command -v appimageupdatetool)" "$APPDIR/usr/bin/"; fi
# bundled yt-dlp (Film tab) — see tools/ / cache; copy if present.
[[ -x "$REPO_ROOT/tools/yt-dlp" ]] && cp -v "$REPO_ROOT/tools/yt-dlp" "$APPDIR/usr/bin/"

log "bundling ONNX Runtime / FFmpeg(x264) / OpenCV / Aravis / espeak-ng"
# ORT (CMake fetched it under the build tree's _deps). TODO: confirm exact path.
copy_lib "$BUILD_DIR"/_deps/onnxruntime*/lib/libonnxruntime*.so*
# FFmpeg — MUST be the GPL build WITH libx264 (same trap as the windeployqt x264
# note in BUILDING.md; a non-x264 libav* breaks swing export). Re-copied last so it
# wins over anything linuxdeploy pulled in.
copy_lib /usr/lib/x86_64-linux-gnu/libavcodec.so* \
         /usr/lib/x86_64-linux-gnu/libavformat.so* \
         /usr/lib/x86_64-linux-gnu/libavutil.so* \
         /usr/lib/x86_64-linux-gnu/libswscale.so* \
         /usr/lib/x86_64-linux-gnu/libswresample.so*
# OpenCV, Aravis, espeak-ng — usually linuxdeploy finds these from the binary's
# NEEDED entries; copy_lib here is a backstop, harmless if duplicated.

log "bundling GPU runtime (CUDA 12 + cuDNN 9) — required for GPU ORT"
[[ -n "${CUDA_LIB_DIR:-}"  ]] && copy_lib "$CUDA_LIB_DIR"/libcudart.so* "$CUDA_LIB_DIR"/libcublas*.so* "$CUDA_LIB_DIR"/libcufft.so* || log "WARN: CUDA_LIB_DIR unset — GPU ORT will fall back to CPU on hosts without CUDA"
[[ -n "${CUDNN_LIB_DIR:-}" ]] && copy_lib "$CUDNN_LIB_DIR"/libcudnn*.so* || log "WARN: CUDNN_LIB_DIR unset"

# The Spinnaker SDK EULA forbids redistribution — do NOT bundle it in a shipped
# package. The Windows build delay-loads a user-installed SDK at runtime; on Linux
# HAVE_SPINNAKER is not defined so no Spinnaker code is present. The SPINNAKER_DIR
# hook is retained only for private/local rigs where you have accepted those terms.
if [[ -n "${SPINNAKER_DIR:-}" ]]; then
    log "bundling Spinnaker SDK (LOCAL/private only — EULA forbids redistribution!)"
    copy_lib "$SPINNAKER_DIR"/lib/libSpinnaker*.so*
else
    log "Spinnaker not bundled — industrial cameras degrade to absent (enumerator tolerates this)"
fi

# ── 6. seal: appimagetool with update-info; sign via detached gpg .sig ────────────
# We do NOT use appimagetool's embedded --sign: the in-app gate verifies the
# detached *.AppImage.sig (design §6), and a passphrase-protected key can't be fed
# to appimagetool's internal gpg non-interactively. So sign the .sig ourselves below.
mkdir -p "$DIST_DIR"
out="$DIST_DIR/$APPIMAGE_NAME"
[[ "$DO_SIGN" == 1 && -z "${SIGN_KEY:-}" ]] && die "SIGN_KEY not set (pass --no-sign for an unsigned dev build)"
log "running appimagetool → $out"
# appimagetool writes the companion .zsync to the *current working directory* (by
# basename), not next to the output path — so run it from DIST_DIR with a relative
# output name and both land together. APPDIR is absolute, so it still resolves.
( cd "$DIST_DIR" && ARCH=x86_64 appimagetool -u "$UPDATE_INFO" "$APPDIR" "$APPIMAGE_NAME" )

# Belt-and-braces: relocate a stray .zsync if an older appimagetool dropped it in CWD.
[[ -f "${APPIMAGE_NAME}.zsync" && ! -f "${out}.zsync" ]] && mv "${APPIMAGE_NAME}.zsync" "$DIST_DIR/"
[[ -f "${out}.zsync" ]] && log "zsync delta file: ${out}.zsync" || log "WARN: no .zsync emitted (zsyncmake missing)"

# Detached signature asset — THE trust anchor the in-app gate verifies (design §6).
# For a passphrase-protected key, supply GPG_PASSPHRASE so signing is non-interactive
# (CI, automated local runs); without it, gpg falls back to the agent (cached/prompt).
if [[ "$DO_SIGN" == 1 ]]; then
    log "writing detached signature ${out}.sig"
    gpg_pass_args=()
    [[ -n "${GPG_PASSPHRASE:-}" ]] && gpg_pass_args=(--pinentry-mode loopback --passphrase "$GPG_PASSPHRASE")
    gpg --batch --yes "${gpg_pass_args[@]}" --local-user "$SIGN_KEY" \
        --output "${out}.sig" --detach-sign "$out"
fi

log "done."
log "release artifacts in $DIST_DIR:"
ls -lh "$DIST_DIR"/PinPointStudio-"${VERSION}"-x86_64.AppImage* 2>/dev/null || true
cat <<EOF

Next: upload these as assets to a GitHub Release tagged exactly '${VERSION}'
on ${GH_OWNER}/${GH_REPO} (CI does this in P2 — .github/workflows/release.yml):
  ${APPIMAGE_NAME}
  ${APPIMAGE_NAME}.zsync
  ${APPIMAGE_NAME}.sig
EOF
