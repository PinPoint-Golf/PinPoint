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

#include "reference_bands.h"

// The reference-band provider.
//
// The corridors this file used to compile in — 39 green bands per (DOF, position) with a per-DOF
// amber margin, plus the ±10° archetype shift on the face DOF — are CONTENT now, in
// src/Resources/diagnostics/norms.json. They were migrated at stage 2 behind a parity gate that
// proved every band edge and every classified delta bit-identical, and the table, its two providers
// and that gate were deleted together at stage 9. Deleting them as one change was the point: a
// parity test that outlives what it compares against pins the shipped norms to a frozen table, so
// the first legitimate corpus re-seat of a corridor would fail a gate nobody wanted.

namespace pinpoint::analysis {

// ── NormBandProvider ────────────────────────────────────────────────────────

QString NormBandProvider::cellMeasureId(PpJointDof dof, PpSwingPosition pos)
{
    const int p = static_cast<int>(pos);
    if (p < 0 || p >= kNumPos)
        return QString();
    return QStringLiteral("m_%1_p%2").arg(QLatin1String(dofName(dof))).arg(p + 1);
}

// The shared, cached set — NOT a fresh makeNormProvider(). Callers build a band provider per lookup
// (the wrist grid does it per view, and the table this replaces was free), so re-reading two JSON
// files each time would be a real regression.
NormBandProvider::NormBandProvider() : m_norms(sharedNormProvider()) {}

NormBandProvider::NormBandProvider(std::shared_ptr<const INormProvider> norms, GradePolicy policy)
    : m_norms(std::move(norms)), m_policy(policy)
{
}

NormBandProvider::~NormBandProvider() = default;

Band NormBandProvider::band(PpJointDof dof, PpSwingPosition pos, const BandContext &ctx) const
{
    if (!m_norms)
        return {};

    const QString measureId = cellMeasureId(dof, pos);
    if (measureId.isEmpty())
        return {};

    const NormResolution res = m_norms->resolve(measureId, ctx.resolvedContextId());
    if (!res.found())
        return {};                 // no norm anywhere on the chain — the engine greys the cell

    // Green is the Ideal band, amber is the Watch edge — projected by bandEdgesOf(), which is the
    // ONE place that precedence (explicit monitor over z-derived, SwingLab margin over both) is
    // stated. The wrist grid renders these four numbers directly, so a second copy of that rule
    // here is how the grid ends up drawing an edge the engine does not grade on.
    //
    // Shape::Target, explicitly: this provider only ever grades the 39 migrated (DOF, position)
    // wrist-angle cells, and the class holds no CharacteristicPack — only an INormProvider keyed on
    // a bare measureId string — so there is no Measure in reach to read a shape off. Every one of
    // those cells is a two-sided joint-angle corridor; none of the shipped one-sided measures
    // (smash factor and its kin) route through this grid at all.
    const NormBandEdges e = bandEdgesOf(*res.norm, Shape::Target, m_policy, ctx.tuning.marginFor(dof));

    Band b;
    b.greenLo = e.idealLo;
    b.greenHi = e.idealHi;
    b.amberLo = e.watchLo;
    b.amberHi = e.watchHi;
    b.valid   = true;
    return b;
}

std::unique_ptr<IReferenceBandProvider> makeReferenceBandProvider()
{
    return std::make_unique<NormBandProvider>();
}

} // namespace pinpoint::analysis
