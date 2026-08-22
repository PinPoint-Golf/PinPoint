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

#include "ppcp_live_session.h"

#include <algorithm>
#include <cstring>

namespace Ppcp {
namespace {

std::string idStr(const ppcp_id &id)
{
    return std::string(id.v, id.len);
}

}  // namespace

PpcpLiveSession::PpcpLiveSession() = default;
PpcpLiveSession::~PpcpLiveSession() = default;

void PpcpLiveSession::attach(ppcp_peer *peer, const PpcpSourceDeclaration *declaration)
{
    m_peer = peer;
    m_declaration = declaration;
    m_open = false;
    m_localTimebases.clear();
    m_peerHealth = PeerHealth{};
    m_lastLinkState = PPCP_LINK_LIVE;
    m_publishedWith = 0;
}

void PpcpLiveSession::detach()
{
    m_peer = nullptr;
    m_declaration = nullptr;
    m_open = false;
    m_localTimebases.clear();
}

bool PpcpLiveSession::registerEstimators(std::string *err)
{
    // I21 / CORE 6.1d — one probe sequence per timebase THIS HOST declares.
    //
    // The list comes from the declaration and not from a constant, which is the
    // whole difference the invariant turns on: a host sampling four cameras
    // against four free-running clocks declares four Timebases, and each needs
    // its own measured relation because 5.4c and I18 forbid deriving the second
    // from the first.  `timebase_ref` itself is always registered, even when
    // the declaration is absent, because a host that could not relate its own
    // reference clock to a device would have nothing to arbitrate with.
    m_localTimebases.clear();

    auto add = [&](const std::string &tb) {
        if (tb.empty()) return;
        if (std::find(m_localTimebases.begin(), m_localTimebases.end(), tb)
            != m_localTimebases.end())
            return;
        m_localTimebases.push_back(tb);
    };

    add(m_cfg.timebaseRef);
    if (m_declaration && m_declaration->peer()) {
        const ppcp_peer_desc *self = m_declaration->peer();
        for (std::size_t i = 0; i < self->timebase_count; ++i)
            add(idStr(self->timebases[i].id));
    }

    for (const std::string &tb : m_localTimebases) {
        // `remote_tb` is NULL: 6.1b says a prober does not know which clock a
        // responder stamps on until the responder says so, and inventing one
        // would be an assumption dressed as a measurement.
        const ppcp_result r = ppcp_peer_sync_add_timebase(m_peer, tb.c_str(), nullptr);
        if (r != PPCP_OK) {
            if (err) *err = "ppcp_peer_sync_add_timebase(" + tb + "): "
                            + ppcp_result_str(r);
            return false;
        }
    }
    m_stats.syncEstimators = ppcp_peer_sync_count(m_peer);
    return true;
}

bool PpcpLiveSession::open(const Config &cfg, std::string *err)
{
    if (!m_peer) { if (err) *err = "no peer attached"; return false; }
    if (ppcp_peer_get_role(m_peer) != PPCP_ROLE_HOST) {
        // I20 and 5.10e together: only a host opens a hosted Session, because
        // the two arbitration parameters are the statement that arbitration
        // occurs and a non-host asserting them would be claiming a role.
        if (err) *err = "session_open with arbitration parameters needs role: host";
        return false;
    }
    m_cfg = cfg;

    // 5.10e made structural by libppcp: ppcp_session_make_hosted takes both
    // parameters and there is no setter, so a hosted Session cannot be built
    // without them and a hostless one cannot be given them.
    ppcp_session s{};
    ppcp_result r = ppcp_session_make_hosted(&s, m_cfg.sessionId.c_str(),
                                             m_cfg.timebaseRef.c_str(),
                                             m_cfg.coincidenceWindowNs,
                                             m_cfg.issueHoldNs);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_session_make_hosted: ") + ppcp_result_str(r);
        return false;
    }
    r = ppcp_session_set_heartbeat_interval(&s, m_cfg.heartbeatIntervalMs);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_session_set_heartbeat_interval: ")
                        + ppcp_result_str(r);
        return false;
    }

    // ⚠ NO `epoch`.  I15 / CORE 5.3b: a wall-clock reading is a LABEL and is
    // never used to compute an interval, and this host computes every interval
    // from `tb:host`.  Setting one here would put a number on the wire that the
    // only honest use of is display — and H3's ingest path is asserted to
    // contain no reference to it at all.  If a label is ever wanted it belongs
    // beside the session record, not in the arbitration frame.

    r = ppcp_peer_session_open(m_peer, &s);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_peer_session_open: ") + ppcp_result_str(r);
        return false;
    }

    if (!registerEstimators(err)) return false;

    // 6.3c — a burst on connect.  Not a one-shot handshake: 6.3a needs offset
    // AND rate, and a rate needs two exchanges separated in time, which is why
    // ppcp_sync_estimator_relation() answers PPCP_ERR_NOT_FOUND until it has
    // them rather than publishing an optimistic zero skew.
    (void)ppcp_peer_sync_trigger(m_peer, PPCP_SYNC_ON_CONNECT);

    m_open = true;
    return true;
}

