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
// catalogue (shot_analyzer_design.md §A), each metric either LIVE or a PLANNED placeholder.
//
// LIVE (21) — a producer emits it today: metric_extractor ×4, kinematic_series ×3 + shaft-lean,
//   foot_metrics ×5 + ball_position ×1, head_track ×3, tempo_metrics ×2, plus wristScore /
//   wristResemblance (Summary, from a ScoreBreakdown, Wrist session).
// PLANNED (22, `.planned = true`) — in the design catalogue but no producer in this build: the
//   whole-body rotation / spine / pelvis / club-delivery / kinematic-sequence / alignment
//   metrics and swingScore. The PlannedMetricProvider claims these so they resolve "planned", and
//   their `.requirement` reads as "will need …" on the detail page.
//
// See the metric-catalogue developer guide for promoting a placeholder to live (add the producer,
// drop `.planned`, move the key from PlannedMetricProvider to a real provider).
//
// `description` (what it is + why it matters) and `howToRead` (sign, what good looks like, caveats)
// are written as coach-facing narratives for the directory detail page; the biomechanics, formulae
// and reference ranges draw on docs/design/shot_analyzer_design.md, docs/reference/wristmetrics.md,
// docs/reference/golf_swing_normative_reference.md and docs/reference/swing_json_schema.md.
//
// requirement.minTier stays Angles2D for every metric: wrist DOFs come from the fused IMU regardless
// of camera reconstruction tier, and the speed/foot metrics are 2D face-on — gating is by IMU role /
// face-on camera / club track, never by tier. Wrist-Motion-session gating lives in the providers.

