# Wired transport for PPCP — design

**Status:** proposed, not built. 29 August 2026.
**Repos:** `PinPointStudio` (host), `PinPointCapture` (device), `libppcp` (protocol).
**Spec touchpoints:** `CORE` §3, §3.2, §6.3; `ENC` §2, §2.1; `RV` §2 (direct path), §3.5d, §5.2g, §5.3, §7.7.

---

## 0. The governing principle

**Wired is an optimisation. It is never a requirement.**

No feature, no session type and no diagnostic may be reachable only over a cable.
WiFi stays the supported transport; the cable makes it better. The reasons are
three and they all point the same way:

- Apple ships **no public API for direct USB communication on iOS**, and the
  protocol the phone speaks to a host over the cable is undocumented,
  unversioned and unsupported. usbmux is stable in practice because Xcode
  depends on it, not because anything is promised. **If it changes in iOS 27 we
  must lose a transport, not a product.**
- An App Store reviewer has an iPhone and nothing else — no PinPointStudio, no
  cable, no camera rig (§9).
- The design already has the shape: `ENC` 2.1b binds a link by `link_id` and
  forbids inferring anything from the transport address, so a transport swap is
  invisible above `link_bind`.

Everything below is written to that constraint. Where wired and correctness
disagree, wired loses.

**The kill path, concretely.** Wired sits behind a setting (default off until M1
and M10 report, default on after). Turning it off restores today's behaviour
exactly: the advertisement suppression of §6.1 lifts, the phone dials over WiFi,
and **no data migrates** — because nothing is stored per transport (§6.4). That is
the whole exit plan, and it is short only because the identity and storage design
made it so. ⚠ Anything that would lengthen it — a wired-only setting, a
transport-tagged store, a feature reachable only over the cable — is out of bounds
for that reason and not merely on taste.

---

## 1. Why this is worth building, in one number

`libppcp`'s sync estimator publishes an offset sigma with a **hard floor set by the
minimum observed round-trip time**. From `src/ppcp_sync.c:251-260`:

```c
double resid = sync_sqrt(var);
double asym  = (e->min_rtt_ns > 0) ? (double)e->min_rtt_ns * 0.5 : 0.0;
e->offset_sigma_ns = sync_sqrt(resid * resid + asym * asym);
```

> `offset_sigma_ns ≥ ½ · min_rtt_ns`

and the skew sigma is floored by the offset sigma over the measurement span
(`ppcp_sync.c:262-271`):

> `skew_sigma_ppm ≥ offset_sigma_ns · √2 / span_ns · 1e6`

Substituting one into the other:

> **`skew_sigma_ppm ≥ (min_rtt_ns · 0.707 / span_ns) · 1e6`**

Time to any target skew sigma is therefore **linear in minimum RTT**. Halve the
minimum RTT and you halve the convergence time, or hold the time and halve the
residual. Nothing else in the estimator is a lever the host controls: the window
size, the admit fraction and the filter constant are all in the library, and the
probe cadence does not appear in the expression at all.

That is the arithmetic behind the measured behaviour already recorded for this
project — *"~90s is link quality not cadence; MORE traffic is worse; 7 configs
measured, all reverted"*. Seven cadence and burst configurations were tried and
reverted because **cadence is not in the equation**. Raising traffic raises
`min_rtt` and makes it worse. The only variable with any leverage is the transport.

Worked, with the numbers we actually see:

| Transport | `min_rtt` | offset sigma floor | notes |
|---|---|---|---|
| WiFi, **as shipped before 29 Aug** | **12.95 ms** *(measured)* | 6.47 ms | 87% of it was this host's own 20 ms poll, not the network — §7 |
| **WiFi, measured 29 Aug 2026** | **1.64 ms** *(measured)* | **0.82 ms** | iPhone 16, idle link, converged at 350 s. `offset_sigma` **1.16 ms**, `skew_sigma` 10.5 ppm |
| USB via usbmux | ~0.5 ms *(predicted)* | ~0.25 ms | **unmeasured — M1** |

⛔ **The measured baseline is far better than this document originally assumed, and
it changes the size of the prize.** The table above once read "5 GHz WiFi, shared
infra: 4 ms" as its comparison point. The real number, once the host stops polling
(§7, shipped), is **1.64 ms** — and `offset_sigma` sits at **1.16 ms against the
5 ms gate below, a 4.3× margin**.

So on *this* measurement the cable argues for roughly a **3× improvement on a
number already 4× inside the gate**. The arithmetic is unchanged and still correct;
what changed is that most of the win on a quiet network turned out to be a poll in
this host, and has been taken **with no cable at all**.

⛔ **But read the measurement for what it is: one idle link on an uncontended
network, which is the BEST case for WiFi and the case a range does not have.**
1.64 ms is a floor, not a typical value. §3.2's own table is about the *tail*, not
the floor — *"2.4 GHz or congested WiFi: very heavy-tailed. **May not reach useful
sigma at all**"* — and a driving range is a room full of phones, a public SSID and
a golfer's body between the handset and the access point. The quantity that decides
whether a Candidate is arbitrated is the sigma **at the moment of the shot**, not
the sigma after five quiet minutes.

⚠ **So the accuracy case is not spent; it was measured in the wrong conditions.**
M1 must run a contended arm as well as an idle one (§11), and what the cable is
really selling there is not a better floor but a **floor that does not move**.

### 1.1 And clock alignment was never the whole of it

Three further reasons, none of which this document's opening argument covered and
all of which survive Phase 0 untouched:

| | Why the cable wins |
|---|---|
| **Contention** | Wired latency does not degrade because the room filled up. WiFi's does, and unpredictably — see above |
| **Reliability** | A cable does not drop, roam, hit client isolation, or lose multicast. `RV` 3.6a exists because *"it will not work at a range"*; the cable is the path with no such clause |
| **Power** | A phone on a cable is charging. A long range session on 5 GHz with the camera running is a battery problem, and the cable removes it |
| **Cross-platform reconnection** | `ppcp_discovery.cpp` is `#if defined(__APPLE__)` throughout, so Windows and Linux have no reconnection path at all today |

⚠ **The power point cuts both ways and both are real.** §9.6 records charging as a
*thermal* cost — sustained encode plus charging is a double load and iOS throttles
quietly. It is also the thing that lets a session outlast a battery. Neither
cancels the other; M10 is what says which dominates in a session of realistic
length.

`CORE` §3.2's own transport table says the same thing qualitatively — *"USB tunnel
| very tight, stable | Fastest convergence, lowest sigma. **Best available.**"* —
and `RV` §5.4 adds *"a wired tunnel is the best available option where the peers
are co-located"*. This document is that sentence turned into code.

**Second reason, and it is nearly as strong.** `src/Ppcp/ppcp_discovery.cpp` guards
its browser and its advertiser with `#if defined(__APPLE__)` and
`makePlatformBrowser()` returns null everywhere else. **On Windows and Linux this
host has no reconnection path at all today** — a persisted pairing is held and
never used, and the operator scans a QR code every session. usbmux device
enumeration is presence detection that works identically on all three platforms
and needs no multicast, no responder and no router that forwards mDNS. Wired is
the first cross-platform reconnection path we would have.

**What it is not.** It is not reliably a bandwidth win. Mark's test device is an
iPhone 16 (`iPhone17,3`), which is USB-C at **USB 2 speeds** — 480 Mbps raw, and
realistically 30-40 MB/s through usbmux. A good 5 GHz link to the same phone can
beat that. Only the Pro models (USB 3, 10 Gbps) are an unambiguous bulk win. §7
takes this seriously rather than assuming it away.

---

## 2. What already exists, in all three repos

Read this before the design; most of it is already built and the new work is
smaller than it looks.

### `libppcp` — nothing to do, by construction

`include/ppcp/peer.h:10` — *"It owns no socket, no thread, no timer and no clock."*
The library is sans-I/O and byte-oriented. A wired transport is invisible to it.
`ENC` 2.1b is explicit that a link is bound by `link_id` and **"MUST NOT infer
either from arrival order, from the transport address, or from a rendezvous
identity"** — so a link whose streams arrive over different transports is legal by
construction, which §8 makes use of.

One config field matters: `ppcp_peer_config.listener` (`peer.h:163`) already
selects dialler or listener, and `ppcp_peer_hello()` already exists. The library
supports the direction reversal we need; the embeddings hardcode it.

### `PinPointStudio` — the transport is nearly ready

- `src/Ppcp/ppcp_transport.{h,cpp}` (2640 lines) — OpenSSL directly, external
  TLS 1.3 PSK, two or three TCP connections per link, `link_bind`, a `Connector`
  and a `Listener`. Deliberately Qt-free so the conformance harness can drive it
  headless.
- ⛔ **`Connector` is heavily tested and has never run in the product. The wired
  path would be PinPointStudio's first dial.** Every reference to
  `Connector::connect` outside `ppcp_transport.cpp` is in `src/Ppcp/tests`
  (~20 call sites across four suites, including one that dials the real host
  service). The discovery path *looks* like a caller and is not:
  `startDiscovery()` computes `Ppcp::decideDial(...)` at
  `ppcp_host_service.cpp:1073` and then uses the answer only to mark the phone
  present — `m_seenInstances.insert(inst, pid); emit phonesChanged();` — because
  under 3.5e the host advertises and **the phone** dials. This is the same shape
  as `PpcpListener` on the device: built, tested, unreached. Plan for it.
- The single seam: `dialTcp()` at `ppcp_transport.cpp:1148-1176` is the *only*
  place a socket is created for a dial. It takes `cfg.host`/`cfg.port` and returns
  a connected non-blocking fd. Everything downstream — TLS, `link_bind`,
  concurrency across channels — is address-agnostic.
- `src/Ppcp/ppcp_host_service.{h,cpp}` — owns one accept thread and a GUI-thread
  `QTimer` pump. `ppcp_host_peer.cpp:487` hardcodes `cfg.listener = true`.
- ⛔ **`adoptLink()` identifies a phone by `link->pairingId()`
  (`ppcp_host_service.cpp:623`), and that value is empty on the dialling side.**
  `ppcp_transport.h` says so in terms — *"Empty on the dialling side, which never
  resolves anything"* — because it is filled by the **listener's** identity
  resolver. On a wired dial PinPointStudio is the client, so `pairing` comes back
  empty and three things silently do not happen: `m_rv.noteLinkEstablished()`,
  the `m_pairedThisRun` insert that is *"the only place that fact exists"* for a
  device row (`ppcp_host_service.h:661`), and a correct `refreshAdvertisement()`.
  **The phone would connect and never appear in the UI.** ⚠ This is the same
  failure class as the already-fixed empty-Phones-list bug, where TLS resumption
  skipped identity resolution and links carried no `pairingId`. §6.3 is the fix.