bool PpcpLiveSession::close(const char *reason, std::string *err)
{
    if (!m_peer || !m_open) { if (err) *err = "no open session"; return false; }
    const ppcp_result r = ppcp_peer_session_close(m_peer, reason);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_peer_session_close: ") + ppcp_result_str(r);
        return false;
    }
    m_open = false;
    return true;
}

void PpcpLiveSession::triggerSync(ppcp_sync_trigger why)
{
    if (!m_peer) return;
    (void)ppcp_peer_sync_trigger(m_peer, why);
}

void PpcpLiveSession::pump(std::int64_t nowNs)
{
    if (!m_peer) return;

    // 6.3d — TWO CADENCES.  The sync schedule (burst spacing, then one exchange
    // per five seconds under 6.3g) and the liveness schedule
    // (`heartbeat_interval_ms`) are separate concerns that share a channel, and
    // neither call reads the other's number.
    std::size_t probes = 0;
    if (ppcp_peer_sync_pump(m_peer, nowNs, &probes) == PPCP_OK) m_stats.probesQueued += probes;

    // 7.4a on this end (queue a due `heartbeat`), 7.4c on the other (count the
    // misses).  One call serves both because a host is both ends of liveness.
    (void)ppcp_peer_liveness_pump(m_peer, nowNs);

    const ppcp_link_state now = ppcp_peer_link_state(m_peer);
    if (now != m_lastLinkState) {
        if (now == PPCP_LINK_LOST) ++m_stats.linkLosses; else ++m_stats.linkRestores;
        m_lastLinkState = now;
        if (m_onLinkState) m_onLinkState(now);
    }

    publishRelations();
}

void PpcpLiveSession::publishRelations()
{
    // 6.1f — publish the current estimate for every registered timebase that
    // has one, as a single `relation_update`.  6.3a is the gate: an estimator
    // that has not yet fitted a rate has no relation, and this counts them so
    // "nothing published yet" is distinguishable from "nothing to publish".
    const std::size_t n = ppcp_peer_sync_count(m_peer);
    std::size_t with = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const ppcp_sync_estimator *e = ppcp_peer_sync_estimator_at(m_peer, i);
        if (e && ppcp_sync_estimator_has_estimate(e)) ++with;
    }
    m_stats.syncEstimators = n;
    m_stats.estimatorsWithoutEstimate = n - with;
    if (with == 0) return;
    if (with == m_publishedWith && m_stats.relationsPublished > 0) {
        // The same estimators still have estimates.  6.3e makes the published
        // offset FILTERED rather than stepped, so it moves continuously; the
        // maintenance cadence of 6.3g republishes it on its own schedule and
        // nothing is lost by not republishing on every pump.  What must never
        // be suppressed is a relation that did not exist before, which is why
        // the comparison is on the count of estimators that HAVE one.
        return;
    }

    std::size_t published = 0;
    if (ppcp_peer_publish_relations(m_peer, &published) == PPCP_OK && published > 0) {
        m_stats.relationsPublished += published;
        m_publishedWith = with;
        if (m_onRelations) m_onRelations();
    }
}

