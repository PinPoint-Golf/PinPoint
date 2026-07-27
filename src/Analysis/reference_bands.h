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

#include "wrist_assessment_types.h"
#include "../Diagnostics/norm_provider.h"

#include <QString>

#include <memory>

// Reference bands — the "where good comes from" provider (design §6). A band is the expected
// Δ-from-address corridor for a (DOF, position) cell: a GREEN core with an AMBER margin either side;
// outside amber is RED. Supplied behind an abstract provider so the source is swappable; the one
// implementation is NormBandProvider, which projects the corridors out of the diagnostics norm set.

namespace pinpoint::analysis {

// SwingLab `bands.*` overrides — per-DOF amber margin (degrees either side of green). A negative
// value means "use the compiled-in table default", so a default-constructed BandTuning is a no-op
// and the bands are byte-identical to the frozen corridors. Full corridor (lo/hi) tuning is a
// Corpus-2 re-seat (pipeline_validation_and_tuning.md §6.6), not a sweep target — only the margins
// are exposed here.
struct BandTuning {
    double radUlnMargin     = -1.0;
    double flexExtMargin    = -1.0;
    double forearmMargin    = -1.0;
    double trailWristMargin = -1.0;
    double elbowMargin      = -1.0;

    double marginFor(PpJointDof dof) const
    {
        switch (dof) {
        case PpJointDof::LeadWristRadUln:   return radUlnMargin;
        case PpJointDof::LeadWristFlexExt:  return flexExtMargin;
        case PpJointDof::LeadForearmRot:    return forearmMargin;
        case PpJointDof::TrailWristFlexExt: return trailWristMargin;
        case PpJointDof::LeadElbowFlex:     return elbowMargin;
        default:                            return -1.0;
        }
    }
};

// Context the bands are keyed by (design §6 "context parameterisation").
//
// `contextId` is the node in the diagnostics context tree this shot belongs to, and it is now the
// real key: archetype/club/shape all fold into it, and NormBandProvider resolves by walking the
// tree upward from it. The three legacy ints remain ONLY so a caller that has not been migrated
// still compiles and behaves; `resolvedContextId()` maps them across.
//
// Empty contextId + archetype 0 means "full swing, nothing more specific known", which is exactly
// what the default context is for.
struct BandContext {
    QString    contextId;       // diagnostics context tree node; empty => derive from `archetype`
    int        archetype = 0;   // LEGACY: 0 = neutral, 1 = bowed, 2 = cupped
    int        club      = 0;   // LEGACY: unused by the norm path
    int        shape     = 0;   // LEGACY: unused by the norm path
    BandTuning tuning;          // SwingLab bands.* margin overrides (default = no-op)

    // The context to resolve norms in. A caller that sets contextId wins outright; otherwise the
    // legacy archetype int is translated, so detectArchetype()'s existing 0/1/2 keeps working
    // without every call site changing at once.
    QString resolvedContextId() const
    {
        if (!contextId.isEmpty()) return contextId;
        switch (archetype) {
        case 1:  return QStringLiteral("archetype_bowed");
        case 2:  return QStringLiteral("archetype_cupped");
        default: return QStringLiteral("full_swing");
        }
    }
};

// A resolved band for one cell. `valid == false` means "no band for this cell" (not in the table,
// or a position with no defined corridor, e.g. trail-wrist at P8) → the engine greys the cell.
struct Band {
    double greenLo = 0.0, greenHi = 0.0;
    double amberLo = 0.0, amberHi = 0.0;
    bool   valid   = false;
};

// Tier-1 band classification of a Δ value (design §7.1). Robust to a malformed band: an invalid or
// inverted green range yields Grey (no assessment) rather than a false Green; outside every valid
// range is Red. Reference handling (P1) and unavailable cells (Grey) are decided by the engine,
// which only calls this for assessable, non-reference cells.
inline PpRag classifyDelta(double delta, const Band &b)
{
    if (!b.valid || b.greenLo > b.greenHi)
        return PpRag::Grey;
    if (delta >= b.greenLo && delta <= b.greenHi)
        return PpRag::Green;
    if (b.amberLo <= b.amberHi && delta >= b.amberLo && delta <= b.amberHi)
        return PpRag::Amber;
    return PpRag::Red;
}

// The abstract provider seam.
class IReferenceBandProvider {
public:
    virtual ~IReferenceBandProvider() = default;
    virtual Band band(PpJointDof dof, PpSwingPosition pos, const BandContext &ctx = {}) const = 0;
};

// There is no compiled band table any more.
//
// `ConfigReferenceBandProvider` held one: 39 green corridors per (DOF, position) with a per-DOF
// amber margin, and `ArchetypeBandProvider` shifted the face DOF ±10° for a bowed or cupped style
// as a compiled special case. Both were migrated into the norm set at stage 2 — the corridors are
// ordinary norm rows and the archetype shift is two more rows under the `archetype_bowed` /
// `archetype_cupped` contexts — proven byte-identical, and deleted at stage 9 together with the
// parity gate that pinned them. The seam below is what survives, and NormBandProvider is the only
// implementation: corridors are CONTENT, and a second copy compiled into the binary would be a
// second answer to the same question.

// Bands projected from the diagnostics norm set (src/Resources/diagnostics/norms.json).
//
// This is the provider the app uses. It exists as an IReferenceBandProvider rather than replacing
// the seam because the wrist grid renders `Band::greenLo/greenHi` directly into PpRagCell — the
// band SHAPE is user-visible, not just the resulting PpRag — so projecting a Norm back into a Band
// made the migration structurally exact instead of merely tested:
//
//     greenLo = mu - sigmaLo      greenHi = mu + sigmaHi        (the Ideal band)
//     amberLo = monitorLo         amberHi = monitorHi           (the Watch/Action edge)
//
// Those four numbers come from `bandEdgesOf()` (norm.h), which is the one place that precedence is
// stated: a norm with no explicit monitor band — anything authored in the corridor editor — has its
// amber edges derived from the GradePolicy instead. classifyDelta() was left completely untouched.
//
// Resolution walks the context tree from ctx.resolvedContextId(). A DOF/position with no norm in
// any context yields an INVALID band, which is what the engine greys — same as the old table
// returning nothing for trail wrist at P8.
class NormBandProvider : public IReferenceBandProvider {
public:
    // Built on demand; the provider owns its norm source. Passing one in is the test seam.
    NormBandProvider();
    explicit NormBandProvider(std::shared_ptr<const INormProvider> norms,
                              GradePolicy                         policy = {});
    ~NormBandProvider() override;

    Band band(PpJointDof dof, PpSwingPosition pos, const BandContext &ctx = {}) const override;

    // The measure id a (DOF, position) cell keys on: "m_<dofName>_p<N>". Exposed because the norm
    // set, the pack's cell measures and this provider must agree on it, and a convention duplicated
    // in three places is a convention that will drift.
    static QString cellMeasureId(PpJointDof dof, PpSwingPosition pos);

private:
    std::shared_ptr<const INormProvider> m_norms;
    GradePolicy                          m_policy;
};

// No kind parameter: there is one provider, and a factory offering a choice that does not exist
// would read as a decision a caller has to make.
std::unique_ptr<IReferenceBandProvider> makeReferenceBandProvider();

} // namespace pinpoint::analysis
