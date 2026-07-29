# PinPoint Studio — macOS In-App Update

*The authoritative design for self-updating the macOS build. This is the **original**
of the pattern the other two mirror: **Sparkle** updating a **Developer ID-signed,
notarized `.app`** delivered inside a **`.dmg`**, advertised by a **Sparkle appcast**,
gated by a **pinned EdDSA (Ed25519) signature**, surfaced as a **prompt-then-install**
flow on launch and from Settings → General. Distribution and the feed are **GitHub
Releases** — the same host the Linux and Windows updaters use. This doc is the
contract; the phased build is
[`../implementation/macos_update_impl.md`](../implementation/macos_update_impl.md).
Read it alongside the Windows design [`windows_update.md`](windows_update.md) — macOS
and Windows are deliberately symmetric (both delegate the update UI to a Sparkle-family
engine), and the Linux design [`linux_update.md`](linux_update.md) for the shared
"GitHub Releases, locally-signed, prompt-then-install" philosophy.*

---

## 1. Goals & non-goals

**Goals**
- Same product behaviour as the Linux/Windows updaters: the app notices a newer
  release, tells the user what changed, downloads **only after confirmation**, verifies
  authenticity against a pinned key, and relaunches into the new build.
- The update engine is **Sparkle 2** — the de-facto standard for non-App-Store macOS
  apps and the framework WinSparkle is a port of. We do **not** hand-roll a
  download/verify/relaunch state machine on macOS (that is exactly what we did on Linux
  *because* there is no Sparkle there; on macOS there is, so the app code shrinks to
  *configuration + a "Check now" button + a relaunch-gate delegate*).
- Distribution and the update feed are **GitHub Releases** — no separate server, same
  repo (`PinPoint-Golf/PinPointStudio`) and the same publish step as Linux/Windows.
- The app is **Developer ID-signed and notarized** (decided). On macOS this is not
  optional polish as on the other platforms — Gatekeeper quarantine and App
  Translocation make a *correctly signed + notarized* bundle a prerequisite for Sparkle
  to update in place at all (§6). It is the macOS analogue of "produce a relocatable
  AppImage", and like that on Linux it is the **80%** of this work.

**Non-goals**
- Background silent auto-install (rejected, as on Linux/Windows — see §5,
  prompt-then-install). Sparkle *can* install on quit silently; we use the
  prompt-on-launch behaviour instead.
- Mac App Store / `pkg` receipts / MDM. Out of scope — the App Store has its own update
  channel and forbids Sparkle; this is the direct-download build.
- A custom QML update UI on macOS. Sparkle owns its native window, exactly as
  WinSparkle does on Windows and as the QML banner is the Linux idiom (§5).
- **A core/cuda component split or a hardware-adaptive runtime sidecar.** The whole
  Windows §4.3–4.4 complexity (a ~1 GB CUDA runtime managed separately) **does not
  exist on macOS**: there is no CUDA. The mac build accelerates via **CoreML /
  Accelerate / Metal**, all OS-resident — so the `.app` is one self-contained,
  hardware-agnostic artifact and the Sparkle enclosure is simply "the whole app". This
  is a real simplification over Windows, not an omission.
- A universal (fat) binary — **still rejected, and now permanently**. The repo's ONNX
  Runtime versions diverge per arch (arm64 1.26 + CoreML + ORT-GenAI vs Intel 1.20.1,
  CPU, no local LLM) because Microsoft stopped publishing Intel macOS prebuilts after
  ORT 1.21 / ORT-GenAI 0.11.4. A single lipo'd binary would have to carry two different
  ONNX majors and two different engine sets behind one `Info.plist`, which no amount of
  packaging cleverness makes coherent. **Two separate DMGs, two separate feeds.**
  `package_macos.sh` refuses a universal bundle outright rather than let one be built
  by accident.

  > **Superseded:** v1 shipped **x86_64 only**, with Apple Silicon running under
  > Rosetta 2 and native `arm64` deferred to GA. Native `arm64` now ships as a second
  > DMG + `appcast-mac-arm64.xml`. Existing Apple Silicon users who installed the Intel
  > build **stay on it** — there is deliberately no Rosetta→native migration hook, which
  > keeps the "each architecture's binary only ever sees its own feed" invariant (§4.1)
  > literally true. They move over by reinstalling, if they ever choose to.

## 2. The cross-platform mapping

