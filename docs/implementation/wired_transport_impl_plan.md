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
| `[~]` | PPS | **Windows and Linux — now their own briefs: see §Phase 2W and §Phase 2L below.** ⚠ Linux is likely a *prerequisites* problem rather than a code one (the `AF_UNIX` client already compiles and runs there); Windows needs a real provider. ⛔ And §6.2's table asks for seven distinguishable causes when the transport can only see **six** — measured. ✅ **Both now code-complete** (Linux fully verified on hardware 30 Aug; Windows code-complete 31 Aug, hardware verification still owed — no AMDS, no Bonjour SDK, no phone on that box). |
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

### W1 — the usbmux provider · `[x]` DONE 31 Aug 2026 — code complete, unverified against real AMDS

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

### W3 — the tests are currently excluded on Windows · `[x]` DONE 31 Aug 2026

`src/Ppcp/tests/CMakeLists.txt:284` wraps the `ppcp_usbmux_test` target in
`if(NOT WIN32)`, and the dial-seam rows in `ppcp_transport_test.cpp` are
`#ifndef _WIN32`. The reason is the **stub usbmuxd is an `AF_UNIX` server** and
there was nothing to impersonate on Windows.

Porting W1 removes that reason. Give `usbmuxd_stub.h` a **TCP mode** (listen on
`127.0.0.1:0`, hand the port to the `Provider`), then drop both guards. ⛔ Do not
ship the Windows provider with its tests still excluded — the wire format is
identical, so an untested Windows provider is untested for no reason.

### W4 — mDNS, and it is a separate piece of work · `[x]` DONE 31 Aug 2026 — VERIFIED AGAINST THE REAL BONJOUR SDK, live

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

✅ **STATUS 31 Aug 2026 (UPDATED same day, twice) — WiFi RECONNECT MET ON REAL HARDWARE.**
A phone paired by QR, then reconnected over WiFi with no QR rescanned — the
first time this has ever happened on Windows, and it took a real host-side bug
found and fixed live to get there (see the Log: `Entry::remember()` left a
QR-redeemed pairing permanently exhausted; one-line fix, confirmed by
reconnecting again). AMDS is now also installed and W1 verified against the
real service (no-phone probe). **NOT YET MET: this specific criterion is about
the WIRED/cabled path with no QR at all** — no phone has been physically
cabled to this box this session, so the ⛔ `PortNumber` byte-swap,
`ConnectionType`, and `min_rtt`/`offset_sigma` against a real device over USB
remain unverified. This mirrors Phase 2L's own arc exactly: the code is
written and everything testable without a person passes. ✅ Unlike the rest of
this file's caveats of that shape, this one is now HALF discharged rather than
fully open — WiFi reconnection is proven with a phone, on this box; wired is
the remaining half.

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

### L2 — ⛔ SIGPIPE will kill the process on Linux · `[x]` VERIFIED ON HARDWARE 30 Aug 2026

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

### L3 — mDNS on Linux · `[x]` VERIFIED WITH A PHONE 30 Aug 2026

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

✅ **STATUS 30 Aug 2026 — FOUR OF FOUR MET.  ONE TEST STILL OWED (L2), SEE BELOW.**

| Clause | |
|---|---|
| A persisted pairing reconnects over the cable, no QR | ✅ **met** — twice, and cleanly on 30 Aug (1 up / 0 down) |
| §6.2 rows distinguishable | ✅ **met in practice** — three rows observed firing correctly and distinguishably: `no usbmux provider … (errno=111)`, `1 device(s) attached, 1 on a cable — usbmux: ok`, and `no presence record … (Number=3)`. ⚠ Not an exhaustive walk of the table; `ConnectionType == "Network"` and the unresolvable-presence row are still unobserved |
| The prerequisite question has a written answer | ✅ met — `docs/developer/ppcp_prerequisites_developer_guide.md` |
| `min_rtt` / `offset_sigma` recorded for Linux | ✅ **met** — cable **0.902 / 0.694 ms**, WiFi **3.166 / 2.280 ms**, same box and sitting. ⛔ The 29 Aug figures (0.670 / 2.754) are SUPERSEDED — bad run |

✅ **AND L2's HARDWARE TEST IS NOW DONE — 30 Aug 2026.** Cable pulled from a LIVE wired
link with sync probes in flight. PPS survived: same pid throughout, link closed cleanly
(`link down: link closed`), watch torn down with a reason (`device watch ended — no usbmux
provider`), zero kernel signal or crash records. The probe said `send(...,0)` dies and
`send(...,MSG_NOSIGNAL)` survives; the product now agrees.

