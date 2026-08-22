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

#if defined(PPCP_HAVE_PEER)
#  include <ppcp/peer.h>
#endif

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
        for (;;) {
            std::size_t got = 0;
            const IoStatus st = tc->read(m_scratch.data(), m_scratch.size(), got);
            if (st == IoStatus::Ok && got > 0) {
                m_stats.bytesIn += got;
                // The engine gets the channel with the bytes because ENC 2c's
                // "the channel in the header matches the stream it arrived on"
                // is the engine's check to make, not ours — we are the only
                // thing that knows which stream this was.
                const ppcp_result r = m_engine->feed(chNo, m_scratch.data(), got);
                if (r == PPCP_ERR_FATAL_LIMIT || r == PPCP_ERR_MALFORMED) {
                    // ENC 8a — a length beyond the channel limit means the
                    // stream has desynchronised and cannot be resynchronised.
                    // The link is finished; nothing is skipped and retried.
                    tc->close();
                    m_stats.closed = true;
                    return false;
                }
                if (got < m_scratch.size()) break;
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
                // ⚠ The undrained tail is a real gap: the engine handed us
                // bytes it now considers sent. It is closed when L6 lands and
                // the drain call can be told how much was actually taken —
                // reported to the orchestrator as a required shape for
                // ppcp_peer_drain.
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

#if defined(PPCP_HAVE_PEER)

namespace {

// The one thing on this side of the boundary that is libppcp's shape rather
// than ours. Everything above is the embedding's obligation under ground rule 7
// and is built and tested whether or not L6 exists.
class LibppcpEngine final : public PpcpEngine {
public:
    static std::unique_ptr<LibppcpEngine> make(const PpcpHostPeer::Config &cfg,
                                               const PpcpIngestPolicy *policy,
                                               std::string *whyNot)
    {
        auto e = std::unique_ptr<LibppcpEngine>(new LibppcpEngine());
        e->m_policy = policy;
        e->m_storage.resize(ppcp_peer_sizeof());

        ppcp_peer_config pc{};
        pc.role = PPCP_ROLE_HOST;
        pc.clock = hostClock();
        pc.peer_id = cfg.peerId.c_str();

        // The Studio profile set, as the claim file states it (plan §2).
        e->m_profileNames = PpcpSourceDeclaration::studioProfiles();
        e->m_profilePtrs.reserve(e->m_profileNames.size());
        for (const std::string &s : e->m_profileNames) e->m_profilePtrs.push_back(s.c_str());
        pc.profiles = e->m_profilePtrs.data();
        pc.profile_count = e->m_profilePtrs.size();

        // ⚠ I14 / CT-I14 — A CALLBACK, NEVER A NUMBER. The 120 fps floor is in
        // ppcp_ingest_policy.h, in this repository, and libppcp never sees it.
        pc.ingest_policy = &LibppcpEngine::ingestTrampoline;
        pc.health = &LibppcpEngine::healthTrampoline;
        pc.ctx = e.get();

        ppcp_peer *p = nullptr;
        const ppcp_result r =
            ppcp_peer_new(e->m_storage.data(), e->m_storage.size(), &pc, &p);
        if (r != PPCP_OK || !p) {
            if (whyNot) *whyNot = std::string("ppcp_peer_new: ") + ppcp_result_str(r);
            return nullptr;
        }
        e->m_peer = p;
        return e;
    }

    ~LibppcpEngine() override { if (m_peer) ppcp_peer_free(m_peer); }

    ppcp_result feed(std::uint8_t channel, const std::uint8_t *b, std::size_t n) override
    {
        return ppcp_peer_feed(m_peer, channel, b, n);
    }
    ppcp_result drain(std::uint8_t channel, std::uint8_t *out, std::size_t cap,
                      std::size_t *len) override
    {
        return ppcp_peer_drain(m_peer, channel, out, cap, len);
    }

    ppcp_peer *raw() const { return m_peer; }

private:
    LibppcpEngine() = default;

    static bool ingestTrampoline(void *ctx, const ppcp_peer_desc *counterpart)
    {
        LibppcpEngine *self = static_cast<LibppcpEngine *>(ctx);
        if (!self || !self->m_policy || !counterpart) return false;
        return self->m_policy->evaluate(*counterpart).accepted;
    }
    static ppcp_result healthTrampoline(void *ctx, ppcp_readiness *out)
    {
        LibppcpEngine *self = static_cast<LibppcpEngine *>(ctx);
        if (!self || !self->m_host) return PPCP_ERR_INVALID;
        return self->m_host->readiness(out);
    }

    ppcp_peer                *m_peer = nullptr;
    const PpcpIngestPolicy   *m_policy = nullptr;
    const PpcpHostPeer       *m_host = nullptr;
    std::vector<std::uint8_t> m_storage;
    std::vector<std::string>  m_profileNames;
    std::vector<const char *> m_profilePtrs;
};

}  // namespace

std::unique_ptr<PpcpEngine> PpcpHostPeer::makeLibppcpEngine(std::string *whyNot)
{
    return LibppcpEngine::make(m_cfg, &m_policy, whyNot);
}

#else

std::unique_ptr<PpcpEngine> PpcpHostPeer::makeLibppcpEngine(std::string *whyNot)
{
    // ⚠ NOT A STUB, AND DELIBERATELY NOT ONE.  planned.h says why: an
    // application that CALLS an unimplemented symbol should fail at BUILD time
    // with an undefined symbol naming the function, "which is a better
    // diagnostic than a stub returning PPCP_ERR_UNIMPLEMENTED at runtime".  So
    // this returns null and names the package, and nothing in this repository
    // pretends to be an engine in the meantime.
    if (whyNot)
        *whyNot = "libppcp work package L6 has not landed: <ppcp/peer.h> is a placeholder "
                  "and ppcp_peer_new / ppcp_peer_feed / ppcp_peer_drain / "
                  "ppcp_peer_declare have no definition in libppcp.a";
    return nullptr;
}

#endif

}  // namespace Ppcp
