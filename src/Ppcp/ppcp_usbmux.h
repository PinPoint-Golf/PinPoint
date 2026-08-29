/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

// A first-party usbmux client — the USB tunnel of the wired PPCP transport.
// docs/design/wired_transport_design.md §4; Phase 1 of
// docs/implementation/wired_transport_impl_plan.md.
//
// WHY FIRST-PARTY AND NOT `libusbmuxd` (design §4.1).  The protocol we need is
// three messages and a 16-byte header.  Hand-rolling it buys one code path on
// three platforms — the only per-platform difference is the socket — no new
// LGPL surface on three platforms, and no `libplist`.
//
// DELIBERATELY Qt-FREE, matching ppcp_transport.cpp's discipline, so the
// headless conformance harness can still drive the wired path and so the
// sockets can be pumped from whichever thread the embedding chooses.  Nothing
// in this file creates a thread, an event loop or a notifier; it hands out file
// descriptors and the caller decides what watches them.
//
// ── THREADING, AND THE TWO RULES THAT COME WITH IT ─────────────────────────
//
// ⛔ `listDevices()` and `dial()` BLOCK.  A wired dial is a usbmux `Connect`
//    plus, downstream, two concurrent TLS handshakes, and can take hundreds of
//    milliseconds.  They MUST NOT run on PpcpHostService's accept thread, which
//    polls `acceptChannelFor()` and then blocks 250 ms in `accept()` — a wired
//    dial parked there starves WiFi accepts and stalls preview-channel
//    collection (design §6.3).  Worker thread only.
//
// ✅ `Watch` needs no thread at all.  usbmux's `Listen` is a long-lived readable
//    fd, which is exactly the shape the DNS-SD browser already has: put a
//    `QSocketNotifier` on `Watch::fd()` on the GUI thread and call `poll()` when
//    it fires.  ⚠ TEARDOWN ORDER IS LOAD-BEARING: destroy the notifier FIRST,
//    then call `stop()` (or let the Watch destruct).  A QSocketNotifier left on
//    a closed socket is a recorded trap in this codebase
//    (ppcp_host_service.cpp:1205).  `Watch` never closes its fd behind the
//    caller's back — only `stop()`, the destructor and a failed `start()` do.
//
// ── ABSENCE IS NOT AN ERROR (design §6.2, RV 3.6a) ────────────────────────
//
// No usbmux provider, nothing plugged in, a charge-only cable, a phone this
// host is not paired with — none of these is an error state and none of them
// gets a banner.  Every entry point therefore returns a `Result` carrying a
// `Status` from the design §6.2 diagnostics table, and the caller turns that
// into at most one `ppWarn()` line for somebody who went looking.  Silent in
// the UI and specific in the log are not in tension; they are the requirement.

#include "ppcp_transport.h"   // Ppcp::pp_socket_t, Ppcp::kInvalidSocket

#include <cstdint>
#include <string>
#include <vector>

namespace Ppcp {
namespace Usbmux {

// ── The presence port (Phase 1 contract C4) ────────────────────────────────
// The plaintext port PinPointCapture's WiredPresenceListener serves the
// `WiredPresence` CBOR record on, over the usbmux tunnel.  Private range, no
// IANA assignment; a collision is survivable by design because the host then
// gets a record it cannot parse and treats the device as not wired (design
// §5.3).  ⛔ Declared once per repo — this line in PPS and
// `WiredPresenceListener.swift` in PPC — and the two values must agree.
constexpr std::uint16_t kWiredPresencePort = 50915;

// ── Identity: the UDID is stable, the DeviceID is NOT ──────────────────────
//
// ⛔ MEASURED 29 Aug 2026, and it will bite anyone who assumes otherwise: the
// same iPhone on the same cable was `DeviceID` 306 in the morning and 308 in
// the afternoon, with an unchanged `SerialNumber`.  A `DeviceID` is usbmuxd's
// handle on ONE ATTACHMENT and is valid only between that attachment's
// `Attached` and its `Detached`.  Never persist it, never carry it across a
// `Watch` restart, and never make it a map key that outlives the attachment.
//
// `SerialNumber` (the UDID) is the stable identity — key on that.  `DeviceId`
// is only ever the handle you dial with, taken from the CURRENT attachment.
using DeviceId = std::uint32_t;

// ── What the §6.2 diagnostics table needs to tell apart ────────────────────
//
// Design §6.2: "each row is distinguishable, or the row is wrong".  These are
// the rows this layer can see; the two it cannot (a presence record whose
// entries resolve to no held pairing, and a record that fails to parse) belong
// to the reader above this one.
enum class Status {
    Ok = 0,

    // Cannot open /var/run/usbmuxd (or 127.0.0.1:27015).  Windows: Apple
    // Devices / iTunes not installed.  Linux: usbmuxd not running, or the
    // socket's group permissions exclude us.  macOS: should not happen — the
    // daemon is part of the OS.
    NoProvider,

