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

// SESSION DIAGNOSTICS ledger — pure, header-only, no Qt-GUI. The maths behind
// PpSessionDiagnosticsPanel: one row per shot per condition, and every tier, recurrence
// caption, trend, link grade, chain and stage the panel draws reduced out of those rows
// and nothing else. Sibling of lm_session_reductions.h and kept out of QML JS by the same
// rule (analysis pipeline guide §6.2): QML positions and paints, C++ decides the numbers.
//
// Per-shot surfaces answer "what happened on that swing". This answers "what keeps
// happening, and what does the authored model think is causing it" — which is a far more
// dangerous question, because a session is fourteen swings and the temptation to say more
// than fourteen swings support is enormous. Most of what follows is that restraint made
// executable.
//
// FOUR RULES DECIDE EVERY NUMBER HERE, and each of them is a decision rather than an
// implementation detail:
//
//   1. NotAssessable is neither clean nor a zero. A measure the capture never took is
//      excluded from EVERY denominator in this file — the recurrence, the Wilson bound,
//      the trend series, the paired 2×2, the resolving window, the focus-split windows.
//      It is still carried, still counted, still drawn (as an outlined tick, never a
//      gap), because "we did not look" is a fact the golfer is owed. The single easiest
//      way to make this panel lie is to let an occluded shaft read as a clean swing.
//   2. The WILSON LOWER BOUND, never the raw proportion. 1-of-2 and 8-of-10 have similar
//      rates and utterly different evidence; the bound is what separates them without a
//      second rule, and it is why the gate can be one number. The bound is an ORDINAL
//      GATE, not a probability claim — nothing in this file publishes it as a percentage,
//      and the recurrence caption is always a COUNT ("8 of 12 measurable shots") because
//      at this n a percentage is a fabricated precision.
//   3. NO EDGE IS EVER INVENTED. The pack's causal DAG is prior knowledge, fixed before
//      a ball is struck; the session only estimates whether an authored mechanism is
//      active in this golfer today. That is also the multiple-comparisons correction —
//      ~140 conditions imply ~10,000 pairs, and testing only the authored edges among
//      this session's pattern-tier conditions is a handful of pre-specified hypotheses
//      instead of a fishing expedition. The corollary is equally load-bearing: a pattern
//      with no authored edge is REPORTED ON ITS OWN LINE, never dropped for not fitting.
//   4. EVERY GATE NUMBER IS INJECTED. LedgerOptions carries the lot, defaulted from the
//      design document's §4 table, so the corpus can tune them without touching a body.
//      There are no bare thresholds below this line.
//
// TWO THINGS ARE DELIBERATELY ABSENT and their absence is part of the contract.
//
//   The FAULT PROFILE (prior sessions' prevalence) has no input here at all. It biases
//   ranking and presentation and nothing else, and the way to guarantee that in code is
//   to give the arithmetic no way to see it: identical rows produce identical tiers, full
//   stop. hystereticOrder() is where a ranking bias would legitimately enter, and it
//   cannot reach a tier from there.
//
//   CADENCE (Bandwidth / Every shot / Block / Quiet) does not appear in this file either.
//   Cadence gates SURFACING, never the engine — the ledger accumulates at full rate no
//   matter how quiet the panel is being. A cadence parameter in a reduction is how that
//   rule gets broken by accident, so there is not one to pass.
//
// The QML façade is SessionDiagnosticsModel (src/Gui/session/session_diagnostics_model.h).
// Unit-tested standalone in src/Analysis/tests/diagnostic_ledger_test.cpp.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace pinpoint::analysis {

// ── §4 policy numbers — defaults only; every one of them is injected ─────────────
//
// These are the design document's tunables table transcribed, and they exist as named
// constants rather than as literals in a signature so that a corpus sweep changes one
// line and the test that pins the boundary keeps naming the same thing.

// The pattern gate. Ordinal, not a probability: "the evidence clears the bar", not
// "there is a 30% chance". Crossing it is the ONLY event that moves a tier.
inline constexpr double kDiagPatternWilsonLb = 0.30;

// Three assessable shots before any pattern claim, mirroring kMinShotsForSpread for the
// same reason: below it the statistic is a restatement of the last two swings.
inline constexpr int kDiagMinAssessableForPattern = 3;

// Below this share of agreement among a condition's firings, the DIRECTION claim is
// suppressed and the condition presents as dispersion. It stays a pattern — inconsistency
// is a finding, not an absence of one.
inline constexpr double kDiagDirectionAgreementGate = 0.70;

// Theil–Sen needs points before it means anything; five is the floor.
inline constexpr int kDiagMinPointsForTrend = 5;

// Kendall-τ two-sided significance for the trend verdict. Used as an ORDINAL gate on an
// arrow, not as a hypothesis test — shots are not i.i.d. and this file never pretends so.
inline constexpr double kDiagTrendAlpha = 0.05;

// A pattern that has not fired in this many ASSESSABLE shots presents as resolving. The
// panel rewards the fix rather than punishing the history.
inline constexpr int kDiagResolvingWindow = 5;

// "Crossed into Pattern within the last N shots" — the NEW badge's life. Counted in
// SHOTS, not assessable shots: it is about what the golfer has seen since, and they saw
// every shot whether the capture did or not.
inline constexpr int kDiagFreshWindow = 2;

// Fewest shots on which BOTH endpoints were assessable before a 2×2 may be tested. The
// most exposed number in the file to wishful reading, which is why it is the highest.
inline constexpr int kDiagMinPairsForDependence = 8;

// Fisher's exact one-sided threshold for conditional dependence.
inline constexpr double kDiagFisherAlpha = 0.05;

// The n-of-1 quasi-experiment's windows: assessable shots either side of a declared
// focus. Honest ONLY under a focus contract, which is why the grade refuses without one.
inline constexpr int kDiagMovedTogetherWindow = 5;

// Rank-band hysteresis. A card moves in the display order only when its rank changes by
// MORE than this — the golfer must never watch the app change its mind mid-thought.
inline constexpr int kDiagRankBandWidth = 1;

// The driver footer stays empty until the pattern set has been unchanged this many shots.
// Better absent than flickering.
inline constexpr int kDiagDriverDebounceShots = 3;

// Warm-up: the first N shots are not representative and would seed early patterns badly.
// Down-weighted (§5.4), never excluded — a shank on shot 1 is still a shank.
inline constexpr int kDiagWarmUpShots  = 3;
inline constexpr double kDiagWarmUpWeight = 0.5;

// 95% one-sided normal deviate for the Wilson bound.
inline constexpr double kDiagWilsonZ = 1.96;

// diagnostics.json's schema tag. Bumped when the ROW shape changes, never when a gate
// number moves — the file persists the evidence, and the gates are applied on read.
inline constexpr int kDiagSchemaVersion = 1;

// Every §4 tunable in one injectable bundle.
struct LedgerOptions {
    double patternWilsonLb        = kDiagPatternWilsonLb;
    int    minAssessableForPattern= kDiagMinAssessableForPattern;
    double directionAgreementGate = kDiagDirectionAgreementGate;
    int    minPointsForTrend      = kDiagMinPointsForTrend;
    double trendAlpha             = kDiagTrendAlpha;
    int    resolvingWindow        = kDiagResolvingWindow;
    int    freshWindow            = kDiagFreshWindow;
    int    minPairsForDependence  = kDiagMinPairsForDependence;
    double fisherAlpha            = kDiagFisherAlpha;
    int    movedTogetherWindow    = kDiagMovedTogetherWindow;
    int    rankBandWidth          = kDiagRankBandWidth;
    int    driverDebounceShots    = kDiagDriverDebounceShots;
    int    warmUpShots            = kDiagWarmUpShots;
    // 1.0 disables warm-up handling entirely without a second flag to keep in step.
    double warmUpWeight           = kDiagWarmUpWeight;
    double wilsonZ                = kDiagWilsonZ;
};

// ── Statistical primitives ──────────────────────────────────────────────────────

// Wilson score interval, LOWER bound, at `z` (1.96 ⇒ 95%). Clamped at zero.
//
// Counts are DOUBLES because warm-up down-weighting produces fractional effective counts
// (9 firings of which two were warm-up is 8.0 effective firings, not 9). The Wilson form
// is a smooth function of k and n and extends to fractional counts without ceremony; what
// it must never do is go negative, because a lower bound below zero is not a rate the
// golfer can be shown and the gate compares against 0.30.
//
// n == 0 returns 0. Not a NaN, not a 1 — no assessable shots is no evidence, and the
// pattern gate must read it as no evidence rather than as an error to be special-cased at
// nine call sites.
inline double wilsonLowerBound(double fired, double assessable, double z = kDiagWilsonZ)
{
    if (!(assessable > 0.0) || !std::isfinite(fired) || !std::isfinite(assessable))
        return 0.0;
    const double n  = assessable;
    const double p  = std::clamp(fired / n, 0.0, 1.0);
    const double z2 = z * z;
    const double centre = p + z2 / (2.0 * n);
    const double inner  = p * (1.0 - p) / n + z2 / (4.0 * n * n);
    const double lo = (centre - z * std::sqrt(std::max(inner, 0.0))) / (1.0 + z2 / n);
    return std::isfinite(lo) ? std::max(0.0, lo) : 0.0;
}

