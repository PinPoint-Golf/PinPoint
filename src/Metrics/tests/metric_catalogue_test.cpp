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
        checkEqI(static_cast<int>(cat.all().size()), 70, "descriptor count == 70");
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
        checkEqI(countType(cat, MetricType::PointInTime), 26, "PointInTime count");
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
        checkEqI(planned, 25, "25 planned metrics — nothing produces them by any route");
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
        check(gain("spinRate") == SG::None, "…nor does a launch-monitor reading");

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

    // 3e. Launch-monitor metrics — a REQUIREMENT *and* a planned rung, which is the pair a route
    // can state and the old metric-level flag could not.
    //
    // This block used to assert the opposite: that these nine were NOT planned, and that they went
    // Measured the moment a connector set `hasLaunchMonitor`. The requirement half was and is right
    // — the hardware is the golfer's to own, and a metric that needs one must say so rather than
    // promising work. The rest was aspirational: **nothing in this build sets `hasLaunchMonitor`**,
    // no code outside a test even mentions it, so "needs a launch monitor" was telling a golfer to
    // buy a device that would change nothing. Both facts are now stated, on the same rung.
    //
    // When a connector lands, dropping `PLANNED` from those nine rungs is the whole change — and
    // this test flips back with it, which is the point of asserting the connector's absence rather
    // than assuming it.
    {
        const char *lm[] = { "faceAngle", "faceToPath", "spinRate", "spinAxis", "smashFactor",
                             "strikeLocation", "carryDistance", "dynamicLoft", "spinLoft" };

        ShotContext capable = wristShot({ SegmentRole::Pelvis, SegmentRole::Thorax,
                                          SegmentRole::LeadForearm, SegmentRole::LeadHand },
                                        /*faceOn*/ true, /*club*/ true);
        capable.hasBallTrack = true;
        capable.tier = ReconstructionTier::ClubInstrumented;

        int requiresDevice = 0, planned = 0, unavailableWithout = 0, saysPlanned = 0,
            stillUnavailableWith = 0, needsFacet = 0;
        for (const char *k : lm) {
            const MetricDescriptor *d = cat.descriptor(QString::fromLatin1(k));
            if (!d) continue;
            // THE HARDWARE FACT SURVIVES. This is what marking them planned used to destroy, and
            // what keeps them under the "Launch monitor" chip in the directory.
            if (d->baselineRequirement().launchMonitor) ++requiresDevice;
            for (CaptureDevice dev : captureDevicesFor(d->baselineRequirement()))
                if (dev == CaptureDevice::LaunchMonitor) ++needsFacet;
            if (d->planned()) ++planned;

            const MetricAvailability without = cat.resolve(QString::fromLatin1(k), capable);
            if (without.state == MetricAvailability::Unavailable) ++unavailableWithout;
            if (without.reason.contains(QStringLiteral("planned"))) ++saysPlanned;

            // And WITH a monitor reported, still unavailable — because we cannot read it. Anything
            // else here would be the catalogue claiming a number nobody supplied.
            ShotContext withLm = capable;
            withLm.hasLaunchMonitor = true;
            if (cat.resolve(QString::fromLatin1(k), withLm).state
                    == MetricAvailability::Unavailable)
                ++stillUnavailableWith;
        }
        checkEqI(requiresDevice, 9, "all 9 launch-monitor metrics still REQUIRE the device");
        checkEqI(needsFacet, 9, "…so all 9 still file under the Launch monitor chip");
        checkEqI(planned, 9, "…and all 9 are planned, because no connector reads one");
        checkEqI(unavailableWithout, 9, "…Unavailable on a fully-equipped shot without one");
        checkEqI(saysPlanned, 9, "…saying PLANNED, not 'go and buy a launch monitor'");
        checkEqI(stillUnavailableWith, 9,
                 "…and STILL Unavailable with one reported, which is the honest answer today");
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
