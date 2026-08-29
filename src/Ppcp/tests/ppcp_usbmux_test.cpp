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

// The usbmux client of the wired PPCP transport, driven end to end against a
// stub usbmuxd — no phone, no cable, no daemon.
//
// Design §12 names "both halves of the wired path are unreached code" as the
// largest single estimate risk in Phase 1.  These rows are what makes the host
// half reached.  Each names the clause or the measurement it stands for:
//
//   §4.2 header    length INCLUSIVE of 16 bytes, version 1, message 8, tag echoed
//   §4.2 ⛔ port    PortNumber goes on the wire BIG-ENDIAN, and the wrong order
//                   does not error — it connects to a DIFFERENT port
//   §4.2 ⛔ filter  ConnectionType == "Network" is a WiFi pairing and is NOT wired
//   §4.2 consume   `Connect` and `Listen` each turn the socket into something
//                   that carries no more plist
//   §6.2 table     every row the mux layer can see, distinguishable
//   29 Aug measure DeviceID is per-attachment; SerialNumber is the identity

#include "ppcp_usbmux.h"
#include "usbmuxd_stub.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Ppcp;
using Ppcp::Usbmux::Client;
using Ppcp::Usbmux::Device;
using Ppcp::Usbmux::Status;
using Ppcp::Usbmux::Watch;

namespace {

// The UDID measured on the build Mac, 29 Aug 2026.  It is a device serial, not
// a secret, and it is here so the fixture looks like the thing it stands for.
constexpr const char *kUdid = "00008140-000864E426EB001C";

UsbmuxStub::Entry usbDevice(std::uint32_t id, const char *udid = kUdid)
{
    UsbmuxStub::Entry e;
    e.deviceId = id;
    e.udid = udid;
    e.connectionType = "USB";
    e.connectionSpeed = 480000000;   // measured on the iPhone 16
    return e;
}

UsbmuxStub::Entry networkDevice(std::uint32_t id, const char *udid)
{
    UsbmuxStub::Entry e = usbDevice(id, udid);
    e.connectionType = "Network";
    e.connectionSpeed = 0;
    return e;
}

// Drains the watch until `want` events have arrived or the deadline passes.
// A notifier would drive poll() on readability; a test drives it on a clock.
bool drain(Watch &w, std::vector<Watch::Event> &out, std::size_t want, int timeoutMs = 3000)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < want && std::chrono::steady_clock::now() < until) {
        if (!w.poll(out)) return false;
        if (out.size() >= want) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return out.size() >= want;
}

}  // namespace

// ── §4.2 — the header, byte for byte ───────────────────────────────────────
// `length` is INCLUSIVE of the 16-byte header.  Getting that wrong is a hang,
// not an error, because the daemon waits for bytes that never come.
TEST(PpcpUsbmuxFraming, HeaderIsLengthInclusiveVersionOneMessageEightTagEchoed)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok()) << "the stub could not bind " << mux.path();
    mux.setDevices({usbDevice(308)});

    Client c(mux.provider());
    std::vector<Device> devices;
    const auto r = c.listDevices(devices);
    ASSERT_TRUE(r.ok()) << r.message();

    const UsbmuxStub::RequestRecord req = mux.lastRequest();
    ASSERT_TRUE(req.seen);
    EXPECT_EQ(req.length, 16u + req.bodyBytes) << "length must INCLUDE the 16-byte header";
    EXPECT_EQ(req.version, 1u);
    EXPECT_EQ(req.message, 8u);   // 8 = plist
    EXPECT_NE(req.tag, 0u) << "a tag of 0 cannot be matched against a reply";
    EXPECT_EQ(req.messageType, "ListDevices");

    // §4.2's four client keys.  usbmuxd answers Number=5 to a client that omits
    // the version, which is a failure nobody would guess from the symptom.
    EXPECT_NE(req.body.find("<key>kLibUSBMuxVersion</key><integer>3</integer>"),
              std::string::npos);
    EXPECT_NE(req.body.find("<key>ProgName</key>"), std::string::npos);
    EXPECT_NE(req.body.find("<key>ClientVersionString</key>"), std::string::npos);
}

// A daemon whose reply does not echo the tag is one we are not in step with,
// and reading its answer would be guessing.
TEST(PpcpUsbmuxFraming, AReplyThatDoesNotEchoTheTagIsRefused)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setDevices({usbDevice(308)});
    mux.setEchoTag(false);

    Client c(mux.provider());
    std::vector<Device> devices;
    const auto r = c.listDevices(devices);
    EXPECT_EQ(r.status, Status::ProviderProtocol) << r.message();
}

