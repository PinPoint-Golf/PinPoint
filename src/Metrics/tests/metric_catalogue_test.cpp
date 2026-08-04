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
#include "launch_monitor_reading.h"

#include <algorithm>   // std::find — the "is this key claimed" sweep
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

    // 1. Manifest completeness — the full design catalogue (45 produced + 25 planned), each resolvable.
    // The nine additions are the measures the shipped diagnostics pack depends on: every
    // characteristic must resolve to a catalogue metric, so the pack cannot become a second
    // parallel registry of measures. See diagnostics_catalogue_integrity_test, which checks the two
    // registries agree in both directions.
    {
        checkEqI(static_cast<int>(cat.all().size()), 86, "descriptor count == 86");   // 70 + 25 lm. - 9 renamed
        const char *live[] = { "leadWristFlexExt", "leadWristRadUln", "forearmPronation",
                               "leadArmFlexion",  "clubheadSpeed",   "handSpeed", "lagAngle",
                               "impactShaftLean", "stanceWidth",     "leadFootFlare",
                               "trailFootFlare",  "toeLineAngle",    "leadHeelLift",
                               "ballPosition",
                               "headSway",        "headLift",        "headTilt",
                               "tempoBackswing",  "tempoRatio",
                               "wristScore",      "wristResemblance",
                               // The face-on producer batch.
                               "feetAlignment",   "comOverLeadFoot",
                               "secondaryAxisTilt", "spineSideBend", "thoraxLateralDrift",
                               "shoulderPlaneAngle", "elbowAlignment", "trailElbowHeight",
                               "leadHandWidth",   "leadUpperArmToChest", "leadArmToTorso",
                               "pelvisRotation",  "thoraxRotation", "xFactor", "xFactorStretch",
                               "shaftAngleVsHorizontal", "attackAngle", "lowPointAhead",
                               "trailWristFlexExt" };
        bool allPresent = true;
        for (const char *k : live)
            if (!cat.descriptor(QString::fromLatin1(k))) { allPresent = false;
                std::printf("    missing live descriptor: %s\n", k); }
        check(allPresent, "every produced key has a descriptor");
        check(cat.descriptor(QStringLiteral("tempo")) == nullptr, "tempo absent (use tempoBackswing)");
        // ballPosition used to be asserted ABSENT here; it now has a producer
        // (ball_position.cpp via FootMetricsStage), so it is in the live list above.
    }

    // 2. Type / group / scored filtering.
    {
        checkEqI(countType(cat, MetricType::TimeSeries),  38, "TimeSeries count");
        // 26, not 28: `shoulderAlignment` and `hipAlignment` were both PointInTime and both retired
        // as duplicates of a series the catalogue already carries.
        checkEqI(countType(cat, MetricType::PointInTime), 42, "PointInTime count");   // +16: a monitor reports one number per shot
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
        checkEqI(static_cast<int>(cat.query(bfq).size()), 17, "group 'Ball flight' == 17");

        MetricQuery stq; stq.group = QStringLiteral("Strike");
        checkEqI(static_cast<int>(cat.query(stq).size()), 3, "group 'Strike' == 3");   // + lm.strikeHeight

        // Alignment lost `shoulderAlignment` and `hipAlignment`: each was geometrically the same
        // image-plane line as a series the catalogue already carried, read at other phases, which
        // metric_reducer.h exists to express. Two descriptors for one curve is two names for one
        // number. What is left is the elbow and foot lines.
        MetricQuery alq; alq.group = QStringLiteral("Alignment");
        checkEqI(static_cast<int>(cat.query(alq).size()), 2, "group 'Alignment' == 2");
        check(cat.descriptor(QStringLiteral("shoulderAlignment")) == nullptr,
              "shoulderAlignment retired — shoulderPlaneAngle is that line");
        check(cat.descriptor(QStringLiteral("hipAlignment")) == nullptr,
              "hipAlignment retired — hipLineTilt is that line");

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

        // ANALYSIS IS AGNOSTIC OF SESSION TYPE. This block used to assert the opposite — that a
        // Swing-session shot lost the wrist metrics with the reason "produced in Wrist Motion
        // sessions only" — and that behaviour is deliberately gone. A session type is what the
        // operator meant to capture; the sensors are what was captured, and only the sensors may
        // decide. The assertion is now that the answer does NOT move with the session.
        for (const int type : { -1, 0, 1, 2, 7 }) {
            ShotContext any = core; any.sessionType = type;
            check(cat.resolve(QStringLiteral("leadWristFlexExt"), any).state
                      == MetricAvailability::Measured,
                  "wrist Measured whatever the session, given the sensors");
        }
        ShotContext noImu = wristShot({});
        noImu.sessionType = 1;
        check(cat.resolve(QStringLiteral("leadWristFlexExt"), noImu).state
                  == MetricAvailability::Unavailable,
              "and Unavailable without them, even in a Wrist session");
    }

    // 3b. resolve() — Summary scores (ScoreProvider).
    {
        const ShotContext core = wristShot({ SegmentRole::LeadForearm, SegmentRole::LeadHand });
        check(cat.resolve(QStringLiteral("wristScore"), core).state == MetricAvailability::Measured,
              "wristScore Measured on a Wrist shot with forearm+hand");
        check(cat.resolve(QStringLiteral("wristResemblance"), core).state == MetricAvailability::Measured,
              "wristResemblance Measured on a Wrist shot with forearm+hand");

        ShotContext swing = core; swing.sessionType = 0;
        check(cat.resolve(QStringLiteral("wristScore"), swing).state == MetricAvailability::Measured,
              "wristScore follows the IMUs, not the session");

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
        check(cat.descriptor(QStringLiteral("headSway"))->planned() == false, "headSway not planned");
    }

    // 3c-bis. The face-on producer batch, and the BRIDGED state.
    //
    // Bridged is the state the standing rule needs: a metric that a face-on camera can estimate but
    // an IMU could measure is neither Measured nor Unavailable, and collapsing it to either would be
    // a lie in one direction or the other. Body rotation is the only producer that answers it, and
    // it must answer PER SEGMENT — a shot with a pelvis IMU and no thorax IMU has one of each.
    {
        const ShotContext cam = wristShot({}, /*faceOn*/ true);
        for (const char *k : { "secondaryAxisTilt", "spineSideBend", "thoraxLateralDrift",
                               "shoulderPlaneAngle", "elbowAlignment", "trailElbowHeight",
                               "leadHandWidth", "leadUpperArmToChest", "leadArmToTorso",
                               "feetAlignment", "comOverLeadFoot", "trailWristFlexExt" }) {
            const MetricAvailability a = cat.resolve(QString::fromLatin1(k), cam);
            if (a.state != MetricAvailability::Measured)
                std::printf("    expected Measured with a face-on camera: %s\n", k);
            check(a.state == MetricAvailability::Measured, k);
        }

        const ShotContext noCam = wristShot({}, /*faceOn*/ false);
        check(cat.resolve(QStringLiteral("secondaryAxisTilt"), noCam).state
                  == MetricAvailability::Unavailable,
              "the upper body needs the camera");

        // Body rotation: camera only ⇒ Bridged, and the reason names the METHOD rather than a
        // missing device, because a value IS produced.
        const MetricAvailability est = cat.resolve(QStringLiteral("pelvisRotation"), cam);
        check(est.state == MetricAvailability::Bridged, "pelvisRotation Bridged from the camera");
        check(est.reason.contains(QStringLiteral("estimated")),
              "…and says it was estimated, not that something is missing");

        ShotContext pelvisImu = wristShot({ SegmentRole::Pelvis }, /*faceOn*/ true);
        check(cat.resolve(QStringLiteral("pelvisRotation"), pelvisImu).state
                  == MetricAvailability::Measured,
              "a bound pelvis IMU upgrades pelvisRotation to Measured");
        check(cat.resolve(QStringLiteral("thoraxRotation"), pelvisImu).state
                  == MetricAvailability::Bridged,
              "…while the chest, uninstrumented, stays an estimate");
        check(cat.resolve(QStringLiteral("xFactor"), pelvisImu).state == MetricAvailability::Bridged,
              "…and the separation inherits the weaker half");

        ShotContext bothImu = wristShot({ SegmentRole::Pelvis, SegmentRole::Thorax }, false);
        check(cat.resolve(QStringLiteral("xFactor"), bothImu).state == MetricAvailability::Measured,
              "both trunk IMUs Measure the separation, with no camera at all");

        ShotContext nothing = wristShot({}, /*faceOn*/ false);
        check(cat.resolve(QStringLiteral("pelvisRotation"), nothing).state
                  == MetricAvailability::Unavailable,
              "no camera and no trunk IMU is genuinely Unavailable");

        // Club delivery: the measured head, and the ball only where it is genuinely needed.
        ShotContext club = wristShot({}, /*faceOn*/ true, /*club*/ true);
        check(cat.resolve(QStringLiteral("attackAngle"), club).state == MetricAvailability::Measured,
              "attackAngle Measured from a face-on club track — it is NOT a DTL metric");
        check(cat.resolve(QStringLiteral("shaftAngleVsHorizontal"), club).state
                  == MetricAvailability::Measured,
              "shaftAngleVsHorizontal Measured with face-on + club");
        check(cat.resolve(QStringLiteral("lowPointAhead"), club).state
                  == MetricAvailability::Unavailable,
              "lowPointAhead still needs the ball it is measured against");
        ShotContext clubBall = club;
        clubBall.hasBallTrack = true;
        check(cat.resolve(QStringLiteral("lowPointAhead"), clubBall).state
                  == MetricAvailability::Measured,
              "…and lands once the ball is there");

        // attackAngle no longer demands a stereo tier. It never should have: the angle lives in the
        // vertical plane containing the target line, which is the face-on image plane.
        check(cat.descriptor(QStringLiteral("attackAngle"))->baselineRequirement().minTier
                  == ReconstructionTier::Angles2D,
              "attackAngle does not require a stereo reconstruction");
        check(!cat.descriptor(QStringLiteral("attackAngle"))->baselineRequirement().dtlCamera,
              "…nor a down-the-line camera, which is the device that tier stood in for");
        check(cat.descriptor(QStringLiteral("attackAngle"))->baselineRequirement().faceOnCamera,
              "…and does require the face-on camera it is actually read from");
    }

    // 3d. Planned metrics — every rung planned, and always resolving 'planned'.
    //
    // DERIVED FROM THE CATALOGUE, not from a hand-written list. There used to be two such lists —
    // the `.planned` flags and PlannedMetricProvider::provides() — and they drifted: ten planned
    // descriptors were in the first and not the second, so they fell through to the resolver's
    // no-provider branch and reported "no producer available", which is the reason an UNKNOWN key
    // gets. The two statements are not interchangeable — one says "we have not written this yet",
    // the other says "this is not a thing". Both lists are gone; planned is now derived from the
    // route ladder, so there is nothing left to keep in step.
    {
        // Fully capable INCLUDING a down-the-line camera — the point is that these are gated by "no
        // producer for this route", not by missing kit. Without hasDtl the depth metrics would pass
        // this sweep for the wrong reason, which is exactly the conflation the ladder separates.
        ShotContext capable = wristShot({ SegmentRole::Pelvis, SegmentRole::Thorax,
                                          SegmentRole::LeadForearm, SegmentRole::LeadHand,
                                          SegmentRole::LeadThigh, SegmentRole::TrailThigh },
                                        /*faceOn*/ true, /*club*/ true);
        capable.hasBallTrack     = true;
        capable.hasDtl           = true;
        capable.hasLaunchMonitor = true;
        capable.tier = ReconstructionTier::ClubInstrumented;

        int planned = 0, unavailable = 0, saysPlanned = 0;
        for (const MetricDescriptor *d : cat.all()) {
            if (!d->planned()) continue;
            ++planned;
            const MetricAvailability a = cat.resolve(d->key, capable);
            if (a.state == MetricAvailability::Unavailable) ++unavailable;
            if (a.reason.contains(QStringLiteral("planned"))) ++saysPlanned;
            else std::printf("    planned but reason does not say so: %s -> \"%s\"\n",
                             qPrintable(d->key), qPrintable(a.reason));
        }
        std::printf("    %d planned descriptors\n", planned);
        checkEqI(planned, 16, "16 planned metrics — nothing produces them by any route");   // the 9 launch-monitor rungs went live with the connector
        checkEqI(unavailable, planned,
                 "every planned metric resolves Unavailable even with every device present");
        checkEqI(saysPlanned, planned,
                 "…and every one says PLANNED, so none reads as a missing-sensor refusal");
        // swingScore has no exception any more, and that is the improvement: it used to be answered
        // by a hand-written branch in ScoreProvider whose sentence the directory could not see, so
        // it alone reported something other than the roadmap reason. Its route now carries the same
        // specific truth AND the planned word.
        const QString sw = cat.resolve(QStringLiteral("swingScore"), capable).reason;
        check(sw.contains(QStringLiteral("planned")) && sw.contains(QStringLiteral("scorer")),
              "swingScore says both that it is planned and precisely what is missing");
    }

    // 3d-bis. EVERY descriptor WITH A LIVE ROUTE is claimed by some provider.
    //
    // The sweeps above check that planned metrics answer the roadmap reason and that live ones
    // resolve where they should — but both walk descriptors the test names, and neither can see a
    // descriptor that simply fell out of every provider's provides(). That is a real and silent
    // failure mode: MetricCatalogue::resolve() falls back to the descriptor's own ladder, so an
    // unclaimed metric reads as a plausible "needs a face-on camera" on a shot that HAS one, and
    // stays Unavailable however capable the shot is. Nothing about it looks like a bug from the
    // directory.
    //
    // stanceWidthMm shipped exactly that way: declared, produced by foot_metrics.cpp on every
    // ruler-resolved swing, and absent from FootMetricProvider::provides(), so it was reported
    // unavailable on all of them. A per-metric case would not have caught it — this one does,
    // for every key at once and for every key added later.
    //
    // A metric whose every route is planned is EXEMPT, and deliberately: nothing produces it, so
    // there is no producer to claim it. That exemption is what let the placeholder provider go.
    {
        int unclaimed = 0;
        for (const MetricDescriptor *d : cat.all()) {
            if (d->planned()) continue;
            bool claimed = false;
            for (const IMetricProvider *p : cat.providers()) {
                const auto keys = p->provides();
                if (std::find(keys.begin(), keys.end(), d->key) != keys.end()) { claimed = true; break; }
            }
            if (!claimed) {
                ++unclaimed;
                std::printf("    NO PROVIDER CLAIMS: %s\n", qPrintable(d->key));
            }
        }
        checkEqI(unclaimed, 0, "every metric with a live route is claimed by a provider");
    }

    // 3d-ter. The route ladder itself — shape, and the two readings taken off its ends.
    {
        int noRoutes = 0, badOrder = 0;
        for (const MetricDescriptor *d : cat.all()) {
            if (d->routes.empty()) {
                ++noRoutes;
                std::printf("    NO ROUTES: %s\n", qPrintable(d->key));
                continue;
            }
            // Best-first is not decoration: resolveRoutes() takes the FIRST satisfied live rung, so
            // an Estimated rung sitting above a Direct one would hand back a Bridged answer on a
            // shot that could have been Measured.
            bool seenEstimated = false;
            for (const MetricRoute &r : d->routes) {
                if (r.quality == RouteQuality::Estimated) seenEstimated = true;
                else if (seenEstimated) {
                    ++badOrder;
                    std::printf("    ROUTES OUT OF ORDER (Direct below Estimated): %s\n",
                                qPrintable(d->key));
                    break;
                }
            }
        }
        checkEqI(noRoutes, 0, "every descriptor declares at least one acquisition route");
        checkEqI(badOrder, 0, "every ladder is ordered best-first");

        // The floor is what the directory reports as "needs", and it is the LAST live rung.
        const MetricDescriptor *pr = cat.descriptor(QStringLiteral("pelvisRotation"));
        check(pr->baselineRequirement().faceOnCamera,
              "pelvisRotation's floor is the camera, not the IMU that measures it best");
        check(pr->baselineRequirement().imuRoles.empty(), "…and the floor asks for no IMU at all");
        // Its ceiling is BOTH rungs above the floor. Stereo triangulates the hip bearing instead of
        // inferring it from foreshortening, which is a real improvement over `acos(w/w0)` — the
        // design's "the second camera adds nothing" is measured against the IMU ideal, not against
        // what a camera-only shot actually gets, and stating only the IMU here understated the
        // catalogue's own ceiling.
        const auto up = pr->upgradeDevices();
        check(up.size() == 2, "…while its ceiling names both rungs above the camera");
        bool hasImus = false, hasDtl = false;
        for (CaptureDevice dv : up) {
            if (dv == CaptureDevice::BodyImus)  hasImus = true;
            if (dv == CaptureDevice::DtlCamera) hasDtl  = true;
        }
        check(hasImus && hasDtl, "…body IMUs and a down-the-line camera");

        // The knees are the user-facing case for the whole change: readable face-on in principle,
        // properly resolvable only from down the line. Both rungs planned, so the metric is planned
        // — and it STILL reports the camera as its floor and DTL as its upgrade rather than
        // collapsing to one undifferentiated "not yet".
        const MetricDescriptor *lk = cat.descriptor(QStringLiteral("leadKneeFlexion"));
        check(lk->planned(), "leadKneeFlexion is planned — no rung is built");
        check(lk->baselineRequirement().faceOnCamera && !lk->baselineRequirement().dtlCamera,
              "…its floor is the face-on camera");
        const auto lkUp = lk->upgradeDevices();
        check(lkUp.size() == 1 && lkUp.front() == CaptureDevice::DtlCamera,
              "…and a down-the-line camera is what would improve it");

        // Depth metrics state a DEVICE. Three of them used minTier = Stereo3D as a stand-in, which
        // rendered as "a higher reconstruction tier" — true, unactionable, and unfilterable.
        for (const char *k : { "pelvisThrust", "clubPath", "swingPlane", "shaftDirection",
                               "ballBodyDistance", "launchDirection" }) {
            const MetricDescriptor *d = cat.descriptor(QString::fromLatin1(k));
            check(d && d->baselineRequirement().dtlCamera,
                  k);
        }

        // And the reason a golfer sees for one names the camera, not the tier.
        ShotContext everything = wristShot({}, /*faceOn*/ true, /*club*/ true);
        everything.hasBallTrack = true;
        const QString why = cat.resolve(QStringLiteral("clubPath"), everything).reason;
        check(!why.contains(QStringLiteral("tier")), "clubPath's reason does not talk about tiers");
    }

    // 3d-quinquies. What a second camera would do — three answers authored, one derived.
    //
    // The derived one is the point. A face-on camera measures a PROJECTION, exact only while the
    // measured segment lies in the frontal plane, and a swing rotates the body out of it — so any
    // `Projected` rung read past Address is reading a foreshortened quantity. That is geometry, not
    // a fact about any one metric, and authoring it 27 times would be 27 copies of one sentence that
    // a 28th metric would then silently miss.
    {
        using SG = MetricDescriptor::StereoGain;
        const auto gain = [&cat](const char *k) {
            const MetricDescriptor *d = cat.descriptor(QString::fromLatin1(k));
            return d ? d->stereoGain() : SG::None;
        };

        check(gain("clubPath")  == SG::Unlocks,  "clubPath cannot be had without the second camera");
        check(gain("xFactor")   == SG::Improves, "xFactor has an authored stereo rung above the span");
        check(gain("shoulderPlaneAngle") == SG::Refines,
              "shoulderPlaneAngle is a projected line read at the Top — foreshortened, so refined");

        // The two families that genuinely escape, and they are the whole reason this is derived from
        // the phases rather than from the method alone.
        check(gain("toeLineAngle") == SG::None,
              "toeLineAngle is read at Address only — the golfer is square, the projection is exact");
        check(gain("ballPosition") == SG::None, "…as is ball position");
        check(gain("leadWristFlexExt") == SG::None, "an IMU reading owes a camera nothing");
        check(gain("lm.spinRate") == SG::None, "…nor does a launch-monitor reading");

        // attackAngle is Refines, and that is NOT in conflict with the design's correction that a
        // DTL camera is the one view which cannot measure it. Both hold: DTL ALONE puts the
        // target-line direction on its own optical axis, while a CALIBRATED PAIR recovers the 3D
        // velocity vector and removes the unknown depth component the projected reading carries.
        // Replacing the view and triangulating from both are different things.
        check(gain("attackAngle") == SG::Refines,
              "attackAngle is refined by triangulation, though not by a DTL view alone");

        int refines = 0;
        for (const MetricDescriptor *d : cat.all())
            if (d->stereoGain() == SG::Refines) ++refines;
        std::printf("    %d metrics carry projection error a calibrated pair would refine\n", refines);
        checkEqI(refines, 27, "27 projected readings taken past Address");
    }

    // 3d-quater. The upgrade hint — what more kit would buy, on a real shot.
    {
        const ShotContext cam = wristShot({}, /*faceOn*/ true);
        const MetricAvailability est = cat.resolve(QStringLiteral("pelvisRotation"), cam);
        check(est.routeId == QStringLiteral("faceOn"), "the camera rung is the one that fired");

        // THE BEST RUNG, NOT THE NEAREST — and pelvisRotation is the case that distinguishes them.
        // Two rungs sit above the camera: a stereo pair (planned, and skipped for that reason) and
        // a pelvis IMU. Even once the stereo producer lands, the hint must keep naming the IMU: it
        // is cheaper, measures the turn outright rather than triangulating two points, and works on
        // the one camera the owner already has. A nearest-rung walk would recommend a second camera
        // to every face-on owner, which is the purchase the design argues hardest against.
        check(est.upgrade.contains(QStringLiteral("Pelvis"))
                  && est.upgrade.contains(QStringLiteral("measure it directly")),
              "…and the shot is told a pelvis IMU would measure it directly");
        check(!est.upgrade.contains(QStringLiteral("down-the-line")),
              "…and NOT to go and buy a second camera, which is the weaker fix");

        ShotContext pelvisImu = wristShot({ SegmentRole::Pelvis }, /*faceOn*/ true);
        const MetricAvailability best = cat.resolve(QStringLiteral("pelvisRotation"), pelvisImu);
        check(best.routeId == QStringLiteral("pelvisImu"), "the IMU rung fires when it can");
        check(best.upgrade.isEmpty(), "…and nothing better is dangled, because there is nothing");

        // A HINT MAY NEVER ADVERTISE A ROUTE NOBODY BUILT. leadKneeFlexion's better rung is a DTL
        // camera and no producer reads it, so a golfer must not be told to go and buy one; the
        // catalogue-level upgradeDevices() DOES say so, because that answers a different question.
        const MetricAvailability knee = cat.resolve(QStringLiteral("leadKneeFlexion"), cam);
        check(knee.upgrade.isEmpty(), "no upgrade is offered towards an unbuilt route");
    }

    // 3e. Launch-monitor metrics — the connector landed, so this block asserts the opposite of
    // what it used to.
    //
    // It previously asserted the connector's ABSENCE: that nine metrics required the device AND
    // were planned, and were Unavailable even with `hasLaunchMonitor` set, because nothing could
    // read one. It said in as many words that it would flip when a connector arrived. It has.
    //
    // Two things changed together and both are checked here. The nine rungs are live. And every
    // reading is keyed `lm.`, including the ones nothing else could ever produce — because the six
    // quantities we ALSO estimate must keep their bare keys, or the ladder would resolve one winner
    // and the measurement would silently replace the estimate. Comparing the two is the reason to
    // own the device, so that replacement is the failure this block exists to prevent.
    {
        // Nothing but a device will ever measure these.
        const char *lmOnly[] = { "lm.faceAngle", "lm.faceToPath", "lm.spinRate", "lm.spinAxis",
                                 "lm.smashFactor", "lm.strikeLocation", "lm.carryDistance",
                                 "lm.dynamicLoft", "lm.spinLoft", "lm.lieAngle", "lm.closureRate",
                                 "lm.strikeHeight", "lm.backSpin", "lm.sideSpin",
                                 "lm.totalDistance", "lm.offline", "lm.peakHeight",
                                 "lm.descentAngle", "lm.distanceToPin" };
        // These we measure AND estimate. Both keys must exist, independently.
        const char *paired[] = { "clubheadSpeed", "attackAngle", "ballSpeed",
                                 "launchAngle", "launchDirection", "clubPath" };

        ShotContext capable = wristShot({ SegmentRole::Pelvis, SegmentRole::Thorax,
                                          SegmentRole::LeadForearm, SegmentRole::LeadHand },
                                        /*faceOn*/ true, /*club*/ true);
        capable.hasBallTrack = true;
        capable.tier = ReconstructionTier::ClubInstrumented;

        int requiresDevice = 0, needsFacet = 0, planned = 0,
            unavailableWithout = 0, measuredWith = 0, saysNeedsDevice = 0;
        for (const char *k : lmOnly) {
            const MetricDescriptor *d = cat.descriptor(QString::fromLatin1(k));
            if (!d) continue;
            if (d->baselineRequirement().launchMonitor) ++requiresDevice;
            for (CaptureDevice dev : captureDevicesFor(d->baselineRequirement()))
                if (dev == CaptureDevice::LaunchMonitor) ++needsFacet;
            if (d->planned()) ++planned;

            const MetricAvailability without = cat.resolve(QString::fromLatin1(k), capable);
            if (without.state == MetricAvailability::Unavailable) ++unavailableWithout;
            // The requirement is now a true statement with a purchase behind it: buying the device
            // really does produce the number, which is exactly what was NOT true before.
            if (without.reason.contains(QStringLiteral("launch monitor"))) ++saysNeedsDevice;

            ShotContext withLm = capable;
            withLm.hasLaunchMonitor = true;
            if (cat.resolve(QString::fromLatin1(k), withLm).state == MetricAvailability::Measured)
                ++measuredWith;
        }
        const int n = int(std::size(lmOnly));
        checkEqI(requiresDevice, n, "every lm. metric REQUIRES the device");
        checkEqI(needsFacet, n, "…so every one files under the Launch monitor chip");
        checkEqI(planned, 0, "…and none is planned any more — a connector reads them");
        checkEqI(unavailableWithout, n, "…Unavailable on a fully-equipped shot without a monitor");
        checkEqI(saysNeedsDevice, n, "…saying it needs one, which is now worth acting on");
        checkEqI(measuredWith, n, "…and Measured the moment a monitor reports the shot");

        // THE SEPARATION. A bare key and its lm. twin both exist, and the measured one does NOT
        // appear in the bare one's ladder — if it did, resolve() would hand back the device reading
        // under the estimate's name and the comparison would quietly become an identity.
        int bothExist = 0, bareUncontaminated = 0;
        for (const char *k : paired) {
            const MetricDescriptor *bare = cat.descriptor(QString::fromLatin1(k));
            const MetricDescriptor *meas = cat.descriptor(QStringLiteral("lm.") + QString::fromLatin1(k));
            if (bare && meas) ++bothExist;
            if (!bare) continue;
            bool anyDeviceRung = false;
            for (const MetricRoute &r : bare->routes)
                if (r.requirement.launchMonitor) anyDeviceRung = true;
            if (!anyDeviceRung) ++bareUncontaminated;
        }
        checkEqI(bothExist, int(std::size(paired)),
                 "each quantity we both measure and estimate has TWO keys");
        checkEqI(bareUncontaminated, int(std::size(paired)),
                 "…and no bare key has a launch-monitor rung that would supersede our own producer");

        // A monitor must not change the answer for anything it did not measure. Turning it on
        // improves the lm. metrics and touches nothing else.
        ShotContext withLm = capable;
        withLm.hasLaunchMonitor = true;
        int drifted = 0;
        for (const MetricDescriptor *d : cat.all()) {
            if (d->key.startsWith(QStringLiteral("lm."))) continue;
            if (cat.resolve(d->key, capable).state != cat.resolve(d->key, withLm).state) ++drifted;
        }
        checkEqI(drifted, 0, "connecting a monitor changes no metric outside the lm. namespace");
    }

    // 3e2. Every metric that can carry a direction says which way is positive.
    //
    // The obligation is docs/design/pinpoint_sign_conventions.md's: "a metric whose value carries a
    // direction MUST state which way is positive in its own MetricDescriptor". That document exists
    // because THREE SIGNALS SHIPPED INVERTED and none of them failed loudly — an inverted signal
    // fires happily on the wrong swings with correct-sounding consequence text attached. This is
    // that obligation as a test rather than as a request.
    //
    // signNegative MAY be empty: a carry, a spin rate or a duration cannot go negative, and forcing
    // prose onto that would invent a meaning. signPositive may not — every metric in a signable
    // unit has a direction, even the unsigned ones, whose direction is what the magnitude counts.
    {
        const QStringList signable = { QStringLiteral("°"), QStringLiteral("°/s"),
                                       QStringLiteral("mm"), QStringLiteral("cm"),
                                       QStringLiteral("in"), QStringLiteral("yd"),
                                       QStringLiteral("ft"), QStringLiteral("mph"),
                                       QStringLiteral("rpm"), QStringLiteral("ratio") };
        int silent = 0, glossOnly = 0;
        for (const MetricDescriptor *d : cat.all()) {
            const bool carriesDirection =
                signable.contains(d->unit) || d->unit.startsWith(QStringLiteral("%"));
            if (!carriesDirection) continue;
            if (d->signPositive.trimmed().isEmpty()) ++silent;

            // A WORLD-FRAME METRIC MUST NOT BE STATED ONLY AS A RIGHT-HANDED GLOSS. "in-to-out" and
            // "open" flip for a left-handed golfer; "right of the target line" does not. Naming the
            // gloss without the frame is the failure mode that misleads exactly half the readership,
            // and it looks authoritative while doing it.
            const QString sp = d->signPositive.toLower();
            const bool namesGloss = sp.contains(QStringLiteral("in-to-out"))
                                 || sp.contains(QStringLiteral("out-to-in"))
                                 || sp.contains(QStringLiteral("open for a right"));
            if (namesGloss && !sp.contains(QStringLiteral("target line"))) ++glossOnly;
        }
        checkEqI(silent, 0, "every metric that carries a direction says which way is positive");
        checkEqI(glossOnly, 0,
                 "…and no world-frame metric is stated only as a right-handed gloss");

        // The four ISB joint angles, pinned by name. Rule 0: a published standard outranks a
        // popular product, and a commercial sensor reports the inverse of us on bow/cup — so this
        // is the assertion that stops somebody "fixing" us to match it.
        struct IsbRow { const char *key; const char *mustContain; };
        const IsbRow isb[] = {
            { "leadWristFlexExt", "flexion" },
            { "leadWristRadUln",  "ulnar"   },
            { "forearmPronation", "pronation" },
            { "leadArmFlexion",   "flexion" },
        };
        int compliant = 0;
        for (const IsbRow &r : isb) {
            const MetricDescriptor *d = cat.descriptor(QString::fromLatin1(r.key));
            if (d && d->signPositive.toLower().contains(QString::fromLatin1(r.mustContain)))
                ++compliant;
        }
        checkEqI(compliant, 4, "the four ISB joint angles keep ISB polarity (Wu 2005, ref.wu2005)");
    }

    // 3f. The reading table and the manifest must agree.
    //
    // pinpoint::lm::fieldDefs() repeats each metric's label and unit so that writing a swing.json
    // does not drag the catalogue into src/Export. That duplication is only safe if something
    // fails when it drifts, and this is that something: a field added to LaunchMonitorReading with
    // no descriptor would otherwise be written into swing.json under a key nothing can render.
    {
        int missing = 0, labelDrift = 0, unitDrift = 0;
        for (const pinpoint::lm::FieldDef &f : pinpoint::lm::fieldDefs()) {
            const MetricDescriptor *d = cat.descriptor(QString::fromLatin1(f.key));
            if (!d) { ++missing; continue; }
            if (d->label != QString::fromUtf8(f.label)) ++labelDrift;
            if (d->unit  != QString::fromUtf8(f.unit))  ++unitDrift;
        }
        checkEqI(missing, 0, "every launch-monitor reading field has a descriptor");
        checkEqI(labelDrift, 0, "…with the same label");
        checkEqI(unitDrift, 0, "…and the same unit");

        // And the other direction: a descriptor claiming to come from a launch monitor that the
        // reader cannot actually fill would resolve Measured and then be permanently absent.
        int unfillable = 0;
        for (const MetricDescriptor *d : cat.all()) {
            if (!d->key.startsWith(QStringLiteral("lm."))) continue;
            bool found = false;
            for (const pinpoint::lm::FieldDef &f : pinpoint::lm::fieldDefs())
                if (d->key == QString::fromLatin1(f.key)) found = true;
            if (!found) ++unfillable;
        }
        checkEqI(unfillable, 0, "…and no lm. descriptor exists that no reading field can fill");
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

        // THREE foot-group keys need more than the feet, and all three need a ball. FootMetricProvider
        // used to be key-agnostic; these pin that it is not.
        check(cat.resolve(QStringLiteral("ballPosition"), feet).state == MetricAvailability::Unavailable,
              "ballPosition Unavailable with face-on camera but no ball track");
        ShotContext ballCtx = wristShot({}, /*faceOn*/ true);
        ballCtx.hasBallTrack = true;
        check(cat.resolve(QStringLiteral("ballPosition"), ballCtx).state == MetricAvailability::Measured,
              "ballPosition Measured with face-on camera + ball track");
        check(cat.resolve(QStringLiteral("stanceWidth"), ballCtx).state == MetricAvailability::Measured,
              "stanceWidth still Measured without needing a ball track");

        // stanceWidthMm and leadHeelLift are the two foot readings in real-world units, and the ball
        // diameter is the only ruler at the ground plane — foot_metrics.cpp emits neither without it.
        // stanceWidthMm was claimed by NO provider and so read Unavailable on every shot ever taken;
        // leadHeelLift was claimed but understated its requirement, so a ball-less shot was told it
        // was Measured while the producer had declined to emit it. Opposite mistakes, one root: the
        // availability answer has to match what the producer actually does.
        check(cat.resolve(QStringLiteral("stanceWidthMm"), ballCtx).state == MetricAvailability::Measured,
              "stanceWidthMm Measured with face-on camera + ball track");
        check(cat.resolve(QStringLiteral("stanceWidthMm"), feet).state == MetricAvailability::Unavailable,
              "stanceWidthMm Unavailable without the ball-diameter ruler");
        check(cat.resolve(QStringLiteral("leadHeelLift"), ballCtx).state == MetricAvailability::Measured,
              "leadHeelLift Measured with face-on camera + ball track");
        check(cat.resolve(QStringLiteral("leadHeelLift"), feet).state == MetricAvailability::Unavailable,
              "leadHeelLift Unavailable without the ball-diameter ruler (it reads in cm)");

        // Tempo needs no devices at all beyond something that segmented the swing —
        // an IMU-only shot with no camera and no club must still resolve Measured.
        const ShotContext imuOnly = wristShot({ SegmentRole::LeadForearm, SegmentRole::LeadHand },
                                              /*faceOn*/ false, /*club*/ false);
        check(cat.resolve(QStringLiteral("tempoRatio"), imuOnly).state == MetricAvailability::Measured,
              "tempoRatio Measured on an IMU-only shot (no camera, no club)");
        check(cat.resolve(QStringLiteral("tempoBackswing"), noCam).state == MetricAvailability::Measured,
              "tempoBackswing Measured with no devices bound at all");
        check(cat.descriptor(QStringLiteral("tempoRatio"))->planned() == false,
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