⛔ **PHASE 2L IS COMPLETE.** L1, L2 and L3 are all verified on hardware.

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
| 2026-08-29 | 2L | ✅ **WIRED RECONNECT WORKS ON LINUX, AND HERE ARE THE NUMBERS.** A persisted pairing reconnected over the cable with no QR and no network — `[ppcp-rv] link up: TLSv1.2 TLS_PSK_WITH_AES_128_GCM_SHA256 psk transport=usb "pairing=e8a1ba0fcfea21bf"`, confirmed on the phone by the cable glyph and on the host by **zero** TCP connections on 7788 while PPS held three usbmuxd sockets. Measured with `PINPOINT_SYNC_TRACE=1` against the **open-source** `usbmuxd`: **`min_rtt` = 0.670 ms** (floor 0.335 ms), `own_sigma` 0.339 ms, converging to **`offset_sigma` = 2.754 ms**, `skew_sigma` 51.4 ppm, `sigma@5s` 2.766 ms — **under the 5 ms arbitration gate**. |
| 2026-08-29 | 2L | ⚠ **THE CABLE HALVES RTT AND STILL SITS AT 2.754 ms SIGMA — WORKABLE, BUT NOT THE WIN §1 PREDICTS.** `min_rtt` **0.670 ms** (floor 0.335 ms) against Phase 0's WiFi 1.64 ms, yet `offset_sigma` settles at **2.754 ms**, eight times its own floor, and needed 83 s to reach it. ✅ Calibration from Mark: **anything under 1 ms is "pretty fab"** — so 2.754 ms is comfortably inside the 5 ms arbitration gate and usable, but it is not the accuracy result the cable was argued for, and `own_sigma` 0.339 ms shows the host's own timebase is not what is costing it. ⛔ **AND THE COMPARISON I FIRST WROTE HERE WAS CONFOUNDED, so do not repeat it:** Phase 0's WiFi arm ran on the **build Mac**, this ran on **Linux**, so host and transport both changed and "the cable loses on sigma" is not supported by these two numbers. ✅ **The clean experiment is WiFi vs cable on THIS host** — same box, same phone, same session — and it is cheap now that both transports work. Run it before drawing any conclusion about the transport. |
| 2026-08-29 | 2L | ⚠ **AND IT TOOK 83 SECONDS TO GET UNDER THE GATE — ON A LINK THAT NEVER DROPPED.** Link up 21:19:54, first `<-- UNDER THE 5ms GATE` at 21:21:17, with **1 up / 0 down** over the whole run, so no reconnection reset the estimator. Phase 2 measured a takeover reaching the gate at t+35 s on macOS and Mark accepted that gap for the idle case; **83 s is more than twice it, and unexplained**. A shot inside that window cannot be arbitrated. ⛔ This and the worse-than-WiFi sigma are probably the same question and should be chased together. |
| 2026-08-29 | 2L | ⚠ **THE LINK FLAPPED 247 up / 246 down WHILE PPC's SESSION WAS STALE, AND NOT AFTERWARDS.** In the 21:12–21:16 window it cycled about once a second, every close an orderly `link down: link closed` with no error. ✅ **It did NOT recur once PPC was restarted**: the traced run that follows held **1 up / 0 down across ~110 s**. So this is a symptom of the stale-session state, not a standing defect, and the wired link is stable in normal use — Mark's read, and the counts bear it out. ⛔ Two corrections to my own earlier claims: it is NOT the explanation for the 83 s convergence (that happened on the stable link), and my reading of "33 dial dispatches in 25 s" as 33 failures was wrong — they were this cycling plus a presence port with nothing behind it. |
| 2026-08-29 | 2L | ⚠ **THE PHONE GOT VERY HOT and the session was ended for it** — design §10/M10 names thermal and battery as measurements the wired path owes, and this is the first observation of either. Uncontrolled: PPC had been foregrounded with the camera live for over an hour, across ~8 PPS restarts and a link cycling once a second, so the cable is one candidate among several. ⛔ Worth treating as a real signal rather than an anecdote: a phone that overheats on the cable is a product problem regardless of which component causes it. |
| 2026-08-29 | 2L | ✅ **§6.2's diagnostics earned their keep again, and the exact line is worth quoting**: `[ppcp-usb] no presence record — usbmux: connection refused inside the device — the capture app is not running or not in the foreground (or this computer is not trusted; M5 pending) (Number=3) — retrying every 2 s while it stays plugged in`. That named, first time and in one line, why every dial was failing: **PPC's session was stale.** It had been foregrounded all evening but its link died with each PPS restart, and PPC re-dials only on the transition to ACTIVE — so a continuously-foregrounded app never re-establishes anything, including serving the presence port. ⚠ Restarting PPC fixed both transports instantly. |
| 2026-08-29 | 2L | ⚠ **PROCESS NOTE, and it cost most of an evening: `PINPOINT_LOG_STDERR=1` puts the whole app log on stderr.** Without it `ppWarn()` reaches only the in-app log, and I spent six instrumented builds reconstructing from outside a process what one environment variable would have shown directly. ⛔ Any future headless or remote session on this app should set `PINPOINT_LOG_STDERR=1` and `PINPOINT_SYNC_TRACE=1` at launch before doing anything else. |
| 2026-08-30 | 2L | ✅ **`9a068bf` VERIFIED AGAINST ITS OWN SCENARIO — the one commit that had only a code-reading behind it.** Phone unplugged, `usbmuxd` gone (socket file still present, so `connect()` fails ECONNREFUSED — `errno=111`, the exact case the old code gave up on). PPS started at 07:08:16 with no daemon and printed **one** `wired path unavailable` line, not one every five seconds. Phone plugged in at 07:11:20; **`wired path armed — /var/run/usbmuxd` at 07:11:24 — 4 s later, with no restart of PPS**, against `kWiredWatchRetrySecs` = 5. On the old code that line could never have appeared. ✅ It then dialled immediately and reported `no presence record … (Number=3)` because PPC was deliberately not running — the correct diagnosis, and the clean separation between "the watch recovered" and "a phone answered" that was missing last night. |
| 2026-08-30 | 2L | ⛔ **THE ROW BELOW IS SUPERSEDED — its cable figure was last night's bad run. Clean pair, same box / phone / sitting: WiFi `min_rtt` 3.166 ms → `offset_sigma` **2.280 ms** (1.44× floor); cable `min_rtt` 0.902 ms → `offset_sigma` **0.694 ms** (1.54× floor), `skew_sigma` 8.5 ppm, link 1 up / 0 down.** ✅ **The cable is 3.3× better on sigma and lands UNDER 1 ms — "pretty fab" by Mark's calibration — sitting at the same multiple of its floor as WiFi does of its.** §1's argument holds: the transport is the lever, and the accuracy case for wired is REPRODUCED on Linux. ⚠ Last night's 2.754 ms was measured amid link flapping and a stale phone session; it is not representative and should not be quoted. I drew a conclusion from it twice and was wrong twice — the controlled run is this one. |
| 2026-08-30 | 2L | ⚠ **(superseded — kept for the reasoning, not the numbers)** THE CLEAN COMPARISON, SAME BOX / SAME PHONE / SAME SITTING — and it mostly retracts yesterday's alarm while sharpening what is left.** WiFi on this Linux host: `min_rtt` **3.166 ms** (floor 1.583), `offset_sigma` **2.280 ms**, `own_sigma` 1.610 ms. Cable on the same host: `min_rtt` **0.670 ms** (floor 0.335), `offset_sigma` **2.754 ms**, `own_sigma` 0.339 ms. ⛔ So cable-vs-WiFi sigma is **2.754 vs 2.280**, not the 2.4× gap the cross-platform figures suggested — they are within 20% of each other and both well inside the 5 ms gate. ⚠ **The real finding is the ratio to floor: WiFi sits at 1.4× its floor, the cable at 8.2× its own.** The cable's 4.7× RTT advantage is entirely unrealised — it *should* be reaching ~0.34 ms and is not. That, not "worse than WiFi", is what to chase, and it means the accuracy case for the cable is neither proven nor refuted: the transport is delivering, the estimator is not using it. |
| 2026-08-30 | 2L | ⛔ **FOR THE PinPointCapture TEAM — A PHONE THAT HOLDS A PAIRING AND REPORTS "No host", AND IT COSTS THE WHOLE MORNING'S FIRST CONNECTION.** Observed 30 Aug 2026, after an overnight idle in which the phone had previously become **very hot** (possible thermal reboot). ⚠ **Symptom:** PPC launched, displayed **"No host"**, and attempted **nothing** — no WiFi dial *and* no presence served on 50915, so the host's wired dials all returned `Number=3`. ✅ **Proof it DID hold the pairing:** after a background/reopen, the cable link came up on **`pairing=e8a1ba0fcfea21bf`** — the pairing from the previous night, the one PPC had just said it did not have. Nothing re-paired it; the intervening QR created a DIFFERENT pairing (`eac76853408c8886`), which is the one the WiFi link used. ⚠ **Why both symptoms appear together, which is the diagnostic:** `ReconnectCoordinator` dials only *"while at least one pairing is held"* and `WiredPresenceListener` publishes *the pairings it holds* — so **zero visible pairings gates both**, where a wired-only fault would gate one. ⛔ **Hypothesis worth testing first:** the launch-time read of `PairingSecretStore` returned `[]` and was cached for the session. `PairingSecretStore.swift:300` returns `[]` when the file is absent, which is **indistinguishable from a read that failed**, and the file is `completeUntilFirstUserAuthentication` — readable only **after the first unlock following a boot**. The store's own header documents this exact failure shape from its Keychain era: *"unreadable while the phone is locked, so the reconnection sweep read nothing and reported no pairings held, which is a different statement and an untrue one."* ✅ **Suggested repro:** reboot the phone, launch PPC as early as possible after boot (before/around first unlock), and check whether `pairings()` is empty while the file exists on disk; then relaunch and see it reappear. ✅ **Suggested fix shape:** distinguish "no file" from "could not read" at `:300`, retry the load rather than caching an empty list for the session, and re-read on becoming-active. Files: `Sources/Platform/Rendezvous/PairingSecretStore.swift`, `ReconnectCoordinator.swift`. |
| 2026-08-30 | 2L | ⚠ **AN ORPHANED PAIRING ON THE HOST IS INVISIBLE, UNPRUNABLE, AND SILENTLY CHANGES WHICH ONE THE CABLE USES.** PPS now holds **two** — `e8a1ba0fcfea21bf` and `eac76853408c8886` — for one phone, because the re-pair above created a second rather than replacing the first. ⛔ `resolveFirstWiredPeer()` takes the **first** published identity that resolves against a held pairing, so with more than one the pairing a wired link runs on is effectively arbitrary: observed here as the cable adopting the OLDER pairing while the WiFi link minutes earlier used the newer. Harmless today — both resolve, both work — but it means "which pairing is this link on" is not a question the host answers deterministically. ⚠ **And there is no host-side prune.** E57 gave the DEVICE `pairings()` and `revoke(_:)` and made them real; PPS has no equivalent, so orphans accumulate in QSettings for the life of the install and each one lengthens `rotationPeriodSeconds()`. Not urgent, not a correctness bug, but it should be a decision rather than an accident. |
| 2026-08-30 | 2L | ✅ **L2 VERIFIED ON HARDWARE — THE PHASE IS COMPLETE.** Cable pulled out of a live `transport=usb` link with probes in flight, which is precisely the case `MSG_NOSIGNAL` exists for and precisely what killed the process in the standalone probe without it. **PPS survived** — pid unchanged from 07:08:13 through the unplug, link closed cleanly, `[ppcp-usb] device watch ended — no usbmux provider` printed with a reason rather than silence, and no kernel signal record. ✅ With this, **L1, L2 and L3 are all verified on hardware and Phase 2L is done**: Linux went from "no reconnection at all" to both transports reconnecting from a persisted pairing with no QR, the cable measurably better than WiFi (`offset_sigma` 0.694 ms vs 2.280 ms), and the app surviving the cable being ripped out mid-session. |
| 2026-08-30 | 2L | ⛔ **FOR THE PinPointCapture TEAM — A LINK THAT DROPS WHILE PPC IS FOREGROUNDED LEAVES IT DORMANT FOR EVER. Mark's diagnosis, and the code agrees.** ⚠ **Symptom:** after a wired link goes down, PPC holds a pairing, has no link up, is active on screen — and attempts **nothing**, on either transport. The only recovery is for the user to background the app and reopen it. ⛔ **Cause: the reconnect sweep is EDGE-triggered on the wrong edges.** `ReconnectCoordinator` fires on exactly two things (its own header, :70-84): *"on an explicit entry to the connect flow, and on the app becoming active while at least one pairing is held and no link is up"*. **A link ending while the app is already active is neither.** `AppModel.linkDidEnterBackground()` exists (`AppModel.swift:710`, called from `RootView.swift:158`) and there is **no counterpart for a link dropping while foregrounded** — nothing calls `attempt()` (`:303`) on that event. ✅ **The machinery is already there and merely unarmed:** the widening cadence (3 s, then 2/5/10 s, then every 30 s) explicitly *"does not stop, because the host may be switched on at any moment while the user waits"* — it just never starts. ⚠ **This is the SAME BUG SHAPE THIS PLAN ALREADY FIXED ON THE HOST**, one repo over: *"the wired path only ever fires on a usbmux `Attached` event, so in the MOST COMMON REAL SEQUENCE IT NEVER FIRES AT ALL"* — resolved there by a periodic re-probe rather than trusting an edge. The device now needs the same treatment on the WiFi path. ✅ **Suggested fix shape:** treat "a link ended while active" as a third trigger for `attempt()`, or level-check `(pairing held && no link up && active)` on the existing cadence rather than arming it only on the becoming-active transition. ⛔ **Field impact, which is why this is not cosmetic:** any disconnection — host restarted, cable pulled, WiFi blip, phone unplugged — strands the phone until an operator thinks to background and reopen the app. It also accounted for most of the friction across 29–30 Aug: every host restart required a manual PPC restart, which twice led me to suspect host-side faults that did not exist. |
| 2026-08-30 | 2L | ⚠ **THE TWO PPC FINDINGS ABOVE ARE SEPARATE FAULTS AND MUST NOT BE READ AS ONE — proven by a live test.** Cable unplugged, PPS up and advertising, PPC backgrounded then reopened: **link up over WiFi in under 5 s**, `transport=wifi`, `pairing=e8a1ba0fcfea21bf`. ✅ **So the becoming-active trigger works, the sweep works, and the pairing IS held and readable** — which means (a) the dormancy fault is *only* the missing link-down trigger, so the fix is small and the machinery beneath it is sound, and (b) this morning's "No host" was **not** dormancy: the trigger fired then too, on a cold launch, and found an empty pairing list. ⛔ They fail at different points — **one never asks, the other asks and is told the wrong answer** — and fixing only the trigger would leave a phone that reboots overnight still starting the day unable to reconnect. ⚠ Also note the phone reconnects on `e8a1ba0fcfea21bf`, the pairing it earlier reported not having: held, readable, working. That makes the morning's empty read look **transient rather than data loss**, and strengthens the launch-timing hypothesis over any theory in which the store was actually cleared. |
| 2026-08-30 | 2L | ⛔ **OPEN: EVERY DIAL FAILS TLS AUTHENTICATION AFTER A WORKING LINK, AND THE COUNTER THAT WOULD NAME THE CAUSE IS UNREACHABLE.** Symptom: PPC shows *"looking…"*, dials, and the host logs `PPCP TLS handshake failed` **twice every 30 s** — two channels × the phone's sweep at its 30 s plateau. Last good link **07:35:25** (`transport=wifi`, `pairing=e8a1ba0fcfea21bf`); every dial since has failed. Triggered by launching PPC with the cable already plugged in; the same phone reconnected fine minutes earlier with the cable out. ✅ **Established:** only one host advertises on the network, so it is not dialling elsewhere; the host still holds both pairing ids, unchanged; the message is **deliberately uniform** (`ppcp_transport.cpp:219-222`, RV 5.3c/7.7c — naming the cause would be the distinguisher 7.7c forbids, and RT-11 pins unknown-identity and wrong-key as indistinguishable), so there is no detail to extract and this is NOT a logging gap. ⛔ **Hypothesis killed:** rotation cannot be racing the dial — `identityResolver()` offers **every** held pairing to `ppcp_rv_resolve_psk_identity` with no pre-filter (deliberately, since a filtered candidate set is itself a timing signal under 5.3d), so identity resolution is independent of the advertised `rid`. ⚠ **What remains:** the identity the phone presents matches none of the host's pairings, and I could not determine why from outside the process. ⛔ **THE DECISIVE DIAGNOSTIC EXISTS AND IS DEAD CODE:** `PpcpRendezvous::diagnosticExport()` (`ppcp_rendezvous.cpp:897`) prints `resolver: calls= resolved= no-pairing= expired= exhausted= invalidated=` plus per-pairing `uses=`, `exp=` and `invalidated=` — which distinguishes **every** remaining hypothesis in one line — and **it has no caller anywhere in the application**, only tests. ✅ **Next step, and it is worth doing permanently rather than as a debug hack:** give it a caller — a log line after N consecutive refusals, or a menu action. Same pattern the PPC entries note, where `pairings()`/`revoke()` were written and never wired up. ⚠ A pairing store write is timestamped at exactly 07:35:25, but `phoneNames` is written in the same section on link-up, so the correlation is suggestive and NOT evidence about the pairing blob itself. |
| 2026-08-30 | 2L | ⛔ **MY WATCH-RETRY FIX (`9a068bf`) IS INCOMPLETE AND I VERIFIED THE WRONG HALF.** It recovers the case *"no usbmux daemon when PPS started"* — proven on hardware, 4 s. It does **not** recover *"daemon went away and came back"*, which on Linux is every unplug/replug and therefore the common case. Cause: when the watch ends, `onWatchReadable()` calls **`stop()`** (`ppcp_wired_link.cpp:418`), which sets `m_running = false` and stops the retry timer; `onRetryTick()` opens with `if (!m_running) return;`, and nothing ever calls `startWired()` again (single call site, `ppcp_host_service.cpp:339`). **So after any cable unplug the wired path is dead until PPS restarts** — observed live: cable replugged 07:36, usbmuxd active, and no `wired path armed` line since 07:30:52. ✅ **Fix shape:** on watch end tear down the watch and notifier but keep `m_running` and the retry timer alive so `tryOpenWatch()` can re-arm; reserve full `stop()` for shutdown. ⚠ I let a verified startup case stand for the whole class — the same mistake in miniature as trusting an edge instead of checking the level, which is the theme of three defects in this plan already. |
| 2026-08-30 | 2L | ✅ **THE CABLE REACHES ITS FLOOR — best run yet, and it settles the accuracy question.** Fresh PPS process, cable link, 190 s in: `min_rtt` **0.852 ms** (floor 0.426), `offset_sigma` **0.464 ms** — **1.09× floor, i.e. essentially the theoretical limit** — `skew_sigma` 5.9 ppm, 1 up / 0 down / 0 handshake failures. Against WiFi on the same box (2.280 ms) that is **4.9× better**. ⛔ Both earlier cable figures are now superseded: 29 Aug's 2.754 ms (8.2× floor) was measured amid flapping and a stale session, and 30 Aug's 0.694 ms (1.5× floor) came from the process that later failed every handshake. **§1's argument is confirmed on Linux: RTT is the lever, and the cable delivers it.** |
| 2026-08-30 | 2L | ⚠ **THE HANDSHAKE FAILURE DID NOT SURVIVE A PPS RESTART — so it is HOST IN-PROCESS STATE, not the stored pairings.** Same phone, same cable, same two pairing ids unchanged on disk: the new process connected on the first attempt (`transport=usb`, 0 refusals) where the old one had refused **63 consecutive** dials. ⚠ **Shape of it, which is the useful part:** the second pairing was added AT RUNTIME by QR at ~07:18; three links then succeeded across both transports over ~19 minutes (07:18:44, 07:23:16, 07:30:36, 07:35:25); from 07:37 every handshake failed until restart. So it is not immediate corruption on adding a runtime pairing — something degrades later. ⛔ **NOT REPRODUCIBLE ON DEMAND and I could not catch it live.** ✅ The instrumentation added today (`diagnosticExport()` after 3 consecutive refusals, then every 20) is now permanent and will name the cause — `no-pairing` / `expired` / `exhausted` / `invalidated`, plus per-pairing `uses`/`exp`/`invalidated` — the moment it recurs. That is the trap to leave set rather than a bug to keep guessing at. |
| 2026-08-30 | 2L | ✅ **The advertiser now states the rotation it is actually doing**, which had to be inferred yesterday: `rotating 2 pairing(s) every 30s` — against `every 900s` when it held one. Confirms the orphaned pairing shortens rotation thirtyfold. ⚠ Not the cause of the handshake failures (`identityResolver()` ignores the advertised `rid` by design) but it is a real consequence of orphan accumulation, and a second reason to want a host-side prune. |
| 2026-08-30 | 2L | ✅ **THE COMPLETED WATCH FIX VERIFIED ON HARDWARE — the half `9a068bf` missed.** Unplug/replug with PPS left running: `08:08:15 device watch ended` → `08:08:20 wired path unavailable (errno=111)` (the retry ran while the daemon was gone, and said so **once**) → `08:09:05 wired path armed` → `1 device(s) attached, 1 on a cable`. **No PPS restart.** On the previous binary the sequence stopped dead at the first line and the cable was gone for the life of the process. Three things confirmed together: the retry timer survives the watch ending, resetting `m_watchAnnouncedDown` in `closeWatch()` keeps one-line-per-state-change honest across a cycle rather than going silent, and re-arming lands inside `kWiredWatchRetrySecs`. ✅ **L2 also confirmed a second time** — PPS survived this unplug too, same pid throughout, link closed cleanly. |
| 2026-08-30 | 2L | ✅ **CONFIRMED WITH DIRECT EVIDENCE — PPC's WIRED PRESENCE RECORD IS STALE AFTER A RE-PAIR, and this is why a freshly-paired phone will not go wired.** The morning's hypothesis, now proven by the host's own §6.2 line: `[ppcp-usb] a cabled device published **4 listener(s), none of which resolves to a pairing this host holds**` (`DialOutcome::NoMatch`, RV 3.4c) — **while the live WiFi link was running on `9fcd0caed02a2134`**, a pairing both ends demonstrably hold. So `WiredPresenceListener` is publishing the pairings PPC held **when the listener was built** and does not refresh when one is added. ⛔ **Consequence: after ANY pairing, the cable can never be used until PPC is restarted** — the operator pairs, sees WiFi, and has no way to know the cable is unreachable. This is exactly the *"refresh on foreground entry and on pairing-set change"* item Phase 2 recorded as owed. ⚠ Note also the phone published **4** listeners against the host's **1** pairing: orphans accumulate on the DEVICE side too, one per re-pair, mirroring the host-side problem. |
| 2026-08-30 | 2L | ⛔ **A CONNECTED PHONE MAKES Settings → Phones UNTYPEABLE: the alias field loses focus on every heartbeat.** `ppcp_host_service.cpp:395` hooks peer health with `[this](const PeerHealth &) { emit phonesChanged(); }`, and its own comment says the intent — *"every `heartbeat_ack` moves this phone's battery/thermal/storage reading … all this hook has to do is tell Settings → Phones that it moved."* ⚠ **But the mechanism is structural, not a value update:** `PhonesPanel.qml:90` binds `rows: controller.phones`, so each emission re-reads the whole `QVariantList`, the `Repeater` destroys and recreates every delegate, and the `PpTextField` being typed into (`:206-212`) goes with it. Focus is lost mid-word, once per heartbeat, for as long as a phone is linked. ✅ **Fix shape: separate "the phone LIST changed" from "a phone's READINGS moved."** The health hook should drive a narrower notification that the readings bind to, leaving `phonesChanged()` for genuine structural change; coalescing or suppressing no-op emissions would also work but leaves the rebuild-on-every-tick cost. ⚠ The same shape exists in `CamerasPanel` and `ImusPanel` (identical alias-field-inside-a-rebuilt-Repeater pattern) — phones is simply the one with a once-per-second signal behind it. |
| 2026-08-30 | 2L | ⛔ **FOR THE PinPointCapture TEAM (3rd) — PPC AUTHENTICATES AND THEN NEVER BINDS, AND THE HOST DROPS IT. Cleanest signal we have had.** Operator tapped *"find the host"*; PPC failed and fell back to **"No host"**. Host side, 14:43:04: `[ppcp-rv] PPCP stream closed: **no link_bind inside the bind timeout** (ENC 2.1c)`. ✅ **What that rules OUT, and it is most of the field:** `handshake failed` **0** — TLS-PSK succeeded, so the identity resolved and both ends genuinely share a pairing; `channel N bound` **0**; `link up` **0**. So this is **not** authentication, **not** the host refusing, and **not** discovery. The phone connected, authenticated, and then went silent before binding. ⚠ **One candidate to check first**, from this plan's own C6 finding: `ppcp_peer_hello()` auto-emits `link_bind` at `ppcp_peer.c:929` only when `!listener && has_link_id` — **a client that never set a link id emits nothing and produces exactly this silence**. ⛔ **We could not see PPC's side: it emits no PPCP events to the device syslog**, so `idevicesyslog` on a cabled phone shows only UIKit noise. Device-side instrumentation is what this needs. ⚠ **Context that may matter:** the same phone was simultaneously publishing **4** wired presence listeners, none of which resolves to the host's 1 pairing — so it holds ≥5 pairings, 4 of them stale, and the pairing set is badly out of step between the two ends. |
| 2026-08-30 | 2L | ⚠ **THREE PPC FAULTS ARE NOW OPEN AND THEY SHOULD GO TO THAT TEAM TOGETHER**, because two of them share a root and the third compounds both: (1) **holds a pairing, reports "No host"** — asks and is told the wrong answer; (2) **a link dropping while foregrounded leaves the app dormant** — never asks; (3) **authenticates then never sends `link_bind`** — asks, is answered, and does not follow through. ✅ (1) and (2) are the same design error in two places: deciding at an EDGE instead of checking the LEVEL, which is the shape this plan has now found five times across both repos, twice in host code I wrote myself. ⚠ **Practical consequence for anyone testing:** restarting PPC has cleared every stuck state today, which is why it kept masking these — and each restart also adds another orphaned pairing. |
| 2026-08-30 | 2L | ⛔ **HOST-SIDE GAP: `NoMatch` LATCHES THE RETRY OFF, AND A PHONE'S PAIRING SET CHANGES WITHOUT UNPLUGGING.** `noteDialOutcome()` does `if (o == DialOutcome::NoMatch) r->linked = true;` — "leave this one alone" — on the stated reasoning that *"none of this phone's pairings is one of ours"* is settled *"until it replugs"*. ⚠ **It is not settled: it also changes when PPC RESTARTS and rebuilds its presence record**, which is both an everyday event and precisely what the fix on the device side will do. Observed today: phone published 4 stale listeners at 14:41:23 → `NoMatch` → retry latched off; PPC restarted at ~14:45 and its record now carried the live pairing; **the host never looked again**, so the cable stayed unreachable while WiFi worked. ✅ Cycling the cable re-armed it and it went wired immediately (`transport=usb`, `pairing=9fcd0caed02a2134`, 14:47:54) — proving the record HAD refreshed and only the latch was hiding it. ⚠ **This is the SIXTH instance in this plan of trusting an edge instead of checking the level**, and the third in host code. ✅ **Fix shape:** do not latch `NoMatch` for the life of an attachment — re-probe on a long backoff (the cost is one refused plist round trip), or treat any change in the published record as grounds to re-ask. The concern the latch exists for — not hammering a phone that genuinely is not ours — is met by a slow cadence rather than by never asking again. |
| 2026-08-30 | 2L | ⛔⛔ **FOR THE PinPointCapture TEAM — THE ROOT FINDING, AND IT SUPERSEDES THE THREE SYMPTOM REPORTS ABOVE: PPC DOES NOT SERVE ITS WIRED PRESENCE PORT CONTINUOUSLY.** Caught by Mark noticing the timing was too tight to be chance, then confirmed in the host log: PPS started 15:46:01, saw the cable (`1 device(s) attached, 1 on a cable — usbmux: ok`), and from 15:46:05 got `no presence record — connection refused inside the device (Number=3)` — **nothing listening on port 50915** — retrying every 2 s. It was refused for **35 seconds**. The instant Mark tapped **"find the host"** in PPC, the very next probe succeeded: `15:46:40 link up transport=usb` + `peer declared`. ⛔ **So the cable is unreachable except in a window that opens when PPC enters its connect flow** — a restart, or a find-host. Outside that window the host can do nothing: it is knocking every two seconds on a port with nothing behind it. ✅ **This explains every wired success today and every failure**: 07:23, 07:30, 07:58 and 15:41 all followed a PPC restart; 15:46:40 followed a find-host; every long silent stretch was PPC running normally with the presence port closed. ⚠ **Same shape as the dormancy finding** — PPC does connection work only at explicit moments, never continuously — which is why "restart PPC" has been the universal remedy all day and has masked this since the port began. ✅ **Suggested fix:** bring `WiredPresenceListener` up whenever a pairing is held and keep it up, rather than tying it to connect-flow entry; and refresh it on pairing-set change (see the separate finding above). |
| 2026-08-30 | 2L | ⚠ **AND A SECOND, DISTINCT WIRED FAILURE MODE — presence IS served, the link IS accepted, and it dies inside the same second.** Not to be confused with the above: there the port is closed and no link forms at all; here the dial succeeds, TLS-PSK completes on the correct pairing, the link is adopted, and `pump()` finds **every channel socket closed** — `isOpen()` is `sock != PP_INVALID_SOCKET`, so this is the sockets going away, not a protocol rejection. No channel bind, no declare, `link closed` (not `duplicate link`, so **our** backstop did not do it). Observed **653 times in one process**, once per retry. ⚠ **Correlation, not proof:** it happens when PPC already holds — or believes it holds — a link, and never when PPC's link slot is empty. ⛔ **Eliminated as causes**, each on evidence: the takeover (happens with no competing link), `closeWatch`/watch re-arm (happens on a fresh original subscription), usbmux tunnel ownership (`Client` holds no socket and `dial()` only closes on failure), authentication (TLS succeeds every time), and the duplicate-link backstop (wrong reason string). ✅ **What would settle it:** device-side instrumentation, or logging at the point a channel's socket is invalidated whether it was a clean EOF or an error with errno. From the host we can see only that the far end went away. |
| 2026-08-30 | 2L | ⚠ **TWO CORRECTIONS TO MY OWN REPORTS, so the PPC team is not sent after ghosts.** (1) **PPC's transport indicator is CORRECT** — I twice inferred it was showing WiFi spuriously; Mark confirmed it reads *cable* and matches the host. My inference was wrong, built on assuming that "no WiFi link on the host" meant the phone was confused. (2) **"PPC doesn't notice when a link dies" was my theory and it is superseded** by the presence-port finding above, which is more specific and better evidenced. ⛔ Several hours today went into theories that died on evidence — the takeover, my own `closeWatch`, tunnel ownership, a pairing-visibility theory. **The lesson for whoever picks this up: the host log's `[ppcp-usb]` line names the state precisely (`Number=3` = nothing listening; `none of which resolves` = stale pairing set; `link closed` = the far end went away), and reading it beats reasoning about it.** |
| 2026-08-30 | 2L→fix | ✅ **ALL FOUR PPC/PPS FAULTS FIXED. Two of the Linux team's onward diagnoses are REFUTED with code references — the central findings were right, two of the follow-on causes were not.** Detail in the four rows below. Linux's own port changes build clean and pass 7/7 affected suites **on macOS**, so there is no cross-platform regression to worry about. |
| 2026-08-30 | 2L→fix | ✅ **FIX 1 (PPC, the root) — the presence listener is now LEVEL-DRIVEN.** Confirmed exactly as reported: `beginWiredListening()` had one caller, `beginSearchingForHost()`, which itself had two — becoming active, and the find-host button — and `adoptWiredLink()` stopped it. So the port really was closed except in a window the operator opened. Now a 2 s reconcile holds one rule: **a pairing is held, no link is up, the app is active ⇒ the listener is up publishing the CURRENT set.** ⚠ Matches the host's own 2 s wired retry so the phone is listening within about one probe of becoming eligible. |
| 2026-08-30 | 2L→fix | ✅ **FIX 2 (PPC) — the stale record after a re-pair, fixed at the STORE and not at the call sites.** `PairingSecretStore` now carries a `generation` bumped inside `mutate()` — the single funnel every mutator already passes through — so the reconcile detects a changed pairing set **without reading the file on every tick**, and a future mutator cannot forget to announce itself. ⛔ That "somebody will forget" property is the whole reason it is not a notification posted by each caller. Two tests: every mutation moves it; **a read does not** (otherwise every tick would restart the listener for ever). |
| 2026-08-30 | 2L→fix | ✅ **FIX 3 (PPC) — a link ending while foregrounded no longer strands the app, and the fix went where the state was ALREADY being watched.** The link dying does not clear `AppModel.link` — only an explicit `disconnect()` does — so `beginSearchingForHost()`'s `link == nil` guard refused to start. The 250 ms `hostLinkTicker` already polls the session, so it now checks the level: state `.lost` ⇒ tear the dead session down, then `linkDidEnd()` re-arms both the browse and the cable. ✅ The widening cadence beneath it was always there and merely unarmed. |
| 2026-08-30 | 2L→fix | ✅ **FIX 4 (PPS, mine) — the `NoMatch` latch is gone.** The Linux team is right and my stated reasoning was wrong: *"it changes only when the phone is unplugged or newly paired"* ignored that **the capture app restarting rebuilds its presence record**, which is an everyday event. Replaced with `kWiredNoMatchRetrySecs = 60` — slow down, never stop. The concern the latch existed for is met by the cadence; the cost of being wrong is one refused plist round trip a minute against a cable that could never come back. ⚠ Sixth instance of edge-versus-level in this plan, third in host code, second of mine. |
| 2026-08-30 | 2L→fix | ⛔ **REFUTED — "the pairing store returns `[]` on a failed read".** It does not. `rowsLocked()` **throws** `StoreError.storage`, and its comment already says *"Not `[]`. A store we cannot read is not a store that is empty, and conflating the two is precisely what reported 'no pairings held' to a user who had one."* `ReconnectCoordinator` has a distinct `.pairingStoreUnreadable` outcome that **retries** rather than stopping, and `AppModel` records the diagnosis and keeps searching — all of it already in place under erratum E56. ⚠ **And the evidence does not support the conclusion either: "No host" is simply the status-card title whenever no link is up** (`HostLinkState.none.title`), not a report about pairings. The morning's symptom is fully explained by FIX 1 + FIX 3. |
| 2026-08-30 | 2L→fix | ⛔ **REDIRECTED — "authenticates then never binds" cannot be the `hello`/link-id cause suggested.** On the device the **transport owns `link_bind`**: `PpcpTransport` mints the link id and writes the frame itself, and `DevicePeer.setLinkId` is called **nowhere** — there is a long comment saying so, from a preview-channel bug that cost two days in August. The dial path does send the bind as the first frame on every stream. ✅ So the engine hypothesis is dead; the cause is not visible from the host, which is what FIX 5 addresses. |
| 2026-08-30 | 2L→fix | ✅ **FIX 5 (PPC) — the device has a voice now.** The two unexplained faults both hinged on the same gap the Linux team named: *"PPC emits no PPCP events to the device syslog"*. The cause was mundane — the app diagnoses with `print()`, which goes to **stdout** and is visible in Xcode and nowhere else. New `PpcpLog` uses `Logger`, so it reaches the unified log and therefore `idevicesyslog` on a cabled phone from **any** platform, Linux included. Instrumented at exactly the points that would have settled both: per-channel `dialled` / `link_bind sent` / `link_bind FAILED`, link `lost` with its error, and presence up/down/failed with the pairing count. ⚠ `privacy: .public` on every interpolation or a release build redacts it to `<private>` — which would have left the same silence. ⛔ No key, no identity, no payload (`RV` 7.2b); session ids only, which the host already prints. |
| 2026-08-30 | 2L→fix | ⚠ **THE THERMAL OBSERVATION IS ACCEPTED AS A REAL SIGNAL AND IS NOT A WIRED DEFECT.** Mark's direction: reduce what the phone renders during capture (**nobody is looking at the handset while a golfer swings**) and arm/disarm around states where a capture would be discarded anyway — analysis, replay. ⛔ Neither is built here; recorded so M10 is designed against a phone that is not needlessly hot rather than measuring the current waste. |
| 2026-08-30 | adj | ⚠ **ADJACENT TO WIRED, NOT PART OF IT, but it was found by the wired work and belongs with the rest of the story: EVERY STUDIO A PHONE HAD EVER PAIRED WITH WAS CALLED "PinPointStudio".** `cfg.displayName` was a constant (`ppcp_host_service.cpp`), so Mark — testing one phone against macOS, Linux and Windows — had three rows under one indistinguishable name and no way to tell which machine to forget. ✅ **The forget UI already existed**: `RememberedStudiosView` lists pairings and revokes them individually and is reachable from two places; it was written under D7 with no caller and wired up under #96. What it lacked was names worth telling apart. |
| 2026-08-30 | adj | ✅ **The Studio name is settable, defaults to the machine's own host name** (`.local` trimmed), and lives in **Settings→Phones rather than General** — it is a fact about the phone relationship, and Phones is the screen someone is on when they wonder what their phone is showing. Clearing it returns to the default rather than publishing nothing. ⚠ Truncated to 64 bytes for `RV` 4.3 **by BYTES, stepping back off a continuation byte** — the limit is the wire's, and a multi-byte name cut mid-sequence is invalid UTF-8 rather than merely short. |
| 2026-08-30 | adj | ⛔ **IT TRAVELS ON TWO PATHS AND NEEDS BOTH — the second one is the part that is easy to miss.** The pairing code carries the name at pairing time and is the only channel that existed; but a persisted pairing has **no expiry** (`RV` 7.4a) and a code is read once and never again, so a machine renamed afterwards would keep its old name on every already-paired phone **for ever**. So `declare` carries it too, in `product.model` — which is the convention this system **already uses in the other direction**, since the host names a phone's row from *its* `product.model`. 5.2c makes `product` informational and I19 forbids inferring behaviour from it, so a display name is exactly what it may carry. ✅ **libppcp needs no change**: both channels already exist. |
| 2026-08-30 | adj | ⛔ **DELIBERATELY NOT IN THE mDNS TXT RECORD, and this is the one real design decision rather than plumbing.** §3's advertisement is cleartext on the LAN and deliberately minimal (`txtvers`, `pv`, `role`, `rn`, `rid`), and **`rid` ROTATES precisely so a passive radio observer cannot link one venue's sightings to another's** (3.4d/3.4e). A stable human name beside it would undo that in a single field — it *is* the tracking beacon the rotation exists to prevent. The two chosen paths are already private or already consented to: a code the operator is deliberately showing, and the inside of an authenticated link. |
| 2026-08-30 | adj | ✅ **Verified on hardware, including the case that would have failed silently.** Host defaulted to `Marks-Mac-mini`, renamed to "Mark's Mac mini", and a phone paired **BEFORE** the rename showed the new name on its next connect. ⚠ One consequence worth knowing: renaming writes the phone's pairing store, which bumps the generation that restarts the wired presence listener — which is why `PairingSecretStore.rename()` is a **no-op when the name has not changed**. It runs on every connect, and without that guard the cable listener would restart on every single connection. |
| | | ⚠ **Next session starts here.** Read design doc §1, §1.1 and §7.1, then Findings above. Phase 1 proceeds; the accuracy argument alone no longer carries it, and ~~M1b is the measurement that matters~~ — **withdrawn; M1b was dismissed as circular**. |
| 2026-08-31 | 2W | ✅ **Phase 2W opened. `PinPointStudio`, `PinPointCapture` and `libppcp` pulled to latest first** (`1af2e11`, `2460d92`, `a9785bb`) — this session's own baseline Debug build of the pulled tree was clean before anything below was touched. |
| 2026-08-31 | 2W | ✅ **W1 DONE — `openProvider()`'s `Kind::Tcp` arm now actually dials.** A generic `socket(AF_INET,…)` + non-blocking `connect()` to `prov.host:prov.port`, sharing a new `finishConnect()` helper with the existing AF_UNIX arm rather than duplicating the pending/timeout/refused logic a second time. Everything above the socket byte — plist framing, the ⛔ `PortNumber` byte-swap, the `ConnectionType` filter, `Status::describe()`'s existing Windows wording — needed no change, exactly as the brief predicted. |
| 2026-08-31 | 2W | ✅ **W3 DONE — the usbmux client suite is un-gated on Windows and now proves the wire format there too.** `usbmuxd_stub.h` grew a TCP-loopback mode (bind `127.0.0.1:0`, hand the bound port to `Provider::tcpSocket()`, new factory) behind the *same* `Ppcp::pp_socket_t` the product code already uses, so the platform split is almost entirely `#ifdef _WIN32` swaps of `closesocket`/`WSAPoll`/`ioctlsocket(FIONBIO)` for their POSIX names — no new abstraction. `ppcp_transport_test.cpp`'s three dial-seam rows (contract C1) needed the same treatment; two were already portable once the loopback-connect helper was widened, the third used `socketpair(AF_UNIX,…)`, which **Winsock does not implement at all** — substituted with a short-lived `127.0.0.1:0` listener that hands back an equivalent connected, non-blocking pair (`tcpSocketpairNonBlocking()`). ✅ `ctest`: `ppcp_usbmux_test` 15/15, `ppcp_transport_test` (whole suite, dial-seam included) all pass. |
| 2026-08-31 | 2W | ✅ **W4 DONE — mDNS ported, and Windows joins the FAST resolve path, not Linux's.** CMake gained a Windows arm of the DNS-SD probe (Bonjour SDK for Windows — fixed default install path, `BONJOUR_SDK_HOME` override), widening `PP_HAVE_DNS_SD` exactly as L3 did for Avahi. ⛔ **The one real design call**: macOS and Windows both talk to *Apple's own* `mDNSResponder` — natively there, and via the Bonjour SDK's `mDNSResponder.exe` service here, the SAME upstream codebase cross-compiled rather than a second implementation — so both answer a local resolve inline in sub-millisecond time and are safe to call from inside the browse callback, unlike Avahi's independently-implemented compat shim which measurably deadlocks there (Phase 2L). New macro `PP_DNS_SD_INLINE_RESOLVE = __APPLE__ \|\| _WIN32` replaces the old `__APPLE__`-only split around `BonjourBrowser`'s epoll machinery, so Windows takes the same `struct pollfd`-based bounded-poll path macOS already had — **Linux's epoll code is untouched and still Linux-only**. ⚠ **Reasoned from the shared codebase, not yet measured on Windows hardware** the way the Avahi number was (no Bonjour SDK on this box) — confirm on first bring-up, same caveat the design doc already carries for AMDS trust. |
| 2026-08-31 | 2W | ⛔ **`dnssd.dll` is DELAY-LOADED, not implicit-linked — found by tracing, not by a failure.** It ships only with iTunes / "Apple Devices" / Bonjour Print Services and may not be redistributed (§4.3); an ordinary implicit link would crash the whole app at load time on any machine without one of those installed, which RV 3.6a's *"absence is not an error"* forbids. Same mechanism this codebase already uses for `Spinnaker_v140.dll` (CLAUDE.md): `/DELAYLOAD:dnssd.dll` + `delayimp.lib`, plus a new `ppcp_dnssd_runtime.cpp` probe (`LoadLibraryW(L"dnssd.dll")`, magic-static, silent — RV 3.6a's one log line is the Qt-based caller's job, not this Qt-free probe's) gating both `makePlatformBrowser()` and `makePlatformAdvertiser()` on Windows before either touches a `DNSService*` symbol. |
| 2026-08-31 | 2W | ✅ **Full app build clean, zero warnings, on the pulled tree with every 2W change in it.** `ppcp_usbmux.cpp`, `ppcp_discovery.cpp`, `ppcp_dnssd_runtime.cpp` all compiled with nothing to report. Configured with no Bonjour SDK present (this box has neither AMDS nor the SDK installed): `PPCP discovery: no Bonjour SDK found — reconnection discovery is off. Pairing by code still works (RV 3.6b)` — the silent-absence path, exercised for real rather than merely reasoned about. |
| 2026-08-31 | 2W | ⚠ **Three gaps found in the standalone `ppcp-tests` suite and NONE of them is 2W's to fix — the suite had simply never run on Windows with OpenSSL before.** `-DOPENSSL_ROOT_DIR` had to be pointed at this box's `C:/pp-vcpkg/vcpkg_installed/x64-windows` (the app's own build directory already had it cached from an earlier session; the standalone suite is a fresh configure and has no such cache) — once it was, seven more targets sprang into existence that had never been reached on Windows before and surfaced their own, unrelated gaps: (1) `ppcp_bootstrap_test.cpp` — H10 guided pairing, a different work package — drives `socketpair(AF_UNIX,…)` at eleven call sites plus `MSG_DONTWAIT`; same class of fix as W3's one call site, an order of magnitude more of it. Excluded on Windows (`if(NOT WIN32)`) with the reason recorded in the CMakeLists comment rather than silently dropped. (2) `ppcp_app_tu_syntax`'s syntax-only probe passes `-fsyntax-only -Wno-unused-command-line-argument` — Clang/GCC flags MSVC's `cl.exe` cannot parse (`D8021`) — a tooling gap in a target that was never runnable on MSVC at all, syntax-only or otherwise. (3) `ppcp_rendezvous_test`/`ppcp_host_service_test` needed `iphlpapi`/`bcrypt` added to their own link lists (`_rv_link`/`_hs_link`) — the app's root CMakeLists already links these for exactly the same symbols (`GetAdaptersAddresses`, `BCryptGenRandom`) but the standalone suite keeps its own separate lists and nobody had needed to add them there yet. Fixed as build-plumbing, not counted against 2W. |
| 2026-08-31 | 2W | ⚠ **A genuine Windows/environment timing finding, in code this session added: a loopback connect to an unbound TCP port is a SILENT DROP on this box, not the instant RST real hardware gives a closed port.** `ppcp_usbmux_test`'s new `absentProvider()` fixture (a TCP port nothing listens on, W3's Windows substitute for AF_UNIX's "path does not exist") took **~2.0–2.1 s** to resolve — astride `listDevices()`'s 2000 ms production default — so the SAME code path landed as `Status::NoProvider` in one run and `Status::Timeout` in the next, a race rather than a defect: both mean *no usbmux provider*, both retry, neither is ever a banner (RV 3.6a). Test now accepts either on Windows, with the finding recorded in the assertion's own comment rather than silently loosened. ⚠ **Not reproduced as a hard RST here** — suspected sandbox/virtualised-network characteristic of *this* box rather than a Windows-general one; worth re-checking on bare metal before trusting the 2000 ms default's margin there. Left the production constant alone: changing a timeout to fit one sandboxed VM's network stack would be exactly backwards. |
| 2026-08-31 | 2W | ⚠ **`ppcp_link_bind_test` — 2 of 10 flaky, and NOT this session's code.** `ConcurrentDialsBindIntoSeparateLinks` and `AnAbandonedDialDoesNotDisturbTheNextLink` (ENC 2.1b/2.1c, plain-TCP `Connector`/`dialTcp()` — no usbmux, no wired transport) intermittently return a `nullptr` link where one is expected; `ppcp_transport_test.cpp`'s own dial-seam rows, added this session and exercising the identical `Connector::connect()` machinery under a custom `cfg.dial`, passed reliably throughout. Same root-cause family as the row above is plausible (this box's loopback/scheduling characteristics under concurrency) but unconfirmed — recorded rather than chased, since this file predates 2W and was, like the three gaps above, simply never run on Windows before this session. |
| 2026-08-31 | 2W | ✅ **`ctest` on the standalone suite: 13/15 pass** (excluding `ppcp_app_tu_syntax` and the opt-in `ppcp_advertise_hold`, both pre-existing gaps above) — every PPCP-discovery and wired-transport target green, `ppcp_usbmux_test` 15/15 once the timing fixture above was made tolerant, `ppcp_transport_test`'s new dial-seam rows all pass, `ppcp_host_service_test`/`ppcp_rendezvous_test`/`ppcp_advertise_test` all pass now that Qt's `bin/` is on `PATH` (they crashed `0xc0000135` — DLL not found — until it was; the earlier PATH omission was this session's, not a code defect). Only `ppcp_link_bind_test`'s two pre-existing flakes remain, recorded above. |
| 2026-08-31 | 2W | ⛔ **WHAT IS STILL UNVERIFIED, and it is the same shape of gap Phase 2L had at the same point: everything testable without a person passes, and nothing here proves wired reconnection works on Windows.** This box has **neither** Apple Mobile Device Service **nor** the Bonjour SDK for Windows installed, and no PPCP pairing exists (no physical phone). So: W1's dial to a *real* AMDS, W2's §6.2 table against real Connect/mux-layer refusals, W4's mDNS against a *real* Bonjour-for-Windows responder, and the "Windows — done when" `session_open`-with-no-QR criterion are all **code-complete and unexercised**. ✅ **What differs from a from-scratch port**: every layer below the two absent installers is now proven — the plist protocol, the byte-swap, the diagnostics mapping, the dial-seam contract, the DNS-SD factory split — so what remains is prerequisite installation and one hardware session, not further coding. |
| 2026-08-31 | 2W | ⚠ **W2 not separately checked off.** Its content — silent absence (RV 3.6a), `Status::describe()`'s Windows wording, the six-not-seven-causes correction — was already satisfied by W1's own code and by the pre-existing `describe()` strings; nothing new was needed. Left open because its actual acceptance criterion (§6.2's rows distinguishable **in practice**, and M5's untrusted-device row) needs the same hardware the "done when" bar does. |
| 2026-08-31 | 2W | ✅ **THE BONJOUR SDK FOR WINDOWS IS NOW INSTALLED AND W4 IS VERIFIED LIVE — half the "unverified" gap above is closed the same session it was written.** Mark installed the official runtime first (`winget install Apple.Bonjour` — `Bonjour64.msi`, `swcdn.apple.com`, no auth needed; confirmed `Bonjour Service` running and `C:\Windows\System32\dnssd.dll` present), then the SDK itself (`bonjoursdksetup.exe`, requires an Apple Developer login I cannot do on the user's behalf — Mark downloaded it and ran it himself). Installed to the default `C:\Program Files\Bonjour SDK\`, exactly where the CMake probe already looked. |
| 2026-08-31 | 2W | ⛔⛔ **TWO REAL DEFECTS SURFACED THE MOMENT THE SDK WAS ACTUALLY PRESENT — both were reasoning errors made without the hardware to check them against, exactly the pattern this whole plan keeps finding.** |
| 2026-08-31 | 2W | ⛔ **DEFECT 1 — HEADER ORDER: `<dns_sd.h>` INCLUDED BEFORE `<winsock2.h>` BROKE THE WHOLE `_WIN32` BUILD, ~150 CASCADING ERRORS.** The Bonjour SDK's `dns_sd.h` (untouched since circa 2007) does a bare `#include <windows.h>` with no `WIN32_LEAN_AND_MEAN` of its own; unguarded, that pulls in the LEGACY WinSock 1.1 `<winsock.h>`, and `ppcp_discovery.cpp`'s own later `#include <winsock2.h>` then collided with it — `struct sockaddr` redefinition, every BSD-socket function "redefinition; different linkage", cascading into `ws2tcpip.h`. ✅ **Fixed at the include order, not by patching Apple's header**: `WIN32_LEAN_AND_MEAN` defined and `<winsock2.h>`/`<ws2tcpip.h>` included BEFORE `<dns_sd.h>` — winsock2.h's own guard is what stops the legacy header redefining anything once dns_sd.h's `windows.h` include runs. This was invisible without the real SDK: the macro-only build (`PP_HAVE_DNS_SD` never set) never compiled `<dns_sd.h>` at all. |
| 2026-08-31 | 2W | ⛔⛔ **DEFECT 2 — `/DELAYLOAD:dnssd.dll` WAS A COMPLETE NO-OP, AND I HAD NO WAY TO KNOW UNTIL THE REAL LIB EXISTED TO INSPECT.** `dumpbin /archivemembers` on the real `dnssd.lib` shows exactly ONE object, `DLLStub.obj` — this is not a conventional import library at all. Every `DNSService*` entry point is a small stub that does its OWN `LoadLibrary`/`GetProcAddress` against `dnssd.dll` **at call time** — Apple's own hand-rolled delay-loading, predating the `/DELAYLOAD` linker convention this codebase uses for `Spinnaker_v140.dll`. `dumpbin /dependents PinPointStudio.exe` confirmed it directly: `dnssd.dll` appears in **neither** the regular nor the delay-load dependency list, ever — there is no PE import descriptor for `/DELAYLOAD` to act on. ✅ **The safety property I was chasing was real, just already provided by Apple at the SDK level rather than by the linker flag I added.** Removed the dead `/DELAYLOAD:dnssd.dll` + `delayimp.lib` lines and corrected every comment that had described the mechanism wrongly (CMakeLists.txt, `ppcp_dnssd_runtime.h/.cpp`). **Kept `ppcp_dnssd_runtime.cpp`'s `LoadLibraryW` probe anyway** — not because it gates a thunk, but as this codebase's own explicit, early diagnostic point, consistent with how AMDS and Spinnaker are both checked before touching a symbol, rather than trusting every one of a 2007-era stub library's error paths. |
| 2026-08-31 | 2W | ✅ **REBUILT CLEAN AFTER BOTH FIXES — zero errors, zero warnings, app and standalone suite alike.** Configure line changed to `PPCP discovery: DNS-SD via Bonjour SDK for Windows (self-delay-loading)`, correctly naming what is actually happening now. |
| 2026-08-31 | 2W | ✅✅ **`PpcpAdvertise.RegisteringWithTheRealResponderIsVisibleToARealBrowse` PASSES — genuine, live, end-to-end, no mock on either side.** Registers `_ppcp._tcp` with the REAL `mDNSResponder.exe` service now running on this box, browses for its own advertisement through the REAL browse API, resolves it, and verifies `txtvers`/`role`/`pv`/`rn`/`rid`/`port` round-tripped correctly and that the minted `rid` is verifiable against `K_id` and rejected against a different key (3.4b) — 1018 ms, passed. This is the same class of proof the Linux port's `avahi-browse` external observation gave L3, achieved here without a separate CLI probe (`dns-sd.exe` is not shipped with this SDK's `Bin\`, only a GUI `ControlPanel.exe`) because the test itself already puts a real, independent OS service in the loop rather than mocking either end. |
| 2026-08-31 | 2W | ✅ **Full standalone suite re-run with the real SDK present: same shape as before, nothing regressed.** `ppcp_rendezvous_test`, `ppcp_advertise_test` (now genuinely exercising the real-responder row instead of skipping it) both pass; `ppcp_host_service_test` passed 35/36, one flake (`TheCountdownRunsWhileTheHostIsStillWaitingForAPhone` — a `codeChanged` signal-count timing assertion, nothing to do with discovery or wired transport) of the same environment-timing-flake family as `ppcp_link_bind_test`'s two, recorded rather than chased for the same reason. |
| 2026-08-31 | 2W | ⛔ **REVISED STATUS: W4 is no longer "code-complete and unexercised" — it is VERIFIED, live, on this box.** What remains unverified is narrower than the previous entry stated: **W1 (a real AMDS) and a physical phone** for the wired half and the full `session_open`-with-no-QR criterion. mDNS discovery itself — registration, browse, resolve, TXT round-trip, `rid` verification — is now proven exactly the way Phase 2L proved it on Linux, on real Windows hardware, against Apple's own responder. |
| 2026-08-31 | 2W | ✅ **Full suite re-run once more, clean: 14/15.** `ppcp_host_service_test`'s countdown flake from the run above did NOT reproduce here — confirms it is a flake and not a deterministic defect, same family as `ppcp_link_bind_test`'s two. **Only remaining red: `ppcp_link_bind_test`, unchanged, pre-existing, out of 2W's scope.** Everything this phase actually owns — usbmux protocol (W1/W3) and mDNS discovery (W4) — is green. |
| 2026-08-31 | 2W | ✅✅ **AMDS INSTALLED TOO (`winget install Apple.AppleMobileDeviceSupport` — `AppleMobileDeviceSupport64.msi`, `swcdn.apple.com`, no auth) — W1 NOW VERIFIED LIVE AGAINST THE REAL SERVICE, NOT JUST THE STUB.** `Apple Mobile Device Service` running, listening `127.0.0.1:27015` exactly where `Provider::platformDefault()` looks. A throwaway probe (`Client(Provider::platformDefault()).listDevices()`, no phone attached) reached the REAL daemon — full plist handshake over the TCP path W1 added — and got back exactly the healthy no-device answer: `status=no device attached (or a charge-only cable)`, `systemError=0`, `devices=0`. **Both halves of Phase 2W (W1's usbmux protocol and W4's mDNS) are now proven against real Apple services on this box, independently of each other and of any phone.** |
| 2026-08-31 | 2W | ⛔ **THE ONLY THING LEFT IS A PHYSICAL PHONE.** Every prerequisite this box can hold without one, it now holds: AMDS (W1), the Bonjour SDK (W4), and — untouched by this session — QR pairing, which never depended on either. What remains untestable from here: a phone actually cabled in and recognised (`ConnectionType == "USB"`, the ⛔ `PortNumber` byte-swap against a real device rather than the stub, `min_rtt`/`offset_sigma` for AMDS specifically), a phone reconnecting over WiFi via the mDNS this session proved component-by-component but never end-to-end through the running app, and the "Windows — done when" `session_open`-with-no-QR bar itself. Handed to Mark rather than attempted here — this session does not launch or drive the app against a device. |
| 2026-08-31 | 2W | ⚠ **Cosmetic log bug found and fixed while handing over for the first real pairing: `ppcp_wired_link.cpp:416`'s "wired path armed —" line printed `m_provider.path` unconditionally**, which is `Provider`'s struct-default `"/var/run/usbmuxd"` on a `Kind::Tcp` provider — `platformDefault()` never touches `path` on Windows. So the very first line of Windows wired diagnostics named the wrong provider even though the TCP connection underneath it was correct (verified separately by the AMDS probe above). Added `Provider::describe()` (host:port for Tcp, path for Unix) and pointed the log at it. Rebuilt clean; log now reads `wired path armed — 127.0.0.1:27015`. |
| 2026-08-31 | 2W | ⛔⛔ **FIRST REAL PAIRING ATTEMPT, AND IT REPRODUCES THE OPEN LINUX-SESSION FINDING — "EVERY DIAL FAILS TLS AUTHENTICATION AFTER A WORKING LINK" — with sharper evidence than that session ever got.** QR pairing succeeded (`13:49:53`, `transport=wifi`, `pairing=50f5410f2c544c0f`, both channels bound, peer declared, 2 camera Sources). The link **died 29 s later** (`13:50:22`, `link closed`, "0 link(s) still up") and **every reconnect since has failed** (`PPCP TLS handshake failed`, repeating). The permanent diagnostic the Linux session added for exactly this case (`diagnosticExport()` after 3 consecutive refusals) fired and is the smoking gun: `resolver: calls=6 resolved=3 exhausted=3` — **half the identity-resolution attempts hit `exhausted`, and only one held pairing can produce that**: `f88f449e846b7717`, `uses=1/1`, `persisted=no` — the SPENT one-time QR pairing code, still held with a `+292s` expiry. The other, `50f5410f2c544c0f`, `uses=0/1`, `persisted=yes`, is untouched. ✅ **Reading: the phone is presenting the SPENT QR-code pairing's identity on reconnect, not the newly-persisted one that resulted from it** — every such attempt is correctly refused by the host (RV 5.3b working exactly as designed), which is why the phone can no longer see the host. mDNS itself is not implicated: the advertiser is rotating exactly the one PERSISTED pairing (`"rotating 1 pairing(s)"`), so the `rid` on the wire names the right pairing — this looks like a **device-side (PinPointCapture) bug in which stored credential gets offered**, not a Windows host defect, and not something this session's W1/W3/W4 code touches (`ppcp_rendezvous.cpp`'s resolver is unmodified by this port). ⚠ Cross-checked against a fresh `PinPointStudio_log_20260831_135233.txt` export Mark provided — identical content, same conclusion, not an artifact of the stderr capture. A read-only Explore pass into `PinPointCapture`'s reconnect/pairing-store code is in flight to locate the exact call site before anything is changed there. |
| 2026-08-31 | 2W | ⛔ **CORRECTION TO THE ROW ABOVE — I misread `diagnosticExport()`'s `uses=` field, and it flips who is exhausted.** It prints `usesRemaining`, not a spent-count: `uses=0/1` is EXHAUSTED, `uses=1/1` is UNSPENT. So `50f5410f2c544c0f` (`uses=0/1`, `persisted=yes` — the pairing the phone actually paired and reconnects on) is the one stuck exhausted; `f88f449e846b7717` (`uses=1/1`) is an unrelated, still-live QR code and was never the problem. The "device presents the wrong identity" theory is wrong — the phone was reconnecting on the RIGHT pairing the whole time, and the host was refusing it anyway. |
| 2026-08-31 | 2W | ⛔⛔ **ROOT CAUSE, HOST-SIDE, ONE LINE — `Entry::remember()`, `ppcp_rendezvous.cpp:269-279`.** `noteLinkEstablished()` spends a `mu:1` code's `usesRemaining` to 0 (7.3a, correct — that's the CODE being redeemed) and calls `remember()` to persist it in the SAME function. `remember()` sets `persisted = true` and stops — nothing ever promotes the now-persisted pairing's `usesRemaining` back to unlimited, unlike the TWO OTHER places that create a persisted `Entry`: `adoptGuidedPairing()` (`:620`, `e.usesRemaining = UINT64_MAX;`) and `loadPersisted()` (`:696`, same line, with the comment *"a persisted pairing is not a code: it has no expiry and no use limit (7.4a)"*). So a pairing born from a QR code is permanently stuck at `usesRemaining=0` for the life of the process — every reconnect refused as EXHAUSTED — while a pairing born from a guided pairing or reloaded from disk is correctly unlimited. ✅ **This is exactly why "restart fixes it" kept masking the class of bug the Linux session already flagged**: `loadPersisted()` reloads from the store and re-derives `usesRemaining = UINT64_MAX` on every fresh launch, silently repairing the very entry the bug had broken. **Fix: one line, `usesRemaining = UINT64_MAX;` right after `persisted = true;` in `remember()`**, the same convention the other two sites already use. ✅ **Confirmed NOT a device-side (PinPointCapture) issue**: the earlier Explore pass into that repo found `K_id` is a pure function of `PRK` (RV 11.1a — "a pairing is a pairing"), so the phone presents the identical, correct identity on reconnect; it was always this host resolving it and then refusing it. |
| 2026-08-31 | 2W | ✅ **Regression test added — `ppcp_rendezvous_test.cpp`, `APersistedPairingHasNoUseLimitEvenWhenItWasBornFromAMuOneCode`.** Pairs on a default (`mu:1`) code, calls `noteLinkEstablished()`, asserts `usesRemaining == UINT64_MAX` post-persist, then dials AGAIN on the same identity (the reconnect the live bug broke) and asserts it succeeds with no new `refusedExhausted`. `ppcp_rendezvous_test`: 100% pass, nothing else regressed (verified: the fixture this test sits beside, `RT5_...SingleUseCodeIsRefusedAtTheHandshake`, runs with NO store installed, so `remember()` never fires there and its `usesRemaining==0` assertion is unaffected by this fix — checked before applying, not after). |
| 2026-08-31 | 2W | ✅✅ **FIX CONFIRMED LIVE — Mark reconnected successfully.** App rebuilt with the fix, relaunched, phone reconnected. This is the first end-to-end proof of the "Windows — done when" bar's core claim (a persisted pairing reconnects) on this platform, on real hardware, closing the loop the AMDS/Bonjour component checks could not reach by themselves. |
| 2026-09-03 | 2W | ⛔⛔ **THE BONJOUR SDK FOR WINDOWS PATH IS RETIRED — a real, user-visible problem, not a latent one.** Mark found `mdnsNSP.dll` (Bonjour's Winsock namespace provider, last touched by Apple in 2015) throwing a Program Compatibility Assistant popup — *"This module is blocked from loading into the Local Security Authority"* — on this box. Investigated properly rather than waved off: Windows' CodeIntegrity/Operational log showed **`lsass.exe` hit the block twice, at last boot** (the popup's trigger — Microsoft wires that specific dialog to LSA-protection blocks, not general Code Integrity ones), plus **659 hits on `svchost.exe`** and **76 on Windows Defender's own service** (`MpDefenderCoreService.exe`) in the background, all regardless of whether PinPointStudio was even running. ⚠ **My first read of this understated it** — I initially framed it as "mostly harmless background noise" before finding the `lsass.exe` entries; Mark was right to push back and ask for the real diagnosis. **The dependency question this raises: shipping "install Bonjour for Windows" as the prerequisite for working WiFi reconnect would put this exact popup on every Windows customer's machine, on every boot, for as long as they use the feature it exists for** — not a today's-Windows-quirk, since LSA/CodeIntegrity hardening is a direction Microsoft keeps tightening, not loosening. ✅ **Decision (Mark's, after discussion): build a native mDNS/DNS-SD engine — no Apple dependency of any kind — rather than work around Bonjour's symptom.** This was already named as the alternative ("a native responder") in §W4 of this tracker's own brief; this session builds it. |
| 2026-09-03 | 2W | ⛔ **GPL-compliance constraint stated explicitly by Mark before any code was written, and it shaped the whole approach**: nothing may be adapted from an existing mDNS implementation (Apple's mDNSResponder, Avahi) regardless of that implementation's own license, since this codebase's own license (GPLv2-or-later) and Apple's mDNSResponder (Apache 2.0) do not mix cleanly under a strict GPLv2-only reading. **Answer: write the wire codec and the socket engine from scratch against the public RFC 6762 (Multicast DNS) / RFC 6763 (DNS-SD) specification text only** — no third-party library, nothing but Winsock (an OS API, not a redistributed one) and this project's own existing protocol-semantic functions (`buildTxtRecord()`, `parseTxtRecord()`, `decideDial()`, `reachableEndpoints()` — all pre-existing, already-licensed PPCP-RV code, untouched by the mDNS work itself). Both new files' header comments state this provenance explicitly, for auditability. |
| 2026-09-03 | 2W | ✅ **New: `ppcp_mdns_wire.{h,cpp}` — the wire codec, portable, no socket, no platform `#ifdef`.** `MessageWriter` (header/question/record encode, RDATA builders for PTR/SRV/A — TXT rdata is `buildTxtRecord()`'s own output verbatim, DNS-SD's TXT format and PPCP-RV's TXT format being the identical length-prefixed-strings wire shape by construction, so there is deliberately no second TXT encoder to disagree with the first) and `parseMessage()` (header/question/record decode). ⛔ **The trap this class of code is famous for, handled deliberately**: a NAME-bearing RDATA field (PTR's target, SRV's target) can carry an RFC 1035 §4.1.4 compression pointer into EARLIER PART OF THE SAME PACKET — decoding against an isolated copy of just the RDATA bytes breaks exactly there, so `Record::targetName` is decoded eagerly against the FULL buffer at parse time, not deferred. Pointer-chasing is loop-guarded (a label-count cap, not a visited-offset set — simpler, and just as safe against any pointer arrangement). Scope stated in the header, deliberately narrow: one service type (`_ppcp._tcp`), IPv4 only (IPv6 mDNS is a different multicast group and a real, documented gap — not attempted), no write-side name compression (the whole record set is a few hundred bytes, nowhere near needing it — compression IS followed on READ, since real responders use it), no known-answer suppression (RFC 6762 §7.1, an optimisation this network's own 3.6a tolerance makes optional). |
| 2026-09-03 | 2W | ✅ **`ppcp_mdns_wire_test.cpp` — 8/8 pass, 0 ms total, on every platform (no `_WIN32` guard — pure codec).** Round-trips a full PTR+SRV+TXT+A answer set with every field checked field-by-field; proves the cache-flush bit does not corrupt the class field it shares a byte with; **builds a message BY HAND (not through `MessageWriter`, which never compresses on write) specifically to prove the READER follows a compression pointer inside RDATA independently of the writer ever producing one** — the two must not be allowed to only agree with each other; a truncated header, an RDLENGTH running past the buffer, and a **self-referencing compression-pointer loop** are all refused rather than crashed or hung on (the loop case's real assertion is that the call returns at all). |
| 2026-09-03 | 2W | ✅ **New: `ppcp_mdns_native.{h,cpp}` — the Windows socket engine, `RvBrowser`/`RvAdvertiser` implementations, one UDP 5353 multicast socket each.** `NativeAdvertiser`: on `start()`, reuses `reachableEndpoints()` (the SAME IPv4 enumeration RV 4.3d's own QR code already uses — a phone gets one, consistent answer to "how do I reach this host" regardless of which channel told it) for its A record(s), derives an SRV/A target hostname from `GetComputerNameExW` sanitised to a valid DNS label, announces once on `start()`, answers any query naming `_ppcp._tcp.local`/its own instance/its own hostname with the WHOLE bundle (simpler and always correct for a four-record set — no per-question-type filtering needed), sends a goodbye (TTL=0, every asserted record) on `stop()`. `updateTxt()` sends **only** the TXT record, cache-flush set, PTR/SRV untouched — the one-packet-per-rotation property `kAdvertisementCycleTargetS`'s whole timing budget assumes, and which `ppcp_discovery.h`'s own header comment had flagged as **unconfirmed and possibly wrong** for any non-Bonjour Windows API (`windns.h`'s `DnsServiceRegister`/`DnsServiceDeRegister` has no evident update call) — this engine makes the assumption true by construction rather than inheriting someone else's API shape. `NativeBrowser`: sends a PTR query on `start()`, tracks partial per-instance state (PTR seen? SRV? TXT? A?) and fires `FoundFn` once all four are known — matching `RvBrowser::FoundFn`'s own documented bar ("resolves far enough to carry rn and rid") — expires an instance whose TTL lapses with no refresh even absent a goodbye packet (packet loss is ordinary on a shared multicast segment, 3.6a). |
| 2026-09-03 | 2W | ⚠ **A new interface method, `RvBrowser::tick(nowS)`, added with a no-op default — needed because `process()` alone cannot drive periodic re-querying.** `process()` only fires on socket readability (an EXTERNAL event); a browser with no platform daemon of its own (nothing else on this process owns UDP 5353 for it) needs to periodically SEND its own query (RFC 6762 §5.2's continuous-querying schedule — 1 s initial interval, doubling, capped at 60 s) and expire stale entries, and nothing calls `process()` to let it do that if nothing else is talking. Wired into the SAME call site `RvReconnectionAdvertisement::tick()` already uses (`PpcpHostService::onTick()`, the existing 20 ms/50 Hz timer) — one more `if (m_browser) m_browser->tick(nowS);` line, costing every OTHER backend (Bonjour, Avahi) nothing beyond the no-op default, since their platform daemon already does continuous querying on its own. |
| 2026-09-03 | 2W | ✅ **`makePlatformBrowser()`/`makePlatformAdvertiser()` (`ppcp_discovery.cpp`) — Windows branch moved ahead of the `PP_DNS_SD_AVAILABLE` macro entirely, unconditional, no runtime probe.** `PP_HAVE_DNS_SD` is no longer ever defined on Windows (the CMake probe that used to set it is deleted — see below), so `BonjourBrowser`/`BonjourAdvertiser`'s ~700-line `#if defined(PP_DNS_SD_AVAILABLE)` block now compiles OUT on Windows automatically, by the SAME macro machinery that already excluded it before W4 existed — nothing needed hand-stripping. Two now-stale header comments corrected: `ppcp_discovery.h`'s *"this process still binds no multicast socket"* (now qualified — true on macOS/Linux, false and DELIBERATELY so on Windows) and its *"Windows is deferred (CA5)"* `RvAdvertiser` comment (CA5's dependency OBJECTION is answered, not overridden — there is no longer a dependency to object to). |
| 2026-09-03 | 2W | ✅ **Cleanup: the delay-load probe (`ppcp_dnssd_runtime.{h,cpp}`) and both Bonjour-SDK-for-Windows CMake blocks deleted outright, not left dead.** They existed only to gate `dnssd.dll` calls that no longer happen — root `CMakeLists.txt`'s `if(WIN32)` DNS-SD block now just adds `ppcp_mdns_wire.cpp`/`ppcp_mdns_native.cpp` to the source list and prints one status line; `src/Ppcp/tests/CMakeLists.txt`'s matching block does the same for the four test targets that compile `ppcp_discovery.cpp`. Nothing references `BONJOUR_SDK_HOME` or looks for `dns_sd.h` on Windows any more. |
| 2026-09-03 | 2W | ✅✅ **VERIFIED, FOUR INDEPENDENT WAYS, BEFORE CALLING THIS DONE.** (1) Unit: `ppcp_mdns_wire_test` 8/8, `ppcp_advertise_test`/`ppcp_rendezvous_test` both pass — the pre-existing `RegisteringWithTheRealResponderIsVisibleToARealBrowse` test (originally written for Bonjour) needed **zero changes** to exercise the new engine, because it calls `makePlatformAdvertiser()`/`makePlatformBrowser()` and never named Bonjour directly — it now runs the FULL native advertise→browse→resolve→verify loop in 4 ms and checks every RT-7/RT-8 field (txtvers, pv, role, rn, rid, port) plus the `rid`-against-`K_id` cross-check (accepts the right key, rejects a different one). (2) Live app log: PPS relaunched prints `discovery: native mDNS, no Bonjour dependency (browse only)` and `advertising: advertising _ppcp._tcp as PPCP-0058F8E3 …` with no errors. (3) Socket-level, external observation: `Get-NetUDPEndpoint -LocalPort 5353` shows `PinPointStudio` bound to `0.0.0.0:5353` **coexisting** with Apple's own `mDNSResponder` (still running, bound to `::1`/the LAN address) and Brave's own mDNS listener — direct proof the multi-listener assumption `SO_REUSEADDR` depends on holds on this box, in front of a real second implementation. (4) Wire-level, fully independent process: a hand-built PowerShell probe (its own `UdpClient`, joining the SAME multicast group, sending a genuine RFC 6762 PTR query) got back a real 278-byte, 4-answer response naming the exact instance PPS's own log had just printed — a client that has never heard of this codebase, talking to it correctly over the actual network. |
| 2026-09-03 | 2W | ✅ **Full standalone suite re-run for a final regression check: 15/17 pass.** Every discovery/mDNS-touching target green (`ppcp_mdns_wire_test`, `ppcp_rendezvous_test`, `ppcp_advertise_test`, `ppcp_host_service_test`). Two failures, both pre-existing and unrelated to this session: `ppcp_link_bind_test` (documented earlier in this tracker, plain-TCP `Connector` flakiness, nothing to do with discovery) and **`ppcp_live_session_test.TheProbeExchangeRecoversTheOffsetAndTheSkew`** — new, in code pulled from upstream just before this session started (`ppcp_live_session.cpp`/`.h`/`_test.cpp` all changed substantially in that pull) and never touched by the mDNS work. ⚠ **Checked, not assumed**: re-ran it standalone 5 times, `--gtest_repeat=5`, **5/5 deterministic failures** (`Expected: (rel.offset_sigma_ns) > (0.0), actual: 0 vs 0`) — a real bug, not a flake, in Mark's own in-flight sync/probe-exchange work. Flagged rather than fixed: out of scope for this session and not code I have context on. |
| 2026-09-03 | 2W | ✅ **Documentation updated to match**: `docs/developer/ppcp_prerequisites_developer_guide.md`'s Windows section rewritten — no longer tells a reader to install the Bonjour SDK (now actively wrong AND actively harmful advice, given the LSA finding above), states plainly not to install the Bonjour runtime to get discovery working, and separates the (unaffected, still-needed) AMDS/usbmux prerequisite from the (now unnecessary) mDNS one. `ppcp_discovery.h`'s file-level and `RvAdvertiser`-level header comments corrected to state the Windows exception rather than the now-false universal claim. |
| 2026-09-03 | 2W | ⚠ **A NEW, EXPECTED, FIRST-RUN COST OF THIS TRADE: WINDOWS FIREWALL NOW PROMPTS ON THIS APP BY NAME.** Mark hit "Windows Defender Firewall has blocked some features of this app" dialogs and had to click through them. ⛔ **Direct, mechanical consequence of the architecture, not a defect** — the retired Bonjour-SDK path never touched the network itself; it asked the ALREADY-RUNNING, ALREADY-EXEMPTED `mDNSResponder.exe` system service to do it over local IPC, so `PinPointStudio.exe` itself had nothing for the firewall to ask about. The native engine is different BY DESIGN: this process now owns the UDP 5353 socket outright, and Windows prompts once per unique exe path, per network profile, the first time an app asks to receive on one — every future install, from a fresh binary or after an update that changes the exe path, will see this once. ✅ **Confirmed working correctly afterward**: `Get-NetFirewallRule -DisplayName "*PinPointStudio*"` now shows `Allow` inbound rules for this build's exe on both Private and Public profiles; a subsequent clean relaunch started in ~5 s with no hang and the native engine advertising normally. ⛔ **Product note for whoever scopes the installer**: this is a NEW first-run UX element the Bonjour-SDK path never had, and it is the price of removing that path's own, worse first-run element (the LSA popup, on every boot, forever, not just once). Worth being upfront about in release notes / support materials rather than letting an operator discover it cold. |
| 2026-09-03 | 2W | ✅✅✅ **THE LAST GAP CLOSED — A REAL PHONE RECONNECTED OVER WIFI, ON A HOST PROCESS WHERE THE BONJOUR SDK CODE PATH DOES NOT EXIST AT ALL.** `08:33:27`, same held pairing as the earlier live test (`50f5410f2c544c0f`), `transport=wifi`, no QR rescanned: `PPCP channel 1/0 bound`, `link up`, cameras declared, preview resumed. ✅ **Why this is definitive rather than merely another data point**: this PPS process was launched AFTER the Bonjour-SDK removal — `BonjourAdvertiser` is not compiled into this binary, full stop. RV §3's design has the PHONE do the browsing and dialling on every reconnect (no cached-IP fallback anywhere in this protocol, per every reconnect path this whole tracker has described), so the phone finding this host, verifying its current `rid` against a held pairing, and completing the TLS-PSK handshake is only possible if it freshly resolved the native advertiser's PTR/SRV/TXT/A records. **This is real-hardware proof of the exact thing this session set out to replace**, on top of the four synthetic/component-level proofs already logged above. `offset_sigma` settled at 1.34 ms, comfortably under the 5 ms arbitration gate, on the WiFi link this discovery path produced. |
| 2026-09-03 | 2W | ⚠ **Two things noticed live, unrelated to mDNS, recorded rather than acted on.** (1) `payload table full [...] EVICTED unfinished` spam on both camera streams starting immediately after reconnect, alongside repeated `host tick stalled ~850-1300 ms` warnings — the host's own 20 ms tick loop falling badly behind, well past what triggers "a phone declares the link lost after 3 missed intervals." Plausibly a Debug-build performance characteristic under two-camera load rather than a protocol defect (nothing about it is discovery- or pairing-shaped), but flagged rather than diagnosed — out of this session's scope and not chased further. (2) The process exit was clean: `link down: shutdown "pairing=...” — 0 link(s) still up`, not an abrupt `link closed` or a crash, and `Get-Process` confirms no process remained after — the "quitting after a phone paired terminates the process" class of bug this tracker's own git history shows was already fixed did not recur. |
| 2026-09-03 | 2W | ✅ **PHASE 2W (WINDOWS PORT), SECOND CUT — STATUS: mDNS FULLY VERIFIED, LIVE, NO APPLE DEPENDENCY. Wired (usbmux/AMDS) remains verified only against the real service with no phone cabled — the one piece still owed is a phone physically over USB, unchanged from this tracker's earlier "Windows — done when" entries.** Not committed as of this entry. |
| 2026-09-03 | 2W | ⛔ **What is NOT yet done, stated plainly.** No IPv6 (a real, documented gap — a Windows LAN with IPv4 disabled would see nothing). No receive-side IP-TTL check (RFC 6762 §11's off-link-spoof hardening — needs `WSARecvMsg`/ancillary-data parsing on Windows, real added complexity for a hardening measure this protocol's actual security boundary, TLS-PSK, does not depend on; every mDNS field is already untrusted per 3.6a regardless). No known-answer suppression (a traffic optimisation, not correctness). **Not yet tested with a live phone reconnecting over this specific engine** — the four verifications above are all component/protocol-level proof; the WiFi-reconnect-with-a-real-iPhone test the "Windows — done when" bar asks for was done against the Bonjour-SDK engine (now retired) two days earlier in this same tracker, not against this one. That is the next thing to prove, not a gap in this session's own claims. |
