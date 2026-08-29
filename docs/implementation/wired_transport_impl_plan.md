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

**Porting to another platform?** Read **§Phase 2W (Windows)** or **§Phase 2L
(Linux)** — each is a self-contained brief — then *"What is shared, and must not
be re-litigated per platform"* immediately after them. ⛔ **The single most
important thing either brief says:** on Windows and Linux there is **no WiFi
reconnection at all today** (`ppcp_discovery.cpp` is `__APPLE__` throughout), so
wired is the *only* reconnection path there — and the whole WiFi-versus-wired
takeover is **inert until mDNS is ported on BOTH**. ⚠ It is not a Windows-only
gap: the guard excludes every non-Apple platform, Linux included. Porting mDNS is a precursor to that
behaviour, not to wired itself.

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

**3. The idle-network sigma win is mostly taken — but that is a narrower result
than it first looked.** Design doc §1's table assumed a WiFi `min_rtt` of ~4 ms.
**Measured: 1.64 ms** once the host stops polling, with `offset_sigma` at 1.16 ms
against the 5 ms gate — a **4.3× margin**. On *this* measurement USB argues for
~3× on a number that is no longer marginal.

⛔ **Do not read that as "the cable is not worth building", and my first write-up
of it did.** The run was **one idle link on an uncontended network — the best case
for WiFi and the case a range does not have.** 1.64 ms is a floor, not a typical
value, and `CORE` §3.2's table is about the *tail*: *"congested WiFi: very
heavy-tailed. May not reach useful sigma at all."* A shot happens at an instant,
not after five quiet minutes. **M1 is therefore split (§11): an idle arm and a
contended arm, reported at the 95th percentile and worst 5 s window.** What the
cable sells is a floor that does not move, and the idle measurement cannot see it.

⚠ **And clock alignment was never the whole case** — design doc §1.1, added after
this run. Contention-resistance, reliability (no drops, no roaming, no multicast,
no client isolation), **power** (a cabled phone is charging, which is what lets a
long range session outlast a battery), and cross-platform reconnection all survive
Phase 0 untouched. Phase 0 improved one of five arguments. It did not settle the
question.

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

## Phase 1 contracts — fixed 29 Aug 2026, before any agent started

Agents implement against these verbatim. They are not negotiable between agents,
and an agent that finds one wrong reports it rather than changing it.

### C1 — `ConnectorConfig::dial` (PPS transport agent owns)

```cpp
// Supplies a CONNECTED socket for ONE channel.  Called once per channel of a
// link (twice today, three times under ENC 2.1d), so it must be re-entrant and
// must return a DISTINCT fd on each call.  Null = dialTcp(cfg, deadline), which
// is every caller today.
//
//  * `deadline` is an absolute value in the same units as nowMs(), i.e. the
//    same deadline dialTcp() already takes.
//  * The returned fd MUST be non-blocking and MUST be connected (or have its
//    connect completed) by the time it is returned.
//  * OWNERSHIP TRANSFERS ON RETURN.  From the moment a valid fd is returned the
//    Connector owns it and closes it on any later failure.  Returning
//    PP_INVALID_SOCKET means "could not dial", and the dialler must already
//    have closed anything it opened.
//  * It MUST NOT throw; a failure is PP_INVALID_SOCKET.
using DialFn = std::function<pp_socket_t(double deadline)>;
DialFn dial;
```

Placement: a new field in `ConnectorConfig` after `options`, before `log`.
Call site: exactly one — inside `dialLink()`, currently
`const pp_socket_t s = dialTcp(cfg, deadline);` — becomes
`const pp_socket_t s = cfg.dial ? cfg.dial(deadline) : dialTcp(cfg, deadline);`
Nothing else in the 2114-line transport changes.

⛔ `applyOptions()` gains a family guard in the same change: read the family with
`getsockname()` and apply `IPPROTO_TCP`/`TCP_NODELAY` only for `AF_INET`/`AF_INET6`.
`SO_NOSIGPIPE`, `SO_SNDBUF` and `SO_RCVBUF` are `SOL_SOCKET` and stay unguarded.
The guard is defensive — the wired dialler sets its own options on its own fd —
but it is the thing that stops a future caller getting `ENOPROTOOPT` on `AF_UNIX`.

### C2 — `adoptLink()` (PPS host-service agent owns)

```cpp
// Empty = "read it off the link", which is every caller today.  A DIALLING
// caller resolved the pairing under RV 5.3b before it dialled (§5.2), and
// link->pairingId() has nothing to say on this side.
void adoptLink(std::unique_ptr<Ppcp::PeerConnection> link,
               const QString &resolvedPairingId = {});
```

Body rules, in order:

1. `pairingId` = `resolvedPairingId` when non-empty, else
   `QString::fromStdString(link->pairingId())`.
2. ⛔ `noteLinkEstablished()` is called **only** when `resolvedPairingId` is
   empty. RV 7.3a spends a *pairing code*; a wired reconnection uses a persisted
   pairing and spends nothing.
3. `m_pairedThisRun.insert(pairingId)` and `refreshAdvertisement()` still run in
   both cases — a wired link resolves against a persisted pairing, so the
   Settings→Phones row must be identical either way.
4. Everything below that is unchanged, including the "DO NOT DEDUPLICATE BY
   PAIRING" comment, which stays true.

### C3 — `WiredPresence` CBOR schema (PPC agent writes it, PPS host-service agent reads it)

A CBOR **definite-length map**, four keys, emitted in **`ENC` 4e deterministic
order** — which is the order below. `dl` is **omitted entirely** when absent —
never `null`.

| key | type | notes |
|---|---|---|
| `"dl"` | tstr, optional | display label. ⛔ UNTRUSTED (RV 4.4d) — `sanitiseLabel()` before display, never used as a key |
| `"pv"` | tstr | `"1.0"`. RV 3.3a — the host filters on MAJOR before dialling |
| `"role"` | tstr | `"capture"` |
| `"peers"` | array of definite maps | each `{"port": uint, "psk_identity": bstr}`, in that key order |

⛔ **AMENDED 29 Aug, and the first version of this contract was wrong.** It
specified `pv, role, dl, peers`, which does **not** sort under `ENC` 4e — bytewise
over the *encoded* key puts `dl`(`62 64 6c`) before `pv`(`62 70 76`) before
`role`(`64 …`) before `peers`(`65 …`). libppcp's writer *enforces* 4e and sets
its sticky error on the offending key, so a record carrying a label could not be
emitted in the original order at all without
`ppcp_cbor_writer_init_order(…, PPCP_CBOR_ORDER_LITERAL)` — and `cbor.h` says
that escape hatch **"exists for two reasons and no others"**, neither of which is
this. The order above needs no escape hatch. ✅ Costs the host nothing: the reader
was already required to accept any key order. ⚠ Note `pv, role, peers` — the
no-label case — was deterministic all along, so only the labelled record was
affected, which is exactly how this would have survived a casual test.

- `port` — the actual bound port of that pairing's `PpcpListener`, 1..65535.
- `psk_identity` — exactly **17** bytes, `0x01 ‖ rn2(8) ‖ tag(8)`, as registered.
- **No `rid`.** `PpcpRendezvous::identityResolver()` already returns both `kTls`
  and `pairingId` from the 17 octets; a second resolution path would be a second
  place to get expiry, exhaustion and invalidation wrong (§5.3).

Reader rules (host):

- Accept keys in **any** order and **ignore unknown** ones (forward compat).
- Refuse: `pv` MAJOR ≠ 1; `peers` absent, empty, or more than **16** entries;
  any `psk_identity` whose length ≠ 17; a record longer than **4096** bytes.
- A refusal is **silence** — the device is treated as not wired, one `ppWarn()`
  line, no banner (§6.2, RV 3.6a).

**Framing: there is none.** The device writes the record and closes the
connection. The host reads to EOF with a 4096-byte cap and a **2 s** deadline.
A short read, a timeout or a cap breach is the same refusal as a parse failure.

### C4 — the presence port, and the loopback bind

```
PPCP_WIRED_PRESENCE_PORT = 50915
```

Private range, no IANA assignment. A collision is survivable by design: the host
gets a record it cannot parse and treats the device as not wired (§5.3).
Declared once per repo — `ppcp_usbmux.h` in PPS, `WiredPresenceListener.swift`
in PPC — with the constant's value written in both and this contract cited.

⛔ **Both wired listeners bind `127.0.0.1` and nothing else** — the presence
listener and every per-pairing `PpcpListener` on the wired path — via
`NWParameters.requiredLocalEndpoint`. An all-interfaces bind publishes the
device's whole pairing list to the LAN (§5.3, §5.4) and pulls the iOS
local-network permission into a path that does not need it.

### C5 — one `PpcpListener` per held pairing

`PpcpCredentials` carries **one** `tlsKey` and **one** `nextPskIdentity()`, and
`NWListener` registers exactly one (key, identity) pair. So the device runs **one
listener per held pairing**, each on its own ephemeral port (`port: 0`, read the
bound port back), and `peers[]` has one entry per listener. That is what the
array in C3 is for; it is not a list of hosts.

### C6 — wired sequencing on the host, and why there is no second `link_bind`

The host is the initiator on the cable (RV 2d inverts). Sequence, exactly:

1. `Connector::connect()` with `cfg.dial` set. **The transport mints the
   `link_id` and writes `link_bind` itself** — it already does, on every dial.
2. `adoptLink(link, resolvedPairingId)` → `configurePhonePeer()` builds the
   engine with **`HostEngineConfig::listener = false`**.
3. ⛔ **Never call `ppcp_peer_set_link_id()` on the host.** `has_link_id` stays
   false, so `ppcp_peer_hello()`'s auto-open at `ppcp_peer.c:929`
   (`!listener && has_link_id && …`) does not fire and libppcp emits **no second
   `link_bind`**. This is the trap: `listener=false` alone is safe *only*
   because the link id is never handed to the library.
4. Call `ppcp_peer_hello()` once, after `attach()`, on the wired path only.
5. The device auto-replies `hello_accept` (`ppcp_peer.c:2324`), which raises
   `PPCP_EVENT_CONNECTED` on the host — **the same event the WiFi path already
   declares on** (`ppcp_host_peer.cpp:204`). ✅ Nothing downstream changes.

`HostEngineConfig::listener` becomes a parameter of `configurePhonePeer()` /
`PpcpHostPeer::Config`, defaulting to `true`. `ppcp_host_peer.cpp:487`'s
`cfg.listener = true` becomes `cfg.listener = m_cfg.listener`.

⛔ **C6 ADDENDUM, found on the device side and it applies to BOTH ends: `declare`
moves too, not just `hello`.** On the wired path the peer that used to dial no
longer originates first, so at `open()` time **there is no agreed wire version
yet**. `declare`, the sync timebase registration and the connect-trigger burst
must all wait for the `connected` event alongside `hello`.