| Concern | **macOS (this doc)** | Windows | Linux |
|---|---|---|---|
| Package | **`.app` inside a `.dmg`** | Inno Setup installer (`.exe`) | AppImage (single file) |
| Update engine | **Sparkle 2** | WinSparkle | appimageupdatetool (zsync) |
| Feed / "appcast" | **Sparkle appcast XML** on GitHub Releases (§4.1) | Sparkle appcast XML on GitHub Releases | GitHub Releases JSON |
| Binary transport | **full DMG** (deltas a GA option, §4.4) | full `-core` installer | zsync delta |
| Authenticity | **EdDSA (Ed25519) on appcast + Developer ID codesign** (§6) | EdDSA (Ed25519) on appcast | pinned GPG sig on AppImage |
| First-install trust | **notarization / Gatekeeper** (§6) | (Authenticode, GA) | (none — AppImage is unconfined) |
| UX | **Sparkle's native UI** (§5) | WinSparkle's native UI | PinPoint QML banner |
| Signing custody | **local/offline key** (§6) | local/offline key | local/offline key |

The split worth internalising: macOS is **Windows' twin, not Linux's**. As on Windows,
the engine owns the feed parse, version compare, download, signature verify, progress
UI, and the swap — so `UpdateController` is a **thin façade**, the shared QML
rich-state UI stays **Linux-only**, and "less PinPoint code, not more" is the point.
The **one** place macOS is *harder* than Windows is trust on first contact: Windows v1
tolerates an unsigned installer (SmartScreen warns, the user clicks through, then
EdDSA gates every later update); macOS Gatekeeper is stricter — an un-notarized app is
quarantined and may be **translocated to a read-only randomized path**, which stops
Sparkle from updating in place. So Developer ID + notarization is pulled forward from
"GA polish" (where it sits on Windows) to a **v1 requirement** here (§6).

## 3. Components

```
src/Update/
  mac_sparkle_update.h/.mm     MacSparkleUpdater — thin Qt/Cocoa shim around Sparkle 2:
                               owns an SPUStandardUpdaterController (SPUUpdater +
                               SPUStandardUserDriver + its native UI), exposes
                               checkNow() + setAutomaticChecks(), and implements the
                               SPUUpdaterDelegate relaunch-postpone hook wired to the
                               session guard. Compiled on macOS only (.mm — Obj-C++).
  mac_sparkle_backend.h/.cpp   MacSparkleBackend — the macOS UpdateBackend: owns a
                               MacSparkleUpdater, reports supported + ownsStateMachine()
                               == false, and forwards checkNow()/setAutomaticChecks()/
                               shutdown() to it. Plain C++ (Cocoa stays behind the shim's
                               void* handles), so no test ever compiles Obj-C. The
                               structural twin of WinSparkleBackend.
  update_backend_factory.h/.cpp  makeUpdateBackend() returns a MacSparkleBackend under
                               `Q_OS_MACOS && HAVE_SPARKLE` — the SOLE platform #ifdef.
  update_controller.h/.cpp     UNCHANGED by the macOS port — the shared QML façade (it
                               already delegates to whatever backend the factory returns).
                               The rich Downloading/Verifying/ReadyToRelaunch states never
                               light up here (Sparkle owns that UI).
  (embedded framework)         Sparkle.framework — vendored/fetched, embedded in
                               Contents/Frameworks/, code-signed; carries Sparkle's own
                               Autoupdate/Updater helper that performs the post-quit swap.
  (Info.plist keys)            SUFeedURL (static GitHub latest-release URL, §4.1),
                               SUPublicEDKey (the pinned Ed25519 public key, base64, §6),
                               SUEnableAutomaticChecks (mapped to the user pref, §5).
```

- **Sparkle is an Objective-C framework.** We embed `Sparkle.framework`, link it, and
  drive it from a single `.mm` file via the Sparkle 2 API
  (`SPUStandardUpdaterController` is the batteries-included entry point — it constructs
  the updater, the standard user driver, and the UI). EdDSA is Sparkle 2's native
  signature scheme (`SUPublicEDKey` + the `sign_update`/`generate_keys` tools).
