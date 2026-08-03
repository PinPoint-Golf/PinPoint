# PinPoint Studio — macOS Code Signing Developer Guide

**Audience**: Developers building, running, or releasing PinPoint Studio on macOS
**Location**: `tools/setup_dev_signing.sh` + `cmake/DevCodesign.cmake` (debug), `tools/setup_release_signing.sh` + `tools/package_macos.sh` + `packaging/macos/entitlements.plist` (release), `CMakeLists.txt` (wiring)
**Scope**: Why both debug *and* release builds must be signed, the two distinct signing identities, which keychain each lives in and why, how each is wired, and the gotchas
**Status**: Debug signing wired into every macOS build and working headlessly; release signing driven by the packaging script, per-architecture

> This guide deliberately contains **no certificate names, Team IDs, or account
> details**. Wherever a real identity is needed it is shown as a placeholder
> (`<DEV_ID_APPLICATION>`, `<TEAM_ID>`, `<NOTARY_PROFILE>`). The actual values and
> first-time account setup live in `docs/implementation/macos_release_runbook.md`.

---

## Contents

1. [Why macOS builds must be signed](#1-why-macos-builds-must-be-signed)
2. [Two builds, two identities](#2-two-builds-two-identities)
3. [Debug signing — what it's for and how it's wired](#3-debug-signing--what-its-for-and-how-its-wired)
4. [Creating the debug signing identity](#4-creating-the-debug-signing-identity)
4a. [Keychains — the headless-signing problem](#4a-keychains--the-headless-signing-problem)
5. [Release signing — the distribution path](#5-release-signing--the-distribution-path)
6. [The ordering rule: sign last, never touch the bundle after](#6-the-ordering-rule-sign-last-never-touch-the-bundle-after)
7. [Verifying a signature](#7-verifying-a-signature)
8. [Gotchas](#8-gotchas)
9. [Quick reference](#9-quick-reference)
10. [File map](#10-file-map)

---

## 1. Why macOS builds must be signed

On macOS a code signature is not optional polish — it is the **identity** the
operating system uses to recognise an app across launches. Two OS subsystems
depend on it, and they bite at *different* stages:

- **TCC (Transparency, Consent & Control)** — the privacy permission system
  behind camera, microphone, speech recognition and Bluetooth. When you grant a
  permission, macOS stores it keyed to the app's **code-signing identity**, not
  merely its path or bundle id. This affects **debug builds**.
- **Gatekeeper** — the launch-time check that decides whether an app downloaded
  from outside the App Store is allowed to run on *someone else's* Mac. This
  affects **release builds**.

The failure modes are different but the root cause is the same — a missing or
unstable identity:

| Build | Symptom if unsigned / unstable identity | Subsystem |
|-------|------------------------------------------|-----------|
| Debug | Camera/mic/speech/Bluetooth return *Denied* at runtime even though System Settings shows the app as enabled. Every rebuild "forgets" granted permissions. | TCC |
| Release | Other users get *"PinPoint Studio is damaged / from an unidentified developer and can't be opened."* | Gatekeeper |

### Why an unsigned debug build "forgets" permissions

TCC matches a stored grant against the running process's code identity:

- **Properly signed** (stable cert) → grant is keyed to the certificate's
  *designated requirement*, which is constant across rebuilds. Grant sticks. ✅
- **Ad-hoc signed** (`codesign -s -`) → no certificate, so the requirement falls
  back to the **cdhash** (a hash of the actual Mach-O). Every rebuild changes the
  binary → new cdhash → the grant no longer matches → *Denied*. ❌
- **Unsigned** → no durable identity at all. The System Settings row still shows
  "enabled" (that pane is keyed loosely by bundle id), but runtime enforcement
  has nothing to match, so every entitled API returns *Denied*. ❌

Because all four subsystems gate through the same identity check, an unsigned
build fails *all of them at once* — the tell that it's an identity problem, not
four separate permission bugs.

---

## 2. Two builds, two identities

PinPoint uses **two completely separate signing identities**, on purpose:

| | Debug / dev loop | Release / distribution |
|---|---|---|
| **Identity** | Self-signed local cert (default name `PinPoint Dev`) | `Developer ID Application` cert (`<DEV_ID_APPLICATION>`) |
| **Trusted by other Macs?** | No (irrelevant — never leaves your machine) | Yes (issued by Apple) |
| **Notarized?** | No | Yes |
| **Hardened runtime?** | No | Yes |
| **Purpose** | Stable identity so TCC grants persist across rebuilds | Gatekeeper trust so users can launch it |
| **Keychain** | Dedicated `pinpoint-dev.keychain-db`, password on disk | **Login** keychain, unlocked by prompting |
| **Works headlessly (SSH/CI)?** | **Yes** — that is the whole point of the dedicated keychain | **No** — must run in a desktop session |
| **Set up by** | `tools/setup_dev_signing.sh` | `tools/setup_release_signing.sh` |
| **Where wired** | `cmake/DevCodesign.cmake`, run every build | `tools/package_macos.sh`, run at release time |
| **Cost if leaked** | None — disposable | High — it's your distribution key |

**Why not reuse the release cert for debug builds?** The only thing TCC needs is
a *stable* identity — it does not care whether the cert is Apple-trusted. Using
the real `Developer ID` key on every automatic build would expose a sensitive
secret in the build's hot path, drag in the hardened-runtime apparatus the dev
tree doesn't want (see §8), couple the inner loop to release credentials, and buy
nothing in return. Keep the valuable key for distribution; sign dev builds with a
free, disposable, stable local cert.

---

## 3. Debug signing — what it's for and how it's wired

Every macOS build signs the finished `.app` with the local cert automatically.
The wiring lives in `CMakeLists.txt` (inside `if(APPLE)`) and `cmake/DevCodesign.cmake`.

```cmake
set(PINPOINT_DEV_CODESIGN_IDENTITY "PinPoint Dev" CACHE STRING
    "Keychain identity used to sign the dev bundle so TCC permission grants persist across rebuilds")
add_custom_target(PinPointStudio_codesign ALL
    COMMAND ${CMAKE_COMMAND}
        "-DBUNDLE=$<TARGET_BUNDLE_DIR:PinPointStudio>"
        "-DIDENTITY=${PINPOINT_DEV_CODESIGN_IDENTITY}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DevCodesign.cmake")
add_dependencies(PinPointStudio_codesign PinPointStudio_force_plist)
```

Key properties:

- **Runs on every build.** It's a custom target with no output, so it's always
  considered out of date — necessary because the per-build `Info.plist`
  force-copy would otherwise invalidate a prior signature (see §6).
- **Runs last.** It depends on `PinPointStudio_force_plist`, which is the final
  step that mutates the bundle. Signing must come after every bundle write.
- **Non-fatal when the cert is missing.** If `PINPOINT_DEV_CODESIGN_IDENTITY`
  isn't in the keychain, `DevCodesign.cmake` prints setup instructions and the
  build still succeeds (so Linux/CI and un-provisioned Macs aren't broken) — the
  app simply won't retain permissions until the cert exists.
- **Identity-agnostic.** Override the cert with
  `-DPINPOINT_DEV_CODESIGN_IDENTITY=<name>` at configure time.

After the first signed build, **macOS re-prompts for each permission once** (the
identity changed from "unsigned" to the dev cert). Grant them once; they persist
across all future rebuilds. To clear stale grants from a previous unsigned
identity:

```sh
tccutil reset All com.pinpoint-golf.pinpointstudio
```

---

## 4. Creating the debug signing identity

One command, once per machine. No Apple Developer account is required — the cert
is self-signed.

```sh
tools/setup_dev_signing.sh
```

It is idempotent: re-running replaces the keychain and its contents. What it does
is the subject of §4a, and the *why* is worth reading before you reach for
Keychain Access instead.

### Adopting an existing identity — `--p12`

If a signed build on this machine has **already been granted permissions**, do not
mint a fresh cert. TCC keys those grants to the certificate — specifically to a
designated requirement that pins the leaf (`certificate leaf = H"…"`) — so a new
identity silently resets every camera/mic/speech/Bluetooth grant. Export the old
one and adopt it:

```sh
# From Terminal.app ON the Mac — exporting a private key ALWAYS requires GUI
# approval, and no ACL or partition list covers that.
security export -t identities -f pkcs12 -P <pass> -o /tmp/pp.p12

tools/setup_dev_signing.sh --p12 /tmp/pp.p12 --p12-pass <pass>
```

### Confirm it's installed

A self-signed cert is an *untrusted* root, so it is hidden by the `-v` ("valid
only") filter. List **without** `-v`:

```sh
security find-identity -p codesigning | grep "PinPoint Dev"
# → ... "PinPoint Dev" (CSSMERR_TP_NOT_TRUSTED)
```

`CSSMERR_TP_NOT_TRUSTED` is expected and harmless — `codesign` signs with the
cert regardless of trust, and TCC keys the grant to the signature either way.
`DevCodesign.cmake` deliberately queries without `-v` for this reason.

---

## 4a. Keychains — the headless-signing problem

This section exists because the obvious arrangement does not work, and the failure
looks like something else entirely.

### The problem

The login keychain is where a signing identity naturally goes, and it is the one
keychain a headless build **cannot** reach. It unlocks only by **prompting**. A
build driven over SSH — an agent, a remote shell, CI — sits in launchd session
`Background` rather than `Aqua`, has nothing to prompt with, and `codesign` dies
with `errSecInternalComponent`. That error reads like a broken cert or a missing
key; it is neither.

Nothing about `codesign` wants a human. It wants an **unlockable** keychain. That
distinction is the entire fix.

### The fix — a dedicated keychain with its password on disk

`tools/setup_dev_signing.sh` puts the dev identity in
`~/Library/Keychains/pinpoint-dev.keychain-db`, whose password lives at
`~/.config/pinpoint/dev-keychain.pass` (mode 0600) and therefore unlocks with no
interaction at all. This is not a novel trick — it is what every macOS CI system
does, and what fastlane ships as `create_keychain` / `setup_ci`.

Two failure modes those tools learned the hard way are avoided deliberately:

- The keychain is **appended** to the search list and the **default keychain is
  left alone**. Clobbering the host's keychain configuration is a standing
  complaint about `setup_ci`.
- It is ordered **first**.

### Search-list order is not cosmetic

This is the part that costs an afternoon if you miss it. `codesign` resolves
`-s <name>` through the keychain search list **in order**, and its own
`--keychain` flag **does not affect identity lookup at all** — passing it returns
"no identity found", which reads exactly like the cert is missing.

So with the new keychain second in the list, `codesign` keeps finding the *login*
copy of the same identity, which it still cannot unlock, and fails exactly as
before. Putting the dedicated keychain first **and deleting the login copy** is
what actually fixes it. The setup script does both.

### The unlock block in `DevCodesign.cmake`

The keychain re-locks on reboot, so `DevCodesign.cmake` unlocks it before looking
for the identity:

```cmake
set(_pp_kc     "$ENV{HOME}/Library/Keychains/pinpoint-dev.keychain-db")
set(_pp_kcpass "$ENV{HOME}/.config/pinpoint/dev-keychain.pass")
if(EXISTS "${_pp_kc}" AND EXISTS "${_pp_kcpass}")
    file(READ "${_pp_kcpass}" _pp_pw)
    string(STRIP "${_pp_pw}" _pp_pw)
    execute_process(COMMAND security unlock-keychain -p "${_pp_pw}" "${_pp_kc}" ...)
endif()
```

Absent either file this is a **silent no-op**: the build falls through to whatever
the search list already offers, so a GUI-session machine with the cert still in
the login keychain keeps working unchanged. A failed unlock warns rather than
dies — the `codesign` that follows names the real failure better than a guess
here would.

### The password in `ps` — a deliberate trade

The password reaches `security` as an argv element, so it is briefly visible in
`ps` **to this user**. Accepted, because: it guards a self-signed key whose only
power is making local TCC grants stick, it is a throwaway regenerable in one
command, and the file itself is 0600. None of this touches the Developer ID path,
which stays in the login keychain and signs from a real desktop session.

---

## 5. Release signing — the distribution path

Release signing is for **other people's Macs**. It is driven end-to-end by
`tools/package_macos.sh` and requires two things from your **login** keychain:

- a `Developer ID Application` certificate (`<DEV_ID_APPLICATION>`), and
- a stored **notarytool profile** (`<NOTARY_PROFILE>`) holding an app-specific
  password.

### Setting the credentials up on a Mac

```sh
# From Terminal.app on the machine (VNC/screen-share counts) — NOT over SSH.
tools/setup_release_signing.sh --p12 ~/certs/pinpoint_developer_id.p12
```

`setup_release_signing.sh` imports the backed-up `.p12` into the login keychain,
installs the **Developer ID G2 intermediate the `.p12` omits** (without it the
leaf imports but `security find-identity -v` reports 0 valid identities), sets the
partition list so `codesign` never prompts, and stores the notarytool profile. Both
passwords are read interactively — neither lands on disk or in argv. It **refuses
to run headless**, failing with an explanation rather than letting `codesign` die
with `errSecInternalComponent` later.

> **Import, never re-issue.** Minting a fresh Developer ID cert looks like it works
> and quietly breaks two things: every existing user's TCC grants (keyed to a
> designated requirement that names the signing cert) and Sparkle in-place updates
> (a differently-signed build is a *different app* to Sparkle). Always import the
> backed-up `.p12`.

Note the asymmetry with the dev path, and that it is intentional: a password
sitting on disk next to the key is the right trade for a throwaway self-signed
cert (§4a) and the wrong one for a distribution key. So the release identity stays
in the login keychain, and release signing runs in a desktop session.

### Running the release build

```sh
export SIGN_IDENTITY="Developer ID Application: <NAME> (<TEAM_ID>)"   # or omit → auto-detect
export NOTARY_PROFILE=<NOTARY_PROFILE>
tools/package_macos.sh
```

If those env vars are unset the script still produces a valid **unsigned** DMG
(useful for local packaging tests) — each stage skips with a clear message rather
than failing. `--no-sign` forces the skip even when a cert is present. With
credentials set it runs the full chain:

1. **Sign Sparkle's nested code first.** `Sparkle.framework/Versions/B` contains
   XPC services (`Downloader.xpc`, `Installer.xpc`), `Updater.app` and the
   `Autoupdate` helper. The generic dylib/framework sweep does **not** descend into
   them, and the framework's own seal references them — so they are signed
   bottom-up *before* the framework, or notarization rejects unsigned nested code.
   Hardened Runtime, no entitlements (only the main app carries those).
2. **Sign everything else inside-out.** Every nested dylib/framework/plugin
   (including `Contents/PlugIns`, `Contents/Resources` dylibs and the bundled
   `yt-dlp_macos`), then the `.app` last with the entitlements file:
   ```sh
   codesign --force --options runtime --timestamp \
            --entitlements packaging/macos/entitlements.plist \
            --sign "$SIGN_IDENTITY" "$STAGE_APP"
   codesign --verify --deep --strict --verbose=1 "$STAGE_APP"
   ```
3. **Sign the DMG.**
   ```sh
   codesign --force --timestamp --sign "$SIGN_IDENTITY" "$DMG"
   ```
4. **Notarize** — upload to Apple, which scans for malware and issues a ticket:
   ```sh
   xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
   ```
5. **Staple** the ticket into the DMG so it's trusted offline, then validate:
   ```sh
   xcrun stapler staple "$DMG" && xcrun stapler validate "$DMG"
   ```

### One architecture per run

The script builds for the **host** architecture only — `build/macos-release-$(uname -m)`
— and names the output `PinPointStudio-<version>-<arch>.dmg`. A full release is
therefore *two* runs on two machines (arm64 and x86_64), each producing its own DMG
and feeding its own appcast. The script hard-fails if the bundle's baked
`SUFeedURL` does not match its detected architecture, because a stale CMake cache
produces exactly that mismatch and it is otherwise invisible until users get the
wrong updates.

### The entitlements file

`packaging/macos/entitlements.plist` carries a single key:

```xml
<key>com.apple.security.cs.disable-library-validation</key>
<true/>
```

This is **required**: Hardened Runtime turns on library validation, and PinPoint
bundles a lot of third-party code (ONNX Runtime, OpenCV, FFmpeg, Aravis, Qt)
signed by its own vendors rather than by `<TEAM_ID>`. Without the opt-out the
process is killed the moment it loads one. Re-signing every nested binary could
let us drop this later.

`allow-jit`, `allow-unsigned-executable-memory` and
`allow-dyld-environment-variables` are deliberately **omitted** until a launch
crash proves one is needed — add the minimum, never the set.

> **Keep the file comment-free.** `codesign`'s AMFI parser rejects XML comments,
> because an XML comment may not contain a double hyphen and any `--flag`
> rationale would. The rationale lives in `package_macos.sh` beside `ENTITLEMENTS`
> instead. Camera/mic/Bluetooth/speech are `NS*UsageDescription` strings in
> `Info.plist`, not entitlements.

---

## 6. The ordering rule: sign last, never touch the bundle after

**Any write into a `.app` after it is signed invalidates the signature.** macOS
then treats the app as tampered — Gatekeeper rejects the release build, and TCC
rejects the debug build's grants, exactly as if it were unsigned.

This is why both paths sign **last**:

- **Debug**: `PinPointStudio_codesign` depends on `PinPointStudio_force_plist`,
  which force-copies `Info.plist` in as the final bundle mutation. Sign after the
  plist, never before. If you add a new POST_BUILD step that writes into the
  bundle, it **must** run before the codesign step or it will break the seal.
- **Release**: `package_macos.sh` deploys Qt, relocates libraries, and copies the
  plist *before* the signing stage. Resources copied in after signing are the
  classic "notarization succeeded but the app is damaged" cause.

---

## 7. Verifying a signature

```sh
APP=path/to/PinPointStudio.app

# Who signed it, and the sealed identifier:
codesign -dv --verbose=4 "$APP"
#   Debug   → Authority=PinPoint Dev,  Signature=...,  TeamIdentifier=not set
#   Release → Authority=Developer ID Application: …,   TeamIdentifier=<TEAM_ID>

# Structural integrity of the seal (both builds should pass):
codesign --verify --deep --strict --verbose=2 "$APP"

# Release only — Gatekeeper acceptance (needs notarize + staple):
spctl -a -t open --context context:primary-signature -vv "$DMG"   # → accepted, source=Notarized Developer ID
xcrun stapler validate "$DMG"
```

A debug build will **not** pass `spctl` / Gatekeeper (untrusted, un-notarized) —
that's expected and fine; it only needs to satisfy TCC on your own machine.

---

## 8. Gotchas

- **Modifying the bundle after signing invalidates the seal.** The single most
  common cause of both "permissions denied despite signed" and "notarized but
  damaged". Sign last. (§6)
- **`security find-identity -v` hides self-signed certs.** `-v` means "valid
  (trusted) only"; a self-signed root is untrusted, so it won't appear. Query
  without `-v` to find the dev cert. (§4)
- **`errSecInternalComponent` over SSH means "keychain can't be unlocked", not
  "cert is broken".** The login keychain unlocks only by prompting and a
  `Background` launchd session has nothing to prompt with. Use the dedicated dev
  keychain. (§4a)
- **`codesign --keychain` does not affect identity lookup.** It resolves `-s <name>`
  through the *search list*, in order. Passing `--keychain` returns "no identity
  found" and sends you hunting for a missing cert that is right there. Fix the
  search-list order instead. (§4a)
- **A duplicate identity earlier in the search list wins — and fails.** Leaving the
  old login-keychain copy in place means `codesign` keeps picking the one it can't
  unlock. The setup script deletes it. (§4a)
- **OpenSSL 3 PKCS12 won't import into the keychain by default.** `security
  import` fails with *"MAC verification failed (wrong password?)"*. Two fixes are
  needed together: pass `-legacy` to `openssl pkcs12 -export` (3DES/RC2 + SHA1
  MAC, which Apple's importer understands), **and** use a non-empty password —
  the importer also chokes on a passwordless PKCS12 MAC. (`setup_dev_signing.sh`
  handles this; it bites if you roll your own.)
- **A Developer ID `.p12` does not contain the CA chain.** The leaf imports fine
  and `security find-identity -v` then reports **0 valid identities**, which looks
  like a bad export. Install the Developer ID G2 intermediate too —
  `setup_release_signing.sh` does. (§5)
- **Never re-issue the Developer ID cert.** It resets every installed user's TCC
  grants and breaks Sparkle in-place updates, because both key off the signing
  identity. Import the backup. (§5)
- **Sparkle's nested code needs signing before the framework is sealed.** The XPC
  services, `Updater.app` and `Autoupdate` are not reached by a
  dylib/framework sweep, and the framework's seal references them — notarization
  rejects the result otherwise. (§5)
- **Hardened Runtime + library validation rejects the bundled Qt.** Qt frameworks
  are signed by Qt/Apple, not your Team ID, so a hardened-runtime app refuses to
  load them unless `com.apple.security.cs.disable-library-validation` is set.
  This is why release uses an entitlements file and debug skips hardened runtime
  altogether. (§5)
- **TCC keys grants to the signing identity.** Changing the identity
  (unsigned → signed, or swapping certs) makes macOS re-prompt. After first
  enabling signing, expect one round of prompts, then persistence. Clear stale
  grants with `tccutil reset All com.pinpoint-golf.pinpointstudio`. (§3)
- **Debug and release are distinct TCC identities.** Granting permission to the
  signed release app does not grant it to your dev build, and vice versa — they
  have different designated requirements.
- **Bundle id must stay constant** (`com.pinpoint-golf.pinpointstudio`). It is the
  coarse key for the System Settings row and part of the designated requirement;
  changing it orphans every existing grant.

---

## 9. Quick reference

| Task | Command |
|------|---------|
| Set up dev signing (once per Mac) | `tools/setup_dev_signing.sh` |
| Adopt an existing dev identity | `tools/setup_dev_signing.sh --p12 <file> --p12-pass <pass>` |
| Set up release signing (desktop session only) | `tools/setup_release_signing.sh --p12 <file>` |
| List code-signing identities (incl. untrusted) | `security find-identity -p codesigning` |
| Show the keychain search list and its order | `security list-keychains -d user` |
| Unlock the dev keychain by hand | `security unlock-keychain -p "$(cat ~/.config/pinpoint/dev-keychain.pass)" ~/Library/Keychains/pinpoint-dev.keychain-db` |
| Check whether you are in a desktop session | `launchctl managername`  → `Aqua` (GUI) or `Background` (SSH) |
| Override dev cert name | configure with `-DPINPOINT_DEV_CODESIGN_IDENTITY=<name>` |
| Re-sign dev bundle manually | `cmake -DBUNDLE=<app> -DIDENTITY="PinPoint Dev" -P cmake/DevCodesign.cmake` |
| Clear stale permission grants | `tccutil reset All com.pinpoint-golf.pinpointstudio` |
| Inspect a signature | `codesign -dv --verbose=4 <app>` |
| Verify seal integrity | `codesign --verify --deep --strict <app>` |
| Build signed release DMG (host arch) | `SIGN_IDENTITY=… NOTARY_PROFILE=… tools/package_macos.sh` |
| Package without signing | `tools/package_macos.sh --no-sign` |
| Gatekeeper check (release) | `spctl -a -t open --context context:primary-signature -vv <dmg>` |

---

## 10. File map

| Path | Role |
|------|------|
| `tools/setup_dev_signing.sh` | Creates/adopts the dev identity in the dedicated `pinpoint-dev` keychain; puts it first in the search list and removes the login copy. Idempotent. |
| `cmake/DevCodesign.cmake` | Unlocks that keychain, then signs the dev `.app`; non-fatal if either is absent |
| `CMakeLists.txt` (`if(APPLE)`) | `PinPointStudio_codesign` target + `PINPOINT_DEV_CODESIGN_IDENTITY`, ordered after `PinPointStudio_force_plist` |
| `~/Library/Keychains/pinpoint-dev.keychain-db` | The dev identity's home (not in the repo) |
| `~/.config/pinpoint/dev-keychain.pass` | Its password, 0600 (not in the repo) |
| `tools/setup_release_signing.sh` | One-time release credential install: Developer ID `.p12` + G2 intermediate + partition list + notarytool profile. Desktop session only. |
| `tools/package_macos.sh` | Release build → deploy → verify → sign → DMG → notarize → staple, for the host arch |
| `packaging/macos/entitlements.plist` | Hardened-runtime entitlements (library-validation opt-out). Comment-free by necessity. |
| `Info.plist.in` | Bundle id + `NS*UsageDescription` strings TCC shows in its prompts |
| `docs/implementation/macos_release_runbook.md` | First-time account/cert/notary setup, the real identity values, and the per-arch release steps |

---

*Related: `docs/implementation/macos_release_runbook.md` (release account setup),
`docs/design/macos_update.md` (Sparkle auto-update), and the "Critical gotchas"
section of `CLAUDE.md`.*
