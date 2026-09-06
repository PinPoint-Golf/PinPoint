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

#include "metric_catalogue.h"

// The one place every descriptor is declared (design §3.3 / build order §3) — the full design
// catalogue (shot_analyzer_design.md §A). 86 descriptors.
//
// EVERY DESCRIPTOR DECLARES A ROUTE LADDER, and it is the only statement of what the metric needs.
// `.routes` is ordered BEST FIRST, and its LAST rung is the cheapest way to get the metric at all;
// both ends are read. The first satisfied live rung decides the shot's answer (Direct → Measured,
// Estimated → Bridged) and the last is what the directory reports as "needs". There is no
// `.requirement` and no `.planned` field any more: a flat requirement could only ever state the
// floor, so the rest of the ladder lived in provider C++ and in prose, and the two drifted.
//
// PRODUCED (45) — metric_extractor ×4, kinematic_series ×3 + shaft-lean, foot_metrics ×5 +
//   ball_position ×1, head_track ×3, lower_body_metrics ×6, upper_body_metrics ×9,
//   body_rotation ×4, club_delivery ×3, the trail-wrist series ×1, tempo_metrics ×2, and the two
//   wrist Summary scores.
//
// DEVICE (26) — the `lm.` readings a connected launch monitor supplies. Live, not planned: the
//   GC Quad connector reads them out of FSX2020's LastShot.CSV. They resolve Measured on a shot the
//   monitor reported and say "needs a launch monitor" otherwise, which is now a true statement with
//   a purchase behind it rather than a promise. See the Launch monitor block at the foot of this
//   file for why every one of them is `lm.`-prefixed even where nothing else could ever produce it.
//
// PLANNED (16) — every rung planned, so nothing produces them by any route. `.planned` on a ROUTE
//   rather than on a metric is the distinction that had been missing, and nine of these were
//   mis-stated because of it: `clubPath` is not work we owe, it is a metric that needs a camera
//   pointing down the target line, and saying "planned" about it promised a pipeline while hiding
//   the hardware. Now both are stated, and if a DTL capture path lands they resolve with no
//   catalogue change beyond dropping `PLANNED` from the rung.
//
//   They group by WHY, which is worth keeping legible because it is what a reader deciding what to
//   build next needs first — and the reason now sits on the route, next to the requirement it
//   explains, rather than in a list somewhere else that had to be kept in step:
//     · DEPTH — the face-on camera's blind axis (pelvisThrust, clubPath, swingPlane, shaftDirection,
//       launchDirection, ballBodyDistance). Requires `dtlCamera`.
//     · SAGITTAL — present in the image, foreshortened to noise by the frontal projection
//       (spineForwardBend, the two knee flexions). The knees keep their face-on rung, Estimated and
//       planned, because it says exactly why nobody reads them there.
//     · NO KEYPOINT — nothing sits between the shoulders and the hips in either pose layout
//       (thoracicFlexion, lumbarExtension), but the back CONTOUR of a DTL silhouette shows both.
//     · SENSORS WE DO NOT PLACE — hipInternalRotation.
//     · WORK WE HAVE NOT DONE — launchAngle / ballSpeed need a ball-FLIGHT tracker (the detector we
//       have is an at-spot presence tracker), kinematicSequence needs angular-SPEED series, and
//       swingScore needs a live adherence scorer. These four KEEP their planned rungs even though a
//       launch monitor now measures the same quantities, because `lm.ballSpeed` is a separate
//       metric and not a substitute — the point of owning the device is to check our estimate
//       against it, which needs both to exist.
//
// NO DESCRIPTOR AND NO PROVIDER MAY READ `sessionType`. Availability is about the shot's data and
//   devices; a session type is what the operator meant to capture, which is not evidence about what
//   was captured. See metric_providers.h.
//
// See the metric-catalogue developer guide for promoting a route to live.
//
// `description` (what it is + why it matters) and `howToRead` (sign, what good looks like, caveats)
// are written as coach-facing narratives for the directory detail page; a route's `summary` is the
// third voice and the shortest — how THIS rung gets the number, named as a method rather than as a
// missing device. The biomechanics, formulae and reference ranges draw on
// docs/design/shot_analyzer_design.md, docs/reference/wristmetrics.md,
// docs/reference/golf_swing_normative_reference.md and docs/reference/swing_json_schema.md.
//
// minTier stays Angles2D on every rung: wrist DOFs come from the fused IMU regardless of camera
// reconstruction tier, and the speed/foot metrics are 2D face-on. Depth is stated as the `dtlCamera`
// DEVICE, not as `minTier = Stereo3D` — three metrics used the tier as a proxy for it, which is a
// pipeline fact standing in for the thing the golfer would actually have to buy, and which no
// directory filter could read.