- `UpdateController` is the single QML context property `updateController`, registered
  in `main.cpp` on all platforms. It is **always** a thin façade over an `UpdateBackend`;
  on macOS the factory hands it a `MacSparkleBackend`, on Linux a `LinuxAppImageBackend`,
  on Windows a `WinSparkleBackend`, else an inert fallback. `MacSparkleBackend` is the
  structural twin of `WinSparkleBackend`; the only platform `#ifdef` is the one
  `#elif defined(Q_OS_MACOS) && defined(HAVE_SPARKLE)` arm in `update_backend_factory.cpp`
  (before the macOS port, that arm did not exist and macOS got the inert
  `InertUpdateBackend`, `State::Unsupported`).
- Sparkle is **only meaningful in an installed, signed build.** A dev build run from
  `build/Qt_…-Debug/PinPointStudio.app` has no notarized identity, no `SUFeedURL` it
  should act on, and updating it in place would be wrong. `MacSparkleBackend` detects
  "this is a packaged build" (via `MacSparkleUpdater::isInstalledBuild()`) using a CMake
  `-DPINPOINT_INSTALLED` define baked **only**
  into the release-packaged target (the same mechanism the Windows design uses), and
  stays inert otherwise — the analogue of the Linux `$APPIMAGE`-unset → `devbuild`
  check. (Belt-and-braces: also treat an App-Translocated / read-only bundle path as
  non-updatable and surface guidance rather than fail silently — §8.)

## 4. The feed and the transport

### 4.1 Appcast — Sparkle XML on GitHub Releases

Sparkle consumes a classic **Sparkle appcast** (RSS/XML) at `SUFeedURL`. We host it as
a **release asset** and point Sparkle at GitHub's stable "latest release" redirect —
exactly the WinSparkle arrangement:

```
https://github.com/PinPoint-Golf/PinPointStudio/releases/latest/download/appcast-mac.xml
```

`releases/latest/download/<asset>` always 302-redirects to that asset on the latest
**non-prerelease, non-draft** release — the same GitHub behaviour the Linux and Windows
updaters rely on. So the feed URL is **static and baked into `Info.plist`** (it never
encodes a version); "what is the latest version?" is answered by whatever
`appcast-mac.xml` the newest published release carries.

A release's `appcast-mac.xml` has one `<item>`:

```xml
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>PinPoint Studio</title>
    <item>
      <title>PinPoint Studio v0.1-alpha3</title>
      <sparkle:releaseNotesLink>
        https://github.com/PinPoint-Golf/PinPointStudio/releases/latest/download/release-notes-mac.html
      </sparkle:releaseNotesLink>
      <pubDate>Wed, 18 Jun 2026 09:00:00 +0000</pubDate>
      <sparkle:minimumSystemVersion>12.0.0</sparkle:minimumSystemVersion>
      <enclosure
        url="https://github.com/PinPoint-Golf/PinPointStudio/releases/download/v0.1-alpha3/PinPointStudio-v0.1-alpha3-x86_64.dmg"
        sparkle:version="10003"
        sparkle:shortVersionString="v0.1-alpha3"
        sparkle:os="macos"
        length="612368384"
        type="application/octet-stream"
        sparkle:edSignature="<base64 Ed25519 signature of the .dmg>" />
    </item>
  </channel>
</rss>
```

Read by Sparkle:
- `enclosure@url` → the DMG to download (a **specific** release URL, not `latest/`, so
  the bytes match the signature exactly — §6).
- `sparkle:version` → the **monotonic build number** Sparkle compares against the
  running app's `CFBundleVersion` (§7). **This is the comparison key, not the display
  string.**
- `sparkle:shortVersionString` → the human display version (`v0.1-alpha3`), shown in
  Sparkle's window; matched to `CFBundleShortVersionString`.
- `sparkle:edSignature` → the pinned-key signature Sparkle verifies before extracting
  and installing (§6).
- `sparkle:minimumSystemVersion` → minimum macOS; Sparkle silently skips an item the
  host OS is too old for.
- `releaseNotesLink` (or inline `<description>`) → "what's new", rendered in Sparkle's
  own update window.

