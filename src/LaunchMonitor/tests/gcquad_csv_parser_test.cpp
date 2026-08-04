// LastShot.CSV → LaunchMonitorReading.
//
// The reference row is the real sample captured from FSX2020 (see
// /mnt/swingdata/LastShot.CSV), reproduced verbatim including its trailing comma.
// Expected values are the catalogue-unit conversions of that row, computed by hand
// so the test fails if the conversion constants are ever "tidied".
//
// SIGNS ARE THE POINT OF HALF THIS FILE. The whole reason for a launch monitor here
// is to compare its measurement against our own estimate; a sign that disagrees with
// the catalogue's stated polarity turns that comparison into nonsense that still
// looks plausible. So every signed quantity is asserted with its sign explicitly.

#include "gcquad_csv_parser.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

using namespace pinpoint::lm;

namespace {

// The real header and row, byte for byte.
const char *kHeader =
    "Shot ID, Club, Club head Speed (m/s), Ball Speed (m/s), Launch Angle (deg), "
    "Azimuth (deg), Side Spin (rpm), Back Spin (rpm), Total Spin (rpm), "
    "Descent Angle (deg), Carry (m), Total Distance (m), Offline (m), "
    "Peak Height (m), Distance to Pin (m), Vert Path (deg), Horiz Path (deg), "
    "Face to Path (deg), Face to Target (deg), Lie (deg), Loft (deg), "
    "Closure Rate (deg/s), Horiz Impact (mm), Vert Impact (mm), ";

const char *kRow =
    "283, Irn, 38.978607, 49.923466, 19.695501, 4.982033, 1635, 7437, 7614, "
    "47.953861, 137.551910, 145.092789, 29.383856, 28.229187, 37.012428, "
    "-6.255224, 1.319672, 5.622859, 6.942531, -1.328706, 29.739687, "
    "3232.705811, 2.136441, -17.925056, ";

QByteArray sampleFile()
{
    return QByteArray(kHeader) + "\r\n" + QByteArray(kRow) + "\r\n";
}

} // namespace

// ── The reference row ───────────────────────────────────────────────────────────

TEST(GcQuadCsvParser, ReadsTheReferenceRow)
{
    QString err;
    const auto r = parseLastShotCsv(sampleFile(), &err);
    ASSERT_TRUE(r.has_value()) << err.toStdString();
    EXPECT_TRUE(err.isEmpty());

    EXPECT_EQ(r->deviceShotId.toStdString(), "283");
    EXPECT_EQ(r->deviceClub.toStdString(), "Irn");
}

TEST(GcQuadCsvParser, ConvertsSpeedsToMph)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    // 38.978607 m/s ÷ 0.44704, 49.923466 m/s ÷ 0.44704
    ASSERT_TRUE(r->clubheadSpeed.has_value());
    EXPECT_NEAR(*r->clubheadSpeed, 87.1927, 0.01);
    ASSERT_TRUE(r->ballSpeed.has_value());
    EXPECT_NEAR(*r->ballSpeed, 111.6756, 0.01);
}

TEST(GcQuadCsvParser, ConvertsDistances)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    ASSERT_TRUE(r->carryDistance.has_value());
    EXPECT_NEAR(*r->carryDistance, 150.4286, 0.01);   // 137.55191 m → yd
    ASSERT_TRUE(r->totalDistance.has_value());
    EXPECT_NEAR(*r->totalDistance, 158.6754, 0.01);   // 145.092789 m → yd
    ASSERT_TRUE(r->distanceToPin.has_value());
    EXPECT_NEAR(*r->distanceToPin, 40.4773, 0.01);    // 37.012428 m → yd

    // Peak height is read in feet, not yards — golf states apex in feet.
    ASSERT_TRUE(r->peakHeight.has_value());
    EXPECT_NEAR(*r->peakHeight, 92.6154, 0.01);       // 28.229187 m → ft
}

TEST(GcQuadCsvParser, KeepsMillimetresForStrike)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    // Already mm in the file and mm in the catalogue — must pass through untouched.
    ASSERT_TRUE(r->strikeLocation.has_value());
    EXPECT_NEAR(*r->strikeLocation, 2.136441, 1e-6);
    ASSERT_TRUE(r->strikeHeight.has_value());
    EXPECT_NEAR(*r->strikeHeight, -17.925056, 1e-6);
}

