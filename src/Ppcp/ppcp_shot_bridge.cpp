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

#include "ppcp_shot_bridge.h"

#include <algorithm>
#include <cstring>

namespace Ppcp {
namespace {

std::string idStr(const ppcp_id &id) { return std::string(id.v, id.len); }

}  // namespace

PpcpShotBridge::PpcpShotBridge() = default;
PpcpShotBridge::~PpcpShotBridge() = default;

void PpcpShotBridge::attach(ppcp_peer *peer, const PpcpSourceDeclaration *declaration,
                            const PpcpLiveSession *session)
{
    stop();
    m_peer = peer;
    m_declaration = declaration;
    m_session = session;
}

void PpcpShotBridge::detach()
{
    stop();
    m_peer = nullptr;
    m_declaration = nullptr;
    m_session = nullptr;
}

ppcp_result PpcpShotBridge::idTrampoline(void *ctx, ppcp_id *out)
{
    PpcpShotBridge *self = static_cast<PpcpShotBridge *>(ctx);
    if (!self || !out || !self->m_idFn) return PPCP_ERR_INVALID;
    std::string s;
    if (!self->m_idFn(&s) || s.empty()) return PPCP_ERR_INVALID;
    return ppcp_id_set(out, s.c_str(), s.size());
}

bool PpcpShotBridge::policyTrampoline(void *ctx, const ppcp_candidate *c,
                                      const ppcp_timebase_relation *rel, double sigmaNs)
{
    // 8.2d — exclude-and-retain, and this is the ONLY thing in this application
    // that decides "too uncertain".  It is called only where a relation EXISTS:
    // a missing or `unrelated` one is decided by the specification, not by
    // policy, and libppcp never asks about those.
    PpcpShotBridge *self = static_cast<PpcpShotBridge *>(ctx);
    (void)c;
    (void)rel;
    if (!self) return true;
    if (sigmaNs > self->m_cfg.maxConversionSigmaNs) {
        ++self->m_stats.excluded;
        return false;   // excluded from setting `t0`; STILL in Shot.candidates (I8)
    }
    return true;
}

bool PpcpShotBridge::start(const Config &cfg, IdFn idFn, std::string *err)
{
    stop();
    if (!m_peer) { if (err) *err = "no peer attached"; return false; }
    if (!idFn) { if (err) *err = "no id source: 8.3e forbids the library minting one"; return false; }
    m_cfg = cfg;
    m_idFn = std::move(idFn);

    m_storage.assign(ppcp_arbiter_sizeof(), 0);
    ppcp_arbiter *a = nullptr;
    // I20 — refused for a peer that is not `role: host` or does not declare
    // Arbitrate.  Reported, never worked around: a host that arbitrated without
    // declaring it fails CONF §1d, and the failure would be silent on the wire.
    const ppcp_result r = ppcp_arbiter_new(m_storage.data(), m_storage.size(), m_peer,
                                           &PpcpShotBridge::idTrampoline, this, &a);
    if (r != PPCP_OK || !a) {
        if (err) *err = std::string("ppcp_arbiter_new: ") + ppcp_result_str(r);
        m_storage.clear();
        return false;
    }
    (void)ppcp_arbiter_set_policy(a, &PpcpShotBridge::policyTrampoline, this);
    m_arbiter = a;
    m_reported.clear();
    return true;
}

void PpcpShotBridge::stop()
{
    m_arbiter = nullptr;
    m_storage.clear();
    m_reported.clear();
}

const ppcp_source *PpcpShotBridge::ownSource(const std::string &sourceId) const
{
    if (!m_declaration) return nullptr;
    for (const ppcp_source &s : m_declaration->sources())
        if (idStr(s.id) == sourceId) return &s;
    return nullptr;
}

const ppcp_capture_profile *PpcpShotBridge::profileForSource(const ppcp_source *s) const
{
    // 5.12e / I33 — the profile supplies `timing.convention` and, for
    // `nominal_frame_start`, the offset.  A Source whose profile has no
    // `format` — a microphone, an IMU — takes NULL: 6.1d fixes `convention:
    // mid` there and the canonical instant is the raw instant, so passing a
    // profile that has no exposure to apply would be pretending there is one.
    if (!s || s->profile_count == 0) return nullptr;
    for (std::size_t i = 0; i < s->profile_count; ++i)
        if (s->profiles[i].format.present) return &s->profiles[i];
    return nullptr;
}

bool PpcpShotBridge::nominate(const std::string &sourceId, const char *basis,
                              std::int64_t rawHostNs, std::int64_t exposureNs,
                              double confidence, const ppcp_estimate *tof,
                              std::string *outCandidateId, std::string *err)
{
    if (!m_peer || !m_arbiter) { if (err) *err = "arbitration is not running"; return false; }

    const ppcp_source *src = ownSource(sourceId);
    if (!src) {
        // I26 / 5.12a / 7.1a — a Candidate names a Source THIS peer declared,
        // that Source names a Timebase it declared, and `at` is expressed in
        // that timebase.  Refused here as well as by libppcp, so the counter
        // records a detector wired to a Source the declaration never carried
        // instead of the failure arriving as a generic INVALID.
        ++m_stats.nominationsRefused;
        if (err) *err = "no declared Source named " + sourceId + " (I26)";
        return false;
    }

    std::string cid;
    if (!m_idFn || !m_idFn(&cid) || cid.empty()) {
        if (err) *err = "could not mint a Candidate id";
        return false;
    }

    // I33 / 5.12e — the canonical-instant conversion, applied ONCE, here, by
    // the nominator, because only the nominator holds the exposure and the
    // Source's `timing`.  8.2a then forbids the arbiter applying it again.
    ppcp_candidate c{};
    const ppcp_result mr = ppcp_candidate_make_canonical(
        &c, cid.c_str(), src, profileForSource(src), basis, rawHostNs,
        exposureNs, confidence, tof);
    if (mr != PPCP_OK) {
        if (err) *err = std::string("ppcp_candidate_make_canonical: ") + ppcp_result_str(mr);
        return false;
    }

    // 7.1d — EVERY nomination is emitted, before this host knows whether it
    // will win.  Emitting only winners destroys the evidence that explains why
    // detection fired, and CT-I8 asserts the loser survives.
    const ppcp_result nr = ppcp_peer_nominate(m_peer, &c);
    if (nr != PPCP_OK) {
        if (err) *err = std::string("ppcp_peer_nominate: ") + ppcp_result_str(nr);
        return false;
    }
    ++m_stats.nominated;

    // 8.2a — and the host arbitrates its OWN Candidates on the same terms as
    // anyone else's.  A host that grouped only foreign candidates would have a
    // per-peer slot, which is the same defect as a per-modality one.
    bool excluded = false;
    (void)ppcp_arbiter_observe(m_arbiter, &c, &excluded);

    if (outCandidateId) *outCandidateId = cid;
    return true;
}

void PpcpShotBridge::observe(const ppcp_event &ev)
{
    if (!m_arbiter || !ev.msg) return;
    switch (ev.kind) {
    case PPCP_EVENT_CANDIDATE: {
        bool excluded = false;
        if (ppcp_arbiter_observe(m_arbiter, &ev.msg->body.candidate.candidate, &excluded)
            == PPCP_OK)
            ++m_stats.observedForeign;
        break;
    }
    case PPCP_EVENT_SHOT:
        // 8.2k — a DEVICE-minted Shot referencing a Candidate this host still
        // holds is NOT competed with: the host attaches its own Candidates to
        // it and issues nothing of its own (I35).  8.2l — where both issued,
        // neither is withdrawn and the host emits `shot_link` with `basis:
        // shared_candidate`.  Both are libppcp's, and both are why this arm
        // hands the Shot straight over rather than deciding anything.
        if (ppcp_arbiter_observe_shot(m_arbiter, &ev.msg->body.shot.shot) == PPCP_OK)
            ++m_stats.adopted;
        break;
    case PPCP_EVENT_CAPTURE_REQUEST:
        // 8.4b — answered with a Capture, possibly `absent` with
        // `absent_reason: outside_buffer`, and NEVER with an `error`: an absent
        // capture is a result, not a failure (I10).
        ++m_stats.captureRequests;
        if (m_onCaptureRequest)
            m_onCaptureRequest(ev.msg->body.capture_request, ev.msg->env.msg_id);
        break;
    default:
        break;
    }
    collectIssued();
}

std::size_t PpcpShotBridge::pump(std::int64_t nowRefNs)
{
    if (!m_arbiter) return 0;
    std::size_t issued = 0;
    (void)ppcp_arbiter_pump(m_arbiter, nowRefNs, &issued);
    m_stats.issued = ppcp_arbiter_issued_count(m_arbiter);
    // 8.2h — a group issued after the mint deadline overlaps the window in
    // which the nominating peer is entitled to mint, and produces two Shots for
    // one event with no defect on either side.  Counted, because it is how a
    // host finds out it is running slow rather than finding out from a user.
    m_stats.late = ppcp_arbiter_late_count(m_arbiter);
    collectIssued();
    return issued;
}

void PpcpShotBridge::collectIssued()
{
    if (!m_arbiter || !m_onShot) return;
    const std::size_t n = ppcp_arbiter_group_count(m_arbiter);
    for (std::size_t i = 0; i < n; ++i) {
        const ppcp_shot *s = ppcp_arbiter_shot_at(m_arbiter, i);
        if (!s) continue;
        const std::string id = idStr(s->id);
        if (id.empty()) continue;
        if (std::find(m_reported.begin(), m_reported.end(), id) != m_reported.end()) continue;
        m_reported.push_back(id);
        m_onShot(*s);
    }
}

bool PpcpShotBridge::requestCapture(const std::string &shotId, std::int64_t t0RefNs,
                                    const std::vector<std::string> &streamIds,
                                    std::int64_t preNs, std::int64_t postNs,
                                    std::string *err)
{
    if (!m_peer) { if (err) *err = "no peer attached"; return false; }
    if (streamIds.empty()) { if (err) *err = "capture_request names at least one Stream"; return false; }

    // 5.13c — `t0` is in `Session.timebase_ref`, which for this host is
    // `tb:host`.  The owner inverts §6.1's conversion at its end when it
    // expresses the interval in its own convention; this host does not, and
    // must not, do it for them.
    const std::string ref = m_session ? m_session->config().timebaseRef
                                      : std::string(kHostTimebaseId);
    ppcp_instant t0{};
    if (ppcp_instant_make(&t0, ref.c_str(), ref.size(), t0RefNs) != PPCP_OK) {
        if (err) *err = "t0 is not a valid Instant on " + ref;
        return false;
    }

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

    const ppcp_result r = ppcp_peer_capture_request(m_peer, shotId.c_str(), &t0,
                                                    ids.data(), ids.size(), preNs, postNs);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_peer_capture_request: ") + ppcp_result_str(r);
        return false;
    }
    return true;
}