> **Channel / arch caveat (same as Linux §7, Windows §4.1).**
> `releases/latest/download/…` resolves to the latest **non-prerelease** release.
> During alpha/beta we therefore **publish releases as non-prerelease** so the macOS
> `latest/download/appcast-mac.xml`, the Windows `appcast-win.xml`, and the Linux
> `gh-releases-zsync … latest` all resolve off the same release. **Architecture** is
> handled the same way a Stable/Beta split would be: `SUFeedURL` is baked at **configure**
> time from `CMAKE_SYSTEM_PROCESSOR` — x86_64 bakes `appcast-mac.xml`, arm64 bakes
> `appcast-mac-arm64.xml` — so each architecture's binary only ever sees its own feed.
> No runtime arch negotiation in the appcast; Sparkle has no arch attribute to negotiate
> with, and one item per feed is the whole mechanism.
>
> **`appcast-mac.xml` is a FROZEN name.** It is baked into the `Info.plist` of every
> Intel install already in the field, and Sparkle re-reads that URL from the *installed*
> bundle — never from anything we publish later. Renaming it for symmetry with the arm64
> name would point every one of them at a 404: they would poll forever, never update, and
> there is no remote fix. **Add feeds; never rename this one.**
>
> Two independent checks enforce the pairing, because a stale CMake cache silently
> produces a binary of one arch carrying the other's feed: `package_macos.sh` cross-checks
> the baked `SUFeedURL` against `lipo -archs` of the built binary before naming the DMG,
> and `make_appcast_mac.sh` re-checks it against the mounted DMG's bytes before its EdDSA
> signature blesses them.

### 4.2 Why an appcast, not the GitHub JSON API

Identical reasoning to Windows §4.2: the Linux updater parses the GitHub JSON itself;
Sparkle expects a Sparkle appcast and parses it for us, so we **generate that XML at
release time** rather than teach Sparkle the GitHub JSON shape. The Linux feed is the
GitHub API; the macOS and Windows feeds are generated XML on the same Releases — all
"GitHub Releases", different representations dictated by each engine.

### 4.3 Transport — the DMG is the whole app (no component split)

Sparkle downloads the `enclosure` DMG, mounts it, validates the `.app` inside, and
swaps it for the running bundle. There is **one** enclosure: the complete, notarized
`PinPointStudio.app` in a DMG. **No core/cuda split exists on macOS** (§1 non-goals):
the mac build has no CUDA runtime — acceleration is CoreML/Accelerate/Metal, resident
in the OS — so the bundle is already hardware-agnostic and there is nothing gigabyte-
sized to factor out. Every user, Intel or (Rosetta) Apple Silicon, updates the same
single artifact. This makes the macOS transport story strictly **simpler** than
Windows: no enclosure-selection question, no adaptive sidecar, no second `AppId`.

The DMG is large (Qt + ORT + OpenCV + FFmpeg + the bundled pose/voice models +
espeak-ng data), but that is a one-time download cost; deltas address the *update* cost
(§4.4) and GitHub's 2 GB/asset limit is comfortable.

### 4.4 Deltas — a clean GA option, not v1

Sparkle has **native binary deltas** (the `BinaryDelta` tool emits a `.delta` from the
previous `.app` to the new one; the appcast lists them under `<sparkle:deltas>`). This
is the macOS analogue of Linux's zsync — a code-only point release ships a few MB
instead of the whole bundle, because the bundled models/ORT/Qt are byte-identical
across releases. **v1 ships full DMGs** (simplest correct path, mirroring Windows
shipping a full installer); deltas are a GA add that requires keeping the prior
release's `.app` around at release time to diff against, and listing the extra
`<sparkle:deltas>` enclosures in `appcast-mac.xml`. The client needs **no** change —
Sparkle picks a delta automatically when one matching the installed version is offered,
and falls back to the full DMG otherwise.

## 5. UX — prompt-then-install (Sparkle's native UI)

**Sparkle owns the update UI on macOS, exactly as WinSparkle does on Windows.** We do
**not** drive a custom QML flow: the standard user driver (`SPUStandardUserDriver`)
keeps "update available → release notes → download progress → install & relaunch"
inside the framework. Re-skinning that in QML is precisely the effort the "use Sparkle"
decision exists to avoid. So:

