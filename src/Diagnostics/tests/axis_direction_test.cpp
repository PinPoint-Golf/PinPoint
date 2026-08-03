// Every corridor signal points the way its own condition claims.
//
// This is the test that makes the direction audit non-recurring. A signal whose direction is
// inverted does not fail loudly — it fires happily on the wrong swings, with correct-sounding
// consequence text attached, and looks exactly like a working detector. Three of these shipped in
// the seed pack and none was caught by anything.
//
// The fixture below is the authority: it states, per signal, which end of its measure means the
// condition is PRESENT, and why. Adding a corridor signal without adding a row here fails the test
// — a new signal cannot slip in unaudited.
//
//   cmake --build build/analyzer-tests --target axis_direction_test
//   ctest --test-dir build/analyzer-tests -R axis_direction --output-on-failure

#include "../characteristic_pack.h"

#include <QFile>

#include <cstdio>
#include <map>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// Expected direction per signal, with the sign convention that decides it. Every `why` quotes the
// metric catalogue, not an opinion — where the catalogue does not state a convention, the signal is
// listed as UNDEFINED below rather than guessed at.
struct Expect {
    const char *signalId;
    Direction   direction;
    const char *why;
};

static const Expect kExpected[] = {
    // ── setup ───────────────────────────────────────────────────────────────
    { "sig_postureUpright",   Direction::Low,
      "spineForwardBend is 'the forward tilt of the trunk'; upright is less of it" },
    { "sig_ballTooClose",     Direction::Low,   "ballBodyDistance: 'higher means further away'" },
    { "sig_ballTooFar",       Direction::High,  "ballBodyDistance: 'higher means further away'" },
    { "sig_alignmentOpen",    Direction::Low,
      "shoulderAlignment: 'open is negative and closed is positive' — the club-path convention" },
    { "sig_alignmentClosed",  Direction::High,  "shoulderAlignment: closed is the positive end" },
    // Ball position follows the OUTSIDE convention (0 % lead heel .. 100 % trail heel), not the
    // lead-positive one — see docs/design/pinpoint_sign_conventions.md rule 1. So forward is LOW.
    { "sig_ballForward",      Direction::Low,
      "ballPosition: '0 % is level with the lead heel'; forward is the low end" },
    { "sig_ballBack",         Direction::High,
      "ballPosition: '100 % with the trail heel', so higher is further back" },
    { "sig_stanceWide",       Direction::High,  "stanceWidth: 'higher is wider'" },
    { "sig_stanceNarrow",     Direction::Low,   "stanceWidth: 'higher is wider'" },
    { "sig_sPosture",         Direction::High,  "lumbarExtension: 'higher means more arched'" },
    { "sig_cPosture",         Direction::High,  "thoracicFlexion: 'higher means more rounded'" },

    // ── backswing / top ─────────────────────────────────────────────────────
    { "sig_reverseSpine",     Direction::Low,
      "secondaryAxisTilt: players 'set roughly 6-8 deg at address and increase it'; reverse is too little" },
    { "sig_lossOfWidth",      Direction::Low,   "leadHandWidth: 'lower means narrower'" },
    { "sig_flatShoulderPlane",Direction::Low,   "shoulderPlaneAngle: 'lower means flatter'" },
    { "sig_flyingElbow",      Direction::High,
      "trailElbowHeight: 'higher means the elbow has risen further above the shoulder line'" },
    // Moved off pelvisLift, which is the pelvis CENTRE rising, onto the hip LINE. One hip riding up
    // over the other and the whole pelvis lifting evenly are different observations, and the second
    // is what pelvisLift measures — so a hike and an even lift were being read off one number.
    { "sig_trailHipHike",     Direction::High,
      "hipLineTilt: 'POSITIVE MEANS THE TRAIL HIP SITS ABOVE THE LEAD HIP'; a hike is more of it" },
    // The lead knee working IN toward the trail leg is movement AWAY from the lead side, so the
    // fault is the LOW tail — the same trap sway sits in two rows below, and the same reason the
    // descriptor states its sign in capitals.
    { "sig_leadKneeDriftIn",  Direction::Low,
      "leadKneeDrift: 'POSITIVE IS TOWARD THE LEAD SIDE, so a lead knee working inward toward the "
      "trail leg reads NEGATIVE'" },
    { "sig_pelvisRiseBackswing", Direction::High,
      "pelvisLift: 'HIGHER MEANS THE PELVIS HAS RISEN'" },
    { "sig_xfactorDeficit",   Direction::Low,
      "xFactorStretch: 'roughly 5 deg of added stretch is typical'; a deficit is less" },

    // ── transition / downswing ──────────────────────────────────────────────
    { "sig_transitionRush",   Direction::Low,
      "tempoRatio: 'a quick, snatchy transition dropping it well below 3:1'" },
    { "sig_casting",          Direction::Low,
      "lagAngle: 'a larger retained angle ... means more stored lag'; casting spends it" },
    { "sig_earlyExtension",   Direction::High,
      "pelvisThrust: 'a rising, toward-ball trace through the downswing IS early extension'" },
    { "sig_overTheTop",       Direction::Low,
      "clubPath: 'in-to-out (+) or out-to-in (-)'; over the top delivers out-to-in" },
    { "sig_forwardLunge",     Direction::High,
      "thoraxLateralDrift: 'positive is toward the lead side'" },
    { "sig_sway",             Direction::Low,
      "pelvisSway: 'positive is toward the lead side'; sway goes AWAY from it, so negative" },
    { "sig_slide",            Direction::High,
      "pelvisSway: a slide runs toward the lead side in the downswing" },
    { "sig_hangingBack",      Direction::Low,
      "pelvisSway: hanging back is NOT having moved to the lead side by impact" },

    // ── impact / follow-through ─────────────────────────────────────────────
    { "sig_scooping",         Direction::Low,
      "leadWristFlexExt: '+ is bowed/flexed, - is cupped/extended'; scooping ADDS loft, so cupped" },
    { "sig_insufficientSet",  Direction::Low,
      "leadWristRadUln is the HINGE; less set is less of it" },
    { "sig_lossOfPosture",    Direction::Low,
      "spineForwardBend: losing posture is standing UP, the negative deviation from address" },
    { "sig_chickenWing",      Direction::High,
      "leadArmToTorso: 'a rising angle there means the arm is separating from the body'" },
    { "sig_lateBuckle",       Direction::High,  "leadKneeFlexion: 'higher means more bend'" },

    // ── the other two alignment lines ───────────────────────────────────────
    // Same convention as the shoulder line, and stated in each descriptor rather than inferred from
    // its neighbour: an author reading only the feet row must be able to settle the tail from it.
    { "sig_feetAlignmentOpen",   Direction::Low,
      "feetAlignment: 'OPEN IS NEGATIVE AND CLOSED IS POSITIVE'" },
    { "sig_feetAlignmentClosed", Direction::High,
      "feetAlignment: closed is the positive end" },
    { "sig_hipAlignmentOpen",    Direction::Low,
      "hipAlignment: 'OPEN IS NEGATIVE AND CLOSED IS POSITIVE'" },
    { "sig_hipAlignmentClosed",  Direction::High,
      "hipAlignment: closed is the positive end" },

    // ── content extension: setup ────────────────────────────────────────────
    { "sig_kneeFlexExcess",       Direction::High, "leadKneeFlexion: 'higher means more bend'" },
    { "sig_kneeFlexInsufficient", Direction::Low,  "leadKneeFlexion: 'higher means more bend'" },

    // ── content extension: backswing ────────────────────────────────────────
    { "sig_insideTakeaway",       Direction::Low,
      "shaftDirection: 'POSITIVE POINTS RIGHT OF THE TARGET … outside in the takeaway'; inside is left" },
    { "sig_outsideTakeaway",      Direction::High, "shaftDirection: outside is the positive end" },
    { "sig_earlyFaceRoll",        Direction::High,
      "m_leadForearmRot_p2 highMeans: 'the face opening in the takeaway'" },
    { "sig_steepBackswingPlane",  Direction::High,
      "m_shaftPlaneBackswing highMeans: 'climbing further above the plane it started on — steeper'" },
    { "sig_flatBackswingPlane",   Direction::Low,  "the other end of that same range" },
    { "sig_acrossTheLine",        Direction::High,
      "shaftDirection: 'across the line at the top' is the positive, right-of-target end" },
    { "sig_laidOff",              Direction::Low,  "…and laid off is the negative, left-of-target end" },
    { "sig_overswing",            Direction::High,
      "shaftAngleVsHorizontal: 'POSITIVE IS PAST PARALLEL and negative is short of it'" },
    { "sig_bentLeadArm",          Direction::High,
      "m_leadElbowFlex_p4 highMeans: 'more bend in the lead elbow at the top'" },
    { "sig_trailKneeStraighten",  Direction::Low,
      "trailKneeFlexion: 'HIGHER MEANS MORE BEND'; straightening is losing it" },
    { "sig_headDropBackswing",    Direction::Low,
      "headLift measures a rise from address; a drop is the negative end" },
    { "sig_headRiseBackswing",    Direction::High, "…and a rise is the positive end" },
    { "sig_excessiveHeadSway",    Direction::High,
      "m_headSwayBack highMeans: 'the head further from the ball line, off the ball'" },
    { "sig_excessiveHeelLift",    Direction::High, "leadHeelLift measures a lift; more is more" },
    { "sig_shortBackswing",       Direction::Low,
      "thoraxRotation at the top: a short backswing is LESS turn" },
    { "sig_disconnection",        Direction::High,
      "leadUpperArmToChest: 'HIGHER MEANS A LARGER GAP — the arm further from the chest'" },

    // ── content extension: transition and downswing ─────────────────────────
    { "sig_steepDownswingShaft",  Direction::High,
      "m_shaftPlaneDelivery highMeans: 'a steeper delivery plane, the shaft higher above the "
      "address plane'" },
    { "sig_underPlaneStuck",      Direction::Low,  "…and stuck under the plane is the other end" },
    { "sig_overTheTop",           Direction::High,
      "m_transitionPlaneShift highMeans: 'the shaft moved OUTSIDE the plane it was on at the top'. "
      "Over the top IS that outward move — a DIFFERENT event from sig_steepDownswingShaft above, "
      "which reads the plane at P6: a golfer can be steep at delivery from a steep BACKSWING "
      "without ever re-routing the club outward in transition" },
    { "sig_shallowing",           Direction::Low,
      "…and the shaft dropping under that plane is the other end. Not a fault — it is the move good "
      "players make on purpose, which is why the condition on this tail is kind Delivery" },
    { "sig_hipSpinOut",           Direction::High,
      "pelvisRotation at P5: spinning out is the pelvis ALREADY further open in early downswing" },
    { "sig_hipStall",             Direction::Low,
      "m_pelvisRotRateP6P7 highMeans: 'still turning hard into impact rather than stalling'" },
    { "sig_deceleration",         Direction::Low,
      "m_handSpeedP6P7 highMeans: 'the hands still accelerating into the ball'" },

    // ── content extension: impact ───────────────────────────────────────────
    { "sig_insufficientShaftLean", Direction::Low,
      "impactShaftLean: more lean is the positive end, so not enough of it is the low one" },
    { "sig_lowPointBehind",       Direction::Low,
      "lowPointAhead: ahead of the ball is positive, so behind it is negative" },
    // The catalogue's convention is the strike direction, NOT steepness — so "too steep" is the LOW
    // tail. This is precisely the inversion `highMeans` exists to prevent: the condition's name and
    // the metric's sign point opposite ways, and an author matching the words would get it wrong.
    { "sig_attackTooSteep",       Direction::Low,
      "attackAngle: 'HIGHER MEANS A MORE UPWARD STRIKE', so steeper is the low end" },
    { "sig_attackTooShallow",     Direction::High, "…and shallower or upward is the high end" },
    { "sig_openFaceToPath",       Direction::High,
      "faceToPath: 'POSITIVE MEANS THE FACE IS OPEN TO THE PATH'" },
    { "sig_closedFaceToPath",     Direction::Low,  "…and negative means closed" },
    { "sig_hipsClosedImpact",     Direction::Low,
      "pelvisRotation at impact: closed is LESS open, the low end" },
    { "sig_axisTiltImpactLow",    Direction::Low,
      "secondaryAxisTilt: more tilt away from the target is positive, so too little is the low end" },
    { "sig_axisTiltImpactHigh",   Direction::High, "…and too much is the high end" },

    // ── content extension: finish ───────────────────────────────────────────
    { "sig_offBalanceFinish",     Direction::High,
      "comOverLeadFoot: 'HIGHER MEANS FURTHER FROM THE LEAD ANKLE', so off balance is the high end" },
    { "sig_weightBackFinish",     Direction::Low,
      "pelvisSway: 'positive is toward the lead side'; still back at the finish is negative" },
    { "sig_abbreviatedFinish",    Direction::Low,
      "thoraxRotation at the finish: a cut-short follow-through is LESS turn" },

    // ── content extension: ball flight and strike ───────────────────────────
    // Every one of these reads a metric whose descriptor states its sign in capitals, because the
    // outcome layer is where a wrong tail would be most visible and least questioned: a golfer who
    // is told they pull it will believe it.
    { "sig_pull",          Direction::Low,
      "launchDirection: 'POSITIVE IS RIGHT OF THE TARGET', so a pull is the low end" },
    { "sig_push",          Direction::High, "…and a push is the high end" },
    { "sig_launchLow",     Direction::Low,  "launchAngle: 'HIGHER MEANS A HIGHER LAUNCH'" },
    { "sig_launchHigh",    Direction::High, "…and ballooning is the high end of the same range" },
    { "sig_ballSpeedDeficit", Direction::Low, "ballSpeed: 'HIGHER IS FASTER'; a deficit is less" },
    { "sig_slice",         Direction::High,
      "spinAxis: 'POSITIVE TILTS RIGHT … a fade or a slice'" },
    { "sig_hook",          Direction::Low,  "…and negative tilts left" },
    { "sig_strikeToe",     Direction::High, "strikeLocation: 'POSITIVE IS TOWARD THE TOE'" },
    { "sig_strikeHeel",    Direction::Low,  "…negative toward the heel" },
    { "sig_smashDeficit",  Direction::Low,
      "smashFactor: 'HIGHER IS A MORE EFFICIENT STRIKE'" },
    { "sig_spinExcess",    Direction::High, "spinRate: 'HIGHER IS MORE SPIN'" },
    { "sig_spinDeficit",   Direction::Low,  "…and a knuckleball is the low end" },
    { "sig_carryDeficit",  Direction::Low,  "carryDistance: 'HIGHER IS FURTHER'" },

    // ── unwatched tails: the second tail of a corridor that already graded ──
    //
    // Every row below closes a tail that was producing a colour on the dashboard with no fault
    // behind it. They are the most inversion-prone rows in the fixture, because each one is the
    // OPPOSITE of a signal that already shipped and reads naturally as "the same thing again" —
    // so each `why` quotes the measure's own highMeans and then says which end of it this is.

    // Second signals on conditions that already existed (resolution C).
    { "sig_hangingBackPelvisDown",  Direction::Low,
      "m_pelvisSwayDown highMeans 'the pelvis further toward the lead side during the downswing'; "
      "hanging back is the pelvis NOT going there, the low end" },
    { "sig_slidePelvisImpact",      Direction::High,
      "m_pelvisSwayImpact highMeans 'the pelvis further toward the lead side at impact than at "
      "address'; a slide is too much of it" },
    { "sig_offBalanceFinishSway",   Direction::High,
      "m_pelvisSwayFinish highMeans 'the pelvis further toward the target at the finish'; falling "
      "through it is the high end, where sig_weightBackFinish takes the low" },
    { "sig_hangingBackThoraxDrift", Direction::Low,
      "m_thoraxDrift highMeans 'the chest further toward the lead side by early downswing'; the "
      "chest staying back is the low end, where sig_forwardLunge takes the high" },

    // Setup, posture and lateral.
    { "sig_postureTooBent",         Direction::High,
      "m_spineBendAtAddress highMeans 'more forward bend from the hips, standing over the ball "
      "more'; too bent is the high end, where sig_postureUpright takes the low" },
    { "sig_flatLumbarSpine",        Direction::Low,
      "m_lumbarCurve highMeans 'a more arched lower back'; a flat lumbar spine is less arch" },
    { "sig_flatThoracicSpine",      Direction::Low,
      "m_thoracicCurve highMeans 'a more rounded upper back'; a flat thoracic spine is less round" },
    { "sig_diving",                 Direction::High,
      "m_spineBendLoss highMeans 'more forward bend than at address during the swing, a dip'; "
      "diving IS that dip, so it is the high end" },
    { "sig_steepShoulderPlane",     Direction::High,
      "m_shoulderPlane highMeans 'a steeper, more vertical shoulder turn'; steep is the high end, "
      "where sig_flatShoulderPlane takes the low" },
    { "sig_excessiveAxisTiltTop",   Direction::High,
      "m_axisTiltAtTop highMeans 'more tilt away from the target at the top'; excessive tilt is the "
      "high end, where sig_reverseSpine (too little) takes the low" },
    { "sig_backingOffTheBall",      Direction::Low,
      "m_pelvisThrustDown highMeans 'the pelvis further toward the ball during the downswing'; "
      "backing away is the pelvis going the other way, the low end" },
    { "sig_pelvisDriftLeadBackswing", Direction::High,
      "m_pelvisSwayBack highMeans 'the pelvis further toward the lead side during the backswing'; "
      "drifting lead-side going back is the high end, where sig_sway (toward the trail side) takes "
      "the low" },
    { "sig_pelvisSinkBackswing",    Direction::Low,
      "m_pelvisLiftTop highMeans 'the pelvis higher than at address by the top'; sinking is lower "
      "than at address, the low end, where sig_trailHipHike takes the high" },
    { "sig_headDriftLeadBackswing", Direction::Low,
      "m_headSwayBack highMeans 'the head further from the ball line, off the ball' — i.e. toward "
      "the TRAIL side; drifting toward the target is the low end" },

    // Arms and club.
    { "sig_inToOutPath",            Direction::High,
      "m_clubPathAtImpact highMeans 'a more in-to-out path through impact', so in-to-out is the "
      "high end" },
    { "sig_outToInPath",            Direction::Low,
      "…and out-to-in is the low end of the same range. This is the tail sig_overTheTop used to "
      "read: the path is measured, the over-the-top MOVE is now inferred from it" },
    { "sig_trailElbowDeep",         Direction::Low,
      "m_trailElbowRise highMeans 'the trail elbow higher above the shoulder line'; an elbow behind "
      "and below the body is the low end, where sig_flyingElbow takes the high" },
    { "sig_overRotationAtTop",      Direction::High,
      "m_thoraxRotP4 highMeans 'a bigger shoulder turn at the top'; over-turning is the high end, "
      "where sig_shortBackswing takes the low" },
    { "sig_clubShortOfParallel",    Direction::Low,
      "m_shaftAngleP4 highMeans 'the shaft further past parallel — a longer backswing'; short of "
      "the top is the low end, where sig_overswing takes the high" },
    { "sig_faceHeldShutTakeaway",   Direction::Low,
      "m_leadForearmRot_p2 highMeans 'the lead forearm rotated further away from the target — the "
      "face opening in the takeaway'; a face held shut is less of that rotation, the low end" },
    { "sig_armsOverConnected",      Direction::Low,
      "m_leadUpperArmToChest highMeans 'the lead arm running further from the chest — less "
      "connected'; arms pinned to the chest is the low end, where sig_disconnection takes the high" },
    { "sig_lockedLeadArm",          Direction::Low,
      "m_leadElbowFlex_p4 highMeans 'more bend in the lead elbow at the top'; an arm locked straight "
      "is the least bend, the low end, where sig_bentLeadArm takes the high" },

    // Release, impact and the transition stretch.
    { "sig_excessiveLag",           Direction::High,
      "m_lagAngleDown highMeans 'more angle retained between the lead arm and the shaft in the "
      "downswing'; holding it too long is the high end, where sig_casting takes the low" },
    { "sig_overSet",                Direction::High,
      "m_leadWristRadUln_p4 highMeans 'more wrist set at the top, the club hinged further up from "
      "address'; over-set is the high end, where sig_insufficientSet takes the low" },
    { "sig_bowedLeadWrist",         Direction::High,
      "m_leadWristAtImpact highMeans 'a more bowed lead wrist at impact, less cupped'; bowed is the "
      "high end, where sig_scooping (cupped) takes the low" },
    { "sig_excessiveShaftLean",     Direction::High,
      "m_impactShaftLean highMeans 'the hands further ahead of the clubhead at impact — more "
      "forward lean'; too much lean is the high end, where sig_insufficientShaftLean takes the low" },
    { "sig_lowPointTooFarAhead",    Direction::High,
      "m_lowPointAhead highMeans 'the arc bottoming out further ahead of the ball, toward the "
      "target'; too far ahead is the high end, where sig_lowPointBehind takes the low" },
    { "sig_hipsTooOpenAtImpact",    Direction::High,
      "m_pelvisRotP7 highMeans 'the pelvis further open at impact'; over-cleared is the high end, "
      "where sig_hipsClosedImpact takes the low" },
    { "sig_latePelvisRotation",     Direction::Low,
      "m_pelvisRotP5 highMeans 'the pelvis already further open in early downswing'; a pelvis slow "
      "to start is the low end, where sig_hipSpinOut (too early) takes the high" },
    { "sig_excessiveSeparationStretch", Direction::High,
      "m_xFactorStretch highMeans 'more separation added between chest and pelvis through "
      "transition'; stretching past control is the high end, where sig_xfactorDeficit takes the low" },
    { "sig_faceHeldOpenImpact",     Direction::High,
      "m_leadForearmRot_p7 highMeans 'the face arriving open'; holding it off IS that high end, "
      "where sig_flipping takes the low — the impact-side mirror of the P2 pair" },
    { "sig_faceRolledShutImpact",   Direction::Low,
      "m_leadForearmRot_p7 highMeans 'the lead forearm rotated further away from the target at "
      "impact — the face arriving open'; rolling the hands over turns it the other way, so the face "
      "arriving SHUT "
      "is the low end. Same convention its P2 sibling uses, where sig_earlyFaceRoll takes the high "
      "and sig_faceHeldShutTakeaway the low" },
    { "sig_hipsUnderRotatedTop",    Direction::Low,
      "m_pelvisRotP4 highMeans 'a deeper hip turn at the top'; not getting round is the low end. "
      "The high end of THIS axis is deliberately unclaimed — turning too far at the top is a chest "
      "fault and sig_overRotationAtTop already holds it on thorax_rotation_top" },
};

