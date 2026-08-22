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

#include "ppcp_host_peer.h"

#include <cstring>

#include "ppcp_host_engine.h"

namespace Ppcp {
namespace {

// The pump's working buffer. Not a protocol constant: ENC §8's limits are the
// library's to enforce on a whole frame, and this is how many bytes are moved
// between a socket and the engine per call. Sized to the default chunk of
// ENC 6f so a bulk chunk usually crosses in one read.
constexpr std::size_t kPumpBuffer = 256u * 1024u;

}  // namespace

PpcpHostPeer::PpcpHostPeer(Config cfg)
    : m_cfg(std::move(cfg)), m_policy(m_cfg.ingest)
{
    m_scratch.resize(kPumpBuffer);
}

PpcpHostPeer::~PpcpHostPeer() = default;

bool PpcpHostPeer::declareSelf(const PpcpSourceDeclaration::Inventory &inv, std::string *err)
{
    if (!m_declaration.build(m_cfg.peerId, inv, err)) return false;

    // Validated before it can be sent, through libppcp's own validators. A
    // declaration that fails I19 must not reach the wire, and finding that out
    // at the encoder rather than here would mean the host had already committed
    // to a session it cannot describe itself in.
    std::string where;
    if (m_declaration.validate(&where) != PPCP_OK) {
        if (err) *err = "declaration is not valid: " + where;
        return false;
    }
    return true;
}

IngestVerdict PpcpHostPeer::evaluate(const ppcp_peer_desc &counterpart) const
{
    return m_policy.evaluate(counterpart);
}

ppcp_result PpcpHostPeer::readiness(ppcp_readiness *out) const
{
    if (!out) return PPCP_ERR_INVALID;

    // CORE 5.15's question is "if I arm now, will the first shot have settled
    // exposure?" — a measurement, not a state name (5.15a).
    //
    // Order matters: a BLOCKED reason is the more informative answer, so the
    // two conditions that make arming refusable are checked before settledness.

    // CORE §9 / 5.14g1 — "a peer under storage pressure REFUSES TO ARM rather
    // than dropping swings a consumer has not received". The floor is this
    // application's, not the protocol's; the reason on the wire is 5.15's.
    std::uint64_t freeBytes = 0;
    if (m_storage && m_storage(&freeBytes) && freeBytes < m_cfg.storageFloorBytes) {
        ppcp_readiness_not_settled(out, 0);
        return ppcp_readiness_set_blocked(out, "storage_full");
    }

    // CORE 5.8's ThermalLevel is an ordinal protocol vocabulary, not a platform
    // passthrough: `critical` is the level at which this host cannot become
    // ready at all, and the mapping from whatever the OS said is the
    // embedding's (the ThermalFn).
    ppcp_thermal_level thermal = PPCP_THERMAL_NOMINAL;
    if (m_thermal && m_thermal(&thermal) && thermal == PPCP_THERMAL_CRITICAL) {
        ppcp_readiness_not_settled(out, 0);
        return ppcp_readiness_set_blocked(out, "thermal_limit");
    }

    // A host owning no Sources is not "blocked": it has nothing to arm and
    // nothing is wrong with it. CORE 5.6.1 — Source count is not a role marker.
    // `no_source` is for a peer that ought to have one and does not, so it is
    // reported only when this host has declared and found nothing.
    if (m_declaration.peer() && m_declaration.sourceCount() == 0) {
        ppcp_readiness_not_settled(out, 0);
        return ppcp_readiness_set_blocked(out, "no_source");
    }

    // Host cameras are machine-vision cameras under manual exposure: there is
    // no auto-exposure loop to settle, so the honest answer is `settled`. When
    // a host camera under an automatic mode arrives, this is where its
    // estimated time-to-settled goes, measured and not guessed.
    return ppcp_readiness_settled(out);
}

void PpcpHostPeer::attach(PeerConnection *link, PpcpEngine *engine)
{
    m_link = link;
    m_engine = engine;
    m_stats = PumpStats{};
}

bool PpcpHostPeer::pump()
{
    if (!m_link || !m_engine) return false;

    bool alive = false;

    for (Channel ch : m_link->channels()) {
        TransportChannel *tc = m_link->channel(ch);
        if (!tc || !tc->isOpen()) continue;
        alive = true;
        const std::uint8_t chNo = static_cast<std::uint8_t>(ch);

        // ── Socket to engine ───────────────────────────────────────────────
        // Read what is there and no more. A channel with nothing on it returns
        // WouldBlock, which is not an error and not a reason to stop looking at
        // the others: ENC 2d makes cross-channel arrival order explicitly
        // unordered, so a control frame must not wait on a bulk read.
        //
        // ⚠ THE TAIL IS OURS TO KEEP (L6). ppcp_peer_feed() consumes WHOLE
        // frames and reports how many bytes it took; a socket read ends
        // wherever the network felt like ending it, so the remainder is a
        // partial frame that must be re-presented with the next read behind
        // it. The engine deliberately buffers nothing (peer.h: nine megabytes
        // per link for a caller that already has the bytes), which makes this
        // buffer the embedding's obligation under ground rule 7 and not an
        // optimisation.
        std::vector<std::uint8_t> &tail = m_tails[chNo];
        for (;;) {
            std::size_t got = 0;
            const std::size_t keep = tail.size();
            if (keep >= m_scratch.size()) {
                // A frame larger than the whole working buffer. ENC §8's limits
                // are 1 MiB control / 8 MiB bulk, so grow rather than refuse:
                // refusing here would be this repository inventing a limit the
                // specification does not have (I14's discipline, applied to
                // sizes).
                m_scratch.resize(keep + kPumpBuffer);
            }
            std::memcpy(m_scratch.data(), tail.data(), keep);
            const IoStatus st = tc->read(m_scratch.data() + keep, m_scratch.size() - keep, got);
            if (st == IoStatus::Ok && got > 0) {
                m_stats.bytesIn += got;
                // The engine gets the channel with the bytes because ENC 2c's
                // "the channel in the header matches the stream it arrived on"
                // is the engine's check to make, not ours — we are the only
                // thing that knows which stream this was.
                std::size_t consumed = 0;
                const ppcp_result r =
                    m_engine->feed(chNo, m_scratch.data(), keep + got, &consumed);
                tail.assign(m_scratch.begin() + static_cast<std::ptrdiff_t>(consumed),
                            m_scratch.begin() + static_cast<std::ptrdiff_t>(keep + got));
                if (r == PPCP_ERR_FATAL_LIMIT || r == PPCP_ERR_MALFORMED) {
                    // ENC 8a — a length beyond the channel limit means the
                    // stream has desynchronised and cannot be resynchronised.
                    // The link is finished; nothing is skipped and retried.
                    tc->close();
                    m_stats.closed = true;
                    return false;
                }
                if (keep + got < m_scratch.size()) break;
                continue;   // the buffer filled; there may be more waiting
            }
            if (st == IoStatus::WouldBlock) break;
            if (st == IoStatus::Closed || st == IoStatus::Error) {
                tc->close();
                m_stats.closed = true;
                alive = false;
                break;
            }
            break;
        }
        if (!tc->isOpen()) continue;

        // ── Engine to socket ───────────────────────────────────────────────
        for (;;) {
            std::size_t len = 0;
            const ppcp_result r = m_engine->drain(chNo, m_scratch.data(), m_scratch.size(), &len);
            if (r != PPCP_OK || len == 0) break;

            std::size_t written = 0;
            const IoStatus st = tc->write(m_scratch.data(), len, written);
            m_stats.bytesOut += written;
            if (st == IoStatus::WouldBlock || written < len) {
                // CORE T2's backpressure, observed rather than assumed. The
                // engine has more than this channel can take right now; the
                // OTHER channels are untouched, which is the entire reason
                // there are two of them (CORE §3.1).
                //
                // ⚠ STILL A REAL GAP, AND L6 DID NOT CLOSE IT. The engine
                // handed us bytes it now considers sent, and a short write
                // loses them. ppcp_peer_feed() gained an `out_consumed` for
                // exactly this problem in the inbound direction;
                // ppcp_peer_drain() has no matching "I only took N" and
                // dequeues whole frames on the way out. Reported to the
                // orchestrator as the remaining asymmetry; until it is
                // answered, the pump must not offer a channel more than its
                // socket will take, and it counts the times it did.
                ++m_stats.wouldBlockOnWrite;
                break;
            }
            if (st == IoStatus::Closed || st == IoStatus::Error) {
                tc->close();
                m_stats.closed = true;
                alive = false;
                break;
            }
        }
    }

    return alive;
}

// ── The binding to libppcp's engine (L6) ────────────────────────────────────
//
// ⚠ IT MOVED, AND THE MOVE IS THE POINT. Building the peer used to live here,
// beside the socket pump, which made the bundle path of H3 either a second
// construction of the same engine or a dependency on OpenSSL to read a file.
// ppcp_host_engine.cpp is now the one place that says what a PinPointStudio
// peer is, and both transports ask it — which is what makes "the same ingest
// path" a fact about the code rather than a claim in a comment.

std::unique_ptr<PpcpEngine> PpcpHostPeer::makeLibppcpEngine(std::string *whyNot)
{
    HostEngineConfig cfg;
    cfg.peerId = m_cfg.peerId;
    cfg.policy = &m_policy;
    cfg.health = [this](ppcp_readiness *out) { return this->readiness(out); };
    cfg.listener = true;
    return makeHostEngine(std::move(cfg), whyNot);
}

}  // namespace Ppcp