// Median of a copy. Even n takes the mean of the two central values.
inline double medianOf(std::vector<double> v)
{
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(mid), v.end());
    const double hi = v[mid];
    if (v.size() % 2 == 1) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + long(mid));
    return 0.5 * (lo + hi);
}

// Theil–Sen slope: the MEDIAN of the pairwise slopes.
//
// Chosen over least squares for one reason that matters more here than anywhere else in
// the codebase: a golf session contains shanks. Least squares hands a single wild swing
// enough leverage to invert an arrow, and an arrow that flips because of one ball is worse
// than no arrow. Theil–Sen has a ~29% breakdown point by construction — the outlier
// contributes a handful of extreme pairwise slopes to a median that ignores them.
//
// Fewer than two points, or every x identical, is a slope of zero rather than a NaN: the
// caller's gate is "enough points" and it should not also have to be "and finite".
inline double theilSenSlope(const std::vector<double> &x, const std::vector<double> &y)
{
    const int n = int(std::min(x.size(), y.size()));
    if (n < 2) return 0.0;

    std::vector<double> slopes;
    slopes.reserve(size_t(n) * size_t(n - 1) / 2);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const double dx = x[size_t(j)] - x[size_t(i)];
            if (dx == 0.0) continue;
            const double s = (y[size_t(j)] - y[size_t(i)]) / dx;
            if (std::isfinite(s)) slopes.push_back(s);
        }
    }
    if (slopes.empty()) return 0.0;
    const double m = medianOf(std::move(slopes));
    return std::isfinite(m) ? m : 0.0;
}

// The same against an implicit x of 0, 1, 2… — the ordinal position of the assessable
// shot in its own series, which is what every trend in this file is measured against.
inline double theilSenSlope(const std::vector<double> &series)
{
    std::vector<double> x(series.size());
    for (size_t i = 0; i < x.size(); ++i) x[i] = double(i);
    return theilSenSlope(x, series);
}

// Kendall's TAU-B — the tie-handled variant, and the -b is not optional here.
//
// z-scores from a quantised measure tie constantly (a launch monitor reporting whole
// degrees, a corridor with a coarse sigma), and tau-a divides by the untied pair count
// regardless, so a heavily tied series reads as a weak trend when it is in fact a clean
// one. tau-b divides by the geometric mean of the un-tied pairs in each variable, which
// is the correction that keeps a step-shaped improvement legible.
//
// Returns 0 for fewer than two points or a fully-tied variable (no ordering to detect).
inline double kendallTau(const std::vector<double> &x, const std::vector<double> &y)
{
    const int n = int(std::min(x.size(), y.size()));
    if (n < 2) return 0.0;

    double sum = 0.0;          // concordant − discordant
    double tiesX = 0.0, tiesY = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const double dx = x[size_t(j)] - x[size_t(i)];
            const double dy = y[size_t(j)] - y[size_t(i)];
            const int sx = (dx > 0.0) - (dx < 0.0);
            const int sy = (dy > 0.0) - (dy < 0.0);
            if (sx == 0) tiesX += 1.0;
            if (sy == 0) tiesY += 1.0;
            sum += double(sx * sy);
        }
    }
    const double n0 = 0.5 * double(n) * double(n - 1);
    const double den = std::sqrt((n0 - tiesX) * (n0 - tiesY));
    if (!(den > 0.0)) return 0.0;
    return std::clamp(sum / den, -1.0, 1.0);
}

// Two-sided p for a tau under the normal approximation. Ordinal gating only (§5.6).
inline double kendallTauPValue(double tau, int n)
{
    if (n < 3) return 1.0;
    const double num = 3.0 * tau * std::sqrt(double(n) * double(n - 1));
    const double den = std::sqrt(2.0 * (2.0 * double(n) + 5.0));
    if (!(den > 0.0)) return 1.0;
    const double zStat = num / den;
    return std::clamp(std::erfc(std::fabs(zStat) / std::sqrt(2.0)), 0.0, 1.0);
}

// log C(n, k) through lgamma — the only way to reach a 2×2 tail at session n without
// overflowing a factorial or reaching for a big-integer type.
inline double logChoose(int n, int k)
{
    if (k < 0 || n < 0 || k > n) return -std::numeric_limits<double>::infinity();
    return std::lgamma(double(n) + 1.0) - std::lgamma(double(k) + 1.0)
         - std::lgamma(double(n - k) + 1.0);
}

// Fisher's exact test, ONE-SIDED, on the 2×2
//
//                 downstream fired   downstream clean
//   upstream fired        a                 b
//   upstream clean        c                 d
//
// WHICH TAIL, AND WHY IT IS A CHOICE. The tail summed is the UPPER one — P(X ≥ a) under
// the hypergeometric with both margins fixed. That is the probability of seeing this much
// co-firing OR MORE by chance, which tests exactly the hypothesis the pack authored: the
// edge says "upstream makes downstream more likely", and the edge is the only reason this
// pair is being tested at all (rule 3). A two-sided test would spend half its alpha on the
// direction the model does not claim, and a lower tail would be testing a mechanism nobody
// wrote down. The caller is responsible for orienting (a,b,c,d) so that row 1 is the
// upstream firing — grading a link the other way round asks a different question.
//
// Degenerate margins return 1.0: a table with an empty row or column carries no evidence
// of dependence, and 1.0 is what the alpha gate needs to read there.
inline double fisherExactOneSided(int a, int b, int c, int d)
{
    if (a < 0 || b < 0 || c < 0 || d < 0) return 1.0;
    const int r1 = a + b, r2 = c + d;
    const int c1 = a + c, c2 = b + d;
    const int n  = r1 + r2;
    if (r1 <= 0 || r2 <= 0 || c1 <= 0 || c2 <= 0 || n <= 0) return 1.0;

    const double lden = logChoose(n, c1);
    double p = 0.0;
    const int hi = std::min(r1, c1);
    for (int x = a; x <= hi; ++x) {
        const int y = c1 - x;
        if (y < 0 || y > r2) continue;
        p += std::exp(logChoose(r1, x) + logChoose(r2, y) - lden);
    }
    return std::clamp(p, 0.0, 1.0);
}

// ── The row model (design §A2 / brief §3.1) ─────────────────────────────────────

// Exactly one of three states per condition per shot. There is no fourth, and the third
// is the whole reason this is an enum rather than a bool.
enum class ShotState { Fired, Clean, NotAssessable };

// One condition's reading on one shot.
struct ConditionRow {
    QString   conditionId;
    ShotState state = ShotState::NotAssessable;

    float   confidence = 1.0f;      // the detector's own confidence in this reading
    int     direction  = 0;         // +1 / −1 in the measure's own sense; 0 = unsigned

    QString drivingMeasureId;       // the measure the corridor was applied to
    double  value      = 0.0;
    double  corridorLo = 0.0;
    double  corridorHi = 0.0;
    double  z          = 0.0;       // signed departure from the corridor, in sigma

    QString contextId;              // the corridor's context (club, tee, …)
    bool    contextInferred = false;// carried into ranking as a confidence demotion

    // The pack's materiality flag, CARRIED AND NEVER ARITHMETIC. An immaterial firing is
    // still a firing in every denominator here — letting materiality quietly shrink a
    // count is how a ledger starts disagreeing with the ticks drawn beside it.
    bool    material = true;

    // Why the capture could not assess this one — "shaft occluded at P6", "ball not
    // tracked". Never blank on a NotAssessable row: the review strip prints it in the
    // slot where a corridor would go, and a blank there reads as a bug.
    QString notAssessableReason;
};

// One shot: its metadata and one row per condition the pack tried to assess.
struct ShotRecord {
    int     shotId      = 0;
    QString club;
    QString contextId;
    qint64  timestampMs = 0;
    bool    warmUp      = false;    // declared warm-up, over and above the first-N rule
    std::vector<ConditionRow> rows;
};

// The row for `conditionId` on this shot, or nullptr. Linear — a shot carries tens of
// rows, not thousands.
inline const ConditionRow *rowFor(const ShotRecord &shot, const QString &conditionId)
{
    for (const ConditionRow &r : shot.rows)
        if (r.conditionId == conditionId) return &r;
    return nullptr;
}