- ✅ The accept thread's hand-off — `QMetaObject::invokeMethod(this, ...,
  Qt::QueuedConnection)` at `ppcp_host_service.cpp:291-295`, with the rule that
  *"the channel crosses threads, the link never does"* — is directly reusable by
  a dialling thread. Nothing about link intake is accept-specific except the
  `pairingId` above.
- `src/Ppcp/ppcp_discovery.cpp` — macOS-only, as above.

### `PinPointCapture` — the listener already exists and is tested

- `Sources/Platform/Network/PpcpTransport.swift:770` — `actor PpcpListener`, a
  real `NWListener` with TLS-PSK, `link_bind` binding through the library's
  `PpcpLinkBinder`, a 5 s bind timeout, N connections on one port. Built, tested
  in `Tests/TransportLoopbackTests`, and **currently unused in the product**
  because `RV` 3.5e made the host advertise instead.
- `Packages/Core/Sources/CaptureCore/Ppcp/DevicePeer.swift:445` already takes
  `listener: Bool = false` as a parameter. The device side is parameterised
  already; the host side is not.
- The finding that shapes everything below, quoted from `PpcpTransport.swift:740`:

  > *The only server-side entry point is `add_pre_shared_key`, which registers a
  > (key, identity) pair up front … This listener can only accept the identity it
  > registered. A rotating identity — which is every conformant client — cannot
  > reach it.*

---

## 3. The one hard constraint, and everything that falls out of it

**usbmux is host→device only. There is no reverse tunnel.** Apple ships no
equivalent of `adb reverse`; a host process opens a connection *to* a port the
device is listening on, and a device process cannot open a connection *to* the
host. This is not a limitation of any library — it is the shape of the USB
multiplexer on all three platforms.

Three consequences, in order:

1. **The device listens; the host dials.** Forced. Not a preference.
2. **The host is the TLS client.** `RV` 5.2g — *"the peer that dialled is the TLS
   client"* — is satisfied naturally rather than bent. Good.
3. **The device's TLS server cannot resolve a PSK identity.** This is `RV` 3.5d
   and PPC finding D1. `Network.framework`'s listener matches exactly the one
   identity it registered at `NWParameters` construction, and
   `sec_protocol_options_set_pre_shared_key_selection_block` is client-side only.
   A host that draws a fresh CSPRNG `rn2` per `RV` 5.3a produces an identity the
   device's listener has never heard of and gets alert 115.

Point 3 is the whole design problem. §5 solves it.

Also forced: **the wired path inverts version negotiation.** `RV` 2d — the dialler
is the initiator and sends `hello`; the listener sends `hello_accept` and states
`min_version`. On wired, the *host* sends `hello`. Anything in either codebase
that assumed host-as-responder is now wrong; `ppcp_host_peer.cpp:487` is the known
instance.

**Rejected alternatives, briefly.**

| Alternative | Why not |
|---|---|
| USB Ethernet / NCM interface | Only routes with Personal Hotspot enabled, which needs a cellular plan and a user action every session. No interface appeared on this Mac with a phone attached over USB and hotspot off. Not dependable enough to build a timing story on. |
| ExternalAccessory framework | MFi programme only. Not available to us. |
| Private `MobileDevice.framework` on macOS | Not portable to Windows or Linux, and fragile across OS releases. usbmux's wire protocol is more stable than the framework that speaks it. |
| Keep the device dialling, over some tunnel | Does not exist. See above. |

---

## 4. The tunnel: a first-party usbmux client

### 4.1 Do not take a third-party dependency

`libusbmuxd` + `libplist` (LGPL-2.1) would work, but the protocol we actually need
is three messages and a 16-byte header, and hand-rolling it buys:

- **One code path on three platforms.** The only per-platform difference is the
  socket: `AF_UNIX` to `/var/run/usbmuxd` on macOS and Linux, `AF_INET` to
  `127.0.0.1:27015` on Windows. Everything above that byte is identical.
- **No new build-system or licence surface** on three platforms, and no `libplist`.
- **Qt-free**, matching `ppcp_transport.cpp`'s existing discipline, so the headless
  conformance harness can still drive the wired path.

Estimated ~600 lines including a minimal XML-plist emitter and a scanning reader
for the dozen keys we read. That is smaller than the vendoring would be.

### 4.2 The protocol we implement

Header, little-endian, `length` inclusive of the header:

```
struct { uint32 length; uint32 version /* = 1 */; uint32 message /* = 8, plist */; uint32 tag; }
```

followed by an XML plist. Three request types:

| Request | Keys | Reply |
|---|---|---|
| `ListDevices` | — | `DeviceList: [ { DeviceID, Properties: { SerialNumber, ConnectionType, ConnectionSpeed, ProductID, LocationID } } ]` |
| `Listen` | — | `Result { Number: 0 }`, then asynchronous `Attached` / `Detached` on the same socket, forever |
| `Connect` | `DeviceID`, `PortNumber` | `Result { Number: 0 }`, after which **the socket becomes the raw tunnel** |

✅ **All of the above was verified live against `/var/run/usbmuxd` on the build
Mac on 29 August 2026, with a phone attached**, rather than recalled. The probe
script is reproduced in §10. Observed: reply header `version=1, message=8`, the
request tag echoed, and

```
DeviceID=306  ConnectionType=USB  SerialNumber=00008140-…  ConnectionSpeed=480000000
```

Three details that will cost a day each if they are not written down now:

- ✅ **`PortNumber` is byte-swapped, and this is now measured, not remembered.**
  It goes on the wire big-endian (`htons`), unlike every other integer in the
  plist. Connecting to lockdown's port 62078 on the attached device:

  | `PortNumber` sent | Result |
  |---|---|
  | `htons(62078)` | `Result Number=0` — connected |
  | `62078` native | `Result Number=3` — refused (it dialled 32498) |

  ⚠ Note the failure mode: the wrong byte order does not error, it **connects to
  a different port**, which on a busy device could succeed against something
  else entirely.
- ⚠ **Filter on `ConnectionType == "USB"`.** usbmuxd also reports WiFi-paired
  devices with `ConnectionType == "Network"`. Treating one of those as wired would
  label a WiFi link as wired and silently invalidate every timing claim this
  design exists to make. This is an accuracy requirement, not tidiness.
- ⚠ **`Listen` consumes its socket**, and so does `Connect`: after `Number=0` the
  socket *is* the tunnel and carries no more plist. Device presence therefore
  needs its own long-lived connection to usbmuxd, separate from every dial.
  Result numbers: 0 ok, 2 bad device, **3 connection refused (verified)**,
  5 bad version.

### 4.3 Per-platform availability

| Platform | Provider | Present by default? | Notes |
|---|---|---|---|
| macOS | Apple's built-in `usbmuxd` | **Yes.** Verified: `srw-rw-rw- /var/run/usbmuxd` on the build Mac. | Nothing to install, nothing to ship. |
| Windows | Apple Mobile Device Service (`AppleMobileDeviceService.exe`), TCP `127.0.0.1:27015` | **No.** Ships with iTunes or the Microsoft Store "Apple Devices" app. | We cannot redistribute it. Detect its absence and say so plainly in Settings→Phones. Loopback needs no firewall rule (unrelated to the listener rule already tracked for the installer). |
| Linux | open-source `usbmuxd` daemon, `/var/run/usbmuxd` | **No.** `apt install usbmuxd` / equivalent, plus udev rules. | Socket permissions may need a group; document the one-liner. |

⚠ **To confirm on first bring-up, not to assume:** whether a `Connect` to an
ordinary application port requires the device to have trusted the computer. Our
belief is that "Trust This Computer" gates *lockdown* services (port 62078) and
not raw usbmux connects, but this has not been measured here and the answer
changes the onboarding copy.

### 4.4 The seam in `PinPointStudio`

`ConnectorConfig` gains an optional dialler:

```cpp
// Supplies a connected, non-blocking socket. Null = dialTcp() with host/port,
// which is every caller today. A wired dial returns the usbmux tunnel fd.
using DialFn = std::function<pp_socket_t(double deadline)>;
DialFn dial;
```

`dialTcp()` becomes the default and `Connector::connect()` calls `cfg.dial` when
set. That is the entire change to the 2114-line transport. ⚠ `applyOptions()`
must skip `TCP_NODELAY`/`SO_SNDBUF` on an `AF_UNIX` fd — `setsockopt(IPPROTO_TCP)`
returns `ENOPROTOOPT` there — and the failure must stay non-fatal.

---

## 5. Direction, identity, and the presence record

### 5.1 The problem restated

The device is the TLS server and can register exactly one PSK identity. The host
is the TLS client and must offer *that* identity, byte for byte. Neither end can
guess the other's `rn2`.

### 5.2 The answer: move the resolution to the client

**The device publishes the identity it registered; the host verifies it before
offering it.**

`RV` 5.3b says *"a server resolves an offered identity by recomputing `tag` with
the `K_id` of each pairing it holds and selecting the match."* Nothing about that
computation requires it to happen inside the TLS handshake, or on the server. The
host holds the same `K_id` for the same pairing. So the host recomputes
`tag = HMAC-SHA256(K_id, "ppcp1 psk-id" || rn2)` over each pairing it holds, and
**refuses to dial** unless one matches — which is 5.3b, run early, by the peer
whose TLS stack can actually do it.

The security property is unchanged and worth stating explicitly: only a peer
holding `K_id` can *produce* a resolvable identity, and only a peer holding `K_id`
can *verify* one. An identity is public data — it crosses the wire in cleartext in
every `ClientHello` — so publishing it before the handshake discloses nothing the
handshake would not.

### 5.3 The presence record

The device opens **one plaintext listener on a fixed port, bound to `127.0.0.1`
and nothing else** — the only constant in this design — serving a small CBOR
record and closing:

```
WiredPresence {
  pv:     "1.0"            // RV 3.3a, so a host filters on MAJOR before dialling
  role:   "capture"
  dl?:    "Mark's iPhone"  // RV 4.4d — UNTRUSTED. sanitiseLabel() before display.
  peers: [ { port: uint,               // the PPCP listener's actual port
             psk_identity: bytes(17) } ]  // RV 5.3a, exactly as registered
}
```

✅ **There is deliberately no `rid` here, and that is a simplification rather than
an omission.** `PpcpRendezvous::identityResolver()` (`ppcp_rendezvous.h:219`)
already recomputes the tag with the `K_id` of every pairing the host holds and
hands back **both** `kTls` and `pairingId` — which is the entire result the dial
needs. A `rid` would be a second resolution path (3.4b, 3.2a) computing the same
answer from a different nonce, and would oblige `PinPointCapture` to mint `rid`s
for a path that has no instance name to derive from one. The 17 octets the host
must verify anyway are sufficient to identify the pairing.

⚠ Reuse `identityResolver()` **as it stands**, including its no-early-exit
constant-time comparison and its policy-after-resolve ordering. The 5.3d timing
argument does not apply on this side — nobody is probing us — but a second,
faster resolver would be a second place for expiry (7.3e), exhaustion (7.3a) and
invalidation (7.3b) to be got wrong.

**This is an `RV` §3 advertisement delivered over a cable instead of over
multicast**, and that framing is the point: the same TXT semantics, the same
`pvAcceptsMajor()`, the same `decideDial()`, the same 3.4c rule that a host never
dials an `rid` it cannot resolve. `RV` §2's *direct* path is defined as *"out of
band — a tunnel, a cached endpoint, a socket handed in by an embedding
application"*, and this is the out-of-band part of it.

Because the record carries the **actual** port, no port derivation scheme is
needed. The device binds `0` for its PPCP listener and reports what it got. Only
the presence port is fixed, and a collision there is survivable: the host gets a
record that fails to parse and treats the device as not wired.

⛔ **Both wired listeners bind loopback only** — `NWParameters.requiredLocalEndpoint`
set to `127.0.0.1`, not the all-interfaces bind that `PpcpListener` does today
(`PpcpTransport.swift:852-860`). Two reasons and both are load-bearing:

1. **Otherwise the presence record is readable by anyone on the WiFi network.** It
   is plaintext and it names every pairing the device holds. The trade recorded in
   §5.4 below is only defensible because the reader has to be on the cable or on
   the device; an all-interfaces bind would quietly convert it into a LAN
   broadcast of the pairing set. This was a hole in the first draft of this
   document.
2. **It keeps the wired path clear of the iOS local-network permission.** Loopback
   is exempt; a listener reachable from the LAN is the case that is not. A wired
   connection that works before the user has answered a permission prompt — or
   after they have declined it — is worth having on its own merits. ⚠ Verify the
   exemption rather than trusting it (M7).

### 5.4 Does the presence port break `RV` §7.7?

`RV` 7.7a forbids PPCP messages before the handshake — this carries none.
7.7b forbids disclosing *Sources, profiles, calibration or stored sessions* — none
of those appear. `rid` is designed for a cleartext multicast TXT record; the PSK
identity is designed for a cleartext `ClientHello`; a port is not a secret.

⛔ **One thing is disclosed that the WiFi path does not disclose, and it is
recorded rather than waved away: the record names every held pairing, so a reader
learns how many the device holds.** `RV` 3.4d1 has the advertiser name one pairing
at a time precisely to keep that count unobservable, and 3.4e's property is
cross-venue unlinkability against a passive radio observer. **The loopback bind is
what makes that observer non-existent** rather than merely unlikely: the reader is
either a process on the device — which has strictly better attacks available to it
— or a process on the host at the other end of a physical cable. On an
all-interfaces bind this trade would not hold and the design would be wrong. **Accepted trade, with the alternative named:** rotating one entry
per read would preserve 3.4d1 at the cost of the host reading repeatedly with no
way to force rotation, for a property the medium already provides.

⚠ A hostile process on the device could bind the presence port first and serve a
bogus record. The consequence is a failed TLS handshake — it holds no `K_tls` —
so this is denial of service and nothing more. Same for a hostile process on the
host reading the record through usbmuxd. Note it in the security section; do not
build a mitigation for it.

### 5.5 The one spec ask, and it is smaller than it looks

`RV` 5.3a requires `rn2` to be *"8 random bytes from a CSPRNG, **fresh per
connection**"*. On this path the device draws `rn2` once per **listener session**,
and the host offers the same identity on each of the link's two or three channels.

⚠ **The shipping implementation already does this.** `ConnectorConfig` carries a
single `identity` for a whole `Connector::connect()` call, and `connect()` dials
every channel of the link with it. "Fresh per connection" is already "fresh per
link" in both applications today. The wired deviation is from *per link* to *per
listener session*, and the device restarts its wired listener on foreground entry
and on any change to the pairing set.

The clause's own stated rationale — *"it is sent in the clear in the `ClientHello`,
so anything stable in it is a tracking beacon"* — is satisfied: the value is not
stable, and on a cable there is no passive observer to beacon at.

**Proposed erratum (`PPCP-RV` 5.3a):** on a *direct* path where the responder is
the TLS server and its platform provides no server-side identity resolver (3.5d),
`rn2` MAY be drawn once per listener session rather than once per connection,
provided the responder publishes the resulting identity out of band and the
initiator verifies it under 5.3b before offering it.

This is the shape of E3 and E56 — a clause read correctly and literally, making a
path unreachable, found on implementation. **It is also not a blocker for a
prototype:** everything in §6 can be built and measured first, and the erratum
raised with numbers attached.

If the erratum is refused, the escape hatch is to link a full TLS stack
(`swift-nio-ssl`/BoringSSL) into `PinPointCapture` for the direct path, giving it a
real 5.3b resolver — which would also close finding D1 outright and unlock the
device-side advertising 3.5b originally wanted.

⚠ **But it must not be done for this document's sake.** A second TLS stack to
review, ship and maintain inside an App Store binary is disproportionate to a
transport optimisation (§0). It is justified only if it is being done for `RV`
3.5b anyway, on that work's own budget. If the erratum is refused and 3.5b is not
being funded, **the correct outcome is that wired does not ship** — not that the
iOS app grows a TLS stack to make it fit.

---

## 6. The design, end to end

```
PinPointStudio                                     PinPointCapture
──────────────                                     ───────────────
WiredDeviceWatch                                   WiredPresenceListener
  usbmux Listen ──► Attached { udid, USB }            fixed port, plaintext
        │                                                    │
        ▼                                                    │
  usbmux Connect(udid, PRESENCE_PORT) ───────────────────────┘
        │  ◄──── WiredPresence { pv, role, dl?, peers[] }
        ▼
  for each peers[i]:  identityResolver()(psk_identity)   ── RV 5.3b, client-side
                        -> { kTls, pairingId }  or  no match
        │  first match wins; no match = not our phone, stay silent
        ▼
  Connector::connect{ dial = usbmuxDial(udid, port),      PpcpListener
                      kTls, identity = peers[i].identity, ── NWListener,
                      channels = {Control, Bulk} }           one registered PSK
        │  ×2 tunnels, concurrent TLS handshakes, TLS client
        ▼
  link_bind{ link_id, channel } first frame on each stream  ── ENC 2.1a
        ▼
  ppcp_peer_config.listener = FALSE;  ppcp_peer_hello()     ── RV 2d inverted
        ▼
  ... ordinary PPCP: hello_accept, session_open, sync_probe bursts, shots
