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

#include "anatomy_vocabulary.h"

#include "../Metrics/metric_descriptor.h"   // PhaseDomain — a reducer is checked against it
#include "../Metrics/metric_reducer.h"

#include <QString>

#include <vector>

// MeasureFacets — how a measure is composed, named and compared.
//
// A measure is a SERIES sampled by a REDUCER:
//
//     series  = what · quantity · relative-to-reference      (a curve; what a producer produces)
//     reducer = at n | Δ n→m | rate n→m | extremum n→m       (how the curve becomes one number)
//
// The reference is simply *another thing* — any anatomy role, or a world datum. It is not a
// separate closed vocabulary, and it is never a *time*: "relative to its value at address" is the
// reducer's anchor, not a reference, because that is the same series sampled twice rather than a
// different spatial relationship.
//
// Structural identity is the SERIES tuple, not the reduced measure. Two measures over one series
// with different reducers are related, not duplicates — and the roadmap ranks series, because one
// producer unblocks every reducer over it. Ranking reduced measures would spread a single piece of
// pipeline work across four rows and understate its payoff.

namespace pinpoint::analysis {

// ── Quantity ────────────────────────────────────────────────────────────────
// Deliberately five. "Position" is absent: a position IS a Distance or a Height relative to a named
// thing, and offering both spellings would let two authors describe one measure two ways — which is
// exactly the duplicate the structural detector exists to prevent.
enum class Quantity { Height, Angle, Distance, Rotation, Speed };

QString                       quantityName(Quantity q);      // stable id / JSON spelling
QString                       quantityLabel(Quantity q);     // human-readable
bool                          quantityFromName(const QString &name, Quantity &out);
const std::vector<Quantity>  &allQuantities();

// Which camera view a measure needs. Derived from the facets where it can be, `Any` otherwise —
// the derivation is a documented heuristic, not a proof, and an author may override it.
enum class ViewNeeded { Any, FaceOn, DownTheLine };
QString viewNeededName(ViewNeeded v);
bool    viewNeededFromName(const QString &name, ViewNeeded &out);

// ── Phase spelling ──────────────────────────────────────────────────────────
// The Phase enum's own names are internal (Top, Delivery, ArmParallelDown). Coaches, the seed pack
// and every authored characteristic speak in P-positions, so labels and JSON tokens use those and
// fall back to a descriptive name where no P-number exists. One spelling, defined once.
QString phaseLabel(Phase p);                             // "P4", "transition"
QString phaseToken(Phase p);                             // "p4", "transition" — the JSON spelling
bool    phaseFromToken(const QString &token, Phase &out);

// ── Series ──────────────────────────────────────────────────────────────────
struct Series {
    AnatomyRole what      = AnatomyRole::Count_;
    Quantity    quantity  = Quantity::Distance;
    AnatomyRole reference = AnatomyRole::Count_;
};

inline bool operator==(const Series &a, const Series &b)
{
    return a.what == b.what && a.quantity == b.quantity && a.reference == b.reference;
}
inline bool operator!=(const Series &a, const Series &b) { return !(a == b); }

// Total order, so a pack's series can live in a sorted container and diff deterministically.
bool operator<(const Series &a, const Series &b);

// ── Validity ────────────────────────────────────────────────────────────────
// The table is indexed on (what-class, quantity, reference-class) rather than being a flat per-role
// list: several legitimate measures use a LINE as the reference with Distance ("pelvis centre,
// distance to the ball line"), which a per-role ban on "Distance for lines" would wrongly reject.
enum class FacetError {
    None,
    WhatIsDatum,           // a world datum describes a reference, never a subject
    PointHasNoAngle,       // a point has no orientation; a joint angle is between two SEGMENTS
    SegmentHasNoDistance,  // a segment's own length is not what anyone means here
    SelfReference,         // a thing relative to itself
    BallQuantity,          // ball: Distance and Speed only
    ClubFaceQuantity,      // club face: face-relative quantities only
    AngleNeedsDirection,   // an angle needs a reference with a direction (segment or datum)
    UnknownRole,
};

struct FacetCheck {
    bool       valid = false;
    FacetError error = FacetError::None;
    QString    reason;      // human-readable, shown in the picker at the moment of the mistake
};

FacetCheck validateSeries(const Series &s);

// Enumerate what is legal, so the picker can grey out impossible chips rather than letting an
// author build something and then rejecting it.
std::vector<Quantity>    legalQuantitiesFor(AnatomyRole what);
std::vector<AnatomyRole> legalReferencesFor(AnatomyRole what, Quantity q);

// ── Reducer validity ────────────────────────────────────────────────────────
// A reducer is well-formed independently of the series it samples.
struct ReducerCheck {
    bool    valid = false;
    QString reason;
};
ReducerCheck validateReducer(const Reducer &r);

// The same, plus the METRIC'S PHASE DOMAIN (metric_descriptor.h): an `at` anchor, or a
// delta/rate/extremum window, that reaches outside the domain is refused with a reason naming it in
// P-positions. Design §5.1 — "the reducers search only inside it".
//
// A SECOND FUNCTION AND NOT A NEW FIELD ON Reducer. The domain is a property of the metric, not of
// the reduction, and a great many callers legitimately have no metric in hand — the facet picker
// validating a half-built composed measure, every existing pack test. Those keep the one-argument
// form, which is this one with the whole-swing default, so the answers they get are unchanged.
//
// The domain is passed in rather than looked up: this file has never been able to see the metric
// catalogue and does not start now (see characteristic_pack.h, same rule). A caller that holds one
// asks it; a caller that does not gets the whole swing, which is what almost every metric means.
ReducerCheck validateReducer(const Reducer &r, const PhaseDomain &domain);

// ── Canonical naming ────────────────────────────────────────────────────────
// Deterministic: identical facets always produce an identical string. Identity is still the tuple,
// never the string — the label is for humans and must never be parsed back.
QString canonicalSeriesLabel(const Series &s);
QString canonicalMeasureLabel(const Series &s, const Reducer &r);

// The stable id used as a JSON key and a MetricDescriptor key. Lower-camel, facet-derived, so the
// same measure minted twice on different machines collides rather than duplicating.
QString canonicalSeriesId(const Series &s);
QString canonicalMeasureId(const Series &s, const Reducer &r);

// ── Structural duplicate detection ──────────────────────────────────────────
// The primary defence against a library that fills with near-identical measures within a month.
// Exact match ⇒ the same series, reuse it. Exactly one facet different ⇒ a near-duplicate, which
// must be surfaced prominently AT THE MOMENT OF CREATION — after the fact nobody merges them.
int  facetDistance(const Series &a, const Series &b);          // 0..3
bool isNearDuplicate(const Series &a, const Series &b);        // exactly one facet differs

struct SeriesMatch {
    Series series;
    int    distance = 0;
};
// Exact matches first (distance 0), then near-duplicates (distance 1). Anything further apart is
// not a duplicate and is not reported — noise here trains authors to dismiss the warning.
std::vector<SeriesMatch> findSimilarSeries(const Series &candidate,
                                           const std::vector<Series> &existing);

// ── Derived properties ──────────────────────────────────────────────────────
ViewNeeded deriveViewNeeded(const Series &s);

// The unit a quantity is expressed in. Angles and rotations are degrees; the rest are pixel-space
// or normalised until a producer declares otherwise, so this returns the dimension rather than a
// fixed unit string.
QString quantityUnitHint(Quantity q);

// True when the series cannot be resolved from the pose SKELETON, because a role it names has no
// keypoint in any layout. This is NOT a capture gap: the spinal roles it covers are readable from
// the back contour of a down-the-line silhouette, so such a measure is a roadmap item whose
// producer is simply not written yet. Do not use this to decide that something is unreachable.
bool seriesNeedsNonPoseSensor(const Series &s);

} // namespace pinpoint::analysis
