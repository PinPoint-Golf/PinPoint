# macOS Release Runbook

The complete, do-this-from-zero guide to cutting a macOS release of PinPoint Studio:
version bump → build → **code sign** → **notarize** → publish, and (from Stage 2) the
Sparkle EdDSA appcast that drives in-app updates. Design:
[`../design/macos_update.md`](../design/macos_update.md). Siblings:
[`windows_release_runbook.md`](windows_release_runbook.md),
[`linux_release_runbook.md`](linux_release_runbook.md).

> **If this is your first time signing a Mac app, start at Part 0 and do every step in
> order.** Part 0 is once-ever setup; Part 1 is what you repeat for each release. The
> first time, budget ~30–45 min for Part 0 (mostly Apple web UI + one Xcode dialog).

---

## How macOS trust works (read this once)

A Mac app downloaded from the internet must clear **Gatekeeper**, or users get scary
"PinPoint Studio is damaged / can't be opened" warnings — and worse, **App
Translocation** runs the app from a random read-only path where Sparkle can't update it.
Clearing Gatekeeper needs **two** things, both done by you, both kept offline:

1. **Developer ID code signature** — you sign the app + DMG with a *Developer ID
   Application* certificate issued by Apple to your account. Proves "this came from
   Mark Liversedge, team `<TEAMID>`, and hasn't been tampered with."
2. **Notarization** — you upload the signed DMG to Apple; their service scans it for
   malware and issues a **ticket**; you **staple** the ticket into the DMG so it's
   trusted even offline.

A **third** layer applies to **auto-update** (Stage 2, now shipped):

3. **EdDSA (Ed25519) signature** — Sparkle verifies each update against a public key
   *pinned inside the app*. This is independent of Apple; it stops anyone (even with a
   stolen Apple cert) from pushing an update unless they also have your EdDSA private key.

> **Where we are now:** both stages have shipped. The in-app updater (Sparkle) is embedded,
> the offline EdDSA key is generated and its public half is committed + pinned, and
> `v0.1-alpha3` is published with the Sparkle-capable signed DMG **and** `appcast-mac.xml`.
> The **[Stage 2]** EdDSA / appcast steps below are therefore now a **required part of every
> release**, not optional. (The one-time key generation in 0.6 is already done — it's kept
> below for reference / disaster recovery.)

---

## Part 0 — One-time setup (do this once, ever)

### 0.1 Apple Developer Program ✅
You've enrolled ($99/yr individual). That gives you a **Team ID** and the right to issue
a Developer ID certificate. (Individual account = you are the "Account Holder", which is
required to create Developer ID certs.)

### 0.2 Create your "Developer ID Application" certificate

This certificate + its private key live in your **login keychain**. The private key is
generated *on your Mac* and never leaves it — Apple only signs the public half.

**Method A — Xcode (recommended; you have Xcode installed):**
1. Open **Xcode** → menu **Xcode ▸ Settings…** (⌘,) → **Accounts** tab.
2. Click **+** (bottom-left) → **Apple ID** → sign in with your developer Apple ID.
3. Select your team in the list → click **Manage Certificates…**
4. Click the **+** (bottom-left of the sheet) → choose **Developer ID Application**.
5. It appears in the list with today's date. Done — close the dialog. Xcode has created
   the cert **and** installed it + its private key into your login keychain.

> If **Developer ID Application** is greyed out / missing from the **+** menu: your Apple
> ID isn't recognised as the Account Holder, or the membership is still activating. Wait a
> few minutes after enrolment, or use Method B.

**Method B — Developer portal + manual CSR (fallback):**
1. **Keychain Access** → menu **Keychain Access ▸ Certificate Assistant ▸ Request a
   Certificate From a Certificate Authority…**
   - User Email: your Apple ID email · Common Name: "Mark Liversedge" ·
     **Saved to disk** (not "emailed") · check **Let me specify key pair information** →
     **2048 bits, RSA**. Save the `.certSigningRequest` file.
2. Go to **developer.apple.com/account** → **Certificates, IDs & Profiles** →
   **Certificates** → **+** → **Developer ID Application** → Continue → upload the
   `.certSigningRequest` → Continue → **Download** the `.cer`.
3. **Double-click the downloaded `.cer`** to install it into your login keychain (it pairs
   with the private key Keychain Access made in step 1).
