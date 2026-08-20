# Windows Release Runbook

How to cut a Windows release that the in-app updater (WinSparkle) will trust and
offer to users. Design: [`../design/windows_update.md`](../design/windows_update.md).

**The one rule that matters:** the EdDSA signing key never leaves your machine.
GitHub only ever sees the **public** key (committed) and the per-installer
**signature** (inside `appcast-win.xml`). If the private key leaks, anyone can ship a
"trusted" update to every user; if you lose it, you can't ship verifiable updates at
all. Back it up offline.

---

## Do this ONCE (first time only)

You only generate and pin the key a single time, ever (until you deliberately rotate
it). After this, skip straight to the per-release checklist.

1. **Install the tools** (see [`../../BUILDING.md`](../../BUILDING.md)): the GitHub
   CLI (`gh auth login`) and Inno Setup 6 (ISCC). `winsparkle-tool.exe` is fetched by
   CMake — after any build it's under
   `build\**\_deps\winsparkle-src\bin\winsparkle-tool.exe`.

2. **Generate the signing key OFFLINE:**
   ```powershell
   $WS = (Get-ChildItem build -Recurse -Filter winsparkle-tool.exe | Select-Object -First 1).FullName
   & $WS generate-key --file C:\keys\pinpoint_win.key     # KEEP SECRET. BACK UP OFFLINE.
   & $WS public-key   --private-key-file C:\keys\pinpoint_win.key   # prints "Public key: <base64>"
   ```

3. **Pin the public key:** paste just the `<base64>` line (no "Public key:" prefix)
   into `src/Resources/keys/pinpoint_release_win_eddsa.pub`, replacing
   `PLACEHOLDER_UNTIL_P2_KEY_GENERATED`. Commit it.

   > Until a build carrying the real key is in users' hands, the updater is inert by
   > design (it refuses the placeholder). So the **first** release that includes the
   > pinned key is what "bootstraps" auto-update — users must install that one
   > normally; every release *after* it can auto-update. Plan key rollout one release
   > ahead of when you need updates to work.

---

## Do this FOR EACH RELEASE

### 1. Bump the version — `src/Core/version.h` only
Edit these and commit + push:
- `PINPOINT_VERSION_MAJOR` / `PINPOINT_VERSION_MINOR` / `PINPOINT_VERSION_POSTFIX`
  — the human version (e.g. `-alpha3`, or `""` for a clean release).
- **`PINPOINT_VERSION_BUILD`** — the monotonic integer the updater compares. **It must
  be strictly larger than the last release** or WinSparkle won't offer the update.
  Formula in the file: `MAJOR*1_000_000 + MINOR*10_000 + PATCH*100 + prerelease`.

That's the *only* version edit — CMake derives the installer version from it, and the
app's WinSparkle display/build version comes from it too. Nothing else to bump.

### 2. Run the full test suite — ALL must pass (MANDATORY GATE)
**A release MUST NOT be cut while any test is failing or not building.** PinPoint's unit
tests are *not* part of the app build — they are nine standalone CTest suites that the
`tests/` umbrella configures, builds and runs as a single registry (see
[`../../BUILDING.md`](../../BUILDING.md) § Testing). Run this from a Developer (vcvars64)
shell with CMake + Ninja on `PATH`:
```powershell
$Qt  = 'C:/Qt/6.11.1/msvc2022_64'
$OCV = 'C:/tools/opencv/build'                          # Analysis & Pose need OpenCV
$env:PATH = "$Qt/bin;$OCV/x64/vc16/bin;$env:PATH"       # so test exes resolve Qt/OpenCV DLLs
if (Test-Path build/tests) { Remove-Item -Recurse -Force build/tests }  # see the warning below
cmake -S tests -B build/tests -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_PREFIX_PATH=$Qt" "-DOpenCV_DIR=$OCV"
if ($LASTEXITCODE) { Write-Error 'RELEASE BLOCKED — configure failed'; return }
cmake --build build/tests -j 6
if ($LASTEXITCODE) { Write-Error 'RELEASE BLOCKED — build failed'; return }
ctest --test-dir build/tests --output-on-failure -j 6
if ($LASTEXITCODE) { Write-Error 'RELEASE BLOCKED — tests failed' } else { 'ALL SUITES PASSED' }
```
The last line must read `100% tests passed, 0 tests failed out of N` — N grows as suites
and tests are added (121 as of v0.1-alpha11), so treat the *zero failures*, not the count,
as the gate. **If the umbrella fails to configure or build, or any test fails, STOP — fix
it and re-run before you tag.** Do not proceed to the build/sign steps below.

> **⚠ Delete `build/tests` first, and it is not housekeeping — the gate is unsound without
> it.** An incremental configure happily reports `100% tests passed` while running binaries
> built from an older checkout, so the gate certifies code that is not the code being
> shipped. Observed 2026-08-16: a full umbrella run over a pre-existing `build/tests`
> reported 134/134, and deleting it and reconfiguring **at the same commit** immediately
> surfaced a failure. Nothing had changed but the staleness. Deleting costs one rebuild per
> release; not deleting costs a green gate that means nothing.
>
> On this platform it also settles the stale-`CMakeCache.txt` trap described below in one
> step — a bad `OpenCV_DIR` is cached and re-used on every retry until the directory goes.

