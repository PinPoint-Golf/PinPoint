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

#include "measure_facets.h"

#include <algorithm>

namespace pinpoint::analysis {

namespace {

// The same (enum → token, label) idiom as enum_table_p.h, with a third column. It stays hand-rolled
// because `Row<E>` carries exactly two strings: splitting `unitHint` into a second table beside a
// Row table would put one row's spelling in two places, which is the one thing these tables exist to
// prevent. If a label here ever goes non-ASCII, decode it with fromUtf8 — see enum_table_p.h for why.
struct QuantityInfo {
    Quantity    q;
    const char *name;
    const char *label;
    const char *unitHint;
};

const QuantityInfo kQuantities[] = {
    { Quantity::Height,   "height",   "height",   "length" },
    { Quantity::Angle,    "angle",    "angle",    "degrees" },
    { Quantity::Distance, "distance", "distance", "length" },
    { Quantity::Rotation, "rotation", "rotation", "degrees" },
    { Quantity::Speed,    "speed",    "speed",    "length/time" },
};

const QuantityInfo &qinfo(Quantity q)
{
    for (const QuantityInfo &qi : kQuantities)
        if (qi.q == q) return qi;
    return kQuantities[0];
}

QString upperFirst(QString s)
{
    if (s.isEmpty()) return s;
    s[0] = s.at(0).toUpper();
    return s;
}

// "lead shoulder" -> "leadShoulder"; used to build ids out of labels.
QString camelToken(const QString &label)
{
    const QStringList parts = label.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString out;
    for (int i = 0; i < parts.size(); ++i)
        out += (i == 0) ? parts[i] : upperFirst(parts[i]);
    return out;
}

} // namespace

// ── Phase spelling ──────────────────────────────────────────────────────────

QString phaseLabel(Phase p)
{
    switch (p) {
    case Phase::Address:              return QStringLiteral("P1");
    case Phase::ShaftParallelBack:    return QStringLiteral("P2");
    case Phase::MidBackswing:         return QStringLiteral("P3");
    case Phase::Top:                  return QStringLiteral("P4");
    case Phase::ArmParallelDown:      return QStringLiteral("P5");
    case Phase::Delivery:             return QStringLiteral("P6");
    case Phase::Impact:               return QStringLiteral("P7");
    case Phase::ShaftParallelThrough: return QStringLiteral("P8");
    case Phase::FollowThrough:        return QStringLiteral("P9");
    case Phase::Takeaway:             return QStringLiteral("takeaway");
    case Phase::Transition:           return QStringLiteral("transition");
    case Phase::Downswing:            return QStringLiteral("downswing");
    case Phase::Release:              return QStringLiteral("release");
    case Phase::Finish:               return QStringLiteral("finish");
    case Phase::MaxSpeed:             return QStringLiteral("max speed");
    }
    return QStringLiteral("?");
}

QString phaseToken(Phase p)
{
    QString s = phaseLabel(p);
    s.remove(QLatin1Char(' '));
    return s.at(0).toLower() + s.mid(1);
}

bool phaseFromToken(const QString &token, Phase &out)
{
    // Every Phase the enum carries, so a pack can name a non-P checkpoint (transition, max speed)
    // without a second vocabulary.
    static const Phase kAll[] = {
        Phase::Address, Phase::ShaftParallelBack, Phase::MidBackswing, Phase::Top,
        Phase::ArmParallelDown, Phase::Delivery, Phase::Impact, Phase::ShaftParallelThrough,
        Phase::FollowThrough, Phase::Takeaway, Phase::Transition, Phase::Downswing,
        Phase::Release, Phase::Finish, Phase::MaxSpeed,
    };
    for (Phase p : kAll) {
        if (phaseToken(p).compare(token, Qt::CaseInsensitive) == 0) { out = p; return true; }
    }
    return false;
}

// ── Quantity ────────────────────────────────────────────────────────────────

QString quantityName(Quantity q)  { return QString::fromLatin1(qinfo(q).name); }
QString quantityLabel(Quantity q) { return QString::fromLatin1(qinfo(q).label); }
QString quantityUnitHint(Quantity q) { return QString::fromLatin1(qinfo(q).unitHint); }

bool quantityFromName(const QString &name, Quantity &out)
{
    for (const QuantityInfo &qi : kQuantities) {
        if (name == QLatin1String(qi.name)) { out = qi.q; return true; }
    }
    return false;
}

const std::vector<Quantity> &allQuantities()
{
    static const std::vector<Quantity> v = [] {
        std::vector<Quantity> r;
        for (const QuantityInfo &qi : kQuantities) r.push_back(qi.q);
        return r;
    }();
    return v;
}

QString viewNeededName(ViewNeeded v)
{
    switch (v) {
    case ViewNeeded::FaceOn:      return QStringLiteral("faceOn");
    case ViewNeeded::DownTheLine: return QStringLiteral("downTheLine");
    case ViewNeeded::Any:         break;
    }
    return QStringLiteral("any");
}

bool viewNeededFromName(const QString &name, ViewNeeded &out)
{
    if (name == QLatin1String("any"))         { out = ViewNeeded::Any;         return true; }
    if (name == QLatin1String("faceOn"))      { out = ViewNeeded::FaceOn;      return true; }
    if (name == QLatin1String("downTheLine")) { out = ViewNeeded::DownTheLine; return true; }
    return false;
}

bool operator<(const Series &a, const Series &b)
{
    if (a.what != b.what)         return int(a.what) < int(b.what);
    if (a.quantity != b.quantity) return int(a.quantity) < int(b.quantity);
    return int(a.reference) < int(b.reference);
}

// ── Validity ────────────────────────────────────────────────────────────────

FacetCheck validateSeries(const Series &s)
{
    FacetCheck c;

    auto fail = [&c](FacetError e, const QString &why) {
        c.valid  = false;
        c.error  = e;
        c.reason = why;
        return c;
    };

    if (s.what == AnatomyRole::Count_ || s.reference == AnatomyRole::Count_)
        return fail(FacetError::UnknownRole, QStringLiteral("Pick both a subject and a reference."));

    const RoleClass wc = roleClass(s.what);
    const RoleClass rc = roleClass(s.reference);

    // A world datum describes where something is measured FROM. It is never the subject.
    if (wc == RoleClass::Datum)
        return fail(FacetError::WhatIsDatum,
                    QStringLiteral("%1 is a reference, not something that can be measured.")
                        .arg(roleLabel(s.what)));

    if (s.what == s.reference)
        return fail(FacetError::SelfReference,
                    QStringLiteral("%1 cannot be measured relative to itself.").arg(roleLabel(s.what)));

    // A point has no orientation. A knee angle is the angle between the shin and the thigh — two
    // segments — which is why the vocabulary carries limb segments at all.
    if (wc == RoleClass::Point && (s.quantity == Quantity::Angle || s.quantity == Quantity::Rotation))
        return fail(FacetError::PointHasNoAngle,
                    QStringLiteral("%1 is a point and has no angle. A joint angle is between two "
                                   "segments — try the segments either side of the joint.")
                        .arg(roleLabel(s.what)));

    // A segment's Distance or Height would be its own length or elevation, which is never what is
    // meant. Measure one of its endpoints instead.
    if (wc == RoleClass::Segment && (s.quantity == Quantity::Distance || s.quantity == Quantity::Height))
        return fail(FacetError::SegmentHasNoDistance,
                    QStringLiteral("%1 is a segment; its %2 would be its own size. Measure a point "
                                   "on it instead.")
                        .arg(roleLabel(s.what), quantityLabel(s.quantity)));

    // An angle is measured against something with a direction. "Spine angle relative to the lead
    // hand" has no meaning; relative to the ground, or to the lead thigh, does.
    if (s.quantity == Quantity::Angle && rc == RoleClass::Point)
        return fail(FacetError::AngleNeedsDirection,
                    QStringLiteral("An angle needs a reference with a direction — a segment or the "
                                   "ground/target line, not the point %1.")
                        .arg(roleLabel(s.reference)));

    if (s.quantity == Quantity::Rotation && rc == RoleClass::Point)
        return fail(FacetError::AngleNeedsDirection,
                    QStringLiteral("A rotation needs a reference with a direction, not the point %1.")
                        .arg(roleLabel(s.reference)));

    // The ball is a sphere with no orientation and no meaningful height above ground during a golf
    // swing — where it is, and how fast it leaves, is the whole of it.
    if (s.what == AnatomyRole::Ball && s.quantity != Quantity::Distance && s.quantity != Quantity::Speed)
        return fail(FacetError::BallQuantity,
                    QStringLiteral("The ball supports distance and speed only."));

    // Face angle and face rotation are the only face-relative quantities that mean anything.
    if (s.what == AnatomyRole::ClubFace && s.quantity != Quantity::Angle && s.quantity != Quantity::Rotation)
        return fail(FacetError::ClubFaceQuantity,
                    QStringLiteral("The club face supports angle and rotation only."));

    c.valid = true;
    return c;
}

std::vector<Quantity> legalQuantitiesFor(AnatomyRole what)
{
    std::vector<Quantity> out;
    for (Quantity q : allQuantities()) {
        // Probe against a reference that cannot itself be the cause of rejection: the ground is a
        // datum with a direction, so it satisfies both the angle rule and the point rules.
        Series probe{ what, q, AnatomyRole::Ground };
        if (what == AnatomyRole::Ground) probe.reference = AnatomyRole::TargetLine;
        if (validateSeries(probe).valid) { out.push_back(q); continue; }

        // Distance/Height against a datum can fail for a segment subject even though the quantity
        // itself is legal for that subject against a point. Re-probe once with a point reference.
        Series probe2{ what, q, AnatomyRole::PelvisCentre };
        if (what == AnatomyRole::PelvisCentre) probe2.reference = AnatomyRole::Neck;
        if (validateSeries(probe2).valid) out.push_back(q);
    }
    return out;
}

std::vector<AnatomyRole> legalReferencesFor(AnatomyRole what, Quantity q)
{
    std::vector<AnatomyRole> out;
    for (AnatomyRole r : allRoles()) {
        if (validateSeries(Series{ what, q, r }).valid) out.push_back(r);
    }
    return out;
}

ReducerCheck validateReducer(const Reducer &r)
{
    ReducerCheck c;

    if (r.kind == ReducerKind::At) {
        if (!r.anchor.has_value()) {
            c.reason = QStringLiteral("Reading a value needs a phase to read it at.");
            return c;
        }
        c.valid = true;
        return c;
    }

    // Delta and Rate run from the anchor to the window's end; Extremum searches the window.
    if (r.kind == ReducerKind::Delta || r.kind == ReducerKind::Rate) {
        if (!r.anchor.has_value()) {
            c.reason = QStringLiteral("A change needs a starting phase.");
            return c;
        }
        if (*r.anchor == r.window.second) {
            c.reason = QStringLiteral("A change needs two different phases.");
            return c;
        }
        c.valid = true;
        return c;
    }

    // Extremum: the window must span something. The anchor is optional — with it the result is the
    // peak deviation from that phase's value, without it the peak absolute value.
    if (r.window.first == r.window.second) {
        c.reason = QStringLiteral("A peak needs a window spanning two phases.");
        return c;
    }
    c.valid = true;
    return c;
}

// ── Canonical naming ────────────────────────────────────────────────────────

QString canonicalSeriesLabel(const Series &s)
{
    return QStringLiteral("%1 %2 relative to %3")
        .arg(roleLabel(s.what), quantityLabel(s.quantity), roleLabel(s.reference));
}

QString canonicalMeasureLabel(const Series &s, const Reducer &r)
{
    const QString base = canonicalSeriesLabel(s);

    switch (r.kind) {
    case ReducerKind::At:
        return QStringLiteral("%1, at %2").arg(base, phaseLabel(r.anchor.value_or(Phase::Address)));
    case ReducerKind::Delta:
        return QStringLiteral("%1, change %2 to %3")
            .arg(base, phaseLabel(r.anchor.value_or(Phase::Address)), phaseLabel(r.window.second));
    case ReducerKind::Rate:
        return QStringLiteral("%1, rate %2 to %3")
            .arg(base, phaseLabel(r.anchor.value_or(Phase::Address)), phaseLabel(r.window.second));
    case ReducerKind::Extremum: {
        const QString word = (r.sense == ExtremumSense::Min) ? QStringLiteral("lowest")
                                                             : QStringLiteral("peak");
        QString out = QStringLiteral("%1, %2 %3 to %4")
                          .arg(base, word, phaseLabel(r.window.first), phaseLabel(r.window.second));
        if (r.anchor.has_value())
            out += QStringLiteral(" from %1").arg(phaseLabel(*r.anchor));
        return out;
    }
    }
    return base;
}

QString canonicalSeriesId(const Series &s)
{
    return QStringLiteral("%1_%2_%3")
        .arg(camelToken(roleLabel(s.what)), quantityName(s.quantity), camelToken(roleLabel(s.reference)));
}

QString canonicalMeasureId(const Series &s, const Reducer &r)
{
    const QString base = canonicalSeriesId(s);

    switch (r.kind) {
    case ReducerKind::At:
        return QStringLiteral("%1_at_%2").arg(base, phaseToken(r.anchor.value_or(Phase::Address)));
    case ReducerKind::Delta:
        return QStringLiteral("%1_delta_%2_%3")
            .arg(base, phaseToken(r.anchor.value_or(Phase::Address)), phaseToken(r.window.second));
    case ReducerKind::Rate:
        return QStringLiteral("%1_rate_%2_%3")
            .arg(base, phaseToken(r.anchor.value_or(Phase::Address)), phaseToken(r.window.second));
    case ReducerKind::Extremum: {
        QString out = QStringLiteral("%1_%2_%3_%4")
                          .arg(base, extremumSenseName(r.sense),
                               phaseToken(r.window.first), phaseToken(r.window.second));
        if (r.anchor.has_value())
            out += QStringLiteral("_from_%1").arg(phaseToken(*r.anchor));
        return out;
    }
    }
    return base;
}

// ── Structural duplicate detection ──────────────────────────────────────────

int facetDistance(const Series &a, const Series &b)
{
    int d = 0;
    if (a.what != b.what)           ++d;
    if (a.quantity != b.quantity)   ++d;
    if (a.reference != b.reference) ++d;
    return d;
}

bool isNearDuplicate(const Series &a, const Series &b) { return facetDistance(a, b) == 1; }

std::vector<SeriesMatch> findSimilarSeries(const Series &candidate, const std::vector<Series> &existing)
{
    std::vector<SeriesMatch> out;
    for (const Series &e : existing) {
        const int d = facetDistance(candidate, e);
        // Distance 2 and 3 are simply different measures. Reporting them would train authors to
        // dismiss the warning, and then the distance-1 case — the one that matters — goes unread.
        if (d <= 1) out.push_back(SeriesMatch{ e, d });
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const SeriesMatch &l, const SeriesMatch &r) { return l.distance < r.distance; });
    return out;
}