// This shot's weight on the effective Wilson counts. Warm-up shots are DOWN-WEIGHTED,
// not dropped: the swings are real and the ticks are drawn, they simply seed a pattern
// claim less hard than a shot hit once the golfer is loose.
//
// UPSTREAM NOTE: the design mock (`Session Dashboard.dc.html`, `_sdRows`) has no warm-up
// handling at all — every shot weighs 1. The mock's published tiers and recurrence counts
// therefore correspond to opt.warmUpWeight == 1.0, and the parity test in
// diagnostic_ledger_test.cpp pins them with warm-up disabled for exactly that reason. The
// mock needs regenerating with §5.4 weighting before its numbers can be quoted as final.
inline double shotWeight(const ShotRecord &shot, int index, const LedgerOptions &opt)
{
    const bool warm = shot.warmUp || index < opt.warmUpShots;
    return warm ? opt.warmUpWeight : 1.0;
}

// ── Tiers (brief §3.2, design §A3) ──────────────────────────────────────────────

enum class Tier { Clean, Watching, Pattern };

// The gate, isolated so the boundary is testable without building a session around it.
//
// Both halves are required and neither is sufficient: 2-of-2 has a splendid raw rate and
// does not reach the assessable floor; 1-of-14 clears the floor and not the bound. Below
// the gate, one firing is WATCHING — which IS the outlier discard, done by evidence weight
// rather than by deleting data, so a single shank sits there forever if it never recurs.
inline Tier conditionTier(int assessable, int fired, double wilsonLower,
                          const LedgerOptions &opt = LedgerOptions())
{
    if (assessable >= opt.minAssessableForPattern && wilsonLower >= opt.patternWilsonLb)
        return Tier::Pattern;
    if (fired >= 1) return Tier::Watching;
    return Tier::Clean;
}

enum class Trend { Improving, Stable, Worsening };

// One condition's session ledger — the reduction the whole panel is drawn from.
struct ConditionLedger {
    QString id;
    QString drivingMeasureId;

    // Rule 1: both of these count ASSESSABLE shots only.
    int assessable = 0;
    int fired      = 0;

    // The same two after warm-up weighting. These, not the raw counts, feed the bound —
    // while the RECURRENCE CAPTION quotes the raw counts, because the caption is a
    // statement about swings that happened and a golfer can count them.
    double effAssessable = 0.0;
    double effFired      = 0.0;

    double wilsonLower = 0.0;
    Tier   tier        = Tier::Clean;

    int  sinceLastFiring = -1;      // ASSESSABLE shots since the last firing; −1 = never
    bool freshThisShot   = false;   // crossed into Pattern within opt.freshWindow shots
    bool resolving       = false;   // Pattern && sinceLastFiring >= opt.resolvingWindow

    double directionAgreement = 1.0;// modal share among SIGNED firings
    int    modalDirection     = 0;  // +1 / −1 / 0 when unsigned or split dead level
    bool   directionClaimed   = true; // agreement >= gate; false ⇒ dispersion, not a
                                      // direction. The WORDING is the panel's job.

    // Per-shot state, full session length, aligned to the shot vector — this is what the
    // tick run is drawn from, and it is why a NotAssessable shot is a short outlined tick
    // and never a gap.
    std::vector<ShotState> run;
    // Signed z aligned the same way; NaN where the shot was not assessable. The paired
    // link tests and the focus-split windows read this, so they inherit rule 1 for free.
    std::vector<double>    zRun;

    ShotState latest = ShotState::NotAssessable;

    // Trend over the assessable z series only.
    bool   trendKnown = false;      // enough points AND a significant tau
    Trend  trend      = Trend::Stable;
    double trendSlope = 0.0;        // Theil–Sen, per assessable shot, on |z|
    double trendTau   = 0.0;
    double trendP     = 1.0;
    int    trendPoints= 0;

    // "8 of 12 measurable shots". A COUNT, always — never a bare percentage at this n
    // (brief §5.1). Built here rather than in the panel so an export and the screen
    // cannot disagree about what recurrence looks like.
    QString recurrence;

    // True when the condition fired on EVERY assessable shot. Its outgoing links cap at
    // Coherent because there is no variance left to covary with (design §A4).
    bool rangeRestricted = false;
};

// The recurrence caption. One place, one wording.
inline QString recurrenceText(int fired, int assessable)
{
    return QString::number(fired) + QStringLiteral(" of ") + QString::number(assessable)
         + QStringLiteral(" measurable shots");
}

// The ledger for `id`, or nullptr.
inline const ConditionLedger *ledgerFor(const std::vector<ConditionLedger> &ls, const QString &id)
{
    for (const ConditionLedger &l : ls)
        if (l.id == id) return &l;
    return nullptr;
}

// Tier/bound over the FIRST `n` shots of an already-extracted run. The prefix form exists
// for freshThisShot, which is a statement about what the ledger looked like two shots ago
// and cannot be answered from the final state.
struct TierSnapshot {
    int    assessable  = 0;
    int    fired       = 0;
    double wilsonLower = 0.0;
    Tier   tier        = Tier::Clean;
};

inline TierSnapshot tierAtPrefix(const std::vector<ShotState> &run,
                                 const std::vector<double>    &weights,
                                 int n, const LedgerOptions &opt)
{
    TierSnapshot s;
    double effA = 0.0, effF = 0.0;
    const int lim = std::clamp(n, 0, int(run.size()));
    for (int i = 0; i < lim; ++i) {
        if (run[size_t(i)] == ShotState::NotAssessable) continue;   // rule 1
        const double w = i < int(weights.size()) ? weights[size_t(i)] : 1.0;
        ++s.assessable;
        effA += w;
        if (run[size_t(i)] == ShotState::Fired) { ++s.fired; effF += w; }
    }
    s.wilsonLower = wilsonLowerBound(effF, effA, opt.wilsonZ);
    s.tier = conditionTier(s.assessable, s.fired, s.wilsonLower, opt);
    return s;
}