**A. Launch check (passive).** Gated on the existing `appSettings.checkForUpdates`
pref, mapped 1:1 to Sparkle's `automaticallyChecksForUpdates` (and the
`SUEnableAutomaticChecks` Info.plist default). With it on, Sparkle checks shortly after
launch (and on its interval, ≥ 1 h) and shows **its own** "update available" window
**only when** an update exists — never nags when up to date. This replaces the Linux
QML launch banner on macOS; the product behaviour ("checks on launch, never downloads
without confirmation") is identical, and the existing toggle copy in `GeneralPanel.qml`
already fits.

**B. Settings → General (active).** The **Version row** in `GeneralPanel.qml`
(`objectName: "setting_version"`) is the manual surface, shared with Linux/Windows but
behaving per-platform:
- The existing **"Check for updates automatically"** toggle binds to
  `appSettings.checkForUpdates`; the controller forwards the change to the active
  backend's `setAutomaticChecks()`, which on macOS flips Sparkle's
  `automaticallyChecksForUpdates` (`MacSparkleBackend` → `MacSparkleUpdater`), exactly
  as `WinSparkleBackend` forwards it to WinSparkle.
- The **Version badge + action chip**: on macOS the rich live states
  (`downloading`/`verifying`/`ready`) never appear — those are Sparkle's window. The
  controller's `state` stays `"idle"`, so the QML falls to the `"check"` mode: the chip
  reads **"Check now"** and calls `updateController.checkNow()` →
  `[updaterController checkForUpdates:nil]` (a user-initiated check shows Sparkle's UI
  whether or not an update is found, and ignores any prior "Skip this version"). The
  chip is shown whenever `updateController.supported` (true on an installed, signed
  macOS build).

So `GeneralPanel.qml` needs **no** change beyond what the Windows port already
required — its existing `supported`/`uState` switch (the `pal` palette and the
`updAction` chip, lines ~697–822) already resolves to the neutral "Check now" path when
`state == "idle"`. The Linux-specific badge palette (`downloading N%`, `verifying`,
`restart to update`) simply never triggers on macOS.

### 5.1 Session safety

The single-active-session invariant holds identically. Sparkle asks the host before it
relaunches/installs, via the **`SPUUpdaterDelegate`** hook:

```
- (BOOL)updater:(SPUUpdater *)updater
        shouldPostponeRelaunchForUpdate:(SUAppcastItem *)item
        untilInvokingBlock:(void (^)(void))installHandler;
```

- While `SessionController::running()`, return **YES** and **retain** `installHandler`;
  the update is downloaded and staged but the swap-and-relaunch is **deferred** (the
  user is told to finish the session first, mirroring the Linux "End the session to
  install" guard and the Windows can-shutdown gate).
- When the session ends (`SessionController::runningChanged` → not running), invoke the
  retained `installHandler` — Sparkle then quits the app gracefully and the helper
  performs the swap. If the user just quits normally first, Sparkle installs on the next
  launch's "ready" state regardless.

> **Threading advantage over Windows.** Sparkle's delegate callbacks run on the **main
> thread / main run loop** (it is a Cocoa UI framework), unlike WinSparkle's
> off-main-thread `can_shutdown`/`shutdown_request`. So the macOS guard can read
> `m_session->running()` and touch `QObject`s **directly** — no `std::atomic` mirror,
> no queued-invoke dance (§8). This is one place macOS is *easier* than Windows.

The relaunch itself goes through Sparkle's helper, not `QCoreApplication::quit()` — but
because we postpone until the session is safely ended, the app's normal `aboutToQuit`
teardown (`updateController.shutdownUpdater()` + `eventBuffer.stop()`, already wired in
`main.cpp`) still runs as Sparkle terminates the host before the swap.

## 6. Security model — EdDSA pinning **and** Developer ID / notarization

macOS has **two** layers of trust, and both matter (this is the key asymmetry vs
Linux/Windows, where v1 leans on the pinned signature alone):

**(a) EdDSA signature pinning — the auto-update trust anchor (same model as
Linux/Windows).** Authenticity of each update is a pinned **Ed25519** signature on the
DMG. "Any signature present" is **not** acceptable; the key is pinned in the bundle.

- Generate an Ed25519 key pair with Sparkle's bundled `generate_keys`. The **private
  key never leaves the maintainer's machine and is NOT a CI secret** — identical
  custody rules to the Linux GPG key and the Windows EdDSA key. (`generate_keys` stores
  the private key in the login Keychain by default; export it to an offline file with
  `generate_keys -x private-key.pem` and keep it off GitHub. Note: Sparkle and
  WinSparkle both use raw Ed25519 in the same appcast `edSignature` format, so this key
  *could* be shared with the Windows key — but we keep them **separate per platform**
  for clean rotation and blast-radius isolation, mirroring "separate from the Linux GPG
  key".)
- The **public** half (base64) is pinned in `Info.plist` as **`SUPublicEDKey`** and
  also committed to the repo for transparency, e.g.
  `src/Resources/keys/pinpoint_release_mac_eddsa.pub`, mirroring the Linux `.asc` and
  the Windows `.pub`. The build **refuses to ship / Sparkle refuses to act** while the
  key is the placeholder (the analogue of the Linux all-zero fingerprint and the
  Windows `PLACEHOLDER` guard).
- At release time the maintainer signs the **exact** DMG bytes that will be published
  with `sign_update PinPointStudio-<ver>-x86_64.dmg`, and writes the emitted base64
  into the `sparkle:edSignature` of `appcast-mac.xml`. **Sign the bytes you publish.**

**Client side (the gate is Sparkle's).** Sparkle downloads the DMG and, **before
installing**, verifies its Ed25519 signature against the pinned `SUPublicEDKey`; a
missing/invalid/wrong-key signature is rejected and the update is never installed.
Because the signed artifact is the *whole DMG* (not a delta), there is no
"reconstruct-then-verify" subtlety — Sparkle verifies the downloaded file as-is.

**(b) Developer ID code signing + notarization — the Gatekeeper / in-place-update
prerequisite (macOS-specific, v1).** Decided: the app is signed with a **Developer ID
Application** certificate and the DMG is **notarized + stapled**. This is load-bearing,
not cosmetic:
- **Sparkle requires the update's code signature to match the host's.** For a
  Developer-ID-signed app, Sparkle checks the new `.app` is signed by the **same Team
  ID** before installing — a second, automatic gate on top of EdDSA. An ad-hoc/unsigned
  app weakens this to EdDSA-only.
- **Gatekeeper / App Translocation.** An un-notarized, quarantined app is run from a
  **read-only randomized path** (translocation), from which Sparkle **cannot** update
  in place. Notarization (so the download is not quarantined) is what keeps the bundle
  at a stable, writable location the updater can replace. This is *why* macOS pulls
  signing forward to v1 while Windows defers Authenticode to GA.
- **Custody.** The Developer ID cert + its private key live in the maintainer's login
  Keychain (offline), **not** a CI secret — same posture as the EdDSA key. Notarization
  uses an app-specific password / notarytool credential, also kept off CI for v1
  (signing + notarizing is a local maintainer step, exactly like the Linux/Windows
  local-signing model). Stapling (`xcrun stapler staple`) embeds the notarization
  ticket so first launch works offline.

Net trust chain for an update: **HTTPS** (transport) → **EdDSA pin** (authenticity, the
PinPoint-controlled anchor) → **Developer ID Team-ID match + notarization ticket**
(macOS platform trust + in-place-update viability). All three must hold; any failure →
Sparkle refuses the install (§8).

## 7. Versioning & channels

Sparkle compares the **running app's `CFBundleVersion`** against the appcast's
`sparkle:version`. Today `Info.plist.in` carries **stale hardcoded** values
(`CFBundleShortVersionString` = `0.1`, `CFBundleVersion` = `1`) that are not derived
from `version.h` — this **must** be reconciled, the macOS mirror of the Windows §7
version-unification work.

**Design rule (single source of truth = `src/Core/version.h`):**
- **`Info.plist.in` becomes a `configure_file` template.** CMake already derives
  `project(VERSION) = MAJOR.MINOR.BUILD` from `version.h`
  (`PINPOINT_VERSION_MAJOR/MINOR/BUILD`, CMakeLists.txt lines ~19–39). Substitute those
  into the plist:
  - **`CFBundleShortVersionString`** = the display version — `@PROJECT_VERSION_MAJOR@.@PROJECT_VERSION_MINOR@`
    plus the postfix, i.e. the `PINPOINT_VERSION_STRING` body (`0.1-alpha3`). Shown in
    Sparkle's window; matched to `sparkle:shortVersionString`.
  - **`CFBundleVersion`** = the **monotonic** `PINPOINT_VERSION_BUILD` (`10003`) — the
    integer Sparkle compares, matched to `sparkle:version`. This replaces the literal
    `1`. (string-comparing `0.1-alpha3` vs a clean `0.1` is unreliable; the integer is
    not — same rationale as Windows.)
- The **appcast generator** reads the same `version.h` so the appcast `sparkle:version`
  / `sparkle:shortVersionString` and the built plist **cannot drift** — one bump of
  `version.h` moves the app version, the Info.plist, and the appcast together.

**Compare rule (who decides "newer"):** Sparkle, using the integer `sparkle:version` vs
the running `CFBundleVersion`. Offer only if strictly greater. This is the macOS
analogue of the Linux `compareVersion()` and the Windows WinSparkle compare — delegated
to the engine, not re-implemented.

**Channels:** v1 ships a single implicit channel, selected — as on Linux/Windows — by
publishing releases as **non-prerelease** so `latest/download/appcast-mac.xml`
resolves. A future Stable/Beta split lands with GA as an `appSettings.updateChannel`
pref that points Sparkle at a different appcast asset (Sparkle 2 also has a native
`channel` attribute on appcast items + `SPUUpdaterDelegate.allowedChannels`, either of
which works). The **architecture** dimension uses the same lever: a native `arm64`
build bakes a different `SUFeedURL` (§4.1).

## 8. Threading & failure modes

- **Sparkle runs on the main run loop.** `SPUStandardUpdaterController` is created after
  the main window is up; its checks/downloads happen without us managing threads, and
  its delegate callbacks are **main-thread** (§5.1) — no atomic mirror, no queued
  invoke, unlike WinSparkle. No PinPoint `QTimer` poll loop (unlike the Linux
  `appimageupdatetool` driver) — Sparkle is the loop. Teardown rides the existing
  `aboutToQuit` → `updateController.shutdownUpdater()` (which releases the
  updater controller on macOS), already wired in `main.cpp`.
- **Network down / GitHub 5xx / rate-limit:** Sparkle surfaces a quiet error in its own
  UI for a manual check, and silently no-ops a failed automatic check; it never blocks
  startup. The `SPUUpdaterDelegate` error hook can log to `PpMessageLog` for
  diagnostics.
- **Bad/missing/wrong-key EdDSA signature, or Team-ID mismatch:** Sparkle rejects the
  file and does not install (§6) — treated as hostile, same posture as the Linux/Windows
  gates.
- **Not an installed/signed build (dev tree):** the macOS controller stays inert
  (`supported = false`, badge reads "Development build"), Sparkle is never initialised —
  the analogue of `$APPIMAGE` unset / `PINPOINT_INSTALLED` undefined.
- **App-Translocated / read-only bundle path:** if the app is running from a
  randomized read-only path (un-notarized + quarantined, or run from the DMG without
  copying to /Applications), in-place update is impossible. Detect it (bundle path under
  `/private/var/folders/.../AppTranslocation/`, or a non-writable bundle) and surface
  guidance ("Move PinPoint Studio to Applications to enable updates") rather than fail
  silently — the macOS analogue of the Linux read-only-location handling. Notarization
  (§6) is what prevents this in the normal path.
- **Update interrupted / app killed mid-install:** Sparkle stages the new bundle and
  performs the swap via its helper **after** the host quits; the old bundle remains
  runnable until the swap completes. A partial download is discarded and re-fetched.

## 9. Relaunch / install

Sparkle's install path, with our session guard in the loop:
1. The user accepts the update in Sparkle's window. If a session is live,
   `updater:shouldPostponeRelaunchForUpdate:untilInvokingBlock:` returns **YES** and we
   hold the install block (the user is told to finish first); otherwise we let it
   proceed.
2. When safe (no session, or the session just ended), the held block is invoked. Sparkle
   verifies the EdDSA signature + Developer ID Team-ID (§6), then quits the host — the
   app's `aboutToQuit` teardown runs (controllers torn down, merger stopped).
3. Sparkle's helper (`Autoupdate`/`Updater`, inside the embedded framework) mounts the
   DMG, replaces the `.app` at its existing (writable, non-translocated) location, and
   relaunches `PinPointStudio`.
4. The user lands on the new version.

There is no PinPoint-side file swap (unlike the Linux `rename(2)` over `$APPIMAGE`) —
Sparkle's helper is the swap mechanism, operating on the artifact Sparkle verified.

## 10. What this is **not** doing

- Not re-implementing Sparkle in QML on macOS. Sparkle's native UI is the macOS idiom,
  as WinSparkle's is the Windows idiom and the QML banner is the Linux idiom.
- Not splitting the app into components or managing a hardware sidecar — macOS has no
  CUDA gigabyte; the DMG is the whole, hardware-agnostic app (§4.3).
- Not shipping a universal binary — ever (§1). Native `arm64` now ships as a **second
  DMG + second feed**, not as a fat binary.
- Not migrating existing Apple Silicon users off the Intel build. There is no
  Rosetta→native hook; they stay on x86_64 until they reinstall. A migration would mean
  one install switching feeds mid-life, which is precisely the invariant §4.1 rests on.
- Not delivering binary deltas in v1 — full DMGs; Sparkle deltas are a clean GA add
  (§4.4).
- Not using the Mac App Store update channel (incompatible with Sparkle).
- Not changing the Linux or Windows updaters. The three share only the
  `updateController` QML context property and the "GitHub Releases, locally-signed,
  prompt-then-install" philosophy; their engines are independent.

---

### Appendix A — Sparkle API used

Driven from `mac_sparkle_update.mm` (Sparkle 2, `<Sparkle/Sparkle.h>`), the updater
controller created after the main window is shown:

| Symbol / call | Use |
|---|---|
| `SPUStandardUpdaterController` (alloc/init, `startingUpdater:YES`) | constructs SPUUpdater + SPUStandardUserDriver + native UI |
| `Info.plist SUFeedURL` | static feed URL (GitHub latest-release redirect, §4.1) |
| `Info.plist SUPublicEDKey` | **the pinned trust anchor** — Sparkle won't verify without it (§6) |
| `Info.plist SUEnableAutomaticChecks` / `updater.automaticallyChecksForUpdates` | maps the existing launch-check pref (§5A) |
| `Info.plist CFBundleVersion` / `CFBundleShortVersionString` | compare key + display version (§7) |
| `updater.updateCheckInterval = 86400` | daily background check (≥ 3600 floor) |
| `[updaterController checkForUpdates:nil]` | the Settings "Check now" action (§5B) |
| `SPUUpdaterDelegate updater:shouldPostponeRelaunchForUpdate:untilInvokingBlock:` | session-safety gate (§5.1, §9) |
| `SPUUpdaterDelegate updater:didAbortWithError:` / `…failedToDownloadUpdate:` | optional → log to `PpMessageLog` |

### Appendix B — release artifacts (per GitHub Release, macOS side)

```
PinPointStudio-<ver>-x86_64.dmg        # Intel enclosure:  notarized, EdDSA-signed bytes
PinPointStudio-<ver>-arm64.dmg         # Apple Silicon enclosure: likewise
appcast-mac.xml                        # Intel feed         (FROZEN name — see §4.1)
appcast-mac-arm64.xml                  # Apple Silicon feed
release-notes-mac.html                 # (optional) "what's new", shown in Sparkle's window
```
**Four macOS assets, not two.** Each appcast carries exactly one `<item>`, pointing at the
DMG of its own architecture. `make_appcast_mac.sh --arch <x86_64|arm64>` is run once per
architecture and verifies the enclosure's actual slice, its baked `SUFeedURL`, and its
`LSMinimumSystemVersion` by mounting the DMG — the filename alone is treated as hearsay,
since in the CI path (runbook Path A) those bytes were built elsewhere and downloaded.
`<ver>` = the **`version.h` string / release tag** (e.g. `v0.1-alpha3`) — the asset
filename reflects the tag, while the *internal* `CFBundleVersion` is the monotonic
`PINPOINT_VERSION_BUILD`. `appcast-mac.xml` and the DMG co-exist with the Windows
(`appcast-win.xml`, `*-core.exe`) and Linux (`*.AppImage*`) assets on the **same**
release; each platform's updater reads only its own assets and ignores the others'
(an Intel install only ever looks at `appcast-mac.xml`, an arm64 install only at
`appcast-mac-arm64.xml`).

**`sparkle:minimumSystemVersion` is computed, not chosen.** Homebrew bottles are built for
the *packaging host's* macOS, so the bundle's real floor is the highest `LC_BUILD_VERSION`
minos across everything shipped — typically the runner's OS version, regardless of the
`CMAKE_OSX_DEPLOYMENT_TARGET` we pin. `package_macos.sh` derives that value and stamps it
as `LSMinimumSystemVersion`; `make_appcast_mac.sh` reads it back out of the DMG being
signed. This matters because Sparkle offers any update whose `minimumSystemVersion` the
user's Mac satisfies and then installs it **in place**: an over-optimistic floor does not
decline gracefully, it replaces a working install with one that cannot launch.