// ── §4.2 ⛔ — the filter that keeps a timing claim honest ───────────────────
TEST(PpcpUsbmuxListDevices, ANetworkConnectionTypeIsFilteredOutOfTheWiredSet)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setDevices({usbDevice(308, kUdid),
                    networkDevice(309, "00008140-DEADBEEFDEADBEEF")});

    Client c(mux.provider());
    std::vector<Device> devices;
    const auto r = c.listDevices(devices);
    ASSERT_TRUE(r.ok()) << r.message();

    // BOTH are reported — the caller has to be able to say "a phone is here but
    // it is on WiFi" in the log — and exactly one is wired.
    ASSERT_EQ(devices.size(), 2u);
    const std::vector<Device> wired = Ppcp::Usbmux::wiredOnly(devices);
    ASSERT_EQ(wired.size(), 1u)
        << "a ConnectionType == \"Network\" device was treated as wired; that would "
           "label a WiFi link as wired and silently invalidate every timing claim "
           "this work exists to make (§4.2)";
    EXPECT_EQ(wired[0].deviceId, 308u);
    EXPECT_EQ(wired[0].serialNumber, kUdid);
    EXPECT_EQ(wired[0].connectionType, "USB");
    EXPECT_EQ(wired[0].connectionSpeed, 480000000u);
    EXPECT_TRUE(wired[0].isWired());
    EXPECT_FALSE(devices[1].isWired());
}

// ── §6.2 — the rows this layer can see, each distinguishable ───────────────
TEST(PpcpUsbmuxDiagnostics, NoProviderIsDistinctFromNothingAttachedAndFromWiFiOnly)
{
    // Row 1: cannot open the provider socket.  ⛔ Not an error state (RV 3.6a)
    // — a status, not a banner.
    {
        Client c(Ppcp::Usbmux::Provider::unixSocket("/tmp/ppcp-muxstub-does-not-exist.sock"));
        std::vector<Device> devices;
        const auto r = c.listDevices(devices);
        EXPECT_EQ(r.status, Status::NoProvider) << r.message();
        EXPECT_TRUE(devices.empty());
        EXPECT_NE(r.systemError, 0) << "the errno is what tells a Linux user it is permissions";
    }

    // Row 2: the daemon is there and nothing is plugged in — or the cable is
    // charge-only.  usbmux cannot tell those apart, and neither do we.
    {
        UsbmuxStub::Daemon mux;
        ASSERT_TRUE(mux.ok());
        mux.setDevices({});
        Client c(mux.provider());
        std::vector<Device> devices;
        const auto r = c.listDevices(devices);
        EXPECT_EQ(r.status, Status::NoDevices) << r.message();
    }

    // Row 3: a device IS attached, over WiFi.
    {
        UsbmuxStub::Daemon mux;
        ASSERT_TRUE(mux.ok());
        mux.setDevices({networkDevice(309, kUdid)});
        Client c(mux.provider());
        std::vector<Device> devices;
        const auto r = c.listDevices(devices);
        EXPECT_EQ(r.status, Status::NoWiredDevices) << r.message();
        EXPECT_EQ(devices.size(), 1u) << "the device is still reported, just not as wired";
    }

    // And the three say three different things to whoever reads the log.
    EXPECT_STRNE(Ppcp::Usbmux::describe(Status::NoProvider),
                 Ppcp::Usbmux::describe(Status::NoDevices));
    EXPECT_STRNE(Ppcp::Usbmux::describe(Status::NoDevices),
                 Ppcp::Usbmux::describe(Status::NoWiredDevices));
}