namespace pinpoint::analysis {

void installMetricManifest(MetricCatalogue &cat)
{
    using P = Phase;
    using R = SegmentRole;

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
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::LeadForearm, R::LeadHand } },
        .usedBy = { QStringLiteral("review:verdict"), QStringLiteral("shotlist:score") },
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
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::LeadForearm, R::LeadHand } },
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
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
        .usedBy = { QStringLiteral("review:verdict"), QStringLiteral("shotlist:score") },
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
        .phases = { P::Top, P::Impact },
        .scored = true,
        .normative = { .dof = PpJointDof::LeadWristFlexExt,
                       .contextNote = QStringLiteral("mid-iron · neutral archetype"),
                       .heuristic = true },
        .requirement = { .imuRoles = { R::LeadForearm, R::LeadHand } },
        .usedBy = { QStringLiteral("chart:review"), QStringLiteral("score:wrist"),
                    QStringLiteral("assessment:wrist") },
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
        .phases = { P::Top, P::Impact },
        .scored = true,
        .normative = { .dof = PpJointDof::LeadWristRadUln,
                       .contextNote = QStringLiteral("mid-iron · neutral archetype"),
                       .heuristic = true },
        .requirement = { .imuRoles = { R::LeadForearm, R::LeadHand } },
        .usedBy = { QStringLiteral("chart:review"), QStringLiteral("score:wrist"),
                    QStringLiteral("assessment:wrist") },
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
        .phases = { P::Top, P::Impact },
        .scored = true,
        .normative = { .dof = PpJointDof::LeadForearmRot,
                       .contextNote = QStringLiteral("mid-iron · neutral archetype"),
                       .heuristic = true },
        .requirement = { .imuRoles = { R::LeadForearm, R::LeadHand, R::LeadUpperArm } },
        .usedBy = { QStringLiteral("chart:review"), QStringLiteral("score:wrist"),
                    QStringLiteral("assessment:wrist") },
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
        .phases = { P::Top, P::Impact },
        .scored = true,
        .normative = { .dof = PpJointDof::LeadElbowFlex,
                       .contextNote = QStringLiteral("mid-iron · neutral archetype"),
                       .heuristic = true },
        .requirement = { .imuRoles = { R::LeadForearm, R::LeadHand, R::LeadUpperArm } },
        .usedBy = { QStringLiteral("chart:review"), QStringLiteral("score:wrist"),
                    QStringLiteral("assessment:wrist") },
    });

    // ---------------------------------------------------- Body rotation (PLANNED — body IMUs, no producer)

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
            "chest open is a hallmark of an efficient downswing. Planned: it needs a dedicated "
            "pelvis IMU, which today's placement slots do not yet provide."),
        .phases = { P::Top, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::Pelvis } },
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
            "the power story lives (see X-factor). Planned: needs a thorax IMU."),
        .phases = { P::Top, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::Thorax } },
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
            "much the gap grows early in the downswing. Planned: needs pelvis and thorax IMUs."),
        .phases = { P::Top },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::Pelvis, R::Thorax } },
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
            "sequencing, not more turn. Planned: needs pelvis and thorax IMUs."),
        .phases = { P::Transition, P::Downswing },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::Pelvis, R::Thorax } },
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
        .phases = { P::Top, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::Pelvis, R::LeadThigh, R::TrailThigh } },
    });

    // ------------------------------------------------ Spine & pelvis (PLANNED — camera-3D / fused)

    cat.addDescriptor({
        .key = QStringLiteral("spineForwardBend"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Spine forward bend"),
        .shortLabel = QStringLiteral("Fwd bend"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Spine & pelvis"),
        .description = QStringLiteral(
            "The forward tilt of the trunk over the ball — the flexion/extension of the thorax "
            "relative to the pelvis — which sets the posture the whole swing rotates around. Losing "
            "this angle (standing up) or adding to it (dipping) through the downswing changes the "
            "low point and the strike."),
        .howToRead = QStringLiteral(
            "Read at Address and Impact; the aim is to retain most of the address posture, with "
            "irons commonly holding around 30–40° into impact. A loss of forward bend into impact "
            "is early extension and pairs with the pelvis-thrust metric; too much is a dip that "
            "moves the low point. Planned: needs pelvis and thorax IMUs (or a calibrated 3D camera)."),
        .phases = { P::Address, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::Pelvis, R::Thorax } },
    });

    cat.addDescriptor({
        .key = QStringLiteral("spineSideBend"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Spine side bend"),
        .shortLabel = QStringLiteral("Side bend"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Spine & pelvis"),
        .description = QStringLiteral(
            "Lateral flexion of the trunk toward the trail side — the side-bend of the thorax "
            "relative to the pelvis — which naturally appears in the downswing as the trail "
            "shoulder works down and under. It is closely tied to attack angle and to hitting up or "
            "down on the ball."),
        .howToRead = QStringLiteral(
            "Read at Impact; at driver impact the thorax commonly shows around 32° of side bend "
            "versus about 10° at the pelvis. Too little side bend often goes with a steep, "
            "over-the-top delivery; too much can throw the low point behind the ball. Planned: "
            "needs a face-on camera (or IMUs)."),
        .phases = { P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
    });

    cat.addDescriptor({
        .key = QStringLiteral("secondaryAxisTilt"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Secondary axis tilt"),
        .shortLabel = QStringLiteral("Axis tilt"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Spine & pelvis"),
        .description = QStringLiteral(
            "How much the spine leans away from the target at impact — the angle of the mid-hip-to-"
            "mid-shoulder line from vertical in the frontal (face-on) plane. It reflects the "
            "trail-side tilt that lets the club approach from the inside and, for the driver, on a "
            "slight upswing."),
        .howToRead = QStringLiteral(
            "Read at Impact. Players tend to set roughly 6–8° of tilt at address and increase it to "
            "about 20–25° by impact; a driver wants more of this than an iron. Too little tilt at "
            "impact is a classic reverse-pivot or early-extension signature. Planned: needs a "
            "face-on camera."),
        .phases = { P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
    });

    cat.addDescriptor({
        .key = QStringLiteral("pelvisSway"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Pelvis sway"),
        .shortLabel = QStringLiteral("Sway"),
        .unit = QStringLiteral("cm"),
        .group = QStringLiteral("Spine & pelvis"),
        .description = QStringLiteral(
            "How far the pelvis slides laterally along the target line, toward or away from the "
            "target, relative to address — the linear partner to pelvis rotation. A little pressure "
            "shift is powerful; too much slide replaces rotation and hurts consistency."),
        .howToRead = QStringLiteral(
            "Read near the top and at Impact. A good pattern moves slightly away from the target in "
            "the backswing and then toward it in the downswing (a pressure shift), returning near "
            "or just ahead of address by impact. Excessive sway away going back usually costs turn "
            "and centredness of strike. Planned: needs a face-on camera and a calibrated ground plane."),
        .phases = { P::Top, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
    });

    cat.addDescriptor({
        .key = QStringLiteral("pelvisThrust"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Pelvis thrust"),
        .shortLabel = QStringLiteral("Thrust"),
        .unit = QStringLiteral("cm"),
        .group = QStringLiteral("Spine & pelvis"),
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
        .phases = { P::Downswing, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true, .minTier = ReconstructionTier::Stereo3D },
    });

    cat.addDescriptor({
        .key = QStringLiteral("pelvisLift"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Pelvis lift"),
        .shortLabel = QStringLiteral("Lift"),
        .unit = QStringLiteral("cm"),
        .group = QStringLiteral("Spine & pelvis"),
        .description = QStringLiteral(
            "How much the pelvis rises or drops vertically relative to address — the up/down "
            "component of pelvis motion. Some rise through impact is part of a powerful, "
            "ground-force-driven action; an uncontrolled early rise is another face of early "
            "extension."),
        .howToRead = QStringLiteral(
            "Read at Impact. A small, controlled rise as the player pushes off the ground is normal "
            "and even desirable; what you are watching for is an early or excessive lift that pulls "
            "the club off its path. Read it alongside pelvis thrust and spine forward bend. "
            "Planned: needs a face-on camera and a calibrated ground plane."),
        .phases = { P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
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
            "How fast the clubhead is travelling near impact — the magnitude of its velocity, taken "
            "as a central difference of the tracked head position and scaled to real-world units "
            "on the ground plane. It is the headline power number and the biggest single driver of "
            "distance."),
        .howToRead = QStringLiteral(
            "Read the peak, which occurs right around impact. As references, tour drivers run about "
            "113 mph and 7-irons about 89 mph (TrackMan 2024), but the right number is club- and "
            "player-dependent. On a single face-on camera this is an in-plane estimate, so treat "
            "motion along the depth axis as approximate. Needs face-on club tracking."),
        .phases = { P::Impact },
        .normative = { .contextNote = QStringLiteral("club-dependent — see reference norms"),
                       .heuristic = true },
        .requirement = { .faceOnCamera = true, .clubTrack = true },
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
        .phases = { P::Impact },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true, .clubTrack = true },
        .usedBy = { QStringLiteral("chart:review") },
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
        .phases = { P::Downswing, P::Impact },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true, .clubTrack = true },
        .usedBy = { QStringLiteral("chart:review") },
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
        .phases = { P::Impact },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true, .clubTrack = true },
        .usedBy = { QStringLiteral("chart:review") },
    });

    // ------------------------------------------------ Club delivery (PLANNED — club track / DTL)

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
        .phases = { P::Downswing },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .clubTrack = true, .minTier = ReconstructionTier::Stereo3D },
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
            "Read at Impact; most good iron shots sit within a few degrees either side of zero, "
            "with the desired path depending on the shape being played. The discriminating axis is "
            "the optical (toward-ball) axis, which is exactly what a face-on camera cannot see — "
            "this is a canonical down-the-line metric. Planned: needs the club track and a DTL camera."),
        .phases = { P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .clubTrack = true, .minTier = ReconstructionTier::Stereo3D },
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
            "Read at Impact. References run about −1.3° for the driver (many good drives are "
            "positive, +3 to +5°) and about −4.5° for a 7-iron (TrackMan). A too-steep iron angle "
            "digs and loses speed; a downward driver angle costs carry. A down-the-line camera "
            "makes it fully in-plane. Planned: needs the club track."),
        .phases = { P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .clubTrack = true, .minTier = ReconstructionTier::Stereo3D },
    });

    cat.addDescriptor({
        .key = QStringLiteral("faceAngle"),
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
            "Read at Impact; you want small, repeatable open/closed values matched to the intended "
            "path. Face angle really needs a club-mounted device or full club tracking to measure "
            "directly — a camera alone can only offer a forearm-and-wrist proxy, which must be "
            "clearly labelled as an estimate. Planned: needs club instrumentation."),
        .phases = { P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .clubTrack = true, .minTier = ReconstructionTier::ClubInstrumented },
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
            "number a lone face-on camera can estimate."),
        .howToRead = QStringLiteral(
            "Read near impact; a positive value (low point ahead of the ball) is the descending, "
            "ball-first strike you want with irons, while the driver is normally struck with the "
            "low point behind the ball. A low point behind the ball on an iron is the fat/thin "
            "signature. Planned: needs a face-on camera with shaft-head and ball tracking, and is "
            "deferred until the measured-clubhead detector lands so the head is measured, not projected."),
        .phases = { P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true, .clubTrack = true, .ballTrack = true },
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
        .phases = { P::Impact },
        .normative = { .heuristic = true },
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
        .phases = { P::Impact },
        .normative = {
            // The FIRST inline corridor in the manifest. Non-DOF, so it cannot
            // delegate to reference_bands (guide step D).
            //
            // PROVISIONAL. These are the published tour figures, and they are
            // measured Takeaway→Top while this metric is Address→Top — so the
            // band sits slightly low for this basis by the (small, structurally
            // bounded, but so far UNMEASURED) Address→Takeaway gap. Re-centre it
            // from the corpus distribution before treating it as authoritative.
            .inlineCorridors = { { .phase = P::Impact,
                                   .greenLo = 2.2, .greenHi = 3.0,
                                   .amberLo = 1.8, .amberHi = 3.6,
                                   .deltaFromAddress = false } },
            .contextNote = QStringLiteral(
                "Measured from address to the top, then top to impact. Published tour figures are "
                "measured from the takeaway instead, so this reads a little higher than the 3:1 "
                "benchmark; the corridor is provisional until the difference is measured on a "
                "corpus."),
            .heuristic = true,
        },
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
        .phases = { P::Transition, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .imuRoles = { R::Pelvis, R::Thorax, R::LeadForearm }, .clubTrack = true },
    });

    // ------------------------------------------------ Feet & stance (whole-body pose, face-on, 2D)

    cat.addDescriptor({
        .key = QStringLiteral("stanceWidth"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Stance width"),
        .shortLabel = QStringLiteral("Stance"),
        .unit = QStringLiteral("mm"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How wide the feet are set at address, measured heel-to-heel from the whole-body pose. "
            "Stance width is a foundation of balance and turn: too narrow costs stability, too wide "
            "restricts the hips."),
        .howToRead = QStringLiteral(
            "This is a single setup measurement, taken at address, with wider stances suiting the "
            "longer clubs and narrower ones the wedges. Real-world millimetres come from the golf "
            "ball itself, whose diameter is fixed by the rules — so the reading depends on the ball "
            "being detected at address. Without it the metric falls back to a fraction of the frame "
            "width, which is only comparable within one camera setup; check the unit before "
            "comparing two swings. Needs a face-on whole-body camera."),
        .phases = { P::Address },
        // No corridor yet: the metric only just gained real-world units, so there
        // is no measured distribution to draw a defensible band from.
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("ballPosition"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Ball position"),
        .shortLabel = QStringLiteral("Ball pos"),
        .unit = QStringLiteral("%"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "Where the ball sits along the stance at address, as a percentage of stance width: 0 % "
            "is level with the lead heel, 100 % level with the trail heel. Ball position sets the "
            "low point of the swing arc relative to the ball, which is why the same swing produces "
            "very different strikes as it moves."),
        .howToRead = QStringLiteral(
            "Read it against the club in hand rather than against a single ideal: the driver wants "
            "the ball forward, off the lead heel, while the wedges want it closer to the middle of "
            "the stance. Values below 0 % are normal and mean the ball is forward of the lead heel. "
            "Because this is a ratio of two distances in the same plane it is directly comparable "
            "between swings and cameras, unlike stance width itself. Needs a face-on camera and a "
            "detected ball at address."),
        .phases = { P::Address },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true, .ballTrack = true },
        .usedBy = { QStringLiteral("chart:review") },
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
        .phases = { P::Address },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
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
        .phases = { P::Address },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
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
            "set open, square or closed to the intended line."),
        .howToRead = QStringLiteral(
            "A single address measurement of stance alignment (open / square / closed) in the image "
            "plane. Because it is measured face-on it reads the apparent line rather than true "
            "target-line alignment, which a down-the-line or overhead view would resolve more "
            "directly. Needs a face-on whole-body camera."),
        .phases = { P::Address },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("leadHeelLift"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Lead heel lift"),
        .shortLabel = QStringLiteral("Heel lift"),
        .unit = QStringLiteral("×frame"),
        .group = QStringLiteral("Feet & stance"),
        .description = QStringLiteral(
            "How far the lead heel rises off the ground through the swing, relative to address, as "
            "a fraction of frame height (positive when the heel lifts). Some players anchor both "
            "heels; others let the lead heel come up in the backswing to allow a bigger turn — both "
            "can work."),
        .howToRead = QStringLiteral(
            "This is a per-frame curve, usually read for how much the heel comes up around the top. "
            "A little lift is common and can free up the backswing turn; keeping the heel down is a "
            "legitimate stylistic choice for stability. Read the trend rather than any single "
            "value. Needs a face-on whole-body camera."),
        .phases = { P::Top },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
        .usedBy = { QStringLiteral("chart:review") },
    });

    // -------------------------------------------- Alignment (PLANNED — pose lines at address/impact)

    cat.addDescriptor({
        .key = QStringLiteral("shoulderAlignment"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Shoulder alignment"),
        .shortLabel = QStringLiteral("Shoulders"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Alignment"),
        .description = QStringLiteral(
            "Which way the shoulder line points — the angle of the line joining the lead and trail "
            "shoulders in the image plane — read at address and again at impact. The shoulders are "
            "the most influential alignment line for a player's start direction, and how they "
            "return at impact tells a different story from how they were set."),
        .howToRead = QStringLiteral(
            "Read at Address and Impact. A common pattern is close to square at address and a touch "
            "open by impact as the upper body clears; shoulders open at address, or slammed wide "
            "open at impact, often signal an out-to-in delivery. Planned: needs a face-on camera "
            "for the image-plane line, with a down-the-line view giving true target-line alignment."),
        .phases = { P::Address, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
    });

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
            "way down). Read it together with lead-arm flexion and shoulder alignment. Planned: "
            "needs a face-on camera."),
        .phases = { P::Address, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
    });

    cat.addDescriptor({
        .key = QStringLiteral("hipAlignment"),
        .type = MetricType::PointInTime,
        .label = QStringLiteral("Hip alignment"),
        .shortLabel = QStringLiteral("Hips"),
        .unit = QStringLiteral("°"),
        .group = QStringLiteral("Alignment"),
        .description = QStringLiteral(
            "Which way the hip line points — the angle of the line joining the lead and trail hips "
            "in the image plane — read at address and at impact. The hips both set a player's aim "
            "and, by how far they open by impact, reveal how well the lower body is leading the "
            "downswing."),
        .howToRead = QStringLiteral(
            "Read at Address and Impact. Near-square at address is typical, and by impact the hips "
            "are usually more open than the shoulders — a lower body that clears ahead of the upper "
            "body is a good sign, whereas hips that stay closed into impact often force the arms to "
            "take over. Planned: needs a face-on camera (down-the-line for true target-line alignment)."),
        .phases = { P::Address, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
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
            "trail ankles in the image plane — read at address and at impact. It complements the "
            "address-only toe line by adding an impact read and by using the ankle joints rather "
            "than the toes, so it is less affected by foot flare."),
        .howToRead = QStringLiteral(
            "Read at Address and Impact. At address it reports the stance line (open / square / "
            "closed); the impact read shows how the feet and lower legs have worked — for example "
            "the trail foot rolling and the ankles re-orienting as the player pushes off. Planned: "
            "needs a face-on camera."),
        .phases = { P::Address, P::Impact },
        .planned = true,
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
    });

    // ---------------------------------------------------- Head (whole-body pose, face-on, 2D; live)

    cat.addDescriptor({
        .key = QStringLiteral("headSway"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Head sway"),
        .shortLabel = QStringLiteral("Head sway"),
        .unit = QStringLiteral("×frame"),
        .group = QStringLiteral("Head"),
        .description = QStringLiteral(
            "How much the head moves side-to-side relative to address, as a fraction of frame width "
            "so it is camera-distance independent. The head is a convenient, stable proxy for "
            "whether the upper body is staying centred: rotating around a steady head is efficient, "
            "while sliding the head off the ball tends to move the low point."),
        .howToRead = QStringLiteral(
            "This is a per-frame curve; some lateral movement (especially a small shift back and "
            "through) is normal, and only excessive sway is a fault. Read the trend and the peak "
            "rather than any single frame, and pair it with pelvis sway to see whether the whole "
            "body is sliding. Needs a face-on camera; Wrist Motion session."),
        .phases = { P::Top, P::Impact },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
        .usedBy = { QStringLiteral("chart:review") },
    });

    cat.addDescriptor({
        .key = QStringLiteral("headLift"),
        .type = MetricType::TimeSeries,
        .label = QStringLiteral("Head lift"),
        .shortLabel = QStringLiteral("Head lift"),
        .unit = QStringLiteral("×frame"),
        .group = QStringLiteral("Head"),
        .description = QStringLiteral(
            "How much the head rises or drops relative to address, as a fraction of frame width "
            "(positive when it rises). Vertical head movement is an early, easy-to-see indicator of "
            "standing up out of posture or dipping into the ball, both of which change the strike."),
        .howToRead = QStringLiteral(
            "A per-frame curve read against the address height. A steady head is ideal; an early "
            "rise through the downswing points toward standing up / early extension, while a dip "
            "suggests a drop into the shot. Read it alongside spine forward bend and pelvis lift. "
            "Needs a face-on camera; Wrist Motion session."),
        .phases = { P::Top, P::Impact },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
        .usedBy = { QStringLiteral("chart:review") },
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
        .phases = { P::Top, P::Impact },
        .normative = { .heuristic = true },
        .requirement = { .faceOnCamera = true },
        .usedBy = { QStringLiteral("chart:review") },
    });
}

} // namespace pinpoint::analysis
