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

#include <QPointF>
#include <QString>

#include <vector>

// AnatomyVocabulary — the curated set of named body roles a characteristic may be built on, and the
// single resolver that turns a role into geometry.
//
// Why a curated vocabulary rather than keypoint indices: the offline pose layout carries 133
// keypoints, of which the face contour (23–90) and the finger joints (91–132) are low-confidence
// and motion-blurred at swing speeds. A characteristic built on keypoint 47 is noise wearing a name.
// 133 points is too many, not too few. Authors never see an index.
//
// Two pose layouts must both resolve through this one vocabulary:
//   * Coco17       — the live path (PoseResult::kNumKeypoints).
//   * WholeBody133 — the offline path (PoseFrame2D / kWholeBodyJoints); indices 0–16 are the
//                    unchanged COCO body joints, so every role that resolves in Coco17 resolves
//                    identically here. The tail (feet/face/hands) is purely additive.
//
// A role that cannot be resolved in the supplied layout returns `valid == false` with a reason.
// It must NEVER fall back to a nearby index: a silently-wrong keypoint produces a plausible number,
// and a plausible wrong number is worse than no number at all.
//
// This resolver is also intended to become the single source of truth for the skeleton overlay,
// which currently computes its neck/pelvis midpoints inline and duplicates its bone topology in
// QML. See anatomy_overlay_parity_test.

namespace pinpoint::analysis {

// ── Layout ──────────────────────────────────────────────────────────────────
// Which keypoint array shape the caller is holding. Derived from the array length rather than
// declared, so callers cannot mislabel it.
enum class PoseLayout { Coco17, WholeBody133 };

// COCO body indices, shared by both layouts. Named so no call site spells a bare integer.
namespace kp {
inline constexpr int Nose          = 0;
inline constexpr int LeftShoulder  = 5;
inline constexpr int RightShoulder = 6;
inline constexpr int LeftElbow     = 7;
inline constexpr int RightElbow    = 8;
inline constexpr int LeftWrist     = 9;
inline constexpr int RightWrist    = 10;
inline constexpr int LeftHip       = 11;
inline constexpr int RightHip      = 12;
inline constexpr int LeftKnee      = 13;
inline constexpr int RightKnee     = 14;
inline constexpr int LeftAnkle     = 15;
inline constexpr int RightAnkle    = 16;

// WholeBody-only foot tail (17–22). Absent in Coco17 — roles using these resolve unavailable there.
inline constexpr int LeftBigToe    = 17;
inline constexpr int LeftSmallToe  = 18;
inline constexpr int LeftHeel      = 19;
inline constexpr int RightBigToe   = 20;
inline constexpr int RightSmallToe = 21;
inline constexpr int RightHeel     = 22;

// Ranges no admitted role may map into — enforced by anatomy_vocabulary_test.
inline constexpr int FaceFirst     = 23;
inline constexpr int FaceLast      = 90;
inline constexpr int HandFirst     = 91;
inline constexpr int HandLast      = 132;
} // namespace kp

// ── Roles ───────────────────────────────────────────────────────────────────
// Lead/trail throughout, never left/right — handedness resolves at evaluation, which halves the
// pack and makes every characteristic handedness-agnostic by construction.
enum class AnatomyRole {
    // Points — upper body
    Head, Neck, LeadShoulder, TrailShoulder, ThoraxCentre,
    LeadElbow, TrailElbow, LeadWrist, TrailWrist, LeadHand, TrailHand,
    // Points — lower body
    PelvisCentre, LeadHip, TrailHip, LeadKnee, TrailKnee,
    LeadAnkle, TrailAnkle, LeadHeel, TrailHeel, LeadToe, TrailToe,
    StanceCentre,
    // Segments — torso
    ShoulderLine, HipLine, Spine, ThoracicSegment, LumbarSegment,
    // Segments — limbs
    LeadUpperArm, TrailUpperArm, LeadForearm, TrailForearm,
    LeadThigh, TrailThigh, LeadShin, TrailShin,
    StanceLine,
    // Club — resolved from the shaft/club track, not from pose
    Shaft, Clubhead, ClubButt, ClubFace,
    // Ball — resolved from the ball track
    Ball,
    // World datums — not body parts; legal only as a measurement reference
    Ground, TargetLine, BallLine,