// ── Derived properties ──────────────────────────────────────────────────────

ViewNeeded deriveViewNeeded(const Series &s)
{
    // Heuristic, and documented as such — an author may override it. Only the clear cases are
    // claimed; everything else is Any, which is the safe answer because it asks nothing of capture.

    // The target and ball lines run into the screen face-on, so anything measured against them
    // needs the down-the-line view to have any depth resolution at all.
    if (s.reference == AnatomyRole::TargetLine || s.reference == AnatomyRole::BallLine)
        return ViewNeeded::DownTheLine;

    // Sagittal-plane posture — spinal curvature and forward bend — is what the down-the-line view
    // exists to show.
    if (s.what == AnatomyRole::ThoracicSegment || s.what == AnatomyRole::LumbarSegment)
        return ViewNeeded::DownTheLine;
    if (s.what == AnatomyRole::Spine && s.quantity == Quantity::Angle
        && (s.reference == AnatomyRole::LeadThigh || s.reference == AnatomyRole::TrailThigh))
        return ViewNeeded::DownTheLine;

    // The stance line runs target-parallel through the feet, so a PERPENDICULAR distance to it —
    // how far the ball or the body sits from that line — is measured along the face-on camera's
    // own axis and cannot be recovered from that view. It needs down-the-line. Distance to the
    // stance CENTRE runs along the line and reads cleanly face-on. The two differ only in the
    // reference, which is exactly why that facet is load-bearing.
    if (s.reference == AnatomyRole::StanceLine && s.quantity == Quantity::Distance)
        return ViewNeeded::DownTheLine;

    // Lateral displacement and address geometry measured across the stance read from face-on.
    if (s.reference == AnatomyRole::StanceCentre || s.reference == AnatomyRole::StanceLine)
        return ViewNeeded::FaceOn;

    return ViewNeeded::Any;
}

bool seriesNeedsNonPoseSensor(const Series &s)
{
    return roleNeedsNonPoseSensor(s.what) || roleNeedsNonPoseSensor(s.reference);
}

} // namespace pinpoint::analysis
