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

#include "ppcp_ingest_policy.h"

namespace Ppcp {
namespace {

// MSG §10 / CORE 10.3 — `reason` is an open-registry Kind, so a peer that does
// not know these strings treats them as opaque and does not fail (I13).
constexpr const char *kReasonRate      = "insufficient_rate";
constexpr const char *kReasonNoProfile = "no_acceptable_profile";

}  // namespace

bool IngestVerdict::profileAccepted(const std::string &sourceId,
                                    const std::string &profileId) const
{
    for (const Note &n : notes)
        if (n.sourceId == sourceId && n.profileId == profileId) return n.accepted;
    return accepted;
}

IngestVerdict PpcpIngestPolicy::evaluate(const ppcp_peer_desc &counterpart) const
{
    IngestVerdict v;

    std::size_t cameraSources = 0;
    std::size_t usableCameraProfiles = 0;

    for (std::size_t i = 0; i < counterpart.source_count; ++i) {
        const ppcp_source &s = counterpart.sources[i];

        // Only camera profiles are rate-judged. A microphone has no frame rate
        // and a wrist sensor has no frames; applying a frame-rate floor to them
        // would be this host inventing a requirement the protocol does not
        // have, which is the same error one level up from I14.
        if (!ppcp_source_kind_is_camera(&s)) continue;
        ++cameraSources;

        for (std::size_t j = 0; j < s.profile_count; ++j) {
            const ppcp_capture_profile &p = s.profiles[j];

            if (!p.rate.present) {
                if (m_limits.acceptProfileWithNoDeclaredRate) ++usableCameraProfiles;
                else
                    v.notes.push_back({ s.id.v, p.id.v, false, kReasonRate });
                continue;
            }

            // The claim is `nominal_mhz`. `max_mhz` is what the sensor can be
            // pushed to under conditions the peer has not promised, and
            // accepting on it would be accepting a capability nobody declared
            // they would deliver — which is exactly the distinction CORE §5.8
            // draws between claimed, measured and achieved.
            if (p.rate.nominal_mhz >= m_limits.minCameraRateMhz) {
                ++usableCameraProfiles;
            } else {
                // MSG 3.4c — noted, not fatal. The peer keeps the connection
                // and keeps every other profile; this one may not be opened.
                v.notes.push_back({ s.id.v, p.id.v, false, kReasonRate });
            }
        }
    }

    // A peer with NO camera Sources is accepted without comment. CORE 5.6.1:
    // "Source count is not a role marker"; a peer that only nominates, only
    // observes, or only carries a wrist sensor has nothing here to judge, and
    // a host that rejected it would have turned an ingest policy about video
    // into a rule about who may connect.
    if (cameraSources == 0) return v;

    if (usableCameraProfiles == 0) {
        // MSG 3.4a — machine-readable, and the connection stays up. The peer
        // is told what it would have to change; nothing is closed, nothing is
        // silently dropped, and a `readiness` or an annotation from it is still
        // welcome.
        v.accepted = false;
        v.reason = counterpart.source_count > 0 ? kReasonRate : kReasonNoProfile;
    }
    return v;
}

}  // namespace Ppcp