4. **Install Apple's intermediate CA** (the manual path skips what Xcode does
   automatically — without it the cert shows as *not trusted* / "0 valid identities").
   Find your cert's issuer, then fetch the matching intermediate from
   <https://www.apple.com/certificateauthority/>:
   ```bash
   # see the issuer (e.g. "Developer ID Certification Authority, OU=G2"):
   security find-certificate -c "Developer ID Application" -p | openssl x509 -noout -issuer
   # G2 issuer → fetch + install the G2 intermediate:
   curl -fsSL -o /tmp/DeveloperIDG2CA.cer https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer
   security import /tmp/DeveloperIDG2CA.cer -k ~/Library/Keychains/login.keychain-db
   ```

**Verify either way** (this is the check the build script does too):
```bash
security find-identity -v -p codesigning | grep "Developer ID Application"
```
You should see one line like:
`1) ABCD…1234 "Developer ID Application: Mark Liversedge (TEAMID9999)"`

### 0.3 BACK UP the certificate + private key — do NOT skip

A Developer ID certificate **cannot be re-downloaded with its private key**. If you lose
this Mac or the keychain, you lose the ability to sign — and Sparkle/macOS treat updates
signed by a *different* cert as a different app. Back it up now:
1. **Keychain Access** → **login** keychain → **My Certificates**.
2. Right-click **"Developer ID Application: … (TEAMID)"** → **Export…** → save as
   `pinpoint_developer_id.p12`, set a strong password.
3. Store the `.p12` + its password somewhere offline (password manager / encrypted drive).
   **Never commit it. It is not a CI secret.**

#### 0.3a The two signing Macs — one per architecture

Releases are built on **two physical Macs, each native to its own architecture** — there
is no cross-build, so each DMG is produced where its slice runs:

| Mac | builds & signs | macOS floor it sets |
|---|---|---|
| **Intel MacBook Pro** | `x86_64` DMG + `appcast-mac.xml` | whatever macOS the MBP runs (usually older → lower Intel floor, which is good) |
| **M4 Mac mini** | `arm64` DMG + `appcast-mac-arm64.xml` | the mini's macOS (26.x) |

> **Intel is on a clock.** Apple is winding down Intel Mac support, and GitHub's
> `macos-15-intel` runner is the last Intel image (retiring August 2027). Expect the
> `x86_64` leg — and this MBP's role — to be dropped in the next couple of years; at that
> point the `appcast-mac.xml` feed goes *frozen*, not broken (existing Intel installs keep
> polling a feed that simply stops gaining entries). Keep publishing both until then.

Each Mac needs the SAME signing identity and the SAME EdDSA key, but its OWN
machine-local notarytool profile (see the per-machine list at the end of this section).
The Intel MBP is almost certainly where the cert was first created, so it likely already
has the identity — verify with `security find-identity -v -p codesigning` before importing
anything.

**When setting up EITHER Mac: import the backed-up `.p12` from 0.3. Do NOT create a new
certificate.** This is the single most damaging wrong turn in this runbook, because issuing
a fresh cert appears to work perfectly and breaks things you will not notice for weeks:

- macOS keys **TCC permission grants** (camera, microphone, Bluetooth, speech) to the
  app's *designated requirement*, which names the signing certificate. A new cert silently
  resets every one of those grants for existing users.
- Sparkle treats an update signed by a different Developer ID as a **different app**, so
  in-place updates for everyone already installed stop working.

> **The `.p12` has the leaf cert only, not the issuing CA.** After import you may see
> `1 identity imported` yet `security find-identity -v -p codesigning` reports
> **`0 valid identities`**. That is not a bad import — the leaf cannot chain to Apple Root
> without the **Developer ID Certification Authority (G2)** intermediate, and codesign will
> not use an identity it cannot fully validate. Install the intermediate (public cert, no
> desktop session needed):
> ```bash
> curl -fsSLO https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer
> security import DeveloperIDG2CA.cer -k ~/Library/Keychains/login.keychain-db
> security find-identity -v -p codesigning     # now shows 1 valid
> ```
> `setup_release_signing.sh` does this automatically when the intermediate is absent; the
> manual steps are here for when you are diagnosing by hand. Seen on the M4, 2026-07-29.