```

Nothing above `link_bind` changes. The engine, the live session, the arbitration
bridge, the preview consumers and the transfer path are all untouched.

### 6.1 Choosing between WiFi and USB when both are available

**This is not a choice between two connections. There is no moment at which one
component holds two links and picks one**, because the two paths dial in opposite
directions and fire independently:

| | WiFi | USB |
|---|---|---|
| Who dials | the **phone** (`RV` 3.5e — the host advertises, the phone browses) | the **host** (usbmux is host→device only, §3) |
| What triggers it | the host's mDNS advertisement | a usbmux `Attached` event |
| Who can see both signals | — | **the host** |

So a phone plugged in while the host is advertising produces **two dials at once**,
and both succeed: same phone, same pairing, two links. ⛔ And nothing collapses
them today — `adoptLink()` deliberately refuses to deduplicate by pairing
(`ppcp_host_service.cpp:818`, and it is right to: with `mu > 1` several devices
share one pairing). The result would be **two `Phone` rows, two sets of preview
consumers, and one phone's Candidates entering the arbiter twice.**

#### The decision is the host's, and the phone cannot help

⚠ **iOS gives an app no way to tell a data-capable host from a dumb charger.**
`UIDevice.batteryState == .charging` is true for a wall plug, and there is no
public API behind it. The phone genuinely does not know it is on a cable to a
Studio. `ReconnectCoordinator.swift` says the other half out loud —
*"PinPointStudio advertises and this device dials. **Always.**"*

That asymmetry is also the lever: **the host owns the only trigger the WiFi path
has.** Under 3.5e the phone dials because the host advertised; a pairing the host
stops advertising is a pairing the phone has nothing to dial.

#### So arbitration is advertisement suppression, not connection selection

The advertiser already rotates one pairing at a time (3.4d1), so withholding one
is a change of set, not a new mechanism:

1. **A live link for that peer already exists → do nothing.** Never open a second,
   on either transport. This subsumes "never switch mid-session".
2. **No live link, and wired is *proven* available → suppress that pairing's
   advertisement, then dial wired.** Proven means *the presence record was read and
   its `rid` resolved* — not merely that a device attached.
3. **The wired path ends → restore the advertisement immediately.** Fail open, and
   ⛔ *"ends"* means **any** ending: the dial failed, the link dropped, the phone
   was unplugged, the app backgrounded. A suppressed advertisement with no wired
   link is a phone that can reach the host by no path at all, which is strictly
   worse than WiFi — and for a pairing *born* on the cable (§6.5) it would mean
   the pairing never works over WiFi at all.
4. **Presence unreadable → change nothing.** The app is backgrounded, the cable is
   charge-only, or USB Restricted Mode has closed the port (§9.5). ⛔ **Plugging a
   phone in to charge must never disturb WiFi**, and keying on "device attached"
   rather than "presence proven" is exactly how it would.

⚠ Rule 2's ordering matters and is easy to get backwards: suppress *before*
dialling, not after the link is up. The wired dial is a few milliseconds over the
cable; the phone's browse-and-dial is not, so suppressing first closes almost the
whole window.

#### The backstop, because suppression is not airtight

The phone can still dial from a pairing code the operator scans, or from an
endpoint carried in one (`RV` 4.3d). So there is a second, defensive collapse:

> **On `onDeclare()`, if the peer's `counterpartId` matches a live `Phone`, close
> the newcomer and keep the incumbent.**

- ⛔ **Keyed on `counterpartId`, never on `pairingId`.** `Peer.id` is the phone
  (`CORE` 5.1a, stable for the entity's lifetime); a pairing is not a phone. That
  is the same distinction `ppcp_host_service.cpp:818` already insists on, and
  `peerForId()` (`:453`) is the lookup, already written.
- ⚠ **It cannot be done at `adoptLink()`.** `counterpartId` is only set at
  `onDeclare()` (`:879`), from `desc->id` — after both TLS handshakes and the
  `hello` exchange. Two wasted handshakes is the unavoidable price of a backstop,
  and it is small.
- **Keep the incumbent, always**, whichever transport it is on. It is the link
  with the sync history, and a rule with no comparison in it cannot oscillate.

#### Plugging in mid-session does nothing — so give the operator a button

Rules 1 and the backstop mean a cable inserted into a running WiFi session changes
nothing. ⚠ **That will read as broken to someone who plugged the cable in
*because* WiFi was bad**, which is precisely when they will do it.

Automatic switching is still wrong: `ppcp_host_service.cpp:648` builds a fresh
`Phone`, `ppcp_peer` and sync estimator per link, so a switch discards the whole
fit and re-converges from nothing (§1) — a silent downgrade for a promised upgrade.
The resolution is to make it explicit rather than automatic:

> **"Use cable" in Settings→Phones**, offered only when a wired path is available
> and unused, which drops and re-dials on request and says what it costs
> ("re-syncs, about a minute").

- **Surface which path a phone is on** next to the battery and thermal pills
  already there, and in the sync trace. An operator who cannot see that the cable
  did nothing cannot act on it. `RV` 5.4k already puts the negotiated TLS mode in
  the diagnostic export; the transport belongs beside it.
- **Fall back either way on a drop**, since the alternative there is no link.

#### Two scoping consequences worth stating

- **The cable carries a pairing; it never establishes one.** The QR scan stays the
  only root of trust, and a phone that has scanned nothing publishes nothing. But
  a first connection *can* run over the cable — see §6.5, which is where that
  turned out to be nearly free. **No new pairing ceremony is in scope.**
- ⚠ **If Phase 3 ships, this section inverts.** Split-transport (§8) makes "both at
  once" the goal rather than the conflict: one link, control on the cable, bulk on
  the radio. Do not build arbitration that Phase 3 would have to tear out — the
  suppression rule is per *link*, and a split link is still one link.

### 6.2 What the operator sees

A phone plugged in with a held pairing simply appears and connects — no QR code,
no network, no multicast. **On Windows and Linux that is the first time a
persisted pairing has ever been usable.** Absence is silent, exactly as `RV` 3.6a
requires of discovery: an unplugged phone, a missing Apple Mobile Device Service,
a Linux box with no `usbmuxd` — none of them is an error state, and none of them
gets an error banner. A single line in the app log (`ppWarn()`, per the one-log
rule) says why wired is unavailable when someone goes looking.

⚠ **Silent in the UI and specific in the log are not in tension — they are the
requirement.** "Not connected" as the only diagnosis is what makes a range
unfixable, and the usbmux client can tell these apart cheaply:

| Observation | Diagnosis |
|---|---|
| Cannot open `/var/run/usbmuxd` (or `127.0.0.1:27015`) | No usbmux provider — Apple Devices not installed (Windows), `usbmuxd` not running (Linux) |
| `ListDevices` empty | Nothing plugged in, **or** a charge-only cable with no data pairs |
| Device present, `ConnectionType == "Network"` | WiFi-paired, not wired. ⛔ Never treat as wired (§4.2) |
| USB device, `Connect` to the presence port → `Number=3` | App not running or not in the foreground — ⛔ **or trust not granted; see below** |
| Presence read, no entry resolves | A phone this host is not paired with — stay silent (3.4c) |
| Device vanishes from `ListDevices` while attached | USB Restricted Mode, most likely (§9.5) — confirm with M8 |
| ~~`Connect` refused at the mux layer~~ | ~~Trust not granted — pending M5~~ ⛔ **This row is wrong. See below.** |

That table is also the acceptance criterion for the diagnostics work: each row is
distinguishable, or the row is wrong.

⛔ **And by that criterion one row IS wrong — found in implementation, 29 Aug
2026.** "`Connect` to the presence port → `Number=3`" and "`Connect` refused at
the mux layer" are **the same wire event**. usbmux answers `Number=3` and there
is nothing else to read: the table asked for seven distinguishable causes and
the transport can see six.

Measured on the build Mac against Apple's daemon, phone attached and **trusted**:

| `Connect` to | Result |
|---|---|
| 62078 (lockdown, listening) | `Number=0` |
| **50915 (the presence port, nothing serving it yet)** | **`Number=3`** |
| 1 (nothing ever listens) | `Number=3` |

So a closed port and a refused-at-the-mux-layer dial are one observation. The
host reports it as one diagnosis naming both possibilities. ⚠ **M5 is what
decides whether the row splits again** — it needs an *untrusted* device, and
every probe run so far has used Mark's dev phone, which is trusted. Until M5,
"trust not granted" is not a diagnosis this host can honestly print.

✅ One incidental result worth keeping: **port 50915 was free on the device**,
so §5.3's fixed presence port collides with nothing on a stock iPhone 16.

### 6.3 Two host-side integration hazards, found by tracing rather than reading

Both are cheap to handle now and expensive to discover during implementation.

✅ **The device *watch* needs no thread at all — only the dial does.** usbmux's
`Listen` is a long-lived readable fd, which is exactly the shape the DNS-SD
browser already has: a `QSocketNotifier` on the GUI thread and a read when it
fires (`ppcp_host_service.cpp:1109`). Use that precedent, including its recorded
cancellation trap — *"a `QSocketNotifier` left on a closed socket"* (`:1205`) —
so the teardown order is drop the notifier, then close the fd. That removes any
blocking-read integration problem before it exists.

**The dial must not run on the accept thread.** `ppcp_host_service.cpp:242-296` is
a tight loop: an `acceptChannelFor()` poll per link still short of its third
channel (`ENC` 2.1d), then a 250 ms `accept()`. A wired dial is a usbmux `Connect`
plus two concurrent TLS handshakes and can block for hundreds of milliseconds —
long enough to starve WiFi accepts and stall preview-channel collection. ⛔ **A
second thread**, using the identical `Qt::QueuedConnection` hand-off, and the same
rule that only self-contained objects cross.

**`adoptLink()` must be told the pairing rather than reading it off the link.**
Per §2, `link->pairingId()` is empty whenever this host dialled. The host already
knows the answer — it resolved the pairing from the presence record under `RV`
5.3b *before* it dialled (§5.2), which is the whole point of moving the resolution
client-side. So:

```cpp
// Empty = "read it off the link", which is every caller today.  A dialling
// caller resolved the pairing before it dialled and passes it here: RV 5.3b
// ran client-side, and pairingId() has nothing to say on this side.
void adoptLink(std::unique_ptr<Ppcp::PeerConnection> link,
               const QString &resolvedPairingId = {});
