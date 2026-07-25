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

#include "anatomy_vocabulary.h"

#include <QHash>

#include <algorithm>

namespace pinpoint::analysis {

namespace {

// One table row per role. Kept as a single flat table so the vocabulary can be read in one screen —
// the moment role metadata is spread across several switch statements it starts to disagree with
// itself.
struct RoleInfo {
    AnatomyRole role;
    RoleClass   cls;
    RoleSource  src;
    const char *name;    // stable id — the JSON spelling. NEVER change one; ids are permanent.
    const char *label;   // human-readable, feeds the canonical measure name
};

// clang-format off
const RoleInfo kRoles[] = {
    // Points — upper body
    { AnatomyRole::Head,          RoleClass::Point,   RoleSource::Pose,      "head",           "head" },
    { AnatomyRole::Neck,          RoleClass::Point,   RoleSource::Pose,      "neck",           "neck" },
    { AnatomyRole::LeadShoulder,  RoleClass::Point,   RoleSource::Pose,      "leadShoulder",   "lead shoulder" },
    { AnatomyRole::TrailShoulder, RoleClass::Point,   RoleSource::Pose,      "trailShoulder",  "trail shoulder" },
    { AnatomyRole::ThoraxCentre,  RoleClass::Point,   RoleSource::Pose,      "thoraxCentre",   "thorax centre" },
    { AnatomyRole::LeadElbow,     RoleClass::Point,   RoleSource::Pose,      "leadElbow",      "lead elbow" },
    { AnatomyRole::TrailElbow,    RoleClass::Point,   RoleSource::Pose,      "trailElbow",     "trail elbow" },
    { AnatomyRole::LeadWrist,     RoleClass::Point,   RoleSource::Pose,      "leadWrist",      "lead wrist" },
    { AnatomyRole::TrailWrist,    RoleClass::Point,   RoleSource::Pose,      "trailWrist",     "trail wrist" },
    { AnatomyRole::LeadHand,      RoleClass::Point,   RoleSource::Pose,      "leadHand",       "lead hand" },
    { AnatomyRole::TrailHand,     RoleClass::Point,   RoleSource::Pose,      "trailHand",      "trail hand" },
    // Points — lower body
    { AnatomyRole::PelvisCentre,  RoleClass::Point,   RoleSource::Pose,      "pelvisCentre",   "pelvis centre" },
    { AnatomyRole::LeadHip,       RoleClass::Point,   RoleSource::Pose,      "leadHip",        "lead hip" },
    { AnatomyRole::TrailHip,      RoleClass::Point,   RoleSource::Pose,      "trailHip",       "trail hip" },
    { AnatomyRole::LeadKnee,      RoleClass::Point,   RoleSource::Pose,      "leadKnee",       "lead knee" },
    { AnatomyRole::TrailKnee,     RoleClass::Point,   RoleSource::Pose,      "trailKnee",      "trail knee" },
    { AnatomyRole::LeadAnkle,     RoleClass::Point,   RoleSource::Pose,      "leadAnkle",      "lead ankle" },
    { AnatomyRole::TrailAnkle,    RoleClass::Point,   RoleSource::Pose,      "trailAnkle",     "trail ankle" },
    { AnatomyRole::LeadHeel,      RoleClass::Point,   RoleSource::Pose,      "leadHeel",       "lead heel" },
    { AnatomyRole::TrailHeel,     RoleClass::Point,   RoleSource::Pose,      "trailHeel",      "trail heel" },
    { AnatomyRole::LeadToe,       RoleClass::Point,   RoleSource::Pose,      "leadToe",        "lead toe" },
    { AnatomyRole::TrailToe,      RoleClass::Point,   RoleSource::Pose,      "trailToe",       "trail toe" },
    { AnatomyRole::StanceCentre,  RoleClass::Point,   RoleSource::Pose,      "stanceCentre",   "stance centre" },
    // Segments — torso
    { AnatomyRole::ShoulderLine,    RoleClass::Segment, RoleSource::Pose,    "shoulderLine",    "shoulder line" },
    { AnatomyRole::HipLine,         RoleClass::Segment, RoleSource::Pose,    "hipLine",         "hip line" },
    { AnatomyRole::Spine,           RoleClass::Segment, RoleSource::Pose,    "spine",           "spine" },
    { AnatomyRole::ThoracicSegment, RoleClass::Segment, RoleSource::Pose,    "thoracicSegment", "thoracic spine" },
    { AnatomyRole::LumbarSegment,   RoleClass::Segment, RoleSource::Pose,    "lumbarSegment",   "lumbar spine" },
    // Segments — limbs
    { AnatomyRole::LeadUpperArm,  RoleClass::Segment, RoleSource::Pose,      "leadUpperArm",   "lead upper arm" },
    { AnatomyRole::TrailUpperArm, RoleClass::Segment, RoleSource::Pose,      "trailUpperArm",  "trail upper arm" },
    { AnatomyRole::LeadForearm,   RoleClass::Segment, RoleSource::Pose,      "leadForearm",    "lead forearm" },
    { AnatomyRole::TrailForearm,  RoleClass::Segment, RoleSource::Pose,      "trailForearm",   "trail forearm" },
    { AnatomyRole::LeadThigh,     RoleClass::Segment, RoleSource::Pose,      "leadThigh",      "lead thigh" },
    { AnatomyRole::TrailThigh,    RoleClass::Segment, RoleSource::Pose,      "trailThigh",     "trail thigh" },
    { AnatomyRole::LeadShin,      RoleClass::Segment, RoleSource::Pose,      "leadShin",       "lead shin" },
    { AnatomyRole::TrailShin,     RoleClass::Segment, RoleSource::Pose,      "trailShin",      "trail shin" },
    { AnatomyRole::StanceLine,    RoleClass::Segment, RoleSource::Pose,      "stanceLine",     "stance line" },
    // Club
    { AnatomyRole::Shaft,         RoleClass::Segment, RoleSource::ClubTrack, "shaft",          "shaft" },
    { AnatomyRole::Clubhead,      RoleClass::Point,   RoleSource::ClubTrack, "clubhead",       "clubhead" },
    { AnatomyRole::ClubButt,      RoleClass::Point,   RoleSource::ClubTrack, "clubButt",       "club butt" },
    { AnatomyRole::ClubFace,      RoleClass::Segment, RoleSource::ClubTrack, "clubFace",       "club face" },
    // Ball
    { AnatomyRole::Ball,          RoleClass::Point,   RoleSource::BallTrack, "ball",           "ball" },
    // World datums
    { AnatomyRole::Ground,        RoleClass::Datum,   RoleSource::World,     "ground",         "the ground" },
    { AnatomyRole::TargetLine,    RoleClass::Datum,   RoleSource::World,     "targetLine",     "the target line" },
    { AnatomyRole::BallLine,      RoleClass::Datum,   RoleSource::World,     "ballLine",       "the ball line" },
};
// clang-format on

static_assert(sizeof(kRoles) / sizeof(kRoles[0]) == static_cast<size_t>(AnatomyRole::Count_),
              "kRoles must carry exactly one row per AnatomyRole — a missing row silently "
              "mis-indexes every lookup below.");

const RoleInfo &info(AnatomyRole r)
{
    // The table is authored in enum order, asserted above, so this is a direct index.
    return kRoles[static_cast<int>(r)];
}

// Lead/trail pairs, for mirroredRole() and the paired-role validity rule.
const std::pair<AnatomyRole, AnatomyRole> kPairs[] = {
    { AnatomyRole::LeadShoulder,  AnatomyRole::TrailShoulder },
    { AnatomyRole::LeadElbow,     AnatomyRole::TrailElbow },
    { AnatomyRole::LeadWrist,     AnatomyRole::TrailWrist },
    { AnatomyRole::LeadHand,      AnatomyRole::TrailHand },
    { AnatomyRole::LeadHip,       AnatomyRole::TrailHip },
    { AnatomyRole::LeadKnee,      AnatomyRole::TrailKnee },
    { AnatomyRole::LeadAnkle,     AnatomyRole::TrailAnkle },
    { AnatomyRole::LeadHeel,      AnatomyRole::TrailHeel },
    { AnatomyRole::LeadToe,       AnatomyRole::TrailToe },
    { AnatomyRole::LeadUpperArm,  AnatomyRole::TrailUpperArm },
    { AnatomyRole::LeadForearm,   AnatomyRole::TrailForearm },
    { AnatomyRole::LeadThigh,     AnatomyRole::TrailThigh },
    { AnatomyRole::LeadShin,      AnatomyRole::TrailShin },
};

// A keypoint read with the admission threshold applied. Returns valid == false rather than a
// fallback position — see the header's note on why a nearby index is never acceptable.
ResolvedPoint readKp(int idx, const KeypointFrame &f, float minConf)
{
    ResolvedPoint out;
    if (idx < 0) {
        out.reason = UnavailableReason::NoKeypoint;
        return out;
    }
    if (!f.ok() || idx >= f.count) {
        out.reason = UnavailableReason::NotInLayout;
        return out;
    }
    if (f.conf[idx] < minConf) {
        out.reason = UnavailableReason::LowConfidence;
        return out;
    }
    out.p     = f.kp[idx];
    out.conf  = f.conf[idx];
    out.valid = true;
    return out;
}

// Midpoint of two keypoints, valid only when both parents are. Confidence is the weaker parent —
// matching the skeleton overlay, so the confidence-driven alpha it already applies is unchanged by
// the extraction.
ResolvedPoint midOf(int ia, int ib, const KeypointFrame &f, float minConf)
{
    const ResolvedPoint a = readKp(ia, f, minConf);
    if (!a.valid) return a;
    const ResolvedPoint b = readKp(ib, f, minConf);
    if (!b.valid) return b;

    ResolvedPoint out;
    out.p     = (a.p + b.p) * 0.5;
    out.conf  = std::min(a.conf, b.conf);
    out.valid = true;
    return out;
}

// Lead/trail → COCO left/right. The single place handedness is applied.
constexpr int pick(bool leadIsLeft, int leftIdx, int rightIdx) { return leadIsLeft ? leftIdx : rightIdx; }

} // namespace

// ── Vocabulary queries ──────────────────────────────────────────────────────

RoleClass  roleClass(AnatomyRole r)  { return info(r).cls; }
RoleSource roleSource(AnatomyRole r) { return info(r).src; }
QString    roleName(AnatomyRole r)   { return QString::fromLatin1(info(r).name); }
QString    roleLabel(AnatomyRole r)  { return QString::fromLatin1(info(r).label); }

bool roleFromName(const QString &name, AnatomyRole &out)
{
    static const QHash<QString, AnatomyRole> lut = [] {
        QHash<QString, AnatomyRole> m;
        for (const RoleInfo &ri : kRoles) m.insert(QString::fromLatin1(ri.name), ri.role);
        return m;
    }();
    const auto it = lut.constFind(name);
    if (it == lut.constEnd()) return false;
    out = it.value();
    return true;
}

const std::vector<AnatomyRole> &allRoles()
{
    static const std::vector<AnatomyRole> v = [] {
        std::vector<AnatomyRole> r;
        r.reserve(static_cast<size_t>(AnatomyRole::Count_));
        for (const RoleInfo &ri : kRoles) r.push_back(ri.role);
        return r;
    }();
    return v;
}

bool roleIsPaired(AnatomyRole r)
{
    for (const auto &p : kPairs)
        if (p.first == r || p.second == r) return true;
    return false;
}

AnatomyRole mirroredRole(AnatomyRole r)
{
    for (const auto &p : kPairs) {
        if (p.first == r)  return p.second;
        if (p.second == r) return p.first;
    }
    return r;
}

bool roleNeedsNonPoseSensor(AnatomyRole r)
{
    // The spinal regions. Neither layout carries a keypoint between the shoulders and the hips, so
    // a straight neck→pelvis line is the same line whether the thorax is flexed or the lumbar
    // arched. These describe real, well-understood conditions that this product cannot see.
    return r == AnatomyRole::ThoracicSegment || r == AnatomyRole::LumbarSegment;
}

int rolePrimaryIndex(AnatomyRole r, bool leadIsLeft)
{
    switch (r) {
    case AnatomyRole::Head:          return kp::Nose;
    case AnatomyRole::LeadShoulder:  return pick(leadIsLeft, kp::LeftShoulder, kp::RightShoulder);
    case AnatomyRole::TrailShoulder: return pick(leadIsLeft, kp::RightShoulder, kp::LeftShoulder);
    case AnatomyRole::LeadElbow:     return pick(leadIsLeft, kp::LeftElbow, kp::RightElbow);
    case AnatomyRole::TrailElbow:    return pick(leadIsLeft, kp::RightElbow, kp::LeftElbow);
    case AnatomyRole::LeadWrist:     return pick(leadIsLeft, kp::LeftWrist, kp::RightWrist);
    case AnatomyRole::TrailWrist:    return pick(leadIsLeft, kp::RightWrist, kp::LeftWrist);
    // Hands have no dedicated body keypoint; the wrist is the honest stand-in on both layouts. The
    // WholeBody hand tail (91–132) is finger joints — motion-blurred at swing speeds and excluded
    // from this vocabulary by design.
    case AnatomyRole::LeadHand:      return pick(leadIsLeft, kp::LeftWrist, kp::RightWrist);
    case AnatomyRole::TrailHand:     return pick(leadIsLeft, kp::RightWrist, kp::LeftWrist);
    case AnatomyRole::LeadHip:       return pick(leadIsLeft, kp::LeftHip, kp::RightHip);
    case AnatomyRole::TrailHip:      return pick(leadIsLeft, kp::RightHip, kp::LeftHip);
    case AnatomyRole::LeadKnee:      return pick(leadIsLeft, kp::LeftKnee, kp::RightKnee);
    case AnatomyRole::TrailKnee:     return pick(leadIsLeft, kp::RightKnee, kp::LeftKnee);
    case AnatomyRole::LeadAnkle:     return pick(leadIsLeft, kp::LeftAnkle, kp::RightAnkle);
    case AnatomyRole::TrailAnkle:    return pick(leadIsLeft, kp::RightAnkle, kp::LeftAnkle);
    case AnatomyRole::LeadHeel:      return pick(leadIsLeft, kp::LeftHeel, kp::RightHeel);
    case AnatomyRole::TrailHeel:     return pick(leadIsLeft, kp::RightHeel, kp::LeftHeel);
    case AnatomyRole::LeadToe:       return pick(leadIsLeft, kp::LeftBigToe, kp::RightBigToe);
    case AnatomyRole::TrailToe:      return pick(leadIsLeft, kp::RightBigToe, kp::LeftBigToe);
    default:                         return -1;   // derived, segment, club, ball or datum
    }
}

// ── Resolution ──────────────────────────────────────────────────────────────

ResolvedPoint resolvePoint(AnatomyRole r, const KeypointFrame &f, bool leadIsLeft, float minConf)
{
    ResolvedPoint out;

    if (roleClass(r) != RoleClass::Point) {
        out.reason = UnavailableReason::NoKeypoint;
        return out;
    }
    if (roleSource(r) != RoleSource::Pose) {
        // Clubhead, club butt and ball are real and measured — just not by this producer.
        out.reason = UnavailableReason::NotFromPose;
        return out;
    }

    switch (r) {
    // Derived midpoints. First-class, and often *more* reliable than a raw keypoint: averaging two
    // independent estimates halves the jitter.
    case AnatomyRole::Neck:
        return midOf(kp::LeftShoulder, kp::RightShoulder, f, minConf);
    case AnatomyRole::PelvisCentre:
        return midOf(kp::LeftHip, kp::RightHip, f, minConf);
    case AnatomyRole::StanceCentre:
        return midOf(kp::LeftAnkle, kp::RightAnkle, f, minConf);
    case AnatomyRole::ThoraxCentre: {
        // Convention, not anatomy: with no spine keypoints the torso centroid is the best available
        // proxy for "the chest". Every seeded use is a displacement (a Delta), so a consistent
        // origin is what matters rather than an anatomically exact one. Revisit if an absolute
        // thorax position is ever needed.
        const ResolvedPoint neck   = midOf(kp::LeftShoulder, kp::RightShoulder, f, minConf);
        if (!neck.valid) return neck;
        const ResolvedPoint pelvis = midOf(kp::LeftHip, kp::RightHip, f, minConf);
        if (!pelvis.valid) return pelvis;
        out.p     = (neck.p + pelvis.p) * 0.5;
        out.conf  = std::min(neck.conf, pelvis.conf);
        out.valid = true;
        return out;
    }
    default:
        break;
    }

    return readKp(rolePrimaryIndex(r, leadIsLeft), f, minConf);
}

ResolvedSegment resolveSegment(AnatomyRole r, const KeypointFrame &f, bool leadIsLeft, float minConf)
{
    ResolvedSegment out;

    if (roleClass(r) != RoleClass::Segment) {
        out.reason = UnavailableReason::NoKeypoint;
        return out;
    }
    if (roleSource(r) != RoleSource::Pose) {
        out.reason = UnavailableReason::NotFromPose;
        return out;
    }
    if (roleNeedsNonPoseSensor(r)) {
        // Admitted to the vocabulary, permanently unresolvable from pose. Reported as NoKeypoint so
        // the caller can route it to the capture-gap list rather than the producer roadmap.
        out.reason = UnavailableReason::NoKeypoint;
        return out;
    }

    // Each segment is an ordered endpoint pair. Order matters: it fixes the sign of every angle
    // measured against this segment, so it must never be swapped casually.
    AnatomyRole ra = AnatomyRole::Count_, rb = AnatomyRole::Count_;
    switch (r) {
    case AnatomyRole::ShoulderLine:  ra = AnatomyRole::TrailShoulder; rb = AnatomyRole::LeadShoulder; break;
    case AnatomyRole::HipLine:       ra = AnatomyRole::TrailHip;      rb = AnatomyRole::LeadHip;      break;
    case AnatomyRole::Spine:         ra = AnatomyRole::PelvisCentre;  rb = AnatomyRole::Neck;         break;
    case AnatomyRole::StanceLine:    ra = AnatomyRole::TrailAnkle;    rb = AnatomyRole::LeadAnkle;    break;
    case AnatomyRole::LeadUpperArm:  ra = AnatomyRole::LeadShoulder;  rb = AnatomyRole::LeadElbow;    break;
    case AnatomyRole::TrailUpperArm: ra = AnatomyRole::TrailShoulder; rb = AnatomyRole::TrailElbow;   break;
    case AnatomyRole::LeadForearm:   ra = AnatomyRole::LeadElbow;     rb = AnatomyRole::LeadWrist;    break;
    case AnatomyRole::TrailForearm:  ra = AnatomyRole::TrailElbow;    rb = AnatomyRole::TrailWrist;   break;
    case AnatomyRole::LeadThigh:     ra = AnatomyRole::LeadHip;       rb = AnatomyRole::LeadKnee;     break;
    case AnatomyRole::TrailThigh:    ra = AnatomyRole::TrailHip;      rb = AnatomyRole::TrailKnee;    break;
    case AnatomyRole::LeadShin:      ra = AnatomyRole::LeadKnee;      rb = AnatomyRole::LeadAnkle;    break;
    case AnatomyRole::TrailShin:     ra = AnatomyRole::TrailKnee;     rb = AnatomyRole::TrailAnkle;   break;
    default:
        out.reason = UnavailableReason::NoKeypoint;
        return out;
    }

    const ResolvedPoint a = resolvePoint(ra, f, leadIsLeft, minConf);
    if (!a.valid) { out.reason = a.reason; return out; }
    const ResolvedPoint b = resolvePoint(rb, f, leadIsLeft, minConf);
    if (!b.valid) { out.reason = b.reason; return out; }

    out.a     = a.p;
    out.b     = b.p;
    out.conf  = std::min(a.conf, b.conf);
    out.valid = true;
    return out;
}

} // namespace pinpoint::analysis
