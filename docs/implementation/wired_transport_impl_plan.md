# Wired transport for PPCP — implementation plan and tracker

**This is the tracker.** Maintain it here. The session is cleared after Phase 0, so
this file plus the design doc is the whole handover.

**Design:** `PinPointStudio/docs/design/wired_transport_design.md` (commit `d9582c3`).
**Repos:** `PinPointStudio` (host), `PinPointCapture` (device), `libppcp` (protocol).

---

## Context

PPCP links a phone to Studio over WiFi today. A cable would improve clock
alignment, and the reason is arithmetic rather than preference: `ppcp_sync.c:251-260`
floors `offset_sigma` at **half the minimum RTT**, and `:262-271` floors
`skew_sigma` at `offset_sigma·√2/span`. Time to any target is therefore linear in
min RTT, and the probe cadence does not appear at all — which is why seven cadence
configurations were measured and reverted. The transport is the only lever.

The consumer that decides whether this matters is `PpcpShotBridge`, which rejects a
conversion whose sigma exceeds `maxConversionSigmaNs` = **5 ms** (*"a frame at 200
fps"*, `ppcp_shot_bridge.h:112`). Above it, a phone's Candidates are not arbitrated.

**Phase 0 exists because the host's own I/O currently dominates that number.**
`libppcp` stamps `t4` with `ppcp_clock_read()` at `ppcp_peer.c:2675`, *inside*
`ppcp_peer_feed()`, which `PpcpHostPeer::pump()` calls — and `pump()` runs only from
a 20 ms `QTimer` (`ppcp_host_service.cpp:127`). There is no `QSocketNotifier` on any
PPCP channel; notifiers exist only for the DNS-SD browser and advertiser fds
(`:1109`, `:1222`). So `t4` is stamped when a tick gets round to the reply.

⚠ **The code already claims otherwise.** `ppcp_host_service.cpp:122` reads
*"pump() is driven by bytes being ready; tick() by TIME PASSING"*. The second half is
true and the first is not. Phase 0 makes the comment true.

✅ **Correction to the design doc.** §7.1 proposes calling `ppcp_peer_sync_observe()`
*and* adding a notifier. Because `t4` is read inside `feed()`, **the notifier alone is
sufficient** and `sync_observe()` is not needed. Phase 0 is smaller than the doc says;
fix §7.1 when the measurement lands.

**Why Phase 0 must come first:** it is the only way to attribute the gain correctly.
Measure the cable against a poll-limited baseline and the poll's improvement gets
credited to the cable. It is also independently valuable — it improves WiFi sync
whether or not wired ever ships — and it closes the one gap the design doc admits
to: §1's table has no measured WiFi `offset_sigma` in it.

---

## How to use this file

Status: `[ ]` not started · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` dropped

Update the status boxes and the **Log** at the bottom as work lands. A new session
should read: this file, then the design doc, then the Log.

---

## Phase 0 — make `pump()` prompt, and measure it on WiFi · ✅ COMPLETE 29 Aug 2026

**No wired code. `PinPointStudio` only. Two builds, two measurement arms.**

### Work

| | Repo | Item |
|---|---|---|
| `[x]` | PPS | **Step 1 — instrument only.** Add `min_rtt` to the `PINPOINT_SYNC_TRACE` line beside `offset_sigma`/`skew_sigma` (`ppcp_host_service.cpp`, the `m_syncTrace` block in `onTick()`). Reuse `ppcp_peer_sync_estimator_for_pair()` + `ppcp_sync_estimator_min_rtt_ns()` — **both already public**, no `libppcp` change. Needs a thin accessor via `PpcpLiveSession`, which already exposes `relatedTimebases()` and `stats()`. |
| `[x]` | — | **Build 1**, then run the **BEFORE** arm. |
| `[x]` | PPS | **Step 2 — the fix.** One `QSocketNotifier(fd, Read)` per channel, owned by `Phone`. Created in `adoptLink()` from `link->channels()` + `TransportChannel::fd()`, and in `adoptChannel()` for a channel adopted later under `ENC` 2.1d. On activation call `ph->peer->pump()` — **`pump()`, not `tick()`**; `tick()` stays on the timer because it is driven by time passing. |
| `[x]` | PPS | ⛔ Destroy the notifiers in `dropPhone()` **before** the link and its channels close. `ppcp_host_service.cpp:1205` already records this trap: *"a `QSocketNotifier` left on a closed socket"*. |
| `[x]` | PPS | Re-entrancy guard: `pump()` → `drainEvents()` → preview consumers can spin the event loop. Disable the notifier for the duration of the pump, or a `bool` guard on `Phone`. |
| `[x]` | PPS | Update the comment at `ppcp_host_service.cpp:122` so it describes what the code now does. |
| `[x]` | — | **Build 2** (ownership diagnostic — see Findings), **Build 3** (the fix), then the **AFTER** arm. |
| `[x]` | PPS | Record both arms in this tracker's Log **and** in §1 of the design doc, closing its admitted gap. |
| `[x]` | PPS | Fix design doc §7.1 — drop the `ppcp_peer_sync_observe()` recommendation, keep the notifier. |
| `[x]` | PPS | Commit tracker + fix + doc updates. |

### Critical files

- `src/Ppcp/ppcp_host_service.cpp` — `onTick()` (trace block), `adoptLink()` `:612`,
  `adoptChannel()` `:845`, `dropPhone()` `:721`, timer setup `:127`
- `src/Ppcp/ppcp_host_service.h` — `struct Phone` `:504`
- `src/Ppcp/ppcp_host_peer.h` — `pump()` `:133`, `tick()` `:135` (no change expected)
- `src/Ppcp/ppcp_transport.h` — `TransportChannel::fd()` (already public, unused in
  product code)

### Measurement — M0

Both arms carry the Step 1 instrumentation, so `min_rtt` is directly readable in each
rather than inferred.

```
PINPOINT_LOG_STDERR=1 PINPOINT_SYNC_TRACE=1 <PinPointStudio>
xcrun devicectl device process launch --terminate-existing ... PinPointCapture
```

Phone paired, on WiFi, idle link, **5 minutes per arm**, back to back so network
conditions are comparable. Record per arm:

**Result — iPhone 16 over WiFi, idle link, converged at ~350 s, back-to-back arms.**

| | BEFORE (20 ms poll) | AFTER (notifier) | change |
|---|---|---|---|
| `min_rtt` | 12.95 ms | **1.64 ms** | **7.9× better** |
| floor = ½·`min_rtt` | 6.47 ms | 0.82 ms | 7.9× |
| `offset_sigma` — **gates arbitration** | 2.50 ms | **1.16 ms** | **2.2×** |
| `own_sigma` — what we publish to the phone | ≥ 6.47 ms (10.0 ms at t+31 s) | **0.86 ms** | ≳ 7.5× |
| `skew_sigma` | 24.18 ppm | 10.50 ppm | 2.3× |
| `sigma@5s` | 2.51 ms | 1.16 ms | 2.2× |
| margin under the 5 ms gate | 2.0× | **4.3×** | |

Tests: `ppcp_host_service_test` (isolated, per the advertise-suite poisoning note),
`ppcp_app_tu_syntax`, `ppcp_host_peer_test`, `ppcp_link_bind_test` — **4/4 pass**.

### Definition of done

1. `pump()` runs on fd-readable; the `:122` comment is true.
2. `min_rtt` visible in the trace.
3. Both arms recorded here **and** in design doc §1.
4. Directly affected PPCP test targets pass — **through `ctest`**, once, at the end.
5. Committed.

### Findings — three of them, and two are corrections to the design

**1. The poll was 87% of the measured round trip, and my model of why was wrong.**
I predicted the poll would add `20/33 ≈ 0.6 ms` to `min_rtt` — the expected minimum
of a uniform 0–20 ms delay over a 32-sample window. It added **11.3 ms**. The model
assumed the delay was *random*; it is not. The probe is queued and written on a
tick, and the reply is read on the *next* tick, so the wait is very nearly a whole
period every single time. **Min-RTT filtering had no spread to filter.** A polled
loop does not add a random delay, it adds a phase.

**2. ⛔ The relation that gates arbitration is the PHONE'S, not this host's — and
the design doc §7.1 got this wrong.** Both peers call
`ppcp_peer_publish_relations()`, and libppcp puts a published relation into the
*same* `p->relations` an arriving `relation_update` writes to (`ppcp_peer.c:1978`
and `:2835`). The trace's `own_dir` shows this host's estimator produces
`tb:host -> tb:hosttime`, the **reverse** of the direction `offsetToRefNs()` and
`PpcpShotBridge` use. So the number the 5 ms gate sees was never this host's to
improve directly.

It improved anyway — 2.50 → 1.16 ms — and that is the second thing I predicted
wrongly. I expected it to barely move. **The host is also the *responder* to the
phone's probes**, so a reply that waited for a tick inflated the phone's measured
RTT too. The poll degraded both directions; the notifier fixes both.

**3. ⚠⚠ The case for the cable is materially weaker than the design doc claims,
and this is the finding that matters most.** Design doc §1's table assumed a WiFi
`min_rtt` of ~4 ms. **Measured: 1.64 ms** once the host stops polling. And
`offset_sigma` now sits at 1.16 ms against a 5 ms gate — a **4.3× margin**.

USB might reach ~0.5 ms `min_rtt` and ~0.3 ms sigma. That is real, but it is roughly
a **3× improvement on a number already 4× inside the gate**, not the 8× on a
marginal number that §1 implied. **Phase 0 appears to have captured most of the
available win, with no cable.** M1 must be judged against this baseline, and the
honest question for Phase 1 is now *"what does the remaining 3× buy that 4.3× of
margin does not already"* — with the Windows/Linux reconnection case (§1's second
argument, untouched by this) carrying more of the weight than it used to.

### Prediction made before the run, kept for the record

Predicted from the window arithmetic: a uniform 0–20 ms poll delay over a 32-sample
window contributes about `20/33 ≈ 0.6 ms` to `min_rtt`. On WiFi at ~4 ms true RTT
that is ~15% — **a real but modest improvement**. ⚠ A large improvement means the
model is wrong and the poll was costing more than predicted; no improvement means
`min_rtt` was never poll-limited and design doc §7.1 is wrong. Either way, say so
rather than fitting the story to the number.

---

## Orchestration for Phases 1–3 — Opus subagents

Phase 0 is done by hand in this session: it is one small change plus a measurement,
and the measurement is the point. **Phases 1 onward are orchestrated**, with me
holding the contracts and the review and Opus agents doing the building.

### The constraint that decides the wave shape

⛔ **Two agents cannot build `PinPointStudio` at once.** There is one warm build
directory (`build/Qt_6_11_1_for_macOS_Debug`, ~47 s incremental); concurrent CMake
into it corrupts both. Worktree isolation would fix the file conflicts but each
worktree needs a cold Qt configure, which costs far more than the parallelism buys.

So parallelism comes from **repos, not from files**:

| Wave | Runs in parallel | Why it is safe |
|---|---|---|
| 1 | **PPC agent** (Swift, own repo, own toolchain) ‖ **PPS transport agent** | Different repos, different builds, zero contention |
| 2 | **PPS host-service agent** (alone) | Same build dir as wave 1's PPS agent — must follow it, and it depends on wave 1's seam |
| 3 | **PPS platform agent** (Windows/Linux providers) ‖ **PPC submission agent** (`xcprivacy`, review notes) | Different repos again |

⚠ Within PPS the work is effectively **serial**. Say so rather than promising a
speed-up the build directory cannot deliver.

### Contracts I fix before any agent starts

Agents implement against these; they do not negotiate them with each other. Written
into this tracker first, so a later session can see what was agreed.

1. `ConnectorConfig::dial` — the exact `DialFn` signature and ownership of the fd.
2. `adoptLink(std::unique_ptr<PeerConnection>, const QString &resolvedPairingId = {})`.
3. The `WiredPresence` CBOR schema — field names, types, order. **No `rid`.**
4. The presence port constant, and that both wired listeners bind `127.0.0.1`.
5. Which tests each agent owns, so two agents do not write the same fixture.

### The brief every agent gets

Each agent is pointed at its section of `docs/design/wired_transport_design.md` and
handed the ⛔ list below. These are traps found by tracing this codebase, and an
agent reading only the code **will not** discover them:

- `PortNumber` is byte-swapped; the wrong order **connects to a different port**
  rather than erroring.
- Filter `ConnectionType == "USB"` — usbmux also reports WiFi-paired devices.
- `link->pairingId()` is **empty when we dialled**; the pairing must be passed in.
- Destroy a `QSocketNotifier` **before** closing its fd (`:1205`).
- The dial must not run on the accept thread (`:242-296`).
- Both wired listeners bind loopback only — an all-interfaces bind publishes the
  pairing list to the LAN.
- Never switch a live link's transport; never migrate a link between transports.

### My role, not delegated

Contracts, the ⛔ brief, review of each agent's diff against the design doc, the
integration build and `ctest` run, and this tracker. ⚠ Agents report their own
results; take a green claim as a claim until the integration run agrees.

---

## Phase 1 — the tunnel, macOS only · NEXT SESSION

Gated on Phase 0. Also gated on **M10 (thermal)**, which is a Phase 1 item, not a
Phase 2 one. Waves 1–2 above; measurements are mine, not an agent's.

| | Repo | Item |
|---|---|---|
| `[ ]` | PPS | `src/Ppcp/ppcp_usbmux.{h,cpp}` — first-party usbmux client, Qt-free. `listDevices()`, `watch()`, `dial(udid, port)`. Minimal XML-plist emit/scan; no `libusbmuxd`, no `libplist`. Wire format is **verified** in design doc §4.2 — header `<IIII` LE, `version=1`, `message=8`, tag echoed; ⛔ `PortNumber` byte-swapped (`htons`), and the wrong order *connects to a different port* rather than erroring; results 0 ok / 3 refused; filter `ConnectionType == "USB"`. |
| `[ ]` | PPS | `ConnectorConfig::dial` seam — a `DialFn` returning a connected non-blocking fd. `dialTcp()` (`ppcp_transport.cpp:1148`) becomes the default; it has exactly **one** call site (`:1244`). ⚠ Guard `applyOptions()` against `TCP_NODELAY`/`SO_*BUF` on an `AF_UNIX` fd. |
| `[ ]` | PPS | ⛔ `adoptLink()` takes the resolved pairing as a parameter. `link->pairingId()` is **empty when we dialled** (`ppcp_transport.h`), so `ppcp_host_service.cpp:623` would give no device row, no `noteLinkEstablished()`, wrong advertisement set. Same class as the fixed empty-Phones-list bug. |
| `[ ]` | PPS | `cfg.listener` becomes a parameter, not the constant at `ppcp_host_peer.cpp:487`; call `ppcp_peer_hello()` on the wired path (`RV` 2d inverts — the host is the initiator here). |
| `[ ]` | PPS | ⛔ The dial runs on **its own thread**, never the accept thread (`:242-296` polls `acceptChannelFor()` then blocks 250 ms in `accept()`). ✅ The *watch* needs no thread — usbmux `Listen` is a readable fd, so a `QSocketNotifier` as at `:1109`. |
| `[ ]` | PPS | Stub usbmuxd for tests — plist protocol over `AF_UNIX`, scripted attach/detach and `Connect` results. Makes the path testable with no phone and no cable. |
| `[ ]` | PPC | `WiredPresenceListener` — fixed port, plaintext, serves the CBOR record. ⛔ Bound to `127.0.0.1` via `requiredLocalEndpoint`, **not** all interfaces. `ppcp_cbor_writer` is already used from Swift (`Candidate.swift:245`). |
| `[ ]` | PPC | Start `PpcpListener` on the wired path with `listener: true` in `DevicePeer`; publish `psk_identity` and the actual port. Record lists persisted pairings **plus any scanned, not-yet-connected code**, mirroring the host's resolver. |
| `[ ]` | PPC | `isIdleTimerDisabled` while a session is live — one line, owed to capture regardless, and load-bearing for USB Restricted Mode. |
| `[ ]` | — | **M1** (`min_rtt` wired vs WiFi, Phase 0 fix in place), **M10** (sustained capture before thermal throttle, cabled vs not — ⚠ screen for issue #101's ~8.8 s gap signature first), **M3** (does bulk poison control), **M5** (does `Connect` need device trust — needs an *untrusted* device). |

**Phase 1 done:** a phone on a cable reaches `session_open` with a published
`TimebaseRelation`, and M1/M3/M10 are numbers in this tracker.

⛔ **Stop conditions.** M1 flat → the accuracy case is gone; only the Windows/Linux
reconnection case survives, which is a much smaller reason to build a much smaller
thing. M10 bad → the cable buys sync and costs capture, and that trade goes to Mark
before anything ships.

---

## Phase 2 — cross-platform and product

| | Repo | Item |
|---|---|---|
| `[ ]` | PPS | *(wave 3 agent)* Windows provider (`AF_INET 127.0.0.1:27015`, Apple Mobile Device Service); Linux provider (`AF_UNIX`). Detect-and-explain per the design doc §6.2 diagnostics table — seven distinguishable causes, and that table is the acceptance criterion. |
| `[ ]` | PPS | §6.1 arbitration: per-pairing mDNS advertisement suppression, **suppressed before dialling**, restored on **any** ending. Plus the `onDeclare()` backstop keyed on `counterpartId` (not `pairingId`) via the existing `peerForId()` `:453`. |
| `[ ]` | PPS | Settings→Phones shows the transport; **"Use cable"** offered when wired is available and unused. ⛔ Never switch automatically — a switch discards the sync fit. |
| `[ ]` | PPS | Wired behind a setting, default off until M1 and M10 report. |
| `[ ]` | PPS | ⚠ Name an owner for the Linux `usbmuxd` + udev prerequisite. |
| `[ ]` | PPC | Presence listener lifecycle: refresh `rn2` on foreground entry and pairing-set change; stop on background. |
| `[ ]` | PPC | `PrivacyInfo.xcprivacy` with reason **35F9.1** for `mach_absolute_time`/`mach_continuous_time` — ⛔ **none exists today**, and it is a hard submission blocker independent of this work. |
| `[ ]` | PPC | Review notes, self-contained demo path and a pairing video for guideline 2.1. |
| `[ ]` | libppcp | Raise the `RV` 5.3a erratum (per-listener-session `rn2` on a direct path) with Phase 1 numbers attached; document the presence record so a third party could interoperate. |

---

## Phase 3 — split-transport link · gated on M3/M4

Control on the cable, bulk on the radio; one link, two transports — legal under
`ENC` 2.1b. Only if M3 shows the single USB pipe starves the control channel.
✅ `ConnectionSpeed` is readable before dialling (measured `480000000` on the
iPhone 16), so the decision is per device, not global policy.

---

## Verification

- **Tests:** `ctest` only, never a bare test binary — it supplies `PINPOINT_CORE_NORMS`.
  Directly affected targets only, once, at the end.
- **Build:** `cmake --build build/Qt_6_11_1_for_macOS_Debug --target PinPointStudio`
  (~47 s). ⛔ Never `macos-release-arm64` — serial Makefiles.
- **Phase 0 end-to-end:** phone paired on WiFi, `PINPOINT_SYNC_TRACE=1`, confirm the
  trace prints `min_rtt` and the link stays up across a 5-minute idle run in both arms.

---

## Log

| Date | Phase | What happened |
|---|---|---|
| 2026-08-29 | — | Design committed `d9582c3`. Nothing built. |
| 2026-08-29 | — | Plan agreed. Phase 0 by hand this session; Phases 1+ orchestrated with Opus agents, waves shaped by repo because PPS has one warm build directory. |
| 2026-08-29 | 0 | ✅ **Complete.** `QSocketNotifier` per channel; `min_rtt` and `own_sigma` added to the trace. `min_rtt` 12.95 → 1.64 ms, `offset_sigma` 2.50 → 1.16 ms, gate margin 2.0× → 4.3×. 4/4 affected tests pass. Three findings above; two are corrections to the design doc, and one weakens the case for the cable. |
| | | ⚠ **Next session starts here:** re-read Finding 3 before doing any Phase 1 work. The M1 gate is now "what does ~3× more buy on a number already 4.3× inside the gate", and the answer may be that the Windows/Linux reconnection argument carries Phase 1 rather than the accuracy one. |