```

⚠ Downstream of that, check `noteLinkEstablished()` before calling it on a wired
link: `RV` 7.3a's single-use accounting is about **spending a pairing code**, and a
wired reconnection spends nothing. The persisted-pairing path is what wired uses,
and it must not decrement anything.

### 6.4 What a QR pairing leaves behind, and what USB picks up

The QR path is the only way a pairing is ever created (§6.1: wired is a
reconnection path, not a pairing path). This is what it deposits, and how the
cable finds it again with no user action.

**What the QR ceremony leaves on disk — automatically, on both ends.**

| | Where | Default |
|---|---|---|
| Host | `PRK` in the application's own settings, keyed by `pairingId` | **Persisted.** `adoptLink()` → `noteLinkEstablished()` remembers a `mu: 1` code on the spot (`ppcp_host_service.cpp:626-632`, erratum E57) |
| Device | `PRK` via `PairingSecretStore.save` | **Persisted.** Opt-in was declined deliberately — *"the consent gate defaulted shut … the phone reached the end of a working handshake holding nothing"* (E57, 25 Aug) |

⚠ `PRK` now lives in the app's own settings on **every** platform (E56, keychain
gone), which is what makes the rest of this work on Windows and Linux — where it
is the *only* reconnection path they have (§1).

**The join, when the cable goes in.** The `pairingId` is the hinge, and the host
recovers it from the identity alone:

```
usbmux Attached(udid) → read presence → identityResolver()(psk_identity)
                                             ↓
                                    { kTls, pairingId }      ← the SAME pairingId
                                                                the QR pairing made
```

The device's `psk_identity` is `0x01 || rn2 || HMAC(K_id, …)`, and `K_id` descends
from the `PRK` the QR ceremony wrote (`RV` §5.1). So the host resolves it against
its own ledger exactly as it would a WiFi handshake — same function, same policy
checks, different moment. Nothing new is stored, and nothing is looked up by
address or by `udid`.

**What is reused, and why it needs no work.** Every persistent key in this
application is either a **pairing** or a **peer**, and neither is transport-bound:

| Carried over | Keyed on | Consequence |
|---|---|---|
| Phone alias | `pairingId` — `phoneAliasFor(const QString &pairingId)` (`:1280`) | The name the operator typed reappears on the cable |
| Capture/import ledger, transfer dedup | `peerId` — *"the MINTING peer, not whoever handed us the file"* (`ppcp_import_ledger.h:72`) | A capture half-transferred over WiFi resumes and dedups over USB |
| Pairing itself, revocation, "Forget" | `pairingId` | One list, both transports |
| Camera Sources | re-declared per link (`MSG` 3.3 — *"a peer's cameras exist the moment it declares and at no other moment"*) | Fresh by design, not lost |

✅ **The phone's identity is an app-minted, persisted UUID** — `PeerIdentity.current`
(`RecordingSession.swift:52-65`), *"minted on first use and stable thereafter"*,
held in `UserDefaults`. Not a hardware serial (unavailable on iOS) and not
`identifierForVendor`. ⚠ A reinstall mints a new one — but a reinstall also
destroys `PairingSecretStore`, so the phone must re-pair and arrives as a genuinely
new peer. Consistent, and no special case is needed.

⚠ **Two Studio instances on one machine** would each watch usbmux and each resolve
its own pairing — and the *device* settles it, not us: `CORE` **I20 admits at most
one host**, so the second `hello` carrying `role: host` is refused. Nothing to
build; worth knowing before someone reports it as a wired bug.

✅ **Replay (host → phone) needs nothing extra.** The tunnel constrains only *who
dials*; once up it is an ordinary bidirectional stream. `Annotation` is already the
one content type that flows both ways (`CORE` 5.18d), carried by the **Markup**
profile both peers declare.

✅ **Nothing in the application keys on an endpoint, an address or a transport.**
That is the real reason the swap is cheap, and it is worth re-checking rather than
assuming if either store grows a new key.

**What does not carry over**, and should not:

- **The sync fit.** Fresh `ppcp_peer` and estimator per link (`:648`) — the whole
  of §1 and §6.1.
- **The live Session.** A new `session_open`; `Session.id` is per conversation.
- Anything the device chose not to persist.

**Edge cases, in the order they will actually happen.**

- ⛔ **A phone paired from a `mu > 1` code can never use the cable.**
  `PairingSecretStore.save` refuses to persist a group credential outright (7.4f —
  *"declining opt-in is not a licence to keep a group credential"*), so its
  presence record is empty and nothing dials. Latent rather than live: this host
  publishes only `mu: 1`. ⚠ But it is a real constraint on the multi-device
  workflow the spec allows, and it belongs in front of anyone who proposes
  publishing `mu > 1`.
- **Paired by QR while already plugged in.** The ceremony completes over WiFi and
  the session **stays** there (§6.1 rule 1 — a live link is never displaced). The
  device's listener refreshes on a pairing-set change, so the *next* connection is
  the wired one. No special case, and no reason to add one.
- **"Forget" on either end.** The entry leaves the presence record, nothing dials,
  and the path degrades to the QR code. Symmetric, and it needs no wired-specific
  revocation.
- **Never paired at all.** The record resolves nothing, the host stays silent
  (3.4c, one layer out), and a stranger's phone plugged into the machine is a
  no-op rather than a prompt.

### 6.5 A phone's *first* connection over the cable

⛔ **NOT REACHABLE FROM `PinPointCapture` AS THE APP IS WRITTEN — found 29 Aug 2026
on implementation, and this section was written assuming otherwise.** The presence
record is specified to list persisted pairings *plus any scanned, not-yet-connected
code*, but nothing on the device ever holds such a code:
`RendezvousCoordinator.scan()` derives the keys and dials **immediately**, so there
is no interval during which a scanned-but-unused pairing exists to be published.
The `HeldPairing` API added in Phase 1 supports it; the scan flow would have to
publish-then-wait, which changes a user-facing path. **Deferred out of Phase 1 —
re-scope this section or move it to Phase 2 before anyone relies on it.**


**What the user does: exactly what they do today.** The host displays a pairing
code, the operator scans it with the phone. ⛔ **The cable is not a credential.**
Physical possession authenticates nothing here — anyone who can reach the machine
can plug a phone into it — and inventing a cable-only trust root would mean an
unauthenticated key agreement plus something to bind it, which is `RV` §11 guided
pairing and an entire subsystem this document is not opening.

What changes is only **which wire carries the connection the scan authorises.**

#### It works with no new host machinery, and that is not a coincidence

`identityResolver()` recomputes the tag with the `K_id` of every pairing it holds
— *"**outstanding codes** and persisted pairings alike"* (`ppcp_rendezvous.h:208`).
A code that has been scanned but never connected is already in that set. So:

```
host publishes code ─► operator scans ─► both ends derive PRK (RV §5.1)
                                              │
        device publishes the identity on its presence port
                                              │
        host resolves it ─ identityResolver() ─► { kTls, pairingId }  ← the code
                                              │
                                     host dials over usbmux
