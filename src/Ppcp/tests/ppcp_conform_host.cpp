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
// ── S5 — THE INTEROPERABILITY ROWS OF `PPCP-CONF` §5 ───────────────────────
//
// H8 drove this binary from `ppcp-conform`, which picks its own counterparts.
// `CONF` §5's pairings are named the other way round: a HOST of a stated shape
// against a PEER of a stated shape.  Four of those shapes are decisions no
// counterpart can make for this host, so they are options here and nowhere
// else in the application:
//
//   --never-issue        IOP-7.  The arbiter is BUILT and never PUMPED, which
//                        is exactly what `ppcp-sim`'s `silent-host` does
//                        (SIM_F_NEVER_ISSUE).  Retention and exclusion still
//                        run; only 8.2h's issue step does not.
//   --issue-delay-ms N   IOP-8.  8.2h runs only N ms after the link came up,
//                        so the device's 8.2i deadline fires first and 8.2k's
//                        attach-rather-than-issue is what is measured.  libppcp
//                        plan §9 records that the deadline is
//                        `issue_hold_ns + heartbeat_interval_ms` after the
//                        Candidate, and that a delay without a clear margin
//                        makes the row pass for the wrong reason.
//   --nominate-acoustic  IOP-6.  This host's OWN microphone Source nominates,
//                        through H5's `PpcpShotBridge::nominate()`, from a
//                        synthetic onset raised when the device's Candidate
//                        arrives.  Two nominators, one `basis: acoustic`, two
//                        peers — the I8 shape a per-modality slot loses.
//   --issue-hold-ms / --coincidence-ms / --heartbeat-ms
//                        the Session parameters of 5.10e, because IOP-8 needs
//                        the margin above stated rather than assumed.
//
// And two that are not about the wire at all:
//
//   --import-bundle P    IOP-3's import half and IOP-10's read direction: a
//                        `.ppcpbndl` through `PpcpBundleTransport` →
//                        `makeHostEngine()`'s peer → `PpcpImportSink`, which is
//                        the SAME path a socket feeds (plan A10).
//                        `--import-twice` re-reads it against the same ledger,
//                        which is I34.
//   --write-bundle P     IOP-10's write direction: this host's own Session
//                        record, through `ppcp_bundle_writer`, so the device
//                        reads what we wrote.
//
// `--summary PATH` writes what this host SAW as JSON.  Every interoperability
// row asserts on both ends: `ppcp-sim`'s `--expect` is the counterpart's view
// and this file is the host's, and a row that only ever read one of them would
// be measuring half a pairing.
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
//
// ⚠ EXCEPT WITH `--tls-psk`, WHICH IS S5 WAVE 2 AND IS THE REAL LISTENER.  The
// pair that matters is PinPointStudio ↔ PinPointCapture, and neither of them is
// `ppcp-sim`: they speak TLS 1.3 with an external PSK (RV §5), so that run must
// not go anywhere near the harness socket.  `--tls-psk HEX --tls-identity TEXT`
// therefore takes the plaintext option OFF and installs an `IdentityResolver`
// over the given key — the same `Ppcp::Listener`, the same handshake, the same
// `ENC` §2.1 binding as the application's.  `run-tls-host.sh` is its driver.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QDir>

#include <ppcp/bundle.h>
#include <ppcp/frame.h>

#include "ppcp_bundle_transport.h"
#include "ppcp_host_engine.h"
#include "ppcp_host_peer.h"
#include "ppcp_import_ledger.h"
#include "ppcp_import_sink.h"
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
    bool          trace = false;
    std::string   portFile;
    int           runMs = 120000;
    std::string   peerId = "peer:pinpointstudio-conform";

    // ── S5 ──────────────────────────────────────────────────────────────────
    std::string   row;             // the CONF §5 pairing this run is
    std::string   summaryPath;     // where this host's own view is written
    bool          neverIssue = false;          // IOP-7
    int           issueDelayMs = 0;            // IOP-8; 0 is "no delay"
    bool          nominateAcoustic = false;    // IOP-6
    int           maxNominations = 8;

    // 5.10e — the Session parameters, so IOP-8's margin is stated.  Negative
    // means "libppcp's own default", which is what every other row uses.
    long long     issueHoldMs = -1;
    long long     coincidenceMs = -1;
    long long     heartbeatMs = -1;

    // IOP-3 / IOP-10
    std::string   importBundle;
    std::string   importRoot;
    bool          importTwice = false;
    std::string   writeBundle;

    // Wave 2 — the REAL listener
    std::string   tlsPskHex;
    std::string   tlsIdentity = "any";
};