void PpcpLiveSession::observe(const ppcp_event &ev)
{
    switch (ev.kind) {
    case PPCP_EVENT_HEARTBEAT: {
        // 7.4b — the counterpart's degradation.  Only `heartbeat_ack` carries
        // it; a bare `heartbeat` is the host's own message coming back through
        // the event ring on a loopback fixture and says nothing about anyone's
        // thermal state.
        if (!ev.msg) break;
        if (std::strncmp(ev.msg->env.type, "heartbeat_ack", ev.msg->env.type_len) != 0
            || ev.msg->env.type_len != 13)
            break;
        const ppcp_body_heartbeat_ack &a = ev.msg->body.heartbeat_ack;
        m_peerHealth.valid = true;
        m_peerHealth.thermal = a.thermal;
        m_peerHealth.hasVendorLabel = a.has_vendor_label;
        m_peerHealth.vendorThermalLabel = a.has_vendor_label ? idStr(a.vendor_thermal_label)
                                                             : std::string();
        m_peerHealth.storageFreeBytes = a.storage_free_bytes;
        m_peerHealth.hasBatteryPct = a.has_battery_pct;
        m_peerHealth.batteryPct = a.battery_pct;
        m_peerHealth.hasCharging = a.has_charging;
        m_peerHealth.charging = a.charging;
        ++m_stats.heartbeatAcks;
        if (m_onHealth) m_onHealth(m_peerHealth);
        break;
    }
    case PPCP_EVENT_LINK_LOST:
        // 7.4c.  The engine raised it; pump() will see the same state and fire
        // the callback once, so this only keeps the count honest if a caller
        // drains events without pumping.
        if (m_lastLinkState != PPCP_LINK_LOST) {
            m_lastLinkState = PPCP_LINK_LOST;
            ++m_stats.linkLosses;
            if (m_onLinkState) m_onLinkState(PPCP_LINK_LOST);
        }
        break;
    case PPCP_EVENT_LINK_RESTORED:
        if (m_lastLinkState != PPCP_LINK_LIVE) {
            m_lastLinkState = PPCP_LINK_LIVE;
            ++m_stats.linkRestores;
            if (m_onLinkState) m_onLinkState(PPCP_LINK_LIVE);
        }
        break;
    case PPCP_EVENT_RELATION_UPDATE:
        // The engine has already folded it into ppcp_peer_relations(); the
        // callback exists so the camera seam can be re-evaluated at the new
        // relation rather than at the old one.
        if (m_onRelations) m_onRelations();
        break;
    default:
        break;
    }
}

bool PpcpLiveSession::arm(const std::vector<std::string> &streamIds, std::string *err)
{
    if (!m_peer) { if (err) *err = "no peer attached"; return false; }
    std::vector<ppcp_id> ids;
    ids.reserve(streamIds.size());
    for (const std::string &s : streamIds) {
        ppcp_id id{};
        if (ppcp_id_set(&id, s.c_str(), s.size()) != PPCP_OK) {
            if (err) *err = "stream id is not a valid Id: " + s;
            return false;
        }
        ids.push_back(id);
    }
    // MSG 5.2 — an EMPTY list means every open capture Stream, which is what
    // this application's single `armed` property means.
    const ppcp_result r = ppcp_peer_arm(m_peer, ids.empty() ? nullptr : ids.data(), ids.size());
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_peer_arm: ") + ppcp_result_str(r);
        return false;
    }
    return true;
}

