# Building PinPoint Studio

This document outlines the dependencies and steps required to build PinPoint Studio from source on Linux, macOS, and Windows.

## General Requirements

- **CMake**: 3.16 or higher (3.24+ recommended — `DOWNLOAD_EXTRACT_TIMESTAMP` support).
- **Qt 6.11**: Quick, QuickControls2, Quick3D, SerialPort, Bluetooth, Multimedia, Network, WebSockets, Concurrent, ShaderTools, GuiPrivate, CorePrivate (the Private modules back the RHI Bayer-demosaic video item).
- **C++ Compiler**: C++20 capable (`CMAKE_CXX_STANDARD 20`, required) — GCC 10+, Clang 12+, or MSVC 2022.
- **OpenCV**: 3.0+ (required on all platforms — pose preprocessing, swing export demosaic).
- **FFmpeg** (optional): libavcodec/libavformat/libavutil/libswscale dev libraries with **libx264**, located via `pkg-config`. Powers the swing-export H.264/MP4 encoder. Without it the app still builds and runs, but swing video export is disabled. GPL FFmpeg builds are fine — the project is GPLv2+.

---

## Linux

PinPoint Studio uses `pkg-config` to locate system libraries.

### 1. Install Build Essentials
```bash
sudo apt update
sudo apt install build-essential cmake pkg-config git
```
`git` is required at configure time — CMake clones whisper.cpp, libsamplerate, Eigen, and (if absent on the system) espeak-ng via FetchContent.

### 2. Install Qt 6.11
Use the [Qt Online Installer](https://www.qt.io/download-qt-installer). Select the following modules:
- Qt Quick / Qt Quick Controls 2 / Qt Quick 3D
- Qt Serial Port, Qt Bluetooth, Qt Multimedia
- Qt Network, Qt WebSockets, Qt Concurrent, Qt Shader Tools

### 3. System Dependencies
```bash
# Industrial camera support (Aravis v0.8.x)
sudo apt install libglib2.0-dev libaravis-dev

# Camera device identifier lookup (USB serial numbers for V4L2 cameras)
sudo apt install libudev-dev

# Pose estimation and video pre-processing
sudo apt install libopencv-dev

# Swing video export (H.264 via libx264) — optional but recommended
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev

# Optional: espeak-ng (built from source if absent)
sudo apt install libespeak-ng-dev
```

### 4. GPU Acceleration (Optional)

#### Whisper (STT)
- **Vulkan** (recommended — works on NVIDIA, AMD, Intel):
  ```bash
  sudo apt install libvulkan-dev glslc
  ```
  `glslc` is the shader compiler ggml's Vulkan backend invokes at build time (CMake's `Vulkan_GLSLC_EXECUTABLE`).
- **CUDA**: Install the [CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit). Vulkan is preferred and checked first; CUDA is the fallback when Vulkan headers are absent.

#### ONNX Runtime (TTS / pose estimation)
`WITH_CUDA` defaults ON. **Unlike the Windows path, the Linux CMake does not probe the installed CUDA version.** When `WITH_CUDA=ON` it unconditionally downloads `onnxruntime-linux-x64-gpu-1.26.0.tgz`, which is built against **CUDA 12.x + cuDNN 9**. There is no CUDA-13 Linux package selection (the `gpu_cuda13` variant exists only for Windows).

- **CUDA 12.x runtime + cuDNN 9 installed** → the CUDA EP loads and ORT runs on the GPU.
- **CUDA 13.x installed (no CUDA 12 alongside)** → the CUDA-12-built EP fails to load at runtime because it imports CUDA-12-versioned shared libraries (e.g. `libcublasLt.so.12`, `libcudnn.so.9`), so ORT **silently falls back to CPU**. To get GPU ORT under CUDA 13, install a CUDA 12.x runtime alongside it (the EP only needs the CUDA 12 `.so`s present, not as the default toolkit), or wait for a CUDA-13 Linux package to be wired into CMake.
- **No CUDA, incompatible CUDA, or `-DWITH_CUDA=OFF`** → ORT runs on CPU. Passing `-DWITH_CUDA=OFF` also avoids the larger GPU download, fetching the CPU-only `onnxruntime-linux-x64-1.26.0.tgz` instead.

> cuDNN 9 is a separate install from the CUDA Toolkit. On Linux install the cuDNN 9 runtime matching CUDA 12 from NVIDIA's apt repository (e.g. `libcudnn9-cuda-12`) or the [cuDNN download page](https://developer.nvidia.com/cudnn).