// Signals whose metric states NO sign convention anywhere, so their direction cannot be checked
// against anything. These are not passes — they are a standing debt, and the count is asserted so
// it can only go down. Every one is on a `planned` metric, so the convention is still free to be
// chosen; it must be written into the descriptor when the producer is.
// Empty, and it must stay that way: every metric carrying a corridor signal now states which way
// is positive. A new entry here is a metric that shipped without saying, which is exactly how the
// three inversions got in.
static const char *kUndefinedConvention[] = { nullptr };

// Signals wrong in a way a DIRECTION cannot express. Both original entries are fixed:
// sig_insufficientSet now reads the hinge (leadWristRadUln) rather than bow/cup, and
// m_spineBendLoss takes the minimum rather than the maximum. Empty, and it should stay that way.
static const char *kKnownDefective[] = { nullptr };

static bool listed(const char *const *arr, size_t n, const QString &id)
{
    for (size_t i = 0; i < n; ++i)
        if (id == QLatin1String(arr[i])) return true;
    return false;
}

int main()
{
    QFile f(QStringLiteral(PP_CORE_PACK_PATH));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("  [FAIL] cannot open %s\n", PP_CORE_PACK_PATH);
        return 1;
    }
    const PackLoadResult res = loadPack(f.readAll(), QStringLiteral("core.json"));
    check(res.parsed, "the shipped pack parses");
    const CharacteristicPack &p = res.pack;

    std::map<QString, Direction> expect;
    for (const Expect &e : kExpected)
        expect[QLatin1String(e.signalId)] = e.direction;

    std::printf("=== every corridor signal points the way its condition claims ===\n");
    {
        int checked = 0, wrong = 0, unaudited = 0;
        for (const Signal &s : p.signalDefs) {
            if (s.test != SignalTest::OutsideCorridor) continue;

            if ((kUndefinedConvention[0] != nullptr
                 && listed(kUndefinedConvention, std::size(kUndefinedConvention), s.id))
                || (kKnownDefective[0] != nullptr
                    && listed(kKnownDefective, std::size(kKnownDefective), s.id)))
                continue;

            const auto it = expect.find(s.id);
            if (it == expect.end()) {
                ++unaudited;
                std::printf("        '%s' has no row in the fixture — audit it\n", qPrintable(s.id));
                continue;
            }
            ++checked;
            if (!s.direction.has_value() || *s.direction != it->second) {
                ++wrong;
                std::printf("        '%s' points %s, its condition needs %s\n", qPrintable(s.id),
                            s.direction ? qPrintable(directionName(*s.direction)) : "nowhere",
                            qPrintable(directionName(it->second)));
            }
        }
        std::printf("      %d audited\n", checked);
        check(wrong == 0, "every audited signal points the way its own condition claims");
        check(unaudited == 0, "no corridor signal is missing from the fixture");
    }

    std::printf("=== both tails of an axis point opposite ways ===\n");
    {
        // One norm, two conditions. If both tails pointed the same way they would fire together on
        // every deviation, and the axis would report a golfer as simultaneously too far forward and
        // too far back.
        std::map<QString, std::vector<const Signal *>> byAxis;
        for (const Condition &c : p.conditions) {
            if (c.axis.isEmpty()) continue;
            for (const QString &sid : c.detectedBy)
                if (const Signal *s = p.signal(sid); s && s->test == SignalTest::OutsideCorridor)
                    byAxis[c.axis].push_back(s);
        }

        int pairs = 0, bad = 0;
        for (const auto &[axis, sigs] : byAxis) {
            if (sigs.size() != 2) continue;
            ++pairs;
            if (!sigs[0]->direction || !sigs[1]->direction
                || *sigs[0]->direction == *sigs[1]->direction) {
                ++bad;
                std::printf("        axis '%s' has both tails pointing the same way\n",
                            qPrintable(axis));
            }
            if (sigs[0]->measures != sigs[1]->measures) {
                ++bad;
                std::printf("        axis '%s' reads two different measures\n", qPrintable(axis));
            }
        }
        std::printf("      %d two-tailed axes\n", pairs);
        check(pairs >= 4, "the known two-tailed axes are present");
        check(bad == 0, "each two-tailed axis reads ONE measure from opposite ends");
    }

    std::printf("=== the outstanding debt is tracked, not forgotten ===\n");
    {
        // These counts may only go DOWN. A rise means a new signal was added without a stated sign
        // convention, or a new defect was accepted.
        int undefined = 0, defective = 0;
        for (const Signal &s : p.signalDefs) {
            if (s.test != SignalTest::OutsideCorridor) continue;
            if ((kUndefinedConvention[0] != nullptr
                 && listed(kUndefinedConvention, std::size(kUndefinedConvention), s.id))) ++undefined;
            if (kKnownDefective[0] != nullptr
                && listed(kKnownDefective, std::size(kKnownDefective), s.id))            ++defective;
        }
        check(undefined == 0,
              "every signal's metric states which way is positive (must stay at zero)");
        check(defective == 0,
              "no signal is left wrong in a way direction cannot express (must stay at zero)");
    }

    std::printf("=== every signal-bearing measure says what HIGH means ===\n");
    {
        // The words an author reads instead of "High"/"Low". Without them the direction choice is
        // made against a convention the author has to remember correctly, which is how the three
        // inversions got in — so a measure carrying a corridor signal must carry these too.
        int measures = 0, silent = 0;
        for (const Signal &s : p.signalDefs) {
            if (s.test != SignalTest::OutsideCorridor) continue;
            for (const QString &mid : s.measures) {
                const Measure *m = p.measure(mid);
                if (m == nullptr) continue;
                ++measures;
                if (m->highMeans.isEmpty()) {
                    ++silent;
                    std::printf("        '%s' does not say what a high value means\n",
                                qPrintable(mid));
                }
            }
        }
        std::printf("      %d signal-bearing measure references\n", measures);
        check(silent == 0, "every measure a corridor signal reads says what HIGH means");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