    Count_
};

// What kind of thing a role is. Drives the validity table: a point has no angle, a line has no
// height, and a datum can only ever be a reference.
enum class RoleClass { Point, Segment, Datum };

// Where a role's geometry comes from. Pose roles resolve from a keypoint frame; the others need a
// track this module never sees, and resolve unavailable here with a reason that says so.
enum class RoleSource { Pose, ClubTrack, BallTrack, World };

// Why a role failed to resolve. Distinguishing these matters: NotInLayout and NoKeypoint are
// permanent for that input, LowConfidence is per-frame, and NotFromPose means "ask a different
// producer" rather than "improve the model".
enum class UnavailableReason {
    None,
    NotInLayout,     // the index exists in the vocabulary but not in this array length
    NoKeypoint,      // no keypoint exists for this role in ANY layout (spinal regions)
    LowConfidence,   // present but below the admission threshold this frame
    NotFromPose,     // club/ball/world — a different producer owns it
};

// ── Resolution results ──────────────────────────────────────────────────────
struct ResolvedPoint {
    QPointF           p;
    float             conf   = 0.0f;
    bool              valid  = false;
    UnavailableReason reason = UnavailableReason::None;
};

struct ResolvedSegment {
    QPointF           a, b;
    float             conf   = 0.0f;   // the weaker endpoint
    bool              valid  = false;
    UnavailableReason reason = UnavailableReason::None;
};

// A borrowed view of one posed frame. Deliberately not tied to PoseResult or PoseFrame2D so this
// module stays free of the analysis headers and both layouts can adapt into it without a copy.
struct KeypointFrame {
    const QPointF *kp    = nullptr;
    const float   *conf  = nullptr;
    int            count = 0;          // 17 or 133

    bool       ok()     const { return kp && conf && count > 0; }
    PoseLayout layout() const { return count > 17 ? PoseLayout::WholeBody133 : PoseLayout::Coco17; }
};

// The admission threshold. Matches the skeleton overlay's kMinScore so the extracted resolver is
// byte-comparable with what the overlay draws today — what is drawn must be definitionally what is
// measured.
inline constexpr float kMinKeypointConf = 0.25f;

// ── Vocabulary queries (pure, no frame needed) ──────────────────────────────
RoleClass   roleClass(AnatomyRole r);
RoleSource  roleSource(AnatomyRole r);
QString     roleName(AnatomyRole r);          // stable lower-camel id, the JSON spelling
QString     roleLabel(AnatomyRole r);         // human-readable, for the canonical measure name
bool        roleFromName(const QString &name, AnatomyRole &out);
const std::vector<AnatomyRole> &allRoles();

// True for roles that come in lead/trail pairs — the mirrored partner is well defined.
bool        roleIsPaired(AnatomyRole r);
AnatomyRole mirroredRole(AnatomyRole r);      // lead <-> trail; identity for unpaired roles

// True when no keypoint exists for this role in any layout, so no amount of pipeline work will
// resolve it. The spinal-region roles are the case: neither layout carries a keypoint between the
// shoulders and the hips, so thoracic flexion and lumbar extension are not derivable from pose at
// all. They stay in the vocabulary because they are the correct way to *describe* the condition —
// a vocabulary shaped by the sensor rather than the domain is the wrong vocabulary — but anything
// built on them is a capture gap, not a roadmap item.
bool        roleNeedsNonPoseSensor(AnatomyRole r);

// ── Resolution ──────────────────────────────────────────────────────────────
// `leadIsLeft` follows the established repo convention: the caller resolves it once from the
// handedness int (`handedness != 2`) and passes the bool down. This module never reads handedness,
// matching foot_metrics.h.
ResolvedPoint   resolvePoint(AnatomyRole r, const KeypointFrame &f, bool leadIsLeft,
                             float minConf = kMinKeypointConf);
ResolvedSegment resolveSegment(AnatomyRole r, const KeypointFrame &f, bool leadIsLeft,
                               float minConf = kMinKeypointConf);

// The raw keypoint index a point role maps to, or -1 for derived/non-pose roles. Exposed so tests
// can assert no admitted role reaches into the face or finger ranges.
int             rolePrimaryIndex(AnatomyRole r, bool leadIsLeft);

} // namespace pinpoint::analysis
