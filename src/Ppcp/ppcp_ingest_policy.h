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

// ⚠ THIS FILE IS WHERE PINPOINTSTUDIO'S THRESHOLDS LIVE, AND IT IS THE ONLY ONE.
//
// CORE 5.7d: "MUST NOT any frame-rate, resolution, quality or confidence
// threshold appear in a profile or anywhere else in this specification (I14).
// Acceptance is host policy, expressed outside the protocol."
// MSG 3.4b: "MUST NOT any threshold that drives a rejection appear in this
// specification.  Whether 60 fps, a given resolution or a given noise figure is
// acceptable is the rejecting peer's own ingest policy."
//
// CT-I14 is a GREP: "grep the implementation's protocol layer for a frame-rate,
// resolution, quality or confidence constant.  Assert every such threshold
// lives in a policy layer above it."  So the numbers are gathered here, in one
// struct, in an application file, and libppcp is handed a CALLBACK — never a
// number.  planned.h says the same thing from the other side: "`ingest_policy`
// is a callback and not a number."
//
// The 120 fps floor is a PRODUCT decision about what PinPointStudio's swing
// analysis can measure from, not a protocol requirement and not a claim about
// what any other host should accept. A third-party host with a different
// analysis pipeline will have a different number, and the protocol carries
// neither.

#include <cstdint>
#include <string>
#include <vector>

#include <ppcp/model.h>

namespace Ppcp {

// CORE 7.2b / MSG 3.4a: a rejection carries a machine-readable reason and does
// NOT close the connection. A peer whose cameras are too slow for this host's
// analysis is still a peer: it can still nominate, still be arbitrated, still
// carry a preview.
struct IngestVerdict {
    // MSG 3.4c — a peer MAY reject individual profiles while accepting the
    // declaration as a whole, and "a profile rejected in `notes` MUST NOT be
    // activated by a later `stream_open`".
    struct Note {
        std::string sourceId;
        std::string profileId;
        bool        accepted = true;
        std::string reason;    // open-registry Kind; set only when rejected
    };

    bool accepted = true;
    std::string reason;        // required when the whole declaration is rejected
    std::vector<Note> notes;

    bool profileAccepted(const std::string &sourceId, const std::string &profileId) const;
};

class PpcpIngestPolicy {
public:
    struct Limits {
        // 120 fps, in the millihertz CORE 5.7 puts `rate` in. Below this the
        // impact frame is far enough from the true impact that the club-face
        // measurements this product exists to make are not recoverable — which
        // is a statement about this analysis pipeline and about nothing else.
        int64_t minCameraRateMhz = 120000;

        // A camera profile that declares no `rate` at all. CORE 5.7 makes
        // `rate` optional (0..1), so this is a real case rather than a
        // malformed one, and it is a POLICY question what to do about it. This
        // host accepts it and re-decides from `AchievedSummary.realised_rate_mhz`
        // when a Capture actually arrives, because I28's discipline cuts both
        // ways: an absent claim is not a claim of failure either.
        bool acceptProfileWithNoDeclaredRate = true;
    };

    PpcpIngestPolicy() = default;
    explicit PpcpIngestPolicy(const Limits &l) : m_limits(l) {}

    const Limits &limits() const { return m_limits; }
    void setLimits(const Limits &l) { m_limits = l; }

    // The decision, as a pure function of what the counterpart declared.
    //
    // It reads `rate` and NOTHING ELSE about how the counterpart got there: not
    // its product, not its role, not its platform. A slow camera is a slow
    // camera whoever made it, and a fast one is acceptable even if this host
    // has never heard of the vendor — which is the open-protocol commitment in
    // the one place a host is most tempted to break it.
    IngestVerdict evaluate(const ppcp_peer_desc &counterpart) const;

private:
    Limits m_limits;
};

}  // namespace Ppcp
