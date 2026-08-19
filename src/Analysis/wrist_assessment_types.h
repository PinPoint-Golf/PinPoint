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

#include <QString>

#include <optional>

// Wrist Motion assessment engine — Phase 0 type vocabulary.
//
// The engine (design: docs/design/wristmotion_assessment_design.md §2) samples wrist / forearm /
// arm joint-angle degrees of freedom at swing positions P1–P8, bands them, runs a rule engine,
// and rolls up a score. This header declares the DOF taxonomy, the P-position enum, handedness,
// and the small helpers that bridge to the existing producer keys. No I/O, no Qt-GUI; header-only.
//
// SIGN CONVENTIONS (canonical, right-handed golfer baseline) are MATCHED EXACTLY to the existing
// quaternion→anatomical decomposition in src/Analysis/wrist_angles.h so the Phase-3 acquisition
// adapter is a pure radians→degrees unit conversion with no per-DOF sign flips on the lead side:
//   LeadWristFlexExt  +flexion  = "bowed"   / −extension = "cupped"  (wrist_angles feRad)
//   LeadWristRadUln   +ulnar    = "hinge"   / −radial               (wrist_angles rudRad)
//   LeadForearmRot    +pronation= "roll"    / −supination           (wrist_angles pronRad)
//   LeadElbowFlex     +flexion magnitude (≥0)                       (wrist_angles flexRad)
// Left-handed golfers are mirrored at the acquisition layer (design §2.3), so the engine itself
// is handedness-agnostic. The per-DOF mirror parity used by that mirroring lives in mirrorSign().

