# Linux Release Runbook — cut & sign an AppImage release

How to ship a Linux AppImage release that the in-app updater will trust. **The
release signing key never leaves your machine** — GitHub only ever sees the public
key (committed) and the produced `.sig`. Design: [`../design/linux_update.md`](../design/linux_update.md).

## Facts you need
- **Repo:** `PinPoint-Golf/PinPointStudio` · releases are the updater's feed.
- **Signing key:** ed25519, sign-only, **passphrase-protected**, fingerprint
  **`C15A1C82CE718ED190B7C3C00F677C2EDA4F7BF0`** — lives only in your local GPG
  keyring (back it up offline; if lost you can't ship verifiable updates).
- **Version source of truth:** `src/Core/version.h` → e.g. `v0.1-alpha1`.
  `package_appimage.sh` embeds it in the **AppImage filename**, which is what the
  updater compares. **The git tag is free-form** (GitHub-constrained, e.g.
  `Version0-1-0-alpha1`) — the updater ignores it.
- **A release is live to users only when it is:** published (non-draft) **and**
  non-prerelease **and** carries all three assets: `*.AppImage`, `*.AppImage.zsync`,
  `*.AppImage.sig`. The gate refuses an unsigned release, and drafts/prereleases are
  invisible to the updater — so an in-progress release can't reach users by accident.

## One-time setup
- `gh auth login` (the [GitHub CLI](https://cli.github.com/)).
- Signing key present: `gpg --list-secret-keys C15A1C82CE718ED190B7C3C00F677C2EDA4F7BF0`.
- Build tooling installed (see [`../../BUILDING.md`](../../BUILDING.md) → *Packaging & Release*):
  `linuxdeploy`, `linuxdeploy-plugin-qt`, `appimagetool`, `appimageupdatetool`,
  `zsync`, `libfuse2t64`.

---

## 0. Bump the version
Edit `src/Core/version.h` (`PINPOINT_VERSION_MAJOR/MINOR/POSTFIX`), commit, push.
The new version must be **strictly newer** than what's installed in the wild or the
updater won't offer it.

## 0.5 Run the full test suite — ALL must pass (MANDATORY GATE)
**A release MUST NOT be cut while any test is failing or not building.** PinPoint's unit
tests are *not* part of the app build — they are nine standalone CTest suites that the
`tests/` umbrella configures, builds and runs as a single registry (see
[`../../BUILDING.md`](../../BUILDING.md) § Testing). OpenCV is found from the system, so
no `OpenCV_DIR` is needed:
```bash
QT=~/Qt/6.11.0/gcc_64
rm -rf build/tests \
  && cmake -S tests -B build/tests -DCMAKE_PREFIX_PATH="$QT" \
  && cmake --build build/tests -j 4 \
  && ctest --test-dir build/tests --output-on-failure -j 4 \
  || echo "❌ RELEASE BLOCKED"
```
The last line must read `100% tests passed, 0 tests failed out of N` — N grows as suites
and tests are added (121 as of v0.1-alpha11), so treat the *zero failures*, not the count,
as the gate. **If the umbrella fails to configure or build, or any test fails, STOP — fix
it and re-run before you tag.** Do not proceed to Path A/B below.

> **⚠ `rm -rf build/tests` first, and it is not housekeeping — the gate is unsound without
> it.** An incremental configure happily reports `100% tests passed` while running binaries
> built from an older checkout, so the gate certifies code that is not the code being
> shipped. Observed 2026-08-16: a full umbrella run over a pre-existing `build/tests`
> reported 134/134, and deleting it and reconfiguring **at the same commit** immediately
> surfaced a failure. Nothing had changed but the staleness. Deleting costs a few minutes of
> rebuild once per release; not deleting costs a green gate that means nothing.

> **Use the umbrella, never a per-suite loop.** `tests/CMakeLists.txt` enumerates the
> suites, so one added later is picked up by this gate automatically. The hand-rolled loop
> that lived here until v0.1-alpha11 carried a hardcoded seven-suite list, and silently
> skipped **LaunchMonitor** and **Update** once they existed — a gate that quietly stops
> covering new code is worse than no gate. It also pointed at `src/Buffer`, which stopped
> being the tests' location when they moved to `src/Buffer/tests`; the umbrella builds the
> `pinpoint_buffer` library first and then pulls those tests in, which the loop never did.
>
> **Never use a bare `-j`** — it is unbounded and OOMs the 16 GB box mid-gate.

## Path A — CI builds, you sign locally (recommended)
CI builds a reproducible **unsigned draft**; you sign that exact artifact and publish.

```bash
TAG=v0.1-alpha1            # free-form; any tag GitHub accepts is fine
git tag "$TAG" && git push origin "$TAG"     # → triggers .github/workflows/release.yml
# …wait for the run to finish; it creates a DRAFT release with the .AppImage + .zsync
```

```bash
# 1. Grab the EXACT AppImage CI built (must sign these bytes, not a local rebuild).
mkdir -p /tmp/rel && gh release download "$TAG" -R PinPoint-Golf/PinPointStudio \
  -p '*-x86_64.AppImage' -D /tmp/rel
AI=$(ls /tmp/rel/PinPointStudio-*-x86_64.AppImage)

# 2. Sign it (gpg-agent prompts for your passphrase — it never enters the shell).
gpg --local-user C15A1C82CE718ED190B7C3C00F677C2EDA4F7BF0 \
    --output "$AI.sig" --detach-sign "$AI"

# 3. Verify locally exactly as the app will (ephemeral keyring + pinned fingerprint).
H=$(mktemp -d); PIN=C15A1C82CE718ED190B7C3C00F677C2EDA4F7BF0
gpg --homedir "$H" --batch --import src/Resources/keys/pinpoint_release_pubkey.asc
gpg --homedir "$H" --batch --status-fd 1 --verify "$AI.sig" "$AI" \
  | grep -q "VALIDSIG $PIN" && echo "✅ verifies" || echo "❌ DO NOT PUBLISH"
rm -rf "$H"

# 4. Upload the signature, then publish (un-draft, non-prerelease).
gh release upload "$TAG" -R PinPoint-Golf/PinPointStudio "$AI.sig"
gh release edit   "$TAG" -R PinPoint-Golf/PinPointStudio --draft=false --prerelease=false
```

## Path B — fully local (no CI; key + build both on your machine)
Use when you want a GPU-bundled build (CI is CPU-only) or no CI at all.

```bash
export CMAKE_PREFIX=~/Qt/6.11.0/gcc_64
export SIGN_KEY=C15A1C82CE718ED190B7C3C00F677C2EDA4F7BF0
# Optional GPU bundle (else CPU-only). Spinnaker optional.
export CUDA_LIB_DIR=/usr/local/cuda-12/lib64 CUDNN_LIB_DIR=/usr/lib/x86_64-linux-gnu
tools/package_appimage.sh            # builds + signs all three (agent prompts for passphrase)

TAG=v0.1-alpha1
gh release create "$TAG" -R PinPoint-Golf/PinPointStudio \
  --title "PinPoint Studio $TAG" --notes "…release notes (shown in the in-app banner)…" \
  dist/PinPointStudio-*-x86_64.AppImage \
  dist/PinPointStudio-*-x86_64.AppImage.zsync \
  dist/PinPointStudio-*-x86_64.AppImage.sig
# created non-draft, non-prerelease by default → live immediately.
```
> Scripted/non-interactive signing: set `GPG_PASSPHRASE=…` and the script feeds it
> via gpg loopback. Prefer the interactive agent prompt when you can (keeps the
> passphrase out of your shell/history).

---

## 1. Post-publish check
```bash
gh release view "$TAG" -R PinPoint-Golf/PinPointStudio --json assets \
  --jq '.assets[].name'      # expect: .AppImage, .AppImage.zsync, .AppImage.sig
```
End-to-end: run an *older* installed AppImage → Settings → General → **Check now**
(or wait for the 4 s launch check) → it should offer the new version, download the
delta, verify, and relaunch.

## Gotchas
- **Sign the bytes you publish.** In Path A, sign the AppImage you *downloaded from
  the draft*, never a local rebuild — a rebuild differs byte-for-byte and the
  signature won't verify (and zsync won't match).
- **Never publish without the `.sig`.** Clients fail the gate and surface an error.
  Keep it a draft until the signature is uploaded.
- **Mixed-platform repo is fine.** Windows-only releases (installer `.exe`s) carry no
  AppImage asset; the Linux updater skips them and looks for the newest release that
  has one.
- **Rollback:** delete or re-draft the release (`gh release delete <tag>` /
  `gh release edit <tag> --draft=true`); the updater stops offering it immediately.