> **Use the umbrella, never a per-suite loop.** `tests/CMakeLists.txt` enumerates the
> suites, so one added later is picked up by this gate automatically. The hand-rolled loop
> that lived here until v0.1-alpha11 carried a hardcoded list that silently skipped
> **LaunchMonitor** once it existed — a gate that quietly stops covering new code is worse
> than no gate. It also pointed at `src/Buffer`, which stopped being the tests' location
> when they moved to `src/Buffer/tests`; the umbrella builds the `pinpoint_buffer` library
> first and then pulls those tests in, which the loop never did.

> **If configure fails on OpenCV, suspect the path before you suspect the code.** Only the
> Analysis and Pose suites need OpenCV, and there are two traps that compound:
>
> - **The `-D` arguments must reach CMake expanded.** They have arrived as the literal
>   strings `$Qt` / `$OCV` — `find_package(OpenCV)` then looks up a nonsense path and
>   reports the generic "did not find OpenCVConfig.cmake", which reads exactly like a
>   missing OpenCV install. Confirm what CMake actually received:
>   ```powershell
>   Select-String 'OpenCV_DIR|CMAKE_PREFIX_PATH' build/tests/CMakeCache.txt
>   ```
>   Real paths, not `$OCV`. The `-D` arguments are quoted above (`"-DOpenCV_DIR=$OCV"`)
>   precisely so PowerShell expands them before CMake sees them — keep the quotes.
> - **The failure is then CACHED.** A failed lookup writes `OpenCV_DIR-NOTFOUND` into
>   `build/tests/CMakeCache.txt`, so every retry fails identically *even after the
>   argument is fixed*. Delete `build/tests` and configure again, or you are debugging a
>   stale answer. (Under the umbrella this is one cache for all nine suites, so a bad path
>   now blocks the whole gate rather than one suite — and one deletion clears it.)
>
> What must exist on disk is `C:\tools\opencv\build\OpenCVConfig.cmake`. If it is there
> and the cache holds real paths, only then is the suite itself worth investigating.
> (Both traps cost real time during the v0.1-alpha10 gate.)

### 3. Build the `-core` installer (the update payload)
Either let CI do it, or build locally — pick one.

**Option A — CI builds it (recommended).** Push a tag; the `windows` job in
`.github/workflows/release.yml` builds the unsigned `-core` installer and stages it on
a **draft** release:
```bash
TAG=v0.1-alpha3                              # free-form; any tag GitHub accepts
git tag "$TAG" && git push origin "$TAG"     # → triggers the build; wait for it to finish
```
Then download the *exact* bytes CI built (you must sign these, not a rebuild):
```powershell
$TAG = 'v0.1-alpha3'
New-Item -ItemType Directory -Force C:\tmp\rel | Out-Null
gh release download $TAG -R PinPoint-Golf/PinPointStudio -p '*-core.exe' -D C:\tmp\rel
$exe = (Get-ChildItem C:\tmp\rel\PinPointStudioSetup-*-core.exe).FullName
```

**Option B — build locally** (no CI, or you want to build on your own machine):
```powershell
pwsh -File packaging\build_installer.ps1 -Components core
$exe = (Get-ChildItem build\Release-Installer -Recurse -Filter 'PinPointStudioSetup-*-core.exe' |
        Sort-Object LastWriteTime -Desc | Select-Object -First 1).FullName
```

### 4. Sign it + generate the appcast
```powershell
pwsh -File packaging\make_appcast.ps1 -PrivateKeyFile C:\keys\pinpoint_win.key -Tag $TAG -Installer $exe
```
This signs `$exe` with your offline key and writes `appcast-win.xml` next to it (the
feed item: version, enclosure URL, signature, length). Optional: add
`-NotesUrl https://github.com/PinPoint-Golf/PinPointStudio/releases/download/$TAG/release-notes-win.html`
to show "what's new" in WinSparkle's window.

### 5. Verify the signature locally (exactly as the app will)
```powershell
$WS  = (Get-ChildItem build -Recurse -Filter winsparkle-tool.exe | Select-Object -First 1).FullName
$pub = (Get-Content src\Resources\keys\pinpoint_release_win_eddsa.pub -Raw).Trim()
$sig = ([regex]::Match((Get-Content (Join-Path (Split-Path $exe) appcast-win.xml) -Raw),
        'edSignature="([^"]+)"')).Groups[1].Value
& $WS verify --public-key $pub --signature $sig $exe     # must report a VALID signature
```
If this doesn't verify, **stop** — do not publish. (Most common cause: you signed a
rebuild instead of the exact CI bytes.)

