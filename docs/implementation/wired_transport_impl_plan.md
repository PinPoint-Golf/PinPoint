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

## Phase 1 — the tunnel, macOS only · IN PROGRESS

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
| `[~]` | — | **Mine, not an agent's.** ✅ M1a, M2, M12 done; ⛔ **M1b DISMISSED as circular (Mark, 29 Aug) — do not re-run.** **M1a/M1b** (`min_rtt` wired vs WiFi, Phase 0 fix in place), **M10** (sustained capture before thermal throttle, cabled vs not — ⚠ screen for issue #101's ~8.8 s gap signature first), **M3** (does bulk poison control), **M5** (does `Connect` need device trust — needs an *untrusted* device). |

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
| | | ⚠ **Next session starts here.** Read design doc §1, §1.1 and §7.1, then Findings above. Phase 1 proceeds; the accuracy argument alone no longer carries it, and ~~M1b is the measurement that matters~~ — **withdrawn; M1b was dismissed as circular**. |