TEST(GcQuadCsvParser, AnglesPassThroughWithTheirSigns)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    EXPECT_NEAR(*r->launchAngle,     19.695501, 1e-6);
    EXPECT_NEAR(*r->launchDirection,  4.982033, 1e-6);   // Azimuth
    EXPECT_NEAR(*r->descentAngle,    47.953861, 1e-6);
    EXPECT_NEAR(*r->faceToPath,       5.622859, 1e-6);
    EXPECT_NEAR(*r->faceAngle,        6.942531, 1e-6);   // Face to Target
    EXPECT_NEAR(*r->dynamicLoft,     29.739687, 1e-6);   // Loft
    EXPECT_NEAR(*r->closureRate,   3232.705811, 1e-6);

    // Negative values must survive as negative — a lost sign here is invisible
    // downstream and would silently invert a comparison against our estimate.
    ASSERT_TRUE(r->attackAngle.has_value());
    EXPECT_NEAR(*r->attackAngle, -6.255224, 1e-6);       // Vert Path, a descending strike
    ASSERT_TRUE(r->lieAngle.has_value());
    EXPECT_NEAR(*r->lieAngle,    -1.328706, 1e-6);
    EXPECT_LT(*r->strikeHeight, 0.0);                    // struck below centre
}

TEST(GcQuadCsvParser, VertPathIsAttackAngleAndHorizPathIsClubPath)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    // The two are easy to transpose and the transposition is plausible-looking,
    // so pin them to the two distinct values in the sample.
    EXPECT_NEAR(*r->attackAngle, -6.255224, 1e-6);
    EXPECT_NEAR(*r->clubPath,     1.319672, 1e-6);
}

TEST(GcQuadCsvParser, SpinColumnsAreDistinct)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    // spinRate is TOTAL spin, not back spin. Confusing them understates spin by
    // the side component on every shot.
    EXPECT_NEAR(*r->spinRate, 7614.0, 1e-6);
    EXPECT_NEAR(*r->backSpin, 7437.0, 1e-6);
    EXPECT_NEAR(*r->sideSpin, 1635.0, 1e-6);
}

// ── The three values the device does not print ─────────────────────────────────

TEST(GcQuadCsvParser, DerivesSpinAxisFromSideAndBackSpin)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    // atan2(1635, 7437) = 12.399°, tilted right — consistent with the positive
    // side spin and the ball finishing right of target (Offline +29.4 m).
    ASSERT_TRUE(r->spinAxis.has_value());
    EXPECT_NEAR(*r->spinAxis, 12.399, 0.01);
    EXPECT_GT(*r->spinAxis, 0.0);
    EXPECT_GT(*r->offline, 0.0);
}

TEST(GcQuadCsvParser, DerivesSmashFactorFromTheSpeedPair)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    // 49.923466 / 38.978607 — a ratio, so it is unit-free and must come out the
    // same whether the file was metric or imperial (see the imperial test below).
    ASSERT_TRUE(r->smashFactor.has_value());
    EXPECT_NEAR(*r->smashFactor, 1.28079, 1e-4);
}

TEST(GcQuadCsvParser, DerivesSpinLoftAsLoftMinusAttackAngle)
{
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());

    // 29.739687 − (−6.255224). The double negative is exactly the kind of thing
    // that gets "simplified" into a subtraction of magnitudes.
    ASSERT_TRUE(r->spinLoft.has_value());
    EXPECT_NEAR(*r->spinLoft, 35.994911, 1e-6);
}

// ── Robustness across FSX2020 configurations ────────────────────────────────────

TEST(GcQuadCsvParser, MatchesColumnsByNameNotPosition)
{
    // Same three columns, reversed. A positional parser reads club speed as spin.
    const QByteArray f =
        "Shot ID, Total Spin (rpm), Ball Speed (m/s), Club head Speed (m/s)\n"
        "42, 7614, 49.923466, 38.978607\n";

    const auto r = parseLastShotCsv(f);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->deviceShotId.toStdString(), "42");
    EXPECT_NEAR(*r->clubheadSpeed, 87.1927, 0.01);
    EXPECT_NEAR(*r->ballSpeed,    111.6756, 0.01);
    EXPECT_NEAR(*r->spinRate,     7614.0,   1e-6);
}