> **Intel integrated GPU (including Linux on MacBook Pro Intel):**
> ORT's only GPU execution provider on Linux is CUDA. Intel UHD and Iris integrated graphics cannot use it.
> OpenVINO would be the natural alternative EP for Intel hardware, but it requires building ORT from source
> (`-DONNXRUNTIME_USE_OPENVINO=ON`) — Intel stopped bundling their own ORT build in the OpenVINO toolkit
> after the 2024.x series. Pre-built `libonnxruntime_providers_openvino.so` is not available for download.
> ORT therefore runs on CPU on any Linux machine without an NVIDIA GPU.

---

## macOS

Dependencies are best managed via [Homebrew](https://brew.sh/).

### 1. Install Dependencies
```bash
brew install cmake espeak-ng aravis opencv ffmpeg
```
CMake injects the Homebrew prefix into `PKG_CONFIG_PATH` automatically, so `ffmpeg` is found without any extra configuration.

### 2. Install Qt 6.11
Use the [Qt Online Installer](https://www.qt.io/download-qt-installer) or:
```bash
brew install qt@6
export PATH="$(brew --prefix qt@6)/bin:$PATH"
```

### 3. GPU Acceleration

| Platform | Whisper (STT) | ONNX Runtime (TTS / pose) |
|---|---|---|
| Apple Silicon (M1/M2/M3/M4) | Metal (via ggml) | CoreML (ORT 1.26.0) |
| Intel Mac | CPU only | CPU only (ORT pinned to 1.20.1 — last Intel release) |

No additional GPU SDK installation is needed on Apple Silicon — CoreML and Metal are part of the OS.

### 4. Dev Code Signing (recommended)

```bash
tools/setup_dev_signing.sh
```

Run once per machine. Without it the dev bundle is left **unsigned** and the build prints a warning; everything still compiles and runs, but **camera / microphone / speech / Bluetooth permissions have to be granted again after every rebuild**. macOS keys those grants (TCC) to the binary's code signature, and an unsigned build has no durable identity to match — the app shows as "enabled" in System Settings while every entitled API returns Denied.

The script creates a self-signed `PinPoint Dev` identity in a **dedicated keychain** (`~/Library/Keychains/pinpoint-dev.keychain-db`, password in `~/.config/pinpoint/dev-keychain.pass`, mode 0600) and `cmake/DevCodesign.cmake` signs the finished bundle with it at POST_BUILD.

The dedicated keychain is the point, not an implementation detail. The login keychain unlocks only by **prompting**, so any build not driven from a desktop session — an agent or CI job over SSH, which launchd reports as session `Background` rather than `Aqua` — cannot use it, and codesign fails `errSecInternalComponent` with no way to ask. A keychain whose password is on disk unlocks with no interaction, which is the same reason fastlane ships `create_keychain`/`setup_ci`.

> **Moving an identity that already has grants.** TCC matches the *designated requirement*, which pins the certificate (`certificate leaf = H"…"`), so a new cert silently resets every permission. To keep them, export the old identity and adopt it: `tools/setup_dev_signing.sh --p12 <file>`. The export itself must run from **Terminal.app on the machine** — exporting a private key always requires GUI approval and no ACL or partition list covers it:
> ```bash
> security export -t identities -f pkcs12 -P <pass> -o /tmp/pp.p12
> ```

Troubleshooting: `codesign` resolves `-s <name>` through the keychain **search list, in order** (its `--keychain` flag does *not* affect identity lookup — it reports `no identity found`), so `pinpoint-dev.keychain-db` must come first in `security list-keychains -d user`. A stale copy of the same identity in an earlier keychain shadows it and fails. This is entirely separate from the release path in `tools/package_macos.sh`, which signs with a Developer ID cert from the login keychain in a real desktop session.

---

## Windows

### 1. Visual Studio
Install [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) with the **"Desktop development with C++"** workload.

### 2. Qt 6.11
Install via the [Qt Online Installer](https://www.qt.io/download-qt-installer). Select the **MSVC 2022 64-bit** component.

### 3. GPU Acceleration

#### Whisper (STT) — ggml backends
Vulkan is detected and preferred over CUDA. Install whichever you have:
- **Vulkan SDK**: [vulkan.lunarg.com](https://vulkan.lunarg.com/) — works on NVIDIA, AMD, and Intel Arc.
- **CUDA Toolkit**: [developer.nvidia.com/cuda-toolkit](https://developer.nvidia.com/cuda-toolkit) — CUDA fallback when Vulkan headers are absent. Any recent CUDA version works.

#### ONNX Runtime (TTS / pose estimation) — CUDA EP
ORT 1.26.0 ships two Windows GPU packages. CMake selects automatically based on the installed CUDA Toolkit major version:

| Installed CUDA | ORT package selected | EP in app |
|---|---|---|
| CUDA 12.x | `onnxruntime-win-x64-gpu-1.26.0.zip` | CUDA |
| CUDA 13.x | `onnxruntime-win-x64-gpu_cuda13-1.26.0.zip` | CUDA |
| Other / none | `onnxruntime-win-x64-1.26.0.zip` | CPU |

> **Note:** DirectML was discontinued from ORT packages after version 1.24.4 and is no longer supported.

#### cuDNN 9 (required for ORT CUDA EP)
The ORT CUDA EP depends on **cuDNN 9** at runtime. Install it from the [NVIDIA cuDNN download page](https://developer.nvidia.com/cudnn) (free developer account required). Use the **Windows installer** — it places the DLLs under `C:\Program Files\NVIDIA\CUDNN\v9.x\bin\<cuda_major>.<minor>\x64\`.

CMake automatically copies the matching cuDNN DLLs next to the executable at POST_BUILD, so no PATH changes are needed after installation.

> **cuDNN must be installed before running CMake configure** — the glob that finds the DLLs runs at configure time.

### 4. Spinnaker SDK (Optional)
For Teledyne/FLIR industrial cameras, install the [Spinnaker SDK](https://www.teledyneflir.com/products/spinnaker-sdk/) to the default location: `C:\Program Files\Teledyne\Spinnaker`.

The build machine needs the SDK to compile the backend (headers + import lib), and
`HAVE_SPINNAKER` is defined automatically when CMake finds it. The SDK is **never
redistributed**: its imports are delay-loaded and the DLLs are not bundled in the
installer (its EULA forbids it). Distributed release builds therefore include the
integration but discover a **user-installed** SDK at runtime (`spinnaker_runtime.cpp`,
probing the default path above; override with `PINPOINT_SPINNAKER_ROOT`). End users
install the Spinnaker SDK separately to enable high-speed cameras; without it the app
runs normally minus that backend.

### 5. Aravis (Optional)
Set the `ARAVIS_ROOT` environment variable to your Aravis installation directory.

### 6. OpenCV (Required, 3.0+)
Download from [opencv.org](https://opencv.org/releases/). CMake probes `C:\opencv\build` and `C:\tools\opencv\build` automatically. For a custom location: `-DOpenCV_DIR=C:\path\to\opencv\build`.

### 7. FFmpeg (Optional — swing video export)
The swing-export H.264 encoder needs FFmpeg dev libraries with **libx264**, found via `pkg-config`:

1. **pkg-config**: install one if not present, e.g. `choco install pkgconfiglite` (or [pkgconf](https://github.com/pkgconf/pkgconf)). It must be on PATH at configure time.
2. **FFmpeg**: download a `win64-gpl-shared` build from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases) (e.g. `ffmpeg-n7.1-latest-win64-gpl-shared-7.1.zip`) and extract it so that `C:\ffmpeg\lib\pkgconfig` exists. CMake probes `C:\ffmpeg` and `C:\tools\ffmpeg`; for a custom location pass `-DFFMPEG_DIR=`. The BtbN shared builds include MSVC import libraries and relocatable `.pc` files — no patching needed. The **gpl** variant is required for libx264 (the project is GPLv2+, so GPL FFmpeg is fine).

The linked FFmpeg DLLs (`avcodec`, `avformat`, `avutil`, `swscale`, `swresample`) are copied next to the executable at POST_BUILD. If FFmpeg was installed after a previous configure, clear the cached probe before reconfiguring:
```
cmake -U "FFMPEG*" -U "__pkg_config_checked_FFMPEG" <builddir>
```

> **windeployqt caveat:** Qt Multimedia bundles its own LGPL FFmpeg with the **same DLL names** (`avcodec-61.dll`, …) but *without* libx264. Running `windeployqt` overwrites the copied DLLs and breaks swing export ("libx264 encoder not available"). After deploying, re-copy the five DLLs from `C:\ffmpeg\bin` over the deployed ones — Qt Multimedia works fine against the GPL build (same ABI, superset of features).

---

## Build Instructions

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/qt/6.11.x/compiler_arch
cmake --build . --config Release
```

On first configure, CMake downloads all required models and binaries (see table below). Subsequent configures reuse the cache in `build/_deps/` — only a full directory wipe triggers re-downloads.

---

## Packaging & Release (Linux — AppImage with in-app update)

Linux ships as a single self-updating **AppImage** — the native analogue of the
macOS (Sparkle) / Windows (WinSparkle) update story. Design:
[`docs/design/linux_update.md`](docs/design/linux_update.md); build plan:
[`docs/implementation/linux_update_impl.md`](docs/implementation/linux_update_impl.md).

**Build the AppImage:**
```bash
# Host tools (once): linuxdeploy, linuxdeploy-plugin-qt, appimagetool,
# appimageupdatetool, zsync (zsyncmake). All from the AppImageCommunity releases.
export SIGN_KEY=<your-release-gpg-key-fpr>
export CMAKE_PREFIX=~/Qt/6.11.0/gcc_64
export CUDA_LIB_DIR=/usr/local/cuda-12/lib64   # for GPU ORT
export CUDNN_LIB_DIR=/usr/lib/x86_64-linux-gnu # cuDNN 9
tools/package_appimage.sh                      # → dist/PinPointStudio-<ver>-x86_64.AppImage{,.zsync,.sig}
tools/package_appimage.sh --no-sign            # unsigned dev build (no in-app update trust)
```

The script resolves the version from `src/Core/version.h`, builds Release, bundles
Qt/QML + the heavy native deps (ORT, the **x264-capable** FFmpeg — same trap as the
`windeployqt` x264 note in the Windows section, OpenCV, Aravis, espeak-ng,
CUDA/cuDNN, optional Spinnaker) + the `appimageupdatetool` binary the app shells out
to, then
seals with `appimagetool` embedding the `gh-releases-zsync` update information and a
GPG signature.

> **Not yet validated end-to-end.** As of this commit the script encodes the recipe
> but has not been run on a clean VM (the dev host lacks the AppImage tooling). The
> P0 acceptance gate (clean-Ubuntu launch with BLE + cameras + GPU + x264 export) is
> still open — see the impl plan.

**Releasing:** tag exactly the `version.h` string (e.g. `v0.1-alpha1`) and push;
CI (`.github/workflows/release.yml`, P2) builds, signs, and attaches the three
artifacts to the GitHub Release. The in-app updater polls that release feed.

---

## Packaging & Release (macOS — DMG with in-app update)

macOS ships as a Developer ID-signed, notarized **`.app` inside a `.dmg`**, kept up to
date in place by **Sparkle** (the framework WinSparkle is a port of), gated by a pinned
**EdDSA (Ed25519)** signature. Design:
[`docs/design/macos_update.md`](docs/design/macos_update.md) ·
**release runbook:** [`docs/implementation/macos_release_runbook.md`](docs/implementation/macos_release_runbook.md).

**Build the DMG:**
```bash
# Build deps come from Homebrew (CMake probes them): opencv@4, ffmpeg, aravis.
#   brew install opencv@4 ffmpeg aravis
# NB opencv@4, NOT opencv — the plain formula is 5.x now, which moves calib3d into
# separate 3d/calib modules and does not build here. CMake asks for opencv@4 by name
# (it is keg-only, so `brew --prefix opencv` cannot see it) and rejects 5.x explicitly.
tools/package_macos.sh                         # → dist/PinPointStudio-<ver>-<arch>.dmg (signs+notarizes if creds set)
tools/package_macos.sh --no-sign               # unsigned dev build (no in-app update trust)
tools/package_macos.sh --app <prebuilt.app>    # package an already-built .app (copied, never mutated)
tools/package_macos.sh --check-only            # re-run ONLY the verification gate on the staged bundle
```
The DMG is named from the **built binary**, not from the build host: `lipo -archs` on the
staged executable decides the `-x86_64` / `-arm64` suffix, and the script refuses a
universal binary (one arch per DMG — the two architectures ship different ONNX Runtime
majors). It also cross-checks the Sparkle feed baked into `Info.plist` against that arch
and aborts on a mismatch, which is what a stale CMake cache produces.
The script resolves the version from `src/Core/version.h`, builds Release, runs
`macdeployqt`, then **recursively relocates the Homebrew dependency closure** macdeployqt
leaves half-done (OpenCV → VTK → webp; the Qt sql plugin → libmimerapi) into
`Contents/Frameworks` with `@rpath` install names, and **verifies** the bundle before
building the DMG (the clean-host gate, which fails loudly). The gate makes two passes: no
Mach-O may keep an **absolute** dependency outside `/usr/lib` and `/System` (an allow-list —
a deny-list of Homebrew prefixes missed Qt's psql plugin linking a `/Applications/Postgres.app`
path), and every **`@rpath` dependency must actually resolve** inside the bundle, resolved
the way dyld does — including the main executable's `LC_RPATH`s, which is how dlopen'd Qt
plugins find their frameworks. That second pass is what catches a dylib correctly rewritten
to `@rpath` but never copied in, or copied in under a name nothing asks for. With a
**Developer ID Application** identity in the keychain and a `NOTARY_PROFILE`, it also
codesigns (Hardened Runtime, entitlements at `packaging/macos/entitlements.plist`),
notarizes, and staples; without them it emits a valid **unsigned** DMG.

- **Two DMGs, two feeds** — `PinPointStudio-<ver>-x86_64.dmg` → `appcast-mac.xml`, and
  `PinPointStudio-<ver>-arm64.dmg` → `appcast-mac-arm64.xml`. Each architecture's binary
  only ever sees its own feed; there is no arch negotiation inside a feed (Sparkle has no
  such attribute). **`appcast-mac.xml` is a frozen name** — it is baked into the
  `Info.plist` of every Intel install in the field and Sparkle re-reads it from the
  *installed* bundle, so renaming it for symmetry would orphan them permanently.
  There is **no CUDA / no component split** on macOS — the DMG is the whole,
  hardware-agnostic app (acceleration is CoreML/Accelerate/Metal).
- **The two builds are not equivalent.** arm64 gets ONNX Runtime 1.26 with the CoreML
  execution provider **and ORT-GenAI (local LLM)**; x86_64 is pinned to ORT **1.20.1**, CPU
  only, with **no local LLM** (it falls back to Gemini). This is not a choice — Microsoft
  stopped shipping Intel macOS prebuilts for both after ORT 1.21 / ORT-GenAI 0.11.4, so the
  gap widens with every release. Intel is also on a clock at the CI end: GitHub retires
  x86_64 macOS runners in **August 2027**.
- **The shipped minimum macOS is set by the build host, not by us.** Homebrew bottles are
  compiled for the packaging machine's macOS, so a DMG built on a macOS 26 Mac requires
  macOS 26 whatever `CMAKE_OSX_DEPLOYMENT_TARGET` says. `package_macos.sh` therefore
  *computes* the floor (the highest `LC_BUILD_VERSION` minos across everything bundled),
  stamps it as `LSMinimumSystemVersion`, and `make_appcast_mac.sh` reads it back out of the
  DMG it is about to sign. **Ship CI-built DMGs**, not locally-built ones, unless you intend
  the floor your own Mac imposes.
- Developer ID signing + notarization are a **v1 requirement** on macOS (not optional
  polish): an un-notarized app is quarantined and App Translocation stops Sparkle updating
  in place. The cert + notary credential + the EdDSA private key all stay **offline, never
  CI secrets**.

> **Status:** shipping. A Developer ID-signed, notarized, **Sparkle-capable** DMG (with the
> in-app updater embedded and the real pinned EdDSA key) + an EdDSA-signed `appcast-mac.xml`
> are published on the `v0.1-alpha3` release. Still open: the clean-second-Mac acceptance
> gate (BLE + camera + STT + CoreML + x264 export, no Gatekeeper warning) and a real
> update-**offer** test (needs a higher-versioned release, since Sparkle offers only when
> `sparkle:version` strictly exceeds the installed `CFBundleVersion`) — see the impl plan
> [`docs/implementation/macos_update_impl.md`](docs/implementation/macos_update_impl.md).

**Releasing:** tag exactly the `version.h` string (e.g. `v0.1-alpha3`) and push; CI's
`macos` job is a **two-leg matrix** — `x86_64` on `macos-15-intel` and `arm64` on
`macos-15` — and each leg builds its **unsigned** DMG and stages it on the same **draft**
release. `fail-fast: false`, so a flaky leg cannot cancel the other one's good DMG; a
`macos-assets-complete` job then refuses to go green unless **both** DMGs are on the draft,
because a feed whose architecture has no enclosure is invisible in the GitHub UI. The
maintainer signs + notarizes each exact DMG locally and runs `make_appcast_mac.sh --arch`
**once per architecture**, uploads all four assets, and publishes non-prerelease — full
steps in the runbook.

---

## Packaging & Release (Windows — Installer with in-app update)

Windows ships as a per-user **Inno Setup installer** kept up to date in place by
**WinSparkle** (the Windows analogue of macOS Sparkle / the Linux AppImage updater),
gated by a pinned **EdDSA (Ed25519)** signature. Design:
[`docs/design/windows_update.md`](docs/design/windows_update.md); build plan:
[`docs/implementation/windows_update_impl.md`](docs/implementation/windows_update_impl.md);
**release runbook:** [`docs/implementation/windows_release_runbook.md`](docs/implementation/windows_release_runbook.md).

### Prerequisites
- **Inno Setup 6** (`ISCC.exe`) — `choco install innosetup` or [jrsoftware.org](https://jrsoftware.org/isdl.php).
- The Release build toolchain (VS 2022, Qt 6.11 MSVC, CMake) from the Windows section above.
- `WinSparkle` (incl. `winsparkle-tool.exe` for key-gen/sign/verify) is fetched by
  CMake — no manual install.

### Build the installer
```powershell
# Default: core + cuda in one installer (large, ~1.8 GB).
pwsh -File packaging\build_installer.ps1
# Small core-only installer — this is the WinSparkle auto-update payload:
pwsh -File packaging\build_installer.ps1 -Components core
# Standalone NVIDIA CUDA runtime (its OWN Inno AppId; same install dir as core):
pwsh -File packaging\build_installer.ps1 -Components cuda
```
The installer is **per-user / no-UAC** (`PrivilegesRequired=lowest`) precisely so the
in-app updater can replace files with no elevation. The version comes from
`src/Core/version.h` (CMake derives `project(VERSION) = MAJOR.MINOR.BUILD` from it —
bump `version.h` only).

### Components & the auto-update payload
- **core** — app + Qt + ORT (incl. the CUDA provider DLLs) + OpenCV + FFmpeg +
  models. Hardware-agnostic; runs CPU or GPU. **This is the only thing
  WinSparkle updates.** (The Spinnaker SDK is *not* bundled — it is delay-loaded and
  discovered at runtime from a user-installed SDK; see §4.)
- **cuda** — the NVIDIA CUDA + cuDNN runtime, packaged under its **own stable AppId**
  so a `-core` auto-update never touches it. It is **not** in the appcast: the app
  detects an NVIDIA GPU (`nvcuda.dll`) and whether the runtime is present
  (`cudnn64_9.dll` next to the exe) and offers **Settings → General → GPU acceleration
  → Install** when a GPU is present but the runtime isn't — so users who add a GPU
  later adapt with no reinstall (design §4.4).

### Releasing (in-app update)
The full per-release procedure — bump version, build the `-core` installer, sign it
offline with the EdDSA key, generate `appcast-win.xml` (`packaging\make_appcast.ps1`),
upload, and publish — is in the
**[Windows Release Runbook](docs/implementation/windows_release_runbook.md)**. The
signing key never leaves your machine (it is not a CI secret); the pinned public half
lives at `src/Resources/keys/pinpoint_release_win_eddsa.pub` and is compiled into the
app. WinSparkle points at the stable
`releases/latest/download/appcast-win.xml` redirect, so releases must be published
**non-prerelease** for it to resolve.

> **Not yet validated end-to-end.** As of this commit the WinSparkle wiring, signing
> scripts, and CI Windows job are authored but the pinned key is still a placeholder
> (the updater is inert until a real key ships) and the CI job + the CUDA `AppId`
> split await a first run / clean-VM validation — see the impl plan.

---

## Testing

PinPoint Studio has eight standalone unit-test suites (57 tests). **None are part of the application build** — the root `CMakeLists.txt` forces `BUILD_TESTING OFF`, so building the app never compiles them. Each suite recompiles only the handful of `.cpp` it needs (there is no test-linkable library except `pinpoint_buffer`) and stubs out anything that would drag in the heavy app dependencies — so the tests configure and build in seconds, independent of whisper/FFmpeg/OpenCV/QML.

There are two ways to build and run them: the **umbrella** (all suites in one configure/build/ctest) and **per-suite standalone** (fastest single-suite iteration). Both use the shared CMake infrastructure in `tests/`. For the architecture, `pp_add_test` reference, and how to add a test or a new suite, see **[docs/developer/testing_developer_guide.md](docs/developer/testing_developer_guide.md)**.

> On Windows/MSVC: the Analysis and Pose suites need `-DOpenCV_DIR=C:/tools/opencv/build` at configure (the app's OpenCV auto-probe is not in the test projects), and ctest needs Qt's `bin` plus OpenCV's `bin` on `PATH` or the test exes fail with `0xc0000135` (DLL not found).

### Run all suites (umbrella)

```bash
cmake -S tests -B build/tests
cmake --build build/tests -j6
ctest --test-dir build/tests --output-on-failure
```

The Qt prefix is auto-resolved per platform; pass `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.11.x/<abi>` only if Qt is installed somewhere non-standard. Eigen is found automatically (explicit `-DPP_EIGEN_DIR`, then the app build's `build/*/_deps/eigen-src`, then a 3.4.0 fetch) — configuring the app once first lets the tests reuse its copy with no extra download.

Equivalent via the presets in `tests/CMakePresets.json` (CMake reads `CMakePresets.json` from the current directory, so run them **from `tests/`**):

```bash
cd tests
cmake --preset tests && cmake --build --preset tests && ctest --preset tests
```

**Sanitizers** are a single knob shared by every suite — use a separate build dir per config:

```bash
cmake -S tests -B build/tests-asan -DPP_SANITIZE="address;undefined"   # ASan + UBSan
cmake -S tests -B build/tests-tsan -DPP_SANITIZE=thread                # ThreadSanitizer
# (or the tests-asan / tests-tsan presets, run from tests/)
```

### Run a single suite (standalone)

Each suite's `src/<Sub>/tests/CMakeLists.txt` also configures on its own — same file, no umbrella required:

```bash
cmake -S src/IMU/tests -B build/imu-tests
cmake --build build/imu-tests -j6
ctest --test-dir build/imu-tests --output-on-failure
```

Filter within any built tree with `ctest --test-dir <build> -R <regex>`.

### Suite catalog

| Suite | Source root | Tests | Coverage |
|---|---|---|---|
| **EventBuffer** | `src/Buffer/tests` | 8 | Lock-free ring, timeline merger, swing window, wait-flag, watchdog, thread-policy + adversarial producer/consumer fuzz. GoogleTest; links the `pinpoint_buffer` library. |
| **Analysis** | `src/Analysis/tests` | 28 | Wrist Motion assessment engine (bands, Tier-1/Tier-2 rules, composite score v2, live adapter); segmentation chain (phase signals/segmenter, metric extractor, swing scorer); wrist-angle math; IMU anatomical calibration; orientation filters (Madgwick + ESKF); WT9011DCL/WT901BLE67 driver frame-parse; 3D-viz binding; ShaftTracker (radial detection, track assembly, kinematics); shot arbiter; session summary; frame decode; `swing.json` round-trip. |
| **Resource profiler** | `src/Core/tests` | 7 | `pp_profiler`/`pp_os_metrics`/`PpStatsLog` + the `profiler_controller` bridge: tier gating, scope interning, timing aggregation, concurrency, OS/GPU metrics graceful degradation, zero-overhead compile-out. GoogleTest; on Windows links `dxgi`. |
| **Gui model/helper** | `src/Gui/tests` | 4 | `TimelineLabels` (label solver, nearest/active, phase names), `SwingSeriesModel` (playhead→row), `ShotListModel::shotSummary`, `ReanalysisController`. Needs Qt6 Qml (`QML_ELEMENT`). |
| **Shot impact-detector** | `src/IMU/tests` | 3 | IMU impact detector truth table (fires once; taps/waggles/swells rejected; refractory; orientation gate; back-dated `est_t`; 100↔200 Hz parity); `ImuIoWorker` thread/EventBuffer contract; ESKF gyro-unit pin. |
| **Calibrated ball-detection** | `src/Pose/tests` | 3 | `ball_model.h` core (model fitting, theta, multi-cue scoring, gain invariance, drift); calibration protocol (round bookkeeping, profile save/load); `BallDetector` throttle contract. Needs OpenCV (core/imgproc/features2d). |
| **Acoustic onset-detector** | `src/Audio/tests` | 1 | Onset detector truth table (click fires sample-accurately; speech/tone/ambient rejected; refractory; back-dating; reverb confirm; absolute amplitude gate). |
| **In-app update** | `src/Update/tests` | 3 | Linux updater pure logic (version compare, AppImage asset selection across x86_64/aarch64, GPG VALIDSIG parse, placeholder-key refusal); `PlatformTarget` arch-token map + a tripwire on the **frozen** macOS appcast filenames (`appcast-mac.xml` must stay unsuffixed; arm64 is a separate file); `UpdateController` state-machine + relaunch session-safety policy + QML state-string contract, driven by a `FakeUpdateBackend`. GoogleTest; the policy test needs Qt6 Qml + Test. |

Framework note: Buffer, Core and In-app update use GoogleTest (fetched automatically); the other five use a self-contained `main()` + `CHECK`/`CHECK_NEAR` (no GoogleTest). `src/Buffer/tests` also builds `latency_benchmark`, intentionally **not** registered with CTest — run it by hand: `./build/tests/Buffer/latency_benchmark` (umbrella) or `./build/buffer-tests/tests/latency_benchmark` (standalone `-S src/Buffer`).

---

## Automatically Downloaded Dependencies

The following are fetched at `cmake ..` time — no manual steps required:

| Dependency | What | Cached size | Condition |
|---|---|---|---|
| ONNX Runtime 1.26.0 | Prebuilt shared library (platform/CUDA matched) | ~10–80 MB | Always |
| ONNX Runtime GenAI 0.13.1 | Prebuilt shared library (local LLM engine; CUDA or CPU variant) | ~10–60 MB | `WITH_ORTGENAI=ON` (default; auto-off on Intel macOS) |
| whisper.cpp v1.7.2 | Built from source | — | Always |
| espeak-ng 1.52.0 | Built from source | — | Only if not found on system |
| libsamplerate 0.2.2 | Built from source | — | Always |
| Eigen 3.4.0 | Header-only (IMU EKF) | ~few MB | Always |
| `ggml-base.en.bin` | Whisper STT model (Hugging Face) | ~148 MB | Always |
| MoveNet Lightning | ONNX pose model (Hugging Face) | ~9 MB | Always |
| MoveNet Thunder | ONNX pose model (Hugging Face) | ~30 MB | Always |
| u2netp | ONNX person segmentation model | ~4.7 MB | Always |
| ViTPose-B wholebody | ONNX whole-body pose model (HuggingFace JunkyByte/easy_ViTPose) | ~330 MB | `WITH_VITPOSE=ON` (default) |
| yt-dlp | Platform binary for YouTube download | ~15 MB | Always |

If a download fails, the affected feature is disabled but the rest of the build continues normally. Re-run CMake to retry failed downloads.

> **Not downloaded at build time:**
> - The local LLM model (Phi-4-mini) is fetched by the app itself on first run — into the per-user app-data directory, and only when a compatible GPU is present.
> - **ViTPose++-L wholebody** (`vitpose-l-wholebody.onnx`, ~1.2 GB) — the "High" motion-capture-quality pose model. Deliberately never built or packaged; the app downloads it on demand (with an explicit size warning) when the user selects the **High** tier in Settings → General, into `AppLocalDataLocation/models/vitpose/`. "High" runs ViTPose++-L; "Low"/"Medium" run the packaged ViTPose-B.

---

## GPU Acceleration Summary by Platform

| Feature | Windows CUDA 12 | Windows CUDA 13 | Linux CUDA 12 | Linux (Intel GPU) | macOS ARM64 | macOS Intel |
|---|---|---|---|---|---|---|
| Whisper STT | Vulkan or CUDA | Vulkan or CUDA | Vulkan or CUDA | Vulkan (Intel ANV) | Metal | CPU |
| Kokoro TTS (ORT) | CUDA | CUDA | CUDA | CPU | CoreML | CPU |
| MoveNet pose (ORT) | CUDA | CUDA | CUDA | CPU | CoreML | CPU |
| Person segmenter (ORT) | CUDA | CUDA | CUDA | CPU | CoreML | CPU |
| Local LLM (ORT GenAI) | CUDA | CUDA | CUDA | Gemini cloud¹ | built-in | Gemini cloud¹ |

¹ Without a local GPU (or on Intel macOS, where ORT GenAI has no prebuilt) the app skips the local Phi-4-mini download and uses the Gemini cloud backend instead.

Vulkan is preferred over CUDA for Whisper because it works across GPU vendors (NVIDIA, AMD, Intel Arc) without needing the CUDA Toolkit. The Intel ANV Vulkan driver covers integrated graphics on Linux (UHD, Iris, Xe); Intel Arc discrete cards also work via Vulkan.

ORT on Linux with Intel integrated graphics runs on CPU — see the note above on why OpenVINO EP is not supported.

---

## Build Options

| Option | Default | Description |
|---|---|---|
| `-DWITH_CUDA=ON/OFF` | ON | Enable CUDA EP for ONNX Runtime. Windows: auto-disabled to the CPU package when no CUDA 12/13 toolkit matches. Linux: always fetches the CUDA-12 GPU package (no version probe — see [Linux GPU notes](#4-gpu-acceleration-optional)) |
| `-DWITH_COREML=ON/OFF` | ON | Enable CoreML EP on macOS ARM64 |
| `-DWITH_VITPOSE=ON/OFF` | ON | Download and build ViTPose-B wholebody pose estimator (~330 MB model) |
| `-DWITH_MEDIAPIPE=ON/OFF` | OFF | Build MediaPipe BlazePose estimator (requires Python 3.10–3.13 + `qai-hub-models`, `torch`, `onnxscript`) |
| `-DWITH_ORTGENAI=ON/OFF` | ON | ONNX Runtime GenAI local LLM engine (auto-off on Intel macOS — falls back to Gemini cloud) |
| `-DPINPOINT_BUILD_TOOLS=ON/OFF` | OFF | Build the SwingLab offline analysis tools (`swinglab_run`) — see below |
| `-DPINPOINT_DEBUG_LEVEL=<n>` | 1 | Log verbosity (2 = startup info + warnings/errors) |
| `-DASSEMBLYAI_API_KEY=<key>` | — | Seed AssemblyAI API key into settings at build time |
| `-DOpenCV_DIR=<path>` | — | Path to OpenCV CMake config (Windows, non-standard location) |
| `-DFFMPEG_DIR=<path>` | — | FFmpeg root for swing export (Windows, non-standard location; must contain `lib/pkgconfig`) |

---

## SwingLab Offline Tools (Optional)

`swinglab_run` replays a recorded swing directory through the **production** analysis pipeline (same sources, same configs) with JSON-injectable tuning parameters and per-frame trace dumps — used for parameter sweeps and regression triage. It is **not built by default**; enable it with `-DPINPOINT_BUILD_TOOLS=ON`:

```bash
cmake .. -DCMAKE_PREFIX_PATH=/path/to/qt/6.11.x/compiler_arch -DPINPOINT_BUILD_TOOLS=ON
cmake --build . --target swinglab_run
```

It shares the app's heavy dependency set (OpenCV incl. videoio, ONNX Runtime, the ViTPose model), so configure the app at least once first to ensure those are present.

---

## Film Tab — YouTube Download

The Film tab uses a bundled **yt-dlp** binary to download YouTube videos to a local cache. No separate yt-dlp installation is needed.

To use YouTube Premium quality, log into YouTube in your browser before downloading — the app reads your browser's cookies. On Linux, Brave is the recommended browser; Chrome and Chromium require the `secretstorage` Python module which is not available in the bundled binary.
