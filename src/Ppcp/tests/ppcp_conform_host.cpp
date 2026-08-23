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

// ── THE PEER UNDER TEST, HEADLESS.  Work package H8. ────────────────────────
//
// `PPCP-CONF` §2c: an implementation tested only by its own unit tests is in
// the single-implementation trap, so the conformance instrument drives it from
// OUTSIDE, through its real transport, with `tools/ppcp-sim` as the
// counterpart.  This executable is the peer that instrument dials.
//
// ⚠ IT IS NOT A REIMPLEMENTATION OF THE HOST, AND THAT IS THE WHOLE POINT.
// Everything below the `main()` is the application's own code, reached through
// the same entry points `PpcpHostService` uses:
//
//   - `Ppcp::PpcpHostPeer`            — H2, the pump and the readiness answer
//   - `Ppcp::makeHostEngine()`        — the ONE place that says what a
//                                       PinPointStudio peer is (H3's A10 rule)
//   - `Ppcp::PpcpIngestPolicy`        — the real 120 fps floor, as a callback
//   - `Ppcp::PpcpSourceDeclaration`   — the real I19 declaration builder
//   - `Ppcp::PpcpLiveSession`         — H5's §6.3 sync and §7.4 heartbeats
//   - `Ppcp::PpcpShotBridge`          — H5's arbiter, 8.2
//   - `Ppcp::PpcpAnnotationStore`     — H7's markup
//   - `PpcpOfferController`           — H5's MSG 9.1/9.2 offer list
//   - `Ppcp::Listener`                — H1's transport
//
// WHAT IT DOES NOT USE, AND WHY EACH IS HONEST TO LEAVE OUT:
//
//   - `PpcpHostService` itself.  It is a `QObject` that reaches
//     `VideoInputFactory`, and that translation unit pulls in the AVFoundation
//     camera backend and `DeviceEnumerator` (which reaches Bluetooth through
//     `imu_base.h`).  Linking the application's device stack into a
//     conformance harness would make the run depend on what hardware is
//     plugged into the machine, which is the opposite of reproducible.  The
//     composition it performs — construct, declare, build the engine, listen,
//     accept, attach, pump, tick — is performed here line for line, and the
//     `ppcp_app_tu_syntax` row is what keeps the service itself compiling.
//   - `PpcpSourceDeclaration::hostInventory()`, for the same reason: it reads
//     `DeviceEnumerator`, so the declaration would differ between two machines
//     and a conformance claim that is not reproducible is not evidence.  The
//     inventory below is fixed, and it goes through the REAL `build()` and the
//     REAL `validate()` — I19, I22, I27, I28 and I31 are checked by libppcp
//     exactly as they are in the application.
//   - `PpcpRendezvous`.  There is no pairing code here: the transport is
//     plaintext (see below), so no identity is offered and none is resolved.
//
// ⚠ THE SOCKET IS PLAINTEXT AND THIS BINARY MUST NEVER SHIP.  `ppcp-sim` has
// no TLS *transport* — its `--psk-ke-only` mode is a hand-built ClientHello for
// RT-4 and speaks no application data — so the instrument cannot reach a
// TLS-only host at all.  `PPCP-RV` erratum E4 (RV 2c1) settles that: 2c
// constrains the rendezvous PATHS, and a conformance harness socket is not one
// of them; `PPCP-CORE` §3.2's `direct` transport is conformant plaintext.  The
// listener side of that lives behind `PP_PPCP_PLAINTEXT_HARNESS`, a CMake
// option that is OFF by default and set by `src/Ppcp/tests` and nothing else,
// so the application build has no plaintext code path to reach.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <QCoreApplication>

#include "ppcp_host_engine.h"
#include "ppcp_host_peer.h"
#include "ppcp_import_ledger.h"
#include "ppcp_offer_controller.h"
#include "ppcp_source_declaration.h"
#include "ppcp_transport.h"

using namespace Ppcp;