**`tools/setup_release_signing.sh` does all of this** — import, the G2 intermediate,
partition list, and the notarytool profile — reading both passwords interactively so
neither is written to disk, passed as `argv` (where `ps` would show it), or echoed:

```bash
tools/setup_release_signing.sh --p12 ~/certs/pinpoint_developer_id.p12
```

**Run it from Terminal.app on the Mac** (screen share / VNC is fine). It refuses to run
over SSH, because the login keychain unlocks only by prompting and there is nothing to
prompt with in launchd session `Background` — codesign there dies
`errSecInternalComponent`. That is a property of the *login keychain*, not of codesign:
the dev cert signs fine headlessly because it lives in a dedicated keychain
(`tools/setup_dev_signing.sh`), a trade deliberately **not** extended to the distribution
cert.

The manual equivalent, if you would rather see each step:
```bash
security import ~/certs/pinpoint_developer_id.p12 -k ~/Library/Keychains/login.keychain-db \
  -T /usr/bin/codesign -T /usr/bin/security
# Let codesign use it without an interactive prompt on every signature:
security set-key-partition-list -S apple-tool:,apple:,codesign: \
  -s -k "<login-keychain-password>" ~/Library/Keychains/login.keychain-db
security find-identity -v -p codesigning   # must list "Developer ID Application: …"
```
**What each signing Mac must have** (the certificate does NOT carry the last two):

1. **The Developer ID identity + G2 intermediate** — imported from the `.p12`
   (`setup_release_signing.sh` does both). Verify: `security find-identity -v -p codesigning`
   shows `1 valid`.
2. **Its own notarytool profile** — the stored profile is **machine-bound**; the M4's
   profile does nothing for the MBP and vice versa. Run 0.5 (or
   `setup_release_signing.sh --skip-cert`) on each Mac. The app-specific password itself is
   **account-bound**, so the same `xxxx-xxxx-xxxx-xxxx` works on both — reuse your saved
   one; you do not need to generate a second. (You can, if you prefer a per-machine one —
   generating another does not revoke the first.) **Apple silently revokes every
   app-specific password when you change your Apple ID password**, so if a release ever
   fails to notarize with `unable to authenticate`, regenerate and re-run 0.5 on that Mac.
3. **The EdDSA private key** (`pinpoint_release_mac_eddsa_PRIVATE.pem`) — copied to the Mac,
   for the appcast step (1.7). BOTH feeds are signed by the same key (the app pins one
   public key for both arches), so both Macs carry the identical `.pem`.

> **Check the EdDSA key still matches the pin before you need it.** The private key is
> useless if it does not pair with `src/Resources/keys/pinpoint_release_mac_eddsa.pub`,
> which is compiled into every shipped app as `SUPublicEDKey` — signatures from a
> mismatched key are rejected by every installed client, and you would only find out at
> the update-offer stage. Sign anything and verify it round-trips:
> ```bash
> SU=$(find build -name sign_update -type f | head -1)
> echo test > /tmp/t.bin
> SIG=$("$SU" /tmp/t.bin -f ~/certs/pinpoint_release_mac_eddsa_PRIVATE.pem -p)
> "$SU" --verify /tmp/t.bin "$SIG" -f ~/certs/pinpoint_release_mac_eddsa_PRIVATE.pem
> ```
> Verified matching on 2026-07-29 for the key in `~/certs`.

> **Keep the distribution cert in the LOGIN keychain.** The project also has a throwaway
> self-signed *development* cert living in a dedicated keychain with its password on disk
> (`tools/setup_dev_signing.sh`) so that dev builds can sign unattended. Do **not** extend
> that trick to the Developer ID cert: a password sitting next to the key on disk is
> exactly the posture this runbook keeps the cert offline to avoid. Release signing runs
> in a desktop session, deliberately.
>
> Note that `codesign` cannot reach the login keychain from an SSH session
> (`errSecInternalComponent`) — it needs the Aqua session. Signing is therefore a
> sit-at-the-Mac step, or one driven into the desktop session via a LaunchAgent.

### 0.4 Note your Team ID
It's the `(TEAMID9999)` in the identity name above, and on
**developer.apple.com/account** → **Membership**. You'll use it for notarization. Below,
`<TEAMID>` means this value and `<APPLE_ID>` means your developer Apple ID email.