// Every condition's ledger, in FIRST-SEEN order across the session.
//
// First-seen and not alphabetical, and not by rank: this vector is the stable identity of
// the session's condition set, and the panel's ordering is a separate decision applied on
// top of it by hystereticOrder(). Two functions, because "which conditions exist" and
// "which one goes first" churn on completely different schedules (B8).
//
// TREND IS MEASURED ON |z|, NOT z. The sign of z is which side of a two-sided corridor the
// swing fell, and a golfer oscillating from 3 sigma left to 3 sigma right is not improving
// — the magnitude is "how far outside", which is the quantity that shrinking means better.
// Doing it in z-space (rather than raw units) is what lets a mixed-club session pool: the
// corridor is already per-context, so z is context-invariant by construction.
inline std::vector<ConditionLedger> conditionLedgers(const std::vector<ShotRecord> &shots,
                                                     const LedgerOptions &opt = LedgerOptions())
{
    const int n = int(shots.size());

    std::vector<double> weights(size_t(std::max(n, 0)), 1.0);
    for (int i = 0; i < n; ++i) weights[size_t(i)] = shotWeight(shots[size_t(i)], i, opt);

    // First-seen order, with a hash purely for lookup — never iterated, because QHash's
    // iteration order is randomised in Qt6 and a panel that reorders per launch is a bug.
    std::vector<QString> order;
    QHash<QString, int>  index;
    for (const ShotRecord &s : shots) {
        for (const ConditionRow &r : s.rows) {
            if (r.conditionId.isEmpty() || index.contains(r.conditionId)) continue;
            index.insert(r.conditionId, int(order.size()));
            order.push_back(r.conditionId);
        }
    }

    std::vector<ConditionLedger> out;
    out.reserve(order.size());

    for (const QString &id : order) {
        ConditionLedger L;
        L.id = id;
        L.run.assign(size_t(std::max(n, 0)), ShotState::NotAssessable);
        L.zRun.assign(size_t(std::max(n, 0)), std::numeric_limits<double>::quiet_NaN());

        int signedFirings = 0, positives = 0, negatives = 0;
        std::vector<double> absZ, ordinal;

        for (int i = 0; i < n; ++i) {
            const ConditionRow *r = rowFor(shots[size_t(i)], id);
            if (!r) continue;                       // no row at all is NotAssessable
            L.run[size_t(i)] = r->state;
            if (L.drivingMeasureId.isEmpty()) L.drivingMeasureId = r->drivingMeasureId;
            if (r->state == ShotState::NotAssessable) continue;   // rule 1

            if (std::isfinite(r->z)) {
                L.zRun[size_t(i)] = r->z;
                ordinal.push_back(double(absZ.size()));
                absZ.push_back(std::fabs(r->z));
            }
            if (r->state != ShotState::Fired) continue;
            if (r->direction > 0)      { ++positives; ++signedFirings; }
            else if (r->direction < 0) { ++negatives; ++signedFirings; }
        }

        const TierSnapshot now = tierAtPrefix(L.run, weights, n, opt);
        L.assessable  = now.assessable;
        L.fired       = now.fired;
        L.wilsonLower = now.wilsonLower;
        L.tier        = now.tier;
        for (int i = 0; i < n; ++i) {
            if (L.run[size_t(i)] == ShotState::NotAssessable) continue;
            L.effAssessable += weights[size_t(i)];
            if (L.run[size_t(i)] == ShotState::Fired) L.effFired += weights[size_t(i)];
        }
        L.recurrence = recurrenceText(L.fired, L.assessable);
        L.latest = n > 0 ? L.run[size_t(n - 1)] : ShotState::NotAssessable;

        // Assessable shots STRICTLY AFTER the last firing. Assessable, because a shot the
        // capture missed is not evidence the fault has gone.
        L.sinceLastFiring = -1;
        for (int i = n - 1; i >= 0; --i) {
            if (L.run[size_t(i)] != ShotState::Fired) continue;
            int since = 0;
            for (int j = i + 1; j < n; ++j)
                if (L.run[size_t(j)] != ShotState::NotAssessable) ++since;
            L.sinceLastFiring = since;
            break;
        }

        // The NEW badge: Pattern now, and NOT Pattern as of opt.freshWindow shots ago.
        // The comparison is against the ledger's own past, not against a remembered flag,
        // so replay and live agree by construction.
        const TierSnapshot then =
            tierAtPrefix(L.run, weights, std::max(0, n - opt.freshWindow), opt);
        L.freshThisShot = L.tier == Tier::Pattern && then.tier != Tier::Pattern;

        L.resolving = L.tier == Tier::Pattern && L.sinceLastFiring >= opt.resolvingWindow;

        L.rangeRestricted = L.assessable >= opt.minAssessableForPattern
                         && L.fired == L.assessable;

        if (signedFirings > 0) {
            L.directionAgreement = double(std::max(positives, negatives)) / double(signedFirings);
            L.modalDirection = positives > negatives ? 1 : negatives > positives ? -1 : 0;
        } else {
            // Nothing signed to disagree about. 1.0 rather than 0.0 so an unsigned
            // condition is not silently reported as dispersion it never showed.
            L.directionAgreement = 1.0;
            L.modalDirection = 0;
        }
        L.directionClaimed = L.directionAgreement >= opt.directionAgreementGate;

        L.trendPoints = int(absZ.size());
        if (L.trendPoints >= opt.minPointsForTrend) {
            L.trendSlope = theilSenSlope(ordinal, absZ);
            L.trendTau   = kendallTau(ordinal, absZ);
            L.trendP     = kendallTauPValue(L.trendTau, L.trendPoints);
            L.trendKnown = L.trendP <= opt.trendAlpha && L.trendSlope != 0.0;
            if (L.trendKnown)
                L.trend = L.trendSlope < 0.0 ? Trend::Improving : Trend::Worsening;
        }

        out.push_back(std::move(L));
    }
    return out;
}

// ── Coverage (brief §5 honesty devices) ─────────────────────────────────────────

// How few characteristics today's capture can actually speak to. `packConditions` is the
// pack's total and is supplied by the caller — this file has no business loading a pack.
struct Coverage {
    int measurable = 0;   // conditions with at least one assessable shot this session
    int total      = 0;   // conditions in the pack
};

inline Coverage sessionCoverage(const std::vector<ConditionLedger> &ledgers, int packConditions)
{
    Coverage c;
    c.total = std::max(packConditions, 0);
    for (const ConditionLedger &l : ledgers)
        if (l.assessable >= 1) ++c.measurable;
    return c;
}

// ── The authored graph (rule 3) ─────────────────────────────────────────────────

// A node in the pack's DAG, as far as this file needs to know it. Pack-agnostic on
// purpose: the caller marshals whatever its registry holds into these five facts, and no
// pack format reaches the arithmetic.
struct NodeSpec {
    QString id;
    bool    measurable    = false;  // a live measure exists and the capture can take it
    bool    screened      = false;  // a screened root — does not vary shot to shot
    bool    screenEntered = false;  // the physical screen has been done
    bool    asserted      = false;  // the pack asserts it without a measure
    QString outcomeId;              // non-empty ⇒ an outcome node (the declared miss)
    int     phaseOrder    = 0;      // P1…P8 ordinal, for temporal coherence
};

// An authored edge. `type` carries the edge's directional semantics (+1 same sense, −1
// inverse, 0 unsigned) and `strength` the pack's own prior weight, used only to break
// ties deterministically — never to grade.
struct EdgeSpec {
    QString from, to;
    int type     = 0;
    int strength = 0;
};

// The focus contract's split. Without one, Moved-together is not on offer at all — the
// grade is a quasi-experiment and a quasi-experiment needs a declared intervention.
struct FocusSplit {
    bool declared   = false;
    int  baselineEnd = -1;   // index of the LAST baseline shot; the split is after it
};

// Ascending, and the ordering is load-bearing — the panel's stroke, the stage gate and
// the rival-parent choice all compare grades with >=.
enum class LinkGrade { Unanchored, PresentTogether, Coherent, ConditionallyDependent, MovedTogether };

// One graded link. DATA ONLY — no prose. The panel writes "no variance to test" and
// "Fisher exact · 12 paired shots"; this file supplies the facts those sentences quote,
// because a reduction that emits English is a reduction that has to be re-translated.
struct LinkEvidence {
    QString   from, to;
    LinkGrade grade = LinkGrade::Unanchored;

    bool rangeRestricted = false;   // capped at Coherent: upstream fires on every shot
    bool screenOutstanding = false; // an unentered screened root anchors nothing

    int    pairs = 0;               // shots on which BOTH endpoints were assessable
    int    a = 0, b = 0, c = 0, d = 0;
    bool   fisherTested = false;    // false ⇒ refused for too few pairs
    double fisherP = 1.0;
    double tau     = 0.0;           // paired z's, graded support only

    bool   coherent = false;        // direction + temporal order agree with the edge
    int    baselineN = 0, interventionN = 0;
    bool   movedTogether = false;

    int    strength = 0;            // the authored prior, carried for tie-breaks
};

// Is this endpoint PRESENT at all — the floor every higher grade builds on.
//
// A live condition is present when it reaches pattern tier. A screened root is present
// once the SCREEN HAS BEEN DONE and never before — that is the entire epistemics of a
// screened root: it does not vary shot to shot, so no number of swings confirms it and one
// thirty-second test does. An unmeasurable node is present only when the pack ASSERTS it;
// a merely-planned ghost ("measure planned, not yet measurable") asserts nothing, so a link
// through it stays Unanchored rather than borrowing standing from a node nobody claimed.
// That is the difference between drawing an honest gap and bridging one.
inline bool nodePresent(const NodeSpec &node, const ConditionLedger *ledger)
{
    if (!node.outcomeId.isEmpty()) return true;              // a declared miss is observed
    if (node.screened) return node.screenEntered;
    if (ledger && ledger->tier == Tier::Pattern) return true;
    if (!node.measurable && node.asserted) return true;
    return false;
}

