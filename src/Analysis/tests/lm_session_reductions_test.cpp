// Standalone tests for the launch monitor session reductions (src/Analysis/
// lm_session_reductions.h): the n-1 spread, the kMinShotsForSpread floor, the
// omit-rather-than-zero rule, club scoping's effect on a mean, the z-score and the
// tick clamp, and the board's number formatting. Pure — no OpenCV, no fixture. Own
// main()/check() macros. Links launch_monitor_reading.cpp for fieldDefs().
//
//   cmake --build build/analyzer-tests --target lm_session_reductions_test
//   ctest --test-dir build/analyzer-tests -R lm_session_reductions --output-on-failure

#include "../lm_session_reductions.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The stats for one key, BY VALUE — most callers below pass a temporary, and a
// pointer into one dangles the moment the full expression ends. An omitted field
// comes back default-constructed, which is n == 0 and every flag false.
// `def` survives the copy: it points into fieldDefs()' function-local static.
static LmFieldStats stat(const std::vector<LmFieldStats> &v, const char *key)
{
    for (const LmFieldStats &s : v)
        if (s.key == QString::fromLatin1(key)) return s;
    return LmFieldStats{};
}
static bool present(const std::vector<LmFieldStats> &v, const char *key)
{
    return stat(v, key).n > 0;
}

// One shot carrying just the named field.
static LmShotValues one(const char *key, double v)
{
    LmShotValues s;
    s.insert(QString::fromLatin1(key), v);
    return s;
}

static QString k(const char *key) { return QString::fromLatin1(key); }

// One shot's strike, as the ellipse tests need it: location across, height up.
static LmShotValues pair(double loc, double height)
{
    LmShotValues s;
    s.insert(QStringLiteral("lm.strikeLocation"), loc);
    s.insert(QStringLiteral("lm.strikeHeight"), height);
    return s;
}