namespace pinpoint::analysis {

void installMetricManifest(MetricCatalogue &cat)
{
    using P  = Phase;
    using R  = SegmentRole;
    using RM = RouteMethod;
    constexpr RouteQuality Direct    = RouteQuality::Direct;      // → Measured when this rung fires
    constexpr RouteQuality Estimated = RouteQuality::Estimated;   // → Bridged
    constexpr bool         PLANNED   = true;                      // this ROUTE has no producer yet

    // ADDRESS → IMPACT (P1 → P7) — the frontal-plane family's phase domain, design §5.1's table.
    // Every metric that takes it is a LATERAL displacement or a LINE TILT read in the face-on
    // image, and past impact the body has turned far enough that the projection is measuring the
    // ROTATION rather than the quantity named. A hip line 66° out of the image plane still yields
    // an `atan2`, which is how a −88° tilt and a +35 % sway reached the screen looking like
    // measurements. `comOverLeadFoot` deliberately does NOT narrow: it is a distance ALONG the
    // stance line, which survives the turn, and it is read at the finish on purpose.
    constexpr PhaseDomain P1toP7{ Phase::Address, Phase::Impact };

    // One acquisition route. Spelling each rung as six designated initialisers buried the thing the
    // ladder exists to show — that a metric can be got several ways, best first — under its own
    // syntax, so it is a call instead, and a descriptor's `.routes` reads as a list of methods.
    const auto via = [](const char *id, RouteMethod method, RouteQuality quality,
                        MetricRequirement requirement, const QString &summary,
                        bool planned = false) -> MetricRoute {
        return { QString::fromLatin1(id), method, quality, std::move(requirement), summary, planned };
    };

    // ------------------------------------------------------------- Score (Summary, ScoreBreakdown)

    cat.addDescriptor({
        .key = QStringLiteral("wristScore"),
        .type = MetricType::Summary,
        .label = QStringLiteral("Wrist score"),
        .shortLabel = QStringLiteral("Wrist score"),
        .unit = QString(),
        .group = QStringLiteral("Score"),
        .description = QStringLiteral(
            "A single 0–100 summary of the lead-wrist motion for the shot. The assessment engine "
            "bands each lead-wrist checkpoint — bow/cup, hinge, roll and elbow, at the Top and at "
            "Impact — against its reference corridor, then rolls those results into one number. It "
            "is criterion-referenced: it measures how closely this swing matches an efficient "
            "reference model, not how it ranks against other golfers, so the same swing always "
            "earns the same score."),
        .howToRead = QStringLiteral(
            "Read it as a headline, then drill into the individual wrist metrics to see what moved "
            "it. Higher is closer to the reference; a low score points you at whichever checkpoint "
            "fell outside its band — most often a cupped lead wrist at the top. It needs the "
            "lead-forearm and lead-hand IMUs and is produced only in a Wrist Motion session."),
        .signPositive = QString(),
        .signNegative = QString(),
        .routes = {
            via("wristImus", RM::Inertial, Direct, { .imuRoles = { R::LeadForearm, R::LeadHand } },
                QStringLiteral("rolled up from the lead-wrist checkpoints the forearm and hand "
                               "IMUs measure")) },
        .usedBy = { QStringLiteral("review:verdict"),
                    QStringLiteral("shotlist:score") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("wristResemblance"),
        .type = MetricType::Summary,
        .label = QStringLiteral("Wrist resemblance"),
        .shortLabel = QStringLiteral("Pattern"),
        .unit = QString(),
        .group = QStringLiteral("Score"),
        .description = QStringLiteral(
            "A classification of the lead-wrist release pattern rather than a grade. For each of "
            "the three tour archetypes — bowed, neutral and cupped — it computes an independent "
            "0–100 resemblance from how close the wrist's flex/extension sits to that archetype's "
            "centres at the Top and Impact (R_p = 100·exp(−½·d_p²)). The scores are independent, so "
            "a clean bowed action reads e.g. bowed 86 / neutral 40 / cupped 8. None of the patterns "
            "is 'wrong' — they are all workable ways to deliver the club."),
        .howToRead = QStringLiteral(
            "The headline is the best-matching pattern (the highest of the three); the trio tells "
            "you how decisively. When the top two are within a few points the result is flagged "
            "'blended' — the player sits between styles. Use it to read a player's natural pattern "
            "before coaching toward or away from it. v1 scores lead-wrist flex/extension only; it "
            "needs the lead-forearm and lead-hand IMUs."),
        .signPositive = QString(),
        .signNegative = QString(),
        .routes = {
            via("wristImus", RM::Inertial, Direct, { .imuRoles = { R::LeadForearm, R::LeadHand } },
                QStringLiteral("scored from the lead-wrist flex/extension the forearm and hand "
                               "IMUs measure")) },
        .usedBy = { QStringLiteral("review:verdict") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("swingScore"),
        .type = MetricType::Summary,
        .label = QStringLiteral("Swing score"),
        .shortLabel = QStringLiteral("Swing score"),
        .unit = QString(),
        .group = QStringLiteral("Score"),
        .description = QStringLiteral(
            "The planned whole-swing counterpart to the wrist score: a single 0–100 rating of how "
            "closely the full-body action reproduces an idealised, efficient swing for the session "
            "type (Swing, GRF or Coach). Unlike the resemblance-based wrist score it is "
            "adherence-referenced — it rewards proximity to one efficient model rather than "
            "matching a chosen style."),
        .howToRead = QStringLiteral(
            "0–100, higher being closer to the reference action. This metric is a placeholder: the "
            "swing adherence scorer is not yet wired into a live analyzer, so no value is produced "
            "today. When it lands it will summarise the body-rotation, sequence and delivery "
            "metrics the way the wrist score summarises the wrist checkpoints."),
        .signPositive = QString(),
        .signNegative = QString(),
        .routes = {
            via("adherence", RM::Derived, Direct, { .faceOnCamera = true },
                QStringLiteral("the swing adherence scorer is not wired into a live analyzer, so "
                               "nothing produces it yet"), PLANNED) },
        .usedBy = { QStringLiteral("review:verdict"),
                    QStringLiteral("shotlist:score") },
    });

    // ---------------------------------------------------------------- Wrist & forearm (IMU, scored)

    cat.addDescriptor({
        .key = QStringLiteral("leadWristFlexExt"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead wrist — bow / cup"),
        .shortLabel = QStringLiteral("Bow/cup"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "The bow/cup axis of the lead wrist — how flexed (bowed, +) or cupped (extended, −) it "
            "is relative to address — from the fused forearm and hand IMUs (first Cardan component "
            "of q_forearm⁻¹·q_hand about the hand's medio-lateral axis). Of the three wrist motions "
            "this is the one that most directly shapes the clubface, which is why it carries the "
            "highest weight in the wrist score."),
        .howToRead = QStringLiteral(
            "+ is bowed/flexed (the strong, hands-forward look), − is cupped/extended. Read it at "
            "the Top and Impact: a good move bows the wrist through transition so impact sits "
            "roughly 15–30° more flexed than address, while a cupped top tends to leave the face "
            "open. Restricting this axis costs more clubhead speed than the others, so treat a "
            "cupping trend as a priority. Wrist Motion session; needs the lead-forearm and hand IMUs."),
        .flexPositive = true,
        .signPositive = QStringLiteral("flexion — the lead wrist bowed"),
        .signNegative = QStringLiteral("extension — the lead wrist cupped"),
        .phases = { P::Top, P::Impact },
        .scored = true,
        .routes = {
            via("wristImus", RM::Inertial, Direct, { .imuRoles = { R::LeadForearm, R::LeadHand } },
                QStringLiteral("the first Cardan axis of the forearm-to-hand rotation, from the "
                               "two IMU orientations")) },
        .usedBy = { QStringLiteral("assessment:wrist"),
                    QStringLiteral("chart:review"),
                    QStringLiteral("score:wrist"),
                    QStringLiteral("characteristic:scooping"),
                    QStringLiteral("characteristic:bowed_lead_wrist"),
                    QStringLiteral("characteristic:cupped_at_top"),
                    QStringLiteral("characteristic:bowed_at_top") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadWristRadUln"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead wrist — hinge"),
        .shortLabel = QStringLiteral("Hinge"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "The hinge (or 'cock') of the lead wrist — radial (−, toward the thumb) versus ulnar "
            "(+, toward the little finger) — as the second Cardan component of q_forearm⁻¹·q_hand "
            "about the dorsal-palmar axis. This is the axis that sets and stores wrist lag in the "
            "backswing and releases it through the strike."),
        .howToRead = QStringLiteral(
            "+ is ulnar (hinged/cocked), − is radial. A large ulnar value at the top is normal and "
            "desirable; better players hold less deviation into impact, where lower handicaps show "
            "noticeably less wandering. This is the least reliable IMU axis (~5° typical error), so "
            "trust the shape of the trend over any single value. Needs the lead-forearm and hand IMUs."),
        .flexPositive = true,
        .signPositive = QStringLiteral("ulnar deviation — the wrist hinged or cocked"),
        .signNegative = QStringLiteral("radial deviation"),
        .phases = { P::Top, P::Impact },
        .scored = true,
        .routes = {
            via("wristImus", RM::Inertial, Direct, { .imuRoles = { R::LeadForearm, R::LeadHand } },
                QStringLiteral("the second Cardan axis of the forearm-to-hand rotation, from the "
                               "two IMU orientations")) },
        .usedBy = { QStringLiteral("assessment:wrist"),
                    QStringLiteral("chart:review"),
                    QStringLiteral("score:wrist"),
                    QStringLiteral("characteristic:insufficient_set"),
                    QStringLiteral("characteristic:over_set") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("forearmPronation"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead forearm — roll"),
        .shortLabel = QStringLiteral("Roll"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "The roll of the lead forearm — pronation (+, palm rolling down) versus supination (−, "
            "palm rolling up) — as the axial twist of q_upperarm⁻¹·q_forearm about the elbow-to-"
            "wrist axis. It is the rotational component that helps square the clubface through "
            "impact, working together with the bow/cup axis."),
        .howToRead = QStringLiteral(
            "+ is pronated, − is supinated; through the strike the lead forearm rolls toward "
            "square. There is no published tour benchmark, so read it as a trend and as a matched "
            "pair with bow/cup — a player short on bow may compensate with roll, and vice versa. It "
            "needs the lead-forearm, hand and upper-arm IMUs (the upper-arm gives the forearm a "
            "reference to rotate against)."),
        .flexPositive = true,
        .signPositive = QStringLiteral("pronation — the lead forearm rolled toward face-down"),
        .signNegative = QStringLiteral("supination"),
        .phases = { P::Top, P::Impact },
        .scored = true,
        .routes = {
            via("wristImus", RM::Inertial, Direct,
                { .imuRoles = { R::LeadForearm, R::LeadHand, R::LeadUpperArm } },
                QStringLiteral("the twist of the forearm, taken between the upper-arm and hand "
                               "IMUs")) },
        .usedBy = { QStringLiteral("assessment:wrist"),
                    QStringLiteral("chart:review"),
                    QStringLiteral("score:wrist"),
                    QStringLiteral("characteristic:early_face_roll"),
                    QStringLiteral("characteristic:face_held_shut_takeaway"),
                    QStringLiteral("characteristic:open_face_top"),
                    QStringLiteral("characteristic:shut_face_top"),
                    QStringLiteral("characteristic:flipping"),
                    QStringLiteral("characteristic:face_held_open_impact") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadArmFlexion"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead elbow — flexion"),
        .shortLabel = QStringLiteral("Elbow"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "How bent the lead arm is at the elbow, reported as a flexion magnitude from the angle "
            "between the upper-arm and forearm segments (acos of their dot product) along the "
            "shoulder–elbow–wrist chain. A connected, structured swing keeps the lead arm long and "
            "relatively straight through the hitting area."),
        .howToRead = QStringLiteral(
            "0° is a perfectly straight arm; larger values mean more bend. A near-straight lead arm "
            "through impact is the goal. A chicken-wing — rising flexion into and past impact — "
            "usually signals an early release or a stalling body and shows up here as a growing "
            "value. It needs the lead-forearm, hand and upper-arm IMUs."),
        .flexPositive = true,
        .signPositive = QStringLiteral("elbow flexion — a more bent lead arm; 0° is straight"),
        .signNegative = QString(),
        .phases = { P::Top, P::Impact },
        .scored = true,
        .routes = {
            via("wristImus", RM::Inertial, Direct,
                { .imuRoles = { R::LeadForearm, R::LeadHand, R::LeadUpperArm } },
                QStringLiteral("the elbow angle between the upper-arm and forearm IMUs")) },
        .usedBy = { QStringLiteral("assessment:wrist"),
                    QStringLiteral("chart:review"),
                    QStringLiteral("score:wrist"),
                    QStringLiteral("characteristic:bent_lead_arm"),
                    QStringLiteral("characteristic:locked_lead_arm") },
    });

    // ── Forearm rotation — ONE SEGMENT, and not an ISB joint angle ───────────────────────────────
    //
    // ⚠ THIS IS NOT `forearmPronation` AND THE DISTINCTION IS THE WHOLE POINT. That one is the ISB
    // radioulnar angle, defined against the HUMERUS, and it needs an upper-arm sensor. This asks
    // the different and simpler question a lone forearm sensor can answer — how far has the forearm
    // TURNED since address — which the user identifies as the direct indicator of flipping through
    // impact. A rig wearing all three sensors produces both, and they are different quantities.
    //
    // ⚠ NO EXCEPTION TO RULE 0 IS INVOLVED HERE AND NONE SHOULD BE WRITTEN. ISB defines rotations
    // BETWEEN two segment triads; this is one segment twisting about its own long axis, which
    // pinpoint_sign_conventions.md's "what ISB does NOT govern" table names as a family in its
    // "Segment axial rotations" row. Rule 0 not applying means Rule 1 applies — follow the outside
    // world — so this metric is PRONATION-POSITIVE, AGREEING with the wG3's vendor exactly where
    // `leadWristFlexExt` deliberately disagrees with them. That asymmetry looks like a bug beside
    // the two of them and is not one: bow/cup has a published standard that outranks a product, and
    // a single segment's axial rotation does not.
    cat.addDescriptor({
        .key = QStringLiteral("forearmRotation"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead forearm — rotation from address"),
        .shortLabel = QStringLiteral("Rotation"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "How far the lead forearm has turned about its own long axis SINCE ADDRESS — the signed "
            "twist of q_address⁻¹·q_forearm about the elbow-to-wrist axis. It is a whole-segment "
            "rotation, not a joint angle: it needs only the forearm sensor and makes no claim about "
            "the elbow. Read the address-to-impact travel, which is the direct indicator of "
            "flipping — a hand-driven release turns the forearm through impact where a body-driven "
            "one carries it."),
        .howToRead = QStringLiteral(
            "0° is address, whatever posture address happened to be. + is the pronation direction "
            "(rolling toward palm-down), − is supination. Read the change between P1 and P7 rather "
            "than any single value: a large, late pronation into impact is flipping. ⚠ The ABSOLUTE "
            "value is meaningless across sessions or across instruments, because each sensor zeroes "
            "at its own calibration pose — that is exactly why this is stated from address, and why "
            "the travel is comparable when the raw angle is not. ⚠ This is TRAVEL along the path the "
            "forearm actually took, so it is NOT confined to ±180° and a reading past half a turn is "
            "not an error. The top of the backswing lands close to that half turn — around 150–180° "
            "on our own captures — so a swing that turns a little further reads 190°, and folding "
            "that back to −170° would be the mistake. Needs the lead-forearm IMU only."),
        .flexPositive = true,
        .signPositive = QStringLiteral("pronation — the lead forearm rolled toward face-down, "
                                       "measured as travel from address"),
        .signNegative = QStringLiteral("supination — the lead forearm rolled toward face-up"),
        .phases = { P::Top, P::Impact },
        .routes = {
            via("wristImus", RM::Inertial, Direct, { .imuRoles = { R::LeadForearm } },
                QStringLiteral("the axial twist of the forearm about its own long axis, referenced "
                               "to address, from one forearm IMU")) },
    });

    // ── The HackMotion rungs ────────────────────────────────────────────────────────────────────
    //
    // ⚠ NEW KEYS, NEVER OVERWRITTEN ONES, AND NOT A SECOND ROUTE ON THE EXISTING METRIC. A second
    // route would make the pair NOT separately addressable, and separate addressability is the only
    // thing that keeps "which instrument graded this swing" recoverable after the fact. The bare
    // keys always mean our own estimate; these always mean the device's. `Measure::preferKeys` is
    // where a measure states which it would rather have — authored, not inferred.
    //
    // ⚠ THESE PUBLISH IN ISB POLARITY, AND THAT IS NOT A COPY-PASTE SLIP. The wG3 reports the
    // INVERSE of ISB on bow/cup, but the frame map applied upstream (hm_frame.h, candidate C2)
    // already inverts the device's own sense, so what arrives at this key is ISB-conformant. A
    // reader comparing our number against the vendor's app will see opposite signs on bow/cup and
    // agreeing signs on rotation, and both of those are correct.
    //
    // ⚠ THE ARITHMETIC IS THE SAME ARITHMETIC. Both instruments run one MetricExtractor path
    // (metric_extractor.cpp) so any difference between them is the instrument and not the maths.
    // With one instrument per swing that is a property of the code and nothing else can demonstrate
    // it, which is why it is stated here as well as there.
    cat.addDescriptor({
        .key = QStringLiteral("hm.leadWristFlexExt"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead wrist — bow / cup (measured)"),
        .shortLabel = QStringLiteral("Bow/cup (HM)"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "The bow/cup axis of the lead wrist, measured by a HackMotion wG3 rather than by our "
            "own forearm-and-hand IMUs. The twin of `leadWristFlexExt`, computed by the identical "
            "decomposition from the device's two units, and the reference that says whether our own "
            "wrist estimate is right."),
        .howToRead = QStringLiteral(
            "Read exactly as `leadWristFlexExt`: + is bowed/flexed, − is cupped/extended, and impact "
            "should sit meaningfully more flexed than address. ⚠ Reported in ISB polarity, which is "
            "the INVERSE of what the wG3's own application shows on this axis — a correct reading "
            "here looks wrong beside their screen, deliberately. Needs a HackMotion wG3."),
        .flexPositive = true,
        .signPositive = QStringLiteral("flexion — the lead wrist bowed"),
        .signNegative = QStringLiteral("extension — the lead wrist cupped"),
        .phases = { P::Top, P::Impact },
        .routes = {
            via("hackMotion", RM::Device, Direct, { .hackMotion = true },
                QStringLiteral("the bow/cup axis of the wrist, read from a HackMotion wG3")) },
        // The measured rung of the m_leadWristFlexExt_* family, which prefers this over our own
        // `leadWristFlexExt`, and so carries the same names.
        .usedBy = { QStringLiteral("assessment:wrist"),
                    QStringLiteral("chart:review"),
                    QStringLiteral("score:wrist"),
                    QStringLiteral("characteristic:scooping"),
                    QStringLiteral("characteristic:bowed_lead_wrist"),
                    QStringLiteral("characteristic:cupped_at_top"),
                    QStringLiteral("characteristic:bowed_at_top") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("hm.leadWristRadUln"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead wrist — hinge (measured)"),
        .shortLabel = QStringLiteral("Hinge (HM)"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "The hinge (or 'cock') of the lead wrist, measured by a HackMotion wG3 rather than by "
            "our own forearm-and-hand IMUs. The twin of `leadWristRadUln`, computed by the identical "
            "decomposition from the device's two units."),
        .howToRead = QStringLiteral(
            "Read exactly as `leadWristRadUln`: + is ulnar (hinged/cocked), − is radial. This is our "
            "least reliable axis on our own sensors, so the device's reading of it is the more "
            "interesting of the two. Needs a HackMotion wG3."),
        .flexPositive = true,
        .signPositive = QStringLiteral("ulnar deviation — the wrist hinged or cocked"),
        .signNegative = QStringLiteral("radial deviation"),
        .phases = { P::Top, P::Impact },
        .routes = {
            via("hackMotion", RM::Device, Direct, { .hackMotion = true },
                QStringLiteral("the hinge axis of the wrist, read from a HackMotion wG3")) },
        .usedBy = { QStringLiteral("assessment:wrist"),
                    QStringLiteral("chart:review"),
                    QStringLiteral("score:wrist"),
                    QStringLiteral("characteristic:insufficient_set"),
                    QStringLiteral("characteristic:over_set") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("hm.forearmRotation"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead forearm — rotation from address (measured)"),
        .shortLabel = QStringLiteral("Rotation (HM)"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "How far the lead forearm has turned about its own long axis since address, measured by "
            "a HackMotion wG3's lower-arm unit. The twin of `forearmRotation`, by the identical "
            "definition — one segment, referenced to address, and never the ISB radioulnar angle, "
            "which this device cannot measure at all for want of an upper-arm unit."),
        .howToRead = QStringLiteral(
            "Read exactly as `forearmRotation`: 0° is address, + is the pronation direction, and the "
            "P1-to-P7 travel is the flipping indicator. ⚠ Unlike the bow/cup axis, this one AGREES "
            "in sign with the wG3's own application — there is no published standard for a single "
            "segment's axial rotation, so we follow the convention the instruments already use. "
            "Needs a HackMotion wG3."),
        .flexPositive = true,
        .signPositive = QStringLiteral("pronation — the lead forearm rolled toward face-down, "
                                       "measured as travel from address"),
        .signNegative = QStringLiteral("supination — the lead forearm rolled toward face-up"),
        .phases = { P::Top, P::Impact },
        .routes = {
            via("hackMotion", RM::Device, Direct, { .hackMotion = true },
                QStringLiteral("the axial twist of the forearm since address, read from a "
                               "HackMotion wG3's lower-arm unit")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("trailWristFlexExt"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Trail wrist — bow / cup"),
        .shortLabel = QStringLiteral("Trail bow/cup"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Wrist & forearm"),
        .description = QStringLiteral(
            "How much the trail wrist is bent back (extension / cup) or forward (flexion / bow), "
            "the mirror of the lead-wrist face angle. The two wrists work as a pair: the trail "
            "wrist's cup at the top is what the lead wrist's bow has to answer, so reading only one "
            "side tells half the story of what the clubface is doing."),
        .howToRead = QStringLiteral(
            "POSITIVE IS EXTENSION (CUP), negative is flexion — the OPPOSITE polarity to the lead "
            "wrist, and not an inconsistency: the two hands are mirror images, so seen face-on one "
            "signed image-plane angle means flexion on the lead side and extension on the trail "
            "side. The trail wrist typically cups going back, reaching around 45° at the top, and "
            "retains some of that cup deep into the downswing. Read as a change from address. "
            "This is an APPARENT camera-plane angle rather than an anatomical one — a trail-side "
            "hand and forearm IMU would measure the true wrist angle, and the rig does not carry "
            "one — so read it for shape and change rather than as a clinical number."),
        // flexPositive = FALSE: the stored sign is extension-positive. The lead wrist stores
        // flexion-positive, and the two differ because the hands are mirror images seen from the
        // same side — see howToRead. The seven pack corridors are seated on this polarity
        // (m_trailWristFlexExt_p4 at +45°, which is the trail wrist CUPPING at the top).
        .flexPositive = false,
        .signPositive = QStringLiteral(
            "extension — the trail wrist cupped, the opposite of the lead wrist's polarity because "
            "the hands are mirror images"),
        .signNegative = QStringLiteral("flexion — the trail wrist bowed"),
        .phases = { P::Top, P::Impact },
        // LIVE from the face-on pose (pose_wrist_angle_source.cpp buildTrailWristSeries), as an
        // APPARENT camera-plane angle. It is not the anatomical DOF: SegmentRole still has no
        // trail-side arm roles, and PpJointDof no trail-side member, so this is deliberately a
        // METRIC and not a wrist-assessment DOF — adding one would pull the assessment engine, the
        // DOF metadata table and the reference bands into a change that produces one curve.
        //
        // It additionally needs the WholeBody hand keypoints, which MetricRequirement cannot state
        // (there is no pose-layout field, and adding one to every descriptor to serve a single
        // metric is the wrong trade). The producer refuses a 17-keypoint track outright instead.
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the apparent hand-against-forearm angle in the face-on image "
                               "plane, from the WholeBody hand keypoints")) },
        .usedBy = { QStringLiteral("assessment:wrist") },
    });

    // -------------------------------- Body rotation (LIVE — body_rotation.cpp; IMU or face-on estimate)

    cat.addDescriptor({
        .key = QStringLiteral("pelvisRotation"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Pelvis rotation"),
        .shortLabel = QStringLiteral("Pelvis turn"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Body rotation"),
        .description = QStringLiteral(
            "How far the pelvis has turned about the body's vertical axis relative to address — the "
            "engine of the swing's rotational power. It is taken from the pelvis IMU as the pelvis "
            "medio-lateral axis projected into the horizontal plane, so it isolates true axial turn "
            "from sway or tilt."),
        .howToRead = QStringLiteral(
            "Read at the Top and Impact. As a guide the pelvis reaches roughly 45° of turn at the "
            "top and is already re-rotating to about 35–45° open by impact — the pelvis leading the "
            "chest open is a hallmark of an efficient downswing. THE NUMBER IS A MAGNITUDE OF "
            "TURN AWAY FROM ADDRESS, so it is positive at the top and positive again at impact and "
            "passes through zero as the body squares up. With a pelvis IMU bound this is measured "
            "directly; with only a face-on camera it is ESTIMATED from the collapse of the hip "
            "span in the image, which is honest but weakest near square — read small values with "
            "the stated uncertainty rather than at face value."),
        .signPositive = QStringLiteral(
            "turn away from address — a MAGNITUDE, so positive at the top and again at impact, "
            "passing through zero as the body squares"),
        .signNegative = QString(),
        .phases = { P::Top, P::Impact },
        // THE WORKED LADDER. A pelvis IMU measures the turn; a face-on camera estimates it from the
        // hip span. Both rungs are live, both are stated here, and everything downstream follows —
        // the shot resolves Measured or Bridged off whichever fires, a camera-only swing is told an
        // IMU would measure it directly, and the directory can be filtered on either. This used to
        // be an if/else in BodyRotationProvider with a flat `.requirement` beside it naming only the
        // camera, so the catalogue could see the floor and never the rung above it.
        .routes = {
            via("pelvisImu", RM::Inertial, Direct, { .imuRoles = { R::Pelvis } },
                QStringLiteral("measured directly from the pelvis IMU")),
            via("faceOn+dtl", RM::Triangulated, Direct,
                { .faceOnCamera = true, .dtlCamera = true },
                QStringLiteral("the hip line's bearing, triangulated from the calibrated pair — "
                               "the turn read off geometry instead of inferred from foreshortening"), PLANNED),
            via("faceOn", RM::Projected, Estimated, { .faceOnCamera = true },
                QStringLiteral("estimated from the collapse of the hip span in the face-on image")) },
        .usedBy = { QStringLiteral("characteristic:hip_spin_out"),
                    QStringLiteral("characteristic:hip_stall"),
                    QStringLiteral("characteristic:hips_closed_at_impact"),
                    QStringLiteral("characteristic:sequence_order"),
                    QStringLiteral("characteristic:hips_too_open_at_impact"),
                    QStringLiteral("characteristic:late_pelvis_rotation"),
                    QStringLiteral("characteristic:hips_under_rotated_at_top") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("thoraxRotation"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Thorax rotation"),
        .shortLabel = QStringLiteral("Chest turn"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Body rotation"),
        .description = QStringLiteral(
            "How far the chest (thorax) has turned about the vertical axis relative to address, "
            "from the thorax IMU. Together with pelvis rotation it defines the body's coil and how "
            "the upper body unwinds into the ball."),
        .howToRead = QStringLiteral(
            "Read at the Top and Impact; the shoulders typically reach around 90° of turn at the "
            "top of a full swing. The relationship between chest and pelvis turn — how much the "
            "chest outruns the pelvis going back, and how the pelvis leads coming down — is where "
            "the power story lives (see X-factor). A MAGNITUDE OF TURN from address, measured "
            "from a thorax IMU when one is bound and otherwise estimated from the collapse of the "
            "shoulder span in the face-on image. A full shoulder turn sits near the top of what "
            "that estimate can resolve, which is why the corridor over it is wide."),
        .signPositive = QStringLiteral("turn away from address — a MAGNITUDE, so positive at the top and again at impact"),
        .signNegative = QString(),
        .phases = { P::Top, P::Impact },
        .routes = {
            via("thoraxImu", RM::Inertial, Direct, { .imuRoles = { R::Thorax } },
                QStringLiteral("measured directly from the thorax IMU")),
            via("faceOn+dtl", RM::Triangulated, Direct,
                { .faceOnCamera = true, .dtlCamera = true },
                QStringLiteral("the shoulder line's bearing, triangulated from the calibrated "
                               "pair — a real reading, though the IMU stays authoritative"), PLANNED),
            via("faceOn", RM::Projected, Estimated, { .faceOnCamera = true },
                QStringLiteral("estimated from the collapse of the shoulder span in the face-on "
                               "image")) },
        .usedBy = { QStringLiteral("characteristic:abbreviated_finish"),
                    QStringLiteral("characteristic:sequence_order"),
                    QStringLiteral("characteristic:short_backswing"),
                    QStringLiteral("characteristic:over_rotation_at_top") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("xFactor"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("X-factor"),
        .shortLabel = QStringLiteral("X-factor"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Body rotation"),
        .description = QStringLiteral(
            "The separation between the chest and the pelvis — thorax turn minus pelvis turn — "
            "which stretches the trunk and stores elastic energy at the top of the backswing. It "
            "is the most talked-about power number in the modern swing; note that a shoulder-vs-"
            "pelvis measure reads roughly twice the pure spine value, so the method matters."),
        .howToRead = QStringLiteral(
            "Read at the top of the backswing. Tour players commonly show around 40–42° of "
            "separation (TPI), but more is not automatically better — it has to be separation the "
            "player can actually use going down. Pair it with X-factor stretch, which captures how "
            "much the gap grows early in the downswing. It is the difference of the two turn "
            "magnitudes, so it inherits the weaker of them: with no trunk IMUs both halves are "
            "camera estimates and the separation carries both uncertainties."),
        .signPositive = QStringLiteral("the chest turned further than the pelvis"),
        .signNegative = QStringLiteral("the pelvis turned further than the chest"),
        .phases = { P::Top },
        .routes = {
            via("trunkImus", RM::Inertial, Direct, { .imuRoles = { R::Pelvis, R::Thorax } },
                QStringLiteral("the difference of two directly measured turns")),
            via("faceOn+dtl", RM::Triangulated, Direct,
                { .faceOnCamera = true, .dtlCamera = true },
                QStringLiteral("both bearings triangulated from the calibrated pair, so the "
                               "separation no longer inherits two foreshortening estimates"), PLANNED),
            via("faceOn", RM::Projected, Estimated, { .faceOnCamera = true },
                QStringLiteral("estimated from both spans in the face-on image, so it inherits "
                               "the weaker half")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("xFactorStretch"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("X-factor stretch"),
        .shortLabel = QStringLiteral("X-stretch"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Body rotation"),
        .description = QStringLiteral(
            "The extra chest-over-pelvis separation gained at the very start of the downswing — the "
            "peak X-factor in early downswing minus the X-factor at the top. This stretch-shorten "
            "spike is the trunk loading against a pelvis that has already begun to unwind, and it "
            "predicts clubhead speed better than the static top-of-backswing X-factor."),
        .howToRead = QStringLiteral(
            "Look for a positive spike through transition into early downswing; roughly 5° of added "
            "stretch is typical, and skilled players add proportionally more. A player who reaches "
            "the top with big separation but no stretch is not using the coil — the fix is "
            "sequencing, not more turn. Measured against the X-factor at the top, so it needs a "
            "segmented top of backswing; without one it is absent rather than anchored to an "
            "arbitrary instant."),
        .signPositive = QStringLiteral("separation still growing after the top — the stretch"),
        .signNegative = QStringLiteral("separation already unwinding at the top"),
        .phases = { P::Transition, P::ArmParallelDown },
        .routes = {
            via("trunkImus", RM::Inertial, Direct, { .imuRoles = { R::Pelvis, R::Thorax } },
                QStringLiteral("the measured separation, less its value at the Top")),
            via("faceOn+dtl", RM::Triangulated, Direct,
                { .faceOnCamera = true, .dtlCamera = true },
                QStringLiteral("the triangulated separation, less its value at the Top"), PLANNED),
            via("faceOn", RM::Projected, Estimated, { .faceOnCamera = true },
                QStringLiteral("the estimated separation less its value at the Top, so both "
                               "spans' noise carries through")) },
        .usedBy = { QStringLiteral("characteristic:xfactor_deficit"),
                    QStringLiteral("characteristic:excessive_separation_stretch") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("hipInternalRotation"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Hip internal rotation"),
        .shortLabel = QStringLiteral("Hip rotation"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Body rotation"),
        .description = QStringLiteral(
            "True rotation at the hip joints — each thigh turning axially relative to the pelvis — "
            "as opposed to how far the pelvis as a whole has turned. It is what lets a player load "
            "into the trail hip going back and clear the lead hip coming down; limited hip internal "
            "rotation is a common physical restriction behind sway and early extension."),
        .howToRead = QStringLiteral(
            "Read per side at the Top and Impact; amplitudes of roughly 50° on the lead hip and 40° "
            "on the trail hip are typical references. Restricted rotation on one side often forces "
            "a compensation elsewhere in the chain, so this is as much a physical-screening tool as "
            "a swing metric. Planned: needs a pelvis IMU plus thigh IMUs."),
        .signPositive = QStringLiteral("internal rotation of that hip"),
        .signNegative = QStringLiteral("external rotation"),
        .phases = { P::Top, P::Impact },
        .routes = {
            via("pelvisThighImus", RM::Inertial, Direct,
                { .imuRoles = { R::Pelvis, R::LeadThigh, R::TrailThigh } },
                QStringLiteral("the thigh-against-pelvis twist — thigh IMUs we do not place "
                               "today, and no producer for it"), PLANNED) },
    });

    // ------------------------ Spine & tilt, then Pelvis & lateral
    //                          (part LIVE — lower_body_metrics.cpp / upper_body_metrics.cpp)
    //
    // These were one "Spine & pelvis" group and are now two, along the line between an ANGLE of the
    // trunk and a TRANSLATION of the centre. The split is not cosmetic: the chart's metric presets
    // are derived from `.group` (ChartMetrics::seriesGroups), so a group is also the unit a reader
    // plots together, and ten members made the one useful group unreadable. Angles and displacements
    // also rarely share a y-axis honestly — side bend and axis tilt are degrees about the same
    // corridor, whereas sway/lift/drift are all % stance width.
    //
    // hipLineTilt is named for an angle but lives with the LATERAL half deliberately: it is a pelvis
    // reading, it is measured in the same frontal plane as sway and lift, and it is read alongside
    // them. Grouping is by what a coach reads together, not by unit.

    cat.addDescriptor({
        .key = QStringLiteral("spineForwardBend"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Spine forward bend"),
        .shortLabel = QStringLiteral("Fwd bend"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Spine & tilt"),
        .description = QStringLiteral(
            "The forward tilt of the trunk over the ball — the flexion/extension of the thorax "
            "relative to the pelvis — which sets the posture the whole swing rotates around. Losing "
            "this angle (standing up) or adding to it (dipping) through the downswing changes the "
            "low point and the strike."),
        .howToRead = QStringLiteral(
            "Read at Address and Impact; the aim is to retain most of the address posture, with "
            "irons commonly holding around 30–40° into impact. A loss of forward bend into impact "
            "is early extension and pairs with the pelvis-thrust metric. GAINING bend is a dip, and "
            "it is a TRANSITION event — read it between P5 and P6, where it happens, not across the "
            "downswing as a whole: a golfer who drops into it and then stands up by impact shows "
            "both, and a reading spanning the pair cancels them. Planned: needs pelvis and thorax "
            "IMUs (or a calibrated 3D camera)."),
        .signPositive = QStringLiteral("more forward bend from the hips"),
        .signNegative = QStringLiteral("standing taller than upright, which a swing does not reach"),
        .phases = { P::Address, P::Impact },
        .routes = {
            via("dtl", RM::Projected, Direct, { .dtlCamera = true },
                QStringLiteral("the trunk's hinge over the ball, seen square-on from down the "
                               "line"), PLANNED),
            via("trunkImus", RM::Inertial, Direct, { .imuRoles = { R::Pelvis, R::Thorax } },
                QStringLiteral("thorax-relative-pelvis flexion from the two trunk IMUs — "
                               "sagittal, so the face-on camera cannot stand in"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:posture_too_upright"),
                    QStringLiteral("characteristic:posture_too_bent"),
                    QStringLiteral("characteristic:diving") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("spineSideBend"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Spine side bend"),
        .shortLabel = QStringLiteral("Side bend"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Spine & tilt"),
        .description = QStringLiteral(
            "Lateral flexion of the trunk toward the trail side — the side-bend of the thorax "
            "relative to the pelvis — which naturally appears in the downswing as the trail "
            "shoulder works down and under. It is closely tied to attack angle and to hitting up or "
            "down on the ball."),
        .howToRead = QStringLiteral(
            "Read at Impact; at driver impact the thorax commonly shows around 32° of side bend "
            "versus about 10° at the pelvis. Too little side bend often goes with a steep, "
            "over-the-top delivery; too much can throw the low point behind the ball. Taken as "
            "the shoulder line against the hip line: with no keypoint between them, two segments "
            "that both exist is the honest reading of thorax-relative-to-pelvis, and the "
            "difference cancels the whole-body lean that secondary axis tilt already reports. "
            "POSITIVE IS SIDE BEND TOWARD THE TRAIL SIDE. Needs a face-on camera."),
        .signPositive = QStringLiteral("side bend toward the TRAIL side"),
        .signNegative = QStringLiteral("side bend toward the lead side"),
        .phases = { P::Impact },
        // Side bend is the shoulder line MINUS the hip line, so it inherits BOTH lines'
        // foreshortening and expires with them — read past impact it reports the turn. The
        // geometry, and why ten metrics share this domain, is on P1toP7 at the top of this
        // function; the nine below point back at it rather than repeat it.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("lateral trunk flexion, read in the frontal plane the face-on "
                               "camera sees square-on")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("secondaryAxisTilt"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Secondary axis tilt"),
        .shortLabel = QStringLiteral("Axis tilt"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Spine & tilt"),
        .description = QStringLiteral(
            "How much the spine leans away from the target at impact — the angle of the mid-hip-to-"
            "mid-shoulder line from vertical in the frontal (face-on) plane. It reflects the "
            "trail-side tilt that lets the club approach from the inside and, for the driver, on a "
            "slight upswing."),
        .howToRead = QStringLiteral(
            "READ AT THE TOP AND AT IMPACT, and read the PAIR — the two anchors answer different "
            "questions and the fault library depends on both. Players tend to set roughly 6–8° of "
            "tilt at address, hold some of it at the top, and increase it to about 20–25° by "
            "impact; a driver wants more of this than an iron. Too little tilt at impact on its "
            "own is the common half-signal and does not settle anything: early extension and a "
            "stalled pelvis flatten the same angle. Leaning TOWARD the target at the top, together "
            "with too little tilt at impact, is the reverse pivot — that conjunction is what "
            "identifies it, which is why `reverse_pivot` is detected from both anchors and neither "
            "alone. POSITIVE IS AWAY FROM THE TARGET — the one lateral channel that is "
            "trail-positive rather than lead-positive, because the quantity is named for the lean "
            "away from the target and inverting it would leave every sentence about it backwards. "
            "Needs a face-on camera."),
        .signPositive = QStringLiteral("the upper body leaning AWAY from the target — trail-side lean"),
        .signNegative = QStringLiteral("leaning toward the target"),
        // Top joined Impact when reverse_pivot landed. The top anchor was always read — three
        // conditions hang off m_axisTiltAtTop — and the descriptor said Impact only, so the metric
        // documented itself for one phase while the library graded it at two.
        .phases = { P::Top, P::Impact },
        // Frontal-plane trunk lean: see P1toP7.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the frontal spine vector against vertical, in the face-on image")) },
        .usedBy = { QStringLiteral("characteristic:excessive_axis_tilt_impact"),
                    QStringLiteral("characteristic:reverse_spine_p7"),
                    QStringLiteral("characteristic:reverse_spine_p4"),
                    QStringLiteral("characteristic:excessive_axis_tilt_top"),
                    QStringLiteral("characteristic:reverse_pivot"),
                    QStringLiteral("characteristic:coming_out_of_it") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("pelvisSway"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Pelvis sway"),
        .shortLabel = QStringLiteral("Sway"),
        .unit = QStringLiteral("% stance width"),
        .group = QStringLiteral("Pelvis & lateral"),
        .presets = { QStringLiteral("Plumb Bob") },
        .description = QStringLiteral(
            "How far the pelvis slides laterally relative to address — the linear partner to "
            "pelvis rotation — as a percentage of the golfer's own stance. POSITIVE IS TOWARD THE "
            "LEAD SIDE, negative away from it. A little pressure shift is powerful; too much slide "
            "replaces rotation and hurts consistency."),
        .howToRead = QStringLiteral(
            "Read near the top and at Impact. A good pattern goes slightly NEGATIVE in the "
            "backswing, away from the lead side, then positive through the downswing — a pressure "
            "shift — returning near or just past zero by impact. A large negative peak going back "
            "is sway and usually costs turn and centredness of strike; still negative at impact is "
            "hanging back. The scale is the golfer's own address stance rather than centimetres, "
            "which needs no ruler and asks the question a coach actually asks — how far across the "
            "stance, not how many centimetres across the room. Needs a face-on camera."),
        .signPositive = QStringLiteral("the pelvis moved toward the LEAD side"),
        .signNegative = QStringLiteral("moved away from the lead side"),
        .phases = { P::Top, P::Impact },
        // A lateral projection of a rotating pelvis: see P1toP7. This is the channel the
        // design's screenshot showed at +35 % past impact.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("lateral travel of the hip centre in the face-on image, against "
                               "the ground plane")) },
        .usedBy = { QStringLiteral("characteristic:hanging_back"),
                    QStringLiteral("characteristic:slide"),
                    QStringLiteral("characteristic:sway"),
                    // weight_back_at_finish / off_balance_finish were read off this curve AT
                    // THE FINISH until 2026-09-04; past impact the lateral projection is
                    // rotation, so the measure was deleted (domain = P1-P7) and both left.
                    QStringLiteral("characteristic:pelvis_drift_lead_backswing") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("pelvisThrust"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Pelvis thrust"),
        .shortLabel = QStringLiteral("Thrust"),
        .unit = QStringLiteral("cm"),
        .group = QStringLiteral("Pelvis & lateral"),
        .description = QStringLiteral(
            "How far the pelvis pushes toward the ball (along the line from the player to the ball) "
            "relative to address — the depth-axis partner to sway. A late, controlled move is "
            "normal, but thrusting toward the ball early is the mechanical definition of early "
            "extension, one of the most common amateur faults."),
        .howToRead = QStringLiteral(
            "Read in the downswing and at Impact; the pelvis should stay back over the toe-line and "
            "move toward the ball only late, if at all. A rising, toward-ball trace through the "
            "downswing is early extension and pairs with a loss of spine forward bend. This motion "
            "lives along the camera's optical axis, so it genuinely needs a down-the-line view — a "
            "lone face-on camera cannot resolve it. Planned."),
        .signPositive = QStringLiteral("the pelvis moved toward the ball"),
        .signNegative = QStringLiteral("moved away from the ball"),
        // TOP AS WELL AS THE DOWNSWING, and it is not a widening of scope. The pelvis moving
        // toward the ball on the way BACK is its own fault, and reading it needs its own
        // window: the downswing measure is the peak between P5 and P7 taken from address, so
        // a golfer who moves in going back and holds it scores as early extension without
        // having extended. See pelvis_thrust_backswing.
        .phases = { P::Top, P::ArmParallelDown, P::Impact },
        .routes = {
            via("dtl", RM::Triangulated, Direct, { .faceOnCamera = true, .dtlCamera = true },
                QStringLiteral("toward-and-away-from-the-ball travel lies along the face-on "
                               "camera's blind axis, so it takes the down-the-line view to see it "
                               "at all"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:early_extension"),
                    QStringLiteral("characteristic:backing_off_the_ball"),
                    QStringLiteral("characteristic:pelvis_thrust_backswing") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("pelvisLift"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Pelvis lift"),
        .shortLabel = QStringLiteral("Lift"),
        .unit = QStringLiteral("% stance width"),
        .group = QStringLiteral("Pelvis & lateral"),
        .description = QStringLiteral(
            "How much the pelvis rises or drops vertically relative to address, as a percentage of "
            "the golfer's own stance — the up/down component of pelvis motion. HIGHER MEANS THE "
            "PELVIS HAS RISEN. Some rise through impact is part of a powerful, ground-force-driven "
            "action; an uncontrolled early rise is another face of early extension."),
        .howToRead = QStringLiteral(
            "Read at Impact. A small, controlled rise as the player pushes off the ground is normal "
            "and even desirable; what you are watching for is an early or excessive lift that pulls "
            "the club off its path. Read it alongside pelvis thrust and spine forward bend. This is "
            "the pelvis CENTRE rising or sinking as a whole; one hip riding up above the other is a "
            "different quantity and has its own metric in hip line tilt. Needs a face-on camera."),
        .signPositive = QStringLiteral("the pelvis rose"),
        .signNegative = QStringLiteral("the pelvis dropped"),
        .phases = { P::Top, P::Impact },
        // Same projection, vertical half: see P1toP7.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("vertical travel of the hip centre in the face-on image, against "
                               "the ground plane")) },
        .usedBy = { QStringLiteral("characteristic:pelvis_sink_backswing"),
                    QStringLiteral("characteristic:pelvis_rise_backswing") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("hipLineTilt"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Hip line tilt"),
        .shortLabel = QStringLiteral("Hip tilt"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Pelvis & lateral"),
        .presets = { QStringLiteral("Plumb Bob") },
        .description = QStringLiteral(
            "The tilt of the line between the two hips, seen face-on. POSITIVE MEANS THE TRAIL HIP "
            "SITS ABOVE THE LEAD HIP. The trail hip riding upward at the top is the signature of a "
            "pelvis that hiked instead of turning — the golfer found the feeling of a backswing by "
            "lifting the trail side rather than by rotating over the trail leg."),
        .howToRead = QStringLiteral(
            "Read at the top. This is an ABSOLUTE angle rather than a change from address, because "
            "the figure it is read against is absolute: the trail hip sits somewhat above the lead "
            "hip at the top for everybody, and the question is how much. It is measured in the "
            "image plane, so a golfer standing at an angle to the camera will read differently from "
            "one square to it — read it alongside pelvis lift, which rises when the whole pelvis "
            "comes up rather than one side of it. Needs a face-on camera. IT IS ALSO THE HIP "
            "ALIGNMENT READING: at address and impact the same line answers open / square / closed, "
            "which is why `hipAlignment` is not a separate metric — one curve, sampled at different "
            "phases, and positive still means the trail hip sits higher."),
        .signPositive = QStringLiteral("the TRAIL hip sits above the lead hip"),
        .signNegative = QStringLiteral("the lead hip sits above the trail hip"),
        // THE WHOLE P1-P7 LADDER, not three instants. The hip line is read as a progression — level
        // at address, the trail hip riding up at the top, and back toward level through impact —
        // and the shape of that travel is the reading, which three points cannot draw. The
        // corridors still live at P1 and P4 where there are numbers to grade against; the rest are
        // shown and not graded, which is the honest state of them.
        .phases = { P::Address, P::ShaftParallelBack, P::MidBackswing, P::Top,
                    P::ArmParallelDown, P::Delivery, P::Impact },
        // The hip line foreshortens to noise as the pelvis turns: see P1toP7.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the hip line's angle to horizontal, in the face-on image")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:trail_hip_hike"),
                    QStringLiteral("characteristic:hip_alignment_open"),
                    QStringLiteral("characteristic:hip_alignment_closed") },
    });

    // The plumb bob: where the hips sit over the stance, and — with hipLineTilt above — the pair a
    // coach reads together at address. It keeps `Pelvis & lateral` as its group, because sway and
    // lift are what it is read AGAINST, and joins the cross-cutting "Plumb Bob" preset so the two
    // can be plotted together without either leaving its group.
    cat.addDescriptor({
        .key = QStringLiteral("plumbBobDistance"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Plumb bob"),
        .shortLabel = QStringLiteral("Plumb bob"),
        .unit = QStringLiteral("in"),
        .group = QStringLiteral("Pelvis & lateral"),
        .presets = { QStringLiteral("Plumb Bob") },
        .description = QStringLiteral(
            "Where the centre of the hips sits relative to the centre of the stance, measured along "
            "the stance line and seen face-on. POSITIVE MEANS THE HIPS ARE AHEAD OF THE STANCE "
            "CENTRE, toward the lead side. It is the setup reading a coach takes by dropping a line "
            "from the hips, and how far it should sit ahead depends on the club: about an inch or "
            "two with a wedge, half that with a mid-iron, and roughly an inch BEHIND centre with a "
            "driver, where the ball is forward and the upper body is tilted away from the target."),
        .howToRead = QStringLiteral(
            "Read it first at address, against the corridor for the club in hand — the same number "
            "means different things with a wedge and a driver, and the corridors differ "
            "accordingly. Then read the travel: the hips move toward the lead side through the "
            "downswing on every good swing, so the value at impact is larger than at address, and "
            "the shift between them is the number that says whether the golfer got onto the lead "
            "side at all. ⚠ IT IS AN ESTIMATE TO A FRACTION OF AN INCH, NOT A MEASUREMENT. The "
            "inches come from the ball's own diameter used as a ruler, which is exact at the "
            "ball's depth on the ground and the hips are about a metre above that — a camera not "
            "at hip height carries a small scale bias. ⚠ AND THE MID-SWING READINGS CARRY A TURN "
            "ARTEFACT: turning the pelvis moves the APPARENT hip centre sideways in a face-on image "
            "with no actual shift, so address and impact are the trustworthy ends of the ladder "
            "and the readings between them overstate the travel. Needs a face-on camera and a "
            "detected ball; without the ball there is no ruler and the metric is absent rather "
            "than reported in some other unit."),
        .signPositive = QStringLiteral("the hips sit AHEAD of the stance centre, toward the lead side"),
        .signNegative = QStringLiteral("the hips sit behind centre, toward the trail side"),
        .phases = { P::Address, P::ShaftParallelBack, P::MidBackswing, P::Top,
                    P::ArmParallelDown, P::Delivery, P::Impact },
        // The hip centre over the stance, in the same frontal plane: see P1toP7.
        .domain = P1toP7,
        .routes = {
            via("faceOnBall", RM::Projected, Direct,
                { .faceOnCamera = true, .ballTrack = true },
                QStringLiteral("the hip centre's offset from the ankle-line centre along the stance "
                               "line in the face-on image, in the ball's own diameter ruler")) },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadKneeDrift"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead knee drift"),
        .shortLabel = QStringLiteral("Knee drift"),
        .unit = QStringLiteral("% stance width"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How far the lead knee has travelled sideways RELATIVE TO ITS OWN HIP, as a percentage "
            "of the golfer's stance. POSITIVE IS TOWARD THE LEAD SIDE, so a lead knee working "
            "inward toward the trail leg reads NEGATIVE. Read at the top, it separates a golfer who "
            "turned into the trail hip from one who found the same feeling by letting the lead knee "
            "collapse across."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the reading that matters is at the top, and a large negative value "
            "there is the observation. It is a DIFFERENCE — the knee's sideways travel minus its "
            "own hip's — and that is the whole point of the metric rather than a detail of it. "
            "Pelvic rotation carries the lead hip toward the trail side as seen from the front, so "
            "the raw knee position moves the same way whether the golfer turned deeply or did not "
            "turn at all; subtracting the hip is what tells those two apart. It cannot separate "
            "them perfectly, since hip and knee sit at different distances from the axis the pelvis "
            "turns about, so treat a reading here as an observation that points at a cause rather "
            "than as the cause itself — the down-the-line view and a physical hip screen are what "
            "settle it. Needs a face-on camera."),
        .signPositive = QStringLiteral("the lead knee moved toward the LEAD side"),
        .signNegative = QStringLiteral("moved toward the trail side — working inward"),
        // IMPACT AS WELL, and for the same reason pelvisThrust gained Top: the knee falling
        // inward THROUGH impact is a different fault from it working in at the top, and the
        // model carried only the second. See lead_knee_valgus_impact.
        .phases = { P::Top, P::Impact },
        // Lateral knee travel, same projection: see P1toP7.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("lateral travel of the lead knee in the face-on image, against the "
                               "ground plane")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:lead_knee_drifts_in_at_top"),
                    QStringLiteral("characteristic:lead_knee_valgus_impact") },
    });

    // ------------------------------------------------------- Club & speed (face-on club track, 2D)

    cat.addDescriptor({
        .key = QStringLiteral("clubheadSpeed"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Clubhead speed"),
        .shortLabel = QStringLiteral("Club speed"),
        .unit = QStringLiteral("mph"),
        .group = QStringLiteral("Club & speed"),
        .description = QStringLiteral(
            "How fast the clubhead is travelling as it arrives at the ball — the hands' velocity plus "
            "the shaft's rotation about them, composed from the tracked club and scaled to "
            "real-world units by the club's length. It is the headline power number and the "
            "biggest single driver of distance."),
        .howToRead = QStringLiteral(
            "Read the value at impact, which is where the curve peaks; the curve ends there because "
            "the ball takes a quarter of the club's speed in the next two frames. As widely-published references, tour "
            "drivers run about 113 mph and 7-irons about 89 mph, but the right number is club- and "
            "player-dependent. On a single face-on camera this is an in-plane estimate, so treat "
            "motion along the depth axis as approximate. Needs face-on club tracking."),
        .signPositive = QStringLiteral("a faster clubhead"),
        .signNegative = QString(),
        .phases = { P::Impact },
        // Address→Impact, like the frontal-plane family, but for a different reason: the
        // clubhead's speed is DISCONTINUOUS at contact (an iron gives the ball a quarter of it in
        // two frames), and every reducer here is a window ABOUT an instant — a ±15 ms median, a
        // 40 ms centred mean. Straddling the step, the Impact reading and the PEAK tile both come
        // back low and early, which is the artefact this metric's timing fix removed at the
        // producer. The producer masks the post-impact tail (design §5.1's domain mask), so the
        // reducers read the club as it arrives, never a blend of arriving and departing.
        .domain = P1toP7,
        .routes = {
            via("clubSensorFused", RM::Fused, Direct,
                { .faceOnCamera = true, .imuRoles = { R::Club }, .clubTrack = true },
                QStringLiteral("v_grip from the tracked grip path plus omega-cross-r from a shaft "
                               "sensor — the axial term no camera sees, and the reason this rung "
                               "sits above the second camera rather than below it"), PLANNED),
            via("faceOn+dtl", RM::Triangulated, Direct,
                { .faceOnCamera = true, .dtlCamera = true, .clubTrack = true },
                QStringLiteral("the head path triangulated from the calibrated pair, which "
                               "restores the depth component of its velocity"), PLANNED),
            via("faceOnClub", RM::Projected, Direct, { .faceOnCamera = true, .clubTrack = true },
                QStringLiteral("differentiated from the clubhead's path in the face-on image, "
                               "scaled by the ball-diameter ruler")) },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("handSpeed"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Hand speed"),
        .shortLabel = QStringLiteral("Hand speed"),
        .unit = QStringLiteral("mph"),
        .group = QStringLiteral("Club & speed"),
        .description = QStringLiteral(
            "How fast the hands (the grip end of the club) are moving, from the tracked grip point "
            "— a proxy for how much speed the body and arms are delivering to the handle before the "
            "clubhead releases. In an efficient swing the hands lead and then decelerate as the "
            "clubhead accelerates past them."),
        .howToRead = QStringLiteral(
            "Read the peak, which in a good release comes slightly before impact — the hands "
            "slowing lets the clubhead sling past for maximum speed at the ball. Hands still "
            "accelerating at impact usually mean the release is late or the body has stalled. Needs "
            "face-on club tracking (the grip point)."),
        .signPositive = QStringLiteral("faster hands"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("faceOn+dtl", RM::Triangulated, Direct,
                { .faceOnCamera = true, .dtlCamera = true, .clubTrack = true },
                QStringLiteral("the grip path triangulated from the calibrated pair, which "
                               "restores the depth component of its velocity"), PLANNED),
            via("faceOnClub", RM::Projected, Direct, { .faceOnCamera = true, .clubTrack = true },
                QStringLiteral("differentiated from the grip's path in the face-on image, scaled "
                               "by the ball-diameter ruler")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:deceleration") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lagAngle"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lag angle"),
        .shortLabel = QStringLiteral("Lag"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club & speed"),
        .description = QStringLiteral(
            "The angle held between the lead forearm and the club shaft — the visible 'lag' that "
            "stores energy in the downswing. It is derived from the shaft track (grip to head) and "
            "the lead-forearm pose, and its release is what delivers clubhead speed to the ball."),
        .howToRead = QStringLiteral(
            "Read through the downswing into impact: a larger retained angle deep into the "
            "downswing means more stored lag, which then releases toward impact. Casting (the angle "
            "widening early) throws away speed and steepens the club, while holding it too long can "
            "leave the face open. It needs both the face-on club track and the lead-forearm pose."),
        .signPositive = QStringLiteral("more lag retained — a tighter forearm-to-shaft angle"),
        .signNegative = QString(),
        .phases = { P::ArmParallelDown, P::Impact },
        .routes = {
            via("faceOnClub", RM::Projected, Direct, { .faceOnCamera = true, .clubTrack = true },
                QStringLiteral("the lead forearm against the shaft, both read in the face-on "
                               "image")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:casting"),
                    QStringLiteral("characteristic:excessive_lag") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("impactShaftLean"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Shaft lean"),
        .shortLabel = QStringLiteral("Shaft lean"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club & speed"),
        .description = QStringLiteral(
            "How far the shaft leans forward (toward the target) from vertical at impact, from the "
            "tracked club and shaft. Forward shaft lean means the hands are ahead of the clubhead "
            "at the strike, which de-lofts the club and is a signature of solid iron contact."),
        .howToRead = QStringLiteral(
            "Read at Impact; forward lean with the hands ahead of the ball is typical and desirable "
            "for irons, while the driver is played with the shaft close to vertical or leaning "
            "back. Too little lean (or backward lean) on an iron usually means an early release, "
            "with thin/fat tendencies. It needs the face-on club track; Wrist Motion session."),
        .signPositive = QStringLiteral("the shaft leaning FORWARD, toward the target"),
        .signNegative = QStringLiteral("leaning back, away from the target"),
        .phases = { P::Impact },
        .routes = {
            via("faceOnClub", RM::Projected, Direct, { .faceOnCamera = true, .clubTrack = true },
                QStringLiteral("the shaft's lean at impact, from the face-on shaft track")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:insufficient_shaft_lean"),
                    QStringLiteral("characteristic:excessive_shaft_lean") },
    });

    // ------------------ Club delivery (part LIVE — club_delivery.cpp; the rest is down-the-line work)

    cat.addDescriptor({
        .key = QStringLiteral("swingPlane"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Swing plane"),
        .shortLabel = QStringLiteral("Plane"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "The tilt and direction of the plane the clubhead swings on through the downswing — a "
            "best-fit plane of the head path (or a lead-hand proxy) over the knee-to-knee section, "
            "reported as a tilt angle from the ground plus an azimuth. It captures whether the club "
            "is delivered on an inclined circle that matches the player and the club."),
        .howToRead = QStringLiteral(
            "Read over the downswing; the numbers are club-dependent, so compare like with like and "
            "look for consistency across swings more than an absolute target. A down-the-line "
            "camera is the classic view for plane, and with no club tracked the value falls back to "
            "a hand-path proxy that should be labelled as such. Planned: needs the club track."),
        .signPositive = QStringLiteral("a steeper plane"),
        .signNegative = QStringLiteral("a flatter plane"),
        .phases = { P::ArmParallelDown },
        .routes = {
            via("dtl", RM::Triangulated, Direct, { .dtlCamera = true, .clubTrack = true },
                QStringLiteral("an SVD best-fit plane through the head path needs that path in "
                               "three dimensions, which one camera cannot give"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:flat_backswing_plane"),
                    QStringLiteral("characteristic:steep_backswing_plane"),
                    QStringLiteral("characteristic:steep_downswing_shaft"),
                    QStringLiteral("characteristic:under_plane_stuck") },
        // over_the_top and shallowing USED to hang here, as a planned P4→P5 delta of
        // this down-the-line series. They moved to transitionPlaneDelta below, which
        // is the same idea measured a way the face-on camera can actually deliver.
    });

    // The transition delta, and the reason it is a separate key from swingPlane
    // above: this is not a plane ANGLE, it is the CHANGE in one between two windows,
    // and it comes off the face-on camera rather than down-the-line. Over the top is
    // not the same event as steep_downswing_shaft — a golfer can be steep at P6 off a
    // steep backswing without ever re-routing the club outward in transition — so it
    // wants its own measure, which is what this is.
    cat.addDescriptor({
        .key = QStringLiteral("transitionPlaneDelta"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Transition plane delta"),
        .shortLabel = QStringLiteral("Plane Δ"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "How much the swing plane changes between the backswing and the downswing, in degrees. "
            "The shaft vector sweeps a circle on the swing plane, and a circle on a plane images as "
            "an ellipse whose axis ratio gives that plane's inclination — so fitting one ellipse "
            "over takeaway-to-top and another over top-to-impact, then subtracting, measures the "
            "re-route out of the top without needing depth or a second camera."),
        .howToRead = QStringLiteral(
            "EXPERIMENTAL. Its corridor is a placeholder, set deliberately wider than anything yet "
            "observed so that the measure records and accumulates without grading anyone, because "
            "the characterisation behind it comes from a single golfer. "
            "Positive means the club STEEPENED between backswing and downswing, the over-the-top "
            "direction; negative means it shallowed, which good players often do on purpose. Read "
            "only the change — the underlying absolute plane angles are not calibrated and carry a "
            "large body-depth bias, so they are not a coaching number on their own."),
        .signPositive = QStringLiteral("the club steepened in transition — the over-the-top direction"),
        .signNegative = QStringLiteral("the club shallowed in transition"),
        .phases = { P::Transition },
        .routes = {
            via("faceOnClub", RM::Projected, Direct, { .faceOnCamera = true, .clubTrack = true },
                QStringLiteral("the shaft vector's own ellipse over each window, from the face-on "
                               "shaft track — no depth and no foreshortening model needed")) },
        .usedBy = { QStringLiteral("characteristic:over_the_top"),
                    QStringLiteral("characteristic:shallowing") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("clubPath"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Club path"),
        .shortLabel = QStringLiteral("Path"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "The horizontal direction the clubhead is travelling at impact relative to the target "
            "line — in-to-out (+) or out-to-in (−). Together with face angle it determines the "
            "ball's start line and curvature, making it one of the two numbers that most directly "
            "explain shot shape."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS IN-TO-OUT for a right-handed golfer — the head travelling "
            "right of the target line — and negative is out-to-in. Most good iron shots sit within "
            "a few degrees either side of zero, with the desired path depending on the shape being "
            "played. THIS METRIC IS THE HOUSE CONVENTION for the target-line axis: `launchDirection` "
            "and `shaftDirection` both state their signs by reference to it, and it is the same "
            "convention Foresight publishes, so `lm.clubPath` needs no negation on the way in and "
            "the measured and estimated pair stay directly comparable. The discriminating axis is "
            "the optical (toward-ball) axis, which is exactly what a face-on camera cannot see — "
            "this is a canonical down-the-line metric. Planned: needs the club track and a DTL camera."),
        .signPositive = QStringLiteral("the head travelling RIGHT of the target line — in-to-out for a right-hander"),
        .signNegative = QStringLiteral("travelling left — out-to-in for a right-hander"),
        .phases = { P::Impact },
        .routes = {
            via("dtl", RM::Triangulated, Direct, { .dtlCamera = true, .clubTrack = true },
                QStringLiteral("the horizontal velocity angle turns on the depth axis, which is "
                               "exactly what the face-on view is blind to"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:in_to_out_path"),
                    QStringLiteral("characteristic:out_to_in_path") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("attackAngle"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Attack angle"),
        .shortLabel = QStringLiteral("Attack"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "Whether the clubhead is moving down or up at impact — the vertical angle of its "
            "velocity. It controls compression and low point: irons are struck with a descending "
            "blow, while the driver is best hit slightly on the up to launch it high with low spin."),
        .howToRead = QStringLiteral(
            "Read at Impact. HIGHER MEANS A MORE UPWARD STRIKE: widely-published references run "
            "about −1.3° for the driver (many good drives are positive, +3 to +5°) and about −4.5° "
            "for a 7-iron. A too-steep iron angle "
            "digs and loses speed; a downward driver angle costs carry. THIS IS A FACE-ON READING, "
            "and this descriptor used to say the opposite. The angle lives in the vertical plane "
            "containing the target line, which IS the face-on image plane; a down-the-line camera "
            "puts that same direction on its own optical axis and is the one view that cannot "
            "measure it. What remains is a small cosine term from the path's depth component. "
            "Needs the club track with a MEASURED clubhead — a projected head carries the grip's "
            "motion, and differentiating it gives a confident number about the wrong thing."),
        .signPositive = QStringLiteral("an UPWARD strike"),
        .signNegative = QStringLiteral("a descending strike"),
        .phases = { P::Impact },
        .routes = {
            via("faceOnClub", RM::Projected, Direct, { .faceOnCamera = true, .clubTrack = true },
                QStringLiteral("the vertical velocity angle at impact, read in the face-on image "
                               "plane — which is the plane containing the target line, so this is "
                               "NOT a down-the-line metric")) },
        .usedBy = { QStringLiteral("characteristic:attack_too_shallow"),
                    QStringLiteral("characteristic:attack_too_steep"),
                    // The upward and downward halves of the two low-point outcomes. `m_attackAngle`
                    // PREFERS `lm.attackAngle` and falls back to this, so both keys carry the same
                    // four names — the ladder is a choice of instrument, not of quantity, and a
                    // directory that listed them on only one rung would understate whichever kit
                    // the reader happens to own.
                    QStringLiteral("characteristic:top"),
                    QStringLiteral("characteristic:sky") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lowPointAhead"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Low point"),
        .shortLabel = QStringLiteral("Low pt"),
        .unit = QStringLiteral("in"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "Where the bottom of the swing arc is relative to the ball, as a signed distance along "
            "the target line — positive when the low point is ahead of (target-side of) the ball. "
            "It is the single best 2D summary of ball-then-turf contact and the one club-delivery "
            "number a lone face-on camera can estimate — emphasis on ESTIMATE: it comes off the "
            "synthesized club arc, carries a published ±2 in, and is meant to be read across a "
            "handful of swings rather than one."),
        .howToRead = QStringLiteral(
            "Read near impact; a positive value (low point ahead of the ball) is the descending, "
            "ball-first strike you want with irons, while the driver is normally struck with the "
            "low point behind the ball. A low point behind the ball on an iron is the fat/thin "
            "signature. ⚠ READ IT AS A SESSION TENDENCY, NOT AS A SINGLE SWING. It is taken off "
            "the synthesized club arc — the interpolation between the located P-positions that the "
            "club overlay draws — because the clubhead detector does not hold a lock through "
            "impact, where the club is fastest and most blurred. That arc is an estimate of the "
            "path rather than an observation of it, and against a launch monitor it carries about "
            "±2 inches of spread on one swing, which is most of the width of the corridor it is "
            "graded in. It is UNBIASED, so several swings average to something trustworthy while "
            "any one of them can be out by more than the finding it drives. The arc vertex is "
            "refined below sample spacing by a local parabola, and is refused outright unless the "
            "arc is seen to turn over inside the window. The target direction comes from the "
            "head's own travel across impact rather than from handedness, so a mirrored camera "
            "cannot invert the sign. Needs a face-on camera, the club track and the ball (which "
            "supplies both the reference the answer is stated against and, through its diameter, "
            "the inches it is stated in)."),
        .signPositive = QStringLiteral("the arc bottoming out AHEAD of the ball, on the target side"),
        .signNegative = QStringLiteral("bottoming out behind the ball"),
        .phases = { P::Impact },
        // ESTIMATED, not Direct — so the shot-level answer resolves Bridged, and this summary is
        // shown verbatim as the reason. The rung is honest about the method rather than about a
        // missing device: the number IS produced on every swing that has an arc, it is simply an
        // interpolated arc's vertex rather than a measured clubhead's.
        .routes = {
            via("faceOnClubBall", RM::Projected, Estimated,
                { .faceOnCamera = true, .clubTrack = true, .ballTrack = true },
                QStringLiteral("estimated from the synthesized club arc's low point against the "
                               "ball, in the face-on image and in the ball's own ruler — about "
                               "±2 in on a single swing, so read it across several")) },
        .usedBy = { QStringLiteral("characteristic:low_point_behind_ball"),
                    QStringLiteral("characteristic:low_point_too_far_ahead"),
                    // …and the two outcomes those become when a strike height and an attack angle
                    // agree with them. Same note as attackAngle: `lm.lowPointAhead` is preferred
                    // over this key and carries the same names.
                    QStringLiteral("characteristic:top"),
                    QStringLiteral("characteristic:sky") },
    });

    // --------------------------------------------- Tempo & sequence (phase events / fused)
    // Both tempo metrics are anchored at IMPACT rather than a setup phase: they
    // describe the WHOLE swing, and Impact is the one instant every ladder has
    // (it is the acoustic marker, not a detection). The phase list must be
    // non-empty or no corridor can reach QML — MetricCatalog::descriptor() builds
    // the corridor list by looping `phases`.

    cat.addDescriptor({
        .key = QStringLiteral("tempoBackswing"),
        .type = MetricType::Summary,
        .label = QStringLiteral("Backswing tempo"),
        .shortLabel = QStringLiteral("Backswing"),
        .unit = QStringLiteral("s"),
        .group = QStringLiteral("Tempo & sequence"),
        .description = QStringLiteral(
            "How long the backswing takes, from address to the top of the swing. Backswing time is "
            "the foundation of tempo — it sets the rhythm the downswing has to match — and it is "
            "remarkably consistent within a good player, even across clubs."),
        .howToRead = QStringLiteral(
            "As a reference, tour backswings cluster around 0.75–0.85 s (TPI report 0.847 ± 0.111 "
            "s). Note those published figures are measured from the TAKEAWAY, while this one is "
            "measured from ADDRESS, so it reads slightly longer. The absolute value matters less "
            "than its consistency and its ratio to the downswing (see tempo ratio). Needs a "
            "confidently segmented swing — it is refused rather than estimated when the phase "
            "events are unreliable."),
        .signPositive = QStringLiteral("a longer backswing"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("phases", RM::Derived, Direct, { },
                QStringLiteral("timed off the segmented phase ladder, whatever produced it — an "
                               "IMU-only swing and a camera-only swing both qualify, and the "
                               "producer refuses a ladder it does not trust")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("tempoRatio"),
        .type = MetricType::Summary,
        .label = QStringLiteral("Tempo ratio"),
        .shortLabel = QStringLiteral("Tempo"),
        .unit = QStringLiteral(":1"),
        .group = QStringLiteral("Tempo & sequence"),
        .description = QStringLiteral(
            "The rhythm of the swing as a single number — backswing time divided by downswing time "
            "(top to impact). It captures the relationship between the two halves of the swing "
            "independently of how fast the player swings overall, which is why teachers lean on it "
            "so heavily."),
        .howToRead = QStringLiteral(
            "The classic tour figure is about 3:1 (backswing three times as long as the "
            "downswing), with most good players between roughly 2.2:1 and 3.0:1. A ratio that "
            "drifts from a player's norm — often a quick, snatchy transition dropping it well below "
            "3:1 — is a reliable early warning of a rhythm problem. Read it alongside its "
            "uncertainty: the top of the swing sits in both halves of the sum, so a small error in "
            "locating it moves this number more than you would expect."),
        .signPositive = QStringLiteral("a backswing slower relative to the downswing"),
        .signNegative = QString(),
        .phases = { P::Impact },
        // The corridor that used to be inlined here (green 2.2–3.0, amber 1.8–3.6) is now the
        // `m_tempoRatio` norm row in src/Resources/diagnostics/norms.json, and the note explaining
        // why it is provisional — published figures are measured Takeaway→Top where this metric is
        // Address→Top — is that norm's citation. It was the manifest's only inline corridor.
        .routes = {
            via("phases", RM::Derived, Direct, { },
                QStringLiteral("timed off the segmented phase ladder, whatever produced it — an "
                               "IMU-only swing and a camera-only swing both qualify, and the "
                               "producer refuses a ladder it does not trust")) },
        .usedBy = { QStringLiteral("characteristic:transition_rush") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("kinematicSequence"),
        .type = MetricType::Sequence,
        .label = QStringLiteral("Kinematic sequence"),
        .shortLabel = QStringLiteral("Sequence"),
        .unit = QString(),
        .group = QStringLiteral("Tempo & sequence"),
        .description = QStringLiteral(
            "The order, timing and size of the peak rotational speeds of the body segments — "
            "pelvis, then thorax, then lead arm, then club — as the swing fires from the ground up. "
            "A proximal-to-distal sequence, with each segment peaking and handing off to the next, "
            "is the signature of an efficient, powerful downswing, and this metric shows it directly."),
        .howToRead = QStringLiteral(
            "You want a clean proximal-to-distal order (pelvis → chest → arm → club) with only the "
            "club still accelerating through impact; out-of-order or overlapping peaks flag a leak "
            "of speed or a stall. Reference peaks run roughly pelvis ~480, thorax ~605 and lead-arm "
            "~1310 °/s with about a 50 ms transition gap. Planned: needs body IMUs (pelvis, thorax "
            "and lead forearm) and the club track."),
        .signPositive = QString(),
        .signNegative = QString(),
        .phases = { P::Transition, P::Impact },
        .routes = {
            via("segmentImus", RM::Inertial, Direct,
                { .imuRoles = { R::Pelvis, R::Thorax, R::LeadForearm }, .clubTrack = true },
                QStringLiteral("the ordered peak-speed nodes already exist; what is missing is "
                               "angular-SPEED series for the pelvis and thorax to order"), PLANNED) },
    });

    // ------------------------------------------------ Feet & stance (whole-body pose, face-on, 2D)

    cat.addDescriptor({
        .key = QStringLiteral("stanceWidth"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Stance width"),
        .shortLabel = QStringLiteral("Stance"),
        .unit = QStringLiteral("% shoulder width"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How wide the feet are set at address, measured heel-to-heel from the whole-body pose "
            "and expressed against the golfer's own shoulder width. Stance width is a foundation of "
            "balance and turn: too narrow costs stability, too wide restricts the hips."),
        .howToRead = QStringLiteral(
            "100 % means the heels are exactly shoulder-width apart; higher is wider. Read at "
            "address, against the club in hand — the longer clubs want a wider base and the wedges "
            "a narrower one. It is deliberately relative to the golfer's own frame rather than in "
            "millimetres, because a tall player and a short player take genuinely different stances "
            "and neither is wrong; it also happens to be how a stance is described out loud. Needs "
            "a face-on whole-body camera with both shoulders visible at address."),
        .signPositive = QStringLiteral("a wider stance"),
        .signNegative = QString(),
        .phases = { P::Address },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("heel-to-heel span over shoulder width — both spans read in the "
                               "same image, which is why it needs no ruler")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:stance_narrow"),
                    QStringLiteral("characteristic:stance_wide") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("stanceWidthMm"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Stance width (absolute)"),
        .shortLabel = QStringLiteral("Stance mm"),
        .unit = QStringLiteral("mm"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "The same heel-to-heel measurement in real-world millimetres, scaled by the golf ball's "
            "own diameter, which the rules fix. It answers a different question from the shoulder-"
            "width reading: how wide in the room, rather than how wide for this golfer."),
        .howToRead = QStringLiteral(
            "Read at address. Useful for tracking one golfer over time or setting up a mat, but not "
            "comparable between golfers of different heights — use the shoulder-width reading for "
            "that, and for any normative judgement. Present only when the ball was detected at "
            "address, since the ball IS the ruler."),
        .signPositive = QStringLiteral("a wider stance"),
        .signNegative = QString(),
        .phases = { P::Address },
        .routes = {
            via("faceOnBall", RM::Projected, Direct, { .faceOnCamera = true, .ballTrack = true },
                QStringLiteral("the same heel-to-heel span through the ball-diameter ruler, which "
                               "is what puts it in millimetres")) },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("ballPosition"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Ball position"),
        .shortLabel = QStringLiteral("Ball pos"),
        .unit = QStringLiteral("% stance width"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "Where the ball sits along the stance at address, as a percentage of stance width: "
            "0 % IS LEVEL WITH THE LEAD HEEL and 100 % with the trail heel, so a HIGHER value means "
            "the ball is further BACK. That is the scale other golf software uses, which is why it "
            "does not follow the lead-positive convention the displacement metrics do. Ball "
            "position sets the low point of the swing arc relative to the ball, which is why the "
            "same swing produces very different strikes as it moves."),
        .howToRead = QStringLiteral(
            "Read it against the club in hand rather than against a single ideal: a driver wants "
            "the ball forward, near 0 % off the lead heel, while a wedge wants it around 50 %, "
            "closer to the middle of the stance. Values below 0 % are normal and mean the ball is "
            "forward of the lead heel. "
            "Because this is a ratio of two distances in the same plane it is directly comparable "
            "between swings and cameras, unlike stance width itself. Needs a face-on camera and a "
            "detected ball at address."),
        .signPositive = QStringLiteral(
            "the ball further BACK, toward the trail foot — 0% is the lead heel, 100% the trail "
            "heel, the scale other golf software uses"),
        .signNegative = QStringLiteral("forward of the lead heel, which is a real driver setup"),
        .phases = { P::Address },
        .routes = {
            via("faceOnBall", RM::Projected, Direct, { .faceOnCamera = true, .ballTrack = true },
                QStringLiteral("the ball projected onto the heel line over stance width — a same- "
                               "plane ratio, so it needs no scale")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:ball_back"),
                    QStringLiteral("characteristic:ball_forward") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadFootFlare"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Lead foot flare"),
        .shortLabel = QStringLiteral("Lead flare"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How much the lead foot is turned out (flared) at address, from the angle of that "
            "foot's heel-to-big-toe line in the image plane. Lead-foot flare is a setup choice that "
            "changes how freely the lead hip can clear through impact."),
        .howToRead = QStringLiteral(
            "A single address measurement. More flare (toe pointing out toward the target) makes it "
            "easier for the lead hip to rotate open and clear through the strike, which can help "
            "players who struggle to finish their turn. Needs a face-on whole-body camera."),
        .signPositive = QStringLiteral("the lead toe turned out, away from the ball"),
        .signNegative = QStringLiteral("turned in"),
        .phases = { P::Address },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the heel-to-big-toe line's angle, in the face-on image")) },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("trailFootFlare"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Trail foot flare"),
        .shortLabel = QStringLiteral("Trail flare"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How much the trail foot is turned out at address, from the angle of its heel-to-big-"
            "toe line in the image plane. Trail-foot flare regulates how much the trail hip can "
            "turn and load in the backswing."),
        .howToRead = QStringLiteral(
            "A single address measurement. A square (un-flared) trail foot restrains and stores the "
            "backswing turn, while flaring it out lets the hips turn more freely going back — a "
            "useful lever for players who lack mobility or over-rotate. Needs a face-on whole-body "
            "camera."),
        .signPositive = QStringLiteral("the trail toe turned out, away from the ball"),
        .signNegative = QStringLiteral("turned in"),
        .phases = { P::Address },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the heel-to-big-toe line's angle, in the face-on image")) },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("toeLineAngle"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Toe line"),
        .shortLabel = QStringLiteral("Toe line"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "The alignment of the stance at address, taken as the angle of the line joining the two "
            "big toes relative to the image horizontal. It is a quick read on whether the feet are "
            "set open, square or closed to the intended line. OPEN IS NEGATIVE AND CLOSED IS "
            "POSITIVE, the same convention as club path and shoulder alignment."),
        .howToRead = QStringLiteral(
            "A single address measurement of stance alignment (open / square / closed) in the image "
            "plane. Because it is measured face-on it reads the apparent line rather than true "
            "target-line alignment, which a down-the-line or overhead view would resolve more "
            "directly. Needs a face-on whole-body camera."),
        .signPositive = QStringLiteral("a closed stance line as the camera sees it"),
        .signNegative = QStringLiteral("an open stance line — and this one line metric FLIPS for a mirrored camera"),
        .phases = { P::Address },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the big-toe-to-big-toe line's angle, in the face-on image")) },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadHeelLift"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead heel lift"),
        .shortLabel = QStringLiteral("Heel lift"),
        .unit = QStringLiteral("cm"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How far the lead heel rises off the ground through the swing, relative to address, in "
            "centimetres. HIGHER MEANS THE HEEL IS FURTHER OFF THE GROUND. Some players anchor both "
            "heels; others let the lead heel come up in the backswing to allow a bigger turn — both "
            "can work."),
        .howToRead = QStringLiteral(
            "This is a per-frame curve, usually read for how much the heel comes up around the top. "
            "A little lift is common and can free up the backswing turn; keeping the heel down is a "
            "legitimate stylistic choice for stability. Read the trend rather than any single "
            "value. The centimetre scale comes from the ball-diameter ruler at the ground plane, so "
            "the metric is absent rather than rescaled when no ball was detected. Needs a face-on "
            "whole-body camera and a detected ball."),
        .signPositive = QStringLiteral("the lead heel further off the ground"),
        .signNegative = QString(),
        .phases = { P::Top },
        // ballTrack is not optional decoration: foot_metrics.cpp emits this curve only when
        // mmPerPx resolved, because the reading is centimetres and the ball diameter is the only
        // ruler at the ground plane. The rung once named the face-on camera alone, so a ball-less
        // shot was told the metric was Measured while the producer had declined to emit it. The
        // howToRead below it had described the ball dependency correctly all along.
        .routes = {
            via("faceOnBall", RM::Projected, Direct, { .faceOnCamera = true, .ballTrack = true },
                QStringLiteral("heel elevation against the toe, through the ball-diameter ruler — "
                               "the reading is in centimetres and the ball is the only ruler at "
                               "the ground plane")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:excessive_heel_lift") },
    });

    // -------------------------------------------- Alignment (pose lines at address / impact)
    //
    // `shoulderAlignment` and `hipAlignment` USED TO LIVE HERE and are gone. Each was geometrically
    // identical to a line this catalogue already carries — the shoulder line's angle is
    // `shoulderPlaneAngle`, the hip line's is `hipLineTilt` — differing only in which phases it was
    // read at, which metric_reducer.h exists to express: a series is what a producer produces, a
    // reducer is how it is sampled. Two descriptors for one curve would have been two names for one
    // number, under two different sign conventions. The pack measures `m_shoulderAlignment` and
    // `m_hipAlignment` now point at the surviving series.


    cat.addDescriptor({
        .key = QStringLiteral("elbowAlignment"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Elbow alignment"),
        .shortLabel = QStringLiteral("Elbows"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Alignment"),
        .description = QStringLiteral(
            "The angle of the line joining the two elbows in the image plane, read at address and "
            "at impact — a compact read on how the arms and elbows are structured relative to the "
            "body. It complements the shoulder line by showing what the arms are doing "
            "independently of the torso."),
        .howToRead = QStringLiteral(
            "Read at Address and Impact; the change between the two reflects how the arms fold, "
            "rotate and re-deliver through the strike (for example the trail elbow tucking on the "
            "way down). Read it together with lead-arm flexion and the shoulder line. POSITIVE "
            "MEANS THE TRAIL ELBOW SITS ABOVE THE LEAD ELBOW, the same convention every body line "
            "in the product uses. Needs a face-on camera."),
        .signPositive = QStringLiteral("the TRAIL elbow sits above the lead elbow"),
        .signNegative = QStringLiteral("the lead elbow sits above the trail elbow"),
        .phases = { P::Address, P::Impact },
        // The elbow line foreshortens as the arms cross the body: see P1toP7.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the elbow line's angle, in the face-on image")) },
    });


    cat.addDescriptor({
        .key = QStringLiteral("feetAlignment"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Feet alignment"),
        .shortLabel = QStringLiteral("Feet"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Alignment"),
        .description = QStringLiteral(
            "The alignment of the feet as a body line — the angle of the line joining the lead and "
            "trail ankles in the image plane — read at address and at impact. OPEN IS NEGATIVE AND "
            "CLOSED IS POSITIVE, the same convention as shoulder and hip alignment. It complements "
            "the address-only toe line by adding an impact read and by using the ankle joints rather "
            "than the toes, so it is less affected by foot flare."),
        .howToRead = QStringLiteral(
            "Read at Address and Impact. At address it reports the stance line (open / square / "
            "closed); the impact read shows how the feet and lower legs have worked — for example "
            "the trail foot rolling and the ankles re-orienting as the player pushes off. "
            "POSITIVE MEANS THE TRAIL ANKLE SITS ABOVE THE LEAD ANKLE in the image, which for a "
            "golfer standing on level ground is the trail foot set further from the camera — a "
            "closed stance. Like the toe line it reads the APPARENT line rather than true "
            "target-line alignment, which a down-the-line or overhead view would resolve directly; "
            "unlike the toe line its sign does not invert for a left-handed golfer or a mirrored "
            "camera. Needs a face-on camera."),
        .signPositive = QStringLiteral("the TRAIL ankle sits above the lead ankle — a closed stance"),
        .signNegative = QStringLiteral("the lead ankle sits above the trail ankle — an open stance"),
        .phases = { P::Address, P::Impact },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the ankle line's angle, in the face-on image")) },
        .usedBy = { QStringLiteral("characteristic:feet_alignment_closed"),
                    QStringLiteral("characteristic:feet_alignment_open") },
    });

    // ---------------------------------------------------- Head (whole-body pose, face-on, 2D; live)

    cat.addDescriptor({
        .key = QStringLiteral("headSway"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Head sway"),
        .shortLabel = QStringLiteral("Head sway"),
        .unit = QStringLiteral("cm"),
        .group = QStringLiteral("Head"),
        .description = QStringLiteral(
            "How far the head moves side-to-side relative to address, in centimetres. POSITIVE IS "
            "TOWARD THE LEAD SIDE, the same displacement convention pelvis sway follows. The head is "
            "a convenient, stable proxy for whether the upper body is staying centred: rotating "
            "around a steady head is efficient, while sliding the head off the ball tends to move "
            "the low point."),
        .howToRead = QStringLiteral(
            "This is a per-frame curve; some lateral movement (especially a small shift back and "
            "through) is normal, and only excessive sway is a fault. Read the trend and the peak "
            "rather than any single frame, and pair it with pelvis sway to see whether the whole "
            "body is sliding. The centimetre scale comes from the inter-ear ruler, so the metric is "
            "absent rather than rescaled when the head is too small or too occluded to measure. "
            "Needs a face-on camera; Wrist Motion session."),
        .signPositive = QStringLiteral("the head moved toward the LEAD side"),
        .signNegative = QStringLiteral("moved away from the lead side"),
        .phases = { P::Top, P::Impact },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("lateral head travel through the INTER-EAR ruler taken at address "
                               "— the ear keypoints are the scale, which is why this reads in "
                               "centimetres off one camera and needs no ball")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:excessive_head_sway"),
                    QStringLiteral("characteristic:head_drift_lead_backswing") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("headLift"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Head lift"),
        .shortLabel = QStringLiteral("Head lift"),
        .unit = QStringLiteral("cm"),
        .group = QStringLiteral("Head"),
        .description = QStringLiteral(
            "How far the head rises or drops relative to address, in centimetres. HIGHER MEANS THE "
            "HEAD HAS RISEN. Vertical head movement is an early, easy-to-see indicator of standing "
            "up out of posture or dipping into the ball, both of which change the strike."),
        .howToRead = QStringLiteral(
            "A per-frame curve read against the address height. A steady head is ideal; an early "
            "rise through the downswing points toward standing up / early extension, while a dip "
            "suggests a drop into the shot. Read it alongside spine forward bend and pelvis lift. "
            "The centimetre scale comes from the inter-ear ruler, so the metric is absent rather "
            "than rescaled when it does not resolve. Needs a face-on camera; Wrist Motion session."),
        .signPositive = QStringLiteral("the head rose"),
        .signNegative = QStringLiteral("the head dropped"),
        .phases = { P::Top, P::ArmParallelDown, P::Impact },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("vertical head travel through the INTER-EAR ruler taken at "
                               "address — the reading is in centimetres, and the ear "
                               "keypoints are the scale, so it needs no ball")) },
        .usedBy = { QStringLiteral("chart:review"),
                    QStringLiteral("characteristic:head_drop_backswing"),
                    QStringLiteral("characteristic:head_rise_backswing"),
                    QStringLiteral("characteristic:coming_out_of_it") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("headTilt"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Head tilt"),
        .shortLabel = QStringLiteral("Head tilt"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Head"),
        .description = QStringLiteral(
            "How the eye-line tilts relative to its address angle, in degrees — the rotational "
            "(not translational) head measure. It picks up the head cocking or levelling through "
            "the swing, which can influence how level the shoulders turn and how the player sees "
            "the ball."),
        .howToRead = QStringLiteral(
            "A per-frame curve of the eye-line angle change from address. Small, stable changes are "
            "normal; a large or abrupt tilt change can accompany a loss of posture or an "
            "over-active head. Read it with head sway and lift for the full picture of head motion. "
            "Needs a face-on camera; Wrist Motion session."),
        .signPositive = QStringLiteral("the eye line tilted further than at address, trail-end high"),
        .signNegative = QStringLiteral("tilted the other way from address"),
        .phases = { P::Top, P::Impact },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the eye line's angle in the face-on image — an angle, so it needs "
                               "no scale")) },
        .usedBy = { QStringLiteral("chart:review") },
    });

    // ------------------------------------------ Diagnostics-pack measures (src/Resources/diagnostics)
    // Every characteristic in the shipped diagnostics pack resolves to a catalogue metric, so one
    // coverage view spans both registries and there is no second parallel list of measures. These
    // nine had no key before the pack was authored; each is PLANNED — the pack names what it needs,
    // and this is where that need is recorded.
    //
    // The four length measures are expressed as a PERCENTAGE of a body dimension rather than in
    // millimetres. A raw length is not comparable between a tall golfer and a short one, nor
    // between two camera distances, so a corridor over it could never transfer. Naming the
    // normaliser in the unit ("% shoulder width") keeps that visible at the point of reading.

    cat.addDescriptor({
        .key = QStringLiteral("thoracicFlexion"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Thoracic flexion at address"),
        .shortLabel = QStringLiteral("Upper-back round"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Spine & tilt"),
        .description = QStringLiteral(
            "How far the upper back is rounded forward at address, measured as the thoracic "
            "segment's angle from vertical. This is the C-posture axis: the thoracic spine rotates "
            "best near neutral, and in flexion it loses a meaningful share of the turn available, "
            "which the swing then has to find somewhere else."),
        .howToRead = QStringLiteral(
            "Read at address, from down the line. Higher means more rounded. It is distinct from "
            "spine forward bend, which is the hinge from the hips, and from lumbar extension, which "
            "is the low-back arch — three different regions that a single neck-to-pelvis line "
            "cannot tell apart. Planned: neither pose layout carries a keypoint between the "
            "shoulders and the hips, so this cannot come from the skeleton — but upper-back "
            "rounding is plainly visible in the BACK CONTOUR of a down-the-line silhouette. That "
            "makes it a producer worth building rather than a gap that can never close."),
        .signPositive = QStringLiteral("a more rounded upper back"),
        .signNegative = QStringLiteral("a flatter, more extended upper back"),
        .phases = { P::Address },
        .routes = {
            via("dtlContour", RM::Projected, Direct, { .dtlCamera = true },
                QStringLiteral("the upper-back contour of a down-the-line silhouette — no pose "
                               "layout carries a keypoint between the shoulders and the hips, so "
                               "this cannot come from the skeleton at all"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:c_posture"),
                    QStringLiteral("characteristic:flat_thoracic_spine") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lumbarExtension"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Lumbar extension at address"),
        .shortLabel = QStringLiteral("Low-back arch"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Spine & tilt"),
        .description = QStringLiteral(
            "How far the low back is arched at address, measured as the lumbar segment's angle from "
            "neutral. This is the S-posture axis. An exaggerated arch pre-tensions the lower back "
            "and makes it harder to turn the pelvis without the spine taking the load."),
        .howToRead = QStringLiteral(
            "Read at address, from down the line. Higher means more arched. Planned, for the same "
            "reason as thoracic flexion: no pose layout carries a keypoint between the shoulders "
            "and the hips, but the low-back arch is plainly visible in the BACK CONTOUR of a "
            "down-the-line silhouette."),
        .signPositive = QStringLiteral("a more arched low back"),
        .signNegative = QStringLiteral("a flattened low back"),
        .phases = { P::Address },
        .routes = {
            via("dtlContour", RM::Projected, Direct, { .dtlCamera = true },
                QStringLiteral("the low-back contour of a down-the-line silhouette — no pose "
                               "layout carries a keypoint between the shoulders and the hips, so "
                               "this cannot come from the skeleton at all"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:s_posture"),
                    QStringLiteral("characteristic:flat_lumbar_spine") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("shoulderPlaneAngle"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Shoulder plane angle"),
        .shortLabel = QStringLiteral("Shoulder plane"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Body rotation"),
        .description = QStringLiteral(
            "The angle the shoulder line makes with the ground, sampled through the swing. At the "
            "top it describes how steeply or flatly the shoulders have turned. A flat plane sets "
            "the club behind the body, which is commonly recovered by throwing the upper body out "
            "at the start of the downswing."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the reading that matters is at the top. Lower means flatter, more "
            "horizontal shoulders. It is a consequence of how the golfer is built and how they set "
            "up as much as of what they did, so read it alongside address posture rather than on "
            "its own. POSITIVE MEANS THE TRAIL SHOULDER SITS ABOVE THE LEAD SHOULDER, the same "
            "convention as the hip line. Needs a face-on camera. IT IS ALSO THE SHOULDER ALIGNMENT "
            "READING: at address and impact the same line answers open / square / closed, which is "
            "why `shoulderAlignment` is not a separate metric."),
        .signPositive = QStringLiteral("the TRAIL shoulder sits above the lead shoulder"),
        .signNegative = QStringLiteral("the lead shoulder sits above the trail shoulder"),
        .phases = { P::Address, P::Top, P::Impact },
        // The shoulder line foreshortens as the thorax turns: see P1toP7.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the shoulder line's angle to horizontal, in the face-on image")) },
        .usedBy = { QStringLiteral("characteristic:flat_shoulder_plane"),
                    QStringLiteral("characteristic:steep_shoulder_plane"),
                    QStringLiteral("characteristic:alignment_open"),
                    QStringLiteral("characteristic:alignment_closed") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadKneeFlexion"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead knee flexion"),
        .shortLabel = QStringLiteral("Lead knee"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "The angle at the lead knee — the lead shin against the lead thigh — through the swing. "
            "A knee that collapses through impact drops the whole body and makes the strike depend "
            "on timing rather than on repeatable geometry."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the reading that matters is the peak from impact into the "
            "follow-through. Higher means more bend. A knee angle is between two SEGMENTS, not a "
            "property of the knee point itself — which is why the shin and thigh exist separately "
            "in the anatomy vocabulary. Needs a face-on camera."),
        .signPositive = QStringLiteral("more knee bend"),
        .signNegative = QString(),
        .phases = { P::Impact, P::ShaftParallelThrough },
        .routes = {
            via("dtl", RM::Triangulated, Direct, { .faceOnCamera = true, .dtlCamera = true },
                QStringLiteral("the shin-against-thigh angle seen from down the line, where the "
                               "bend is in view rather than pointing at the camera"), PLANNED),
            via("faceOn", RM::Projected, Estimated, { .faceOnCamera = true },
                QStringLiteral("a sagittal angle foreshortened by the frontal projection — "
                               "visible, but close enough to noise that no producer reads it "
                               "there today"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:excessive_knee_flex"),
                    QStringLiteral("characteristic:insufficient_knee_flex"),
                    QStringLiteral("characteristic:late_buckle") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadArmToTorso"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead arm to torso angle"),
        .shortLabel = QStringLiteral("Arm/torso"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Arms"),
        .description = QStringLiteral(
            "The angle between the lead upper arm and the torso, through the swing. It describes "
            "how connected the lead arm stays to the body's turn. The lead arm folding through "
            "impact shortens the radius exactly where it should be longest, costing speed and "
            "consistency of face angle."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the change from impact into the follow-through is the reading that "
            "matters. A rising angle there means the arm is separating from the body rather than "
            "extending down the line. Needs a face-on camera."),
        .signPositive = QStringLiteral(
            "the arm further from the torso — UNSIGNED 0–180°, a frontal projection cannot say "
            "which side it left on"),
        .signNegative = QString(),
        .phases = { P::Impact, P::ShaftParallelThrough },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the lead arm against the torso line, in the face-on image")) },
        .usedBy = { QStringLiteral("characteristic:chicken_wing") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("ballBodyDistance"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Ball distance from the body"),
        .shortLabel = QStringLiteral("Ball reach"),
        .unit = QStringLiteral("% shoulder width"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How far the ball sits from the golfer, measured across the stance line and expressed "
            "as a percentage of shoulder width so it is comparable between golfers. Standing too "
            "close crowds the arms and steepens the swing; reaching for the ball pulls the "
            "shoulders forward and rounds the upper back even in someone whose standing posture is "
            "fine. It is a different axis from ball position, which runs ALONG the stance."),
        .howToRead = QStringLiteral(
            "A single setup measurement at address. Higher means further away. This is a depth "
            "measurement across the stance line, so it needs the down-the-line view — face-on "
            "cannot resolve it, because the distance runs along that camera's own axis. Normalised "
            "by shoulder width: a raw millimetre reading is not comparable between a tall golfer "
            "and a short one."),
        .signPositive = QStringLiteral("standing further from the ball"),
        .signNegative = QStringLiteral("standing closer to the ball"),
        .phases = { P::Address },
        .routes = {
            via("dtl", RM::Triangulated, Direct, { .dtlCamera = true, .ballTrack = true },
                QStringLiteral("the ball's standoff is measured across the stance line, which is "
                               "the depth axis the face-on camera cannot see"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:ball_too_close"),
                    QStringLiteral("characteristic:ball_too_far") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("thoraxLateralDrift"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Thorax lateral drift"),
        .shortLabel = QStringLiteral("Chest drift"),
        .unit = QStringLiteral("% stance width"),
        .group = QStringLiteral("Pelvis & lateral"),
        .description = QStringLiteral(
            "How far the centre of the chest has moved sideways from its address position, as a "
            "percentage of stance width. The upper body moving toward the target ahead of the "
            "sequence steepens the attack and narrows the margin for a clean strike."),
        .howToRead = QStringLiteral(
            "A per-frame curve of displacement from address; the change into the early downswing is "
            "the reading that matters. POSITIVE IS TOWARD THE LEAD SIDE, the same convention as "
            "pelvis sway, of which this is the chest's counterpart — the two are read together, "
            "because the same absolute movement means something different depending on whether the "
            "pelvis went with it. Normalised by "
            "stance width so it compares across golfers and camera distances. Needs a face-on "
            "camera."),
        .signPositive = QStringLiteral("the chest moved toward the LEAD side"),
        .signNegative = QStringLiteral("moved away from the lead side"),
        .phases = { P::Address, P::ArmParallelDown },
        // Lateral chest travel, same projection: see P1toP7.
        .domain = P1toP7,
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("lateral travel of the chest centre in the face-on image, against "
                               "the ground plane")) },
        .usedBy = { QStringLiteral("characteristic:forward_lunge"),
                    QStringLiteral("characteristic:hanging_back") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("trailElbowHeight"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Trail elbow height"),
        .shortLabel = QStringLiteral("Trail elbow"),
        .unit = QStringLiteral("% shoulder width"),
        .group = QStringLiteral("Arms"),
        .description = QStringLiteral(
            "How high the trail elbow sits relative to the shoulder line, as a percentage of "
            "shoulder width. A trail elbow lifted away from the body at the top disconnects the "
            "arms from the turn and needs a re-route to deliver the club."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the reading that matters is at the top. Higher means the elbow has "
            "risen further above the shoulder line. Normalised by shoulder width rather than left "
            "in millimetres, so the same reading means the same thing for any golfer. Needs a "
            "face-on camera."),
        .signPositive = QStringLiteral("the trail elbow higher above the shoulder line"),
        .signNegative = QStringLiteral("below the shoulder line"),
        .phases = { P::Top },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("trail elbow height over shoulder width — both read in the same "
                               "image, so it needs no ruler")) },
        .usedBy = { QStringLiteral("characteristic:flying_elbow"),
                    QStringLiteral("characteristic:trail_elbow_deep") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadHandWidth"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Swing width at the top"),
        .shortLabel = QStringLiteral("Width"),
        .unit = QStringLiteral("% arm length"),
        .group = QStringLiteral("Arms"),
        .description = QStringLiteral(
            "How far the lead hand is from the centre of the chest, as a percentage of lead arm "
            "length — the swing's width. The hands collapsing toward the chest in the backswing "
            "shortens the arc and removes the space the downswing needs to build speed."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the reading that matters is at the top. Lower means narrower. "
            "Expressed against the golfer's own arm length, so it reads as a fraction of the width "
            "actually available to them rather than as an absolute distance. Needs a face-on "
            "camera."),
        .signPositive = QStringLiteral("the hands further from the chest — a wider arc"),
        .signNegative = QStringLiteral("the hands closer to the chest"),
        .phases = { P::Top },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("hand distance from the body over arm length — both read in the "
                               "same image")) },
        .usedBy = { QStringLiteral("characteristic:loss_of_width") },
    });

    // ======================================================================================
    // Content extension: the metrics the extended fault library needs.
    //
    // Every one of these is authored BEFORE the measure and the signal that will read it, because
    // `axis_direction_test` settles a signal's tail by quoting the descriptor's own `howToRead`.
    // A metric that ships without saying which way is positive is exactly how three signals shipped
    // inverted, so the sign sentence is the first thing written here, not the last.
    // ======================================================================================

    // ---------------------------------------------------- Lower body (whole-body pose, face-on, 2D)

    cat.addDescriptor({
        .key = QStringLiteral("trailKneeFlexion"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Trail knee flexion"),
        .shortLabel = QStringLiteral("Trail knee"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "The angle between the trail shin and the trail thigh through the swing. The trail knee "
            "holds the flex it was given at address while the pelvis turns against it; losing that "
            "flex on the way back is how a turn becomes a slide, and it takes the pressure off the "
            "trail side before the downswing can use it."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the change from address to the top is the reading that matters. "
            "HIGHER MEANS MORE BEND, the same convention as the lead knee. A knee angle is between "
            "two SEGMENTS, not a property of the knee point. Needs a face-on camera."),
        .signPositive = QStringLiteral("more knee bend"),
        .signNegative = QString(),
        .phases = { P::Address, P::Top },
        .routes = {
            via("dtl", RM::Triangulated, Direct, { .faceOnCamera = true, .dtlCamera = true },
                QStringLiteral("the shin-against-thigh angle seen from down the line, where the "
                               "bend is in view rather than pointing at the camera"), PLANNED),
            via("faceOn", RM::Projected, Estimated, { .faceOnCamera = true },
                QStringLiteral("a sagittal angle foreshortened by the frontal projection — "
                               "visible, but close enough to noise that no producer reads it "
                               "there today"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:trail_knee_straighten") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("comOverLeadFoot"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Balance over the lead foot"),
        .shortLabel = QStringLiteral("Balance"),
        .unit = QStringLiteral("% stance width"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How far the pelvis centre sits from the lead ankle, along the stance line, as a "
            "percentage of stance width. At the finish a golfer who has used the ground is stacked "
            "over the lead foot and can hold the position; one who has not is still falling."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the reading that matters is at the finish. HIGHER MEANS FURTHER "
            "FROM THE LEAD ANKLE — still back, or fallen through it — so a balanced finish is the "
            "low end. It is a proxy for balance, not a measurement of it: without pressure data "
            "this reads geometry only. Needs a face-on camera."),
        .signPositive = QStringLiteral(
            "further FROM the lead ankle — UNSIGNED, because still back and fallen through are the "
            "same fault seen from either side"),
        .signNegative = QString(),
        .phases = { P::Impact, P::Finish },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the balance point against the lead foot, in the face-on image "
                               "against the ground plane")) },
        .usedBy = { QStringLiteral("characteristic:off_balance_finish") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("balanceHeelToe"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Balance heel to toe"),
        .shortLabel = QStringLiteral("Heel/toe"),
        .unit = QStringLiteral("% foot length"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "Where the balance point sits between the heels and the toes, as a percentage of foot "
            "length from the heel. THE DEPTH-AXIS PARTNER TO BALANCE OVER THE LEAD FOOT, which "
            "measures along the stance line instead — a golfer can be perfectly placed between "
            "their feet and still be sat on their heels, and the two faults have nothing to do "
            "with one another. The reading that matters is at address: starting on the heels "
            "leaves only one direction to move in when the downswing loads the ground, and that "
            "direction is toward the ball."),
        .howToRead = QStringLiteral(
            "Read at Address. HIGHER MEANS FURTHER TOWARD THE TOES; 50 % is the middle of the "
            "foot. Like balance over the lead foot this is a proxy and not a measurement — "
            "without pressure data it reads geometry only, and it inherits that metric's caveat "
            "rather than improving on it. Heel-to-toe travel lies along the face-on camera's "
            "blind axis, so it needs the down-the-line view. Planned."),
        .signPositive = QStringLiteral("the balance point further toward the toes"),
        .signNegative = QStringLiteral("further back toward the heels"),
        .phases = { P::Address },
        .routes = {
            via("dtl", RM::Triangulated, Direct, { .faceOnCamera = true, .dtlCamera = true },
                QStringLiteral("heel-to-toe travel lies along the face-on camera's blind axis, "
                               "the same axis pelvis thrust lives on, so it takes the "
                               "down-the-line view to see at all"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:weight_in_heels_address") },
    });

    // ---------------------------------------------------- Arms

    cat.addDescriptor({
        .key = QStringLiteral("leadUpperArmToChest"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead arm connection"),
        .shortLabel = QStringLiteral("Connection"),
        .unit = QStringLiteral("% shoulder width"),
        .group = QStringLiteral("Arms"),
        .description = QStringLiteral(
            "The gap between the lead upper arm and the chest, normalised by shoulder width. "
            "Coaches call the arm staying near the body 'connection'; the arm running away from it "
            "in the backswing is a different fault from the trail elbow flying, and a different one "
            "again from the lead elbow folding after impact, which is why it is its own curve."),
        .howToRead = QStringLiteral(
            "A per-frame curve; the widest gap between address and the top is the reading that "
            "matters. HIGHER MEANS A LARGER GAP — the arm further from the chest. Some separation is "
            "normal and a value of zero would mean the arm is pinned, which is its own fault. Needs "
            "a face-on camera."),
        .signPositive = QStringLiteral("a larger gap — the arm further from the chest"),
        .signNegative = QString(),
        .phases = { P::Top },
        .routes = {
            via("faceOn", RM::Projected, Direct, { .faceOnCamera = true },
                QStringLiteral("the lead-arm connection gap over shoulder width — both read in "
                               "the same image")) },
        .usedBy = { QStringLiteral("characteristic:disconnection"),
                    QStringLiteral("characteristic:arms_over_connected") },
    });

    // ---------------------------------------------------- Shaft geometry (club track, DTL)

    cat.addDescriptor({
        .key = QStringLiteral("shaftDirection"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Shaft direction"),
        .shortLabel = QStringLiteral("Shaft dir"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "Where the shaft points relative to the target line, seen down the line. Read in the "
            "takeaway it says whether the club was dragged inside or pushed outside; read at the "
            "top it is the across-the-line / laid-off distinction, which is the same question asked "
            "at the other end of the backswing."),
        .howToRead = QStringLiteral(
            "Read at P2 and at the Top. POSITIVE POINTS RIGHT OF THE TARGET for a right-handed "
            "golfer — across the line at the top, outside in the takeaway — and negative points "
            "left: laid off, or dragged inside. Zero is parallel to the target line, which is the "
            "reference both positions are named against. Planned: needs the club track and a "
            "down-the-line camera."),
        .signPositive = QStringLiteral("pointing RIGHT of the target line — across the line for a right-hander"),
        .signNegative = QStringLiteral("pointing left — laid off, or dragged inside"),
        .phases = { P::ShaftParallelBack, P::Top },
        .routes = {
            via("dtl", RM::Triangulated, Direct, { .dtlCamera = true, .clubTrack = true },
                QStringLiteral("where the shaft points relative to the target line is a bearing "
                               "on the depth axis, so it needs the down-the-line view"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:across_the_line"),
                    QStringLiteral("characteristic:inside_takeaway"),
                    QStringLiteral("characteristic:laid_off"),
                    QStringLiteral("characteristic:outside_takeaway") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("shaftAngleVsHorizontal"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Shaft angle at the top"),
        .shortLabel = QStringLiteral("Past parallel"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "How far past horizontal the shaft has travelled at the top of the backswing. "
            "'Parallel' is the reference every coach uses for backswing length, and length is a "
            "different question from plane or direction — a swing can be long and on plane."),
        .howToRead = QStringLiteral(
            "Read at the Top. ZERO IS PARALLEL TO THE GROUND; POSITIVE IS PAST PARALLEL and "
            "negative is short of it. Length is strongly club-dependent and partly a matter of "
            "flexibility and style, so the corridor is wide and a reading outside it is a "
            "conversation, not a verdict. Taken from the grip-to-head vector rather than from the "
            "shaft angle itself, because a line carries a 180° ambiguity that the endpoint pair "
            "resolves for free. Needs the club track with a MEASURED clubhead and a face-on "
            "camera."),
        .signPositive = QStringLiteral("PAST parallel to the ground; zero IS parallel"),
        .signNegative = QStringLiteral("short of parallel"),
        .phases = { P::Top },
        .routes = {
            via("faceOnClub", RM::Projected, Direct, { .faceOnCamera = true, .clubTrack = true },
                QStringLiteral("the shaft against horizontal, from the face-on shaft track")) },
        .usedBy = { QStringLiteral("characteristic:overswing"),
                    QStringLiteral("characteristic:club_short_of_parallel") },
    });

    // ---------------------------------------------------- Ball flight (ball track, face-on)
    //
    // Start line and launch would be resolvable from a ball-FLIGHT track; CURVATURE would not,
    // because it develops over a flight we do not see indoors. Both halves are still planned, and
    // for a reason the descriptors below now state plainly rather than implying it is nearly done:
    // the ball detector we have is an AT-SPOT PRESENCE tracker, not a flight tracker. It answers
    // "is the ball still there" and "when did it go", which is exactly what shot detection and the
    // address ruler need and exactly not what a launch reading needs.

    cat.addDescriptor({
        .key = QStringLiteral("launchDirection"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Start direction"),
        .shortLabel = QStringLiteral("Start dir"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The horizontal direction the ball leaves on, relative to the target line. Start "
            "direction is dominated by where the face pointed at impact, which is what makes it the "
            "first question of any miss: a pull and a push are the same swing fault only if the "
            "face agreed with the path."),
        .howToRead = QStringLiteral(
            "Read just after Impact from the ball track. POSITIVE IS RIGHT OF THE TARGET for a "
            "right-handed golfer, negative is left — the same convention as club path and the "
            "alignment lines. It says nothing about curvature, which needs a launch monitor. "
            "Planned, and TWICE blocked. THE BALL TRACK IS NOT A BALL-FLIGHT TRACK: the detector "
            "locks the stationary ball, reports whether it is still at that spot and records the "
            "instant it vanishes, so BallSample2D::center is always the locked spot and never a "
            "ball in the air. What this metric needs is a tracker that FOLLOWS the ball after it "
            "leaves. Beyond that, start direction is "
            "left and right of the target — the depth axis of a face-on camera — so even with a "
            "flight track this one needs a down-the-line or overhead view."),
        .signPositive = QStringLiteral("the ball starting RIGHT of the target line — a push for a right-hander"),
        .signNegative = QStringLiteral("starting left — a pull"),
        .phases = { P::Impact },
        .routes = {
            via("dtlFlight", RM::Triangulated, Direct, { .dtlCamera = true, .ballTrack = true },
                QStringLiteral("where the ball left relative to the target line is a depth-axis "
                               "reading, and it needs a ball-FLIGHT track rather than the at-spot "
                               "detector we have"), PLANNED) },
        // NO usedBy. The pull and push characteristics read the MEASURED start direction
        // (`lm.launchDirection`), because this route is planned optical work and resolves
        // nothing — a claim here would point two characteristics at a metric that never
        // produces a value. It moves back if the optical producer ever lands.
    });

    cat.addDescriptor({
        .key = QStringLiteral("launchAngle"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Launch angle"),
        .shortLabel = QStringLiteral("Launch"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The vertical angle the ball leaves on. Together with ball speed it is most of what "
            "decides carry, and it is the reading that separates a thin strike from a fat one when "
            "both have lost distance: the thin one launches far too low, the fat one loses speed."),
        .howToRead = QStringLiteral(
            "Read just after Impact from the ball track. HIGHER MEANS A HIGHER LAUNCH. Strongly "
            "club-dependent — a driver and a wedge have nothing to say to each other here — so the "
            "corridor is authored per club and reading it at the general context means little. "
            "The angle itself is a face-on reading — the ball rises in the plane the camera sees — "
            "so unlike start direction this needs no second view. Planned all the same, because "
            "THE BALL TRACK IS NOT A BALL-FLIGHT TRACK: the detector locks the stationary ball, "
            "reports whether it is still at that spot and records the instant it vanishes, so "
            "BallSample2D::center is always the locked spot and never a ball in the air. What this "
            "metric needs is a tracker that FOLLOWS the ball after it leaves."),
        .signPositive = QStringLiteral("a higher launch"),
        .signNegative = QStringLiteral("the ball leaving below horizontal"),
        .phases = { P::Impact },
        .routes = {
            via("ballFlight", RM::Projected, Direct, { .faceOnCamera = true, .ballTrack = true },
                QStringLiteral("needs a tracker that follows the ball after it leaves — the "
                               "detector we have locks the stationary ball and reports the "
                               "instant it vanishes"), PLANNED) },
        // NO usedBy. The high- and low-launch characteristics read the MEASURED launch
        // angle (`lm.launchAngle`), because this route is planned optical work and resolves
        // nothing — a claim here would point two characteristics at a metric that never
        // produces a value. It moves back if the optical producer ever lands.
    });

    cat.addDescriptor({
        .key = QStringLiteral("ballSpeed"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Ball speed"),
        .shortLabel = QStringLiteral("Ball spd"),
        .unit = QStringLiteral("mph"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "How fast the ball leaves the face. It is the single best summary of how much of the "
            "clubhead's energy reached the ball, so a collapse in it with an otherwise ordinary "
            "swing is the signature of a strike problem rather than a speed problem."),
        .howToRead = QStringLiteral(
            "Read just after Impact, averaged over several streaks rather than one — a single "
            "frame-pair estimate is noisy. HIGHER IS FASTER. Club- and athlete-dependent, so it is "
            "read against the golfer's own normal rather than a population figure. Planned. THE "
            "BALL TRACK IS NOT A BALL-FLIGHT TRACK. The detector locks the stationary ball, reports "
            "whether it is still at that spot and records the instant it vanishes; "
            "BallSample2D::center is always the locked spot and never a ball in the air. What this "
            "metric needs is a tracker that FOLLOWS the ball after it leaves, which is a different "
            "piece of work from the one we have."),
        .signPositive = QStringLiteral("a faster ball off the face"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("ballFlight", RM::Projected, Direct, { .faceOnCamera = true, .ballTrack = true },
                QStringLiteral("needs a tracker that follows the ball after it leaves — the "
                               "detector we have locks the stationary ball and reports the "
                               "instant it vanishes"), PLANNED) },
        .usedBy = { QStringLiteral("characteristic:ball_speed_deficit") },
    });

    // ---------------------------------------------------- Launch monitor
    //
    // EVERY READING FROM A LAUNCH MONITOR IS KEYED `lm.`, WITHOUT EXCEPTION — including the ones we
    // will never produce ourselves, like face angle. That is not a naming preference; it is the
    // whole design.
    //
    // The monitor's first job here is to be a REFERENCE INSTRUMENT we check our own optical
    // estimates against. The developer guide already names it as the validation instrument for
    // clubheadSpeed, handSpeed, attackAngle, clubPath, launchAngle and ballSpeed. But this ladder
    // resolves exactly ONE winner per key: a Device rung added above `clubheadSpeed`'s camera rung
    // would not sit beside our estimate, it would replace it, and the comparison that justifies
    // owning the device would be gone the moment it was plugged in. So the measurement is a metric
    // of its own and the estimate keeps its bare key untouched. A shot carries both, side by side.
    //
    // WHICH QUANTITIES KEEP A BARE COUNTERPART: those with a non-launch-monitor route in this file,
    // live or planned. `clubheadSpeed` and `attackAngle` we produce today; `ballSpeed`,
    // `launchAngle`, `launchDirection` and `clubPath` are planned optical work we still intend. Each
    // of those six has an `lm.` twin below and the pair IS the validation surface. Everything else
    // here has no bare form at all, because nothing but a device will ever measure it — the nine
    // descriptors that used to carry a PLANNED launchMonitor rung under a bare key were RENAMED
    // rather than promoted, so no key is left behind resolving Unavailable for ever.
    //
    // SIGNS. Foresight states every lateral quantity as POSITIVE-IS-RIGHT for a right-handed golfer,
    // and positive face angle as OPEN. That is self-consistent — face-to-path really is face minus
    // path in its numbers — and it already matches what `faceToPath`, `spinAxis` and
    // `launchDirection` say here. The one place it did NOT match was the old bare `faceAngle`, whose
    // prose said "OPEN IS NEGATIVE AND CLOSED IS POSITIVE" while the metric it feeds said the
    // opposite. `lm.faceAngle` states the device's convention, which resolves that disagreement
    // rather than carrying it forward; the raw block in swing.json and the metric now agree, and a
    // reader comparing the two never has to know which one was negated on the way in.

    cat.addDescriptor({
        .key = QStringLiteral("lm.clubheadSpeed"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Clubhead speed (measured)"),
        .shortLabel = QStringLiteral("Club spd (LM)"),
        .unit = QStringLiteral("mph"),
        .group = QStringLiteral("Club & speed"),
        .description = QStringLiteral(
            "Clubhead speed at impact as a launch monitor measured it. Its whole value beside "
            "`clubheadSpeed` is that the two can be compared: ours is a projected head-path speed "
            "from a single face-on camera and loses whatever the club was doing along the depth "
            "axis, and this is the number that says by how much."),
        .howToRead = QStringLiteral(
            "Read at Impact. HIGHER IS FASTER. Expect our camera estimate to sit BELOW this on most "
            "swings — a projection cannot recover the component pointing at the lens — and treat a "
            "large or inconsistent gap as a statement about the camera setup rather than about the "
            "golfer."),
        .signPositive = QStringLiteral("a faster clubhead"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.ballSpeed"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Ball speed (measured)"),
        .shortLabel = QStringLiteral("Ball spd (LM)"),
        .unit = QStringLiteral("mph"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "How fast the ball left the face, measured. It is the single best predictor of distance "
            "there is, and the numerator of smash factor."),
        .howToRead = QStringLiteral(
            "Read just after Impact. HIGHER IS FURTHER, for a given launch angle and spin. Read it "
            "against clubhead speed rather than alone: a ball speed that is low for the speed that "
            "produced it is a strike problem, not a power problem."),
        .signPositive = QStringLiteral("a faster ball off the face"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor")) },
        // NO usedBy: ball_speed_deficit reads the bare `ballSpeed` measure, which is where
        // the pack has always pointed it. Claiming it here too would have two metrics
        // answering for one characteristic.
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.smashFactor"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Smash factor"),
        .shortLabel = QStringLiteral("Smash"),
        .unit = QStringLiteral("ratio"),
        .group = QStringLiteral("Strike"),
        .description = QStringLiteral(
            "Ball speed divided by clubhead speed — how much of the club's energy reached the ball. "
            "It isolates strike quality from speed: two golfers swinging identically fast can be a "
            "club apart in distance because one finds the middle of the face."),
        .howToRead = QStringLiteral(
            "Read at Impact. HIGHER IS A MORE EFFICIENT STRIKE, with the practical ceiling set by "
            "the club's loft — a driver reaches far higher than a wedge, so this is read per club "
            "and never across them. BOTH SPEEDS COME FROM THE DEVICE. Deriving it from our camera "
            "clubhead speed instead would put a projection error straight into the ratio and read "
            "as a strike fault, which is why there is no bare `smashFactor`."),
        .signPositive = QStringLiteral("a more efficient strike — more of the club's speed reaching the ball"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("ball speed over clubhead speed, both from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:smash_deficit") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.attackAngle"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Attack angle (measured)"),
        .shortLabel = QStringLiteral("AoA (LM)"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "The vertical direction the clubhead was travelling at impact, measured. The twin of "
            "our camera-derived `attackAngle`, and the reference that says whether the low-point "
            "geometry we read off the shaft track is right."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS AN UPWARD STRIKE, negative a descending one — you want "
            "descending with an iron and level-to-upward with a driver. Compare against our own "
            "attack angle on the same shot: a consistent offset is a calibration matter, a scattered "
            "one means the shaft track is not finding the head reliably."),
        .signPositive = QStringLiteral("an UPWARD strike"),
        .signNegative = QStringLiteral("a descending strike"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the vertical club path at impact, read from a launch monitor")) },
        // The measured rung of `m_attackAngle`, which prefers this over our projected
        // `attackAngle` and so carries the same four names.
        .usedBy = { QStringLiteral("characteristic:attack_too_steep"),
                    QStringLiteral("characteristic:attack_too_shallow"),
                    QStringLiteral("characteristic:top"),
                    QStringLiteral("characteristic:sky") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.clubPath"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Club path (measured)"),
        .shortLabel = QStringLiteral("Path (LM)"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "The horizontal direction the clubhead was travelling at impact. With face angle it "
            "settles both halves of ball flight: the path largely chooses the start line and the "
            "face-to-path chooses the curve."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS IN-TO-OUT for a right-handed golfer — travelling right of "
            "the target line — and negative is out-to-in. Zero is neither, which is not the same as "
            "good: a path is only right relative to the shot being played."),
        .signPositive = QStringLiteral("the head travelling RIGHT of the target line — in-to-out for a right-hander"),
        .signNegative = QStringLiteral("travelling left — out-to-in for a right-hander"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the horizontal club path at impact, read from a launch monitor")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.launchAngle"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Launch angle (measured)"),
        .shortLabel = QStringLiteral("Launch (LM)"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The vertical angle the ball left on. Together with ball speed and spin it settles the "
            "whole flight, and it is mostly delivered loft plus a contribution from attack angle."),
        .howToRead = QStringLiteral(
            "Read just after Impact. HIGHER LAUNCHES HIGHER. What is right depends on the club and "
            "on the spin beside it — a high launch with low spin carries, a high launch with high "
            "spin balloons and costs distance."),
        .signPositive = QStringLiteral("a higher launch"),
        .signNegative = QStringLiteral("the ball leaving below horizontal"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:launch_high"),
                    QStringLiteral("characteristic:launch_low") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.launchDirection"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Start direction (measured)"),
        .shortLabel = QStringLiteral("Start (LM)"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The horizontal angle the ball started on, relative to the target line. It is dominated "
            "by face angle, which is what makes it the cleanest evidence of where the face actually "
            "pointed — and what makes a pull and a push readable without any club data at all."),
        .howToRead = QStringLiteral(
            "Read just after Impact. POSITIVE STARTS RIGHT of the target for a right-handed golfer, "
            "negative starts left. Read it with spin axis: start line plus curvature is the whole "
            "shot shape, and either on its own can flatter a miss."),
        .signPositive = QStringLiteral("the ball starting RIGHT of the target line — a push for a right-hander"),
        .signNegative = QStringLiteral("starting left — a pull"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the initial ball direction, read from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:pull"),
                    QStringLiteral("characteristic:push") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.lowPointAhead"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Low point (measured)"),
        .shortLabel = QStringLiteral("Low pt (LM)"),
        .unit = QStringLiteral("in"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "Where the bottom of the swing arc was relative to the ball, measured. The twin of our "
            "camera-derived `lowPointAhead`, and the reference that says whether the arc vertex we "
            "refine out of the shaft track is landing in the right place."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS AHEAD OF THE BALL, on the target side — the descending, "
            "ball-first strike you want with an iron; a driver is normally struck behind it. "
            "Compare against our own low point on the same shot: a consistent offset is a "
            "calibration matter, a scattered one means the arc vertex is not being found reliably."),
        .signPositive = QStringLiteral("the arc bottoming out AHEAD of the ball, on the target side"),
        .signNegative = QStringLiteral("bottoming out behind the ball"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the low point of the swing arc, read from a launch monitor")) },
        // The measured rung of `m_lowPointAhead` — preferred over our own key, same names on both.
        .usedBy = { QStringLiteral("characteristic:low_point_behind_ball"),
                    QStringLiteral("characteristic:low_point_too_far_ahead"),
                    QStringLiteral("characteristic:top"),
                    QStringLiteral("characteristic:sky") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.faceAngle"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Face angle"),
        .shortLabel = QStringLiteral("Face"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "Where the clubface is pointing at impact relative to the target line — the primary "
            "control on where the ball starts, since start direction is dominated by face angle. "
            "Small open/closed differences here are the difference between a fairway and a penalty "
            "area."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS OPEN — pointing right of the target for a right-handed "
            "golfer — and negative is closed. That is the device's own convention and the same one "
            "club path and face-to-path use here, so face minus path really is face-to-path. You "
            "want small, repeatable values matched to the intended path. Not optically resolvable "
            "at our frame rates: a camera can offer only a forearm-and-wrist proxy, which would be "
            "an estimate wearing a measurement's clothes. The lead-wrist measures corroborate it; "
            "they do not stand in for it."),
        .signPositive = QStringLiteral("the face pointing RIGHT of the target line — OPEN for a right-hander"),
        .signNegative = QStringLiteral("pointing left — closed for a right-hander"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor — not optically resolvable at our "
                               "frame rates")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.faceToPath"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Face to path"),
        .shortLabel = QStringLiteral("Face/path"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The face angle relative to the club path at impact. This is the number that decides "
            "which way the ball curves, and it is why a slice and a pull can come from the same "
            "out-to-in swing: the path chose the start line, the face-to-path chose the shape."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE MEANS THE FACE IS OPEN TO THE PATH for a right-handed golfer "
            "— curvature to the right — and negative means closed, curving left. Zero is a straight "
            "shot on whatever line the path started it. Requires a launch monitor: face orientation "
            "at impact is a sub-millisecond event and is not optically resolvable at our frame "
            "rates."),
        .signPositive = QStringLiteral("the face OPEN to the path — curvature to the right, a fade"),
        .signNegative = QStringLiteral("closed to the path — curvature to the left, a draw"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("face angle less club path, from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:closed_face_to_path"),
                    QStringLiteral("characteristic:open_face_to_path") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.dynamicLoft"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Dynamic loft"),
        .shortLabel = QStringLiteral("Dyn loft"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "The loft actually presented to the ball at impact, as opposed to the loft stamped on "
            "the club. Shaft lean removes it and flipping the hands adds it, which is why two "
            "golfers with the same 7-iron can launch it four degrees apart."),
        .howToRead = QStringLiteral(
            "Read at Impact. HIGHER MEANS MORE LOFT DELIVERED — a higher, weaker flight. Requires a "
            "launch monitor: it is derived from face orientation at impact, which is not optically "
            "resolvable at our frame rates. Our `impactShaftLean` is the camera-side corroboration "
            "and moves against this one."),
        .signPositive = QStringLiteral("more loft delivered to the ball"),
        .signNegative = QStringLiteral("the face delofted past square"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.spinLoft"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Spin loft"),
        .shortLabel = QStringLiteral("Spin loft"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "The angle between the delivered loft and the direction the clubhead is travelling. It "
            "is what generates spin: a small spin loft gives a hot, low-spinning strike, a large one "
            "trades speed for spin and height."),
        .howToRead = QStringLiteral(
            "Read at Impact. HIGHER MEANS MORE SPIN AND LESS SPEED for the same clubhead speed. "
            "Dynamic loft less attack angle, both from the device — so it inherits their signs, and "
            "a descending strike ADDS to it rather than subtracting."),
        .signPositive = QStringLiteral("a larger angle between delivered loft and the direction of travel"),
        .signNegative = QStringLiteral("the face delivered below the path direction"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("delivered loft less the vertical club path, from a launch monitor")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.lieAngle"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Lie angle"),
        .shortLabel = QStringLiteral("Lie"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "How the sole sat relative to the ground at impact — toe up or toe down. A club whose "
            "lie does not match the golfer points the face off line at impact even when everything "
            "else is square, so it is a fitting question that reads as a swing fault."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS TOE UP and negative is toe down, with zero a sole sitting "
            "flat to the ground — Foresight's own published convention, which we take unchanged. A "
            "persistent offset across many shots with the same club is a fitting finding; "
            "shot-to-shot scatter is a delivery one."),
        .signPositive = QStringLiteral("the clubhead sole toe UP relative to the ground at impact"),
        .signNegative = QStringLiteral("the sole toe DOWN; zero is flat to the ground"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the sole's angle to the ground at impact, read from a launch monitor")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.closureRate"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Closure rate"),
        .shortLabel = QStringLiteral("Closure"),
        .unit = QStringLiteral("°/s"),
        .group = QStringLiteral("Club delivery"),
        .description = QStringLiteral(
            "How fast the face was rotating through impact. It is the reason two golfers with the "
            "same average face angle can differ enormously in consistency: the faster the face is "
            "turning, the more a few milliseconds of timing costs in face angle."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS CLOSING — Foresight's published convention: a higher "
            "positive value is a faster closing face through the hitting area, and a low or stable "
            "one a squarer, held-off release. LOWER IS MORE FORGIVING, because it makes face angle "
            "less sensitive to timing. A high closure rate with good face numbers is a golfer "
            "relying on timing to be repeatable. The device may report this in degrees per second "
            "OR in revolutions per minute; the connector reads the unit the header declares and "
            "converts, so the number here is always degrees per second."),
        .signPositive = QStringLiteral("the face rotating CLOSED through impact — a faster closing rate"),
        .signNegative = QStringLiteral("the face rotating open; a low or stable value is a squarer, held-off release"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the face's rotation rate through impact, read from a launch monitor")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.strikeLocation"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Strike location"),
        .shortLabel = QStringLiteral("Strike"),
        .unit = QStringLiteral("mm"),
        .group = QStringLiteral("Strike"),
        .description = QStringLiteral(
            "Where on the face the ball was struck, across the heel-toe axis. Off-centre contact "
            "bleeds speed and, through gear effect, curves the ball in the opposite direction to "
            "the miss — which is why a toe strike can draw and a heel strike fade from one swing."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS TOWARD THE TOE, negative toward the heel, zero at the "
            "centre of the face. Requires a launch monitor, or face impact markers: the impact "
            "position on the face is not resolvable from any camera view we take."),
        .signPositive = QStringLiteral("struck toward the TOE"),
        .signNegative = QStringLiteral("struck toward the heel"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the face-impact position, from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:strike_heel"),
                    QStringLiteral("characteristic:strike_toe"),
                    QStringLiteral("characteristic:shank") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.strikeHeight"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Strike height"),
        .shortLabel = QStringLiteral("Strike ht"),
        .unit = QStringLiteral("mm"),
        .group = QStringLiteral("Strike"),
        .description = QStringLiteral(
            "Where on the face the ball was struck up the crown-sole axis. It is the other half of "
            "strike location and the one that explains spin surprises: high on a driver face gives "
            "the launch-high, spin-low strike that carries, low gives the opposite."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS ABOVE CENTRE, negative below. Read it alongside spin rate "
            "and launch angle rather than alone — its whole effect is on those two. The two ends of "
            "it are the two low-point misses: a strike well BELOW centre is the leading edge "
            "catching the ball's equator (thin), and one well above it is the club already climbing "
            "by the time it arrives, which is what the ground being struck first does (chunk)."),
        .signPositive = QStringLiteral("struck ABOVE the centre of the face"),
        .signNegative = QStringLiteral("struck below centre"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the face-impact height, from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:thin"),
                    QStringLiteral("characteristic:chunk"),
                    // `top` and `sky` are those two plus a low point and an attack angle. The strike
                    // height is one conjunct of three, and recording it here is how the directory
                    // can answer "what would I lose without this reading" truthfully.
                    QStringLiteral("characteristic:top"),
                    QStringLiteral("characteristic:sky") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.spinRate"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Spin rate"),
        .shortLabel = QStringLiteral("Spin"),
        .unit = QStringLiteral("rpm"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "How fast the ball is spinning as it leaves — the TOTAL spin, back and side combined. "
            "Too much costs distance and makes the flight balloon; too little costs the height and "
            "stopping power a shot needs to hold a green."),
        .howToRead = QStringLiteral(
            "Read just after Impact. HIGHER IS MORE SPIN. Strongly club-dependent — what is a "
            "knuckleball for a wedge is a spinny drive — so the corridor is authored per club. "
            "Requires a launch monitor: spin is not measurable over the short flight an indoor "
            "capture sees."),
        .signPositive = QStringLiteral("more total spin"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:spin_deficit"),
                    QStringLiteral("characteristic:spin_excess") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.backSpin"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Back spin"),
        .shortLabel = QStringLiteral("Backspin"),
        .unit = QStringLiteral("rpm"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The component of spin about the horizontal axis — the part that generates lift and "
            "makes the ball hold its height and stop on landing."),
        .howToRead = QStringLiteral(
            "Read just after Impact. HIGHER LIFTS AND STOPS MORE. Its value here is mostly as one "
            "of the two components behind spin axis; total spin is the number to grade against."),
        .signPositive = QStringLiteral("more backspin"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.sideSpin"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Side spin"),
        .shortLabel = QStringLiteral("Sidespin"),
        .unit = QStringLiteral("rpm"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The component of spin about the vertical axis — the part that curves the ball left or "
            "right."),
        .howToRead = QStringLiteral(
            "Read just after Impact. POSITIVE CURVES RIGHT for a right-handed golfer. Spin axis is "
            "the better number to read for shape, because it states the curvature relative to the "
            "total spin rather than in absolute rpm that mean different things at different speeds."),
        .signPositive = QStringLiteral("spin curving the ball RIGHT"),
        .signNegative = QStringLiteral("curving the ball left"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.spinAxis"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Spin axis"),
        .shortLabel = QStringLiteral("Spin axis"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The tilt of the ball's axis of rotation, which is what curves the flight. It is the "
            "outcome the golfer actually sees, where face-to-path is the cause of it at impact."),
        .howToRead = QStringLiteral(
            "Read just after Impact. POSITIVE TILTS RIGHT for a right-handed golfer — a fade or a "
            "slice — and negative tilts left. Computed from the device's own side and back spin, so "
            "it carries their signs. Requires a launch monitor: the curvature develops over a "
            "flight an indoor capture never sees."),
        .signPositive = QStringLiteral("the axis tilted RIGHT — a fade or a slice"),
        .signNegative = QStringLiteral("tilted left — a draw or a hook"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("the tilt of side spin against back spin, from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:hook"),
                    QStringLiteral("characteristic:slice") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.carryDistance"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Carry"),
        .shortLabel = QStringLiteral("Carry"),
        .unit = QStringLiteral("yd"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "How far the ball flies before it lands. It is the number a golfer plans a round with, "
            "and the one every other ball-flight metric is ultimately serving."),
        .howToRead = QStringLiteral(
            "Read after Impact. HIGHER IS FURTHER. Club- and athlete-dependent, so it is read "
            "against the golfer's own normal for that club. A FLIGHT-MODEL OUTPUT, not an "
            "observation: indoors the ball is stopped by a screen a few metres away, and every "
            "carry number anywhere comes from launch conditions plus a model."),
        .signPositive = QStringLiteral("the ball carrying further"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor's flight model")) },
        .usedBy = { QStringLiteral("characteristic:carry_deficit") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.totalDistance"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Total distance"),
        .shortLabel = QStringLiteral("Total"),
        .unit = QStringLiteral("yd"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "Carry plus roll. It is the more variable of the two by a long way, because roll depends "
            "on ground the simulator is assuming rather than measuring."),
        .howToRead = QStringLiteral(
            "Read after Impact. HIGHER IS FURTHER. Prefer carry when comparing swings: total "
            "distance moves with the simulated surface as much as with the strike, so a change here "
            "with no change in carry says nothing about the golfer."),
        .signPositive = QStringLiteral("the ball finishing further away"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor's flight model")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.offline"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Offline"),
        .shortLabel = QStringLiteral("Offline"),
        .unit = QStringLiteral("yd"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "How far left or right of the target line the ball finished. It is the outcome that "
            "start direction and spin axis jointly produce, and so an INDEPENDENT WITNESS to both: "
            "a shot shape read off the club numbers should agree with where the ball actually went, "
            "and when it does not, one of the two is wrong."),
        .howToRead = QStringLiteral(
            "Read after Impact. POSITIVE IS RIGHT of the target for a right-handed golfer, negative "
            "left. Read with start direction to separate the two ways of missing right: started "
            "right and flew straight is a push, started straight and curved is a slice."),
        .signPositive = QStringLiteral("finishing RIGHT of the target line"),
        .signNegative = QStringLiteral("finishing left of it"),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor's flight model")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.peakHeight"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Peak height"),
        .shortLabel = QStringLiteral("Apex"),
        .unit = QStringLiteral("ft"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "The highest point of the flight. It is the readable symptom of the launch-and-spin "
            "pair: a ballooning flight shows up here long before the golfer notices the distance "
            "it is costing."),
        .howToRead = QStringLiteral(
            "Read after Impact. HIGHER IS A HIGHER FLIGHT. Stated in feet, as golf states apex. "
            "Read with spin rate — a high apex from speed is not the same finding as a high apex "
            "from spin, and only the second one is costing anything."),
        .signPositive = QStringLiteral("a higher flight"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor's flight model")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.descentAngle"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Descent angle"),
        .shortLabel = QStringLiteral("Descent"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "How steeply the ball came down. It is what decides whether a shot holds the green it "
            "lands on, and it is the reason a long iron that carries far enough can still be the "
            "wrong club into a firm green."),
        .howToRead = QStringLiteral(
            "Read after Impact. HIGHER IS STEEPER, and steeper stops faster. Club-dependent: a "
            "wedge should come down steeply and a driver should not."),
        .signPositive = QStringLiteral("a steeper descent — the ball stopping faster"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor's flight model")) },
    });

    cat.addDescriptor({
        .key = QStringLiteral("lm.distanceToPin"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Distance to pin"),
        .shortLabel = QStringLiteral("To pin"),
        .unit = QStringLiteral("yd"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "How far the ball finished from the pin the simulator was playing to. Unlike everything "
            "else here it is a property of the SHOT THE SOFTWARE SET UP as much as of the swing — "
            "change the hole and it changes with nothing else moving."),
        .howToRead = QStringLiteral(
            "Read after Impact. LOWER IS CLOSER. NEVER COMPARE IT ACROSS SHOTS unless the target "
            "was the same: it is meaningful within a session aimed at one pin, and meaningless as "
            "an aggregate. Offline is the one to read for dispersion, because it is stated relative "
            "to the target line rather than to a distance."),
        .signPositive = QStringLiteral("finishing further from the pin"),
        .signNegative = QString(),
        .phases = { P::Impact },
        .routes = {
            via("launchMonitor", RM::Device, Direct, { .launchMonitor = true },
                QStringLiteral("read from a launch monitor's flight model")) },
    });

    // ------------------------------------------- Derived from launch-monitor readings
    //
    // THE FIRST DESCRIPTOR HERE THAT NEEDS A LAUNCH MONITOR AND IS NOT A READING, and the naming
    // follows from that rather than from where the numbers came in. `lm.` means THE DEVICE SAID
    // THIS — the whole section above rests on it, and the catalogue test enforces it in both
    // directions: no reading field may lack a descriptor, and no `lm.` descriptor may exist that no
    // reading field can fill. A quantity we compute has no field to be filled from, so an `lm.` key
    // would fail that check, and rightly: the device did not say this, we did.
    //
    // So it is a bare key with a Device rung. There is no bare-versus-`lm.` pair to worry about
    // here — the pairing rule above exists to stop a device rung REPLACING an optical estimate of
    // the same quantity, and nothing optical will ever estimate this one, because it is made of
    // spin axis and carry.

    cat.addDescriptor({
        .key = QStringLiteral("compoundMiss"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Compound miss"),
        .shortLabel = QStringLiteral("Compound"),
        .unit = QStringLiteral("ratio"),
        .group = QStringLiteral("Ball flight"),
        .description = QStringLiteral(
            "Whether the ball started off line and then curved FURTHER THE SAME WAY — the "
            "pull-hook and the push-slice, the two misses with nothing at impact aimed anywhere "
            "near the target. It is one number rather than two because a diagnosis needs the "
            "conjunction: a shot that starts left and fades is a shape, and only a shot that starts "
            "left and keeps going left is the smother."),
        .howToRead = QStringLiteral(
            "Read at Impact. POSITIVE IS STARTED AND CURVED RIGHT, negative both left, and ZERO "
            "when the two disagree — a pull-fade reads 0, not a small number, because the halves "
            "cancelling is what makes it a different shot. Stated as a RATIO against the "
            "boundaries: each half is divided by the point its own word starts to apply, so BEYOND "
            "±1 MEANS BOTH HALVES CLEARED THEIR OWN THRESHOLD and 0.9 means one of them did not. "
            "Read it beside start direction and spin axis, which are the two halves themselves."),
        .signPositive = QStringLiteral("started RIGHT and curved further right — the push-slice"),
        .signNegative = QStringLiteral("started left and curved further left — the pull-hook"),
        .phases = { P::Impact },
        .routes = {
            via("compoundMiss", RM::Derived, Direct, { .launchMonitor = true },
                QStringLiteral("start direction against measured curvature, from a launch monitor")) },
        .usedBy = { QStringLiteral("characteristic:pull_hook"),
                    QStringLiteral("characteristic:push_slice") },
    });
}

} // namespace pinpoint::analysis