namespace {

void say(const std::string &line)
{
    std::fprintf(stderr, "[pps-host] %s\n", line.c_str());
    std::fflush(stderr);
}

// 8.3e — ids SHOULD be UUIDs, and the library has no random source (ground
// rule 8) so the embedding mints them.  A real v4, because a counter would be
// a namespace collision waiting for a second host.
std::string uuid4()
{
    static std::mt19937_64 rng{ std::random_device{}() };
    std::uniform_int_distribution<std::uint64_t> d;
    const std::uint64_t a = d(rng), b = d(rng);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-4%03x-%04x-%012llx",
                  static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xffff),
                  static_cast<unsigned>(a & 0x0fff),
                  static_cast<unsigned>(((b >> 48) & 0x3fff) | 0x8000),
                  static_cast<unsigned long long>(b & 0xffffffffffffull));
    return buf;
}

// The inventory this host declares.  FIXED, so the declaration is the same on
// every machine — see the note at the top.  A camera AND a microphone, because
// CT-I8's second half ("two Candidates of the same `basis` from DIFFERENT
// peers") is unreachable without a host Source that nominates, and 3.3d's
// symmetric declaration is unreachable without one that does not.
PpcpSourceDeclaration::Inventory harnessInventory()
{
    PpcpSourceDeclaration::Inventory inv;

    PpcpSourceDeclaration::Camera c;
    c.backend = VideoInputFactory::Backend::Aravis;
    c.id = "pps-conform-cam-0";
    c.label = "Down the line";
    c.caps.vendorName = "Basler";
    c.caps.modelName = "acA1300-200uc";
    c.caps.serialNumber = "22334455";
    c.caps.connectionInterface = CameraCapabilities::Interface::USB3;
    c.caps.resolution.kind = CapabilityKind::Range;
    c.caps.resolution.widthRange = { 64, 1280, 4, 1280 };
    c.caps.resolution.heightRange = { 64, 1024, 2, 1024 };
    c.caps.resolution.defaultResolution = { 1280, 1024 };
    c.caps.pixelFormat.kind = CapabilityKind::Discrete;
    PixelFormat mono;
    mono.nativeKey = "Mono8";
    mono.encoding = PixelEncoding::Mono8;
    mono.bitsPerPixel = 8;
    c.caps.pixelFormat.supported = { mono };
    c.caps.pixelFormat.defaultFormat = mono;
    c.caps.frameRate.kind = CapabilityKind::Range;
    c.caps.frameRate.range = { 20.0, 200.0, 0.0, 200.0 };
    c.caps.frameRate.readable = true;
    c.caps.gain.kind = CapabilityKind::Range;
    c.caps.gain.range = { 0.0, 24.0, 0.1, 0.0 };
    c.caps.exposureTime.kind = CapabilityKind::Range;
    c.caps.exposureTime.range = { 20.0, 10000.0, 1.0, 500.0 };
    inv.cameras.push_back(std::move(c));

    inv.hasMicrophone = true;
    inv.microphone.id = "pps-conform-mic-0";
    inv.microphone.label = "bay microphone";
    return inv;
}

struct HarnessOptions {
    std::uint16_t port = 0;
    std::string   portFile;
    int           runMs = 120000;
    std::string   peerId = "peer:pinpointstudio-conform";
};