⚠ **And this fails silently rather than loudly, which is why it is written here.**
`ppcp_peer_declare()` does **not** refuse a declaration sent from
`PPCP_PEER_INIT` — `ppcp_peer.c:978` reads
`if (p->state == PPCP_PEER_CONNECTED || p->state == PPCP_PEER_INIT) p->state = PPCP_PEER_DECLARED;`
— so a premature declare is accepted, skips version agreement, and everything
downstream looks fine. ✅ The host side already happens to be correct here
(`ppcp_host_peer.cpp:204` declares on `PPCP_EVENT_CONNECTED`), but do not
"simplify" it to declare at attach time.

### C7 — test ownership, so two agents never write the same fixture

| Agent | Owns (creates/edits) | ⛔ must not touch |
|---|---|---|
| PPS transport | `src/Ppcp/ppcp_usbmux.{h,cpp}`, `tests/ppcp_usbmux_test.cpp` (+ its stub usbmuxd), the `dial` seam rows of `tests/ppcp_transport_test.cpp` | `ppcp_host_service*`, `ppcp_host_peer*` |
| PPS host-service | `ppcp_host_service.{h,cpp}`, `ppcp_host_peer.{h,cpp}`, `ppcp_host_engine.h`, `tests/ppcp_host_service_test.cpp` | `ppcp_transport.*`, `ppcp_usbmux.*`, `ppcp_transport_test.cpp` |
| PPC | `Sources/Platform/Network/WiredPresenceListener.swift`, `Packages/Core/Tests/CaptureCoreTests/WiredPresenceTests.swift`, `HostLinkSession.swift` | nothing in PPS |

The stub usbmuxd is the transport agent's, and the host-service agent consumes it
through `ppcp_usbmux.h` rather than copying it.

---

## Phase 1 — the tunnel, macOS only · ✅ COMPLETE 29 Aug 2026

Gated on Phase 0. Also gated on **M10 (thermal)**, which is a Phase 1 item, not a
Phase 2 one. Waves 1–2 above; measurements are mine, not an agent's.

| | Repo | Item |
|---|---|---|
| `[x]` | PPS | *(wave 1, transport agent)* `src/Ppcp/ppcp_usbmux.{h,cpp}` — first-party usbmux client, Qt-free. `listDevices()`, `watch()`, **`dial(deviceId, port)`** (⛔ not `udid` — see the Log). Minimal XML-plist emit/scan; no `libusbmuxd`, no `libplist`. Wire format is **verified** in design doc §4.2 — header `<IIII` LE, `version=1`, `message=8`, tag echoed; ⛔ `PortNumber` byte-swapped (`htons`), and the wrong order *connects to a different port* rather than erroring; results 0 ok / 3 refused; filter `ConnectionType == "USB"`. |
| `[x]` | PPS | *(wave 1, transport agent — contract C1)* `ConnectorConfig::dial` seam — a `DialFn` returning a connected non-blocking fd. `dialTcp()` (`ppcp_transport.cpp:1148`) becomes the default; it has exactly **one** call site (`:1244`). ⚠ Guard `applyOptions()` against `TCP_NODELAY`/`SO_*BUF` on an `AF_UNIX` fd. |
| `[x]` | PPS | *(wave 2, host-service agent — contract C2)* ⛔ `adoptLink()` takes the resolved pairing as a parameter. `link->pairingId()` is **empty when we dialled** (`ppcp_transport.h`), so `ppcp_host_service.cpp:623` would give no device row, no `noteLinkEstablished()`, wrong advertisement set. Same class as the fixed empty-Phones-list bug. |
| `[x]` | PPS | *(wave 2, host-service agent — contract C6)* `cfg.listener` becomes a parameter, not the constant at `ppcp_host_peer.cpp:487`; call `ppcp_peer_hello()` on the wired path (`RV` 2d inverts — the host is the initiator here). |
| `[x]` | PPS | *(wave 2, host-service agent)* ⛔ The dial runs on **its own thread**, never the accept thread (`:242-296` polls `acceptChannelFor()` then blocks 250 ms in `accept()`). ✅ The *watch* needs no thread — usbmux `Listen` is a readable fd, so a `QSocketNotifier` as at `:1109`. |
| `[x]` | PPS | *(wave 1, transport agent — contract C7)* Stub usbmuxd for tests — plist protocol over `AF_UNIX`, scripted attach/detach and `Connect` results. Makes the path testable with no phone and no cable. |
| `[x]` | PPC | *(wave 1, PPC agent — contracts C3, C4)* `WiredPresenceListener` — fixed port, plaintext, serves the CBOR record. ⛔ Bound to `127.0.0.1` via `requiredLocalEndpoint`, **not** all interfaces. `ppcp_cbor_writer` is already used from Swift (`Candidate.swift:245`). |
| `[x]` | PPC | *(wave 1, PPC agent — contract C5)* Start `PpcpListener` on the wired path with `listener: true` in `DevicePeer`; publish `psk_identity` and the actual port. Record lists persisted pairings **plus any scanned, not-yet-connected code**, mirroring the host's resolver. |
| `[x]` | PPC | *(wave 1, PPC agent)* `isIdleTimerDisabled` while a session is live — one line, owed to capture regardless, and load-bearing for USB Restricted Mode. |
| `[x]` | — | ✅ **M12 — DISCHARGED 29 Aug, over a real cable. The device-side mux dials IPv4 loopback; no fallback needed.** `Connect(device, 50915)` → `Number=0`, and the presence record came back: **118 bytes, fully consumed, no framing**, `ENC` 4e key order (`dl, pv, role, peers`) in the *live* record and not merely the fixture, `dl` = "iPhone", **two held pairings → two listeners on ephemeral ports 57131/57132** exactly as contract C5 predicts, both identities 17 bytes starting `0x01` and neither valid UTF-8. `requiredInterfaceType = .loopback` is NOT required. ~~⛔ M12 — does the device-side mux connect to `127.0.0.1` or `::1`?~~ `NWParameters.requiredLocalEndpoint` **pins the address family**, so the presence listener is IPv4-loopback only. If usbmuxd on the device dials `::1`, the presence read fails and the phone simply looks un-wired — an indistinguishable, silent failure. ✅ Fallback if it bites: `requiredInterfaceType = .loopback`, which covers both families and keeps the LAN exclusion §5.3 needs. ⚠ Needs the cable; unanswerable in a simulator. **Check this FIRST if M1 cannot read a presence record.** |
| `[~]` | — | **Mine, not an agent's.** ✅ M1a, M2, M12 done; ⛔ **M1b DISMISSED as circular (Mark, 29 Aug) — do not re-run.** **M1a/M1b** (`min_rtt` wired vs WiFi, Phase 0 fix in place), **M10** (sustained capture before thermal throttle, cabled vs not — ⚠ screen for issue #101's ~8.8 s gap signature first), ~~M3~~ ✅ done — bulk does NOT poison control, **M5** (does `Connect` need device trust — needs an *untrusted* device). |

**Phase 1 done:** a phone on a cable reaches `session_open` with a published
`TimebaseRelation`, and M1/M3/M10 are numbers in this tracker.

### ✅ Phase 1 definition of done — MET 29 Aug 2026, on hardware

```
[ppcp-usb] wired path armed (PINPOINT_PPCP_WIRED=1) — /var/run/usbmuxd
[ppcp-usb] 1 device(s) attached, 1 on a cable — usbmux: ok
[ppcp-rv]  link up: TLSv1.2 TLS_PSK_WITH_AES_128_GCM_SHA256 psk … transport=usb "pairing=023e26a759228c15"
[ppcp]     peer declared: "iPhone 16" - 2 camera Source(s)
```

Zero `live session open refused` lines, and the Session opens **on declare**
(`ppcp_host_service.cpp:1035`) rather than on a UI action, so nothing was waiting
on an operator. One link, **zero drops** across the whole 350 s run.

### Measurement — M1a (idle) · ✅ DISCHARGED

Wired vs the Phase 0 WiFi arm, **both at t+350 s**, same phone, same room, gate
on/off back to back.

| | WiFi (Phase 0, 350 s) | **Wired (350 s)** | change |
|---|---|---|---|
| `min_rtt` | 1.64 ms | **0.795 ms** | 2.1× |
| floor = ½·`min_rtt` | 0.82 ms | 0.398 ms | 2.1× |
| `offset_sigma` — **gates arbitration** | 1.16 ms | **0.341 ms** | **3.4×** |
| `own_sigma` | 0.86 ms | 0.401 ms | 2.1× |
| `skew_sigma` | 10.50 ppm | **3.31 ppm** | 3.2× |
| `sigma@5s` | 1.16 ms | 0.342 ms | 3.4× |
| margin under the 5 ms gate | 4.3× | **14.6×** | |

✅ **The design's prediction held this time, and that is worth recording after
Phase 0's did not.** §1 said *"on this measurement USB argues for roughly a 3×
improvement on a number already 4× inside the gate."* Measured: **3.4×** on
`offset_sigma`, gate margin 4.3× → 14.6×. ⚠ `min_rtt` was predicted at ~0.5 ms
and came in at 0.795 ms — the right order, modestly worse than hoped.

### Measurement — M1b (contended) · `[-]` DISMISSED 29 Aug 2026 — the test is circular

⛔ **M1b CANNOT be answered on a quiet network, and this run demonstrates that
rather than assuming it.** A WiFi control arm was taken back to back with the
wired arm — same phone, same room, same session, gate off so the phone dialled
over WiFi (`transport=wifi`, the tag added in this phase earning its keep). It
converged to `offset_sigma` **1.118 ms** and `skew_sigma` **10.46 ppm**, against
Phase 0's 1.16 ms / 10.50 ppm — **the rig reproduces itself**, which is what
makes the rest of this credible.

Both arms, **identical converged window t+250..360 s, 109 samples each**:

| | WIRED | WiFi | |
|---|---|---|---|
| `min_rtt` median | 0.795 ms | 2.234 ms | 2.8× |
| **`min_rtt` spread (max−min)** | **0.000 ms** | **0.000 ms** | — |
| `offset_sigma` median | 0.341 ms | 1.128 ms | 3.3× |
| `offset_sigma` **p95** | 0.351 ms | 1.132 ms | 3.2× |
| `offset_sigma` max | 0.352 ms | 1.133 ms | 3.2× |
| worst 5 s window | 0.352 ms | 1.133 ms | 3.2× |
| gate margin on the worst window | **14.2×** | 4.4× | |

