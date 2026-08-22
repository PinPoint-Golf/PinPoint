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

#include "ppcp_host_engine.h"

#include <vector>

#include <ppcp/peer.h>

#include "ppcp_ingest_policy.h"
#include "ppcp_source_declaration.h"

namespace Ppcp {
namespace {

class LibppcpEngine final : public PpcpEngine {
public:
    static std::unique_ptr<LibppcpEngine> make(HostEngineConfig cfg, std::string *whyNot)
    {
        auto e = std::unique_ptr<LibppcpEngine>(new LibppcpEngine());
        e->m_cfg = std::move(cfg);
        e->m_storage.resize(ppcp_peer_sizeof());

        ppcp_peer_config pc{};
        pc.role = PPCP_ROLE_HOST;
        // CONF 2a — the injected clock where a test supplied one, and the real
        // host clock otherwise.  Not a fallback with an opinion: a null `now`
        // is the absence of an override, not a broken clock.
        pc.clock = e->m_cfg.clock.now ? e->m_cfg.clock : hostClock();
        pc.peer_id = e->m_cfg.peerId.c_str();

        // ENC 2.1a — the listener half of erratum E1. H1's PeerListener binds
        // by `link_id` before a byte reaches here, and a listener that sent a
        // `link_bind` of its own would be answering a binding with a binding.
        pc.listener = e->m_cfg.listener;

        // The Studio profile set, as the claim file states it (plan §2).
        e->m_profileNames = PpcpSourceDeclaration::studioProfiles();
        e->m_profilePtrs.reserve(e->m_profileNames.size());
        for (const std::string &s : e->m_profileNames) e->m_profilePtrs.push_back(s.c_str());
        pc.profiles = e->m_profilePtrs.data();
        pc.profile_count = e->m_profilePtrs.size();

        // MSG 3.1b / CORE 10.1e: NULL is "the one version this library speaks"
        // and NULL again is "the oldest it accepts". A host that named a window
        // here would be making a compatibility decision libppcp already makes
        // correctly; it becomes ours the day we must speak to something older.
        pc.versions = nullptr;
        pc.version_count = 0;
        pc.min_version = nullptr;

        // ⚠ I14 / CT-I14 — A CALLBACK, NEVER A NUMBER. The 120 fps floor is in
        // ppcp_ingest_policy.h, in this repository, and libppcp never sees it.
        pc.ingest_policy = &LibppcpEngine::ingestTrampoline;
        pc.health = &LibppcpEngine::healthTrampoline;
        // CORE 7.4b — degradation is REPORTED, not silently absorbed. Null when
        // the embedding supplied no reading, so `heartbeat_ack` says nothing
        // rather than saying "nominal" on no evidence.
        if (e->m_cfg.healthReport) pc.health_report = &LibppcpEngine::healthReportTrampoline;
        // MSG 6.1b — the clock this host stamps `t2`/`t3` on. `tb:host` and no
        // other, because hostClock() answers for no other timebase (I1); a
        // host that named a timebase it does not read would be fabricating the
        // very instant §6.3 exists to measure.
        if (!e->m_cfg.syncTimebase.empty()) pc.sync_timebase = e->m_cfg.syncTimebase.c_str();
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

    ppcp_result feed(std::uint8_t channel, const std::uint8_t *b, std::size_t n,
                     std::size_t *consumed) override
    {
        return ppcp_peer_feed(m_peer, channel, b, n, consumed);
    }
    ppcp_result drain(std::uint8_t channel, std::uint8_t *out, std::size_t cap,
                      std::size_t *len) override
    {
        return ppcp_peer_drain(m_peer, channel, out, cap, len);
    }
    ppcp_result drainPeek(std::uint8_t channel, const std::uint8_t **out,
                          std::size_t *len) override
    {
        return ppcp_peer_drain_peek(m_peer, channel, out, len);
    }
    ppcp_result drainCommit(std::uint8_t channel, std::size_t written) override
    {
        return ppcp_peer_drain_commit(m_peer, channel, written);
    }
    ppcp_peer *peer() const override { return m_peer; }

private:
    LibppcpEngine() = default;

    // I14 / MSG 3.4a — the verdict is ours and so is the REASON. L6 added
    // `out_reason` so a rejection carries a machine-readable Kind without the
    // library ever holding a threshold: it asks, and writes down the answer.
    static bool ingestTrampoline(void *ctx, const ppcp_peer_desc *counterpart,
                                 ppcp_id *out_reason)
    {
        LibppcpEngine *self = static_cast<LibppcpEngine *>(ctx);
        if (!self || !counterpart) return false;
        if (!self->m_cfg.policy) return true;
        const IngestVerdict v = self->m_cfg.policy->evaluate(*counterpart);
        if (!v.accepted && out_reason && !v.reason.empty())
            (void)ppcp_id_set_z(out_reason, v.reason.c_str());
        return v.accepted;
    }
    static ppcp_result healthTrampoline(void *ctx, ppcp_readiness *out)
    {
        LibppcpEngine *self = static_cast<LibppcpEngine *>(ctx);
        if (!self || !self->m_cfg.health) return PPCP_ERR_INVALID;
        return self->m_cfg.health(out);
    }
    static ppcp_result healthReportTrampoline(void *ctx, ppcp_health *out)
    {
        LibppcpEngine *self = static_cast<LibppcpEngine *>(ctx);
        if (!self || !self->m_cfg.healthReport) return PPCP_ERR_INVALID;
        return self->m_cfg.healthReport(out);
    }

    HostEngineConfig          m_cfg;
    ppcp_peer                *m_peer = nullptr;
    std::vector<std::uint8_t> m_storage;
    std::vector<std::string>  m_profileNames;
    std::vector<const char *> m_profilePtrs;
};

}  // namespace

std::unique_ptr<PpcpEngine> makeHostEngine(HostEngineConfig cfg, std::string *whyNot)
{
    return LibppcpEngine::make(std::move(cfg), whyNot);
}

}  // namespace Ppcp