TEST(GcQuadCsvParser, HonoursImperialUnitsDeclaredInTheHeader)
{
    // FSX2020 can be switched to imperial: same columns, different declared units.
    // Trusting the documented metric default here would inflate every speed by 2.24.
    const QByteArray f =
        "Shot ID, Club head Speed (mph), Ball Speed (mph), Carry (yds), Peak Height (ft)\n"
        "7, 87.1927, 111.6756, 150.4286, 92.6154\n";

    const auto r = parseLastShotCsv(f);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r->clubheadSpeed, 87.1927,  0.01);
    EXPECT_NEAR(*r->ballSpeed,     111.6756, 0.01);
    EXPECT_NEAR(*r->carryDistance, 150.4286, 0.01);
    EXPECT_NEAR(*r->peakHeight,    92.6154,  0.01);
    // Unit-free, so identical to the metric row above.
    EXPECT_NEAR(*r->smashFactor,   1.28079,  1e-4);
}

TEST(GcQuadCsvParser, ReadsClosureRateInEitherPublishedUnit)
{
    // Foresight documents closure rate as reported in "degrees per second (dps) or revolutions per
    // minute (rpm)". Both must land on the same number in the catalogue's °/s, because the
    // installation chooses the unit and the golfer does not. rpm is an ANGULAR RATE here, not the
    // spin unit: 1 rev/min = 6 °/s. Read as deg/s it would be six times too low and still look
    // entirely plausible.
    const QByteArray dps =
        "Shot ID, Closure Rate (deg/s)\n"
        "1, 3232.705811\n";
    const QByteArray rpm =
        "Shot ID, Closure Rate (rpm)\n"
        "2, 538.784302\n";                       // 3232.705811 / 6

    const auto a = parseLastShotCsv(dps);
    const auto b = parseLastShotCsv(rpm);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(a->closureRate.has_value());
    ASSERT_TRUE(b->closureRate.has_value());
    EXPECT_NEAR(*a->closureRate, 3232.705811, 1e-6);
    EXPECT_NEAR(*b->closureRate, 3232.705811, 1e-3);
}

TEST(GcQuadCsvParser, LieAngleSignFollowsForesight)
{
    // Foresight publishes this one: negative is toe DOWN, positive is toe UP, zero is a sole flat
    // to the ground. The sample row is negative, i.e. toe down at impact.
    const auto r = parseLastShotCsv(sampleFile());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->lieAngle.has_value());
    EXPECT_NEAR(*r->lieAngle, -1.328706, 1e-6);
    EXPECT_LT(*r->lieAngle, 0.0);
}

TEST(GcQuadCsvParser, IgnoresUnknownColumns)
{
    // A newer FSX2020 adding a column must not stop the shot being read.
    const QByteArray f =
        "Shot ID, Club head Speed (m/s), Wind Speed (m/s), Humidity (%)\n"
        "9, 38.978607, 3.2, 41\n";

    const auto r = parseLastShotCsv(f);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r->clubheadSpeed, 87.1927, 0.01);
}

TEST(GcQuadCsvParser, HandlesBomLfAndQuotedCells)
{
    const QByteArray f =
        QByteArray("\xEF\xBB\xBF") +
        "Shot ID,Club,Club head Speed (m/s)\n"
        "11,\"Iron, 7\",38.978607\n";

    const auto r = parseLastShotCsv(f);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->deviceShotId.toStdString(), "11");
    EXPECT_EQ(r->deviceClub.toStdString(), "Iron, 7");   // the comma stayed inside
    EXPECT_NEAR(*r->clubheadSpeed, 87.1927, 0.01);
}

