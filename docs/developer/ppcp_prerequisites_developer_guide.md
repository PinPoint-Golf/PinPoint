# PPCP Prerequisites — Developer Guide

**Audience**: Developers building PinPointStudio with the capture-device link
**Location**: `src/Ppcp/`, plus the `if(PP_OPENSSL_FOUND AND TARGET ppcp)` block in `CMakeLists.txt`
**Status**: Linux verified 29 Aug 2026; macOS shipping; Windows deferred (CA5)

---

## Why this page exists

PPCP has **two independent optional dependencies**, and missing either one
degrades the build *silently and by design*. Neither is an error, neither stops
the build, and both announce themselves only as a line in CMake's configure
output. That is deliberate — RV 3.6b requires discovery failure to be silent —
but it means a developer can build PinPointStudio, run it, and never understand
why a phone will not connect.

⛔ **The most common way to lose a day here is to conclude "PPCP is broken on
Linux" when the real answer is "a package is not installed".** It happened during
the Linux port: this machine had no OpenSSL, so the entire transport had never
been compiled, and CMake's warning had been scrolling past unread for weeks.

---

## The two dependencies, and what each one buys

| | Dependency | Without it you lose | CMake gate |
|---|---|---|---|
| **Transport** | OpenSSL ≥ 1.1.1 (dev headers) | **Everything.** No listener, no TLS, no link of any kind — wired or WiFi | `HAVE_PPCP_TRANSPORT` |
| **Discovery** | A DNS-SD library | **Reconnection only.** Pairing by code still works | `PP_HAVE_DNS_SD` |

They are not layered: discovery is only ever consulted *inside* the transport
block, so no OpenSSL means no discovery regardless of Avahi.

### Read the configure output

A fully-equipped Linux box prints:

```
-- OpenSSL 3.5.5: /usr/lib/x86_64-linux-gnu/libssl.so
-- PPCP discovery: DNS-SD via dns_sd
-- PPCP transport: OpenSSL 3.5.5, libppcp 0.1.0 (a9785bb, local)
```

If instead you see either of these, you have a capability gap, not a bug:

```
CMake Warning: PPCP transport NOT built — OpenSSL development headers not found.
-- PPCP discovery: no DNS-SD library — reconnection discovery is off.
```

⚠ **A stale CMake cache will not re-detect a package you just installed.**
`find_package` caches a NOTFOUND result and treats it as final. After installing
OpenSSL or Avahi you must clear the cached entries, not merely re-run `cmake`:

```
cmake -U 'OPENSSL_*' -U '_OPENSSL*' -U 'DNSSD_*' -S . -B build/<your-build-dir>
```

---

## Install lines

### Linux (Debian / Ubuntu) — a **supported** configuration

```bash
# Build-time
sudo apt install libssl-dev libavahi-compat-libdnssd-dev

# Run-time
sudo apt install usbmuxd avahi-daemon

# Diagnostics — not required to build or run, but you will want them
sudo apt install libimobiledevice-utils avahi-utils
```

`usbmuxd` is **runtime only** — it is a daemon, not a library; nothing links
against it. PPS speaks to it over `/var/run/usbmuxd` as a plain AF_UNIX client.

⚠ **Socket permissions vary by distribution.** On Ubuntu 26.04 the socket is
created `srw-rw-rw-` (0666) and any desktop user can talk to it, so no group
membership is needed. Other distributions restrict it to a `usbmux` group. If
`ListDevices` returns nothing while a phone is plainly attached, check this
before suspecting the cable:

```bash
ls -l /var/run/usbmuxd        # expect srw-rw-rw-, or your user in the owning group
```

### macOS

Nothing to install for discovery — Apple ships the `DNSService*` symbols in
libSystem, which is why `ppcp_discovery.cpp` had no link line at all until the
Linux port added one. OpenSSL comes from Homebrew:

```bash
brew install openssl@3
```

usbmux is provided by the OS.

### Windows

⛔ **Deferred (CA5), and it is a dependency decision rather than a protocol one.**
There is no `dns_sd.h` on Windows without Apple's Bonjour SDK, which is an
installer and a system service rather than a header — so `PP_HAVE_DNS_SD` is
never set and both discovery factories return null. usbmux requires Apple Mobile
Device Service, which ships with iTunes / Apple Devices and **cannot be
redistributed with PinPointStudio**. See `wired_transport_impl_plan.md` §Phase 2W.

---

## How the Linux discovery port works

Avahi ships `avahi-compat-libdns_sd`, which exposes the **same `dns_sd.h` API**
Apple does, so Linux reuses `BonjourBrowser` and `BonjourAdvertiser` rather than
gaining a second backend.

⚠ **It was expected to be five preprocessor guards and a CMake probe. It was
not.** Those five guards are genuinely all it takes to *compile*, and everything
below is what it took to make the browser actually work — an identical API is not
identical behaviour.

`ppcp_discovery.cpp` derives one macro and keys everything off it:

```c
#if defined(__APPLE__) || defined(PP_HAVE_DNS_SD)
#define PP_DNS_SD_AVAILABLE 1
#endif
```