// ── ⛔⛔ THE ONE THAT COSTS A DAY ───────────────────────────────────────────
//
// `PortNumber` goes on the wire BIG-ENDIAN.  Measured against Apple's daemon on
// 29 Aug 2026, twice:  htons(62078) -> Number=0;  62078 native -> Number=3,
// because it dialled 32498.  ⚠ The failure mode is the danger — the wrong byte
// order does not error, it connects to a DIFFERENT PORT, and on a busy device
// that can succeed against something else entirely.
//
// This test is the thing that stops that recurring.
TEST(PpcpUsbmuxConnect, PortNumberIsBigEndianOnTheWire)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setDevices({usbDevice(308)});
    mux.setConnectResult(0);

    Client c(mux.provider());
    Ppcp::Usbmux::Result diag;
    const pp_socket_t s = c.dial(308, Ppcp::Usbmux::kWiredPresencePort, &diag);
    ASSERT_NE(s, kInvalidSocket) << diag.message();
    ::close(s);

    const UsbmuxStub::RequestRecord req = mux.lastConnect();
    ASSERT_TRUE(req.seen);
    EXPECT_EQ(req.messageType, "Connect");
    EXPECT_EQ(req.deviceId, 308);

    // The semantic assertion: read the wire value as network order and you get
    // the port the caller asked for.
    ASSERT_GE(req.portNumberOnWire, 0);
    EXPECT_EQ(ntohs(static_cast<std::uint16_t>(req.portNumberOnWire)),
              Ppcp::Usbmux::kWiredPresencePort);

    // And the mechanical one, which is what actually catches a regression on the
    // little-endian machines this ships on: the value is NOT the host-order port.
    if (htons(Ppcp::Usbmux::kWiredPresencePort) != Ppcp::Usbmux::kWiredPresencePort) {
        EXPECT_NE(req.portNumberOnWire, static_cast<long long>(Ppcp::Usbmux::kWiredPresencePort))
            << "the host-order port reached the wire — usbmuxd would ntohs() it and dial "
            << ntohs(Ppcp::Usbmux::kWiredPresencePort) << " instead, WITHOUT erroring";
        EXPECT_EQ(req.portNumberOnWire, static_cast<long long>(
                                            htons(Ppcp::Usbmux::kWiredPresencePort)));
    }
}

// Contract C4 — the constant itself, so a silent edit on one side of the two
// repos is a test failure and not a mystery.
TEST(PpcpUsbmuxConnect, ThePresencePortIsFiftyThousandNineHundredAndFifteen)
{
    EXPECT_EQ(Ppcp::Usbmux::kWiredPresencePort, 50915);
}

// ── §6.2 — every Connect result maps to its own diagnosis ──────────────────
TEST(PpcpUsbmuxConnect, ResultNumbersMapToDistinctDiagnoses)
{
    struct Row { int number; Status expected; };
    const Row rows[] = {
        {2, Status::UnknownDevice},      // bad device — vanished, or Restricted Mode
        {3, Status::ConnectRefused},     // VERIFIED live: nothing listening in the device
        {5, Status::BadVersion},         // the daemon refused kLibUSBMuxVersion
        {7, Status::UnexpectedResult},   // something we have no name for, reported verbatim
    };

    std::vector<Status> seen;
    for (const Row &row : rows) {
        UsbmuxStub::Daemon mux;
        ASSERT_TRUE(mux.ok());
        mux.setDevices({usbDevice(308)});
        mux.setConnectResult(row.number);

        Client c(mux.provider());
        Ppcp::Usbmux::Result diag;
        const pp_socket_t s = c.dial(308, Ppcp::Usbmux::kWiredPresencePort, &diag);

        EXPECT_EQ(s, kInvalidSocket) << "Number=" << row.number << " must not yield a tunnel";
        EXPECT_EQ(diag.status, row.expected) << diag.message();
        EXPECT_EQ(diag.muxResult, row.number) << "the raw Number is carried through verbatim";
        EXPECT_NE(diag.message().find("Number=" + std::to_string(row.number)), std::string::npos);
        seen.push_back(diag.status);
    }

    // Distinguishable, which is the acceptance criterion of §6.2 — not merely
    // "all four failed".
    for (std::size_t i = 0; i < seen.size(); ++i)
        for (std::size_t j = i + 1; j < seen.size(); ++j)
            EXPECT_NE(seen[i], seen[j]);
}

// ── §4.2 — after Number=0 the socket IS the tunnel ─────────────────────────
// And it comes back with the hygiene contract C1 requires of it, because the
// Connector applies none of its own options to a socket it did not dial.
TEST(PpcpUsbmuxConnect, ASuccessfulConnectYieldsANonBlockingTunnel)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setDevices({usbDevice(308)});
    mux.setConnectResult(0);

    Client c(mux.provider());
    Ppcp::Usbmux::Result diag;
    const pp_socket_t s = c.dial(308, Ppcp::Usbmux::kWiredPresencePort, &diag);
    ASSERT_NE(s, kInvalidSocket) << diag.message();
    EXPECT_EQ(diag.muxResult, 0);

    const int flags = ::fcntl(s, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_TRUE(flags & O_NONBLOCK)
        << "contract C1: the fd handed to ConnectorConfig::dial MUST be non-blocking";

#ifdef SO_NOSIGPIPE
    int nosig = 0;
    socklen_t len = sizeof nosig;
    ASSERT_EQ(::getsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &nosig, &len), 0);
    EXPECT_NE(nosig, 0) << "a peer walking away mid-session must not SIGPIPE the app";
