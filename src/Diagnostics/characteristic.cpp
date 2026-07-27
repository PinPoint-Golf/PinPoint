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

#include "characteristic.h"

#include <QObject>   // tr() for the direction phrasing — the only user-facing strings in this file

#include <algorithm>

namespace pinpoint::analysis {

namespace {

// One table per enum, so a spelling exists in exactly one place. Every `name` is a stable JSON
// token and must never change once a pack has shipped with it.
template <typename E>
struct Row {
    E           value;
    const char *name;
    const char *label;
};

template <typename E, size_t N>
QString nameOf(const Row<E> (&rows)[N], E v)
{
    for (const auto &r : rows)
        if (r.value == v) return QString::fromLatin1(r.name);
    return QString::fromLatin1(rows[0].name);
}

template <typename E, size_t N>
QString labelOf(const Row<E> (&rows)[N], E v)
{
    for (const auto &r : rows)
        if (r.value == v) return QString::fromLatin1(r.label);
    return QString::fromLatin1(rows[0].label);
}

template <typename E, size_t N>
bool fromName(const Row<E> (&rows)[N], const QString &s, E &out)
{
    for (const auto &r : rows)
        if (s == QLatin1String(r.name)) { out = r.value; return true; }
    return false;
}

const Row<MeasureKind> kMeasureKinds[] = {
    { MeasureKind::Composed, "composed", "composed" },
    { MeasureKind::Provided, "provided", "provided" },
};

const Row<MeasureStatus> kMeasureStatuses[] = {
    { MeasureStatus::Live,          "live",          "Live" },
    { MeasureStatus::Planned,       "planned",       "Planned" },
    { MeasureStatus::NoProducer,    "noProducer",    "No producer" },
    { MeasureStatus::NotCapturable, "notCapturable", "Not measurable from capture" },
};

const Row<SignalTest> kSignalTests[] = {
    { SignalTest::OutsideCorridor, "outsideCorridor", "outside its normal range" },
    { SignalTest::Threshold,       "threshold",       "past a threshold" },
    { SignalTest::Order,           "order",           "out of order" },
    { SignalTest::Ratio,           "ratio",           "out of ratio" },
};

const Row<Direction> kDirections[] = {
    { Direction::High, "high", "too much" },
    { Direction::Low,  "low",  "too little" },
};

const Row<ConditionGroup> kGroups[] = {
    { ConditionGroup::Setup,       "setup",       "Setup" },
    { ConditionGroup::Posture,     "posture",     "Posture" },
    { ConditionGroup::Lateral,     "lateral",     "Lateral" },
    { ConditionGroup::ArmsAndClub, "armsAndClub", "Arms & club" },
    { ConditionGroup::Release,     "release",     "Release" },
    { ConditionGroup::Sequence,    "sequence",    "Sequence" },
};

const Row<Observability> kObservabilities[] = {
    { Observability::Observable, "observable", "Observable" },
    { Observability::Latent,     "latent",     "Latent" },
    { Observability::Both,       "both",       "Both" },
};

const Row<ConfirmedBy> kConfirmedBys[] = {
    { ConfirmedBy::Measured, "measured", "Measured" },
    { ConfirmedBy::Screened, "screened", "Physical" },
    { ConfirmedBy::Asserted, "asserted", "Behavioural" },
};

const Row<ProvenanceTier> kTiers[] = {
    { ProvenanceTier::Proposed,    "proposed",    "Proposed" },
    { ProvenanceTier::Supported,   "supported",   "Supported" },
    { ProvenanceTier::Established, "established", "Established" },
};

const Row<ConditionState> kStates[] = {
    { ConditionState::Draft,             "draft",             "Draft" },
    { ConditionState::Candidate,         "candidate",         "Candidate" },
    { ConditionState::Active,            "active",            "Active" },
    { ConditionState::NeedsRevalidation, "needsRevalidation", "Needs revalidation" },
    { ConditionState::Superseded,        "superseded",        "Superseded" },
    { ConditionState::Retired,           "retired",           "Retired" },
};

const Row<EdgeType> kEdgeTypes[] = {
    { EdgeType::Causes,       "causes",       "causes" },
    { EdgeType::Corroborates, "corroborates", "corroborates" },
    { EdgeType::Excludes,     "excludes",     "excludes" },
};

// Words only. Strengths are not probabilities and must never be rendered as percentages.
const Row<Strength> kStrengths[] = {
    { Strength::Weak,     "weak",     "sometimes" },
    { Strength::Moderate, "moderate", "often" },
    { Strength::Strong,   "strong",   "usually" },
};

} // namespace

const Measure *CharacteristicPack::measure(const QString &mid) const
{
    const auto it = std::find_if(measures.begin(), measures.end(),
                                 [&](const Measure &m) { return m.id == mid; });
    return it == measures.end() ? nullptr : &*it;
}

const Signal *CharacteristicPack::signal(const QString &sid) const
{
    const auto it = std::find_if(signalDefs.begin(), signalDefs.end(),
                                 [&](const Signal &s) { return s.id == sid; });
    return it == signalDefs.end() ? nullptr : &*it;
}

const Condition *CharacteristicPack::condition(const QString &cid) const
{
    const auto it = std::find_if(conditions.begin(), conditions.end(),
                                 [&](const Condition &c) { return c.id == cid; });
    return it == conditions.end() ? nullptr : &*it;
}

QString measureKindName(MeasureKind k) { return nameOf(kMeasureKinds, k); }
bool    measureKindFromName(const QString &s, MeasureKind &out) { return fromName(kMeasureKinds, s, out); }

QString measureStatusName(MeasureStatus s) { return nameOf(kMeasureStatuses, s); }
QString measureStatusLabel(MeasureStatus s) { return labelOf(kMeasureStatuses, s); }
bool    measureStatusFromName(const QString &s, MeasureStatus &out) { return fromName(kMeasureStatuses, s, out); }

QString signalTestName(SignalTest t) { return nameOf(kSignalTests, t); }
bool    signalTestFromName(const QString &s, SignalTest &out) { return fromName(kSignalTests, s, out); }

QString directionName(Direction d) { return nameOf(kDirections, d); }
bool    directionFromName(const QString &s, Direction &out) { return fromName(kDirections, s, out); }

QString conditionGroupName(ConditionGroup g)  { return nameOf(kGroups, g); }
QString conditionGroupLabel(ConditionGroup g) { return labelOf(kGroups, g); }
bool    conditionGroupFromName(const QString &s, ConditionGroup &out) { return fromName(kGroups, s, out); }

QString observabilityName(Observability o) { return nameOf(kObservabilities, o); }
bool    observabilityFromName(const QString &s, Observability &out) { return fromName(kObservabilities, s, out); }

QString confirmedByName(ConfirmedBy c) { return nameOf(kConfirmedBys, c); }
bool    confirmedByFromName(const QString &s, ConfirmedBy &out) { return fromName(kConfirmedBys, s, out); }

QString provenanceTierName(ProvenanceTier t) { return nameOf(kTiers, t); }
bool    provenanceTierFromName(const QString &s, ProvenanceTier &out) { return fromName(kTiers, s, out); }

QString conditionStateName(ConditionState s) { return nameOf(kStates, s); }
bool    conditionStateFromName(const QString &s, ConditionState &out) { return fromName(kStates, s, out); }

QString edgeTypeName(EdgeType t) { return nameOf(kEdgeTypes, t); }
bool    edgeTypeFromName(const QString &s, EdgeType &out) { return fromName(kEdgeTypes, s, out); }

QString strengthName(Strength s)  { return nameOf(kStrengths, s); }
QString strengthLabel(Strength s) { return labelOf(kStrengths, s); }
bool    strengthFromName(const QString &s, Strength &out) { return fromName(kStrengths, s, out); }

QString reachLabel(ConfirmedBy c) { return labelOf(kConfirmedBys, c); }

QString reachHint(ConfirmedBy c)
{
    switch (c) {
    case ConfirmedBy::Measured: return QStringLiteral("measured from the swing");
    case ConfirmedBy::Screened: return QStringLiteral("needs a physical screen");
    case ConfirmedBy::Asserted: return QStringLiteral("ask the golfer");
    }
    return QString();
}

DirectionPhrase directionPhrase(Direction d, const QString &highMeans)
{
    const QString   h = highMeans.trimmed();
    const bool      high = (d == Direction::High);
    DirectionPhrase p;
    p.label = high ? QObject::tr("Too much") : QObject::tr("Too little");

    if (h.isEmpty()) {
        p.sentence = high ? QObject::tr("Flagged when the value is higher than the norm.")
                          : QObject::tr("Flagged when the value is lower than the norm.");
        return p;
    }

    p.means    = high ? h : QObject::tr("the other end of that range");
    p.sentence = high
                     ? QObject::tr("Flagged when there is more of it: %1.").arg(h)
                     : QObject::tr("Flagged at the other end of the same range — the opposite of: %1.")
                           .arg(h);
    return p;
}

} // namespace pinpoint::analysis