int main()
{
    std::printf("lm_session_reductions_test\n");

    // ── The field table: every row banded and abbreviated ───────────────────────
    //
    // The panel walks fieldDefs() and trusts these two columns to exist. A 26th field
    // added without them would produce a tile in no band, which the board cannot draw
    // — so it fails here rather than going quietly missing from the screen.
    {
        std::set<QString> groups;
        for (const char *g : pinpoint::lm::fieldGroups())
            groups.insert(QString::fromLatin1(g));
        check(groups.size() == pinpoint::lm::fieldGroups().size(), "band names are unique");

        std::set<QString> used;
        bool allBanded = true, allAbbrev = true;
        for (const pinpoint::lm::FieldDef &f : pinpoint::lm::fieldDefs()) {
            const QString g = QString::fromLatin1(f.group ? f.group : "");
            if (g.isEmpty() || groups.count(g) == 0) allBanded = false;
            if (!f.abbrev || !*f.abbrev)             allAbbrev = false;
            used.insert(g);
        }
        check(allBanded, "every field names a band from fieldGroups()");
        check(allAbbrev, "every field carries a board abbreviation");
        check(used.size() == groups.size(), "every band owns at least one field");

        // The qualifier that disambiguates against our own estimate has no job on a
        // board with no estimate on it — §3.5 of the brief, and the reason abbrev is
        // not MetricDescriptor::shortLabel.
        bool noQualifier = true;
        for (const pinpoint::lm::FieldDef &f : pinpoint::lm::fieldDefs())
            if (QString::fromUtf8(f.abbrev).contains(QStringLiteral("(")))
                noQualifier = false;
        check(noQualifier, "abbreviations drop the (measured)/(LM) qualifier");
    }

    // ── mean and SAMPLE sd, hand-computed ──────────────────────────────────────
    {
        // 80, 84, 88, 92 ⇒ mean 86; deviations −6,−2,2,6 ⇒ ss = 80; n-1 = 3 ⇒ var
        // 26.666…, sd 5.16397779…  (the POPULATION sd would be 4.4721, and picking the
        // wrong one here is the single easiest way to understate a golfer's spread).
        const std::vector<LmShotValues> shots = { one("lm.clubheadSpeed", 80.0),
                                                  one("lm.clubheadSpeed", 84.0),
                                                  one("lm.clubheadSpeed", 88.0),
                                                  one("lm.clubheadSpeed", 92.0) };
        const auto st = lmSessionStats(shots, 0);
        const LmFieldStats s = stat(st, "lm.clubheadSpeed");
        check(present(st, "lm.clubheadSpeed"), "a field every shot carried is present");
        check(s.n == 4, "n counts the shots that carried a value");
        check(near(s.mean, 86.0, 1e-9), "mean is the arithmetic mean");
        check(near(s.sd, 5.163977794943222, 1e-12), "sd uses the n-1 denominator");
        check(!near(s.sd, 4.47213595499958, 1e-6), "…and is NOT the population sd");
        check(s.hasSpread, "n = 4 is above the spread floor");
        check(s.hasLatest && near(s.latest, 80.0, 1e-9), "latest = the focused shot's value");
        check(near(s.z, (80.0 - 86.0) / s.sd, 1e-12), "z = (latest − mean) / sd");
    }

    // ── the spread floor ────────────────────────────────────────────────────────
    {
        const std::vector<LmShotValues> two = { one("lm.spinRate", 4000.0),
                                                one("lm.spinRate", 5000.0) };
        const LmFieldStats s = stat(lmSessionStats(two, 0), "lm.spinRate");
        check(s.n == 2, "two shots still produce a field");
        check(near(s.mean, 4500.0, 1e-9), "…and a mean, which is meaningful at n = 2");
        check(s.sd > 0.0, "…and an sd is computed");
        check(!s.hasSpread, "…but n = 2 is below the floor, so no spread is offered");
        check(near(s.z, 0.0, 1e-12), "z is 0 when there is no spread to divide by");

        const std::vector<LmShotValues> three = { one("lm.spinRate", 4000.0),
                                                  one("lm.spinRate", 5000.0),
                                                  one("lm.spinRate", 4500.0) };
        check(stat(lmSessionStats(three, 0), "lm.spinRate").hasSpread,
              "three shots is exactly the floor, and it passes");
        check(kMinShotsForSpread == 3, "the floor is three");
    }

    // ── sd == 0: identical readings, no divide ─────────────────────────────────
    {
        const std::vector<LmShotValues> same = { one("lm.spinRate", 4686.0),
                                                 one("lm.spinRate", 4686.0),
                                                 one("lm.spinRate", 4686.0),
                                                 one("lm.spinRate", 4686.0) };
        const LmFieldStats s = stat(lmSessionStats(same, 0), "lm.spinRate");
        check(near(s.sd, 0.0, 1e-12), "identical readings ⇒ sd 0");
        check(!s.hasSpread, "…so no spread, whatever n is");
        check(std::isfinite(s.z) && near(s.z, 0.0, 1e-12), "…and z never divides by zero");

        const LmFieldStats o = stat(lmSessionStats({ one("lm.spinRate", 4686.0) }, 0),
                                    "lm.spinRate");
        check(o.n == 1 && near(o.sd, 0.0, 1e-12), "n = 1 ⇒ sd 0 rather than a divide by 0");
        check(o.hasLatest, "…and the single shot is still the latest");
    }

    // ── omitted, not zeroed ────────────────────────────────────────────────────
    {
        // The mock's missing "Distance to pin" tile: a monitor that reports no pin
        // distance must produce NO tile, because a tile reading 0.0 yd would claim it
        // measured the ball finishing at the hole.
        const std::vector<LmShotValues> shots = { one("lm.carryDistance", 166.6),
                                                  one("lm.carryDistance", 171.2),
                                                  one("lm.carryDistance", 160.1) };
        const auto st = lmSessionStats(shots, 0);
        check(present(st, "lm.carryDistance"), "a reported field is present");
        check(!present(st, "lm.distanceToPin"), "a field NO shot carried is omitted");
        check(st.size() == 1, "…and nothing else appears either");
    }

    // ── a field present on SOME shots aggregates over only those ───────────────
    {
        // Three shots; the monitor lost the ball on the middle one, so it has club data
        // and no flight data. Carry's mean must be of two numbers, not of three with a
        // zero in it — and clubhead speed's must still be of all three.
        std::vector<LmShotValues> shots(3);
        shots[0].insert(QStringLiteral("lm.clubheadSpeed"), 84.0);
        shots[0].insert(QStringLiteral("lm.carryDistance"), 160.0);
        shots[1].insert(QStringLiteral("lm.clubheadSpeed"), 86.0);   // no carry
        shots[2].insert(QStringLiteral("lm.clubheadSpeed"), 88.0);
        shots[2].insert(QStringLiteral("lm.carryDistance"), 170.0);

        const auto st = lmSessionStats(shots, 0);
        const LmFieldStats c = stat(st, "lm.carryDistance");
        const LmFieldStats s = stat(st, "lm.clubheadSpeed");
        check(c.n == 2, "carry aggregates over the two shots that carried it");
        check(near(c.mean, 165.0, 1e-9), "…and its mean is not dragged toward 0 by the third");
        check(s.n == 3, "…while the club field keeps all three");
        check(near(s.mean, 86.0, 1e-9), "…and its own mean is untouched");
    }

    // ── the focused shot, and the shot the monitor missed ──────────────────────
    {
        std::vector<LmShotValues> shots(3);
        shots[0].insert(QStringLiteral("lm.ballSpeed"), 110.0);
        shots[1].insert(QStringLiteral("lm.ballSpeed"), 118.0);
        shots[2].insert(QStringLiteral("lm.ballSpeed"), 114.0);

        check(near(stat(lmSessionStats(shots, 1), "lm.ballSpeed").latest, 118.0, 1e-9),
              "latest follows the FOCUSED index, not the newest row");

        // Nothing focused: means and spreads survive, values do not.
        const LmFieldStats none = stat(lmSessionStats(shots, -1), "lm.ballSpeed");
        check(!none.hasLatest, "no focused shot ⇒ no latest");
        check(none.n == 3 && near(none.mean, 114.0, 1e-9),
              "…but the session statistics are unaffected");
        check(near(none.z, 0.0, 1e-12), "…and z is 0 rather than a value minus a mean");
        check(!stat(lmSessionStats(shots, 99), "lm.ballSpeed").hasLatest,
              "an out-of-range focused index is the same as none");

        // Focused on a shot the monitor missed entirely — §5's last row: the board
        // renders, that tile shows an em dash, the mean and spread stay.
        std::vector<LmShotValues> withGap = shots;
        withGap.push_back(LmShotValues{});
        const LmFieldStats gap = stat(lmSessionStats(withGap, 3), "lm.ballSpeed");
        check(!gap.hasLatest, "a focused shot with no reading has no value");
        check(gap.n == 3, "…and does not count toward n");
        check(lmValueText(gap) == QStringLiteral("—"), "…and prints the em dash");
        check(lmMeanText(gap).startsWith(QStringLiteral("μ ")), "…while the mean still prints");
    }

    // ── club scoping is the CALLER's, and this is why it matters ───────────────
    {
        // The reduction aggregates whatever it is handed; the model hands it one club's
        // shots. This pins the cost of getting that wrong: a driver in the middle of an
        // iron session moves the carry mean by 40 yards and the sd by more.
        std::vector<LmShotValues> irons = { one("lm.carryDistance", 160.0),
                                            one("lm.carryDistance", 165.0),
                                            one("lm.carryDistance", 170.0) };
        std::vector<LmShotValues> mixed = irons;
        mixed.push_back(one("lm.carryDistance", 285.0));   // the driver

        const LmFieldStats i = stat(lmSessionStats(irons, 0), "lm.carryDistance");
        const LmFieldStats m = stat(lmSessionStats(mixed, 0), "lm.carryDistance");
        check(near(i.mean, 165.0, 1e-9), "one club: the mean is the golfer's carry");
        check(m.mean > 190.0, "a mixed bag: the mean is a number nobody hit");
        check(m.sd > 4.0 * i.sd, "…and the spread it implies is fiction");
        // Same focused shot (160 yd) both times, so the z of ONE reading moves purely on
        // scope — and it moves the wrong way, which is the part worth pinning. The driver
        // inflates the sd far more than it shifts the mean, so a carry that is a full
        // standard deviation short of this golfer's own 7-iron reads as unremarkable.
        // Mixing the bag does not just blur the strip, it hides the shot the golfer came
        // to find.
        check(near(i.z, -1.0, 1e-9), "one club: the short one is a full SD short");
        check(std::fabs(m.z) < 0.7, "a mixed bag: the same shot reads as unremarkable");
    }

    // ── non-finite readings are not readings ───────────────────────────────────
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::vector<LmShotValues> shots = { one("lm.launchAngle", 12.0),
                                            one("lm.launchAngle", nan),
                                            one("lm.launchAngle", 14.0),
                                            one("lm.launchAngle", 13.0) };
        const LmFieldStats s = stat(lmSessionStats(shots, 0), "lm.launchAngle");
        check(s.n == 3, "a NaN reading is skipped rather than counted");
        check(near(s.mean, 13.0, 1e-9), "…and does not poison the mean");
        check(std::isfinite(s.sd) && s.hasSpread, "…or the sd");

        // Focused ON the NaN: no value, everything else intact.
        check(!stat(lmSessionStats(shots, 1), "lm.launchAngle").hasLatest,
              "focusing a NaN reading yields no value");

        const std::vector<LmShotValues> allNan = { one("lm.launchAngle", nan) };
        check(!present(lmSessionStats(allNan, 0), "lm.launchAngle"),
              "a field whose every reading is non-finite is omitted, like an unreported one");
    }

    // ── order is fieldDefs() order, always ─────────────────────────────────────
    {
        // Handed in the reverse of the table's order; the output must not care.
        std::vector<LmShotValues> shots(1);
        for (const pinpoint::lm::FieldDef &f : pinpoint::lm::fieldDefs())
            shots[0].insert(QString::fromLatin1(f.key), 1.0);

        const auto st = lmSessionStats(shots, 0);
        check(st.size() == pinpoint::lm::fieldDefs().size(), "every reported field gets a row");
        bool ordered = true;
        for (size_t i = 0; i < st.size(); ++i)
            if (st[i].key != QString::fromLatin1(pinpoint::lm::fieldDefs()[i].key)) ordered = false;
        check(ordered, "rows are in fieldDefs() declaration order");
        bool defs = true;
        for (const LmFieldStats &s : st)
            if (s.def == nullptr || QString::fromLatin1(s.def->key) != s.key) defs = false;
        check(defs, "each row points at its own catalogue entry");
    }

    check(lmSessionStats({}, 0).empty(), "no shots at all ⇒ no rows (the panel says so instead)");
    check(lmSessionStats({ LmShotValues{} }, 0).empty(), "a shot with no readings ⇒ no rows");

    // ── the tick: a ±3 SD axis, clamped inside its own strip ───────────────────
    {
        check(near(lmTickPercent(0.0), 50.0, 1e-9), "z 0 sits at the centre");
        check(near(lmTickPercent(1.0), 66.666666666, 1e-6), "z +1 sits at the top of the SD band");
        check(near(lmTickPercent(-1.0), 33.333333333, 1e-6), "z −1 at the bottom of it");
        check(near(lmTickPercent(1.0), kLmSdBandHi, 1e-9), "…which is the band's own edge");
        check(near(lmTickPercent(-1.0), kLmSdBandLo, 1e-9), "…both of them");
        check(near(lmTickPercent(3.0), 98.5, 1e-9), "z +3 reaches the clamped top");
        check(near(lmTickPercent(-3.0), 1.5, 1e-9), "z −3 the clamped bottom");
        check(near(lmTickPercent(12.0), 98.5, 1e-9), "a wild outlier clamps rather than escaping");
        check(near(lmTickPercent(-99.0), 1.5, 1e-9), "…in both directions");
        check(near(lmTickPercent(std::numeric_limits<double>::quiet_NaN()), 50.0, 1e-9),
              "a non-finite z parks at the centre instead of vanishing");
        check(near(kLmSdBandHi - kLmSdBandLo, 100.0 / 3.0, 1e-9),
              "the ±1 SD band is a third of the axis, by construction");
    }

    // ── formatting: the board's own numbers ────────────────────────────────────
    {
        check(lmDecimals(QStringLiteral("mph"))   == 1, "speeds to a tenth");
        check(lmDecimals(QStringLiteral("ratio")) == 2, "smash to a hundredth");
        check(lmDecimals(QStringLiteral("rpm"))   == 0, "spin to the rpm");
        check(lmDecimals(QStringLiteral("mm"))    == 0, "strike to the millimetre");
        check(lmDecimals(QStringLiteral("ft"))    == 0, "apex to the foot");
        check(lmDecimals(QStringLiteral("°"))     == 1, "angles to a tenth");
        check(lmDecimals(QStringLiteral("yd"))    == 1, "distances to a tenth");
        check(lmDecimals(QStringLiteral("furlong")) == 1, "an unknown unit still formats");

        check(lmFormat(84.53, 1) == QStringLiteral("84.5"), "rounds to the chosen places");
        check(lmFormat(4686.0, 0) == QStringLiteral("4686"), "whole numbers carry no point");
        check(lmFormat(-3.5, 1) == QString(QChar(0x2212)) + QStringLiteral("3.5"),
              "negatives use a typographic minus, not a hyphen");
        check(!lmFormat(-3.5, 1).contains(QLatin1Char('-')), "…and no hyphen survives");
        check(lmFormat(-0.04, 1) == QStringLiteral("0.0"),
              "a value that rounds to zero is not printed as negative zero");
        check(lmFormat(-0.004, 2) == QStringLiteral("0.00"), "…at any precision");
        check(lmFormat(-0.06, 1) == QString(QChar(0x2212)) + QStringLiteral("0.1"),
              "…while a value that does NOT round to zero keeps its sign");
        check(lmFormat(std::numeric_limits<double>::infinity(), 1) == QStringLiteral("—"),
              "a non-finite number prints as absent, not as 'inf'");
    }
    {
        // The three tile strings, end to end, on a field with a real spread.
        const std::vector<LmShotValues> shots = { one("lm.clubheadSpeed", 80.0),
                                                  one("lm.clubheadSpeed", 84.0),
                                                  one("lm.clubheadSpeed", 88.0),
                                                  one("lm.clubheadSpeed", 92.0) };
        const LmFieldStats s = stat(lmSessionStats(shots, 3), "lm.clubheadSpeed");
        check(lmValueText(s) == QStringLiteral("92.0"), "the value row prints the focused reading");
        check(lmMeanText(s)  == QStringLiteral("μ 86.0"), "the mean carries its own mu");
        check(lmSdText(s)    == QStringLiteral("±5.2"),
              "the spread is quoted at the value's own precision, not finer");

        // Below the floor: mean yes, spread no — and the tag is a dash, not missing.
        const std::vector<LmShotValues> two = { one("lm.clubheadSpeed", 80.0),
                                                one("lm.clubheadSpeed", 92.0) };
        const LmFieldStats t = stat(lmSessionStats(two, 0), "lm.clubheadSpeed");
        check(lmMeanText(t) == QStringLiteral("μ 86.0"), "the mean shows below the floor");
        check(lmSdText(t)   == QStringLiteral("—"), "…and the spread reads as an em dash");
        check(lmSdText(t)   == lmAbsent(), "…the same em dash a missing value uses");
    }

    // ── The joint spread: the dispersion ellipse ────────────────────────────────
    //
    // What the schematics shade behind a vector. The single-field statistics above
    // cannot express a PATTERN — two SDs describe a box, and a golfer's misses lie on a
    // diagonal inside it.
    {
        // A pair of shots is a pair, not a pattern: the same three-shot floor.
        const std::vector<LmShotValues> two = { pair(2.0, 2.0), pair(-2.0, -2.0) };
        check(!lmPairStats(two, k("lm.strikeLocation"), k("lm.strikeHeight")).has,
              "two shots produce no ellipse");

        // Perfectly correlated, on the 45° diagonal: all the variance is on one axis, so
        // the minor axis collapses and the tilt is 45°. The case an axis-aligned ellipse
        // gets most wrong — it would draw a square where the truth is a line.
        const std::vector<LmShotValues> diag = { pair(-4.0, -4.0), pair(0.0, 0.0),
                                                 pair(4.0, 4.0) };
        const LmPairStats d = lmPairStats(diag, k("lm.strikeLocation"), k("lm.strikeHeight"));
        check(d.has, "three correlated shots produce an ellipse");
        check(d.n == 3, "…over all three of them");
        check(near(d.meanX, 0.0, 1e-9) && near(d.meanY, 0.0, 1e-9), "centred on the mean");
        check(near(d.r, 1.0, 1e-9), "…perfectly correlated");
        check(near(d.tiltDeg, 45.0, 1e-6), "…tilted along the pattern, not the axes");
        check(near(d.minorSd, 0.0, 1e-9), "…with no width across it");
        check(near(d.majorSd, std::sqrt(2.0) * d.sdX, 1e-9),
              "…and its length is the diagonal of the two axis spreads");

        // The other diagonal tilts the other way. A sign error here would draw every
        // heel-low pattern as toe-low.
        const std::vector<LmShotValues> anti = { pair(-4.0, 4.0), pair(0.0, 0.0),
                                                 pair(4.0, -4.0) };
        const LmPairStats a = lmPairStats(anti, k("lm.strikeLocation"), k("lm.strikeHeight"));
        check(near(a.r, -1.0, 1e-9), "the opposite diagonal is anti-correlated");
        check(near(a.tiltDeg, -45.0, 1e-6), "…and tilts the other way");

        // Uncorrelated and unequal: the major axis lies along the wider spread, and the
        // ellipse is axis-aligned because the data really is.
        const std::vector<LmShotValues> box = { pair(-6.0, -1.0), pair(6.0, -1.0),
                                                pair(-6.0, 1.0),  pair(6.0, 1.0) };
        const LmPairStats b = lmPairStats(box, k("lm.strikeLocation"), k("lm.strikeHeight"));
        check(b.has, "a square pattern is still an ellipse");
        check(near(b.r, 0.0, 1e-9), "…uncorrelated");
        check(near(b.tiltDeg, 0.0, 1e-6), "…so it does not tilt");
        check(b.majorSd > b.minorSd, "…major axis along the wider spread");
        check(near(b.majorSd, b.sdX, 1e-9) && near(b.minorSd, b.sdY, 1e-9),
              "…and the axes are just the two spreads");

        // n-1, the same denominator the rest of the header uses.
        check(near(b.sdX, 6.0 * std::sqrt(4.0 / 3.0), 1e-9), "sample SD, n-1 denominator");

        // Every shot identical is a point, not a pattern. No divide, no ellipse.
        const std::vector<LmShotValues> same = { pair(3.0, 3.0), pair(3.0, 3.0),
                                                 pair(3.0, 3.0) };
        check(!lmPairStats(same, k("lm.strikeLocation"), k("lm.strikeHeight")).has,
              "no spread, no ellipse — and no divide by zero");

        // BOTH fields or neither. A shot the monitor read the face on but lost the ball
        // for must not contribute half a point: a pair statistic assembled from two
        // different subsets describes no shot anyone hit.
        std::vector<LmShotValues> partial = { pair(-4.0, -4.0), pair(0.0, 0.0),
                                              pair(4.0, 4.0) };
        partial.push_back(one("lm.strikeLocation", 40.0));     // height missing
        const LmPairStats p = lmPairStats(partial, k("lm.strikeLocation"), k("lm.strikeHeight"));
        check(p.n == 3, "a half-reported shot is skipped entirely");
        check(near(p.meanX, 0.0, 1e-9), "…so it cannot drag the mean");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