namespace pinpoint::analysis {

// The DOF taxonomy (design §2.2). The first four have real producers today (metric_extractor.cpp);
// the remaining ten are reserved slots with no producer yet (trail side + shoulders) and resolve to
// "no data" cells until the relevant instrumentation lands (design §4, §10 Phase 4). Enumerating
// them now keeps the enum stable as producers are added. The order is part of the contract (used as
// an array index) — append before Count, never reorder.
enum class PpJointDof {
    // --- real producers (IMU, lead side) ---
    LeadWristFlexExt = 0,   // + flexion / bowed,   − extension / cupped
    LeadWristRadUln,        // + ulnar (hinge),      − radial
    LeadForearmRot,         // + pronation (roll),   − supination
    LeadElbowFlex,          // + flexion magnitude (≥0)
    // --- reserved: no producer yet (Phase 4 instrumentation) ---
    TrailWristFlexExt,
    TrailWristRadUln,
    TrailForearmRot,
    TrailElbowFlex,
    LeadShoulderElevation,
    LeadShoulderHorizAbd,
    LeadShoulderRotation,
    TrailShoulderElevation,
    TrailShoulderHorizAbd,
    TrailShoulderRotation,
    Count
};

// Swing positions P1…P8 (design §3). Distinct from the producer's golf-named Phase enum
// (swing_analysis.h Address…Finish); the Phase→Pn map is a Phase-3 adapter concern.
enum class PpSwingPosition { P1 = 0, P2, P3, P4, P5, P6, P7, P8, Count };

// 0 unknown, 1 right, 2 left — matches the repo-wide handedness int convention
// (shot_analyzer.h, metric_extractor.h). Kept typed here for engine clarity.
enum class PpHandedness { Unknown = 0, Right = 1, Left = 2 };

// Tier-1 RAG state of a sampled cell (design §8.4). Ref = the P1 reference cell (Δ ≡ 0; shown muted,
// excluded from the score); Grey = no assessment (gap / indeterminate / no band). Green/Amber/Red
// pair with shape + label in the view so colour is never the only channel.
enum class PpRag { Ref, Green, Amber, Red, Grey };

inline const char *ragName(PpRag r)
{
    switch (r) {
    case PpRag::Ref:   return "ref";
    case PpRag::Green: return "green";
    case PpRag::Amber: return "amber";
    case PpRag::Red:   return "red";
    case PpRag::Grey:  return "grey";
    }
    return "grey";
}

constexpr int kNumDof = static_cast<int>(PpJointDof::Count);       // 14
constexpr int kNumPos = static_cast<int>(PpSwingPosition::Count);  // 8

// Handedness mirror parity per DOF (design §2.3). Mirroring a swing left↔right negates the
// lateral / rotational DOFs (radial↔ulnar, pronation↔supination, abduction, axial rotation) and
// preserves the sagittal / magnitude DOFs (flexion-extension, flexion magnitude, elevation). The
// acquisition adapter (or, in Phase-0 tests, the sampler) multiplies a left-handed value by this so
// the resulting angle set is canonical right-handed.
inline double mirrorSign(PpJointDof dof)
{
    switch (dof) {
    case PpJointDof::LeadWristRadUln:
    case PpJointDof::LeadForearmRot:
    case PpJointDof::TrailWristRadUln:
    case PpJointDof::TrailForearmRot:
    case PpJointDof::LeadShoulderHorizAbd:
    case PpJointDof::LeadShoulderRotation:
    case PpJointDof::TrailShoulderHorizAbd:
    case PpJointDof::TrailShoulderRotation:
        return -1.0;
    default:
        return 1.0;
    }
}

// Map a producer metric key → DOF. Accepts both the metric_extractor spelling
// (leadArmFlexion / forearmPronation) and the mockup spelling (leadElbowFlex / leadForearmRot /
// trailWristExt). Returns nullopt for an unknown key. Used by the Phase-3 adapter.
//
// ⚠ THE INSTRUMENT PREFIX IS STRIPPED FIRST, and this one line is what keeps the whole
// DOF × P-position assessment grid working on a HackMotion swing. A DOF is an anatomical axis —
// which instrument measured it is not part of its identity, and a grid that went blank because
// the series arrived as `hm.leadWristFlexExt` would be reporting a naming decision as missing
// anatomy. This is the same best-first idea as Measure::preferKeys one layer up, in the one place
// the join from key to DOF happens.
inline std::optional<PpJointDof> dofForMetricKey(const QString &rawKey)
{
    const QString key = rawKey.startsWith(QLatin1String("hm.")) ? rawKey.mid(3) : rawKey;
    // ⚠ `forearmRotation` DELIBERATELY MAPS TO NOTHING. The grid's LeadForearmRot row is the ISB
    // radioulnar angle, fed by `forearmPronation`; forearm rotation is a different quantity that
    // happens to concern the same bone. Routing it here would put two different measurements in
    // one cell on a three-sensor rig — last writer wins, silently — and would let a wG3 fill a row
    // for an angle it cannot measure at all. An empty row is the honest answer.
    if (key == QLatin1String("leadWristFlexExt")) return PpJointDof::LeadWristFlexExt;
    if (key == QLatin1String("leadWristRadUln"))  return PpJointDof::LeadWristRadUln;
    if (key == QLatin1String("forearmPronation") || key == QLatin1String("leadForearmRot"))
        return PpJointDof::LeadForearmRot;
    if (key == QLatin1String("leadArmFlexion") || key == QLatin1String("leadElbowFlex"))
        return PpJointDof::LeadElbowFlex;
    if (key == QLatin1String("trailWristExt") || key == QLatin1String("trailWristFlexExt"))
        return PpJointDof::TrailWristFlexExt;
    return std::nullopt;
}

// Stable short identifier for logs / test output (not user-facing).
inline const char *dofName(PpJointDof dof)
{
    switch (dof) {
    case PpJointDof::LeadWristFlexExt:       return "leadWristFlexExt";
    case PpJointDof::LeadWristRadUln:        return "leadWristRadUln";
    case PpJointDof::LeadForearmRot:         return "leadForearmRot";
    case PpJointDof::LeadElbowFlex:          return "leadElbowFlex";
    case PpJointDof::TrailWristFlexExt:      return "trailWristFlexExt";
    case PpJointDof::TrailWristRadUln:       return "trailWristRadUln";
    case PpJointDof::TrailForearmRot:        return "trailForearmRot";
    case PpJointDof::TrailElbowFlex:         return "trailElbowFlex";
    case PpJointDof::LeadShoulderElevation:  return "leadShoulderElevation";
    case PpJointDof::LeadShoulderHorizAbd:   return "leadShoulderHorizAbd";
    case PpJointDof::LeadShoulderRotation:   return "leadShoulderRotation";
    case PpJointDof::TrailShoulderElevation: return "trailShoulderElevation";
    case PpJointDof::TrailShoulderHorizAbd:  return "trailShoulderHorizAbd";
    case PpJointDof::TrailShoulderRotation:  return "trailShoulderRotation";
    case PpJointDof::Count:                  return "count";
    }
    return "unknown";
}

} // namespace pinpoint::analysis