bool PpcpLiveSession::disarm(const std::vector<std::string> &streamIds, std::string *err)
{
    if (!m_peer) { if (err) *err = "no peer attached"; return false; }
    std::vector<ppcp_id> ids;
    ids.reserve(streamIds.size());
    for (const std::string &s : streamIds) {
        ppcp_id id{};
        if (ppcp_id_set(&id, s.c_str(), s.size()) != PPCP_OK) {
            if (err) *err = "stream id is not a valid Id: " + s;
            return false;
        }
        ids.push_back(id);
    }
    const ppcp_result r = ppcp_peer_disarm(m_peer, ids.empty() ? nullptr : ids.data(),
                                           ids.size());
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_peer_disarm: ") + ppcp_result_str(r);
        return false;
    }
    return true;
}

bool PpcpLiveSession::isArmed() const
{
    return m_peer && ppcp_peer_is_armed(m_peer);
}

ppcp_link_state PpcpLiveSession::linkState() const
{
    return m_peer ? ppcp_peer_link_state(m_peer) : PPCP_LINK_LIVE;
}

std::uint32_t PpcpLiveSession::missedHeartbeats() const
{
    return m_peer ? ppcp_peer_missed_heartbeats(m_peer) : 0u;
}

bool PpcpLiveSession::zeroHost() const
{
    return m_peer && ppcp_peer_zero_host(m_peer);
}

const ppcp_relation_set *PpcpLiveSession::relations() const
{
    return m_peer ? ppcp_peer_relations(const_cast<ppcp_peer *>(m_peer)) : nullptr;
}

bool PpcpLiveSession::offsetToRefNs(const std::string &sourceTimebase, std::int64_t atNs,
                                    std::int64_t *outOffsetNs, double *outSigmaNs) const
{
    if (!m_peer || !outOffsetNs) return false;
    const ppcp_relation_set *rs = relations();
    if (!rs) return false;

    ppcp_instant in{};
    if (ppcp_instant_make(&in, sourceTimebase.c_str(), sourceTimebase.size(), atNs) != PPCP_OK)
        return false;
    ppcp_id to{};
    if (ppcp_id_set(&to, m_cfg.timebaseRef.c_str(), m_cfg.timebaseRef.size()) != PPCP_OK)
        return false;

    // ⚠ AT MOST ONE RELATION IS APPLIED, and that is libppcp's guarantee rather
    // than this function's discretion: ppcp_relations_convert refuses when it
    // holds no DIRECT relation (I18, 5.4c) and refuses again when the one it
    // holds is `unrelated` (5.4b).  Neither refusal is turned into a zero here.
    ppcp_instant out{};
    if (ppcp_relations_convert(rs, &in, &to, &out) != PPCP_OK) return false;

    // I4 — identity is identity.  A Source already on `tb:host` needs no
    // relation and gets an offset of zero, which is a fact and not a fallback:
    // the conversion succeeded because the timebases are the same, not because
    // a missing relation was assumed away.
    *outOffsetNs = out.ns - atNs;
    if (outSigmaNs) {
        double sigma = 0.0;
        if (ppcp_relations_sigma_ns(rs, &in, &to, &sigma) == PPCP_OK) *outSigmaNs = sigma;
        else *outSigmaNs = 0.0;
    }
    return true;
}

std::vector<std::string> PpcpLiveSession::relatedTimebases() const
{
    std::vector<std::string> out;
    const ppcp_relation_set *rs = relations();
    if (!rs) return out;
    ppcp_id to{};
    if (ppcp_id_set(&to, m_cfg.timebaseRef.c_str(), m_cfg.timebaseRef.size()) != PPCP_OK)
        return out;
    for (std::size_t i = 0; i < rs->count; ++i) {
        const ppcp_timebase_relation &r = rs->r[i];
        if (r.to.len != to.len || std::memcmp(r.to.v, to.v, to.len) != 0) continue;
        if (r.cls != PPCP_REL_AFFINE) continue;   // 5.4b — `unrelated` relates nothing
        out.push_back(idStr(r.from));
    }
    return out;
}

}  // namespace Ppcp