```

✅ **And this is precisely why dropping `rid` from the presence record (§5.3)
mattered more than it looked.** `resolveRid()` carries an explicit
*"⚠ **ONLY PERSISTED PAIRINGS (7.4a)**"* (`ppcp_rendezvous.h:338`) — an outstanding
code has no `rid` to resolve. **An `rid`-based presence record could not have
carried a first pairing at all.** The simplification was also the enabler; that
was luck, and it is recorded so nobody reintroduces `rid` for tidiness.

#### The one device-side rule

> **The presence record lists persisted pairings *plus* any code scanned and not
> yet connected** — mirroring exactly what the host's resolver already accepts.

Symmetry is the rule to hold onto: the two sets must match, or a code resolves on
one end and is unpublished on the other. This is the only `PinPointCapture` change
that first-pairing-over-cable needs.

#### The race is already safe

The code keeps carrying its endpoints (4.3d), so the phone may also dial over WiFi
from the same scan. Both fire, one wins, and `mu: 1` settles it: the winner spends
the code through `noteLinkEstablished()`, and *"the resolver refuses a spent code
outright (7.3a, `refusedExhausted`)"* — `ppcp_host_service.cpp:637`. **One
ceremony, two possible transports, whichever connects first.** No new code format,
no new user choice, and nothing to decide in advance.

#### And this is a capability, not only an optimisation

⚠ A pairing code's endpoints are useless if the phone cannot reach the host's IP.
**At a venue with no usable WiFi — or a host with no network at all — pairing is
impossible today**, on all three platforms. Over the cable it needs no network.
That does not breach §0: wired is still never *required*, but it is the only path
in a case that currently has none.

### 6.6 Is a cable-born pairing reused over WiFi? Yes, both directions, automatically

A pairing established over the cable is an **ordinary pairing** — the same claim
`RV` 11.1a makes of guided pairing, *"indistinguishable from one established by a
scanned code, so §5, §7.4 and §7.5 apply verbatim"*. Nothing marks it as wired,
and nothing should.

| End | What happens on success | Result |
|---|---|---|
| Host | `noteLinkEstablished()` persists it, then `refreshAdvertisement()` (`ppcp_host_service.cpp:626-632`) | The pairing **enters the mDNS advertised set** (3.5e) |
| Device | `PairingSecretStore.save` persists it (E57, no consent gate) | `ReconnectCoordinator` will browse for and dial it |

So the next session with no cable simply works over WiFi, with no second ceremony
— and the reverse, a QR-over-WiFi pairing later used on a cable, is §6.4.

⛔ **The one way to break this is to leak the §6.1 suppression.** While wired is
live the host withholds that pairing's advertisement, which is intended; if the
suppression is not released when the cable goes away, a pairing born on the cable
is **never advertised** and the phone can never find the host over WiFi. That is
why §6.1 rule 3 is written as *any* ending rather than *dial failed*. ⚠ Worth an
explicit test: pair over cable, unplug, confirm the phone reconnects over WiFi.

#### The one genuinely wired-only user step

⚠ iOS's **"Trust This Computer"** prompt, if a raw usbmux `Connect` to an
application port turns out to need a lockdown pairing record (**M5, still open** —
§11). If it does: one-time, per host machine, at the OS level and not ours, and it
must be named in the onboarding copy rather than left to surprise someone at a
range. If it does not, first plug-in is silent.

---

---

## 7. Do the protocol primitives survive the change of transport?

**Yes — every one of them, unchanged.** PPCP is transport-agnostic by design and
the sync method was chosen for exactly this: `sync.h` says the admit fraction is
*"a fraction rather than a latency constant, because a USB tunnel and congested
2.4 GHz WiFi have nothing in common except that their left tails are the honest
part."* Nothing in the protocol needs a wired variant.

⛔ **But one thing did change, and it was not in the protocol: the dominant error
was this host's own I/O.** ✅ **Fixed and measured 29 August 2026** — §7.1 is kept
as the record of what was wrong and what it cost.

### 7.1 The host stamped `t4` on a 20 ms timer · ✅ FIXED 29 Aug 2026

**Result: `min_rtt` 12.95 → 1.64 ms, `offset_sigma` 2.50 → 1.16 ms, and what this
host publishes to the phone ≳ 6.5 → 0.86 ms.** One `QSocketNotifier` per channel
(`Phone::reads`, `watchChannels()`); `tick()` still runs on the 20 ms timer because
it is the time-passing half. The table below records the state that was measured.


`CORE` 6.1c anticipates precisely this and libppcp offers both paths
(`peer.h:630`): the automatic one, where `t4` is read *"as [the reply] is
handled"*, and `ppcp_peer_sync_observe()`, for *"an embedding that can stamp
closer to the socket … the convenient one; this is the accurate one."*

| | Accurate path bound? | How bytes are read |
|---|---|---|
| `PinPointCapture` | ✅ `sync_observe`, `sync_observe_to`, `sync_reply_stamps`, `zero_residence` — all four (`DevicePeerLive.swift:140-197`) | Network.framework callbacks |
| `PinPointStudio` | ⛔ **None.** Only `ppcp_peer_sync_pump()` (`ppcp_live_session.cpp:188`) | ⛔ `m_timer.setInterval(20)` (`ppcp_host_service.cpp:127`). **`QSocketNotifier` is used only for the DNS-SD browser and advertiser fds — never for a PPCP channel** |

So the host — which is the prober — stamps `t4` when a 20 ms tick gets round to
the reply, not when the bytes land.

⛔ **This document predicted the poll cost ~0.6 ms. Measured, it cost 11.3 ms.**
The prediction reasoned that min-RTT filtering would rescue it: a delay uniform on
[0, 20 ms], minimum over a 32-sample window, is `20/33 ≈ 0.6 ms`.

**The delay is not random — it is a phase.** The probe is queued and written on a
tick and the reply is read on the *next* tick, so the wait is very nearly a whole
period every time, and the filter had no spread to filter. A polled loop does not
add a random delay to a round trip; it quantises it. ⚠ Worth carrying forward: any
future "the filter will absorb it" argument needs the same check.

⛔ **And the relation the 5 ms gate reads was never this host's.** Both peers call
`ppcp_peer_publish_relations()`, and libppcp puts a published relation into the
*same* `p->relations` that an arriving `relation_update` writes to
(`ppcp_peer.c:1978` and `:2835`). This host's estimator produces
`tb:host -> tb:hosttime`; `offsetToRefNs()` and `PpcpShotBridge` convert the other
way, so they read **the phone's** published relation.

It improved regardless — 2.50 → 1.16 ms — because **this host is also the responder
to the phone's probes**, and a reply that waited for a tick inflated the phone's
measured RTT too. The poll degraded both directions, and the notifier fixed both.

✅ **`ppcp_peer_sync_observe()` was not needed.** libppcp reads `t4` with
`ppcp_clock_read()` at `ppcp_peer.c:2675`, *inside* `ppcp_peer_feed()` — which
`pump()` is what calls. Making `pump()` prompt was sufficient, and the second half
of this section's original recommendation was redundant.

⚠ The same 20 ms also capped how promptly a **shot event** was delivered. That is a
separate latency, and it is now gone too.

### 7.2 The rest of the audit

| Primitive | Survives? | Note |
|---|---|---|
| Min-RTT filtering, `PPCP_SYNC_ADMIT_DIV` | ✅ unchanged | A fraction, not a latency constant — written for this exact spread |
| Burst count / spacing (`PPCP_SYNC_BURST`, `_GAP_MS`) | ✅ **do not touch** | Skew sigma depends on the retained window's **span**, not its sample count, and extra traffic raises `min_rtt`. This is why seven cadence configs were measured and all reverted |
| `PPCP_SYNC_WINDOW` × `PPCP_SYNC_MAINTENANCE_MS` | ✅ unchanged, ⚠ know the cap | 32 × 5 s caps the span near 160 s, so `skew_sigma` floors at `offset_sigma·√2/160 s` on **both** transports — steady state ≈18 ppm on WiFi, ≈2 ppm on USB |
| Heartbeat / liveness, three missed intervals | ✅ unchanged | 6.3d already divorces it from the sync cadence. ⚠ Don't shorten it for wired: an unplug closes the tunnel, so use the **transport** event for fast failure, not a tighter timer |
| `zero_residence` / `sync_reply_stamps` | ✅ available and correct | ⚠ Confirm the device keeps `zero_residence` **off**. It folds responder residence into the RTT, which is a small fraction of a WiFi round trip and a large one of a USB round trip |
| Frame limits, `ppcp_cbor_limits_for_channel` | ✅ unchanged | Size limits, not time limits |
| Bind / handshake timeouts (10 s, 5 s) | ✅ unchanged | Embedding policy, generous on both |
| `epoch_stable`, clock discontinuity (6.4) | ✅ unchanged | A property of the clock, not the link |
| Socket buffers (`sndBufBytes`/`rcvBufBytes`) | ✅ leave at OS default | The bandwidth-delay product over a cable is tiny; usbmux does its own windowing |
| Arbitration deadline (8.2h) | ✅ unchanged | It *narrows* a race rather than closing it, by design; a faster link narrows it further for free |

### 7.3 What happens to the clock bias when the transport changes

The trap is real and worth naming: WiFi and usbmux have different latency
asymmetry, so an offset fitted on one is **wrong** on the other — not merely
stale. An estimator that carried its fit across a transport change would publish
a confident, wrong number, and `offset_sigma` would not widen to admit it.

✅ **It cannot happen here, by construction rather than by care.** A transport
change is a new link, and `ppcp_host_service.cpp:648` builds a fresh `Phone`, a
fresh `ppcp_peer` and therefore a fresh estimator per link. There is no code path
that moves a relation between links, and `CORE` I18 forbids composing relations
anyway. The §6.1 "Use cable" action deliberately **drops and re-dials** for this
reason — the re-convergence is not a side effect to be optimised away, it is the
correctness requirement.

⚠ Two things follow that are easy to get wrong later:

- ⛔ **Never add a "migrate the link to the other transport" optimisation.** It
  would silently carry a bias across an asymmetry change. If it is ever wanted,
  the honest form is a fresh estimator that re-converges, which is what dropping
  and re-dialling already is.
- ✅ Within one link, libppcp already has the mechanism if it is ever needed:
  `ppcp_peer_sync_trigger(PPCP_SYNC_TRIGGER_…)` restarts each estimator's window,
  because *"the FIT is stale, not merely the offset"* (`peer.h:641`).

### 7.4 The threshold that decides whether any of this matters

`PpcpShotBridge` rejects a conversion whose sigma exceeds `maxConversionSigmaNs`
— **5 ms, chosen because it is one frame at 200 fps** (`ppcp_shot_bridge.h:112`).
A phone whose relation sits above it cannot have its Candidates arbitrated at all.

That is the number to report M1 and M2 against, rather than sigma in the
abstract: the question is not *"is wired tighter"* but *"does wired move a phone
from marginal to comfortably inside 5 ms, and keep it there."* With §7.1 fixed,
the §1 arithmetic says comfortably; with §7.1 unfixed, wired lands around 0.55 ms
and still passes — which is exactly why the fix must be identified as a separate
contribution rather than folded into the wired result.

## 8. The bandwidth problem, and the split-transport option

⚠ **Wired is not automatically faster for bulk** — but the encoder settles the
part that worried this section most, and it settles it in wired's favour.

`RingBufferRecorder` encodes **HEVC at a provisional 50 Mbit/s**
(`RingBufferRecorder.swift:299`, issue #20), no B-frames, IDR at every 0.5 s
fragment boundary. That is **6.25 MB/s sustained** against roughly 30-40 MB/s
through usbmux on USB 2 — about **six times' headroom**, and a 3-second shot is
~19 MB rather than an unbounded stream.

✅ So *"does it fit USB 2 on non-Pro hardware"* is **yes, comfortably**, and the
real-time stream is not the risk. iPhone 15/16 non-Pro are USB 2 (480 Mbps raw);
Pro models are USB 3 (10 Gbps); a good 5 GHz link can exceed USB 2 on paper. What
remains at issue is **burst behaviour, not bitrate**: a backlog of queued captures
draining at once, and whether that starves the control channel.

✅ **And the unit of loss is already bounded.** A fragment carries an IDR and is
independently decodable — *"a fragment is independently decodable and a clip is a
concatenation"* (REQ-BUF-1) — so an interrupted transfer degrades to whole 0.5 s
fragments, never to undecodable bytes. Above that, `CORE` 5.14f-g makes a Capture
all-or-nothing: `transfer: confirmed` is set **only** by `capture_committed`, so a
partially delivered shot is never presented to analysis as a whole one. Neither
property is transport-specific, and neither may be given a wired variant.

The real risk runs the other way. usbmux multiplexes every tunnel
over **one USB pipe**. `CORE` §3.1 exists because a 25 MB capture must not
head-of-line block the next shot event — two TCP connections solve that at the IP
layer, and usbmux may reintroduce it one layer down. Bulk in flight will raise
`min_rtt` on the control channel, which is precisely the number this whole document
is trying to lower. Minimum-RTT filtering is robust to *some* of this by
construction — it keeps the lowest-RTT half of the window, so clean probes survive
a noisy neighbour — but "some" is a measurement, not an assertion.

**This is why the measurement in §11 gates the phases in §10, rather than following
them.**

✅ **And the host can read the link speed before it dials.** usbmux reports
`ConnectionSpeed` in `ListDevices` — measured live on the attached iPhone 16 as
`480000000`, confirming USB 2 from the device rather than from a spec sheet. So
the split-transport decision is **per device, not a global policy**: a USB 3 phone
takes bulk over the cable, a USB 2 phone may not, and the UI can say *"USB 2"*
honestly instead of implying the cable is fast.

If the measurement says what we expect, the answer is unusually clean, and it is
the place where Mark's *"different approaches by platform if it brings performance
and accuracy"* actually cashes out:

> **Split-transport link.** Channel 0 (control, sync probes, shot events) over the
> cable; channel 1 (bulk, captures) over WiFi. One PPCP link, two transports.

`ENC` 2.1b makes this legal outright — a listener binds streams by `link_id` and
**MUST NOT** infer association from the transport address. Both streams are dialled
by the host, so one `link_id` is minted once and both carry it. Both are separate
TLS sessions over the same `K_tls`, so the WiFi leg is exactly as protected as it
is today. The device already runs one `NWListener`; a second, on the network
interface, is the same actor with different parameters.

The result is the best of each medium: timing off a cable that has no contention,
bulk off a radio that has bandwidth, and the head-of-line problem solved at the
*physical* layer rather than only at the IP layer. **It is Phase 3 and it is
gated** — it doubles the connection matrix and the failure modes, and it is worth
that only if the numbers say the single-pipe contention is real.

---

## 9. Apple platform policy, and what could get the app rejected

Reviewed 29 August 2026 against `PinPointCapture`'s actual `Support/Info.plist`,
entitlements and sources. **The App Store risk from wired support is low, and it
is low for an uncomfortable reason: the sanctioned path does not exist, so the
route we take is one review cannot see.** The real exposure is that usbmux is
undocumented and unsupported, which §0 already answers.

### 8.1 What keeps the device side reviewable

iOS offers no public API for direct USB communication. The two routes are
External Accessory / iAP2 — which needs MFi licensing and an authentication
coprocessor in the accessory, and a PC is not an MFi accessory — and the usbmux
tunnel. **This design takes the second, and the device side of it is entirely
public API:** an `NWListener` on loopback. No IOKit, no `ExternalAccessory`, no
`MobileDevice`.

⛔ **Four things must never appear in `PinPointCapture`, and all four are absent
today (verified):**

| Must not appear | Why | Status |
|---|---|---|
| `UISupportedExternalAccessoryProtocols` | Triggers an MFi licence check | Absent |
| `external-accessory` in `UIBackgroundModes` | Same, plus §6.2's foreground rule | `UIBackgroundModes` absent entirely |
| Any IOKit USB or `MobileDevice` linkage | Guideline 2.5.1, private API | Absent |
| A background mode or silent audio session held open to keep a socket alive | A well-known rejection, and the shot detector's mic does not launder it | Absent |

The whole usbmux client lives in `PinPointStudio` (§4.1). **The wired work adds no
new dependency of any kind to the iOS binary** — one listener and one CBOR encoder
that `libppcp` already provides.

### 8.2 Licensing — checked, and it is already right

| | Licence | Reaches the iOS binary? |
|---|---|---|
| `libppcp` | MIT | Yes, and fine |
| `PinPointCapture` | **MIT** (verified, `LICENSE`) | It is the binary |
| `PinPointStudio` | GPL-2.0-or-later | **No**, and this design keeps it that way |

GPL is incompatible with App Store distribution (the VLC precedent). The usbmux
client is host-side only and the presence record is a format, not shared code, so
no GPL crosses. ⚠ The temptation this design creates is to "share the presence
codec" by lifting it out of `PinPointStudio`. **Do not** — put it in `libppcp`,
which both already link.

✅ That is free rather than aspirational: `PinPointCapture` already encodes CBOR
from Swift through `ppcp_cbor_writer` (`Packages/Core/Sources/CaptureCore/Detect/
Candidate.swift:245`), consuming `libppcp` as the SwiftPM target `CPPCP`. The
record codec belongs in `CaptureCore` beside it; only the listener is platform
code, so `REQ-PORT-3`'s layer purity is unaffected.

### 8.3 Two pre-existing submission gaps this work sits next to

Neither is caused by wired support. Both are on the same submission, so they are
recorded here rather than discovered at upload.

- ⛔ **There is no `PrivacyInfo.xcprivacy` in `PinPointCapture` today** (verified:
  no `.xcprivacy` anywhere in the tree). `Sources/Platform/PpcpTimebases.swift:68,71`
  uses `mach_absolute_time()` and `mach_continuous_time()`, which fall in the
  system-boot-time required-reason category — declared reason **35F9.1**,
  measuring elapsed time between events within the app, which is exactly what
  `CORE` P1/P2 use them for. This is a hard blocker for any submission and needs
  doing regardless of this document. *(The advice that prompted this section named
  `ProcessInfo.systemUptime`; this app does not use it. The conclusion holds under
  a different API.)*
- ⚠ **Guideline 4.2, minimum functionality.** An app that is only a sensor for
  desktop software is exposed. `PinPointCapture` v1 is specified offline-first —
  `CORE` §2.2 gives it Core + Capture + Detect + Mint + **Offline**, and the
  review disposition chose offline-only over tethered-only deliberately — so the
  answer exists on paper. **It does not exist in the build yet:**
  `libppcp/docs/conformance/freeze-readiness.md` records that the ring buffer is
  not wired to the capture path and `extractClip` answers `absent` on every
  device. Wired support does not change this exposure in either direction, but it
  must not be allowed to *substitute* for closing it.

### 8.4 Guideline 2.1 — the reviewer has a phone and nothing else

No Studio host, no cable, no rig. Two obligations follow, and the first is already
this design's discipline:

- **Absence of the wired path is silent and total.** `RV` 3.6a already forbids
  treating discovery failure as an error state, and §6.2 applies it here. A
  reviewer who never plugs in a cable must see a working app, not a disabled one
  or an error banner. The wired path contributes exactly nothing to the UI when no
  device is attached.
- **Review notes and a demo path are a deliverable, not an afterthought.** The
  submission needs a self-contained demo mode plus a video of the host pairing,
  because "any other hardware or resources that might be needed to review the app"
  is a PC running GPL desktop software the reviewer will not have. This is a
  Phase 2 work item, listed in §9.

### 8.5 USB Restricted Mode — the operational risk nobody sees coming

iOS blocks USB data on a device that has been **locked for over an hour**. At a
range that is a session that dies for no visible reason, and the first symptom is
a dropped link rather than anything that names the cause.

⚠ **`PinPointCapture` never sets `isIdleTimerDisabled`** (verified: no occurrence
in `Sources`). So the phone auto-locks on its own idle timer today. Capture needs
the foreground and the screen anyway, so the fix is one line and is owed to the
capture path independently of this document — but it becomes load-bearing here,
because §6.2 makes backgrounding a link drop and §6.1 makes a link drop expensive.

Handling: disable the idle timer while a session is live; on a wired link that
fails with the device present-but-refusing, say *"unlock the phone"* in the app log
rather than reporting a transport error.

### 9.6 Thermal — the cost of the cable that nothing else in this document pays

⛔ **A cable always charges, and an app cannot decline it.** iOS draws from a host
port whenever one is present and offers no API to refuse. So wired adds a charging
thermal load **on top of** sustained HEVC encode at capture frame rates — a load
the WiFi path does not carry at all. iOS then throttles the camera, and it does so
quietly: the effective frame rate falls and nothing announces it.

**This is a real cost and it is not visible anywhere else in this document.** It
belongs in front of a reviewer as a debit.

⚠ **But it is not only a debit.** The same current that heats the phone is what
keeps it alive: a long range session with the camera running is a battery problem,
and a cabled phone does not have it. §1.1 lists power among the reasons to build
this at all. M10 therefore measures **both** — time to throttle and time to flat —
because which one bites first in a session of realistic length is the whole
question, and neither number answers it alone.

✅ **The reporting half is already built.** `ThermalState` is read on the device
and surfaced in the capture UI (`ArmedScreen.swift:317`), and peer-level thermal
and battery reach the host on `heartbeat_ack` (`CORE` 7.4b) and appear in
Settings→Phones and the Cameras pill. So *"does the app report thermal state to
the host as a first-class signal"* is **yes, shipped 26 August**, and no new wire
field is needed. `Readiness` and per-Capture `achieved` cover the frame-rate half.

⛔ **The measurement half does not exist.** Nobody has measured *sustained capture
duration before throttle onset while charging*, on the oldest supported device.
That is **M10** (§11), and it is a Phase 1 item rather than a Phase 2 one: if a
cabled phone throttles sooner than an uncabled one, the cable buys tight sync and
costs capture, and that trade has to be known before the feature is sold as a
straight improvement.

⚠ **Do not confound it with issue #101.** `RingBufferRecorder` records
catastrophic capture gaps clustered at *"8894, 8703, 8854 ms across three
different capture modes — far too tight to be thermal or load"*. That defect is
open and is **not** thermal; a thermal run that hits it and reads it as throttling
would produce a false positive against the cable.

---

## 10. Work, by repo

### Phase 1 — the tunnel and a measurement (macOS only)

| Repo | Work |
|---|---|
| PPS | `src/Ppcp/ppcp_usbmux.{h,cpp}` — Qt-free usbmux client: `listDevices()`, `watch()`, **`dial(deviceId, port)`**. Minimal XML-plist emit/scan. macOS `AF_UNIX` first. ⛔ **Corrected 29 Aug: it cannot be `dial(udid, …)`** — usbmux `Connect` takes a `DeviceID` and the UDID never goes on the wire. And `DeviceID` is **per-attachment**: the same phone on the same cable was 306 on one probe and 308 on the next, `SerialNumber` unchanged. The UDID is the stable *identity*; the `DeviceID` is a *handle* valid only within one `Attached`→`Detached` span, and must never be persisted, cached across a watch restart, or used as a key that outlives the attachment. |
| PPS | `ConnectorConfig::dial` seam in `ppcp_transport.{h,cpp}`; `applyOptions()` guarded for `AF_UNIX`. |
| PPS | `cfg.listener` becomes a parameter, not a constant, at `ppcp_host_peer.cpp:487`; call `ppcp_peer_hello()` on the wired path. |
| PPS | `adoptLink()` takes the resolved pairing as a parameter (§6.3), and `noteLinkEstablished()` is not called for a wired reconnection. |
| PPC | `WiredPresenceListener` — fixed port, plaintext, serves the CBOR record. ⛔ **Bound to `127.0.0.1` via `requiredLocalEndpoint`**, not all interfaces (§5.3). |
| PPC | Start `PpcpListener` on the wired path with `listener: true` in `DevicePeer`; wire `psk_identity` and the actual port into the presence record. Loopback-bound, as above. |
| PPC | `isIdleTimerDisabled` while a session is live (§9.5) — one line, owed to capture regardless. |
| PPS | ⛔ **Prerequisite to M1 (§7.1):** stamp `t4` at the read via `ppcp_peer_sync_observe()`, and drive PPCP channel reads off a notifier rather than the 20 ms `QTimer`. Measure its effect on WiFi **first**, so it is not credited to the cable. |
| PPS | A **stub usbmuxd** for tests: the plist protocol over an `AF_UNIX` socket, scripted device attach/detach and `Connect` results. Makes the whole wired path testable with no phone and no cable — essential on unfamiliar ground. |
| both | `PINPOINT_SYNC_TRACE=1` on both legs; the §11 comparison, reported against the 5 ms `maxConversionSigmaNs` gate (§7.3). |

**Definition of done for Phase 1:** an iPhone on a cable reaches
`session_open` with a `TimebaseRelation` published, and a wired-vs-WiFi
`min_rtt` / `offset_sigma` / time-to-skew-target comparison exists as numbers in
this document.

### Phase 2 — cross-platform and product

| Repo | Work |
|---|---|
| PPS | Windows `AF_INET 127.0.0.1:27015` provider; Linux `AF_UNIX` provider. Detect-and-explain when the provider is absent, per the §6.2 diagnostics table. |
| PPS | ⚠ Name an owner for the Linux `usbmuxd` + udev prerequisite: a documented supported configuration, or best-effort. It is a support burden either way and it should not be discovered by a user. |
| PPS | `WiredDeviceWatch` on **its own thread — not the accept thread** (§6.3). ⚠ No `udid → pairingId` cache: the identity must be re-read each time anyway (it refreshes per listener session), resolution is one HMAC per held pairing, and a cache would go stale the moment a phone is re-paired. |
| PPS | §6.1 arbitration: per-pairing advertisement suppression (suppress before dialling, restore on failure), and the `onDeclare()` backstop keyed on `counterpartId` via the existing `peerForId()`. |
| PPS | Settings→Phones shows the transport and offers **"Use cable"** when a wired path is available and unused (§6.1). |
| PPS | The sync trace and diagnostic export carry the transport beside the `RV` 5.4k TLS outcome. |
| PPC | Presence listener lifecycle: refresh `rn2` on foreground entry and pairing-set change; stop on background (§6.2). |
| PPC | The presence record lists persisted pairings **plus any scanned, not-yet-connected code** (§6.5) — the set must mirror the host's resolver. |
| PPC | `PrivacyInfo.xcprivacy` with reason 35F9.1 for `mach_absolute_time`/`mach_continuous_time` (§9.3) — pre-existing, and this is the submission it blocks. |
| PPC | Review notes, a self-contained demo path and a pairing video for guideline 2.1 (§9.4). |
| libppcp | Raise the 5.3a erratum of §5.5 with Phase 1's numbers attached; document the direct-path presence record so a third party could interoperate. |

### Phase 3 — gated on §11

Split-transport link (§8), if and only if the single-pipe contention measurement
justifies it.

---

## 11. Measurement plan — this gates the phases, it does not follow them

The rig already exists: `PINPOINT_SYNC_TRACE=1` plus the `devicectl` launch loop
recorded for the sync-convergence work, and the WiFi baseline captured then is the
control. Every run needs a WiFi control taken the same session — pose and analysis
runs on this project are non-deterministic and network conditions are worse.

| # | Question | Measurement | Decides |
|---|---|---|---|
| ~~M0~~ | ~~How much of today's `min_rtt` is the 20 ms poll?~~ | **✅ DISCHARGED 29 Aug**: 12.95 → 1.64 ms, i.e. **87% of the round trip was the poll**. `offset_sigma` 2.50 → 1.16 ms | Done. ⚠ It also moved the goalposts — see §1 |
| M1a | `min_rtt` over usbmux, **idle** | 5 min idle link, wired vs the 1.64 ms WiFi control | The floor. ⚠ Expect ~3× — real, but not on its own the reason to build this |
| M1b | ⛔ **`min_rtt` and sigma under CONTENTION** | Both arms with the WiFi loaded — other clients, 2.4 GHz, distance, a body in the path. Report the **95th percentile and the worst 5 s window**, not the converged floor | **This is the real M1.** The cable is selling a floor that does not move, and §1's idle measurement cannot see that. A shot happens at one instant, not after five quiet minutes |
| M11 | Does WiFi ever *fail* where wired does not? | Drop/roam/reconnect counts over a long session, both arms | The reliability half of §1.1, which no sigma number captures |
| M2a | Does the phone hold `zero_residence` off, and what is its residence time? | `ppcp_peer_sync_zero_residence()`, and `t3−t2` distribution | §7.2 — responder residence is a small share of a WiFi round trip and a large share of a USB one |
| M2 | What is time-to-skew-target? | Span at which `skew_sigma_ppm` crosses the WiFi 90 s value | The headline claim, in the units an operator cares about |
| M3 | Does bulk poison control? | `min_rtt` and `offset_sigma` on channel 0 during a 25 MB channel-1 transfer, wired vs WiFi | Whether Phase 3 is needed |
| M4 | usbmux bulk throughput on a USB 2 phone | MB/s for a 25 MB capture, wired vs 5 GHz | Whether wired is a bulk regression, and Phase 3 again |
| M5 | Does `Connect` need device trust? | **Fresh, untrusted** device | Onboarding copy, and the only wired-only user step (§6.6). ⚠ Still open: the 29 Aug probe used Mark's dev phone, which is already trusted, so a successful `Connect` there proves the mechanism and **not** the trust question |
| M6 | Concurrent tunnels | 2 and 3 channels at once through usbmux | That `CORE` T2/T5 survives the multiplexer at all |
| ⛔ M12 | **Does the device-side mux dial `127.0.0.1` or `::1`?** | Read the presence record over a real cable | `NWParameters.requiredLocalEndpoint` **pins the address family**, so the listener is IPv4-loopback only. If the mux dials `::1` the read fails and the phone silently looks un-wired. ✅ Fallback: `requiredInterfaceType = .loopback` — both families, same LAN exclusion. ⚠ Unanswerable in a simulator. **Check this first if M1 cannot read a record** |
| M7 | Does a loopback-bound `NWListener` avoid the local-network prompt? | Fresh install, permission never granted, then declined | §5.3's second reason. Verify the exemption; do not trust it |
| M10 | **Sustained capture cabled vs not — thermal AND battery** | Longest supported capture on the oldest supported device, `thermalState` **and battery level** logged throughout, both arms | §9.6. ⚠ Both directions: charging is a thermal cost and the reason a session outlasts a battery. Report time-to-throttle **and** time-to-flat. ⛔ Phase 1. Screen for issue #101's ~8.8 s gap signature before attributing anything to heat |
| M8 | USB Restricted Mode | Phone locked > 1 hour with the cable attached, then a dial | §9.5 — whether the idle-timer fix is sufficient or the path dies between sessions |

⚠ M1 and M3 are the two that can kill or reshape the design. Run them first, on
macOS, before any Windows or Linux work.

**Already discharged (29 August 2026, build Mac, phone attached).** The usbmux
wire format of §4.2 — header layout, plist keys, the `PortNumber` byte-swap, and
result codes 0 and 3 — was verified against Apple's own daemon with a ~40-line
Python probe over `AF_UNIX`. Anyone re-running it needs no entitlement and no
third-party library:

```python
body = plistlib.dumps({'MessageType': 'ListDevices', 'ClientVersionString': '…',
                       'ProgName': '…', 'kLibUSBMuxVersion': 3}, fmt=plistlib.FMT_XML)