#endif

    // The stub echoes on the tunnel: bytes cross, so this is a live connection
    // and not merely a number that survived a switch statement.
    const char ping[] = "ppcp";
    ASSERT_EQ(::send(s, ping, sizeof ping, 0), static_cast<ssize_t>(sizeof ping));
    char back[sizeof ping] = {};
    std::size_t got = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (got < sizeof ping && std::chrono::steady_clock::now() < until) {
        struct pollfd p{};
        p.fd = s;
        p.events = POLLIN;
        if (::poll(&p, 1, 100) > 0) {
            const ssize_t n = ::recv(s, back + got, sizeof back - got, 0);
            if (n > 0) got += static_cast<std::size_t>(n);
        }
    }
    EXPECT_EQ(got, sizeof ping);
    EXPECT_STREQ(back, "ppcp");
    ::close(s);
}

// A dial to a provider that is not there closes everything it opened and says
// so — never throws, never leaves an fd behind (contract C1).
TEST(PpcpUsbmuxConnect, NoProviderIsACleanRefusalNotAnException)
{
    Client c(Ppcp::Usbmux::Provider::unixSocket("/tmp/ppcp-muxstub-absent.sock"));
    Ppcp::Usbmux::Result diag;
    pp_socket_t s = kInvalidSocket;
    ASSERT_NO_THROW(s = c.dial(308, Ppcp::Usbmux::kWiredPresencePort, &diag));
    EXPECT_EQ(s, kInvalidSocket);
    EXPECT_EQ(diag.status, Status::NoProvider) << diag.message();
}

// ── §6.3 — the watch is a readable fd, and nothing else ────────────────────
TEST(PpcpUsbmuxWatch, ListenDeliversAttachThenDetachOffANonBlockingFd)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setDevices({});           // nothing attached when the watch starts
    mux.setAnnounceOnListen(false);

    Watch w;
    const auto r = w.start(mux.provider());
    ASSERT_TRUE(r.ok()) << r.message();
    ASSERT_TRUE(w.active());
    ASSERT_NE(w.fd(), kInvalidSocket) << "§6.3: the fd is exposed so a QSocketNotifier can "
                                         "watch it — that is why this needs no thread";

    const int flags = ::fcntl(w.fd(), F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_TRUE(flags & O_NONBLOCK) << "poll() must never block the GUI thread";

    // Nothing pending is ordinary, and it is not an error.
    std::vector<Watch::Event> events;
    EXPECT_TRUE(w.poll(events));
    EXPECT_TRUE(events.empty());

    ASSERT_TRUE(mux.waitForListener(3000));
    mux.pushAttached(usbDevice(308));
    ASSERT_TRUE(drain(w, events, 1)) << "no Attached arrived: " << w.lastError().message();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, Watch::EventKind::Attached);
    EXPECT_EQ(events[0].deviceId, 308u);
    EXPECT_EQ(events[0].device.serialNumber, kUdid);
    EXPECT_TRUE(events[0].device.isWired());

    // `Paired` is a real message that is none of our business.
    mux.pushPaired(308);
    mux.pushDetached(308);
    ASSERT_TRUE(drain(w, events, 2)) << "no Detached arrived: " << w.lastError().message();
    ASSERT_EQ(events.size(), 2u) << "a `Paired` message was reported as an event";
    EXPECT_EQ(events[1].kind, Watch::EventKind::Detached);
    EXPECT_EQ(events[1].deviceId, 308u);
    // ⚠ The daemon sends only the DeviceID on a detach.
    EXPECT_TRUE(events[1].device.serialNumber.empty());

    w.stop();
    EXPECT_FALSE(w.active());
    EXPECT_EQ(w.fd(), kInvalidSocket);
    w.stop();   // idempotent — the teardown order is the caller's to sequence
}