// Grade one authored edge on this session's evidence.
//
// The grade is the STRONGEST CLAIM THE EVIDENCE SUPPORTS and never more, so the function
// reads as a ladder with two hard caps on it.
//
// TEMPORAL COHERENCE IS APPROXIMATED BY PHASE ORDER. The design wants "nodes fire in the
// swing's own P1–P8 temporal order within shots". The detector reports one state per
// condition per SHOT, not a moment within the shot, so there is no within-shot timestamp
// to compare. What there is, is the near-total ordering of condition groups by phase —
// takeaway before top before transition before impact — carried on NodeSpec::phaseOrder.
// An edge whose upstream sits at or before its downstream in that ordering is temporally
// coherent; one running backwards through the swing is not. This is an approximation and
// it is stated as one: it will become exact the day the detector timestamps its findings.
//
// The two caps:
//   · An unentered SCREENED ROOT at either end pins the link at Unanchored. A screened
//     root is confirmed by doing the screen once, and until that happens the chain it
//     would anchor is not anchored (brief §5.4). No amount of downstream co-firing
//     substitutes.
//   · RANGE RESTRICTION caps at Coherent. An upstream firing on every measurable shot has
//     no variance to covary with; a 2×2 with an empty row is not evidence of coupling, it
//     is evidence of a constant. This resolves across sessions as the fault improves,
//     which is exactly the right time for it to resolve.
inline LinkEvidence gradeLink(const EdgeSpec &edge,
                              const NodeSpec &from, const NodeSpec &to,
                              const ConditionLedger *fromLedger,
                              const ConditionLedger *toLedger,
                              const FocusSplit &focus = FocusSplit(),
                              const LedgerOptions &opt = LedgerOptions())
{
    LinkEvidence ev;
    ev.from = edge.from;
    ev.to   = edge.to;
    ev.strength = edge.strength;

    // Cap one, applied before anything is counted.
    const bool screenOut = (from.screened && !from.screenEntered)
                        || (to.screened   && !to.screenEntered);
    ev.screenOutstanding = screenOut;
    if (screenOut) return ev;                                  // Unanchored, full stop

    if (!nodePresent(from, fromLedger) || !nodePresent(to, toLedger))
        return ev;                                             // Unanchored
    ev.grade = LinkGrade::PresentTogether;

    // Directional coherence. An UNSIGNED edge (type 0) makes no directional claim, so
    // coherence rests on temporal order alone; a signed edge needs both endpoints to be
    // claiming a direction at all (agreement past the gate) and their modal senses to
    // match the edge's sign.
    bool dirOk = true;
    if (edge.type != 0 && fromLedger && toLedger
        && fromLedger->modalDirection != 0 && toLedger->modalDirection != 0) {
        dirOk = fromLedger->directionClaimed && toLedger->directionClaimed
             && (fromLedger->modalDirection * toLedger->modalDirection)
                == (edge.type > 0 ? 1 : -1);
    }
    const bool temporalOk = from.phaseOrder <= to.phaseOrder;

    // Coherence is a claim about OBSERVED firings — directions that matched, in the
    // swing's own order. Two nodes neither of which the capture measured cannot have been
    // observed doing anything, so a link between them stops at Present together however
    // impeccable the authored semantics are. This is the mock's "neither measured".
    const bool observedBoth = fromLedger && toLedger
                           && fromLedger->assessable >= 1 && toLedger->assessable >= 1;

    ev.coherent = observedBoth && dirOk && temporalOk;
    if (!ev.coherent) return ev;                               // PresentTogether
    ev.grade = LinkGrade::Coherent;

    // Cap two.
    ev.rangeRestricted = fromLedger && fromLedger->rangeRestricted;

    // The paired 2×2, over shots on which BOTH endpoints were assessable (rule 1 again —
    // a pair where either end was occluded is not a pair).
    if (fromLedger && toLedger) {
        const size_t n = std::min(fromLedger->run.size(), toLedger->run.size());
        for (size_t i = 0; i < n; ++i) {
            const ShotState u = fromLedger->run[i], v = toLedger->run[i];
            if (u == ShotState::NotAssessable || v == ShotState::NotAssessable) continue;
            ++ev.pairs;
            const bool uf = u == ShotState::Fired, vf = v == ShotState::Fired;
            if (uf && vf) ++ev.a; else if (uf) ++ev.b; else if (vf) ++ev.c; else ++ev.d;
        }
        std::vector<double> zu, zv;
        const size_t m = std::min(fromLedger->zRun.size(), toLedger->zRun.size());
        for (size_t i = 0; i < m; ++i) {
            if (!std::isfinite(fromLedger->zRun[i]) || !std::isfinite(toLedger->zRun[i])) continue;
            zu.push_back(std::fabs(fromLedger->zRun[i]));
            zv.push_back(std::fabs(toLedger->zRun[i]));
        }
        ev.tau = kendallTau(zu, zv);
    }

    if (ev.rangeRestricted) return ev;                         // Coherent, capped

    if (ev.pairs >= opt.minPairsForDependence) {
        ev.fisherTested = true;
        ev.fisherP = fisherExactOneSided(ev.a, ev.b, ev.c, ev.d);
        if (ev.fisherP <= opt.fisherAlpha) ev.grade = LinkGrade::ConditionallyDependent;
    }

    // Moved together — the session's n-of-1 quasi-experiment, and the only grade that
    // needs a DECLARATION rather than a statistic. Without a focus contract there is no
    // intervention, only the passage of time, and the passage of time is not a treatment.
    //
    // IT BUILDS ON COHERENT, NOT ON CONDITIONALLY DEPENDENT, and that ordering is a
    // decision rather than an oversight. The two grades answer different questions —
    // "do these covary across the session" and "did both improve across a declared
    // change" — and a pair can pass the second while failing the first, because a fault
    // that fires on almost every baseline shot and almost none afterwards has very
    // little within-window variance for a 2×2 to find. Requiring dependence first would
    // discard the strongest evidence in the session for want of the weaker.
    //
    // "In the edge's predicted direction" is read in QUALITY terms: the authored edge says
    // fixing upstream carries downstream with it, so the prediction is that BOTH |z| means
    // fall across the split. The edge's SIGN describes the two measures' senses, which |z|
    // has already normalised away — so the sign has no further work to do here.
    if (!focus.declared || !fromLedger || !toLedger) return ev;

    auto windowMeanAbsZ = [&](const ConditionLedger *L, bool before, int &count) {
        std::vector<double> picked;
        const int n = int(L->zRun.size());
        if (before) {
            for (int i = std::min(focus.baselineEnd, n - 1); i >= 0; --i) {
                if (!std::isfinite(L->zRun[size_t(i)])) continue;
                picked.push_back(std::fabs(L->zRun[size_t(i)]));
                if (int(picked.size()) >= opt.movedTogetherWindow) break;
            }
        } else {
            for (int i = focus.baselineEnd + 1; i < n; ++i) {
                if (!std::isfinite(L->zRun[size_t(i)])) continue;
                picked.push_back(std::fabs(L->zRun[size_t(i)]));
                if (int(picked.size()) >= opt.movedTogetherWindow) break;
            }
        }
        count = int(picked.size());
        if (picked.empty()) return std::numeric_limits<double>::quiet_NaN();
        double s = 0.0;
        for (double v : picked) s += v;
        return s / double(picked.size());
    };

    int ub = 0, ui = 0, db = 0, di = 0;
    const double upBase   = windowMeanAbsZ(fromLedger, true,  ub);
    const double upInt    = windowMeanAbsZ(fromLedger, false, ui);
    const double downBase = windowMeanAbsZ(toLedger,   true,  db);
    const double downInt  = windowMeanAbsZ(toLedger,   false, di);
    ev.baselineN     = std::min(ub, db);
    ev.interventionN = std::min(ui, di);

    if (ev.baselineN >= opt.movedTogetherWindow && ev.interventionN >= opt.movedTogetherWindow
        && std::isfinite(upBase) && std::isfinite(upInt)
        && std::isfinite(downBase) && std::isfinite(downInt)
        && upInt < upBase && downInt < downBase) {
        ev.movedTogether = true;
        ev.grade = LinkGrade::MovedTogether;
    }
    return ev;
}