### 0.5 App-specific password + notarytool credentials
Notarization logs in to Apple as you. Don't use your real password — make an
**app-specific password**:
1. Go to **appleid.apple.com** → **Sign-In and Security** → **App-Specific Passwords** →
   **+** → label it `pinpoint-notary` → copy the generated `xxxx-xxxx-xxxx-xxxx`.
2. Store it in your keychain as a reusable **notarytool profile** (so you never type it
   again, and it stays out of scripts):
   ```bash
   xcrun notarytool store-credentials pinpoint-notary \
     --apple-id "<APPLE_ID>" --team-id "<TEAMID>" --password "xxxx-xxxx-xxxx-xxxx"
   ```
   Now any `notarytool` call can use `--keychain-profile pinpoint-notary`.

> Run this one yourself (it contains the password). In this session you can prefix it with
> `! ` so it runs in your terminal and the secret never enters the chat.

### 0.6 [Stage 2] EdDSA signing key + pin the public key — ✅ DONE (kept for reference)
The release key was generated on 2026-06-18; the public half is committed to
`src/Resources/keys/pinpoint_release_mac_eddsa.pub` and CMake bakes it into the bundle's
`Info.plist` (`SUPublicEDKey`). The private half lives in this Mac's **login Keychain**
(default Sparkle account `ed25519`) — `sign_update` reads it automatically — with an offline
backup at `~/pinpoint_release_mac_eddsa_PRIVATE.pem` (**move this to encrypted offline
storage; it is the root of trust and is NOT a CI secret**). `generate_keys` / `sign_update`
ship in `build/macos-release/_deps/sparkle-src/bin/` once CMake has fetched Sparkle.

> You only ever do this once. To re-derive the steps (e.g. on a new machine, restoring from
> the `.pem` backup): `generate_keys` creates/keeps the Keychain key, `generate_keys -x
> <file>` exports the private half, `generate_keys -p` prints the public base64, and
> `generate_keys -f <file>` imports a private key from the backup. **Bootstrap:** the first
> release shipping both the updater and the real key (= `v0.1-alpha3`) lets all *later*
> releases auto-update; users install that baseline manually.

---

## Part 1 — Cutting a release (repeat every time)

### 1.1 Bump the version — `src/Core/version.h` only
Edit and commit + push:
- `PINPOINT_VERSION_MAJOR` / `MINOR` / `POSTFIX` — the human version (e.g. `-alpha4`, or
  `""` for a clean release). Becomes `CFBundleShortVersionString` / the DMG filename.
- **`PINPOINT_VERSION_BUILD`** — the monotonic integer (becomes `CFBundleVersion`, the key
  Sparkle compares). **Must strictly increase every release.** Formula is in the file.

CMake derives the bundle version + DMG name from these — nothing else to edit.

### 1.2 Run the full test suite — ALL must pass (MANDATORY GATE)
**A release MUST NOT be cut while any test is failing or not building.** PinPoint has
seven standalone CTest suites (they are *not* part of the app build — see
[`../../BUILDING.md`](../../BUILDING.md) § Testing). Build and run every one; each must
report `100% tests passed` (OpenCV comes from Homebrew, so no `OpenCV_DIR` needed —
CMake asks for the keg-only **`opencv@4`** by name; the plain `opencv` formula is 5.x
now and is rejected explicitly):
```bash
# Point QT at whichever Qt 6.11.x you actually have installed — `ls ~/Qt` to check.
# A stale version here fails at the first configure, before any test runs.
# JOBS=6 for the same reason package_macos.sh caps there: a bare `-j` is UNBOUNDED,
# and ~30 parallel clang processes exhaust the 16 GB M4 mini and take the machine down
# mid-release. Never use a bare `-j` here.
QT=$(ls -d ~/Qt/6.11.*/macos | tail -1)
JOBS="${JOBS:-6}"
for s in Buffer=src/Buffer Analysis=src/Analysis/tests Audio=src/Audio/tests \
         Core=src/Core/tests Gui=src/Gui/tests IMU=src/IMU/tests Pose=src/Pose/tests; do
  n=${s%%=*}; d=${s#*=}
  cmake -S "$d" -B "build/tests-$n" -DCMAKE_PREFIX_PATH="$QT" \
    && cmake --build "build/tests-$n" --parallel "$JOBS" \
    && ctest --test-dir "build/tests-$n" --output-on-failure \
    || { echo "❌ RELEASE BLOCKED — $n failed"; break; }
done
```
**If any suite fails to build or any test fails, STOP — fix it and re-run before you
tag.** Do not proceed to the build/sign steps below.