// `Listen` announces everything already attached before it goes quiet, and
// start() must not swallow those replies while reading its own Result.
TEST(PpcpUsbmuxWatch, DevicesAlreadyAttachedAtStartAreNotLost)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setDevices({usbDevice(308), networkDevice(309, "00008140-0000000000000001")});

    Watch w;
    ASSERT_TRUE(w.start(mux.provider()).ok());

    std::vector<Watch::Event> events;
    ASSERT_TRUE(drain(w, events, 2)) << w.lastError().message();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(events[0].device.isWired());
    EXPECT_FALSE(events[1].device.isWired())
        << "the watch reports what the daemon said; the USB filter is the caller's, "
           "and it must still be possible to SEE the Network device";
}

// ── 29 Aug 2026, measured — DeviceID is per-attachment ─────────────────────
// The same iPhone on the same cable was 306 in the morning and 308 in the
// afternoon.  This pins the property the header warns about, so that a future
// caching "optimisation" fails here rather than in the field.
TEST(PpcpUsbmuxWatch, DeviceIdChangesAcrossAReattachWhileTheUdidDoesNot)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setDevices({});
    mux.setAnnounceOnListen(false);

    Watch w;
    ASSERT_TRUE(w.start(mux.provider()).ok());
    ASSERT_TRUE(mux.waitForListener(3000));

    std::vector<Watch::Event> events;
    mux.pushAttached(usbDevice(306));
    ASSERT_TRUE(drain(w, events, 1)) << w.lastError().message();
    mux.pushDetached(306);
    ASSERT_TRUE(drain(w, events, 2)) << w.lastError().message();
    mux.pushAttached(usbDevice(308));   // same phone, same cable, new handle
    ASSERT_TRUE(drain(w, events, 3)) << w.lastError().message();

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].deviceId, 306u);
    EXPECT_EQ(events[2].deviceId, 308u);
    EXPECT_NE(events[0].deviceId, events[2].deviceId)
        << "⛔ a DeviceID is valid only inside one Attached..Detached span";
    EXPECT_EQ(events[0].device.serialNumber, events[2].device.serialNumber)
        << "the UDID is the stable identity — key on that, never on the DeviceID";
}

// A refused `Listen` is a diagnosis, not a half-open watch.
TEST(PpcpUsbmuxWatch, ARefusedListenLeavesNoWatchBehind)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setListenResult(5);   // bad version

    Watch w;
    const auto r = w.start(mux.provider());
    EXPECT_EQ(r.status, Status::BadVersion) << r.message();
    EXPECT_EQ(r.muxResult, 5);
    EXPECT_FALSE(w.active());
    EXPECT_EQ(w.fd(), kInvalidSocket);
}

// The provider going away under a live watch ends it cleanly: poll() says the
// connection is over, and the caller drops its notifier and stops.  ⛔ It is not
// an error state — a phone unplugged and a daemon restarted are ordinary.
TEST(PpcpUsbmuxWatch, TheWatchEndsCleanlyWhenTheProviderHangsUp)
{
    auto mux = std::make_unique<UsbmuxStub::Daemon>();
    ASSERT_TRUE(mux->ok());
    mux->setDevices({});

    Watch w;
    ASSERT_TRUE(w.start(mux->provider()).ok());
    ASSERT_TRUE(mux->waitForListener(3000));

    mux.reset();   // the daemon goes away

    std::vector<Watch::Event> events;
    bool alive = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (alive && std::chrono::steady_clock::now() < until) {
        alive = w.poll(events);
        if (alive) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(alive) << "poll() must report the end of the connection";
    w.stop();
}

// A watch is not a dial: `Listen` consumed its socket and the dial has to open
// its own (§4.2).  Both alive at once, on one daemon, is the shipping shape.
TEST(PpcpUsbmuxWatch, AWatchAndADialCoexistOnSeparateConnections)
{
    UsbmuxStub::Daemon mux;
    ASSERT_TRUE(mux.ok());
    mux.setDevices({usbDevice(308)});
    mux.setConnectResult(0);

    Watch w;
    ASSERT_TRUE(w.start(mux.provider()).ok());

    Client c(mux.provider());
    Ppcp::Usbmux::Result diag;
    const pp_socket_t s = c.dial(308, Ppcp::Usbmux::kWiredPresencePort, &diag);
    ASSERT_NE(s, kInvalidSocket) << diag.message();
    EXPECT_NE(s, w.fd());
    EXPECT_TRUE(w.active()) << "the dial must not have disturbed the Listen connection";

    std::vector<Watch::Event> events;
    EXPECT_TRUE(w.poll(events));
    ::close(s);
}