bool hexToKey(const std::string &hex, Ppcp::Key &out)
{
    if (hex.size() != out.size() * 2) return false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

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
        else if (a == "--trace")     { o.trace = true; }
        else if (a == "--row")       { const char *v = next(); if (!v) return false; o.row = v; }
        else if (a == "--summary")   { const char *v = next(); if (!v) return false; o.summaryPath = v; }
        else if (a == "--never-issue") { o.neverIssue = true; }
        else if (a == "--issue-delay-ms") { const char *v = next(); if (!v) return false;
                                            o.issueDelayMs = std::atoi(v); }
        else if (a == "--nominate-acoustic") { o.nominateAcoustic = true; }
        else if (a == "--issue-hold-ms")  { const char *v = next(); if (!v) return false;
                                            o.issueHoldMs = std::atoll(v); }
        else if (a == "--coincidence-ms") { const char *v = next(); if (!v) return false;
                                            o.coincidenceMs = std::atoll(v); }
        else if (a == "--heartbeat-ms")   { const char *v = next(); if (!v) return false;
                                            o.heartbeatMs = std::atoll(v); }
        else if (a == "--import-bundle")  { const char *v = next(); if (!v) return false;
                                            o.importBundle = v; }
        else if (a == "--import-root")    { const char *v = next(); if (!v) return false;
                                            o.importRoot = v; }
        else if (a == "--import-twice")   { o.importTwice = true; }
        else if (a == "--write-bundle")   { const char *v = next(); if (!v) return false;
                                            o.writeBundle = v; }
        else if (a == "--tls-psk")        { const char *v = next(); if (!v) return false;
                                            o.tlsPskHex = v; }
        else if (a == "--tls-identity")   { const char *v = next(); if (!v) return false;
                                            o.tlsIdentity = v; }
        else if (a == "--help") {
            std::printf("ppcp_conform_host — the PinPointStudio host, headless, on a PLAINTEXT\n"
                        "harness socket (PPCP-CONF 2c, PPCP-RV erratum E4).\n\n"
                        "  --port N            listen port; 0 takes an ephemeral one\n"
                        "  --port-file P       write the bound port to P (for a driver script)\n"
                        "  --run-ms MS         stop after this long (default 120000)\n"
                        "  --peer-id ID        CORE 5.1a Peer.id to declare\n"
                        "  --row NAME          the CONF §5 pairing this run is, for the summary\n"
                        "  --summary P         write what this host saw, as JSON, on exit\n"
                        "\n  the four CONF §5 host shapes:\n"
                        "  --never-issue       build the arbiter and never run 8.2h (IOP-7)\n"
                        "  --issue-delay-ms N  run 8.2h only N ms after link-up (IOP-8)\n"
                        "  --nominate-acoustic this host's own microphone nominates (IOP-6)\n"
                        "  --issue-hold-ms N   5.10e issue_hold, in ms\n"
                        "  --coincidence-ms N  5.10e coincidence_window, in ms\n"
                        "  --heartbeat-ms N    7.4a heartbeat interval, in ms\n"
                        "\n  the bundle directions (IOP-3, IOP-10):\n"
                        "  --import-bundle P   read a .ppcpbndl through the real import path\n"
                        "  --import-root D     where imported clips and the ledger land\n"
                        "  --import-twice      read it again against the same ledger (I34)\n"
                        "  --write-bundle P    write this host's own Session record\n"
                        "\n  wave 2 — the REAL listener, no harness socket:\n"
                        "  --tls-psk HEX       32-byte K_tls as 64 hex characters\n"
                        "  --tls-identity T    the PSK identity to accept; `any` accepts all\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    if (!o.tlsPskHex.empty()) {
        Ppcp::Key probe{};
        if (!hexToKey(o.tlsPskHex, probe)) {
            std::fprintf(stderr, "--tls-psk must be 64 hex characters (32 bytes)\n");
            return false;
        }
    }
    return true;
}

// ── WHAT THIS HOST SAW, AS JSON ─────────────────────────────────────────────
//
// The other half of every `CONF` §5 row.  `ppcp-sim --expect` asserts the
// counterpart's counters; these are ours, and the two together are the pairing.
// Flat and hand-written on purpose: a conformance artefact that needed a JSON
// library would be one more thing between the run and the evidence.
struct Seen {
    std::string row;
    std::string peerId;
    std::string counterpart;
    bool        linkUp = false;
    bool        sessionOpened = false;
    bool        arbiterStarted = false;
    std::string arbiterError;

    std::size_t declaresRx = 0;
    std::size_t candidatesRx = 0;
    std::size_t shotsRx = 0;            // a Shot the DEVICE minted (8.2i)
    std::size_t streamsRx = 0;
    std::size_t previewStreamsRx = 0;
    std::size_t continuousStreamsRx = 0;
    std::size_t capturesRx = 0;
    std::size_t capturesAbsent = 0;
    std::size_t capturesNotRetained = 0;
    std::size_t previewCapturesRx = 0;
    std::size_t previewPayloadFrames = 0;
    std::size_t payloadFrames = 0;
    std::size_t offersRx = 0;
    std::size_t offersAccepted = 0;
    std::size_t errorsRx = 0;
    std::size_t errorsFatal = 0;
    std::vector<std::string> errorCodes;
    std::size_t unknownRx = 0;

    // PpcpShotBridge::Stats, taken at exit
    std::size_t nominated = 0, observedForeign = 0, excluded = 0, issued = 0;
    std::size_t late = 0, adopted = 0, captureRequests = 0, nominationsRefused = 0;
    std::size_t retained = 0, groups = 0;
    std::size_t maxShotCandidates = 0;
    std::vector<std::string> shotIds;

    // PpcpLiveSession::Stats
    std::size_t probesQueued = 0, relationsPublished = 0, heartbeatAcks = 0;
    std::size_t syncEstimators = 0, estimatorsWithoutEstimate = 0;

    // ── E28 / F-S5-3 — THE LIVE SESSION'S OWN IDENTITY, READ TWICE ─────────
    //
    // Recorded when the Session opens and again at exit.  CORE 4.1a and I16
    // make `timebase_ref` immutable for a Session's life, and the defect E28
    // was raised for moved it without a malformed byte anywhere: one
    // `session_offer` accepted mid-session rebound the host's reference clock
    // to the exporting DEVICE's, so every subsequent `t0` was expressed in a
    // timebase the live Session had never declared.  Two readings and a
    // comparison is the only thing that could have caught it.
    std::string liveSessionIdAtOpen, liveTimebaseRefAtOpen;
    std::string liveSessionIdAtExit, liveTimebaseRefAtExit;
    // The IMPORTED Session, where one was replayed onto this link (MSG §9.1).
    std::string importedSessionId, importedTimebaseRef;
    std::size_t importedIgnored = 0;   // frames kept away from the live arbiter
    std::size_t reconsidered = 0;      // E29 — Candidates re-admitted

    // 8.2i1 / CONF 5b — asked of every timebase the counterpart declared, at
    // exit.  `has_offset: false` is the whole assertion: a host that
    // substituted a zero would answer true with 0 here and nothing else on the
    // wire would show it.
    struct TbProbe { std::string id; bool hasOffset = false; long long offsetNs = 0; };
    std::vector<TbProbe> counterpartTimebases;

    // --import-bundle
    bool        importRan = false;
    std::string importError;
    std::vector<std::string> importPasses;   // one JSON object per pass

    std::string bundleWritten;
    std::size_t bundleFrames = 0;
};

std::string jstr(const std::string &s)
{
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

bool writeSummary(const Seen &s, const std::string &path)
{
    std::ostringstream o;
    auto n = [&](const char *k, long long v) { o << "  \"" << k << "\": " << v << ",\n"; };
    auto b = [&](const char *k, bool v) {
        o << "  \"" << k << "\": " << (v ? "true" : "false") << ",\n";
    };
    o << "{\n";
    o << "  \"row\": " << jstr(s.row) << ",\n";
    o << "  \"peer_id\": " << jstr(s.peerId) << ",\n";
    o << "  \"counterpart\": " << jstr(s.counterpart) << ",\n";
    b("link_up", s.linkUp);
    b("session_opened", s.sessionOpened);
    b("arbiter_started", s.arbiterStarted);
    o << "  \"arbiter_error\": " << jstr(s.arbiterError) << ",\n";
    n("declares_rx", (long long)s.declaresRx);
    n("candidates_rx", (long long)s.candidatesRx);
    n("shots_rx", (long long)s.shotsRx);
    n("streams_rx", (long long)s.streamsRx);
    n("preview_streams_rx", (long long)s.previewStreamsRx);
    n("continuous_streams_rx", (long long)s.continuousStreamsRx);
    n("captures_rx", (long long)s.capturesRx);
    n("captures_absent", (long long)s.capturesAbsent);
    n("captures_not_retained", (long long)s.capturesNotRetained);
    n("preview_captures_rx", (long long)s.previewCapturesRx);
    n("preview_payload_frames", (long long)s.previewPayloadFrames);
    n("payload_frames", (long long)s.payloadFrames);
    n("offers_rx", (long long)s.offersRx);
    n("offers_accepted", (long long)s.offersAccepted);
    n("errors_rx", (long long)s.errorsRx);
    n("errors_fatal", (long long)s.errorsFatal);
    n("unknown_rx", (long long)s.unknownRx);
    n("nominated", (long long)s.nominated);
    n("candidates_foreign", (long long)s.observedForeign);
    n("excluded", (long long)s.excluded);
    n("issued", (long long)s.issued);
    n("late", (long long)s.late);
    n("adopted", (long long)s.adopted);
    n("capture_requests", (long long)s.captureRequests);
    n("nominations_refused", (long long)s.nominationsRefused);
    n("retained", (long long)s.retained);
    n("groups", (long long)s.groups);
    n("max_shot_candidates", (long long)s.maxShotCandidates);
    n("probes_queued", (long long)s.probesQueued);
    n("relations_published", (long long)s.relationsPublished);
    n("heartbeat_acks", (long long)s.heartbeatAcks);
    n("sync_estimators", (long long)s.syncEstimators);
    n("estimators_without_estimate", (long long)s.estimatorsWithoutEstimate);
    n("imported_ignored", (long long)s.importedIgnored);
    n("reconsidered", (long long)s.reconsidered);
    o << "  \"live_session_id_at_open\": " << jstr(s.liveSessionIdAtOpen) << ",\n";
    o << "  \"live_timebase_ref_at_open\": " << jstr(s.liveTimebaseRefAtOpen) << ",\n";
    o << "  \"live_session_id_at_exit\": " << jstr(s.liveSessionIdAtExit) << ",\n";
    o << "  \"live_timebase_ref_at_exit\": " << jstr(s.liveTimebaseRefAtExit) << ",\n";
    o << "  \"imported_session_id\": " << jstr(s.importedSessionId) << ",\n";
    o << "  \"imported_timebase_ref\": " << jstr(s.importedTimebaseRef) << ",\n";

    o << "  \"error_codes\": [";
    for (std::size_t i = 0; i < s.errorCodes.size(); ++i)
        o << (i ? ", " : "") << jstr(s.errorCodes[i]);
    o << "],\n";

    o << "  \"shot_ids\": [";
    for (std::size_t i = 0; i < s.shotIds.size(); ++i)
        o << (i ? ", " : "") << jstr(s.shotIds[i]);
    o << "],\n";

    o << "  \"counterpart_timebases\": [";
    for (std::size_t i = 0; i < s.counterpartTimebases.size(); ++i) {
        const Seen::TbProbe &t = s.counterpartTimebases[i];
        o << (i ? ", " : "") << "{\"id\": " << jstr(t.id)
          << ", \"has_offset\": " << (t.hasOffset ? "true" : "false")
          << ", \"offset_ns\": " << t.offsetNs << "}";
    }
    o << "],\n";

    b("import_ran", s.importRan);
    o << "  \"import_error\": " << jstr(s.importError) << ",\n";
    o << "  \"import_passes\": [";
    for (std::size_t i = 0; i < s.importPasses.size(); ++i)
        o << (i ? ", " : "") << s.importPasses[i];
    o << "],\n";
    o << "  \"bundle_written\": " << jstr(s.bundleWritten) << ",\n";
    o << "  \"bundle_frames\": " << s.bundleFrames << "\n";
    o << "}\n";

    const std::string tmp = path + ".tmp";
    { std::ofstream f(tmp); if (!f) return false; f << o.str(); }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
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
    Harness(const HarnessOptions &o, Seen &seen)
        : m_opt(o), m_peer([&] {
              PpcpHostPeer::Config c;
              c.peerId = o.peerId;
              return c;
          }()), m_seen(seen), m_trace(o.trace)
    {
        m_seen.row = o.row;
        m_seen.peerId = o.peerId;
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
        m_peer.addEventHook([this](const ppcp_event &ev) {
            m_offers.observe(ev);
            count(ev);
            if (m_trace) say("event " + std::to_string(static_cast<int>(ev.kind)));
        });
        m_peer.shotBridge().setShotCallback([this](const ppcp_shot &s) {
            say("SHOT issued: " + idStr(s.id) + " over "
                + std::to_string(s.candidate_count) + " candidate(s)");
            m_seen.shotIds.push_back(idStr(s.id));
            if (s.candidate_count > m_seen.maxShotCandidates)
                m_seen.maxShotCandidates = s.candidate_count;
            appendToBundle(s);
        });
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

        // ⚠ EXACTLY ONE OF THESE TWO.  With a PSK there is a real TLS 1.3
        // handshake and the harness socket is not compiled into this run's code
        // path at all; without one there is no identity and the listener says
        // so (`TlsOutcome::version` == "plaintext-harness").
        if (!m_opt.tlsPskHex.empty()) {
            Ppcp::Key k{};
            (void)hexToKey(m_opt.tlsPskHex, k);
            const std::string wanted = m_opt.tlsIdentity;
            // RV 5.3d — the two failing paths (unknown identity, wrong key) are
            // indistinguishable, so this refuses by returning false and says
            // nothing about which.  `any` exists because RV §10.2's identity
            // carries a rotating `rid` and a script cannot know it in advance.
            m_listener.setIdentityResolver(
                [k, wanted](const unsigned char *id, std::size_t len,
                            Ppcp::ResolvedPairing &out) {
                    if (wanted != "any") {
                        if (len != wanted.size()) return false;
                        if (std::memcmp(id, wanted.data(), len) != 0) return false;
                    }
                    out.kTls = k;
                    out.pairingId = "pairing:run-tls-host";
                    return true;
                });
            say("PPCP listener is TLS 1.3 external-PSK (RV §5) — identity "
                + (wanted == "any" ? std::string("ACCEPT-ANY") : wanted));
        } else {
            m_listener.setPlaintextHarness(true);
        }
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

        if (!tickThisHost(hostNowNs())) {
            drop("link closed");
            return;
        }
        runPendingNominations();
        autoAcceptOffers();
    }

    // The exit path, so a run that ended by running out of time reports the
    // same numbers as one whose link went away.
    void finish()
    {
        if (m_link) drop("run-ms elapsed");
        closeBundle();
    }

private:
    // ── `PpcpHostPeer::tick()`, AND WHEN IT IS NOT CALLED ──────────────────
    //
    // tick() is pump(), §6.3's sync schedule, §7.4's liveness schedule, 8.2h's
    // issue step, pump().  IOP-7 and IOP-8 are stated over a host that does not
    // run the fourth of those, or does not run it yet — so those two rows call
    // the other four directly rather than through tick(), which is precisely
    // what `ppcp-sim`'s `silent-host` (SIM_F_NEVER_ISSUE) and `late-host`
    // (`arb_delay_ms`) do on the other side of the same pairing.
    //
    // ⚠ NOTHING ELSE DIFFERS, and no other row takes this path: with neither
    // option the call below IS `m_peer.tick()`.
    bool tickThisHost(std::int64_t nowNs)
    {
        if (!m_opt.neverIssue && m_opt.issueDelayMs <= 0) return m_peer.tick(nowNs);

        const bool alive = m_peer.pump();
        m_peer.liveSession().pump(nowNs);
        if (!m_opt.neverIssue) {
            const std::int64_t holdUntil =
                m_linkUpNs + static_cast<std::int64_t>(m_opt.issueDelayMs) * 1000000;
            if (nowNs >= holdUntil) m_peer.shotBridge().pump(nowNs);
        }
        if (alive) return m_peer.pump();
        return alive;
    }

    // IOP-6 — THIS HOST'S OWN MICROPHONE NOMINATES.  8.1d's time of flight is
    // null: a synthetic onset has no distance to correct for, and 8.1e forbids
    // inventing one.  The instant is a reading of `tb:host` taken when the
    // device's Candidate reached us, which on loopback is within a millisecond
    // of the onset both microphones would have heard — well inside 5.10's
    // 50 ms coincidence window, which is what makes the two ONE event and the
    // row about I8 rather than about two unrelated Shots.
    //
    // Deferred out of the event hook on purpose: nominating re-enters the
    // engine, and the hook runs while its event ring is being drained.
    void runPendingNominations()
    {
        if (!m_pendingNominations) return;
        const int n = m_pendingNominations;
        m_pendingNominations = 0;
        // ⚠ ASKED OF THE DECLARATION, NEVER SPELLED OUT HERE.  I26 / 5.12a:
        // the Candidate's Source is one this peer DECLARED, and libppcp refuses
        // anything else before a wire sees it.  The first draft passed the
        // inventory's device id and was refused — the declaration builder
        // prefixes a microphone's Source id with `mic:` — which is the check
        // doing its job, and the reason this reads the id back rather than
        // repeating a convention that lives in another file.
        const std::string micId = ownMicrophoneSourceId();
        if (micId.empty()) {
            say("this host declared no microphone Source, so it cannot nominate (I26)");
            return;
        }
        for (int i = 0; i < n; ++i) {
            if (m_nominationsDone >= m_opt.maxNominations) return;
            std::string cid, err;
            if (m_peer.shotBridge().nominate(micId, kBasisAcoustic,
                                             hostNowNs(), 0, 0.9, nullptr, &cid, &err)) {
                ++m_nominationsDone;
                say("nominated from this host's own microphone: " + cid);
            } else {
                say("this host's own nomination was refused: " + err);
            }
        }
    }

public:

private:
    void adopt(std::unique_ptr<PeerConnection> link)
    {
        m_link = std::move(link);

        // ⚠ F-H8-5 — A `ppcp_peer` IS THE CONVERSATION, NOT THE APPLICATION.
        // A fresh engine per link.  The engine carries the counterpart's
        // declaration, the open Session, the `msg_id` sequence and the link
        // state; handing a second, different device the engine the first one
        // left behind means `ppcp_peer_session_open()` refuses — the previous
        // Session is still open on it and nothing closed it, because the link
        // died rather than saying goodbye.  See the same fix in
        // `PpcpHostService::adoptLink()`.
        std::string derr;
        m_engine = m_peer.makeLibppcpEngine(&derr);
        if (!m_engine) {
            say("could not rebuild the engine for this link: " + derr);
            m_link->close();
            m_link.reset();
            return;
        }
        m_peer.attach(m_link.get(), m_engine.get());
        m_sessionOpen = false;
        m_linkUpNs = hostNowNs();
        m_seen.linkUp = true;
        say("link up — " + m_link->tls().describe());
    }

    void drop(const char *why)
    {
        if (!m_link) return;
        // ⚠ BEFORE stop()/detach(), because both of those throw away the state
        // the assertions are made over.  8.2i1's answer in particular — "is
        // there a reading of `timebase_ref` for this peer's clock" — is only
        // askable while the relation set is still attached, and a `false` here
        // is the whole of CONF 5b: a host that substituted a zero would answer
        // `true` with `0` and nothing on the wire would say so.
        harvest();
        m_offers.detach();
        m_peer.shotBridge().stop();
        m_peer.attach(nullptr, nullptr);
        m_link->close();
        m_link.reset();
        m_sessionOpen = false;
        // Every counter this host kept over the link, on one line.  A row that
        // fails deserves a number rather than a guess about why.
        const PpcpShotBridge::Stats &b = m_peer.shotBridge().stats();
        const PpcpLiveSession::Stats &l = m_peer.liveSession().stats();
        char buf[512];
        std::snprintf(buf, sizeof buf,
                      "nominated=%zu foreign=%zu excluded=%zu issued=%zu late=%zu adopted=%zu "
                      "capreq=%zu refused=%zu | probes=%zu relations=%zu hbacks=%zu "
                      "estimators=%zu noestimate=%zu",
                      b.nominated, b.observedForeign, b.excluded, b.issued, b.late, b.adopted,
                      b.captureRequests, b.nominationsRefused,
                      l.probesQueued, l.relationsPublished, l.heartbeatAcks,
                      l.syncEstimators, l.estimatorsWithoutEstimate);
        say(std::string("link down (") + why + ") — " + buf);
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
        m_seen.counterpart = counterpart;
        m_counterpartTimebases.clear();
        for (std::size_t i = 0; i < desc->timebase_count; ++i)
            m_counterpartTimebases.push_back(idStr(desc->timebases[i].id));

        PpcpLiveSession::Config cfg;
        cfg.sessionId = "sess:" + uuid4();
        // 5.10e — stated rather than assumed where a row turns on the margin
        // between this host's issue hold and the device's mint deadline (I35).
        if (m_opt.coincidenceMs >= 0) cfg.coincidenceWindowNs = m_opt.coincidenceMs * 1000000;
        if (m_opt.issueHoldMs >= 0)   cfg.issueHoldNs = m_opt.issueHoldMs * 1000000;
        if (m_opt.heartbeatMs >= 0)
            cfg.heartbeatIntervalMs = static_cast<std::uint32_t>(m_opt.heartbeatMs);
        std::string err;
        if (!m_peer.liveSession().open(cfg, &err)) {
            say("session_open refused: " + err);
            return;
        }
        m_seen.sessionOpened = true;
        m_sessionId = cfg.sessionId;
        if (m_engine && m_engine->peer()) {
            m_seen.liveSessionIdAtOpen = idOrEmpty(ppcp_peer_session_id(m_engine->peer()));
            m_seen.liveTimebaseRefAtOpen = idOrEmpty(ppcp_peer_timebase_ref(m_engine->peer()));
        }
        openBundle(cfg);

        // I20 — libppcp refuses to build an arbiter for a peer that is not
        // `role: host` and does not declare Arbitrate, and this reports the
        // refusal rather than falling back to something that is not arbitration.
        PpcpShotBridge::Config bc;
        bc.peerId = m_opt.peerId;
        if (!m_peer.shotBridge().start(bc, [](std::string *out) { *out = uuid4(); return true; },
                                       &err)) {
            say("the arbiter would not start: " + err);
            m_seen.arbiterError = err;
        } else {
            m_seen.arbiterStarted = true;
        }

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
            if (m_offers.acceptOffer(r)) {
                say("accepted an offered Session (MSG 9.2)");
                ++m_seen.offersAccepted;
            }
        }
    }

    // ── COUNTING WHAT ARRIVED, AND NOTHING ELSE ────────────────────────────
    //
    // A hook, so it never drains: `PpcpHostPeer` is the one drainer of the
    // event ring (see its header) and a second consumer here would give each of
    // us roughly half the conversation.
    void count(const ppcp_event &ev)
    {
        switch (ev.kind) {
        case PPCP_EVENT_DECLARE: ++m_seen.declaresRx; break;
        case PPCP_EVENT_CANDIDATE:
            ++m_seen.candidatesRx;
            if (m_opt.nominateAcoustic) ++m_pendingNominations;
            break;
        // 8.2i — a Shot the DEVICE minted because this host did not answer.
        // Counted apart from the ones this host issued, which is the whole
        // content of IOP-7 and IOP-8.
        case PPCP_EVENT_SHOT: ++m_seen.shotsRx; break;
        case PPCP_EVENT_STREAM_OPEN:
            if (ev.msg) {
                const ppcp_stream &st = ev.msg->body.stream_open.stream;
                const std::string id = idStr(st.id), kind = idStr(st.kind);
                m_streamKind[id] = kind;
                ++m_seen.streamsRx;
                if (kind == PPCP_STREAM_KIND_PREVIEW) ++m_seen.previewStreamsRx;
                if (st.continuity == PPCP_CONTINUOUS) ++m_seen.continuousStreamsRx;
            }
            break;
        case PPCP_EVENT_CAPTURE:
            if (ev.msg) {
                const ppcp_capture &c = ev.msg->body.capture_announce.capture;
                ++m_seen.capturesRx;
                const std::string sk = m_streamKind.count(idStr(c.stream_id))
                                           ? m_streamKind[idStr(c.stream_id)] : std::string();
                if (sk == PPCP_STREAM_KIND_PREVIEW) ++m_seen.previewCapturesRx;
                if (c.completeness == PPCP_ABSENT) {
                    ++m_seen.capturesAbsent;
                    // 5.11c3 / I36 — a preview the peer discarded on purpose is
                    // `absent` with THIS reason, and never a gap.
                    if (c.has_absent_reason && idStr(c.absent_reason) == PPCP_ABSENT_NOT_RETAINED)
                        ++m_seen.capturesNotRetained;
                }
            }
            break;
        case PPCP_EVENT_PAYLOAD:
            ++m_seen.payloadFrames;
            // 5.11j — a preview holds no payload and never reaches a bundle.
            // Counted so "absent from the bundle" is a measured zero here and
            // not an assumption made about the device.
            if (ev.msg && ev.msg->type == PPCP_MT_PAYLOAD_BEGIN) {
                const std::string cap = idStr(ev.msg->body.payload_begin.capture_id);
                if (m_previewCaptureIds.count(cap)) ++m_seen.previewPayloadFrames;
            }
            break;
        case PPCP_EVENT_SESSION_OFFER: ++m_seen.offersRx; break;
        case PPCP_EVENT_ERROR:
            // ⚠ FATALITY IS THE CODE, NOT THE EVENT'S `status`.  MSG §10 makes
            // exactly two codes fatal, and `ev.status` also carries the reason
            // the engine itself raised an error — so classifying on it would
            // read every `profile_not_supported` as a lost link, which is
            // precisely the answer CT-S6 assertion 2 says is wrong: an observer
            // ANSWERS `profile_not_supported` and the transport stays open.
            ++m_seen.errorsRx;
            if (ev.msg) {
                const std::string code = idStr(ev.msg->body.error.code);
                m_seen.errorCodes.push_back(code);
                if (ppcp_msg_error_is_fatal(code.c_str(), code.size())) ++m_seen.errorsFatal;
            }
            break;
        case PPCP_EVENT_UNKNOWN: ++m_seen.unknownRx; break;
        default: break;
        }
        // Kept apart from the switch: a preview Capture's id is needed by the
        // PAYLOAD case above, which may run before or after this one.
        if (ev.kind == PPCP_EVENT_CAPTURE && ev.msg) {
            const ppcp_capture &c = ev.msg->body.capture_announce.capture;
            const std::string sid = idStr(c.stream_id);
            if (m_streamKind.count(sid) && m_streamKind[sid] == PPCP_STREAM_KIND_PREVIEW)
                m_previewCaptureIds.insert(idStr(c.id));
        }
    }

    // Every counter this host kept, into the summary — and, for the CONF 5b
    // question, the answer libppcp gives for each of the counterpart's clocks.
    void harvest()
    {
        const PpcpShotBridge::Stats &b = m_peer.shotBridge().stats();
        const PpcpLiveSession::Stats &l = m_peer.liveSession().stats();
        m_seen.nominated = b.nominated;
        m_seen.observedForeign = b.observedForeign;
        m_seen.excluded = b.excluded;
        m_seen.issued = b.issued;
        m_seen.late = b.late;
        m_seen.adopted = b.adopted;
        m_seen.captureRequests = b.captureRequests;
        m_seen.nominationsRefused = b.nominationsRefused;
        m_seen.retained = m_peer.shotBridge().retainedCount();
        m_seen.groups = m_peer.shotBridge().groupCount();
        m_seen.probesQueued = l.probesQueued;
        m_seen.relationsPublished = l.relationsPublished;
        m_seen.heartbeatAcks = l.heartbeatAcks;
        m_seen.syncEstimators = l.syncEstimators;
        m_seen.estimatorsWithoutEstimate = l.estimatorsWithoutEstimate;
        m_seen.importedIgnored = b.importedIgnored;
        m_seen.reconsidered = b.reconsidered;

        // ⚠ READ FROM THE ENGINE, NOT FROM WHAT THIS HARNESS ASKED FOR.  The
        // question E28 answers is what the LIBRARY thinks the live Session is
        // after a replay, and reading back our own `cfg.sessionId` would answer
        // a different and useless one.
        if (m_engine && m_engine->peer()) {
            m_seen.liveSessionIdAtExit = idOrEmpty(ppcp_peer_session_id(m_engine->peer()));
            m_seen.liveTimebaseRefAtExit = idOrEmpty(ppcp_peer_timebase_ref(m_engine->peer()));
            m_seen.importedSessionId =
                idOrEmpty(ppcp_peer_imported_session_id(m_engine->peer()));
            m_seen.importedTimebaseRef =
                idOrEmpty(ppcp_peer_imported_timebase_ref(m_engine->peer()));
        }

        m_seen.counterpartTimebases.clear();
        for (const std::string &tb : m_counterpartTimebases) {
            Seen::TbProbe p;
            p.id = tb;
            std::int64_t off = 0;
            p.hasOffset = m_peer.liveSession().offsetToRefNs(tb, hostNowNs(), &off);
            p.offsetNs = p.hasOffset ? off : 0;
            m_seen.counterpartTimebases.push_back(p);
        }
    }

    // ── IOP-10, THE WRITE DIRECTION ────────────────────────────────────────
    //
    // ENC 7a: "a bundle contains the same messages, in the same encoding and
    // framing, as the live path."  So this writes the Session this host is
    // actually running — `session_open` with BOTH arbitration parameters
    // (5.10e: the structural statement that the Session HAS a host), its own
    // `declare`, and every Shot as it is issued — and closes with
    // `session_state` and `session_manifest`.  Nothing is composed afterwards
    // from remembered scalars: a `ppcp_shot`'s Candidate list is borrowed
    // storage and is only itself at the moment the Shot is reported.
    void openBundle(const PpcpLiveSession::Config &cfg)
    {
        if (m_opt.writeBundle.empty() || m_writer) return;
        m_writerStorage.resize(ppcp_bundle_writer_sizeof());
        if (ppcp_bundle_writer_new(m_writerStorage.data(), m_writerStorage.size(), &m_writer)
            != PPCP_OK) {
            say("could not create the bundle writer");
            m_writer = nullptr;
            return;
        }
        std::uint8_t buf[PPCP_BUNDLE_HEADER_BYTES + 8];
        std::size_t n = 0;
        if (ppcp_bundle_writer_begin(m_writer, buf, sizeof buf, &n) != PPCP_OK) return;
        m_bundleBytes.insert(m_bundleBytes.end(), buf, buf + n);

        ppcp_msg m{};
        if (ppcp_msg_init(&m, PPCP_MT_SESSION_OPEN, m_bundleMsgId++) != PPCP_OK) return;
        if (ppcp_id_set_z(&m.body.session_open.session_id, cfg.sessionId.c_str()) != PPCP_OK)
            return;
        if (ppcp_id_set_z(&m.body.session_open.timebase_ref, cfg.timebaseRef.c_str()) != PPCP_OK)
            return;
        m.body.session_open.has_arbitration = true;
        m.body.session_open.coincidence_window_ns = cfg.coincidenceWindowNs;
        m.body.session_open.issue_hold_ns = cfg.issueHoldNs;
        m.body.session_open.has_heartbeat_interval = true;
        m.body.session_open.heartbeat_interval_ms = cfg.heartbeatIntervalMs;
        appendFrame(PPCP_CHANNEL_CONTROL, m);

        // MSG 3.3c — the declaration comes before anything that references a
        // Source, in a bundle exactly as on a wire.
        if (const ppcp_peer_desc *self = m_peer.declaration().peer()) {
            if (ppcp_msg_init(&m, PPCP_MT_DECLARE, m_bundleMsgId++) != PPCP_OK) return;
            m.body.declare.generation = 1;
            m.body.declare.peer = *self;
            appendFrame(PPCP_CHANNEL_CONTROL, m);
        }
    }

    void appendToBundle(const ppcp_shot &s)
    {
        if (!m_writer) return;
        ppcp_msg m{};
        if (ppcp_msg_init(&m, PPCP_MT_SHOT, m_bundleMsgId++) != PPCP_OK) return;
        if (ppcp_msg_set_session_id(&m, m_sessionId.c_str()) != PPCP_OK) return;
        m.body.shot.shot = s;
        appendFrame(PPCP_CHANNEL_CONTROL, m);
    }

    void closeBundle()
    {
        if (!m_writer) return;
        ppcp_msg m{};
        if (ppcp_msg_init(&m, PPCP_MT_SESSION_STATE, m_bundleMsgId++) == PPCP_OK
            && ppcp_id_set_z(&m.body.session_state.session_id, m_sessionId.c_str()) == PPCP_OK) {
            m.body.session_state.state = PPCP_SESSION_CLOSED;
            // 4.4a / I10 — completeness is ASSERTED by the owner.  This host
            // recorded everything it arbitrated, so `complete` is the honest
            // assertion and not a default.
            m.body.session_state.completeness = PPCP_COMPLETE;
            appendFrame(PPCP_CHANNEL_CONTROL, m);
        }
        if (ppcp_msg_init(&m, PPCP_MT_SESSION_MANIFEST, m_bundleMsgId++) == PPCP_OK
            && ppcp_id_set_z(&m.body.session_manifest.session_id, m_sessionId.c_str()) == PPCP_OK) {
            m.body.session_manifest.completeness = PPCP_COMPLETE;
            m.body.session_manifest.count_shots = m_seen.shotIds.size();
            m.body.session_manifest.count_candidates = m_seen.candidatesRx + m_nominationsDone;
            m.body.session_manifest.count_captures = 0;
            appendFrame(PPCP_CHANNEL_CONTROL, m);
        }
        if (ppcp_bundle_writer_finish(m_writer) != PPCP_OK) {
            say("the bundle writer refused to finish");
            return;
        }
        std::ofstream f(m_opt.writeBundle, std::ios::binary);
        if (!f) { say("could not open " + m_opt.writeBundle + " for writing"); return; }
        f.write(reinterpret_cast<const char *>(m_bundleBytes.data()),
                static_cast<std::streamsize>(m_bundleBytes.size()));
        f.close();
        m_seen.bundleWritten = m_opt.writeBundle;
        m_seen.bundleFrames = ppcp_bundle_writer_frame_count(m_writer);
        say("wrote " + std::to_string(m_bundleBytes.size()) + " bundle bytes over "
            + std::to_string(m_seen.bundleFrames) + " frames to " + m_opt.writeBundle);
        m_writer = nullptr;
    }

    void appendFrame(std::uint8_t channel, const ppcp_msg &m)
    {
        std::vector<std::uint8_t> buf(64 * 1024);
        std::size_t n = 0;
        const ppcp_result r =
            ppcp_bundle_writer_append_msg(m_writer, channel, &m, buf.data(), buf.size(), &n);
        if (r != PPCP_OK) {
            say("the bundle writer refused a frame: " + std::to_string(static_cast<int>(r)));
            return;
        }
        m_bundleBytes.insert(m_bundleBytes.end(), buf.begin(), buf.begin() + n);
    }

    std::string ownMicrophoneSourceId() const
    {
        for (const ppcp_source &src : m_peer.declaration().sources())
            if (idStr(src.kind) == "microphone") return idStr(src.id);
        return {};
    }

    static std::string idStr(const ppcp_id &id)
    {
        return std::string(id.v, id.len);
    }

    static std::string idOrEmpty(const ppcp_id *id)
    {
        return id ? std::string(id->v, id->len) : std::string();
    }

    HarnessOptions                    m_opt;
    PpcpHostPeer                      m_peer;
    Seen                             &m_seen;
    Ppcp::PpcpImportLedger            m_ledger;
    PpcpOfferController               m_offers;
    Listener                          m_listener;
    std::unique_ptr<PpcpEngine>       m_engine;
    std::unique_ptr<PeerConnection>   m_link;
    std::uint16_t                     m_port = 0;
    bool                              m_sessionOpen = false;
    bool                              m_trace = false;

    std::int64_t                      m_linkUpNs = 0;
    int                               m_pendingNominations = 0;
    int                               m_nominationsDone = 0;
    std::string                       m_sessionId;
    std::vector<std::string>          m_counterpartTimebases;
    std::map<std::string, std::string> m_streamKind;
    std::set<std::string>             m_previewCaptureIds;

    ppcp_bundle_writer               *m_writer = nullptr;
    std::vector<std::uint8_t>         m_writerStorage;
    std::vector<std::uint8_t>         m_bundleBytes;
    std::uint64_t                     m_bundleMsgId = 1;
};

// ── IOP-3's IMPORT HALF, AND IOP-10's READ DIRECTION ───────────────────────
//
// Plan A10 / CORE §9: "a consumer gains a FILE TRANSPORT, not an importer."  So
// this is not an import routine — it is `PpcpBundleTransport` (a file reader and
// a chunking policy) feeding the peer `makeHostEngine()` builds for a SOCKET,
// with `PpcpImportSink` deciding what becomes a record.  There is no branch
// anywhere below it that could tell a Capture that arrived in a file from one
// that arrived on a wire.
//
// ⚠ A FRESH ENGINE PER PASS, ONE LEDGER FOR BOTH.  That is the I34 claim: the
// engine is the conversation (F-H8-5) and a second reading of a bundle is a
// second conversation, but the ledger is this host's memory and it is what makes
// the second read a no-op rather than a duplicate.
int runImport(const HarnessOptions &opt, Seen &seen)
{
    seen.row = opt.row;
    seen.peerId = opt.peerId;
    seen.importRan = true;

    std::string root = opt.importRoot;
    if (root.empty()) root = QDir::tempPath().toStdString() + "/pps-iop-import";
    QDir().mkpath(QString::fromStdString(root));

    PpcpHostPeer::Config pc;
    pc.peerId = opt.peerId;
    PpcpHostPeer peer(pc);
    peer.setStorage([](std::uint64_t *freeBytes) {
        if (!freeBytes) return false;
        *freeBytes = 64ull * 1024 * 1024 * 1024;
        return true;
    });
    std::string err;
    if (!peer.declareSelf(harnessInventory(), &err)) {
        seen.importError = "could not declare: " + err;
        return 1;
    }

    PpcpImportLedger ledger;
    ledger.setPath(root + "/ledger.json");

    const int passes = opt.importTwice ? 2 : 1;
    int rc = 0;
    for (int pass = 0; pass < passes; ++pass) {
        std::unique_ptr<PpcpEngine> engine = peer.makeLibppcpEngine(&err);
        if (!engine) {
            seen.importError = "could not build the engine: " + err;
            return 1;
        }

        PpcpImportSink::Config sc;
        sc.importRoot = root;
        sc.writeClips = true;
        PpcpImportSink sink(ledger, engine->peer(), sc);

        PpcpBundleTransport::Options topt;
        topt.sink = engine->peer();
        topt.index = sink.index();
        topt.onFrame = [&sink](std::uint8_t) { sink.drainEvents(); };

        const PpcpBundleTransport::Result r =
            PpcpBundleTransport::streamFile(opt.importBundle, topt);
        sink.drainEvents();
        sink.finish(r);
        const PpcpImportSink::Stats &st = sink.stats();

        std::ostringstream o;
        o << "{\"pass\": " << pass
          << ", \"ok\": " << (r.ok ? "true" : "false")
          << ", \"error\": " << jstr(r.error)
          << ", \"minor\": " << r.minor
          << ", \"frames\": " << r.frames
          << ", \"truncated\": " << (r.truncated ? "true" : "false")
          << ", \"manifest_ordered\": " << (r.manifestOrdered ? "true" : "false")
          << ", \"asserted_completeness\": " << (r.assertedCompleteness ? "true" : "false")
          << ", \"completeness\": " << static_cast<int>(r.completeness)
          << ", \"owner_peer_id\": " << jstr(st.ownerPeerId)
          << ", \"session_id\": " << jstr(st.sessionId)
          << ", \"streams\": " << st.streams
          << ", \"captures\": " << st.captures
          << ", \"captures_new\": " << st.capturesNew
          << ", \"captures_already_held\": " << st.capturesAlreadyHeld
          << ", \"digest_conflicts\": " << st.digestConflicts
          << ", \"captures_unattributable\": " << st.capturesUnattributable
          << ", \"clips_written\": " << st.clipsWritten
          << ", \"clip_bytes\": " << st.clipBytes
          << ", \"commits_queued\": " << st.commitsQueued
          << ", \"unknown_events\": " << st.unknownEvents
          << ", \"session_dir\": " << jstr(st.sessionDir)
          << ", \"session_held\": "
          << (ledger.holdsSession(st.ownerPeerId, st.sessionId) ? "true" : "false")
          << "}";
        seen.importPasses.push_back(o.str());

        say("import pass " + std::to_string(pass) + ": " + (r.ok ? "ok" : "FAILED " + r.error)
            + ", frames=" + std::to_string(r.frames)
            + ", captures=" + std::to_string(st.captures)
            + " new=" + std::to_string(st.capturesNew)
            + " held=" + std::to_string(st.capturesAlreadyHeld));
        if (!r.ok) rc = 1;
    }
    ledger.save();
    return rc;
}

}  // namespace