Three things measured on Ubuntu 26.04 with `libavahi-compat-libdnssd1 0.8`,
recorded because each was an open question:

- ✅ **`DNSServiceUpdateRecord` updates the TXT record in place** and the service
  instance name does not move. RV 3.2d rotation therefore costs one announcement,
  as on macOS, and the degradation clause at `ppcp_discovery.h:302-308` is unused.
- ✅ **`DNSServiceRefSockFD` returns a real pollable fd**, which is what
  `RvBrowser::fd()` / `RvAdvertiser::fd()` hand to their `QSocketNotifier`.
- ⚠ **`DNSServiceProcessResult` blocks** when nothing is pending. Poll the fd
  first. The existing code does; new call sites must not assume otherwise.

⛔ **And two traps that only Avahi has, both of which cost real time to find.**
Neither can be reproduced on macOS, so they are recorded here rather than left to
be rediscovered:

1. **A resolve called from inside a browse callback never returns.** The shim
   will not service it re-entrantly, and the symptom is a hang, not an error —
   the first Linux port froze for a full 300 s with the main thread in
   `unix_stream_data_wait`. `BonjourBrowser` therefore records instances in the
   callback and resolves them from `process()`, after `DNSServiceProcessResult`
   has returned.
2. **A resolve is not finished after one `DNSServiceProcessResult`.** Avahi wakes
   the socket several times before it delivers, so retiring the ref on the first
   wakeup destroys the resolve before it answers — and it fails *quietly*, as a
   browse that finds instances and resolves none. `Pending::done` is set by the
   resolve callback and nothing is retired until it is.

Because a resolve costs **650 ms** here (against sub-millisecond on macOS), it
cannot be done inline on the GUI thread at all. Off Apple, `fd()` returns an
**epoll set** carrying the browse socket plus every pending resolve socket — N
fds folded into the one the `RvBrowser` interface exposes, so the owner's single
`QSocketNotifier` drives everything and nothing ever blocks.

⚠ **Expect a warning banner on stderr.** Every process linking the compat shim
prints:

```
*** WARNING *** The program 'PinPointStudio' uses the Apple Bonjour compatibility layer of Avahi.
*** WARNING *** Please fix your application to use the native API of Avahi!
```

It comes from the library, cannot be suppressed from our side, and is harmless.
The alternative is a second backend against Avahi's native API — which is a real
option (`avahi_entry_group_update_service_txt()` satisfies 3.2d cleanly) but
would need an eventfd shim, because native Avahi is poll-loop driven and exposes
no single fd the way `DNSServiceRefSockFD` does.

---

## What you can still do with pieces missing

| Missing | Pairing by QR | Reconnect (WiFi) | Reconnect (cable) |
|---|---|---|---|
| Nothing | ✅ | ✅ | ✅ |
| DNS-SD library | ✅ | ⛔ needs a fresh code each time | ✅ |
| `usbmuxd` | ✅ | ✅ | ⛔ |
| OpenSSL | ⛔ | ⛔ | ⛔ |

The middle row is the one worth internalising: **pairing has never depended on
mDNS**. The pairing code carries every address the host is reachable at (RV 4.3d),
which is precisely what makes it work where discovery cannot — *"discovery WILL
NOT WORK AT A RANGE"* (3.6a), and a golf range is the target environment.

---

## Diagnosing a phone that will not connect

Work down this list; it is ordered by how often each is the answer.

```bash
# 1. Is the transport even compiled in?  Re-read the configure output.
grep -E "PPCP (transport|discovery)" <your-build-log>

# 2. Is the daemon up?  It is socket-activated, so "inactive" with no phone
#    attached is CORRECT — attach a phone first, then check.
systemctl is-active usbmuxd

# 3. Does usbmux see the phone at all, independently of PinPointStudio?
idevice_id -l                 # UDID, or nothing

# 4. Is the host advertising?  This is the direct test of the discovery port.
avahi-browse -rpt _ppcp._tcp  # expect a PPCP-XXXXXXXX instance while PPS runs

# 5. Is the phone trusted by THIS machine?  Trust is per-host and lives in a file.
ls /var/lib/lockdown/*.plist
```

⚠ **Trust is per-host.** A phone paired to another machine is not paired to
yours, and PPCP pairing is a *second, separate* thing from iOS lockdown trust —
a box can be trusted by the phone and still hold no PPCP pairing, in which case
you need a QR scan regardless of what the cable says.

Design `§6.2` carries the full diagnostic table the wired path is held to, along
with the one row known to be wrong: a closed presence port and a refusal at the
mux layer are the same wire event (`Number=3`), so the host names both causes
rather than guessing.

---

## Related

- `docs/design/wired_transport_design.md` — the protocol and the §6.2 diagnostics table
- `docs/implementation/wired_transport_impl_plan.md` — phase tracker; §Phase 2L is the Linux port
- `docs/developer/testing_developer_guide.md` — the PPCP suite is standalone, **not** part of the app build