### 1.3 Build the DMGs — **there are TWO, each on its own Mac**

A release needs both an `x86_64` and an `arm64` DMG. Because there is no cross-build, each
is produced natively on its own machine (0.3a): **the Intel MBP builds `x86_64`, the M4
builds `arm64`.** Everything from here through 1.7 runs **on each Mac for its own arch**,
and the two sets of assets meet on one draft release in 1.8.

> **Native-per-Mac is the right way to build for release, not a fallback.** Homebrew
> bottles are built for the packaging host's macOS, so a DMG's real minimum-macOS is the
> build host's OS (the tooling computes it and stamps `LSMinimumSystemVersion` — it will
> not let the appcast lie). This is exactly why the Intel MBP matters: it usually runs an
> **older** macOS than the M4, giving Intel users a **lower floor** than a macOS-26 machine
> or even the `macos-15-intel` CI runner would. Build each arch on the oldest healthy Mac
> you have for it.

**Path B — build locally (the release path here).** With that Mac's identity + notary
profile + EdDSA key from Part 0 in place, the packaging script does the whole
build → deploy → relocate → verify → **sign → notarize → staple** → DMG. It builds
**host-native only** — the DMG is for the Mac you are sitting at:
```bash
brew install opencv@4 ffmpeg aravis   # build deps (once) — @4, NOT plain opencv (that is 5.x)
export SIGN_IDENTITY="Developer ID Application: M R LIVERSEDGE (WKLCK77L4U)"  # or omit → auto-detect
export NOTARY_PROFILE=pinpoint-notary
tools/package_macos.sh                # slow the first time
DMG=$(ls -t dist/PinPointStudio-*-$(uname -m).dmg | head -1)
```
The DMG is named from the built binary, so its `-x86_64` / `-arm64` suffix always matches
its contents, and the script aborts if the Sparkle feed baked into the bundle disagrees
with that arch (a build tree reused across arches). With `SIGN_IDENTITY`/`NOTARY_PROFILE`
unset it still builds a valid **unsigned** DMG and says it skipped signing.

Run this on the **Intel MBP** for `x86_64`, and on the **M4** for `arm64`. When Intel is
eventually dropped (0.3a), you simply stop running the MBP half — the `arm64` release is
unaffected.

**Path A — CI builds the unsigned DMGs (fallback / validation).** Pushing a tag runs the
two-leg `macos` matrix (`macos-15-intel` + `macos-15`) and stages both **unsigned** DMGs on
a draft, with `macos-assets-complete` going red if either is missing. Use it to validate
the workflow, or to get a DMG for an arch whose Mac you cannot reach — then sign + notarize
the downloaded bytes exactly as Path B does. It carries the CI runner's (higher) floor, so
prefer a native Path B build for what you actually ship:
```bash
TAG=v0.1-alpha4
git tag "$TAG" && git push origin "$TAG"      # wait for Actions
gh release download "$TAG" -R PinPoint-Golf/PinPointStudio -p '*-x86_64.dmg' -D /tmp/rel
gh release download "$TAG" -R PinPoint-Golf/PinPointStudio -p '*-arm64.dmg'  -D /tmp/rel
```