⛔ **The distributions are degenerate: p95 ≈ median ≈ max on BOTH arms, and
`min_rtt` spread is exactly 0.000 ms on both.** There is no tail to take a 95th
percentile of. §11 asks M1b for *"the 95th percentile and the worst 5 s window"*
precisely because the cable's claim is about the tail — and **on an idle,
uncontended link WiFi has no tail either.** So this measurement cannot
distinguish "the cable's floor does not move" from "nothing moved today". It
confirms the 3.2× steady-state gain and says **nothing whatever about
contention**, which is the claim that actually matters.

⛔ **DISMISSED BY MARK, 29 Aug 2026, and the reasoning is better than the
measurement would have been.** *"Evidence that a wired connection is better than
WiFi when WiFi performance is degraded is pretty much not required — it's almost
a circular argument. If performance was unaffected when WiFi performance is
degraded then you wouldn't say that WiFi performance is degraded."*

✅ **Correct, and the circularity is sharper than "almost".** The cable does not
traverse the radio, so radio contention **cannot** reach it — true by
construction, the same way §7.3 argues a transport change cannot carry a stale
clock bias *"by construction rather than by care"*. The number was fixed before
the experiment ran.

⚠ **One distinction kept for the record, because it does not change the
decision but does change what is still unknown.** M1b had two halves and only
one is circular:

- **The cable half is circular.** USB latency does not depend on WiFi occupancy.
  Nothing to learn.
- **The WiFi half is not** — but it asks a question about *WiFi*, not about the
  cable: how badly does WiFi actually degrade at a real range? That sizes
  **whether the cable is needed**, never whether it works, and it was never
  gating this build. It also cannot be answered in a quiet room: it is field
  observation at a range, not a lab test.

✅ **So nothing was lost by dismissing it**, and §1.1's other three arguments
never depended on it: reliability, power, and — the strongest, needing no
measurement at all — **cross-platform reconnection, since `ppcp_discovery.cpp`
is `#if defined(__APPLE__)` throughout and Windows and Linux have no
reconnection path today.**

⚠ **The Phase 0 review's framing — "M1b is the measurement that matters" — was
mine and is now withdrawn.** It was right that an idle measurement oversells the
cable; it was wrong that a contended one would settle anything.

⚠ **Kept only as a note on method**, since it is why the run was attempted and
abandoned rather than never tried. §11 specifies contention as *"other clients,
2.4 GHz, distance, a body in the path"* — physical conditions. From this Mac
they are not reachable:

- **The Mac is on Ethernet (`en0`), not WiFi.** It cannot compete for airtime
  with the phone by generating traffic of its own; its path to the AP is wired.
- **Every synthetic load needs root.** `ping` intervals under 1 s, and
  `dnctl`/`pfctl` shaping, all require privilege — and a `pf` rule left behind
  is a way to break this machine's networking after the measurement ends.
- ⛔ **A radio flood could disrupt the operator's own access to this machine.**
  The Mac is administered over VNC; if that client sits on the same AP,
  saturating the air to measure the phone would degrade the session being used
  to run the measurement.

**M1b would have needed a human in the room.** It is dismissed rather than
deferred, so no later session should re-open it: ⛔ **do not re-run this test.**
M1a/M2 stand on their own, and §1.1's contention argument is accepted as an
argument rather than carried as an open measurement.

### Measurement — M3 (does bulk poison control?) · ✅ DISCHARGED — it does not

**1750 MB pushed over the same USB pipe in 58.5 s (70 × 25 MB, all verified to
succeed, 29.9 MB/s sustained) while the wired sync trace ran.**

| window | n | `min_rtt` min/med/max | `offset_sigma` min/med/max |
|---|---|---|---|
| BEFORE (quiet) | 287 | 0.7645 / 0.7645 / 0.7645 ms | 0.349 / 0.602 / 155.131 ms |
| **DURING (1750 MB)** | 58 | **0.7645 / 0.7645 / 0.7645 ms** | **0.347 / 0.348 / 0.357 ms** |
| AFTER (quiet) | 35 | 0.7645 / 0.7645 / 0.7645 ms | 0.347 / 0.348 / 0.361 ms |

- ⛔ **`min_rtt` did not move by one part in ten thousand** — 0.7645 ms in all
  three windows. 1.75 GB of concurrent traffic across the same pipe changed it
  by nothing at all.
- **Zero probes lost** — `probes` issued equalled `observed` in every sample.
- Worst `offset_sigma` under load 0.357 ms → **14.0× gate margin**.

⚠ **Compare DURING against AFTER, not against BEFORE.** BEFORE's max of 155 ms
is the startup convergence transient, so a naive reading would suggest load
*improved* things. It did not: DURING and AFTER are indistinguishable
(median 0.348 vs 0.348 ms, max 0.357 vs 0.361 ms).

✅ **Verdict: bulk does not poison control over usbmux, so §8's head-of-line
worry does not reproduce and PHASE 3 IS NOT JUSTIFIED on this evidence.** §8
hoped min-RTT filtering would be *"robust to some of this"* and said *"'some' is
a measurement, not an assertion"* — measured, it is robust to all of it.

⚠ **Two limits on the claim, stated rather than buried.** The load went through
usbmux's **file-copy service, not the app's own channel-1 tunnel** — the same
daemon and the same physical pipe, which is exactly §8's stated concern, but not
literally a PPCP bulk channel. And 29.9 MB/s is ~50% of the USB 2 raw ceiling;
it is **~5× the 6.25 MB/s the encoder actually sustains** (§8), so it exceeds
real demand, but the bus was not pinned at 100%.

### Measurement — M4 (wired bulk throughput) · ✅ wired half done

**29.9 MB/s sustained** (1750 MB / 58.5 s); individually timed 25 MB copies at
0.85–0.89 s, i.e. 28.2–29.4 MB/s. §8 predicted *"roughly 30-40 MB/s through
usbmux on USB 2"* — measured at the **low end but inside** the range, giving
**4.8× headroom** over the encoder's 6.25 MB/s rather than §8's hoped 6×.

⚠ **USB-C is a connector, not a speed, and three independent lines agree this
phone is USB 2.** The model (`iPhone17,3`, non-Pro), the device's own
`ConnectionSpeed = 480000000` from `ListDevices`, and the measured 29.9 MB/s at
50% of the 60 MB/s raw ceiling. On USB 3 the same 1750 MB would have taken ~2 s
rather than 58.5 s. ✅ This is precisely why §8 makes the split-transport
decision **per device**: a Pro phone is a bulk win on the cable, this one is not.

### Measurement — M2 (time-to-target) · ✅ DISCHARGED, and it is the operator's number

How long the cable takes to reach what WiFi needed **350 s** to reach:

| target (WiFi's converged value) | wired reaches it at | |
|---|---|---|
| `offset_sigma` ≤ 1.16 ms | **35 s** | **10× faster** |
| `skew_sigma` ≤ 10.50 ppm | **65 s** | **5.4× faster** |

⚠ Genuine crossings, not first samples — the trace opens at t+5 s with
`offset_sigma` at 8.35 ms and `skew_sigma` at 44588 ppm.

⛔ **And the single most telling number is not in either table: `min_rtt` was
0.795 ms on the FIRST sample and never moved for 350 s.** Not once. That is the
"floor that does not move" §1.1 says the cable is actually selling, and it is
visible from the first probe rather than after convergence — which is precisely
what an idle *sigma* comparison cannot show.

⛔ **Stop conditions.** M1 flat → the accuracy case is gone; only the Windows/Linux
reconnection case survives, which is a much smaller reason to build a much smaller
thing. M10 bad → the cable buys sync and costs capture, and that trade goes to Mark
before anything ships.

---

## Phase 2 — cross-platform and product

| | Repo | Item |
|---|---|---|
| `[ ]` | PPS | **Windows and Linux — now their own briefs: see §Phase 2W and §Phase 2L below.** ⚠ Linux is likely a *prerequisites* problem rather than a code one (the `AF_UNIX` client already compiles and runs there); Windows needs a real provider. ⛔ And §6.2's table asks for seven distinguishable causes when the transport can only see **six** — measured. |
| `[x]` | PPS | ✅ **DONE. The race is resolved by TAKEOVER, and the `onDeclare()` backstop now closes the residual hole.** Per-pairing advertisement suppression was **dropped as the mechanism** — it could never win the race. Open only as a question: whether suppression is still wanted for anything. ~~§6.1 arbitration: per-pairing mDNS advertisement suppression~~, **suppressed before dialling**, restored on **any** ending. Plus the `onDeclare()` backstop keyed on `counterpartId` (not `pairingId`) via the existing `peerForId()` `:453`. |
| `[~]` | PPS+PPC | ✅ **Transport is now VISIBLE IN BOTH APPS** (`99c9f0a`, `ac151c8`): a pill beside the connection state in Settings→Phones plus the `Transport` fact, which used to read a constant "PPCP"; and a `Connection` row on the device's B3 telemetry, sourced from the session's `listener` flag because the phone cannot see a cable but can see that the host dialled *it*. ⛔ In both, an absent value **hides rather than defaulting to Wi-Fi** — "we do not know" and "on the radio" are different facts. Verified headless via `--probe-qml`. **Residual: the "Use cable" action.** ~~Settings→Phones shows the transport; **"Use cable"** offered when wired is available and unused. ⛔ Never switch automatically — a switch discards the sync fit. |
| `[x]` | PPS | ✅ **Gate REMOVED 29 Aug — wired is ON by default.** `PINPOINT_PPCP_WIRED=0` forces it off and is an escape hatch, not a feature flag; ⚠ **only** the exact value `0` closes it, so a mistyped variable cannot silently disable a transport an operator is relying on. Verified on hardware with **no environment variable set at all**: `wired path armed`, then `transport=usb` one second after the phone app was activated. |
| `[ ]` | PPS | ⚠ Name an owner for the Linux `usbmuxd` + udev prerequisite. |
| `[ ]` | PPC | Presence listener lifecycle: refresh `rn2` on foreground entry and pairing-set change; stop on background. |
| `[ ]` | PPC | `PrivacyInfo.xcprivacy` with reason **35F9.1** for `mach_absolute_time`/`mach_continuous_time` — ⛔ **none exists today**, and it is a hard submission blocker independent of this work. |
| `[ ]` | PPC | Review notes, self-contained demo path and a pairing video for guideline 2.1. |
| `[ ]` | libppcp | Raise the `RV` 5.3a erratum (per-listener-session `rn2` on a direct path) with Phase 1 numbers attached; document the presence record so a third party could interoperate. |

---

## Phase 2W — the Windows port

**Brief for a Claude Code session on the Windows box.** Read this section, then
design doc §4.3, §6.2 and §2. Update the status boxes and add to the **Log** as
you go — this file is the handover between platforms.

### ⛔ Read this first: on Windows there is no WiFi reconnection AT ALL

`src/Ppcp/ppcp_discovery.cpp` guards its browser *and* its advertiser with
`#if defined(__APPLE__)` (`:31`, `:226`, `:572`), and both `makePlatformBrowser()`
and `makePlatformAdvertiser()` return **null** everywhere else. `dns_sd.h` and
`arpa/inet.h` travel under the same guard.

What that means, and it changes the shape of the whole port:

| | macOS today | Windows today |
|---|---|---|
| First pairing by QR code | ✅ works | ✅ **works** — the code carries the endpoint (`RV` 4.3d), so no discovery is involved |
| Reconnection over WiFi | ✅ the host advertises, the phone dials | ⛔ **impossible** — nothing advertises, so the phone has nothing to find |
| Reconnection over the cable | ✅ works | ⛔ this port |

✅ **So wired is worth more on Windows than on macOS**, and the argument needs no
measurement: it is the difference between "a persisted pairing reconnects" and
"the operator scans a QR code every single session".

⚠ **And the WiFi-versus-wired question we spent a day on does not arise here.**
The cable-takes-over-from-WiFi logic, the 2 s retry racing the phone's dial,
`PeerLinkState::WifiIdle` — all of it is **inert on Windows until mDNS is
ported**, because there is never a WiFi link to take over from. Do not spend time
testing it. ⛔ **The `onDeclare()` backstop is NOT inert and still matters**: a
phone can dial from a scanned pairing code while already on the cable, and that
is the case the backstop exists for.

**This is the dependency to state plainly in any plan:** *porting mDNS is a
precursor to the WiFi-before-wired behaviour, not to wired itself.* Wired can
ship on Windows with no mDNS at all; the takeover only becomes reachable — and
only then needs testing — once a Windows host can advertise.

### W1 — the usbmux provider · `[ ]`

**One function.** `openProvider()` in `src/Ppcp/ppcp_usbmux.cpp:397` currently
answers `Kind::Tcp` with `Status::NoProvider` and a comment saying Phase 2. Make
it connect to **`127.0.0.1:27015`** — Apple Mobile Device Service. Everything
above that socket byte is already shared and already tested: the header layout,
the plist emit/scan, the `PortNumber` byte-swap, the `ConnectionType` filter, the
`Listen` watch and the result-code mapping do not change.

- `Provider::platformDefault()` (`:630`) **already returns the Windows shape** —
  `Kind::Tcp`, `127.0.0.1`, `27015`. Nothing to add there.
- `ensureSockets()` already does the Winsock startup.
- ⚠ **`SOCK_STREAM` on loopback needs no firewall rule.** The Windows firewall
  item already tracked in memory is about the *inbound PPCP listener*, which is a
  different socket and a different problem — do not conflate them.

### W2 — Apple Mobile Device Service is a prerequisite we cannot ship · `[ ]`

AMDS arrives with iTunes or the Microsoft Store "Apple Devices" app. **We may not
redistribute it.** Its absence is an ordinary state and must be reported as one
(`RV` 3.6a): one `ppWarn()` line, no banner, nothing on screen. The §6.2
diagnostics table is the acceptance criterion — each row distinguishable, or the
row is wrong.

⛔ **With one correction already measured on macOS: the table asks for seven
causes and the transport can only see six.** A closed presence port and a
refused-at-the-mux-layer dial are the same `Number=3`. Do not print "trust not
granted" as a diagnosis; M5 has not run.

### W3 — the tests are currently excluded on Windows · `[ ]`

`src/Ppcp/tests/CMakeLists.txt:284` wraps the `ppcp_usbmux_test` target in
`if(NOT WIN32)`, and the dial-seam rows in `ppcp_transport_test.cpp` are
`#ifndef _WIN32`. The reason is the **stub usbmuxd is an `AF_UNIX` server** and
there was nothing to impersonate on Windows.

Porting W1 removes that reason. Give `usbmuxd_stub.h` a **TCP mode** (listen on
`127.0.0.1:0`, hand the port to the `Provider`), then drop both guards. ⛔ Do not
ship the Windows provider with its tests still excluded — the wire format is
identical, so an untested Windows provider is untested for no reason.

### W4 — mDNS, and it is a separate piece of work · `[ ]`

Only needed for **WiFi reconnection**, per the box above. Two candidate routes:

- **Bonjour SDK for Windows** — the same `dns_sd.h` API the Apple path already
  uses, so `ppcp_discovery.cpp`'s guard widens rather than a second backend being
  written. ⚠ It needs Apple's Bonjour service present, which is the **same
  dependency class as AMDS** and arrives by the same route.
- **A native responder.** No Apple dependency, but a second implementation of
  §3's browse/advertise semantics — `rid` rotation (3.4d), TXT keys, the 3.4c
  rule that a host never dials an `rid` it cannot resolve.

⚠ **Whichever is chosen, `RV` 3.5d is a hard gate, not a detail**: a host may
advertise for reconnection only if it can resolve a PSK identity server-side.
This host can — `m_listener.setIdentityResolver(m_rv.identityResolver())` — so
the clause is satisfied, but say so in the code the way the macOS path does.

### Windows — done when

A phone with a persisted pairing, plugged in, reaches `session_open` with **no QR
code scanned**, and the §6.2 table's rows are distinguishable in the log. Record
`min_rtt` and `offset_sigma` here for comparison with macOS's 0.795 ms / 0.341 ms
— ⚠ **expect them to differ**: AMDS is a different multiplexer from Apple's own
`usbmuxd`, and that is worth knowing rather than assuming.

---

## Phase 2L — the Linux port

**Brief for a Claude Code session on the Linux box.**

### ✅ Start here: the client code is very likely already done

Unlike Windows, **nothing in the usbmux client is Apple-specific**.
`Provider::platformDefault()` returns `Kind::Unix` with `/var/run/usbmuxd`, which
is exactly where the open-source `usbmuxd` daemon listens, and the `AF_UNIX`
branch of `openProvider()` compiles and runs on Linux as written.

⚠ **So treat Linux as a PREREQUISITES problem first and a code problem second.**
Install the daemon, plug a phone in, run the host — it may simply work. Establish
that before writing anything.

### L1 — prerequisites, and somebody has to own them · `[x]`

- `usbmuxd` (plus `libimobiledevice`'s udev rules in most distributions).
- ⚠ **Socket permissions.** `/var/run/usbmuxd` is typically owned by a `usbmux`
  user; a desktop user may need a group. Document the one-liner.
- ⚠ **Decide and write down whether this is a SUPPORTED configuration or
  best-effort.** It is a support burden either way, and it must not be discovered
  by a user at a range. This is the open tracker item "name an owner for the
  Linux `usbmuxd` + udev prerequisite" — close it here.

### L2 — ⛔ SIGPIPE will kill the process on Linux · `[~]` code complete, UNVERIFIED on hardware

`setNoSigPipe()` (`ppcp_usbmux.cpp:369`) is `#ifdef SO_NOSIGPIPE` — an Apple/BSD
socket option that **does not exist on Linux**, where the function compiles to
`(void)s`. A write to a usbmux socket the daemon has closed then raises `SIGPIPE`
and terminates the process by default.

The transport has a considered answer to the same problem (*"where it does not
[have the option], the listener never writes to a stream it is refusing"*), but
the usbmux client **does** write — every plist request is a write.

✅ Options, in order of preference: `MSG_NOSIGNAL` on the sends; or ignore
`SIGPIPE` process-wide at startup. ⚠ **Verify rather than assume Qt has already
done it** — check with a phone unplugged mid-request, which is the exact case.

### L3 — mDNS on Linux · `[~]` code complete + unit-proven, UNVERIFIED with a phone

⛔ **Yes, Linux needs this too** — the `__APPLE__` gate is not Windows-specific,
and `makePlatformBrowser()` / `makePlatformAdvertiser()` return null on **any**
non-Apple platform. Same consequence as Windows: no WiFi reconnection today, so
wired is the only reconnection path, and the takeover logic is inert until this
lands.

✅ **But Linux is probably the cheaper of the two ports.** Avahi ships
`avahi-compat-libdns_sd`, which exposes the *same* `dns_sd.h` API the Apple path
already uses — so this may be widening the existing `#if` and linking a library
rather than writing a second backend, which is what Windows would need if it
does not use Apple's Bonjour.

**The whole API surface this file uses**, so the shim can be checked against it
before anybody commits to the approach:

| call | needed for | in the compat shim? |
|---|---|---|
| `DNSServiceBrowse` | finding hosts | ✅ standard |
| `DNSServiceResolve` | turning an instance into an endpoint | ✅ standard |
| `DNSServiceRegister` | advertising | ✅ standard |
| `DNSServiceRefSockFD` | the fd the `QSocketNotifier` watches | ✅ standard |
| `DNSServiceProcessResult` | draining that fd | ✅ standard |
| `DNSServiceRefDeallocate` | teardown | ✅ standard |
| **`DNSServiceUpdateRecord`** | ⚠ **rotating the `rid` IN PLACE** (3.2d/3.4d) | ✅ **PRESENT AND CORRECT — measured 29 Aug 2026, see below** |

⚠ **`DNSServiceUpdateRecord` is the one to check first, and it is not a detail.**
`ppcp_discovery.cpp:620` uses it so *"the service keeps its name and one record
changes, so a rotation is a single announcement rather than a deregister, probe
and announce"*. If the shim does not implement it, rotation still works but must
become deregister-and-re-register — a noisier announcement, and a behaviour
difference from macOS worth stating rather than discovering.

✅ **ANSWERED ON THE LINUX BOX, 29 Aug 2026 — the gap does not exist.** Measured
with a standalone C probe against `libavahi-compat-libdnssd1 0.8-18ubuntu1.1` and a
live `avahi-daemon`, observed from outside with `avahi-browse -rpt _ppcp._tcp`:

```
DNSServiceRegister      -> 0 ok
DNSServiceRefSockFD     -> 4
  [register callback] err=0 name='PPCP-PROBE0001'
DNSServiceUpdateRecord  -> 0 ok
t=4s   PPCP-PROBE0001 ; "k=AAAA" "txtvers=1"
t=12s  PPCP-PROBE0001 ; "k=BBBB" "txtvers=1"
```

The TXT changed and **the instance name did not move**, which is exactly what 3.2d
requires. So no re-register fallback is needed and `ppcp_discovery.h:302-308`'s
degradation clause stays unused on Linux. `DNSServiceRefSockFD` also returns a real
pollable fd, which `RvBrowser::fd()` / `RvAdvertiser::fd()` depend on for their
`QSocketNotifier`.

⚠ **Two things the probe taught that the port must respect.** (1) `DNSServiceProcessResult`
**blocks** when nothing is pending — the first probe hung in it. The production code is
already correct (poll the fd, then process), but any new call site must not assume
otherwise. (2) avahi-compat prints a three-line `*** WARNING *** … use the native API of
Avahi!` banner **to stderr on every process that links it**. Harmless, unsuppressable
from our side, and it will appear in PPS's stderr on Linux — worth expecting rather than
investigating later.

### Linux — done when

Same bar as Windows: a persisted pairing reconnects over the cable with no QR
code, the §6.2 rows are distinguishable, and the prerequisite question has a
written answer. Record `min_rtt`/`offset_sigma` here too — Linux runs the
**open-source** `usbmuxd` rather than Apple's, so a difference from macOS is
plausible and worth capturing.

⛔ **STATUS 29 Aug 2026 — ONE OF FOUR MET.  THE PHASE IS NOT DONE.**

| Clause | |
|---|---|
| A persisted pairing reconnects over the cable, no QR | ❌ **not met** |
| §6.2 rows distinguishable | ❌ **not met** — they print to the in-app message log, which needs the GUI |
| The prerequisite question has a written answer | ✅ met — `docs/developer/ppcp_prerequisites_developer_guide.md` |
| `min_rtt` / `offset_sigma` recorded for Linux | ❌ **not met** |

⚠ **Every unmet clause has the same single blocker and it is not code:** this box
holds no PPCP pairing, and one cannot be created without a person physically
scanning a QR on the phone.  Add to the list the L2 unplug-mid-request test,
which needs a hand on the cable.  All the code is written and everything
testable without a person passes — 89 tests, one intentional skip — but
**nothing here should be read as "reconnection works on Linux" until a phone has
actually reconnected.**

---

## What is shared, and must not be re-litigated per platform

⛔ These were decided and in several cases **measured**; a platform port is not
the place to reopen them.

| | |
|---|---|
| `PortNumber` is byte-swapped | Measured twice on macOS. The wrong order **connects to a different port** rather than erroring |
| Filter `ConnectionType == "USB"` | usbmuxd reports WiFi-paired devices too; treating one as wired invalidates every timing claim |
| `DeviceID` is per-attachment | Measured 306 → 308 on one phone, one cable, one day. The **UDID** is the identity |
| A blocking fd from `cfg.dial` **hangs for ever** | `handshakeTimeoutMs` never fires — the transport drives OpenSSL through `poll()` |
| The dial runs on its own thread, never the accept thread | `ppcp_host_service.cpp:242-296` |
| Destroy a `QSocketNotifier` before closing its fd | `:1205` |
| The retry is **flat 2 s**, not exponential | A ramp loses the race to the phone's own dial — measured |
| Never migrate a live link between transports | §7.3; a transport change is a NEW link with a fresh estimator |
| Bulk does not poison control | M3: 1750 MB moved, `min_rtt` unchanged. **Phase 3 is not justified** |

---

## Phase 3 — split-transport link · `[-]` NOT JUSTIFIED — M3 answered it

⛔ **M3 says it does not, so this phase is dropped rather than deferred.** 1750 MB
was pushed over the same USB pipe in 58.5 s while the sync trace ran and
`min_rtt` did not move by one part in ten thousand — 0.7645 ms before, during and
after, with no probe lost. §8 hoped min-RTT filtering would be *"robust to some
of this"* and said *"'some' is a measurement, not an assertion"*; measured, it is
robust to all of it.

⚠ **What would reopen it** — and only these: a device whose bulk demand is far
above the 6.25 MB/s the encoder sustains, or a backlog drain that measurably
lifts `min_rtt` on channel 0. Neither is speculation worth building against now.

Control on the cable, bulk on the radio; one link, two transports — legal under
`ENC` 2.1b. Kept for the record only.
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
| 2026-08-29 | — | Framing corrected after review: the Phase 0 run was an idle link on a quiet network, i.e. WiFi's best case. M1 split into idle (M1a) and **contended (M1b)** arms; M11 added for reliability; M10 now measures battery as well as thermal. Design doc gains §1.1 — contention, reliability, power and cross-platform reconnection, none of which Phase 0 touched. |
| 2026-08-29 | 1 | Contracts C1–C7 fixed and written above **before any agent started** — the dial seam and its fd ownership, `adoptLink`'s resolved pairing, the `WiredPresence` CBOR schema and its framing (there is none), the presence port `50915` and the loopback bind, one listener per held pairing, the wired hello sequencing, and test ownership. |
| 2026-08-29 | 1 | ⛔ **C6 is the contract that took tracing to find.** `ppcp_peer_hello()` auto-emits `link_bind` at `ppcp_peer.c:929` when `!listener && has_link_id`. The PPS transport already writes `link_bind` itself on every dial, so `listener=false` is safe **only** because `ppcp_peer_set_link_id()` is never called on the host. Set both and the host sends two bindings on channel 0. |
| 2026-08-29 | 1 | ✅ Checked and **no change needed**: a responder raises `HELLO` *and* `CONNECTED` (`ppcp_peer.c:2867`), so `ppcp_host_peer.cpp:204`'s declare-on-`CONNECTED` fires identically whichever end dialled. The wired inversion is invisible downstream of `hello`. |
| 2026-08-29 | 1 | ✅ **Re-verified the `PortNumber` byte-swap myself** against `/var/run/usbmuxd`, phone attached, before reviewing any agent code. Plist value **32498** (`= htons(62078)`) → `Number=0`; plist value **62078** native → `Number=3`. usbmuxd reads the field as already network-order and applies `ntohs`, so the native value dials 32498 and finds nothing. Design §4.2's table is correct as written. |
| 2026-08-29 | 1 | ⛔ **New trap: `DeviceID` is per-attachment and NOT stable.** The 29 Aug probe saw `DeviceID=306` for Mark's iPhone; the same phone, same cable, is `DeviceID=308` today (`SerialNumber` unchanged at `00008140-000864E426EB001C`, `ConnectionType=USB`, `ConnectionSpeed=480000000`). ⚠ So a `DeviceID` may only be used inside the lifetime of one `Attached`→`Detached` span — never persisted, never cached across a watch restart, never a map key that outlives the attachment. This sits beside design §10's existing "no `udid → pairingId` cache" rule and is a different trap from it. |
| 2026-08-29 | 1 | ✅ **Wave 1, PPS transport agent — landed and VERIFIED BY ME, not taken on report.** `ppcp_usbmux.{h,cpp}` (946+336 lines, Qt-free), the C1 `dial` seam, a stub usbmuxd and `ppcp_usbmux_test`. My own runs: `ctest -R 'ppcp_usbmux_test|ppcp_transport_test|ppcp_link_bind_test'` → **3/3 pass**, and the **app target links clean** with `ppcp_usbmux.cpp` compiled into it — the integration build the agent did not run. Diff stayed inside its owned files. |
| 2026-08-29 | 1 | ⚠ **One accepted deviation from C1, and it is better than the contract.** The `applyOptions()` family guard is `AF_INET \|\| AF_INET6 \|\| AF_UNSPEC`, not the two families I wrote. On Windows `getsockname()` fails with `WSAEINVAL` on an **unbound** socket, and `applyOptions()` runs immediately after `socket()` on both the dial and listen paths — so my literal contract would have silently disabled `TCP_NODELAY` on every Windows TCP socket. Treating "family unreadable" as "apply as before" means the guard can only ever remove a call that was guaranteed to fail. Contract C1 above is left as written with this noted; the code is right. |
| 2026-08-29 | 1 | ⛔ **Design §6.2 corrected: the diagnostics table asked for seven distinguishable causes and the transport can see six.** "`Connect` to the presence port → `Number=3`" and "`Connect` refused at the mux layer (trust not granted)" are **the same wire event**. I measured it: 62078 (lockdown, listening) → `Number=0`; **50915 (the presence port, nothing serving it) → `Number=3`**; port 1 → `Number=3`. Until **M5** runs on an *untrusted* device, "trust not granted" is not a diagnosis this host can honestly print. ✅ Incidentally: **50915 was free on the device**, so C4's port collides with nothing on a stock iPhone 16. |
| 2026-08-29 | 1 | ⛔ **A blocking fd from `cfg.dial` HANGS FOREVER — it is not a slow failure.** Found by the agent when a test handed over a blocking socketpair end: `Connector::connect()` parked in `read()` inside `SSL_do_handshake` and **`handshakeTimeoutMs` never fired**, because this transport drives OpenSSL through `waitFor()`/`poll()` and a blocking fd never returns to it. C1 states the requirement; nothing stated the consequence. `Usbmux::Client::dial()` therefore sets `O_NONBLOCK` itself rather than trusting its caller. |
| 2026-08-29 | 1 | ⚠ **A trap the design never mentioned, avoided by construction:** usbmuxd sends `Attached` for every already-present device *immediately* after the `Listen` result, on the same socket. A reader that recv's into a scratch buffer swallows them. `Watch::start()` reads exactly the 16-byte header plus exactly `length-16` bytes and never more, so the initial attaches survive into the first `poll()`. Pinned by a test. |
| 2026-08-29 | 1 | ⚠ **API note for the host-service agent:** `listDevices()` returns `Result::ok() == false` for both `NoDevices` and `NoWiredDevices` but **fills the vector in either case** — a `Network` device is still reported, because "a phone is here but it is on WiFi" must be sayable in the log. Do not read `!ok()` as "the vector is empty". |
| 2026-08-29 | 1 | Wave 1 launched: PPC agent (presence listener, per-pairing listeners, idle timer) ‖ PPS transport agent (`ppcp_usbmux`, the `dial` seam, stub usbmuxd). Different repos, different builds. |
| 2026-08-29 | 1 | ✅ **Wave 1, PPC agent — landed.** `WiredPresence` encoder in `Packages/Core` (testable with no app, no socket, no simulator), `WiredPresenceListener` on 50915, one `PpcpListener` per held pairing on ephemeral loopback ports, `isIdleTimerDisabled`. Reported: `swift test --filter WiredPresence` 12/12, `make test-core` 321/321, `make test-app` 134/134 (10 pre-existing known issues). ⚠ Four builds, not the 2–3 the economy rule asks — one was a cold build that tripped the Makefile's 600 s guard. Not yet verified by me; PPC has no cable-side integration I can run until both halves exist. |
| 2026-08-29 | 1 | ⛔ **Contract C3 was WRONG and is amended — my error, found by the device agent.** See C3 above: `pv, role, dl, peers` does not sort under `ENC` 4e, and emitting it needed `PPCP_CBOR_ORDER_LITERAL`, which `cbor.h` says "exists for two reasons and no others". Verified in `libppcp/include/ppcp/cbor.h` before accepting. New order `dl, pv, role, peers` needs no escape hatch and costs the host nothing. ⚠ The unlabelled record was deterministic all along, so **only the labelled case was affected — which is exactly how this would have survived a casual test.** |
| 2026-08-29 | 1 | ⛔ **C6 addendum: `declare` moves to `connected` too, and it fails SILENTLY.** On the wired path the former dialler no longer originates first, so there is no agreed wire version at `open()` — `declare`, the sync timebase registration and the connect burst all wait for `connected` alongside `hello`. Verified: `ppcp_peer.c:978` promotes `INIT→DECLARED`, so a premature `declare` is **accepted**, skips version agreement, and looks fine. ✅ The host is already correct (`ppcp_host_peer.cpp:204`); the instruction is *don't "simplify" it*. |
| 2026-08-29 | 1 | ⛔ **`NWListener.cancel()` returns BEFORE the port is free — a real defect on a FIXED port.** Phase 2's "refresh on foreground entry and on pairing-set change" would `stop()` then `start()` and get `EADDRINUSE`, which is **indistinguishable from the port collision §5.3 says to treat as "not wired"** — so the device would silently stop being reachable on the cable after its first background trip. `stop()` now waits for `.cancelled` with a 2 s backstop plus `allowLocalEndpointReuse`. Found because two tests hit it, not by reading. |
| 2026-08-29 | 1 | ✅ **§5.3's loopback bind measured, with a negative control.** The record reads back on `127.0.0.1:50915` and every non-loopback IPv4 refuses — paired against a deliberately `0.0.0.0`-bound control listener that *must* be reachable, so "a firewall ate it" cannot masquerade as a correct bind. Same for every per-pairing listener. ⚠ Simulator only (the Mac's own stack, so the test is real) — **not yet on a cable**. And it raised **M12**, above. |
| 2026-08-29 | 1 | ⚠ **Two seams had to be opened in `PpcpListener` and both were unavoidable.** `registeredIdentity()` — §5.2's whole argument is "publish what you registered", and it was unimplementable without a read-back — and a `loopbackOnly` flag. Recording it because the design assumed both already existed. |
| 2026-08-29 | 1 | ⛔ **§6.5 (a first pairing born on the cable) is NOT reachable from the app as written.** C3 says the record lists persisted pairings *plus any scanned, not-yet-connected code*, but nothing in PinPointCapture holds such a code: `RendezvousCoordinator.scan()` derives keys and dials immediately, so nothing survives to be published. The `HeldPairing` API supports it; the scan flow would have to publish-then-wait. **Left undone — it changes a user-facing path and is out of proportion to Phase 1.** §6.5 should be re-scoped or moved to Phase 2. |
| 2026-08-29 | 1 | ✅ **C3 reorder landed on the device and VERIFIED BY ME with an independent CBOR decoder**, not taken on report. Both fixtures fully consume with no trailing bytes; keys are in `ENC` 4e order; the unlabelled record is `a3`/map(3) with `dl` genuinely **absent**, not null; both identities are exactly 17 bytes and start `0x01`. `PPCP_CBOR_ORDER_LITERAL` no longer appears on this path. Canonical fixtures, now held by both repos: labelled 125 B `a462646c6d4d61726b…`, unlabelled 68 B `a362707663312e30…`. |
| 2026-08-29 | 1 | ⚠ **A fixture property worth keeping deliberately: neither test identity is valid UTF-8, and one ends in `0x00`.** That is what catches a reader that transcodes, validates as text, or treats the 17 octets as a NUL-terminated string (`RV` 5.3f) — an all-ASCII fixture never would. Both orderings were handed to the host agent so the order-agnostic rule is actually exercised; ⛔ the **unlabelled record alone proves nothing about ordering**, since it encodes identically under either rule. That is precisely how the ordering defect survived on the device side, and it is why the two fixtures are a pair. |
| 2026-08-29 | 1 | ✅ **Wave 2, PPS host-service agent — landed and VERIFIED BY ME.** New `ppcp_wired_link.{h,cpp}` holds the whole wired orchestration; `adoptLink()` takes C2's resolved pairing; `listener` threaded to `ppcp_peer_hello()` per C6. My own runs, not its report: `ppcp_host_service_test` **isolated** (per the advertise-suite poisoning note) → pass, 34 tests; `ppcp_host_peer_test`, `ppcp_app_tu_syntax`, `ppcp_link_bind_test`, `ppcp_transport_test`, `ppcp_usbmux_test`, `ppcp_rendezvous_test` → **6/6 pass**; app target links. All three PPC fixtures are parsed verbatim by the host reader — **the two halves now agree at the byte level without a cable**, which retires part of design §12's largest estimate risk. |
| 2026-08-29 | 1 | ✅ **Checked the two claims that would have failed silently.** (1) `m_wired` is declared *after* `m_rv` (`:760` vs `:666`), so it destructs **first** and the worker joins before the rendezvous whose `Impl *` its `identityResolver()` captured. (2) `identityResolver()` is genuinely thread-safe — it takes `impl->mu` for its whole body and **is already called off the GUI thread today**, on the accept thread via `setIdentityResolver()`. The wired path is the second caller of a callback that was designed for one. |
| 2026-08-29 | 1 | ⛔ **I fixed the one defect the agent flagged and ran out of build budget for: a ~10 s hang on quit.** `stop()` joins the worker, which could be inside `Connector::connect()` at the default `handshakeTimeoutMs` of 10 s — quitting mid-dial froze the GUI thread with no window and no explanation, the classic "it hung on exit". Two changes: `kWiredHandshakeMs = 3000` on the wired path only (min_rtt over usbmux is ~1 ms, so a handshake still unfinished after 3 s is broken, not busy), and a `stopping()` check before **each** of the two phases that can block — the presence dial-and-read, and the TLS connect. Re-verified: `ppcp_host_service_test` passes, app links. |
| 2026-08-29 | 1 | ⚠ **A coupling to watch, reported by the agent rather than hidden.** "We dialled" is derived from `!resolvedPairingId.isEmpty()`, because C2 fixes the signature and that is the only in-band signal. It decides **three** things at once: 7.3a spend accounting, the `listener` flag, and whether `hello` is sent. Documented at `ppcp_host_service.h:594`. ⛔ If a future non-dialling caller ever passes a resolved pairing, all three break together and none of them loudly. Worth a real `enum class Origin { Accepted, Dialled }` if a third caller ever appears. |
| 2026-08-29 | 1 | ⚠ **Phase 1 is code-complete on both hosts and NOTHING HAS CROSSED A CABLE.** The stub usbmuxd's tunnel is a byte echo, not a PPCP listener, so `runJob()`'s dial→handshake steps are reached only on hardware. **M12 runs first** — if the device-side mux dials `::1` the presence read fails silently and every later measurement chases the wrong fault. Then M1a/M1b, M3, M10. Closing the last gap in the suite would need a "forward the tunnel to this local port" mode in the stub. |
| 2026-08-29 | 1 | ⛔ **DEFECT FOUND WHILE SETTING UP M3, and it is worse than the measurement it interrupted: the wired path only ever fires on a usbmux `Attached` event, so in the MOST COMMON REAL SEQUENCE IT NEVER FIRES AT ALL.** Observed live: phone already on the cable, host started, one presence read at `t+3 s` refused with `Number=3` (the app was backgrounded), **and the host never looked again**. When the capture app was foregrounded a minute later and began serving presence, nothing retried — and the phone's own WiFi dial won the link (`transport=wifi`). Exactly one wired attempt was made in the whole run. ⚠ *"Phone left plugged in to charge, operator opens the app"* is the normal way this product will be used and it produces **no `Attached` event**, so wired would essentially never engage in the field. ✅ The fix is a periodic re-probe with backoff for devices that are attached but unresolved — §6.1 rule 4 says *"presence unreadable → change nothing"*, which forbids disturbing WiFi and does **not** say "never look again". **Fix before wired is enabled by default.** ✅ **FIXED same day — see below.** |
| 2026-08-29 | 1 | ✅ The §6.2 diagnostics line earned its keep the moment it mattered: the refusal printed *"the capture app is not running or not in the foreground (or this computer is not trusted; M5 pending)"* — naming both causes of `Number=3` per the correction made earlier today, rather than guessing at one. That single line is what identified the defect above in one read instead of a debugging session. |
| 2026-08-29 | 1 | ✅ **Retry gap FIXED and proven on hardware against the exact failure.** A 1 s tick re-probes every device that is attached, unresolved and not already dialling; state is attachment-scoped (created on `Attached`, erased on `Detached`, DeviceID refreshed on replug); `dropPhone()` calls `retryNow()` so a link *ending* re-arms the cable, which no `Attached` event would ever have done. §6.1 rule 1 still guards the dial, so nothing double-connects. Verified: app killed → host started → **one** log line → app launched → **`transport=usb` one second later**, one link, zero drops, sync healthy. |
| 2026-08-29 | 1 | ⛔ **The first fix was measured and FOUND WANTING — an exponential backoff LOSES the race, and the flat cadence is not a tuning preference.** With 2→4→…→30 s: the operator opened the app, **the phone dialled over WiFi 4 s later**, and the host's next wired probe was up to 30 s away — so WiFi took the link and the cable never got a look in (observed `transport=wifi`). A probe is one `connect()` on a local unix socket plus a refused plist round trip, so **flat 2 s** costs nothing worth measuring and wins. ⚠ Recording it because the ramp looked obviously right and was obviously wrong the moment it met the thing it was racing. |
| 2026-08-29 | 1 | ⛔ **§6.1 HAS A CHICKEN-AND-EGG THE DESIGN DOES NOT ACKNOWLEDGE, and the retry only narrows it.** Rule 2 suppresses a pairing's advertisement *once wired is proven*; rule 4 forbids suppressing on mere attachment (*"plugging a phone in to charge must never disturb WiFi"*). But presence cannot be proven until the capture app is up — and the instant it is up, the phone's own reconnect dials WiFi. **So the host can never legally suppress before the phone has already dialled.** Flat 2 s wins the race in practice, but it is a race and not an arbitration. ⚠ **Phase 2 must resolve this explicitly**, and the honest reading is that §6.1's *"Use cable"* button is the RELIABLE path rather than the fallback it is described as. |
| 2026-08-29 | 1 | ⚠ **Log discipline is part of the fix, not decoration.** A phone plugged in only to charge refuses for ever; a line per attempt would be a line every 2 s for the life of the session, which is how the one-log rule becomes noise nobody reads. So the worker no longer logs a retryable failure — it posts the OUTCOME to the GUI thread, which prints only when the outcome CHANGES. Measured: 3 `[ppcp-usb]` lines across a 75 s run containing dozens of probes. |
| 2026-08-29 | 2 | ✅ **§6.1's deadlock RESOLVED — by Mark, and the answer was to stop racing.** The rule set could not be satisfied: rule 2 may suppress the advertisement only once wired is *proven*, rule 4 forbids suppressing on mere attachment, proof needs the capture app up, and that is the same instant the phone dials. Mark's resolution: **let WiFi connect, dial the cable anyway, and drop the WiFi link once the cable is up.** ⚠ It is NOT the migration §7.3 forbids — nothing carries a clock fit across; the cable link is a fresh `ppcp_peer` with a fresh estimator and the WiFi link is destroyed, which §7.3 names as the honest form. And dialling *before* dropping is better than the existing "Use cable" button, which drops first and leaves a gap. |
| 2026-08-29 | 2 | ⛔ **The takeover costs a MEASURED 35 s and that is why it is gated.** A new cable link's `offset_sigma` runs 8.4 → 36.7 → 28.1 → 9.7 ms and does not fall under the 5 ms arbitration gate until **t+35 s**, so a shot crossing in that window cannot be arbitrated at all. Free at the start of a session, unacceptable mid-session. `PeerLinkState::WifiBusy` therefore leaves a working link alone, and "busy" is deliberately generous — **any** shot-bridge activity or import traffic counts. Mark accepted the 35 s gap for the idle case rather than holding both links until convergence. |
| 2026-08-29 | 2 | ✅ **No `PinPointCapture` change was needed, which is what made this the cheap option.** The phone re-dials only *"on the app becoming active while at least one pairing is held and no link is up"* and never in the background (`ReconnectCoordinator.swift`), so once the cable link exists, dropping WiFi does not bring it back — the host simply closes it and the phone stays put. |
| 2026-08-29 | 2 | ⛔ **`NoMatch` now STOPS the retry, and that is a correctness fix rather than an optimisation.** "None of this phone's pairings is one of ours" is a settled answer that changes only on unplug or re-pair; re-asking every 2 s was pointless work, and it would strand a phone cabled to a Studio it is not paired with under any future device-side deference. |
| 2026-08-29 | 2 | ⚠ **A loop that only the pre-dial check prevents, written down because it is not obvious.** `dropPhone()` calls `retryNow()`, and a takeover drops the WiFi phone — so the entry for the phone just connected over the cable has its `linked` flag cleared moments before the cable link is adopted. What stops an immediate re-dial is the pre-dial `PeerLinkState` check in `onRetryTick()`, which by then answers `Wired`. ⛔ Remove that check as an "optimisation" and a takeover re-dials itself in a loop. |
| 2026-08-29 | 2 | ⚠ **The takeover path is implemented and reviewed but NOT exercised live, and the reason is a good one.** With the flat 2 s retry the cable now wins the race outright — **3 runs out of 3**, `transport=usb` every time — so WiFi never got in first and the takeover never fired. The case it exists for is §6.1's own: *the cable plugged in DURING a live WiFi session*, i.e. the operator who plugs in **because** WiFi was bad. ⛔ That needs someone physically at the machine to plug a cable in mid-session; it cannot be produced over VNC. **Still to be proven on hardware.** |
| 2026-08-29 | 2 | ✅ **§6.1's duplicate-link backstop SHIPPED, and it is the last correctness hole.** `onDeclare()` closes a newcomer whose `counterpartId` matches a live phone, keeping the incumbent. Two tests: one phone declaring twice collapses to one link; **two genuine devices sharing a `mu:2` pairing both survive** — the regression guard for keying on the pairing instead of the counterpart, which is the mistake the design names and which would take down the phone that arrived first. |
| 2026-08-29 | 2 | ⛔ **A trap I walked into and had to back out of, worth recording because the backstop would have caused the outage it exists to prevent.** My first version registered the newcomer's cameras and set its `counterpartId` before the duplicate check. But **everything downstream of a declaration is keyed on the COUNTERPART, not the Phone** — the camera registry, the timebase mappings, the offer list — and a duplicate shares its counterpart id with the incumbent *by definition*. `dropPhone()` would then have called `detachAll(counterpartId)` and torn out the **incumbent's** cameras. ✅ Fixed by running the check *before* `registerPpcpPeer()` and before `counterpartId` is assigned: leaving it empty is what makes the close inert, since every counterpart-keyed teardown in `dropPhone()` is guarded on it. |
| 2026-08-29 | 2 | ✅ **The gate is gone.** Verified with **no environment variable set**: `wired path armed`, `transport=usb` one second after the phone app activated. `PINPOINT_PPCP_WIRED=0` remains as an escape hatch and only the exact value `0` closes it. |
| 2026-08-29 | 2 | ⚠ **Two of my own diagnoses were wrong today and Mark caught one of them; recording both so neither is repeated.** (1) I claimed a link drop was the phone auto-locking. **Measured: false** — the presence port answers continuously for 2+ minutes with no link and no host, so the listener does not stop on its own. (2) I had **not** re-deployed `PinPointCapture` after the transport-row change and was testing an older build without noticing. ⛔ **Deploy after every PPC change before drawing any conclusion from device behaviour.** |
| 2026-08-29 | 2 | ⚠ **One unexplained observation, left as an observation.** Immediately after a fresh `make deploy`, the host got `Number=3` for 90 s and no link; a manual relaunch fixed it instantly and presence has been rock solid since. Cause unknown — a fresh install is a one-off condition, not the steady state. ⛔ **Do not invent a third theory for it**; watch for it recurring and capture the device log if it does. |
| 2026-08-29 | 2 | ✅ **Transport is visible in BOTH apps, verified on the device screen and not merely in code.** ⛔ The device-side bug Mark found is the one that mattered: `CaptureScreenStyle.symbol(for:)` returned `"wifi"` for **every** live state, so a cabled phone drew a Wi-Fi glyph — not uninformative but *wrong*, in the one place a golfer looks. Now `cable.connector` (already an approved glyph) when the link is wired. A/B on the same phone a minute apart, screenshotted via `devicectl capture screenshot`: cable → plug, WiFi (forced with `PINPOINT_PPCP_WIRED=0`) → arcs. ✅ That run also confirms **the escape hatch works**. |
| 2026-08-29 | 2 | ⚠ **And the PPS home screen said "0 Hz" for a phone**, because `phones()` sets `dataRateHz` to zero on purpose — *"a phone is not a Source; its CAMERAS carry the bytes, and claiming a rate here would be inventing a second number for the same bytes"* — so the cell was rendering a measurement that does not exist. It now shows the transport, in accent for the cable. The data was already in that list and simply never drawn. |
| 2026-08-29 | 2 | ⛔ **PROCESS LESSON, and Mark had to ask twice.** I reported the transport as "shown in both apps" having only added a text row in a sheet and a pill in a settings panel — neither of which is where an operator actually looks — and I had **not looked at the phone's screen at all**. `xcrun devicectl device capture screenshot --device <id> --destination <png>` works and is the way to verify a device UI. ⚠ **A UI claim is not verified until the pixels have been seen.** Reading the QML/Swift and watching the tests pass proves the binding compiles, not that anybody can see it. |
| 2026-08-29 | 2L | ✅ **Linux port opened. Two build breaks fixed FIRST — main did not compile on Linux at all.** (1) `shot_processor.cpp:746,748` assigned `std::vector<qint64>` to `std::vector<int64_t>`: same width everywhere, but on LP64 Linux that is `long` vs `long long`, two distinct types. macOS and MSVC make both `long long` and never see it, which is how it reached main from `0c7d128`. Fixed at the Qt/Buffer boundary with element-wise `assign()`, because `deferred_stitch.h` is deliberately Qt-free and is compiled by the standalone Buffer suite. (2) `camera_manager.cpp:611` guarded a block with `HAVE_PPCP` whose `CameraInstance` members exist only under `HAVE_PPCP_TRANSPORT` — every other guard in that file was already `_TRANSPORT`. Only bites on a libppcp-without-OpenSSL box, which is what this one was. Commits `9e483ad`, `a1ef796`. |
| 2026-08-29 | 2L | ⚠ **This box had no OpenSSL, so the PPCP transport had never been built here at all.** `PP_OPENSSL_FOUND` was false and `HAVE_PPCP_TRANSPORT` undefined; CMake's designed warning (*"PPCP transport NOT built … no capture-device link"*) had been firing unread. After `libssl-dev`: `PPCP transport: OpenSSL 3.5.5, libppcp 0.1.0 (a9785bb, local)`, and all **22** `src/Ppcp/` sources compile, `ppcp_usbmux.cpp` and `ppcp_wired_link.cpp` included. Full app build **≈8 min** at `--parallel 4` (the box OOMs above ~4 jobs). Both sibling libs resolved locally and were already current — no `libwrist`/`libppcp` update was needed. |
| 2026-08-29 | 2L | ⛔ **THE BRIEF'S HEADLINE IS EASILY MISREAD AND COST TIME: "no WiFi reconnection at all" DOES NOT MEAN WiFi IS BROKEN ON LINUX.** Pairing works on Linux today and always has. The QR carries the host's endpoints — `publishCode()` calls `reachableEndpoints(m_port)`, whose comment says it outright: *"This is what makes the code work when discovery does not"* — and `reachableEndpoints()` has a POSIX `getifaddrs` branch. The phone dials that endpoint; the host is the listener; no mDNS is involved. What is missing is **reconnection**, i.e. doing it again without a fresh QR (`maxUses = 1`). Stated here because §3.6b already says *"the pairing code path is unaffected"* and the Phase 2L summary reads as though it contradicts it. |
| 2026-08-29 | 2L | ✅ **Scope narrowed by reading the DEVICE side: only the ADVERTISER is on the WiFi-reconnect critical path.** `ReconnectCoordinator.swift:5` — *"PinPointStudio advertises and this device dials. Always"* (3.5d), and there is deliberately no listener on the phone for WiFi. PPS's browser **dials nothing**: `decideDial`'s result only populates `m_seenInstances` → `phonesChanged()`, and `noteAdvertisement` collects guided-pairing candidates that *"NOTHING dials"*. ✅ Mark chose advertiser **+** browser anyway, for parity and because both come from the same link — which also dodges a landmine: `ppcp_advertise_test.cpp:601` has an **unguarded** `ASSERT_TRUE(br)` on `makePlatformBrowser()`, so a half-port turns that test from a skip into a hard failure. |
| 2026-08-29 | 2L | ✅ **Port surface is smaller than the brief feared: the two FACTORIES only.** `ppcp_discovery.cpp` is the sole file in `src/Ppcp/` containing `__APPLE__` (7 occurrences, 5 guarded regions, ~236 lines of which ~215 are the two backend classes). `ppcp_transport.cpp`, `ppcp_rendezvous.cpp` and `ppcp_usbmux.cpp` contain **zero**. `startDiscovery()`/`startAdvertising()` are already called unconditionally (`ppcp_host_service.cpp:337-338`) and return early on a null factory, so nothing above the factories changes. ⚠ **There is currently no dns_sd/Bonjour/Avahi linkage anywhere in the CMake tree** — on macOS the symbols come free from libSystem — so this adds the **first** discovery-library dependency. Windows (W4) faces the same question without Avahi's shim. |
| 2026-08-29 | 2L | ✅ **L2 scoped correctly and it is NARROWER than the brief implies.** `ppcp_transport.cpp:752` and `ppcp_bootstrap.cpp:461` already pass `MSG_NOSIGNAL`, so the WiFi and bootstrap paths are SIGPIPE-safe on Linux today. **Only `ppcp_usbmux.cpp` is exposed**: `setNoSigPipe()` (:369) compiles to `(void)s` off Apple and the two sends at **:465, :468** pass flags `0`. ⚠ I first reported "no SIGPIPE handling anywhere in `src/`" — wrong, because `grep SIGPIPE` does not match `MSG_NOSIGNAL`. |
| 2026-08-29 | 2L | ✅ **L1 largely already satisfied on this box, and Mark's answer is SUPPORTED CONFIGURATION.** `usbmuxd` active; `/var/run/usbmuxd` is `srw-rw-rw-` root:root — **0666, so the brief's `usbmux`-group warning does not apply on Ubuntu**, though distro variation is real and that is the caveat to document. A lockdown trust record already exists for UDID `00008140-000864E426EB001C` (per-host, and independent of the phone's separate pairing with the M4 mac mini). ⚠ **New BUILD prerequisite to document alongside the runtime one:** `libavahi-compat-libdnssd-dev` for mDNS, distinct from `usbmuxd` which is runtime. |
| 2026-08-29 | 2L | ✅ **L2 FIXED AND THE MECHANISM PROVEN, though not yet on a cable.** `sendAll()` (`ppcp_usbmux.cpp`) now passes `MSG_NOSIGNAL` via a `PP_SEND_FLAGS` macro — the same pairing `ppcp_transport.cpp:752` and `ppcp_bootstrap.cpp:461` have always had, which is exactly why those two paths were never exposed. ⛔ **Deliberately NOT `#else` on `SO_NOSIGPIPE`:** the two are independent, a platform may have both, and passing the flag when the option is already set is harmless. Standalone probe, socketpair with the peer closed: `SO_NOSIGPIPE: NOT AVAILABLE` · `send(...,0) -> KILLED BY SIGPIPE` · `send(...,MSG_NOSIGNAL) -> survived`. ⚠ **Existing coverage was Apple-only** — `ppcp_usbmux_test.cpp:323` is inside `#ifdef SO_NOSIGPIPE`, so it compiles away on the one platform that needed it, and there is still no repo test. A deterministic one is hard (the kernel buffers the first write; SIGPIPE fires on the second, after the RST), so no flaky test was added. |
| 2026-08-29 | 2L | ✅ **L3 mDNS PORTED — five preprocessor guards and a CMake probe, no second backend.** `ppcp_discovery.cpp` derives one macro, `PP_DNS_SD_AVAILABLE = __APPLE__ \|\| PP_HAVE_DNS_SD`, and the five `#if defined(__APPLE__)` sites key off it; `BonjourBrowser` and `BonjourAdvertiser` are compiled **verbatim** on Linux against Avahi's compat shim. ⚠ One derived macro rather than the disjunction repeated five times — the two backends are either both compiled or both absent and there is no configuration where that is untrue. Verified in the linked binary: **33 `Bonjour*` symbols** and all **seven** `DNSService*` calls resolving to `libdns_sd.so.1`. Build clean, `--parallel 4`, zero errors. |
| 2026-08-29 | 2L | ⚠ **The CMake probe is the first DNS-SD dependency in the tree and it is a CAPABILITY, not a prerequisite.** `pkg_check_modules(DNSSD avahi-compat-libdns_sd)` with a `find_path`/`find_library` fallback, inside the existing `if(PP_OPENSSL_FOUND AND TARGET ppcp)` block and guarded `if(NOT APPLE)` since libSystem supplies it there. Absent ⇒ no define, both factories keep returning null, and it prints a **STATUS line and never a warning** — 3.6b makes the consequence silent, and the pairing code carries its own endpoints. New configure lines: `PPCP discovery: DNS-SD via dns_sd`. |
| 2026-08-29 | 2L | ⛔ **A GAP THAT WOULD HAVE MADE THE PORT LOOK TESTED WHEN IT WAS NOT.** `src/Ppcp/tests/` is a standalone build that inherits nothing from the app's CMake, and **four** of its targets compile `ppcp_discovery.cpp`. Without its own probe it would have compiled both backends out, the factories would have returned null, and the two real-responder tests would have gone on skipping — a green suite proving nothing. The same detection is now in `src/Ppcp/tests/CMakeLists.txt` at directory scope, and `${DNSSD_LIBRARIES}` was added to `_rv_link` (empty on Apple, so that file's *"DNSServiceRegister lives in libSystem and needs no framework"* comment stays true where it was written). |
| 2026-08-29 | 2L | ⛔⛔ **THE PORT WAS NOT FIVE GUARDS. `ppcp_discovery.cpp` HELD A DEADLOCK THAT macOS CANNOT EXPRESS.** `onBrowse()` resolved each instance inline with a blocking `DNSServiceProcessResult`, justified by a comment reading *"off the main thread by construction because process() is called from wherever the owner watches fd()"*. ⚠ **The owner is the GUI thread** — `startDiscovery()` puts a `QSocketNotifier` on it — so that sentence was wrong on every platform and merely harmless on one. Under Avahi it never returns: `ppcp_advertise_test` hung for its full 300 s with the main thread in `unix_stream_data_wait` (`/proc/<pid>/wchan`; gdb could not attach, `ptrace_scope=1`). ✅ Isolated with a standalone probe: the identical resolve **deferred out of the callback succeeds** (`err=0 host=MarksMBP.local. port=47788`), so the shim will not service a resolve re-entrantly. Shipped on macOS this would have frozen the app the first time a phone was discovered. |
| 2026-08-29 | 2L | ⛔ **AND A DEFERRED RESOLVE IS NOT ENOUGH — IT TAKES 650 ms, MEASURED.** Far too long to hold the GUI thread, so off Apple the resolves are now genuinely asynchronous. ✅ The `RvBrowser` interface is unchanged: `fd()` returns an **epoll set** holding the browse socket plus every pending resolve socket, so the owner's single `QSocketNotifier` drives both — N fds in, one fd out. macOS keeps the inline path verbatim (mDNSResponder answers sub-millisecond), so the working platform carries no risk from this. |
| 2026-08-29 | 2L | ⛔ **A SECOND TRAP INSIDE THE FIRST: A RESOLVE IS NOT DONE AFTER ONE `ProcessResult`.** Avahi wakes the resolve socket several times before it delivers, so retiring the ref on the first wakeup destroys the resolve before it answers — and the symptom is the honest-looking one: a browse that finds instances and resolves none of them. Found only by instrumenting the chain (`onBrowse` fired, `onResolve` never did, and the fd number was being reused). `Pending::done` is now set by `onResolve` and nothing is retired until it is, with a 10 s sweep so an instance that never answers cannot accumulate. |
| 2026-08-29 | 2L | ✅ **L3 DONE, AND THE PROOF IS THE TEST THAT COULD NEVER RUN BEFORE.** `PpcpAdvertise.RegisteringWithTheRealResponderIsVisibleToARealBrowse` registers with the real responder and browses for its own advertisement; it skipped on Linux for want of a backend, then hung, then skipped again on a 10 s deadline, and now **passes**. Suite time fell 10031 ms → **950 ms** because it resolves instead of timing out. Final: `ppcp_advertise_test` 14 passed / 1 skipped (env-gated hold), `ppcp_rendezvous_test` **24/24**, `ppcp_usbmux_test` **15/15**, `ppcp_host_service_test` **36/36** — 89 passed, one intentional skip. ⚠ Run individually with gaps; advertise→host_service back-to-back was NOT attempted, so the recorded flake is neither reproduced nor refuted here. |
| 2026-08-29 | 2L | ⚠ **Test-side `__APPLE__` guards had to be widened too, or the new capability would have been unasserted.** `ppcp_rendezvous_test.cpp:678` and `ppcp_host_service_test.cpp:624` gated their `ASSERT_TRUE(browser)` / `"browse only"` checks on Apple. ✅ Also note `ppcp_rendezvous_test.cpp:694`'s `if (!b) return;` — a **false green** that passed while asserting nothing on Linux, and is now real coverage. `describe()` returns "DNS-SD via Avahi compat (browse only)" off Apple; the "browse only" substring is load-bearing and preserved. |
| 2026-08-29 | 2L | ✅ **The wired path IS live on Linux, proven from OUTSIDE the app.** With PPS running and the phone attached, `journalctl -u usbmuxd` shows `device_control_input` every **2.0 s** — the flat 2 s retry cadence exactly, arriving from an independent observer rather than from our own log. PPS ran 50+ s of continuously-refused presence probes without dying, which is the `sendAll` path exercised hundreds of times. ⚠ **This is not the L2 hardware test**: the probes are refused cleanly (`Number=3`, the capture app is not running), and SIGPIPE needs the socket to close *mid-write*. |
| 2026-08-29 | 2L | ⚠ **L1 CLOSED: SUPPORTED CONFIGURATION (Mark's call).** Written up in the new `docs/developer/ppcp_prerequisites_developer_guide.md` — build deps (OpenSSL, `libavahi-compat-libdnssd-dev`), runtime deps (`usbmuxd`, `avahi-daemon`), diagnostic tools, what each configure line means, the stale-CMake-cache trap, and a capability matrix for what still works with each piece missing. ⛔ **The distro caveat is the real content**: Ubuntu's `/var/run/usbmuxd` is `0666` so no group is needed, and that is packaging rather than a guarantee. |
| 2026-08-29 | 2L | ⛔ **WHAT IS STILL UNVERIFIED, AND IT IS THE HALF THAT NEEDS A HUMAN.** No PPCP pairing exists on this box and one cannot be made without physically scanning a QR, so **neither reconnect path has been end-to-end tested**: (1) WiFi reconnect — the advertiser is proven by unit test against the live responder, but `startAdvertising()` publishes **persisted pairings only** (`ppcp_discovery.h:252`, *"0 pairings — nothing to advertise"*), so `avahi-browse` correctly shows nothing and the loop was never closed with a real phone. (2) Wired reconnect — same blocker. (3) L2's unplug-mid-request. (4) `min_rtt`/`offset_sigma` for Linux, still owed. ⚠ **Do not read the green suite as "reconnection works on Linux"** — read it as "every part we can test without a person passes". |
| | | ⚠ **Next session starts here.** Read design doc §1, §1.1 and §7.1, then Findings above. Phase 1 proceeds; the accuracy argument alone no longer carries it, and ~~M1b is the measurement that matters~~ — **withdrawn; M1b was dismissed as circular**. |