    // The socket opened but the daemon did not speak the protocol: a short
    // read, a header whose version/message/length is not what §4.2 records, or
    // a plist that will not scan.  Distinct from NoProvider because it means
    // something IS listening on that socket and it is not usbmuxd.
    ProviderProtocol,

    // A request or a reply did not complete inside its deadline.
    Timeout,

    // `ListDevices` returned an empty list: nothing plugged in, OR a
    // charge-only cable with no data pairs.  usbmux cannot tell those apart
    // and neither can we — the log line must say both.
    NoDevices,

    // Devices ARE attached but none of them has ConnectionType == "USB".
    // ⛔ These are WiFi-paired devices (ConnectionType == "Network") and MUST
    // NOT be treated as wired: doing so would label a WiFi link as wired and
    // silently invalidate every timing claim this work exists to make (§4.2).
    // An accuracy requirement, not tidiness.
    NoWiredDevices,

    // `Connect` → Result Number 2, "bad device": the DeviceID names no current
    // attachment.  Either the phone was unplugged between the ListDevices and
    // the dial, or the attachment was torn down under us — USB Restricted Mode
    // is the likely cause of the latter (design §9.5, pending M8).
    UnknownDevice,

    // `Connect` → Result Number 3, "connection refused", VERIFIED live.  The
    // device is there and the mux reached it; nothing is listening on that port
    // inside the device.  For kWiredPresencePort that means the capture app is
    // not running, or not in the foreground.
    //
    // ⚠ Design §6.2 carries a SEVENTH row, "Connect refused at the mux layer —
    // trust not granted", as a separate diagnosis.  At the wire level it is not
    // separate: an untrusted device is expected to surface as this same
    // Number 3, and M5 (§4.3) has not been run to say otherwise.  Until it has,
    // ConnectRefused is ONE row wearing two hats and the log line says so.
    ConnectRefused,

    // `Connect` → Result Number 5, "bad version": usbmuxd refused
    // kLibUSBMuxVersion 3.  Would mean a daemon older than anything shipping.
    BadVersion,

    // A Result Number this client has no name for.  Carried through verbatim in
    // Result::muxResult so a log line can still be specific.
    UnexpectedResult,
};

// A short, non-secret phrase for the app log.  Never contains an identity, a
// key, or a UDID — RV 7.2b.
const char *describe(Status s);

struct Result {
    Status status = Status::Ok;

    // The raw `Number` from a usbmux `Result` reply, or -1 when the failure
    // happened before one arrived.  Kept verbatim so an UnexpectedResult can be
    // reported precisely rather than as "something went wrong".
    int muxResult = -1;

    // errno / WSAGetLastError() at the point of a socket failure, or 0.
    int systemError = 0;

    bool ok() const { return status == Status::Ok; }
    explicit operator bool() const { return ok(); }

    // "usbmux: <describe(status)> (Number=3)" — one line, ready for ppWarn().
    std::string message() const;
};

// ── A device, as usbmuxd describes it ──────────────────────────────────────
struct Device {
    // ⛔ Valid only for the current attachment.  See DeviceId above.
    DeviceId deviceId = 0;

    // The UDID.  This is the stable identity; hold onto this, not deviceId.
    std::string serialNumber;

    // "USB" or "Network", verbatim.  Kept as the daemon spelled it rather than
    // reduced to a bool so that an unexpected third value is visible in the log
    // instead of being silently folded into "not wired".
    std::string connectionType;

    std::uint64_t connectionSpeed = 0;   // 480000000 on the measured iPhone 16
    std::uint32_t productId = 0;
    std::uint64_t locationId = 0;

    // ⛔ The §4.2 filter, in one place.  Everything else in this program that
    // wants to know "is this a cable?" asks here.
    bool isWired() const { return connectionType == "USB"; }
};

// ── Where the daemon lives ─────────────────────────────────────────────────
// Structured so the Windows (AF_INET 127.0.0.1:27015, Apple Mobile Device
// Service) and Linux (AF_UNIX, same path) providers of Phase 2 are a value
// change and not a code change.  Only the Unix branch is implemented today;
// asking for Tcp returns Status::NoProvider with a plain reason.
struct Provider {
    enum class Kind { Unix, Tcp };

    Kind kind = Kind::Unix;
    std::string path = "/var/run/usbmuxd";   // Kind::Unix
    std::string host = "127.0.0.1";          // Kind::Tcp  (Phase 2, Windows)
    std::uint16_t port = 27015;              // Kind::Tcp  (Phase 2, Windows)

    // The platform default.  macOS and Linux: the AF_UNIX socket above, which
    // on macOS is part of the OS and verified present (srw-rw-rw-).
    static Provider platformDefault();