### 6. Upload the assets + publish
```powershell
$appcast = Join-Path (Split-Path $exe) 'appcast-win.xml'
# Option A (CI): the -core .exe is already on the draft — just add the appcast:
gh release upload $TAG -R PinPoint-Golf/PinPointStudio $appcast
gh release edit   $TAG -R PinPoint-Golf/PinPointStudio --draft=false --prerelease=false

# Option B (local): create the release with BOTH assets:
gh release create $TAG -R PinPoint-Golf/PinPointStudio `
   --title "PinPoint Studio $TAG" --notes "…release notes…" $exe $appcast
```
**Publish non-draft AND non-prerelease** — `releases/latest/download/appcast-win.xml`
(the URL baked into the app) only resolves to a non-prerelease release.

> `gh release create` (Option B) creates the `v*` tag, which fires `release.yml`. Its
> **`guard` job detects the already-published release and skips the build jobs**, so
> CI won't re-draft your release or overwrite the signed installer — no need to cancel
> the run by hand.

### 7. Build + upload the `-cuda` runtime (MANDATORY — every release)
The app's GPU offer opens **`releases/latest`** and expects the user to find a
`-cuda.exe` there (`src/Update/cuda_runtime_controller.cpp`). So the newest
non-prerelease release must ALWAYS carry one, even when the CUDA payload itself hasn't
changed — otherwise every user who accepts "Enable GPU acceleration" lands on a page
with no GPU installer. (This was missed on v0.1-alpha11 and fixed after the fact.)

**CI cannot build this** — the hosted runner has no CUDA toolkit or cuDNN, and the
install rules are gated on `CUDAToolkit_FOUND` / the cuDNN glob, so a CI-built `cuda`
component would be silently EMPTY. Build it on the dev box:
```powershell
pwsh -File packaging\build_installer.ps1 -Components cuda
$cuda = (Get-ChildItem build\Release-Installer -Recurse -Filter 'PinPointStudioSetup-*-cuda.exe' |
         Sort-Object LastWriteTime -Desc | Select-Object -First 1).FullName
gh release upload $TAG -R PinPoint-Golf/PinPointStudio $cuda
```
It is **unsigned and not in the appcast** — it is a separate Inno product with its own
`AppId` (design §4.3–4.4), fetched manually by the user, never by WinSparkle. Sanity-check
the size against the previous release's `-cuda.exe` (~1 GB); a few-MB file means CMake
didn't find CUDA/cuDNN and the component came out empty.

### 8. Confirm it's live
```bash
gh release view "$TAG" -R PinPoint-Golf/PinPointStudio --json assets \
  --jq '.assets[].name'      # expect: -core.exe, -cuda.exe AND appcast-win.xml
```
On a machine running an **older installed** build: Settings → General → **Check for
updates** (or wait for the launch check) → WinSparkle offers it, downloads the
installer, verifies the signature, and relaunches on the new version — no UAC prompt.

---

## Quick checklist (per release)

- [ ] Bump `PINPOINT_VERSION_BUILD` (+ MAJOR/MINOR/POSTFIX) in `version.h`, commit, push
- [ ] **Run the `tests/` umbrella — `0 tests failed` across all nine suites (mandatory; stop if any fail)**
- [ ] Build the `-core` installer (CI tag push, or `build_installer.ps1 -Components core`)
- [ ] If CI: `gh release download` the exact `-core.exe`
- [ ] `make_appcast.ps1` → signs + writes `appcast-win.xml`
- [ ] `winsparkle-tool verify` the signature — stop if it fails
- [ ] Upload `-core.exe` + `appcast-win.xml`; publish **non-draft, non-prerelease**
- [ ] **Build + upload `-cuda.exe` (`build_installer.ps1 -Components cuda`) — every
      release, local build only; `releases/latest` must always have one**
- [ ] Confirm assets + test an update from an older build

---

## Gotchas & notes
- **Sign the bytes you publish.** In Option A, sign the installer you *downloaded from
  the draft*, never a local rebuild — a rebuild differs byte-for-byte and the signature
  won't verify, so every client rejects the update.
- **`BUILD` must always increase.** It's the only thing WinSparkle compares. If you
  forget to bump it, installed apps see "no newer version".
- **Never publish without `appcast-win.xml`.** No appcast → no feed item. A missing or
  mismatched signature → the installer is rejected and never runs.
- **CUDA is separate (design §4.4), but not optional (step 7).** The `-core` installer
  is the only thing in the appcast, and the CUDA/GPU runtime is offered by the app
  itself based on the hardware it detects — never put the `-cuda` installer in the
  appcast. "Separate" does NOT mean "skippable": the offer links to `releases/latest`,
  so the newest release still has to carry a `-cuda.exe` or the offer dead-ends.
- **Mixed-platform releases are fine.** The same GitHub release can also carry the
  Linux `*.AppImage*` assets; WinSparkle only ever looks at `appcast-win.xml` and the
  Linux updater only looks at its own assets.
- **Rollback:** re-draft or delete the release
  (`gh release edit <tag> --draft=true` / `gh release delete <tag>`) and the updater
  stops offering it immediately.
- **Rotating the key:** generate a new key, pin the new public key, and ship a release
  with it — but remember only users who install that release can verify updates signed
  by the new key. Keep signing with the *old* key until that release has propagated.
