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

// ---------------------------------------------------------------------------
// hm_session_types.h — the two results HmSessionWorker MEASURES
// ---------------------------------------------------------------------------
//
// Both of these were once nested inside HmInstance, which made the session
// worker — pure device-layer code — depend on the GUI controller that displays
// its output. They are the worker's OWN products: it is the thing that drains
// the presence event and the history block, and HmInstance only carries them
// across the thread boundary. So they live down here with the worker, and
// HmInstance aliases them (`using ReferenceAnchor = pinpoint::hm::…`) so call
// sites keep saying HmInstance::… for a type it still hands out.

#include <QQuaternion>
#include <QtGlobal>

#include <wrist/clock.h>       // wr_clock_snapshot — carried WITH the data

#include <utility>
#include <vector>

#include "imu_sample.h"             // pinpoint::ImuSample — the stitched lane's records

namespace pinpoint {
namespace hm {

// ── The reference-pose anchor, kept for Phase D's frame solve ─────────────
//
// Every field of wr_calibration_presence_event that cannot be re-derived once
// the pose has passed. C++ only: Phase D consumes it, QML has no business
// with it, and nothing here derives anything from it — the solve is Phase D's
// and this is the raw measurement the library already took.
//
// ⚠ BOTH FORMS ARE KEPT ON PURPOSE, because they answer different questions:
//
//   the MEAN   — the averaged pose, and the ONLY one a frame solve may use.
//                A person holding a declared pose still wobbles 0.5-2°, which
//                is one to two orders larger than Q14 quantisation (~0.007°),
//                and averaging the run is what removes it.
//   the MEDOID — one real measured pair, for the angle and its provenance.
//                ⚠ IT MUST NOT BE USED FOR THE SOLVE: it is selected on the
//                RELATIVE rotation, which is blind to a whole-arm movement
//                carrying both units together — precisely the motion that
//                contaminates an ABSOLUTE pose. However centrally it is
//                chosen, it still holds whatever the athlete was doing at
//                that instant.
//
// `valid` is the gate and the only gate. A quaternion has no NaN idiom, so
// there is no in-band sentinel: every other field is meaningless when it is
// false.
struct ReferenceAnchor {
    bool        valid = false;
    // (2) THE AVERAGED POSE — what a Phase D frame solve must use.
    QQuaternion qLowerArmMean, qPalmMean;
    // Per unit, [lowerArm, palm]. Check this before trusting the mean.
    float       poseSpreadDeg[2] = { 0.0f, 0.0f };
    // (1) The medoid record — for the angle and its provenance only.
    QQuaternion qLowerArmMedoid, qPalmMedoid;
    quint32     sampleIndex = 0;   // which record the medoid pair came from
    // ⚠ ONE RECORD'S DIFFERENCE, AND IT IS MOSTLY JITTER — NOT "the skew".
    // libwrist's 0x90 analysis measured 89 and 99 ticks on two consecutive
    // records of one capture against a session median of 59: a single reading is
    // dominated by ±½-sample pairing jitter, because the two units share a
    // sample index by construction (one record header) but run two free-running
    // MCU timers. Kept because it is what was measured at the medoid record and
    // it belongs with that record's provenance. ⚠ A frame solve that wants the
    // skew must take HmInstance::skewUsMedian() over a run, never this.
    qint32      skewUs      = 0;   // palm − lower_arm for THAT record
    quint8      samplesUsed = 0;
    float       relativeAngleDeg = 0.0f;
};

// ── Deferred history (Phase E) ──────────────────────────────────────────
//
// The device holds ~7.5 s internally and replays it at up to ~799 Hz against
// a live link carrying 25-100 Hz, so THE BEST DATA FOR A SWING ARRIVES AFTER
// THE SWING — seconds after the SwingWindow would once have been frozen.
// This is deferred_sources_design.md's first real consumer.
struct HistoryResult {
    // A block materialised. ⚠ TRUE DOES NOT MEAN GOOD: a timed-out, holed or
    // empty pull is an ordinary outcome and still produces a block carrying
    // its own coverage. Read `status` and the coverage fields, never this.
    bool valid = false;

    int  status   = -1;      // wr_history_status
    int  attempts = 0;       // how many `a1` requests were issued
    // ⚠ The interval list is then a SUPERSET and coverageFraction /
    // largestGapUs become OPTIMISTIC. Surfaced because an optimistic gap
    // list that does not say so reads as a clean pull.
    bool coverageOverflowed = false;

    // ⚠ THREE NUMBERS ANSWERING THREE DIFFERENT QUESTIONS, and no one of
    // them substitutes for another: how much of what we ASKED FOR arrived,
    // how closely spaced what arrived was, and the AVERAGE rate across it.
    double  coverageFraction = 0.0;
    double  density          = 0.0;
    double  achievedHz       = 0.0;
    // ⚠ THE NUMBER THAT DECIDES WHETHER IMPACT SURVIVED.
    quint32 largestGapUs     = 0;

    // ⚠ READ AS A PAIR. A zero MISMATCH count beside a zero SAMPLE count is
    // NO EVIDENCE, not agreement — either the pull covered a span live never
    // reached, or the digest ring was off.
    quint32 liveOverlapSamples    = 0;
    quint32 liveOverlapMismatches = 0;

    // ⚠ THE HOLE THIS PULL ITSELF CAUSED, and it falls OUTSIDE `requested`
    // by construction: the device stops counting samples while it replays
    // them, so the cost lands in whatever comes next. Nothing on the wire
    // marks it, and the block is the only artefact that survives — so a
    // consumer stitching a session has no other way to know a span was never
    // RECORDED rather than merely never REQUESTED. Empty when unmeasured.
    qint64 selfRecordingGapStartUs = 0;
    qint64 selfRecordingGapEndUs   = 0;

    qint64 requestedStartUs = 0;
    qint64 requestedEndUs   = 0;

    // ⚠ The three kinds MAY OVERLAP — read them as three independent
    // statements about one index axis, not as a partition of it.
    struct Gap { qint64 startUs; qint64 endUs; int kind; };
    std::vector<Gap> gaps;

    // Half-open, ascending, disjoint — what the pull actually delivered.
    std::vector<std::pair<qint64, qint64>> delivered;

    // ⚠ CARRIED BY VALUE so re-analysis a year later reproduces the day's
    // alignment. The fit is persisted WITH the data and never queried
    // afterwards — it re-anchors at every bracket close, so the session's
    // current fit is not the one these samples were dated by.
    wr_clock_snapshot fit{};

    int   calStateAtStart     = -1;
    int   calStateAtEnd       = -1;
    bool  calSpansTransition  = false;
    float presenceAngleDeg    = 0.0f;
    int   configBits          = -1;

    // One sample index is one mapped host time, so ONE tUs vector serves
    // both lanes — the palm is NOT offset by skew_us (see skewUsMedian).
    std::vector<qint64>              tUs;
    std::vector<pinpoint::ImuSample> lowerArm;
    std::vector<pinpoint::ImuSample> palm;

    // ⚠ Samples dropped for having no mapped host time. NEVER falls back to
    // arrival time: it is one-sidedly late and mixing two timebases inside
    // one lane looks fine and corrupts the capture.
    int noHostTimeSkipped = 0;
};

} // namespace hm
} // namespace pinpoint