bool PpcpShotBridge::linkForeignShot(const std::string &localShotId,
                                     const std::string &foreignShotId,
                                     const std::string &foreignSystem, double confidence,
                                     std::string *err)
{
    if (!m_peer) { if (err) *err = "no peer attached"; return false; }
    if (!m_idFn) { if (err) *err = "no id source"; return false; }

    std::string lid;
    if (!m_idFn(&lid) || lid.empty()) { if (err) *err = "could not mint a ShotLink id"; return false; }

    ppcp_shot_link l{};
    // 8.1, middle column — a LIVE ASSOCIATION.  `arrival_pairing` is not one of
    // the three retrospective bases (5.16b/f), which is exactly why it may be
    // confirmed by the observer rather than needing a human.
    ppcp_result r = ppcp_shot_link_make(&l, lid.c_str(), localShotId.c_str(),
                                        foreignShotId.c_str(), PPCP_LINK_ARRIVAL_PAIRING,
                                        confidence);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_shot_link_make: ") + ppcp_result_str(r);
        return false;
    }
    // 5.16e — there is no way to set `confirmed` without saying which kind it
    // was, and this is the kind: the host armed the slot when it detected the
    // swing and observed the reading arrive.
    r = ppcp_shot_link_confirm(&l, PPCP_CONFIRMED_BY_OBSERVER);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_shot_link_confirm: ") + ppcp_result_str(r);
        return false;
    }
    if (!foreignSystem.empty()) {
        r = ppcp_shot_link_set_foreign_system(&l, foreignSystem.c_str());
        if (r != PPCP_OK) {
            if (err) *err = std::string("ppcp_shot_link_set_foreign_system: ")
                            + ppcp_result_str(r);
            return false;
        }
    }

    r = ppcp_peer_shot_link(m_peer, &l);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_peer_shot_link: ") + ppcp_result_str(r);
        return false;
    }
    ++m_stats.shotLinks;
    return true;
}

const PpcpShotBridge::Stats &PpcpShotBridge::stats() const { return m_stats; }

std::size_t PpcpShotBridge::retainedCount() const
{
    return m_arbiter ? ppcp_arbiter_retained_count(m_arbiter) : 0;
}

std::size_t PpcpShotBridge::groupCount() const
{
    return m_arbiter ? ppcp_arbiter_group_count(m_arbiter) : 0;
}

}  // namespace Ppcp