int main(int argc, char **argv)
{
    // QCoreApplication, because `PpcpOfferController` formats its row labels
    // through QLocale and QDateTime and neither works without one.  No event
    // loop is entered: the pump below IS the loop.
    QCoreApplication app(argc, argv);

    HarnessOptions opt;
    if (!parse(argc, argv, opt)) return 2;

    Seen seen;

    // The bundle rows open no socket at all, which is the point of them: CORE
    // §9's offline path needs no rendezvous, no TLS and no listener.
    if (!opt.importBundle.empty()) {
        const int rc = runImport(opt, seen);
        if (!opt.summaryPath.empty() && !writeSummary(seen, opt.summaryPath))
            say("could not write the summary to " + opt.summaryPath);
        return rc;
    }

    Harness h(opt, seen);
    std::string err;
    if (!h.start(&err)) {
        say("FATAL: " + err);
        if (!opt.summaryPath.empty()) (void)writeSummary(seen, opt.summaryPath);
        return 1;
    }
    say("listening on 127.0.0.1:" + std::to_string(h.port())
        + (opt.tlsPskHex.empty() ? " (PLAINTEXT harness socket)" : " (TLS 1.3 external PSK)"));
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
    // ⚠ THE SUMMARY IS WRITTEN ON THE TIMEOUT PATH TOO, and that is not a
    // detail: every row here ends by running out of time, because a host does
    // not decide when a Session is over.  A summary only written on a clean
    // shutdown would never be written at all.
    h.finish();
    if (!opt.summaryPath.empty() && !writeSummary(seen, opt.summaryPath))
        say("could not write the summary to " + opt.summaryPath);
    return 0;
}