    // A named AF_UNIX socket — the stub usbmuxd of the test suite, and nothing
    // else in shipping code.
    static Provider unixSocket(std::string socketPath);
};

// ── The blocking half: enumerate, and dial ─────────────────────────────────
//
// A Client holds no connection: every call opens its own socket to the daemon,
// because `Listen` and `Connect` each CONSUME the socket they are sent on —
// after `Number=0` it is the tunnel and carries no more plist (§4.2).  Device
// presence therefore needs its own long-lived connection (that is `Watch`),
// separate from every dial.
class Client {
public:
    explicit Client(Provider provider = Provider::platformDefault());

    const Provider &provider() const { return m_provider; }

    // Every attached device, INCLUDING ConnectionType == "Network" ones, so
    // that the caller (or the log) can tell "nothing plugged in" from "a
    // WiFi-paired phone the daemon also lists".  `out` is cleared first.
    //
    // Status is Ok only when at least one WIRED device came back; an all-Network
    // list is NoWiredDevices and an empty list is NoDevices, which are two
    // different rows of the §6.2 table.  ⚠ `out` is still filled in either case.
    //
    // ⛔ BLOCKING.  Worker thread only.
    Result listDevices(std::vector<Device> &out, int timeoutMs = 2000);

    // usbmux `Connect`.  On success returns a CONNECTED, NON-BLOCKING fd that
    // IS the tunnel — the caller owns it and closes it — which is exactly the
    // shape ConnectorConfig::dial must return (contract C1).  Socket hygiene is
    // set here and not by the caller: O_NONBLOCK, and SO_NOSIGPIPE where the
    // platform has it.
    //
    // Returns Ppcp::kInvalidSocket on any failure, having closed anything it
    // opened, with `diag` filled in.  Never throws.
    //
    // ⛔ `deviceId` MUST come from the CURRENT attachment — a fresh
    // listDevices() or a Watch Attached event — never from a remembered record.
    //
    // ⛔ `port` is passed in HOST order and byte-swapped on the way out; see the
    // note on kWiredPresencePort's use in the .cpp.  Callers pass 50915, not
    // htons(50915).
    //
    // ⛔ BLOCKING.  Worker thread only.
    pp_socket_t dial(DeviceId deviceId, std::uint16_t port,
                     Result *diag = nullptr, int timeoutMs = 5000);

private:
    Provider m_provider;
};

// ── The non-blocking half: presence ────────────────────────────────────────
//
// A `Listen` connection, held open forever, over which usbmuxd sends `Attached`
// for every device already present and then every attach and detach as they
// happen.  The fd is exposed precisely so the caller can put a QSocketNotifier
// on it: this class is Qt-free and starts no thread (design §6.3).
class Watch {
public:
    enum class EventKind { Attached, Detached };

    struct Event {
        EventKind kind = EventKind::Attached;

        // Always set.  ⛔ Attachment-scoped — see DeviceId.
        DeviceId deviceId = 0;

        // Fully populated for Attached.  ⚠ For Detached usbmuxd sends ONLY the
        // DeviceID, so `device.serialNumber` is EMPTY on a detach: the caller
        // must have remembered the udid it saw at attach if it needs to name
        // the phone that left.
        Device device;
    };

    Watch();
    ~Watch();
    Watch(const Watch &) = delete;
    Watch &operator=(const Watch &) = delete;

    // Opens the connection, sends `Listen`, and consumes exactly the
    // `Result { Number: 0 }` reply — no more, so no attach event is lost
    // between start() and the first poll().  ⛔ BLOCKING (briefly); call it
    // from wherever you would call listDevices().
    Result start(Provider provider = Provider::platformDefault(), int timeoutMs = 2000);

    bool active() const;

    // Ppcp::kInvalidSocket when not active.  ⛔ The caller may watch this fd and
    // must NOT close it, read from it, or write to it.
    pp_socket_t fd() const;

    // Drains whatever attach/detach events are pending, without blocking.
    // Appends to `out`; does not clear it.  Returns false when the connection
    // has ended (EOF, error, or a protocol violation) — the Watch is then
    // inactive-but-for-the-fd and the caller should drop its notifier and call
    // stop().  ⚠ A return of true with an empty `out` is ordinary: usbmuxd also
    // sends `Paired` messages, which are not our business.
    bool poll(std::vector<Event> &out);

    // Idempotent.  ⛔ Destroy any QSocketNotifier on fd() BEFORE calling this.
    void stop();

    // The reason poll() last returned false, for the one log line.
    const Result &lastError() const { return m_lastError; }

private:
    pp_socket_t m_fd = kInvalidSocket;
    std::vector<unsigned char> m_buf;   // partial message across poll() calls
    Result m_lastError;
};

// Convenience: the wired subset of a listDevices() answer, in daemon order.
// ⛔ The one filter in §4.2; use it rather than open-coding the comparison.
std::vector<Device> wiredOnly(const std::vector<Device> &all);

}  // namespace Usbmux
}  // namespace Ppcp
