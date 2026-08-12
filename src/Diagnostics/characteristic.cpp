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

#include "enum_table_p.h"

#include <QObject>   // tr() for the direction phrasing — the only user-facing strings in this file

#include <algorithm>
#include <vector>

namespace pinpoint::analysis {

namespace {

// The Row/nameOf/labelOf/fromName machinery is shared with norm.cpp — see enum_table_p.h,
// which also carries the reason a label is decoded as UTF-8 and a token as Latin-1. What stays here
// is what is genuinely this file's: one table per enum, so a spelling exists in exactly one place.
// Every `name` is a stable JSON token and must never change once a pack has shipped with it.
using detail::fromName;
using detail::labelOf;
using detail::nameOf;
using detail::Row;

const Row<MeasureKind> kMeasureKinds[] = {
    { MeasureKind::Composed, "composed", "composed" },
    { MeasureKind::Provided, "provided", "provided" },
};

const Row<MeasureStatus> kMeasureStatuses[] = {
    { MeasureStatus::Live,           "live",           "Live" },
    { MeasureStatus::Planned,        "planned",        "Planned" },
    { MeasureStatus::NoProducer,     "noProducer",     "No producer" },
    { MeasureStatus::NotCapturable,  "notCapturable",  "Not measurable from capture" },
    { MeasureStatus::ExternalDevice, "externalDevice", "Needs a launch monitor" },
};

// The label is what an AUTHOR reads beside the measure's own `highMeans` sentence, so it says what
// the shape means rather than naming the enum: "higher is better: more of the clubhead speed
// reaching the ball" is a sentence somebody can check. "Floor" is not.
const Row<Shape> kShapes[] = {
    { Shape::Target,  "target",  "A range to sit inside" },
    { Shape::Floor,   "floor",   "Higher is better" },
    { Shape::Ceiling, "ceiling", "Lower is better" },
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
    { ConditionGroup::Impact,      "impact",      "Impact" },
    { ConditionGroup::Finish,      "finish",      "Finish" },
    { ConditionGroup::BallFlight,  "ballFlight",  "Ball flight" },
};

// The token `setup` is ALSO a ConditionGroup token, and a shipped row carries both — `"group":
// "setup", "kind": "setup"` is correct and means two different things. The tables are separate and
// the parsers never cross, but an error message that says only "unknown setup" would send an author
// to the wrong field, so both loaders name the field they are talking about.
const Row<ConditionKind> kKinds[] = {
    { ConditionKind::Fault,     "fault",     "Fault" },
    { ConditionKind::Setup,     "setup",     "Setup" },
    { ConditionKind::Delivery,  "delivery",  "Delivery" },
    { ConditionKind::Outcome,   "outcome",   "Outcome" },
    { ConditionKind::Capacity,  "capacity",  "Capacity" },
    { ConditionKind::Intent,    "intent",    "Intent" },
    { ConditionKind::Equipment, "equipment", "Equipment" },
};

// Names and labels agree here, unlike kStrengths, and that is deliberate: a strength is authored as
// a magnitude and read as a frequency, whereas a prominence is authored and read as the same claim.
// Only the top rung diverges, because "Ubiquitous" is a word for a document and not for a chip.
const Row<Prominence> kProminences[] = {
    { Prominence::Rare,       "rare",       "Rare" },
    { Prominence::Uncommon,   "uncommon",   "Uncommon" },
    { Prominence::Occasional, "occasional", "Occasional" },
    { Prominence::Common,     "common",     "Common" },
    { Prominence::Ubiquitous, "ubiquitous", "Almost everyone" },
};

const Row<Observability> kObservabilities[] = {
    { Observability::Observable, "observable", "Observable" },
    { Observability::Latent,     "latent",     "Latent" },
    { Observability::Both,       "both",       "Both" },
};

const Row<DetectionMode> kDetectionModes[] = {
    { DetectionMode::Any, "any", "any signal" },
    { DetectionMode::All, "all", "every signal" },
};

const Row<ConfirmedBy> kConfirmedBys[] = {
    { ConfirmedBy::Measured, "measured", "Measured" },
    { ConfirmedBy::Screened, "screened", "Physical" },
    { ConfirmedBy::Asserted, "asserted", "Behavioural" },
};

const Row<ProvenanceTier> kTiers[] = {
    { ProvenanceTier::Proposed,      "proposed",      "Proposed" },
    { ProvenanceTier::NoSourceFound, "noSourceFound", "No source found" },
    { ProvenanceTier::Practice,      "practice",      "Coaching practice" },
    { ProvenanceTier::Indirect,      "indirect",      "Indirect support" },
    { ProvenanceTier::Supported,     "supported",     "Supported" },
    { ProvenanceTier::Established,   "established",   "Established" },
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
//
// The labels are a FREQUENCY ladder and the names a magnitude one. They have never agreed, and the
// names are the ones that must not move: they are what 300-odd shipped edges and every user pack
// already store. The outer two follow the naming idiom rather than the label's, for that reason
// alone.
const Row<Strength> kStrengths[] = {
    { Strength::VeryWeak,   "veryWeak",   "rarely" },
    { Strength::Weak,       "weak",       "sometimes" },
    { Strength::Moderate,   "moderate",   "often" },
    { Strength::Strong,     "strong",     "usually" },
    { Strength::VeryStrong, "veryStrong", "always" },
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

QString shapeName(Shape s)  { return nameOf(kShapes, s); }
QString shapeLabel(Shape s) { return labelOf(kShapes, s); }
bool    shapeFromName(const QString &s, Shape &out) { return fromName(kShapes, s, out); }

QString signalTestName(SignalTest t) { return nameOf(kSignalTests, t); }
bool    signalTestFromName(const QString &s, SignalTest &out) { return fromName(kSignalTests, s, out); }

QString directionName(Direction d) { return nameOf(kDirections, d); }
bool    directionFromName(const QString &s, Direction &out) { return fromName(kDirections, s, out); }

QString conditionGroupName(ConditionGroup g)  { return nameOf(kGroups, g); }
QString conditionGroupLabel(ConditionGroup g) { return labelOf(kGroups, g); }
bool    conditionGroupFromName(const QString &s, ConditionGroup &out) { return fromName(kGroups, s, out); }

const std::vector<ConditionGroup> &allConditionGroups()
{
    static const std::vector<ConditionGroup> v = [] {
        std::vector<ConditionGroup> r;
        for (const auto &row : kGroups) r.push_back(row.value);
        return r;
    }();
    return v;
}

QString conditionKindName(ConditionKind k)  { return nameOf(kKinds, k); }
QString conditionKindLabel(ConditionKind k) { return labelOf(kKinds, k); }
bool    conditionKindFromName(const QString &s, ConditionKind &out) { return fromName(kKinds, s, out); }

const std::vector<ConditionKind> &allConditionKinds()
{
    static const std::vector<ConditionKind> v = [] {
        std::vector<ConditionKind> r;
        for (const auto &row : kKinds) r.push_back(row.value);
        return r;
    }();
    return v;
}

QString prominenceName(Prominence p)  { return nameOf(kProminences, p); }
QString prominenceLabel(Prominence p) { return labelOf(kProminences, p); }
bool    prominenceFromName(const QString &s, Prominence &out) { return fromName(kProminences, s, out); }

const std::vector<Prominence> &allProminences()
{
    static const std::vector<Prominence> v = [] {
        std::vector<Prominence> r;
        for (const auto &row : kProminences) r.push_back(row.value);
        return r;
    }();
    return v;
}

double prominenceWeight(Prominence p)
{
    // P(condition) — how much of the population carries this at all. See the header for the two
    // reasons neither bound is available (they are NOT strengthWeight()'s reasons) and for why the
    // 12x spread against that ladder's 9.5x is the part of this table that was chosen.
    //
    // Read them as frequencies an author can check a row against: one in twenty, one in ten, one in
    // five, one in three, three in five. That is the resolution an editorial judgement supports, and
    // no finer.
    switch (p) {
    case Prominence::Ubiquitous: return 0.60;
    case Prominence::Common:     return 0.35;
    case Prominence::Occasional: return 0.20;
    case Prominence::Uncommon:   return 0.10;
    case Prominence::Rare:       return 0.05;
    }
    return 0.20;                      // unreachable-but-safe, mirrors the default rung
}

QString observabilityName(Observability o) { return nameOf(kObservabilities, o); }
bool    observabilityFromName(const QString &s, Observability &out) { return fromName(kObservabilities, s, out); }

QString detectionModeName(DetectionMode d) { return nameOf(kDetectionModes, d); }
bool    detectionModeFromName(const QString &s, DetectionMode &out) { return fromName(kDetectionModes, s, out); }

QString confirmedByName(ConfirmedBy c) { return nameOf(kConfirmedBys, c); }
bool    confirmedByFromName(const QString &s, ConfirmedBy &out) { return fromName(kConfirmedBys, s, out); }

QString provenanceTierName(ProvenanceTier t) { return nameOf(kTiers, t); }
QString provenanceTierLabel(ProvenanceTier t) { return labelOf(kTiers, t); }
bool    provenanceTierFromName(const QString &s, ProvenanceTier &out) { return fromName(kTiers, s, out); }

QString conditionStateName(ConditionState s) { return nameOf(kStates, s); }
bool    conditionStateFromName(const QString &s, ConditionState &out) { return fromName(kStates, s, out); }

QString edgeTypeName(EdgeType t) { return nameOf(kEdgeTypes, t); }
bool    edgeTypeFromName(const QString &s, EdgeType &out) { return fromName(kEdgeTypes, s, out); }

QString strengthName(Strength s)  { return nameOf(kStrengths, s); }
QString strengthLabel(Strength s) { return labelOf(kStrengths, s); }
bool    strengthFromName(const QString &s, Strength &out) { return fromName(kStrengths, s, out); }

const std::vector<Strength> &allStrengths()
{
    static const std::vector<Strength> v = [] {
        std::vector<Strength> r;
        for (const auto &row : kStrengths) r.push_back(row.value);
        return r;
    }();
    return v;
}

double strengthWeight(Strength s)
{
    // P(effect | cause), which is what the labels have always described: how OFTEN this cause
    // produces this effect. See the header for what may and may not be built on that.
    //
    // Neither end touches its bound. 0 is taken — edgeWeight() returns it for "there is no such
    // edge", and an immaterial finding is deliberately weighted 0 — so a rung there would make a
    // claim indistinguishable from its own absence. 1 is worse: a probability of exactly 1 is
    // absorbing, and no evidence can ever revise it afterwards. An author writing "always" is
    // stating a very strong regularity, not a law with no exceptions, and the ceiling has to leave
    // room for them to be wrong.
    switch (s) {
    case Strength::VeryStrong: return 0.95;
    case Strength::Strong:     return 0.80;
    case Strength::Moderate:   return 0.60;
    case Strength::Weak:       return 0.30;
    case Strength::VeryWeak:   return 0.10;
    }
    return 0.60;
}

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
