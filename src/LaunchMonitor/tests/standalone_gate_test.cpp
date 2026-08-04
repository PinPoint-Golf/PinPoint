// Whether a reading nothing else saw becomes a shot of its own.
//
// This decision has behaved differently on a real machine than it did on paper TWICE —
// once because it did not require capture to be active at all, and once because the whole
// path sat behind a setting that was off, so a tester saw silence and concluded nothing
// had been delivered. Both were reasoning errors that no amount of care in the controller
// would have caught, because the controller is where the facts are GATHERED and this is
// where they are JUDGED. Hence a pure function, and hence this file.
//
// The rescue path is deliberately absent from here: a shot the app detected but could not
// record needs no permission beyond storage being on.

#include "standalone_gate.h"

#include <gtest/gtest.h>

using namespace pinpoint::lm;

namespace {

// Everything true — the one combination that records. Each test turns exactly one thing
// off, so a test that fails names the precondition that stopped working.
StandaloneFacts allGood()
{
    StandaloneFacts f;
    f.connectorConfigured = true;
    f.standaloneEnabled   = true;
    f.storeDeviceData     = true;
    f.libraryConfigured   = true;
    f.athleteSelected     = true;
    f.sessionRunning      = true;
    f.captureActive       = true;
    return f;
}

} // namespace

TEST(StandaloneGate, RecordsWhenEverythingHolds)
{
    EXPECT_EQ(decideStandalone(allGood()), StandaloneVerdict::Record);
    EXPECT_TRUE(standaloneVerdictReason(StandaloneVerdict::Record).isEmpty());
}

// ── Each precondition, alone, blocks ────────────────────────────────────────────

TEST(StandaloneGate, NoConnectorBlocks)
{
    auto f = allGood(); f.connectorConfigured = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::NotConfigured);
}

TEST(StandaloneGate, TheSettingBeingOffBlocks)
{
    // OFF BY DEFAULT, and this is the one that made a tester think nothing had shipped.
    auto f = allGood(); f.standaloneEnabled = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::Disabled);
}

TEST(StandaloneGate, StorageBeingOffBlocks)
{
    // "Read the device and discard it" has to mean discard, not discard-except-this.
    auto f = allGood(); f.storeDeviceData = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::StorageOff);
}

TEST(StandaloneGate, NoLibraryBlocks)
{
    auto f = allGood(); f.libraryConfigured = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::NoLibrary);
}

TEST(StandaloneGate, NoAthleteBlocks)
{
    // allocateSwingDir needs a name and a uuid, and the uuid is what a norm cohort
    // resolves through — a shot filed against nobody could never be graded.
    auto f = allGood(); f.athleteSelected = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::NoAthlete);
}

TEST(StandaloneGate, NoSessionBlocks)
{
    auto f = allGood(); f.sessionRunning = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::NoSession);
}

TEST(StandaloneGate, CaptureNotActiveBlocks)
{
    // THE ONE THAT WAS MISSING. A session left open would otherwise record every ball
    // anyone hits on the simulator — which is exactly what pressing Stop said not to do.
    // Nothing about the buffer can stand in for it: with no cameras and no IMUs there are
    // no sources, so the buffer is paused however loudly the user says they are hitting.
    auto f = allGood(); f.captureActive = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::CaptureInactive);
}

TEST(StandaloneGate, NothingIsTrueAndNothingRecords)
{
    EXPECT_EQ(decideStandalone(StandaloneFacts{}), StandaloneVerdict::NotConfigured);
}

// ── Exhaustive: only the all-true combination records ───────────────────────────

TEST(StandaloneGate, EveryOtherCombinationRefuses)
{
    // 2^7 = 128 states. Enumerating them is the point: a precondition added later without
    // a thought about ordering, or one accidentally dropped, shows up here as a state
    // that records when it should not.
    int recorded = 0, refused = 0;
    for (int bits = 0; bits < 128; ++bits) {
        StandaloneFacts f;
        f.connectorConfigured = bits & 1;
        f.standaloneEnabled   = bits & 2;
        f.storeDeviceData     = bits & 4;
        f.libraryConfigured   = bits & 8;
        f.athleteSelected     = bits & 16;
        f.sessionRunning      = bits & 32;
        f.captureActive       = bits & 64;

        const bool all = f.connectorConfigured && f.standaloneEnabled && f.storeDeviceData
                      && f.libraryConfigured && f.athleteSelected && f.sessionRunning
                      && f.captureActive;
        const auto v = decideStandalone(f);
        if (v == StandaloneVerdict::Record) { ++recorded; EXPECT_TRUE(all) << "bits " << bits; }
        else                                { ++refused;  EXPECT_FALSE(all) << "bits " << bits; }
    }
    EXPECT_EQ(recorded, 1);
    EXPECT_EQ(refused, 127);
}

// ── Which reason gets reported when several are false ───────────────────────────

TEST(StandaloneGate, ReportsConfigurationBeforeCurrentState)
{
    // Somebody still setting the monitor up hears about the setting rather than about
    // capture, because the setting is what they can act on. A verdict rather than a bool
    // exists precisely so this ordering is a decision somebody made and can revisit.
    auto f = allGood();
    f.standaloneEnabled = false;
    f.captureActive     = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::Disabled);
}

TEST(StandaloneGate, ReportsCaptureWhenThatIsAllThatIsMissing)
{
    // And somebody already set up hears about capture, rather than being told again
    // about a library they configured weeks ago.
    auto f = allGood();
    f.captureActive = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::CaptureInactive);
}

TEST(StandaloneGate, SessionOutranksCapture)
{
    auto f = allGood();
    f.sessionRunning = false;
    f.captureActive  = false;
    EXPECT_EQ(decideStandalone(f), StandaloneVerdict::NoSession);
}

// ── Every refusal says something ────────────────────────────────────────────────

TEST(StandaloneGate, EveryRefusalCarriesAReason)
{
    // The log line is the only thing a user has when nothing happens, and "nothing
    // happened silently" is what cost real time on a real machine.
    const StandaloneVerdict all[] = {
        StandaloneVerdict::NotConfigured, StandaloneVerdict::Disabled,
        StandaloneVerdict::StorageOff,    StandaloneVerdict::NoLibrary,
        StandaloneVerdict::NoAthlete,     StandaloneVerdict::NoSession,
        StandaloneVerdict::CaptureInactive,
    };
    for (const StandaloneVerdict v : all) {
        const QString reason = standaloneVerdictReason(v);
        EXPECT_FALSE(reason.isEmpty());
        // Phrased as what is missing, not as an error: none of these is one.
        EXPECT_FALSE(reason.contains(QStringLiteral("error"), Qt::CaseInsensitive));
        EXPECT_FALSE(reason.contains(QStringLiteral("fail"), Qt::CaseInsensitive));
    }
}
