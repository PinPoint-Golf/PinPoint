// Standalone test for the session-metadata aggregation used by the carousel's
// session-review drawer. Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure
//
// Covers avg quality (rounded mean), length label (min→max span, min/h formats),
// club mix (distinct, first-seen order), day/time labels (relative + earliest),
// preview-thumb cap, and the empty / unknown-timestamp degenerate cases.

#include "../../Gui/review/session_summary.h"

#include <cstdio>

using namespace pinpoint;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static void checkEq(const QString &got, const QString &want, const char *label)
{
    const bool ok = (got == want);
    std::printf("  [%s] %-34s got \"%s\"  want \"%s\"\n", ok ? "PASS" : "FAIL", label,
                got.toUtf8().constData(), want.toUtf8().constData());
    if (!ok) ++g_fail;
}
static void checkEq(int got, int want, const char *label)
{
    const bool ok = (got == want);
    std::printf("  [%s] %-34s got %d  want %d\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}

// Local instant → epoch ms; round-trips through local time so the time/day labels
// (also formatted in local time) are timezone-independent.
static qint64 ms(int y, int mo, int d, int h, int mi)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi)).toMSecsSinceEpoch();
}

int main()
{
    std::printf("=== session summary aggregation ===\n");
    const qint64 now = ms(2026, 6, 9, 16, 0);   // fixed "now" → deterministic day labels

    // 1. Typical session: rounded mean, distinct clubs, 42-minute span, earliest time.
    {
        QVector<ShotSummaryInput> shots = {
            { 58, true,  ms(2026, 6, 9, 14, 32), QStringLiteral("7 IRON"),         QStringLiteral("/s/a.jpg") },
            { 76, true,  ms(2026, 6, 9, 14, 40), QStringLiteral("PITCHING WEDGE"), QString() },
            { 84, true,  ms(2026, 6, 9, 15, 14), QStringLiteral("7 IRON"),         QStringLiteral("/s/c.jpg") },
        };
        const SessionSummary s = summarizeSession(shots, now);
        checkEq(s.shotCount, 3, "shotCount");
        checkEq(s.avgQuality, 73, "avgQuality round((58+76+84)/3)");   // 72.67 -> 73
        // distinct/first-seen, canonical ids sentence-cased for display.
        checkEq(s.clubMix, QStringLiteral("7 iron · Pitching wedge"), "clubMix distinct/first-seen");
        checkEq(s.lengthLabel, QStringLiteral("42 min"), "lengthLabel span");
        checkEq(s.dayLabel, QStringLiteral("Today"), "dayLabel today");
        checkEq(s.timeLabel, QStringLiteral("14:32"), "timeLabel earliest");
        check(s.previewThumbs.size() == 2, "previewThumbs skip empty (2 of 3)");
    }

    // 2. Over an hour → "H h M m"; 10:05 → 11:17 == 72 min.
    {
        QVector<ShotSummaryInput> shots = {
            { 70, true, ms(2026, 6, 7, 10, 5),  QStringLiteral("DRIVER"), QString() },
            { 80, true, ms(2026, 6, 7, 11, 17), QStringLiteral("DRIVER"), QString() },
        };
        const SessionSummary s = summarizeSession(shots, now);
        checkEq(s.lengthLabel, QStringLiteral("1 h 12 m"), "lengthLabel hours+min");
        checkEq(s.clubMix, QStringLiteral("Driver"), "clubMix single");
    }

    // 3. Empty session → zeros and empty labels (the 0-shot live row case).
    {
        const SessionSummary s = summarizeSession({}, now);
        checkEq(s.shotCount, 0, "empty shotCount");
        checkEq(s.avgQuality, 0, "empty avgQuality");
        check(s.lengthLabel.isEmpty() && s.dayLabel.isEmpty() && s.timeLabel.isEmpty(),
              "empty labels");
        check(s.previewThumbs.isEmpty(), "empty previews");
    }

    // 4. Unknown timestamps (wallclockMs == 0) → no time-derived labels, but score
    //    and club mix still aggregate.
    {
        QVector<ShotSummaryInput> shots = {
            { 40, false, 0, QStringLiteral("PW"), QString() },
            { 60, false, 0, QStringLiteral("PW"), QString() },
        };
        const SessionSummary s = summarizeSession(shots, now);
        checkEq(s.avgQuality, 50, "avgQuality without timestamps");
        check(s.lengthLabel.isEmpty() && s.timeLabel.isEmpty() && s.dayLabel.isEmpty(),
              "no time labels when timestamps unknown");
    }

    // 5. Day labels: yesterday and an older dated label (format branch, locale-agnostic).
    {
        const SessionSummary y = summarizeSession(
            { { 50, true, ms(2026, 6, 8, 9, 0), QStringLiteral("7i"), QString() } }, now);
        checkEq(y.dayLabel, QStringLiteral("Yesterday"), "dayLabel yesterday");

        const qint64 older = ms(2026, 6, 5, 16, 20);
        const SessionSummary o = summarizeSession(
            { { 83, true, older, QStringLiteral("7i"), QString() } }, now);
        const QString wantDated = QDateTime::fromMSecsSinceEpoch(older).date()
                                      .toString(QStringLiteral("ddd d MMM"));
        checkEq(o.dayLabel, wantDated, "dayLabel dated (3+ days)");
        check(o.dayLabel != QStringLiteral("Today") && o.dayLabel != QStringLiteral("Yesterday"),
              "older label is neither Today nor Yesterday");
    }

    // 6. Preview thumbnails capped at maxThumbs (default 4).
    {
        QVector<ShotSummaryInput> shots;
        for (int i = 0; i < 6; ++i)
            shots.append({ 70, true, ms(2026, 6, 9, 14, i), QStringLiteral("7i"),
                           QStringLiteral("/s/%1.jpg").arg(i) });
        const SessionSummary s = summarizeSession(shots, now);
        check(s.previewThumbs.size() == 4, "previewThumbs capped at 4");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