// The node for `id`, or nullptr.
inline const NodeSpec *nodeFor(const std::vector<NodeSpec> &nodes, const QString &id)
{
    for (const NodeSpec &n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

// Grade every authored edge whose two endpoints the caller described. An edge naming a
// node the caller did not marshal is SKIPPED, not invented (rule 3 in the other
// direction — an edge we cannot describe is not an edge we may draw).
inline std::vector<LinkEvidence> gradeLinks(const std::vector<NodeSpec> &nodes,
                                            const std::vector<EdgeSpec> &edges,
                                            const std::vector<ConditionLedger> &ledgers,
                                            const FocusSplit &focus = FocusSplit(),
                                            const LedgerOptions &opt = LedgerOptions())
{
    std::vector<LinkEvidence> out;
    out.reserve(edges.size());
    for (const EdgeSpec &e : edges) {
        const NodeSpec *f = nodeFor(nodes, e.from);
        const NodeSpec *t = nodeFor(nodes, e.to);
        if (!f || !t) continue;
        out.push_back(gradeLink(e, *f, *t, ledgerFor(ledgers, e.from),
                                ledgerFor(ledgers, e.to), focus, opt));
    }
    return out;
}

// The graded link from → to, or nullptr.
inline const LinkEvidence *linkFor(const std::vector<LinkEvidence> &links,
                                   const QString &from, const QString &to)
{
    for (const LinkEvidence &l : links)
        if (l.from == from && l.to == to) return &l;
    return nullptr;
}

// ── The chain rail ──────────────────────────────────────────────────────────────

// How a node is drawn, which is entirely a statement about what is known about it.
enum class ChainNodeKind {
    LiveCard,      // a measured condition at pattern tier
    Ghost,         // no measure: planned, or authored as asserted
    ScreenedRoot,  // does not vary shot to shot; needs a physical screen
    Outcome        // the declared miss at the end of the chain
};

struct ChainNode {
    QString       id;
    ChainNodeKind kind    = ChainNodeKind::Ghost;
    bool          hasLedger = false;
    bool          hasLink = false;   // a graded link to the NEXT node
    LinkEvidence  link;
};

struct Chain {
    std::vector<ChainNode> nodes;
    bool anyLive = false;            // at least one pattern-tier measured node
};

// Two authored parents could explain a fired node and n cannot adjudicate between them.
// The panel names the rival and says so — it never asserts uniqueness (design §A4).
struct RivalParent {
    QString childId;
    QString chosenParentId;
    QString rivalParentId;
    bool    adjudicated = false;     // false ⇒ the rival is unmeasurable or under-paired
};

struct ChainRails {
    std::vector<Chain>       chains;
    std::vector<QString>     unchainedPatterns;   // pattern tier, no authored peer edge
    std::vector<RivalParent> rivals;
    std::vector<LinkEvidence> links;              // every graded edge, as supplied
};

inline ChainNodeKind chainNodeKind(const NodeSpec &n, const ConditionLedger *l)
{
    if (!n.outcomeId.isEmpty()) return ChainNodeKind::Outcome;
    if (n.screened)             return ChainNodeKind::ScreenedRoot;
    if (!n.measurable)          return ChainNodeKind::Ghost;
    return (l && l->tier == Tier::Pattern) ? ChainNodeKind::LiveCard : ChainNodeKind::Ghost;
}

// Pull this session's rails out of the authored graph.
//
// PACK-AGNOSTIC BY CONSTRUCTION: the only inputs are the node/edge descriptions the caller
// marshalled and the ledgers. Nothing here knows what a golf swing is.
//
// Which nodes participate. A measured condition participates only at PATTERN TIER —
// otherwise a single shank would drag a whole chain onto the screen. An unmeasurable node
// (ghost or screened root) and an outcome node always participate, because they are the
// honesty devices: a chain that quietly omitted its unmeasurable middle would be claiming
// a continuity it does not have.
//
// Which paths become chains. Sources of the induced subgraph, walked forward, taking the
// best-graded outgoing edge at each branch (ties on the authored strength, then on id, so
// the walk is deterministic and the rail does not reshuffle between runs — B8). A chain
// needs two nodes and at least one live pattern in it; paths that are contiguous
// sub-paths of a longer chain are dropped so the rail is not drawn twice.
inline ChainRails extractChains(const std::vector<NodeSpec> &nodes,
                                const std::vector<EdgeSpec> &edges,
                                const std::vector<ConditionLedger> &ledgers,
                                const std::vector<LinkEvidence> &links,
                                const LedgerOptions &opt = LedgerOptions())
{
    ChainRails rails;
    rails.links = links;

    auto participates = [&](const NodeSpec &n) {
        if (!n.outcomeId.isEmpty()) return true;
        if (n.screened || !n.measurable) return true;
        const ConditionLedger *l = ledgerFor(ledgers, n.id);
        return l && l->tier == Tier::Pattern;
    };

    std::vector<EdgeSpec> kept;
    kept.reserve(edges.size());
    for (const EdgeSpec &e : edges) {
        const NodeSpec *f = nodeFor(nodes, e.from);
        const NodeSpec *t = nodeFor(nodes, e.to);
        if (!f || !t) continue;
        if (!participates(*f) || !participates(*t)) continue;
        kept.push_back(e);
    }

    // Deterministic branch choice: grade, then authored strength, then id.
    auto better = [&](const EdgeSpec &x, const EdgeSpec &y) {
        const LinkEvidence *lx = linkFor(links, x.from, x.to);
        const LinkEvidence *ly = linkFor(links, y.from, y.to);
        const int gx = lx ? int(lx->grade) : 0;
        const int gy = ly ? int(ly->grade) : 0;
        if (gx != gy) return gx > gy;
        if (x.strength != y.strength) return x.strength > y.strength;
        return x.to < y.to;
    };

    // Sources: participating nodes with no incoming kept edge.
    std::vector<QString> sources;
    for (const NodeSpec &n : nodes) {
        if (!participates(n)) continue;
        bool incoming = false;
        for (const EdgeSpec &e : kept) if (e.to == n.id) { incoming = true; break; }
        if (!incoming) sources.push_back(n.id);
    }

    std::vector<std::vector<QString>> paths;
    for (const QString &src : sources) {
        std::vector<QString> path{ src };
        QHash<QString, bool> seen;
        seen.insert(src, true);
        for (;;) {
            const QString cur = path.back();
            const EdgeSpec *best = nullptr;
            for (const EdgeSpec &e : kept) {
                if (e.from != cur || seen.contains(e.to)) continue;   // no cycles, ever
                if (!best || better(e, *best)) best = &e;
            }
            if (!best) break;
            seen.insert(best->to, true);
            path.push_back(best->to);
        }
        if (path.size() >= 2) paths.push_back(std::move(path));
    }

    // Drop contiguous sub-paths of a longer path.
    auto isSubPath = [](const std::vector<QString> &small, const std::vector<QString> &big) {
        if (small.size() >= big.size()) return false;
        for (size_t off = 0; off + small.size() <= big.size(); ++off) {
            bool all = true;
            for (size_t i = 0; i < small.size(); ++i)
                if (small[i] != big[off + i]) { all = false; break; }
            if (all) return true;
        }
        return false;
    };

    for (size_t i = 0; i < paths.size(); ++i) {
        bool covered = false;
        for (size_t j = 0; j < paths.size() && !covered; ++j)
            if (i != j && isSubPath(paths[i], paths[j])) covered = true;
        if (covered) continue;

        Chain chain;
        for (size_t k = 0; k < paths[i].size(); ++k) {
            const QString &id = paths[i][k];
            const NodeSpec *n = nodeFor(nodes, id);
            const ConditionLedger *l = ledgerFor(ledgers, id);
            ChainNode cn;
            cn.id = id;
            cn.kind = n ? chainNodeKind(*n, l) : ChainNodeKind::Ghost;
            cn.hasLedger = l != nullptr;
            if (cn.kind == ChainNodeKind::LiveCard) chain.anyLive = true;
            if (k + 1 < paths[i].size()) {
                if (const LinkEvidence *le = linkFor(links, id, paths[i][k + 1])) {
                    cn.hasLink = true;
                    cn.link = *le;
                }
            }
            chain.nodes.push_back(std::move(cn));
        }
        if (chain.anyLive) rails.chains.push_back(std::move(chain));
    }

    // Unchained patterns. NOT "patterns that missed the rail" — the test is the authored
    // graph itself: a pattern with no authored edge, in either direction, to any OTHER
    // pattern-tier condition this session. It gets its own line and is never dropped
    // (brief §5.3), and it is never forced onto a chain to make the picture tidier.
    for (const ConditionLedger &l : ledgers) {
        if (l.tier != Tier::Pattern) continue;
        bool joined = false;
        for (const EdgeSpec &e : edges) {
            const QString other = e.from == l.id ? e.to : e.to == l.id ? e.from : QString();
            if (other.isEmpty()) continue;
            const ConditionLedger *o = ledgerFor(ledgers, other);
            if (o && o->id != l.id && o->tier == Tier::Pattern) { joined = true; break; }
        }
        if (!joined) rails.unchainedPatterns.push_back(l.id);
    }

    // Rival parents: a FIRED node with two or more authored parents that participate.
    for (const NodeSpec &n : nodes) {
        const ConditionLedger *child = ledgerFor(ledgers, n.id);
        if (!child || child->fired < 1) continue;

        std::vector<const EdgeSpec *> parents;
        for (const EdgeSpec &e : kept) if (e.to == n.id) parents.push_back(&e);
        if (parents.size() < 2) continue;

        std::stable_sort(parents.begin(), parents.end(),
                         [&](const EdgeSpec *x, const EdgeSpec *y) {
                             const LinkEvidence *lx = linkFor(links, x->from, x->to);
                             const LinkEvidence *ly = linkFor(links, y->from, y->to);
                             const int gx = lx ? int(lx->grade) : 0;
                             const int gy = ly ? int(ly->grade) : 0;
                             if (gx != gy) return gx > gy;
                             if (x->strength != y->strength) return x->strength > y->strength;
                             return x->from < y->from;
                         });

        RivalParent rp;
        rp.childId        = n.id;
        rp.chosenParentId = parents[0]->from;
        rp.rivalParentId  = parents[1]->from;
        const NodeSpec *rival = nodeFor(nodes, rp.rivalParentId);
        const LinkEvidence *rl = linkFor(links, rp.rivalParentId, n.id);
        rp.adjudicated = rival && rival->measurable
                      && rl && rl->pairs >= opt.minPairsForDependence;
        rails.rivals.push_back(rp);
    }
    return rails;
}

// ── The stage machine (design §B1) ──────────────────────────────────────────────

enum class Stage { Cold, Forming, Established, Closing };

// The stage this session has reached, RATCHETED against the stage it had already reached.
//
// The ratchet is itself an anti-churn rule (B8): a session that has shown the chain rail
// does not drop back to a flat card list because one shot was occluded. Established is
// entered once and kept. Closing is not derived at all — it is set by the caller when the
// session ends, and it is the one transition that is an event rather than a threshold.
//
// ESTABLISHED FOLLOWS THE DESIGN DOCUMENT, NOT THE MOCK. B1 requires "≥ 2 linked patterns
// with links ≥ Coherent" — the LINK GRADE is the gate, not the mere existence of an
// authored edge, and not (as the mock hard-codes) the presence of a named condition. Two
// patterns the pack joins but whose directions run backwards through the swing have not
// established anything, and the rail should not be drawn for them.
inline Stage sessionStage(Stage previous,
                          const std::vector<ConditionLedger> &ledgers,
                          const std::vector<LinkEvidence> &links,
                          bool sessionClosed,
                          const LedgerOptions &opt = LedgerOptions())
{
    if (sessionClosed) return Stage::Closing;

    int maxAssessable = 0;
    for (const ConditionLedger &l : ledgers) maxAssessable = std::max(maxAssessable, l.assessable);

    Stage derived = Stage::Cold;
    if (maxAssessable >= opt.minAssessableForPattern) derived = Stage::Forming;

    if (derived == Stage::Forming) {
        // Count the DISTINCT pattern-tier conditions standing at either end of a link
        // graded Coherent or better. Two of them is a chain worth drawing.
        std::vector<QString> linked;
        auto note = [&](const QString &id) {
            const ConditionLedger *l = ledgerFor(ledgers, id);
            if (!l || l->tier != Tier::Pattern) return;
            if (std::find(linked.begin(), linked.end(), id) == linked.end()) linked.push_back(id);
        };
        for (const LinkEvidence &e : links) {
            if (e.grade < LinkGrade::Coherent) continue;
            note(e.from);
            note(e.to);
        }
        if (linked.size() >= 2) derived = Stage::Established;
    }

    if (previous == Stage::Closing) return Stage::Closing;      // never un-closes
    return std::max(previous, derived);
}

// ── Anti-churn (design §B8) ─────────────────────────────────────────────────────

struct RankedCondition {
    QString id;
    double  score = 0.0;   // whatever the caller ranks on — severity, coverage, prevalence
};

// Display order with RANK-BAND HYSTERESIS.
//
// API SHAPE, because it is a decision: this takes the fresh scores AND the order currently
// on screen, and returns the order that should be on screen next. It does not take a
// mutable state object and it holds nothing between calls — the previous order IS the
// state, the caller already has it (it is drawing it), and a pure function of the two is
// testable in a way a stateful sorter is not.
//
// The rule: an item whose fresh rank differs from its displayed rank by MORE than
// `bandWidth` moves; anything else keeps its displayed position. Ties among movers break
// on the fresh rank, so a decisive change lands where it belongs rather than at the end.
// Items not previously on screen enter at their fresh rank.
//
// This is also where a fault profile would legitimately bias presentation — by feeding
// `score`. It cannot reach a tier from here, which is rule 5 of the brief made structural.
inline std::vector<QString> hystereticOrder(const std::vector<RankedCondition> &scored,
                                            const std::vector<QString> &previousOrder,
                                            int bandWidth = kDiagRankBandWidth)
{
    std::vector<RankedCondition> fresh = scored;
    std::stable_sort(fresh.begin(), fresh.end(),
                     [](const RankedCondition &x, const RankedCondition &y) {
                         if (x.score != y.score) return x.score > y.score;
                         return x.id < y.id;
                     });

    struct Placed { QString id; int key; int freshRank; };
    std::vector<Placed> placed;
    placed.reserve(fresh.size());
    for (int i = 0; i < int(fresh.size()); ++i) {
        int prevRank = -1;
        for (int j = 0; j < int(previousOrder.size()); ++j)
            if (previousOrder[size_t(j)] == fresh[size_t(i)].id) { prevRank = j; break; }
        const bool sticks = prevRank >= 0 && std::abs(i - prevRank) <= bandWidth;
        placed.push_back({ fresh[size_t(i)].id, sticks ? prevRank : i, i });
    }
    std::stable_sort(placed.begin(), placed.end(), [](const Placed &x, const Placed &y) {
        if (x.key != y.key) return x.key < y.key;
        if (x.freshRank != y.freshRank) return x.freshRank < y.freshRank;
        return x.id < y.id;
    });

    std::vector<QString> out;
    out.reserve(placed.size());
    for (const Placed &s : placed) out.push_back(s.id);
    return out;
}

// The pattern set as of the first `n` shots, sorted so two sets compare by value.
inline std::vector<QString> patternSetAt(const std::vector<ShotRecord> &shots, int n,
                                         const LedgerOptions &opt = LedgerOptions())
{
    const int lim = std::clamp(n, 0, int(shots.size()));
    const std::vector<ShotRecord> prefix(shots.begin(), shots.begin() + lim);
    std::vector<QString> ids;
    for (const ConditionLedger &l : conditionLedgers(prefix, opt))
        if (l.tier == Tier::Pattern) ids.push_back(l.id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// How many trailing shots the pattern set has been unchanged for — 1 means "since the
// last shot only". The driver footer's debounce input (§B2 zone 3): better absent than
// flickering, so the footer waits for the set to hold still.
inline int patternSetStableShots(const std::vector<ShotRecord> &shots,
                                 const LedgerOptions &opt = LedgerOptions())
{
    const int n = int(shots.size());
    if (n <= 0) return 0;
    const std::vector<QString> now = patternSetAt(shots, n, opt);
    int stable = 1;
    for (int k = n - 1; k >= 1; --k) {
        if (patternSetAt(shots, k, opt) != now) break;
        ++stable;
    }
    return stable;
}

inline bool driverFooterEligible(const std::vector<ShotRecord> &shots,
                                 const LedgerOptions &opt = LedgerOptions())
{
    return patternSetStableShots(shots, opt) >= opt.driverDebounceShots;
}

// ── Session bookends (design §B7) ───────────────────────────────────────────────

// The three shots the closing panel links a replay to, for one condition.
//
// PRINCIPLED SELECTION, not "first and last": max and min |z| on the driving measure among
// ASSESSABLE shots, plus the shot whose z sits closest to the session median z. The
// median one is the point — a golfer shown their best and worst swing learns what their
// range is; shown their most representative one, they learn what their swing IS.
struct Bookends {
    bool   has = false;
    int    worstShotId = -1;          double worstZ = 0.0;   // max |z|
    int    bestShotId  = -1;          double bestZ  = 0.0;   // min |z|
    int    representativeShotId = -1; double medianZ = 0.0;  // closest to the median z
};

inline Bookends conditionBookends(const std::vector<ShotRecord> &shots, const QString &conditionId)
{
    Bookends bk;
    std::vector<double> zs;
    std::vector<int>    ids;
    for (const ShotRecord &s : shots) {
        const ConditionRow *r = rowFor(s, conditionId);
        if (!r || r->state == ShotState::NotAssessable || !std::isfinite(r->z)) continue;
        zs.push_back(r->z);
        ids.push_back(s.shotId);
    }
    if (zs.empty()) return bk;

    int worst = 0, best = 0;
    for (size_t i = 1; i < zs.size(); ++i) {
        if (std::fabs(zs[i]) > std::fabs(zs[size_t(worst)])) worst = int(i);
        if (std::fabs(zs[i]) < std::fabs(zs[size_t(best)]))  best  = int(i);
    }
    const double med = medianOf(zs);
    int rep = 0;
    for (size_t i = 1; i < zs.size(); ++i)
        if (std::fabs(zs[i] - med) < std::fabs(zs[size_t(rep)] - med)) rep = int(i);

    bk.has = true;
    bk.worstShotId = ids[size_t(worst)]; bk.worstZ = zs[size_t(worst)];
    bk.bestShotId  = ids[size_t(best)];  bk.bestZ  = zs[size_t(best)];
    bk.representativeShotId = ids[size_t(rep)];
    bk.medianZ = med;
    return bk;
}

// ── Serialisation — diagnostics.json ────────────────────────────────────────────
//
// WHAT IS PERSISTED IS THE ROWS, NOT THE REDUCTIONS. Every tier, trend, grade and stage in
// this file is a pure function of the rows and the injected gates, so writing them out
// would be writing a cache that can disagree with the code that produced it. Review mode
// re-reduces and gets the identical panel by construction; a gate that moves in a later
// build re-grades an old session honestly instead of quoting a stale verdict.
//
// The options travel alongside so a session records the gates it was read under, and
// schemaVersion tags the ROW shape.

inline QString shotStateToString(ShotState s)
{
    switch (s) {
    case ShotState::Fired: return QStringLiteral("fired");
    case ShotState::Clean: return QStringLiteral("clean");
    case ShotState::NotAssessable: break;
    }
    return QStringLiteral("notAssessable");
}

inline ShotState shotStateFromString(const QString &s)
{
    if (s == QStringLiteral("fired")) return ShotState::Fired;
    if (s == QStringLiteral("clean")) return ShotState::Clean;
    return ShotState::NotAssessable;      // anything unrecognised is "we did not look"
}

inline QJsonObject toJson(const std::vector<ShotRecord> &shots,
                          const LedgerOptions &opt = LedgerOptions())
{
    QJsonArray shotArr;
    for (const ShotRecord &s : shots) {
        QJsonArray rowArr;
        for (const ConditionRow &r : s.rows) {
            QJsonObject ro;
            ro[QStringLiteral("conditionId")]     = r.conditionId;
            ro[QStringLiteral("state")]           = shotStateToString(r.state);
            ro[QStringLiteral("confidence")]      = double(r.confidence);
            ro[QStringLiteral("direction")]       = r.direction;
            ro[QStringLiteral("drivingMeasureId")]= r.drivingMeasureId;
            ro[QStringLiteral("value")]           = r.value;
            ro[QStringLiteral("corridorLo")]      = r.corridorLo;
            ro[QStringLiteral("corridorHi")]      = r.corridorHi;
            ro[QStringLiteral("z")]               = r.z;
            ro[QStringLiteral("contextId")]       = r.contextId;
            ro[QStringLiteral("contextInferred")] = r.contextInferred;
            ro[QStringLiteral("material")]        = r.material;
            ro[QStringLiteral("notAssessableReason")] = r.notAssessableReason;
            rowArr.append(ro);
        }
        QJsonObject so;
        so[QStringLiteral("shotId")]      = s.shotId;
        so[QStringLiteral("club")]        = s.club;
        so[QStringLiteral("contextId")]   = s.contextId;
        so[QStringLiteral("timestampMs")] = double(s.timestampMs);
        so[QStringLiteral("warmUp")]      = s.warmUp;
        so[QStringLiteral("rows")]        = rowArr;
        shotArr.append(so);
    }

    QJsonObject oo;
    oo[QStringLiteral("patternWilsonLb")]         = opt.patternWilsonLb;
    oo[QStringLiteral("minAssessableForPattern")] = opt.minAssessableForPattern;
    oo[QStringLiteral("directionAgreementGate")]  = opt.directionAgreementGate;
    oo[QStringLiteral("minPointsForTrend")]       = opt.minPointsForTrend;
    oo[QStringLiteral("trendAlpha")]              = opt.trendAlpha;
    oo[QStringLiteral("resolvingWindow")]         = opt.resolvingWindow;
    oo[QStringLiteral("freshWindow")]             = opt.freshWindow;
    oo[QStringLiteral("minPairsForDependence")]   = opt.minPairsForDependence;
    oo[QStringLiteral("fisherAlpha")]             = opt.fisherAlpha;
    oo[QStringLiteral("movedTogetherWindow")]     = opt.movedTogetherWindow;
    oo[QStringLiteral("rankBandWidth")]           = opt.rankBandWidth;
    oo[QStringLiteral("driverDebounceShots")]     = opt.driverDebounceShots;
    oo[QStringLiteral("warmUpShots")]             = opt.warmUpShots;
    oo[QStringLiteral("warmUpWeight")]            = opt.warmUpWeight;
    oo[QStringLiteral("wilsonZ")]                 = opt.wilsonZ;

    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = kDiagSchemaVersion;
    root[QStringLiteral("options")]       = oo;
    root[QStringLiteral("shots")]         = shotArr;
    return root;
}

// Rebuild the rows. `optOut` is filled when supplied; a missing key keeps the default,
// so an older file read by a newer build gets today's gates rather than a zeroed struct.
inline std::vector<ShotRecord> fromJson(const QJsonObject &root, LedgerOptions *optOut = nullptr)
{
    if (optOut) {
        const QJsonObject oo = root.value(QStringLiteral("options")).toObject();
        LedgerOptions o;
        auto d = [&](const char *k, double v) { return oo.value(QLatin1String(k)).toDouble(v); };
        auto i = [&](const char *k, int v)    { return oo.value(QLatin1String(k)).toInt(v); };
        o.patternWilsonLb         = d("patternWilsonLb",         o.patternWilsonLb);
        o.minAssessableForPattern = i("minAssessableForPattern", o.minAssessableForPattern);
        o.directionAgreementGate  = d("directionAgreementGate",  o.directionAgreementGate);
        o.minPointsForTrend       = i("minPointsForTrend",       o.minPointsForTrend);
        o.trendAlpha              = d("trendAlpha",              o.trendAlpha);
        o.resolvingWindow         = i("resolvingWindow",         o.resolvingWindow);
        o.freshWindow             = i("freshWindow",             o.freshWindow);
        o.minPairsForDependence   = i("minPairsForDependence",   o.minPairsForDependence);
        o.fisherAlpha             = d("fisherAlpha",             o.fisherAlpha);
        o.movedTogetherWindow     = i("movedTogetherWindow",     o.movedTogetherWindow);
        o.rankBandWidth           = i("rankBandWidth",           o.rankBandWidth);
        o.driverDebounceShots     = i("driverDebounceShots",     o.driverDebounceShots);
        o.warmUpShots             = i("warmUpShots",             o.warmUpShots);
        o.warmUpWeight            = d("warmUpWeight",            o.warmUpWeight);
        o.wilsonZ                 = d("wilsonZ",                 o.wilsonZ);
        *optOut = o;
    }

    std::vector<ShotRecord> shots;
    const QJsonArray shotArr = root.value(QStringLiteral("shots")).toArray();
    shots.reserve(size_t(shotArr.size()));
    for (const QJsonValue &sv : shotArr) {
        const QJsonObject so = sv.toObject();
        ShotRecord s;
        s.shotId      = so.value(QStringLiteral("shotId")).toInt();
        s.club        = so.value(QStringLiteral("club")).toString();
        s.contextId   = so.value(QStringLiteral("contextId")).toString();
        s.timestampMs = qint64(so.value(QStringLiteral("timestampMs")).toDouble());
        s.warmUp      = so.value(QStringLiteral("warmUp")).toBool();
        for (const QJsonValue &rv : so.value(QStringLiteral("rows")).toArray()) {
            const QJsonObject ro = rv.toObject();
            ConditionRow r;
            r.conditionId      = ro.value(QStringLiteral("conditionId")).toString();
            r.state            = shotStateFromString(ro.value(QStringLiteral("state")).toString());
            r.confidence       = float(ro.value(QStringLiteral("confidence")).toDouble(1.0));
            r.direction        = ro.value(QStringLiteral("direction")).toInt();
            r.drivingMeasureId = ro.value(QStringLiteral("drivingMeasureId")).toString();
            r.value            = ro.value(QStringLiteral("value")).toDouble();
            r.corridorLo       = ro.value(QStringLiteral("corridorLo")).toDouble();
            r.corridorHi       = ro.value(QStringLiteral("corridorHi")).toDouble();
            r.z                = ro.value(QStringLiteral("z")).toDouble();
            r.contextId        = ro.value(QStringLiteral("contextId")).toString();
            r.contextInferred  = ro.value(QStringLiteral("contextInferred")).toBool();
            r.material         = ro.value(QStringLiteral("material")).toBool(true);
            r.notAssessableReason = ro.value(QStringLiteral("notAssessableReason")).toString();
            s.rows.push_back(std::move(r));
        }
        shots.push_back(std::move(s));
    }
    return shots;
}

inline QJsonDocument toJsonDocument(const std::vector<ShotRecord> &shots,
                                    const LedgerOptions &opt = LedgerOptions())
{
    return QJsonDocument(toJson(shots, opt));
}

inline std::vector<ShotRecord> fromJsonDocument(const QJsonDocument &doc,
                                                LedgerOptions *optOut = nullptr)
{
    return fromJson(doc.object(), optOut);
}

} // namespace pinpoint::analysis