TEST(GcQuadCsvParser, ShortRowLeavesTrailingFieldsAbsentNotZero)
{
    // A value we did not receive must be absent. Defaulting it to 0 would publish a
    // measured "0.0 mph" that grades and charts as a real reading.
    const QByteArray f =
        "Shot ID, Club head Speed (m/s), Ball Speed (m/s), Total Spin (rpm)\n"
        "12, 38.978607\n";

    const auto r = parseLastShotCsv(f);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->clubheadSpeed.has_value());
    EXPECT_FALSE(r->ballSpeed.has_value());
    EXPECT_FALSE(r->spinRate.has_value());
    // No ball speed ⇒ no smash factor. It must not fall back to anything.
    EXPECT_FALSE(r->smashFactor.has_value());
}

TEST(GcQuadCsvParser, NonNumericCellLosesOnlyItsOwnField)
{
    const QByteArray f =
        "Shot ID, Club head Speed (m/s), Ball Speed (m/s)\n"
        "13, ----, 49.923466\n";

    const auto r = parseLastShotCsv(f);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->clubheadSpeed.has_value());
    EXPECT_TRUE(r->ballSpeed.has_value());
}

// ── Refusals: everything here must return nullopt, never a partial reading ──────

TEST(GcQuadCsvParser, RejectsHeaderOnlyFile)
{
    QString err;
    EXPECT_FALSE(parseLastShotCsv(QByteArray(kHeader) + "\r\n", &err).has_value());
    EXPECT_FALSE(err.isEmpty());
}

TEST(GcQuadCsvParser, RejectsEmptyInput)
{
    // The state a torn read is most likely to catch: the file truncated to nothing
    // between FSX2020 opening it and writing. Must not consume the watermark.
    EXPECT_FALSE(parseLastShotCsv(QByteArray()).has_value());
    EXPECT_FALSE(parseLastShotCsv(QByteArray("   \r\n  \r\n")).has_value());
}

TEST(GcQuadCsvParser, RejectsRowWithNoShotId)
{
    const QByteArray f =
        "Shot ID, Club head Speed (m/s)\n"
        ", 38.978607\n";
    EXPECT_FALSE(parseLastShotCsv(f).has_value());
}

TEST(GcQuadCsvParser, RejectsHeaderWithNoRecognisedColumns)
{
    const QByteArray f =
        "Shot ID, Humidity (%), Wind Speed (m/s)\n"
        "14, 41, 3.2\n";
    EXPECT_FALSE(parseLastShotCsv(f).has_value());
}

TEST(GcQuadCsvParser, RejectsRowWithNoNumbers)
{
    const QByteArray f =
        "Shot ID, Club head Speed (m/s), Ball Speed (m/s)\n"
        "15, , \n";
    EXPECT_FALSE(parseLastShotCsv(f).has_value());
}

// ── The header normalisation the column match depends on ───────────────────────

TEST(GcQuadCsvParser, NormalisesHeaderCells)
{
    QString token, unit;

    normaliseHeaderCell(QStringLiteral("Club head Speed (m/s)"), &token, &unit);
    EXPECT_EQ(token.toStdString(), "clubheadspeed");
    EXPECT_EQ(unit.toStdString(), "m/s");

    normaliseHeaderCell(QStringLiteral("  Distance to Pin (m) "), &token, &unit);
    EXPECT_EQ(token.toStdString(), "distancetopin");
    EXPECT_EQ(unit.toStdString(), "m");

    normaliseHeaderCell(QStringLiteral("Closure Rate (deg/s)"), &token, &unit);
    EXPECT_EQ(token.toStdString(), "closurerate");
    EXPECT_EQ(unit.toStdString(), "deg/s");

    // No declared unit at all — the column still matches, the default applies.
    normaliseHeaderCell(QStringLiteral("Shot ID"), &token, &unit);
    EXPECT_EQ(token.toStdString(), "shotid");
    EXPECT_TRUE(unit.isEmpty());
}

TEST(GcQuadCsvParser, SplitsQuotedCsvLines)
{
    const QStringList cells = splitCsvLine(QStringLiteral("a, \"b,c\", d, \"say \"\"hi\"\"\""));
    ASSERT_EQ(cells.size(), 4);
    EXPECT_EQ(cells.at(0).toStdString(), "a");
    EXPECT_EQ(cells.at(1).toStdString(), "b,c");
    EXPECT_EQ(cells.at(2).toStdString(), "d");
    EXPECT_EQ(cells.at(3).toStdString(), "say \"hi\"");
}