bool parse(int argc, char **argv, HarnessOptions &o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--port") { const char *v = next(); if (!v) return false;
                             o.port = static_cast<std::uint16_t>(std::atoi(v)); }
        else if (a == "--port-file") { const char *v = next(); if (!v) return false; o.portFile = v; }
        else if (a == "--run-ms")    { const char *v = next(); if (!v) return false; o.runMs = std::atoi(v); }
        else if (a == "--peer-id")   { const char *v = next(); if (!v) return false; o.peerId = v; }
        else if (a == "--help") {
            std::printf("ppcp_conform_host — the PinPointStudio host, headless, on a PLAINTEXT\n"
                        "harness socket (PPCP-CONF 2c, PPCP-RV erratum E4).\n\n"
                        "  --port N        listen port; 0 takes an ephemeral one\n"
                        "  --port-file P   write the bound port to P (for a driver script)\n"
                        "  --run-ms MS     stop after this long (default 120000)\n"
                        "  --peer-id ID    CORE 5.1a Peer.id to declare\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

// ── The session this host runs, per link ────────────────────────────────────
//
// `reference-host` in `tools/scenarios/README.md` is the shape the instrument
// expects: "opens the Session, syncs per declared timebase, arms, arbitrates,
// accepts an offered Session".  The first, third and fourth are decisions no
// library can make for an embedding, so they are here.
//
// ⚠ THE APPLICATION DOES NOT YET MAKE THEM.  Nothing in `src/` calls
// `liveSession().open()` — §7.2 of docs/ppcp-conformance.md has said so since
// H5 — so these three calls are the HARNESS's, and every row that depends on a
// Session is evidence about `PpcpLiveSession` rather than about a screen.  That
// distinction is recorded in the claim rather than smoothed over.
class Harness {
public:
    explicit Harness(const HarnessOptions &o)
        : m_opt(o), m_peer([&] {
              PpcpHostPeer::Config c;
              c.peerId = o.peerId;
              return c;
          }())
    {
        // ⚠ A HEALTH SOURCE IS A PRECONDITION FOR LIVENESS, NOT A DECORATION
        // (finding F-H5-3).  Without one every `heartbeat` is answered
        // `error`/`profile_not_supported`, no ack ever returns, and §7.4
        // silently never runs.  The application reads QStorageInfo; this reads
        // a fixed, generous figure, because a harness whose §7.4 rows depended
        // on how full the build machine's disk was would be measuring the disk.
        m_peer.setStorage([](std::uint64_t *freeBytes) {
            if (!freeBytes) return false;
            *freeBytes = 64ull * 1024 * 1024 * 1024;
            return true;
        });

        m_peer.setDeclarationHook([this](const ppcp_peer_desc *d) { onCounterpartDeclared(d); });
        m_peer.addEventHook([this](const ppcp_event &ev) { m_offers.observe(ev); });
        m_offers.setLedger(&m_ledger);
    }

    bool start(std::string *err)
    {
        // MSG 3.3c — declare BEFORE anything that references a Source.
        std::string derr;
        if (!m_peer.declareSelf(harnessInventory(), &derr)) {
            *err = "could not declare: " + derr;
            return false;
        }
        m_engine = m_peer.makeLibppcpEngine(&derr);
        if (!m_engine) {
            *err = "could not build the engine: " + derr;
            return false;
        }

        m_listener.setPlaintextHarness(true);
        m_listener.setChannelsPerPeer(2);
        m_listener.setLog([](const std::string &l) { say(l); });
        std::string lerr;
        if (!m_listener.listen(m_opt.port, &lerr)) {
            *err = "could not listen: " + lerr;
            return false;
        }
        m_port = m_listener.port();
        return true;
    }

    std::uint16_t port() const { return m_port; }

    // One iteration of the application's loop.  `PpcpHostService` runs the same
    // two schedules off a 20 ms QTimer; this runs them off a sleep, because
    // there is no event loop to hang a timer on and ground rule 7 says the
    // thread is the embedding's to supply.
    void step()
    {
        if (!m_link) {
            HandshakeFailure fail;
            std::unique_ptr<PeerConnection> link = m_listener.accept(20, &fail);
            if (link) adopt(std::move(link));
            return;
        }

        // ENC 2.1d — a bulk channel MAY be opened at any later point with the
        // same link_id, and `preview` after the Session is up is the expected
        // case (CT-I36a).  Polled rather than waited on: a zero here would busy
        // spin, and a long one would starve the pump.
        HandshakeFailure ignored;
        if (m_link->channels().size() < 3) m_listener.acceptInto(*m_link, 1, &ignored);

        if (!m_peer.tick(hostNowNs())) {
            drop("link closed");
            return;
        }
        autoAcceptOffers();
    }

private:
    void adopt(std::unique_ptr<PeerConnection> link)
    {
        m_link = std::move(link);
        m_peer.attach(m_link.get(), m_engine.get());
        m_offers.attach(m_engine->peer(), QString());
        m_sessionOpen = false;
        say("link up — " + m_link->tls().describe());
    }

    void drop(const char *why)
    {
        if (!m_link) return;
        m_offers.detach();
        m_peer.shotBridge().stop();
        m_peer.attach(nullptr, nullptr);
        m_link->close();
        m_link.reset();
        m_sessionOpen = false;
        say(std::string("link down (") + why + ")");
    }

    // MSG 3.3 — the counterpart declared, so everything that references its
    // Sources becomes possible NOW and at no earlier moment.  This is where the
    // `reference-host` scenario's "opens the Session, arms, arbitrates" goes.
    void onCounterpartDeclared(const ppcp_peer_desc *desc)
    {
        if (!desc || m_sessionOpen || !m_engine) return;
        m_sessionOpen = true;

        const std::string counterpart = idStr(desc->id);
        m_offers.attach(m_engine->peer(), QString::fromStdString(counterpart));
        say("counterpart declared: " + counterpart);

        PpcpLiveSession::Config cfg;
        cfg.sessionId = "sess:" + uuid4();
        std::string err;
        if (!m_peer.liveSession().open(cfg, &err)) {
            say("session_open refused: " + err);
            return;
        }

        // I20 — libppcp refuses to build an arbiter for a peer that is not
        // `role: host` and does not declare Arbitrate, and this reports the
        // refusal rather than falling back to something that is not arbitration.
        PpcpShotBridge::Config bc;
        bc.peerId = m_opt.peerId;
        if (!m_peer.shotBridge().start(bc, [](std::string *out) { *out = uuid4(); return true; },
                                       &err))
            say("the arbiter would not start: " + err);

        if (!m_peer.liveSession().arm({}, &err)) say("arm refused: " + err);
    }

    // MSG 9.2 — a headless host has to decide, and "accept" is the decision
    // that exercises the path.  In the application this is a row the user taps;
    // the CONTROLLER is the protocol obligation and the delegate is
    // presentation, which is why the offer suite of H5 is written over the
    // model and why the harness can drive the same object.
    void autoAcceptOffers()
    {
        const int n = m_offers.rowCount();
        for (int r = 0; r < n; ++r) {
            const QModelIndex ix = m_offers.index(r, 0);
            if (m_offers.data(ix, PpcpOfferController::AcceptedRole).toBool()) continue;
            if (m_offers.acceptOffer(r))
                say("accepted an offered Session (MSG 9.2)");
        }
    }

    static std::string idStr(const ppcp_id &id)
    {
        return std::string(id.v, id.len);
    }

    HarnessOptions                    m_opt;
    PpcpHostPeer                      m_peer;
    Ppcp::PpcpImportLedger            m_ledger;
    PpcpOfferController               m_offers;
    Listener                          m_listener;
    std::unique_ptr<PpcpEngine>       m_engine;
    std::unique_ptr<PeerConnection>   m_link;
    std::uint16_t                     m_port = 0;
    bool                              m_sessionOpen = false;
};

}  // namespace

int main(int argc, char **argv)
{
    // QCoreApplication, because `PpcpOfferController` formats its row labels
    // through QLocale and QDateTime and neither works without one.  No event
    // loop is entered: the pump below IS the loop.
    QCoreApplication app(argc, argv);

    HarnessOptions opt;
    if (!parse(argc, argv, opt)) return 2;

    Harness h(opt);
    std::string err;
    if (!h.start(&err)) {
        say("FATAL: " + err);
        return 1;
    }
    say("listening on 127.0.0.1:" + std::to_string(h.port()) + " (PLAINTEXT harness socket)");
    if (!opt.portFile.empty()) {
        // Written last and atomically-enough for a driver script: the port is
        // the handshake between this process and `ppcp-conform`.
        const std::string tmp = opt.portFile + ".tmp";
        { std::ofstream f(tmp); f << h.port() << "\n"; }
        std::rename(tmp.c_str(), opt.portFile.c_str());
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt.runMs);
    while (std::chrono::steady_clock::now() < deadline) {
        h.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    say("run-ms elapsed; stopping");
    return 0;
}