### 1.4 Smoke-launch the bundle before signing — STOP if it crashes (MANDATORY GATE)
The test suite (1.2) and the relocatability check (1.3's §5 gate) are both **static** — they
never actually load the bundled dylib closure. A **load-time** crash slips past both: e.g. a
stray build-machine `LC_RPATH` left in the binary makes dyld load a *second* copy of OpenMP
(Homebrew's, ahead of the bundled one), and `libomp` aborts in `__kmp_register_library_startup`
("OMP: Error #15") **before `main()` runs**. The app dies instantly on launch. Notarization is
the slow, irreversible step — never spend it on a bundle you haven't watched open.

So launch the freshly-built bundle once, **unsigned**, and confirm the window actually appears:
```bash
tools/package_macos.sh --no-sign            # build + deploy + relocate, no sign/notarize
open build/macos-pkg/PinPointStudio.app     # the main window MUST appear — quit it after a few seconds
```
If it crashes on launch, open the crash report and read the **faulting library** before
re-running:
- `__kmp_register_library_startup` / `__kmp_fatal` / "OMP Error #15" → a duplicate OpenMP
  runtime: a build-machine rpath (or absolute dep) leaked into the bundle. `package_macos.sh`
  now strips these (`strip_build_rpaths`) and its §5 gate fails the build on any stray
  `/usr/local`, `/opt/homebrew`, or build-tree rpath — so a clean re-run should fix it. If the
  gate *didn't* catch it, widen the prefixes it checks.
- "image not found" / dyld → an unrelocated dependency; same §5 gate territory.

Only once it launches cleanly, re-run with your signing env vars (1.5, Path B) to produce the
signed DMG. (Path A / CI DMG: `hdiutil attach` it and launch the `.app` from the mounted
volume the same way before you sign the downloaded DMG.)

### 1.5 What signing + notarizing actually does (so you understand the script)
`tools/package_macos.sh` runs these for you; here's the manual equivalent, useful for
learning and for re-signing a CI (Path A) DMG:
```bash
ID="Developer ID Application: Mark Liversedge (<TEAMID>)"
# (a) sign every nested dylib/framework, then the .app last, with Hardened Runtime:
#     the script signs inside-out using packaging/macos/entitlements.plist.
# (b) sign the DMG itself:
codesign --force --timestamp --sign "$ID" "$DMG"
# (c) notarize — uploads, waits for Apple's scan (~1–5 min), returns Accepted/Invalid:
xcrun notarytool submit "$DMG" --keychain-profile pinpoint-notary --wait
# (d) staple the ticket into the DMG so it's trusted offline:
xcrun stapler staple "$DMG"
```
If notarization says **Invalid**, get the reason: `xcrun notarytool log <submission-id>
--keychain-profile pinpoint-notary` (see Troubleshooting).

### 1.6 Verify — STOP if anything here fails
```bash
spctl -a -t open --context context:primary-signature -vv "$DMG"   # → accepted, source=Notarized Developer ID
xcrun stapler validate "$DMG"                                      # → The validate action worked
codesign --verify --deep --strict --verbose=2 "$DMG"              # → valid on disk
```
All three must pass before you publish. The most common first-time failure is a missing
or wrong entitlement / an unsigned nested binary — see Troubleshooting.

### 1.7 [Stage 2] EdDSA-sign each DMG + generate BOTH appcasts — REQUIRED every release

**Run this once per architecture.** `--arch` selects the DMG, decides which feed the
enclosure is required to point at, and names the output file. Passing the wrong one is
caught, not silently honoured: the script mounts the DMG and compares `lipo -archs` and
the bundle's `SUFeedURL` against what `--arch` claims, and refuses to sign on a mismatch.

```bash
NOTES="https://github.com/PinPoint-Golf/PinPointStudio/releases/tag/$TAG"

# Intel → appcast-mac.xml  (the FROZEN feed name — never rename it)
packaging/make_appcast_mac.sh --tag "$TAG" --arch x86_64 \
  --dmg /tmp/rel/PinPointStudio-$TAG-x86_64.dmg --notes-url "$NOTES"

# Apple Silicon → appcast-mac-arm64.xml
packaging/make_appcast_mac.sh --tag "$TAG" --arch arm64 \
  --dmg /tmp/rel/PinPointStudio-$TAG-arm64.dmg --notes-url "$NOTES"

# Verify each signature against the published bytes before uploading (must exit 0):
SU=$(find build -name sign_update -type f | head -1)
for a in x86_64 arm64; do
  case "$a" in x86_64) FEED=appcast-mac.xml ;; arm64) FEED=appcast-mac-arm64.xml ;; esac
  D=/tmp/rel/PinPointStudio-$TAG-$a.dmg
  SIG=$(sed -n 's/.*edSignature="\([^"]*\)".*/\1/p' "$(dirname "$D")/$FEED")
  "$SU" --verify "$D" "$SIG"; echo "$a verify exit: $?"
done
```
Sign the **notarized** DMG in each case (its bytes are what users download). If signing
from the offline `.pem` instead of the Keychain, add
`--key-file ~/pinpoint_release_mac_eddsa_PRIVATE.pem`.

`sparkle:minimumSystemVersion` is **not** a constant you set here — it is read from the
`LSMinimumSystemVersion` the packaging script stamped into the app inside that DMG, which
is the highest `minos` across everything bundled. If it reads higher than you expect, the
build host's Homebrew bottles are why; rebuild in CI rather than overriding it. `--min-os`
exists as an escape hatch, but overriding it upward-of-reality is how you ship an update
that installs over a working app and then will not launch.

### 1.8 Tag (if not already) + publish to GitHub
**FOUR macOS assets go up: two DMGs and two appcasts.** A release carrying only one
architecture's pair is worse than no release for the other architecture — its feed
resolves to a release with no enclosure it can use.

With the two-Mac split, the two DMGs are produced on **different machines**, so uploading is
a two-step meet-on-a-draft, and **only one machine may create the release** — if both run
`gh release create` they race and one errors. Create the draft once, each Mac uploads its
own pair, then publish once all four are present:

```bash
# ── On the FIRST Mac to finish (say the M4 / arm64): create the DRAFT and upload its pair.
gh release create "$TAG" -R PinPoint-Golf/PinPointStudio \
   --draft --title "PinPoint Studio $TAG" --notes "…release notes…" \
   dist/PinPointStudio-$TAG-arm64.dmg  "$(dirname "$DMG")/appcast-mac-arm64.xml"

# ── On the OTHER Mac (Intel / x86_64): the draft already exists — just add its pair.
gh release upload "$TAG" -R PinPoint-Golf/PinPointStudio \
   dist/PinPointStudio-$TAG-x86_64.dmg  "$(dirname "$DMG")/appcast-mac.xml" --clobber

# ── From either Mac, once ALL FOUR assets are on the draft: confirm, then publish.
gh release view "$TAG" -R PinPoint-Golf/PinPointStudio --json assets --jq '.assets[].name'
#   expect: both DMGs + appcast-mac.xml + appcast-mac-arm64.xml
gh release edit "$TAG" -R PinPoint-Golf/PinPointStudio --draft=false --prerelease=false

# Confirm BOTH feeds resolve before you walk away — a 404 here means silent
# never-updates-again for every install of that architecture:
for f in appcast-mac.xml appcast-mac-arm64.xml; do
  printf '%-24s ' "$f"
  curl -fsIL -o /dev/null -w '%{http_code}\n' \
    "https://github.com/PinPoint-Golf/PinPointStudio/releases/latest/download/$f"
done
```
*(Path A CI variant: the tag push already made a draft holding the two unsigned DMGs —
replace them with your signed, notarized ones and add both appcasts with a single
`gh release upload … --clobber`, then publish as above.)*

*(When Intel is retired: publish just the arm64 DMG + `appcast-mac-arm64.xml`. Leave the
last-ever `appcast-mac.xml` on its release so existing Intel installs keep resolving it —
frozen, not 404.)*

**Publish non-draft AND non-prerelease** — the `latest/download/…` URLs (Stage 2's
`SUFeedURL`) only resolve to a non-prerelease release. The `gh release create` tag fires
`release.yml`, whose `guard` job sees the published release and **skips** CI so it can't
clobber your signed DMG.

### 1.9 Confirm
```bash
gh release view "$TAG" -R PinPoint-Golf/PinPointStudio --json assets --jq '.assets[].name'
```
Download the DMG **in a browser** (so it gets the `com.apple.quarantine` flag a real user
sees), open it, drag to /Applications, launch — it must open with **no** Gatekeeper
warning. (Stage 2: an older installed build offers the update via Settings → Check now.)

---

## Quick checklist (per release)
The two arch columns are done **on their own machines** — `x86_64` on the Intel MBP,
`arm64` on the M4 (0.3a). Do not treat the release as done until both are ticked: a
half-shipped release leaves one architecture's feed pointing at an enclosure-less release.

- [ ] Bump `PINPOINT_VERSION_BUILD` (+ MAJOR/MINOR/POSTFIX) in `version.h`; commit, push
- [ ] **Run all 7 CTest suites — every one `100% tests passed` (mandatory; stop if any fail)**

| step (run on each Mac for its own arch) | `x86_64` — Intel MBP | `arm64` — M4 |
|---|---|---|
| notarytool profile present on this Mac (0.5 / `--skip-cert`) | ☐ | ☐ |
| `tools/package_macos.sh` → build + sign + notarize + staple | ☐ | ☐ |
| **Smoke-launch** the bundle — main window appears, no crash | ☐ | ☐ |
| Verify: `spctl` → Notarized Developer ID, `stapler validate`, `codesign --verify` | ☐ | ☐ |
| `make_appcast_mac.sh --arch <arch>` → EdDSA signature + the right appcast; `--verify` | ☐ | ☐ |
| Sanity-check the advertised `minimumSystemVersion` is the floor you intended | ☐ | ☐ |

- [ ] One Mac creates the draft; the other uploads its pair — **all four** assets present
      (2 DMGs + `appcast-mac.xml` + `appcast-mac-arm64.xml`)
- [ ] Publish **non-draft, non-prerelease**
- [ ] Confirm **both** feed URLs return 200 at `releases/latest/download/…`
- [ ] Confirm: browser-download each DMG, open with no Gatekeeper warning

---

## Troubleshooting (first-timer errors)
- **`security find-identity` shows nothing / "Developer ID Application" greyed out in
  Xcode** → membership still activating, or you're not the Account Holder. Wait, re-open
  Xcode Accounts, or use Method B (portal + CSR).
- **`security find-identity -v` says "0 valid identities" but `-v` omitted shows the cert
  with `CSSMERR_TP_NOT_TRUSTED`** → the Apple **intermediate CA is missing** (typical after
  the manual Method B path). Install it per Method B step 4 above, then re-check — it
  should flip to "1 valid identities found".
- **`errSecInternalComponent` during codesign** → the private key isn't accessible: open
  Keychain Access, confirm the cert in **login ▸ My Certificates** has a disclosure
  triangle revealing a private key. If not, the cert and key got separated (re-do 0.2).
- **notarytool returns `Invalid`** → run `xcrun notarytool log <id> --keychain-profile
  pinpoint-notary`. Usual causes: a nested binary wasn't signed, **Hardened Runtime not
  enabled** (`--options runtime` — the script does this), or `get-task-allow` left on (a
  Debug entitlement). Fix, re-sign, re-submit.
- **`spctl` says "rejected" / "Unnotarized Developer ID"** → the ticket isn't stapled
  (`xcrun stapler staple "$DMG"`), or notarization actually failed (check the log).
- **App opens but immediately crashes after notarization** → a Hardened-Runtime
  restriction. Check Console.app at launch time; you may need to add an entitlement in
  `packaging/macos/entitlements.plist` (e.g. `allow-unsigned-executable-memory`/`allow-jit`)
  and re-sign + re-notarize. Add the *minimum* needed.
- **"damaged and can't be opened" on the test machine** → you skipped notarization or
  stapling, or signed with the wrong identity. Re-verify §1.6.

## Gotchas & key custody
- **Sign the exact bytes you publish.** Sign + notarize the DMG you upload, then (Stage 2)
  EdDSA-sign *that* file. A rebuild differs byte-for-byte and breaks the signature.
- **`PINPOINT_VERSION_BUILD` must always increase** — it's the only thing Sparkle compares.
- **No CUDA / no component split on macOS** — the DMG is the whole, hardware-agnostic app
  (CoreML/Accelerate/Metal). The ViTPose model is bundled, same as the Windows `-core`
  component.
- **Both architectures ship** — `x86_64` (Intel MBP) and `arm64` (M4), separate DMGs and
  separate feeds (0.3a). Intel is time-limited: Apple is retiring Intel Mac support, so the
  `x86_64` leg will be dropped in the next couple of years, at which point `appcast-mac.xml`
  freezes rather than breaks.
- **Custody:** the Developer ID `.p12` (0.3), the app-specific password (0.5), and the
  [Stage 2] EdDSA private key all stay **offline, never CI secrets** — and each of the two
  signing Macs holds its own copy of the notary profile + the EdDSA key. Losing the cert key
  or the EdDSA key is the one thing that breaks future updates — back them up.
- **Rollback:** `gh release edit <tag> --draft=true` (or `--prerelease=true`) immediately
  stops the updater offering it.