sock.sendall(struct.pack('<IIII', 16 + len(body), 1, 8, tag) + body)
```

That is the single largest block of asserted-from-memory detail in this document,
and it is now measured. It also means the Phase 1 host-side client can be written
against a known-good reference instead of discovered by trial.

---

## 12. Risks and open questions

| | Risk | Handling |
|---|---|---|
| ⛔ | usbmux's single pipe reintroduces the head-of-line blocking `CORE` §3.1 exists to prevent | M3. Phase 3 is the answer if it is real |
| ⛔ | The 5.3a erratum is refused | Phase 1 and 2 still measure and demonstrate; the BoringSSL-on-iOS fallback (§5.5) exists and has independent value |
| ⛔ | **Both halves of the wired path are unreached code**: `PpcpListener` on the device and `Connector` in the host have extensive tests and no product caller | The largest single estimate risk in Phase 1. Exercise both on loopback before a cable is involved |
| ⛔ | Both paths dial autonomously and in opposite directions, so a plugged-in phone double-connects: two `Phone` rows, duplicated preview consumers, one phone's Candidates entering the arbiter twice | §6.1 — suppress the advertisement before dialling, plus an `onDeclare()` backstop on `counterpartId` |
| ⚠ | Suppressing on "device attached" rather than "presence proven" would break WiFi for a phone plugged in only to charge | §6.1 rule 4. Fail open on every wired failure |
| ⛔ | A leaked §6.1 advertisement suppression would leave a cable-born pairing never advertised, so the phone could never find the host over WiFi | §6.6 — release on *any* ending, not just a failed dial. Explicit test: pair over cable, unplug, confirm WiFi reconnect |
| ⚠ | A phone paired from a `mu > 1` code can never use the cable — `PairingSecretStore.save` refuses to persist a group credential (7.4f) | §6.4. Latent today (this host publishes only `mu: 1`), but it constrains the multi-device workflow the spec allows |
| ⚠ | A cable inserted mid-session does nothing, which reads as broken to the operator who inserted it | §6.1 — an explicit "Use cable" action, never a silent automatic switch |
| ⛔ | `adoptLink()` reads `pairingId()` off the link, which is empty when we dialled — the phone would connect and never appear | §6.3. Pass the pairing the host already resolved. Same class as the fixed empty-Phones-list bug |
| ⚠ | A blocking dial on the accept thread would starve WiFi accepts and `ENC` 2.1d channel collection | §6.3 — its own thread, same queued hand-off |
| ⚠ | usbmux `min_rtt` is dominated by the daemon hop, not the wire | M1. If it is, the design is still worth it only if the *tail* is tighter — which is what the estimator actually filters on |
| ⚠ | Windows users without Apple Devices installed | Detect and explain. Never an error state |
| ⚠ | Linux `usbmuxd` socket permissions | Document the group; detect and explain |
| ? | Does a raw usbmux `Connect` need "Trust This Computer"? | M5 — believed no, not measured |
| ⛔ | **A cable always charges and iOS cannot decline it**, so wired adds a thermal load on top of sustained HEVC encode and iOS throttles the camera silently. Unmeasured | §9.6, M10. The one cost that offsets §1's benefit, and it is Phase 1 |
| ✅ | ~~Today's measured WiFi `offset_sigma` is not in this document~~ | **CLOSED 29 Aug** — 1.16 ms, in §1. ⚠ And it is good enough that it weakens the case this document argues |
| ✅ | ~~The host stamps `t4` on a 20 ms timer~~ | **CLOSED 29 Aug** — notifier per channel. It cost 11.3 ms of the 12.95 ms round trip, ~19× the 0.6 ms this document predicted (§7.1) |
| ⛔ | usbmux is undocumented, unversioned and unsupported by Apple; it could change or close in any iOS release | §0 — wired is an optimisation, never a requirement. Losing it must cost a transport, not the product |
| ⚠ | No `PrivacyInfo.xcprivacy` exists in `PinPointCapture` | §9.3. Pre-existing hard blocker on the same submission; `mach_absolute_time` needs reason 35F9.1 |
| ⚠ | USB Restricted Mode kills the cable after an hour locked | §9.5. Disable the idle timer while a session is live; M8 confirms it is enough |
| ⚠ | The app auto-locks today — `isIdleTimerDisabled` is never set | §9.5. One line, owed to the capture path anyway |
| ⚠ | Guideline 4.2: the app must stand up with the cable unplugged | §9.3. Pre-existing and tracked in freeze-readiness; wired must not be allowed to substitute for closing it |
| ⚠ | Guideline 2.1: the reviewer has no host, no cable and no rig | §9.4. Silent absence plus review notes, a demo path and a video |
| ⚠ | The presence codec gets "shared" between a GPL repo and an MIT App Store binary | §9.2. Specify it in `libppcp`, or write it twice |
| — | Backgrounding mid-session drops the link and discards the sync fit | Accepted, and named in §6.2. There is no background mode that would fix it and none may be added |
| ? | Does the iOS listener survive the app being backgrounded mid-session? | It does not, by design (§6.2). Confirm the failure is a clean drop and not a hang |

---

## 13. Future-proofing — priority four, and it did not drive anything

Two seams exist for free and neither cost a decision:

- **`ConnectorConfig::dial`** is a `std::function` returning a connected fd. Any
  future tunnel — an Android `adb forward`, a Thunderbolt bridge, an SSH tunnel to
  a remote host — is a different `DialFn` and nothing else. This is `RV` §2's
  *direct* path expressed as one function pointer.
- **The presence record** is an `RV` §3 advertisement in a different envelope. A
  second wired provider reuses `pvAcceptsMajor()`, `decideDial()` and the
  client-side 5.3b verification unchanged.

⛔ **Android would not reuse most of this, and pretending otherwise would be the
"iOS feature in a generic costume" failure.** Written out now rather than assumed:

| | Android |
|---|---|
| `adb forward` / `reverse` | ⛔ **Not shippable.** Needs developer mode on the phone and `adb` on the user's machine |
| AOA (Open Accessory) | Different semantics — no TCP stream falls out of it |
| **USB tethering (RNDIS/NCM)** | ✅ **The likely answer** — a real network interface, so the *existing* WiFi path runs over a cable |

So the honest position: **the `DialFn` seam generalises; §5's presence-record
ceremony does not, and would not be needed.** Android over USB tethering needs no
usbmux client, no presence record, no identity publication and no direction
inversion — the phone dials a host address as it always did, over a wired
interface. That makes Android *easier*, not harder, and it means none of §5 should
be generalised in anticipation of it. This is an **iOS mechanism**, named as one.

---

## 14. Summary of decisions

| Decision | Because |
|---|---|
| A transport change is always a new link, hence a new estimator | WiFi and usbmux have different latency asymmetry; carrying an offset across would publish a confident wrong number. ⛔ Never add link migration (§7.3) |
| The cable's thermal cost is a debit on the design, measured in Phase 1 | Charging is unavoidable over USB and iOS throttles quietly; reporting already exists, the measurement does not (§9.6) |
| Android is not designed for, and §5 must not be generalised toward it | USB tethering gives Android a network interface, so it needs none of the presence ceremony — this is an iOS mechanism (§13) |
| No protocol primitive changes for wired | PPCP is transport-agnostic and min-RTT filtering was written for exactly this spread; what must change is the **embedding's stamping**, which `CORE` 6.1c already anticipated (§7) |
| Wired is bought for **contention-resistance, reliability and power** as much as for sigma | Phase 0 took most of the idle-network sigma win with no cable; none of the other three moved (§1.1) |
| **Wired is an optimisation and never a requirement** | usbmux is undocumented and unsupported; losing it must cost a transport, not the product (§0) |
| usbmux tunnel, not USB Ethernet or MFi | The only path that works without a cellular plan or a hardware programme |
| First-party usbmux client, ~600 lines | Three platforms differ by one socket type; avoids LGPL and `libplist`; stays Qt-free for the headless harness |
| Device listens, host dials, host is TLS client | Forced by usbmux; satisfies `RV` 5.2g without bending it |
| Device publishes its PSK identity; host verifies it under 5.3b before dialling | The only way past `RV` 3.5d without a second TLS stack on iOS. Discloses nothing a `ClientHello` does not |
| The cable carries a pairing but never establishes one; the QR scan stays the only root of trust | Physical possession authenticates nothing, and a cable-only root would mean building `RV` §11 (§6.5) |
| A first connection may still run over the cable | `identityResolver()` already accepts outstanding codes, so it costs one device-side rule and no host machinery (§6.5) |
| A cable-born pairing is an ordinary pairing, advertised and reusable over WiFi at once | `RV` 11.1a's claim, applied here: nothing marks a pairing with the transport that made it (§6.6) |
| The presence record carries the PSK identity and **no `rid`** | `identityResolver()` already returns `{kTls, pairingId}` from the identity the host must verify anyway; a `rid` would be a second resolver for the same answer (§5.3) |
| Nothing is keyed on address, endpoint or `udid` | Every persistent key is a pairing or a peer, which is why the transport swap costs nothing to reuse (§6.4) |
| One fixed presence port; every other port published in the record | Removes the whole port-derivation problem |
| Both wired listeners bind `127.0.0.1` only | Otherwise the plaintext pairing list is readable from the LAN, and the local-network permission is back in play (§5.3) |
| Foreground only; no background mode, ever | None covers a USB socket, and the silent-audio workaround is a known rejection (§6.2, §8.1) |
| The usbmux client is host-side only | No new dependency in the iOS binary, and no GPL near an App Store submission (§9.2) |
| Prefer wired at session start; never switch a converged link | A reconnect rebuilds the peer and discards the sync fit (`ppcp_host_service.cpp:648`) |
| Split-transport link is Phase 3 and gated | Legal under `ENC` 2.1b, and only worth its complexity if M3/M4 say the single pipe is a problem |
| WiFi/USB arbitration is **advertisement suppression**, not connection selection | Only the host sees both signals — iOS cannot tell a data host from a charger — and under 3.5e the host owns the WiFi path's only trigger (§6.1) |
| Duplicate links collapse on `counterpartId`, keeping the incumbent | `Peer.id` is the phone; a pairing is not (`mu > 1` shares one). And `counterpartId` is not known until `onDeclare()` (§6.1) |
| Switching transport mid-session is an operator action, never automatic | A switch discards the sync fit and re-converges from nothing — a silent downgrade sold as an upgrade (§1, §6.1) |
| `adoptLink()` is told the pairing; the dial gets its own thread | Both are forced by tracing the existing intake path, not by the protocol (§6.3) |
| Measure before Windows and Linux | If M1 shows no `min_rtt` win, the accuracy case is gone and only the Windows/Linux reconnection case survives — a much smaller reason to build a much smaller thing |
