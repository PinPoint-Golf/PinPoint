// Standalone test for the Metric Catalogue layer (metric_catalogue / manifest / providers /
// resolver). Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.0/gcc_64
//   cmake --build build/analyzer-tests --target metric_catalogue_test --parallel 4
//   ctest --test-dir build/analyzer-tests -R metric_catalogue --output-on-failure
//
// Covers: manifest completeness (the 12 live keys, unique, correct types/groups), query filtering
// (type / group / scored / availableOnly), and per-shot resolve() across ShotContexts (session
// gating, IMU-role gating, club-track / face-on gating).
//
// NOT corridors. The catalogue no longer judges a metric — a corridor resolves through the norm set
// (Diagnostics/metric_corridor.h) and is gated by manifest_migration_test.

#include "metric_catalogue.h"

#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static void checkEqI(int got, int want, const char *label)
{
    const bool ok = (got == want);
    std::printf("  [%s] %-40s got %d  want %d\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}

static int countType(const MetricCatalogue &cat, MetricType t)
{
    MetricQuery q;
    q.type = t;
    return static_cast<int>(cat.query(q).size());
}

// A Wrist-Motion shot with the given IMU roles bound and (optionally) a face-on camera + club track.
static ShotContext wristShot(std::vector<SegmentRole> roles, bool faceOn = false, bool club = false)
{
    ShotContext c;
    c.sessionType  = 1;               // Wrist
    c.imuRoles     = std::move(roles);
    c.hasFaceOn    = faceOn;
    c.hasClubTrack = club;
    c.tier         = ReconstructionTier::Mono3DPlusImu;
    return c;
}

int main()
{
    std::printf("=== metric catalogue ===\n");
    const MetricCatalogue cat = makeMetricCatalogue();

    // 1. Manifest completeness — the full design catalogue (21 live + 31 planned), each resolvable.
    // The nine additions are the measures the shipped diagnostics pack depends on: every
    // characteristic must resolve to a catalogue metric, so the pack cannot become a second
    // parallel registry of measures. See diagnostics_catalogue_integrity_test, which checks the two
    // registries agree in both directions.
    {
        checkEqI(static_cast<int>(cat.all().size()), 72, "descriptor count == 72");
        const char *live[] = { "leadWristFlexExt", "leadWristRadUln", "forearmPronation",
                               "leadArmFlexion",  "clubheadSpeed",   "handSpeed", "lagAngle",
                               "impactShaftLean", "stanceWidth",     "leadFootFlare",
                               "trailFootFlare",  "toeLineAngle",    "leadHeelLift",
                               "ballPosition",
                               "headSway",        "headLift",        "headTilt",
                               "tempoBackswing",  "tempoRatio",
                               "wristScore",      "wristResemblance" };
        bool allPresent = true;
        for (const char *k : live)
            if (!cat.descriptor(QString::fromLatin1(k))) { allPresent = false;
                std::printf("    missing live descriptor: %s\n", k); }
        check(allPresent, "all 21 live keys have a descriptor");
        check(cat.descriptor(QStringLiteral("tempo")) == nullptr, "tempo absent (use tempoBackswing)");
        // ballPosition used to be asserted ABSENT here; it now has a producer
        // (ball_position.cpp via FootMetricsStage), so it is in the live list above.
    }

    // 2. Type / group / scored filtering.
    {
        checkEqI(countType(cat, MetricType::TimeSeries),  38, "TimeSeries count");
        checkEqI(countType(cat, MetricType::PointInTime), 28, "PointInTime count");
        checkEqI(countType(cat, MetricType::Summary),      5, "Summary count");
        checkEqI(countType(cat, MetricType::Sequence),     1, "Sequence count (kinematicSequence)");

        MetricQuery gq; gq.group = QStringLiteral("Wrist & forearm");
        checkEqI(static_cast<int>(cat.query(gq).size()), 5, "group 'Wrist & forearm' == 5");

        MetricQuery scq; scq.group = QStringLiteral("Score");
        checkEqI(static_cast<int>(cat.query(scq).size()), 3, "group 'Score' == 3");

        MetricQuery hq; hq.group = QStringLiteral("Head");
        checkEqI(static_cast<int>(cat.query(hq).size()), 3, "group 'Head' == 3");

        MetricQuery brq; brq.group = QStringLiteral("Body rotation");
        checkEqI(static_cast<int>(cat.query(brq).size()), 6, "group 'Body rotation' == 6");

        // Arm geometry (trail elbow height, swing width, arm-to-torso) is its own group rather
        // than being filed under wrist and forearm, which would mislabel it in the directory.
        MetricQuery armq; armq.group = QStringLiteral("Arms");
        checkEqI(static_cast<int>(cat.query(armq).size()), 4, "group 'Arms' == 4");

        // Two groups arrived with the content extension. Ball flight is what the golfer sees and
        // Strike is what the face did; keeping them apart matters because one of them is mostly
        // camera-resolvable and the other is entirely launch-monitor territory.
        MetricQuery bfq; bfq.group = QStringLiteral("Ball flight");
        checkEqI(static_cast<int>(cat.query(bfq).size()), 7, "group 'Ball flight' == 7");

        MetricQuery stq; stq.group = QStringLiteral("Strike");
        checkEqI(static_cast<int>(cat.query(stq).size()), 2, "group 'Strike' == 2");

        MetricQuery alq; alq.group = QStringLiteral("Alignment");
        checkEqI(static_cast<int>(cat.query(alq).size()), 4, "group 'Alignment' == 4");

        MetricQuery sq; sq.scored = true;
        checkEqI(static_cast<int>(cat.query(sq).size()), 4, "scored == true → 4 (wrist DOFs)");
    }

    // 3. resolve() — session + IMU-role gating (wrist).
    {
        const ShotContext core = wristShot({ SegmentRole::LeadForearm, SegmentRole::LeadHand });
        check(cat.resolve(QStringLiteral("leadWristFlexExt"), core).state == MetricAvailability::Measured,
              "bow/cup Measured with forearm+hand");
        check(cat.resolve(QStringLiteral("forearmPronation"), core).state == MetricAvailability::Unavailable,
              "roll Unavailable without upper-arm");

        const ShotContext full = wristShot({ SegmentRole::LeadForearm, SegmentRole::LeadHand,
                                             SegmentRole::LeadUpperArm });
        check(cat.resolve(QStringLiteral("forearmPronation"), full).state == MetricAvailability::Measured,
              "roll Measured with upper-arm added");

        ShotContext swing = core; swing.sessionType = 0;   // Swing session
        const MetricAvailability sw = cat.resolve(QStringLiteral("leadWristFlexExt"), swing);
        check(sw.state == MetricAvailability::Unavailable, "wrist Unavailable in a Swing session");
        check(sw.reason.contains(QStringLiteral("Wrist Motion")), "reason names Wrist Motion");

        // sessionType -1 (directory browse) is session-agnostic → available if sensors present.
        ShotContext browse = core; browse.sessionType = -1;
        check(cat.resolve(QStringLiteral("leadWristFlexExt"), browse).state == MetricAvailability::Measured,
              "wrist Measured when browsing (no session) with sensors");
    }

    // 3b. resolve() — Summary scores (ScoreProvider).
    {
        const ShotContext core = wristShot({ SegmentRole::LeadForearm, SegmentRole::LeadHand });
        check(cat.resolve(QStringLiteral("wristScore"), core).state == MetricAvailability::Measured,
              "wristScore Measured on a Wrist shot with forearm+hand");
        check(cat.resolve(QStringLiteral("wristResemblance"), core).state == MetricAvailability::Measured,
              "wristResemblance Measured on a Wrist shot with forearm+hand");

        ShotContext swing = core; swing.sessionType = 0;
        check(cat.resolve(QStringLiteral("wristScore"), swing).state == MetricAvailability::Unavailable,
              "wristScore Unavailable in a Swing session");

        // swingScore is aspirational — no live scorer, always Unavailable.
        const MetricAvailability sw = cat.resolve(QStringLiteral("swingScore"), swing);
        check(sw.state == MetricAvailability::Unavailable, "swingScore Unavailable (no live scorer)");
        check(sw.reason.contains(QStringLiteral("scorer")), "swingScore reason names the missing scorer");
    }

    // 3c. resolve() — the newly-cataloged live producers (head-track, shaft-lean).
    {
        const ShotContext cam = wristShot({}, /*faceOn*/ true);
        check(cat.resolve(QStringLiteral("headSway"), cam).state == MetricAvailability::Measured,
              "headSway Measured on a Wrist shot with a face-on camera");
        const ShotContext noCam = wristShot({}, /*faceOn*/ false);
        check(cat.resolve(QStringLiteral("headSway"), noCam).state == MetricAvailability::Unavailable,
              "headSway Unavailable without a camera");

        const ShotContext club = wristShot({}, /*faceOn*/ true, /*club*/ true);
        check(cat.resolve(QStringLiteral("impactShaftLean"), club).state == MetricAvailability::Measured,
              "impactShaftLean Measured with face-on + club track");
        check(cat.descriptor(QStringLiteral("headSway"))->planned == false, "headSway not planned");
    }

    // 3d. Planned placeholders — declared, flagged, and always resolving 'planned'.
    //
    // DERIVED FROM THE CATALOGUE, not from a hand-written list. The list version passed while ten
    // planned descriptors were claimed by no provider at all: they fell through to the resolver's
    // no-provider branch and reported "no producer available", which is the reason an UNKNOWN key
    // gets. The two statements are not interchangeable — one says "we have not written this yet",
    // the other says "this is not a thing" — and the distinction is the entire purpose of the
    // planned flag. A hand-written list can only ever check the keys somebody remembered to add to
    // it, so a new `.planned` descriptor with no provider entry was invisible by construction.
    {
        // Use a fully-capable context to prove these are gated by "no producer", not missing sensors.
        ShotContext capable = wristShot({ SegmentRole::Pelvis, SegmentRole::Thorax,
                                          SegmentRole::LeadForearm, SegmentRole::LeadHand,
                                          SegmentRole::LeadThigh, SegmentRole::TrailThigh },
                                        /*faceOn*/ true, /*club*/ true);
        capable.hasBallTrack = true;
        capable.tier = ReconstructionTier::ClubInstrumented;

        int planned = 0, unavailable = 0, saysPlanned = 0;
        for (const MetricDescriptor *d : cat.all()) {
            if (!d->planned) continue;
            ++planned;
            const MetricAvailability a = cat.resolve(d->key, capable);
            if (a.state == MetricAvailability::Unavailable) ++unavailable;
            if (a.reason.contains(QStringLiteral("planned"))) ++saysPlanned;
            else std::printf("    planned but reason does not say so: %s -> \"%s\"\n",
                             qPrintable(d->key), qPrintable(a.reason));
        }
        std::printf("    %d planned descriptors\n", planned);
        check(planned > 0, "the catalogue has planned descriptors, so the sweep can fail");
        checkEqI(unavailable, planned,
                 "every planned metric resolves Unavailable even fully-equipped");
        // swingScore is the ONE legitimate exception and is asserted by name rather than tolerated
        // by a fuzzy count: it is claimed by ScoreProvider, which gives the more specific truth
        // ("no live scorer yet") instead of the generic roadmap reason. Anything else landing here
        // is a descriptor no provider claims.
        checkEqI(saysPlanned, planned - 1,
                 "every planned metric but one reports the ROADMAP reason, not 'no producer'");
        check(!cat.resolve(QStringLiteral("swingScore"), capable)
                   .reason.contains(QStringLiteral("planned")),
              "…and the exception is swingScore, which ScoreProvider answers more specifically");
    }

    // 3e. Launch-monitor metrics — a REQUIREMENT, not a planned promise.
    //
    // The distinction is the whole point of the split: a planned metric has no producer and always
    // resolves Unavailable, whereas these have a producer the golfer may not own. So they must read
    // "needs a launch monitor" without one and Measured with one, through the same requirement path
    // that renders every other absent input. Getting this wrong in either direction is a lie: as
    // `planned` they would promise work we are not doing, and as unconditionally Measured they
    // would report numbers nobody supplied.
    {
        const char *lm[] = { "faceAngle", "faceToPath", "spinRate", "spinAxis", "smashFactor",
                             "strikeLocation", "carryDistance", "dynamicLoft", "spinLoft" };

        ShotContext capable = wristShot({ SegmentRole::Pelvis, SegmentRole::Thorax,
                                          SegmentRole::LeadForearm, SegmentRole::LeadHand },
                                        /*faceOn*/ true, /*club*/ true);
        capable.hasBallTrack = true;
        capable.tier = ReconstructionTier::ClubInstrumented;

        int notPlanned = 0, unavailableWithout = 0, saysWhy = 0, measuredWith = 0;
        for (const char *k : lm) {
            const MetricDescriptor *d = cat.descriptor(QString::fromLatin1(k));
            if (d && !d->planned && d->requirement.launchMonitor) ++notPlanned;

            const MetricAvailability without = cat.resolve(QString::fromLatin1(k), capable);
            if (without.state == MetricAvailability::Unavailable) ++unavailableWithout;
            if (without.reason.contains(QStringLiteral("launch monitor"))) ++saysWhy;

            ShotContext withLm = capable;
            withLm.hasLaunchMonitor = true;
            if (cat.resolve(QString::fromLatin1(k), withLm).state == MetricAvailability::Measured)
                ++measuredWith;
        }
        checkEqI(notPlanned, 9, "all 9 launch-monitor metrics require the device, not a producer");
        checkEqI(unavailableWithout, 9, "…and are Unavailable on a fully-equipped shot without one");
        checkEqI(saysWhy, 9, "…each saying WHY, so the absence is graceful rather than blank");
        checkEqI(measuredWith, 9, "…and Measured the moment a connector reports one, with no "
                                  "catalogue change");
    }

    // 4. resolve() — club-track / face-on gating (kinematics + foot).
    {
        ShotContext noClub = wristShot({}, /*faceOn*/ true, /*club*/ false);
        check(cat.resolve(QStringLiteral("clubheadSpeed"), noClub).state == MetricAvailability::Unavailable,
              "clubheadSpeed Unavailable without club track");

        ShotContext club = wristShot({}, /*faceOn*/ true, /*club*/ true);
        check(cat.resolve(QStringLiteral("clubheadSpeed"), club).state == MetricAvailability::Measured,
              "clubheadSpeed Measured with club track + face-on");
        check(cat.resolve(QStringLiteral("lagAngle"), club).state == MetricAvailability::Measured,
              "lagAngle Measured with club track + face-on pose");

        ShotContext clubNoCam = wristShot({}, /*faceOn*/ false, /*club*/ true);
        check(cat.resolve(QStringLiteral("lagAngle"), clubNoCam).state == MetricAvailability::Unavailable,
              "lagAngle Unavailable without face-on pose");

        ShotContext feet = wristShot({}, /*faceOn*/ true);
        check(cat.resolve(QStringLiteral("stanceWidth"), feet).state == MetricAvailability::Measured,
              "stanceWidth Measured with face-on camera");
        ShotContext noCam = wristShot({}, /*faceOn*/ false);
        check(cat.resolve(QStringLiteral("stanceWidth"), noCam).state == MetricAvailability::Unavailable,
              "stanceWidth Unavailable without face-on camera");

        // ballPosition is the one foot-group key that needs more than the feet.
        // FootMetricProvider used to be key-agnostic; these two pin that it is not.
        check(cat.resolve(QStringLiteral("ballPosition"), feet).state == MetricAvailability::Unavailable,
              "ballPosition Unavailable with face-on camera but no ball track");
        ShotContext ballCtx = wristShot({}, /*faceOn*/ true);
        ballCtx.hasBallTrack = true;
        check(cat.resolve(QStringLiteral("ballPosition"), ballCtx).state == MetricAvailability::Measured,
              "ballPosition Measured with face-on camera + ball track");
        check(cat.resolve(QStringLiteral("stanceWidth"), ballCtx).state == MetricAvailability::Measured,
              "stanceWidth still Measured without needing a ball track");

        // Tempo needs no devices at all beyond something that segmented the swing —
        // an IMU-only shot with no camera and no club must still resolve Measured.
        const ShotContext imuOnly = wristShot({ SegmentRole::LeadForearm, SegmentRole::LeadHand },
                                              /*faceOn*/ false, /*club*/ false);
        check(cat.resolve(QStringLiteral("tempoRatio"), imuOnly).state == MetricAvailability::Measured,
              "tempoRatio Measured on an IMU-only shot (no camera, no club)");
        check(cat.resolve(QStringLiteral("tempoBackswing"), noCam).state == MetricAvailability::Measured,
              "tempoBackswing Measured with no devices bound at all");
        check(cat.descriptor(QStringLiteral("tempoRatio"))->planned == false,
              "tempoRatio no longer planned");
    }

    // 5. query availableOnly gates on the resolved context.
    {
        MetricQuery aq; aq.availableOnly = true;
        const ShotContext core = wristShot({ SegmentRole::LeadForearm, SegmentRole::LeadHand });
        const auto avail = cat.query(aq, &core);
        // forearm+hand, no camera, no club → bow/cup + hinge + wristScore +
        // wristResemblance + both tempo metrics (tempo needs no devices beyond
        // whatever segmented the swing, which an IMU pair does).
        checkEqI(static_cast<int>(avail.size()), 6, "availableOnly (forearm+hand only) → 6");
        check(cat.query(aq, nullptr).empty(), "availableOnly without ctx → empty");
    }

    // 6. There is no corridor() any more.
    //
    // The catalogue described metrics AND judged them until stage 9: `.normative` carried a DOF to
    // delegate to the compiled band table, or an inline corridor per phase. Both are gone. A
    // corridor is now (metric, phase) → measure → norm, resolved in the shot's context, and it is
    // gated by manifest_migration_test — including that every metric which HAD a corridor still
    // resolves one. There is nothing to assert here beyond what the descriptor still owns, which
    // sections 1–5 cover.

    std::printf("=== %s ===\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail ? 1 : 0;
}
