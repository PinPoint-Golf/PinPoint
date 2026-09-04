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

#pragma once

#include "metric_type.h"
#include "swing_analysis.h"          // Phase, SegmentRole, ReconstructionTier
// wrist_assessment_types.h is no longer needed HERE (PpJointDof left with MetricNormative at stage 9)
// but several includers of this header have always got PpJointDof / PpSwingPosition through it.
#include "wrist_assessment_types.h"

#include <QString>
#include <QStringList>

#include <vector>

// MetricDescriptor — the stable identity + metadata of a metric (design §3.4). Constant for every
// shot; never names a producer (design principle 1). Per-shot availability is resolved separately
// (metric_provider.h). All header-only value types, Qt-only, no Qt-GUI.

namespace pinpoint::analysis {

// What ONE ROUTE needs before it can produce a metric (design §3.2). A requirement never names a
// provider. When nothing satisfies it, the resolver renders it into a human-readable availability
// reason ("needs a face-on camera").
struct MetricRequirement {
    bool                     faceOnCamera = false;                     // pose / shaft / ball products
    // A second camera down the target line. The face-on view's blind axis is depth, so anything
    // stated along it — club path, pelvis thrust, ball standoff — needs this view or a stereo
    // reconstruction built from it. Stated as a DEVICE rather than as `minTier = Stereo3D` because
    // that is what a golfer buys and what the directory has to be able to filter on; the tier is
    // what a calibrated pair of them yields.
    bool                     dtlCamera    = false;
    std::vector<SegmentRole> imuRoles;                                 // required anatomical IMU roles
    bool                     clubTrack   = false;                      // ShaftTrack2D present
    bool                     ballTrack   = false;                      // BallTrack2D present
    // A connected launch monitor. Face angle at impact, spin and strike location are not optically
    // resolvable at our frame rates, and an integration is intended rather than a producer of our
    // own. Stating it HERE rather than as a separate status is what makes the absence graceful: a
    // user without one gets "needs a launch monitor" through the same path a missing face-on camera
    // takes, and the day a connector lands the same metric resolves Measured with no content change.
    bool                     launchMonitor = false;
    // A HackMotion wG3 on the lead wrist. Mirrors launchMonitor exactly, and for the same reason:
    // `imuRoles` says which anatomical segments a route needs and is blind to WHICH INSTRUMENT
    // supplied them, so without this a HackMotion route and the Witmotion route it sits beside are
    // indistinguishable — both would read `{ LeadForearm, LeadHand }` and both would resolve
    // Measured on a rig carrying neither. The `hm.*` keys exist precisely to keep the two
    // separately addressable, so their routes have to be separable too.
    bool                     hackMotion  = false;
    ReconstructionTier       minTier     = ReconstructionTier::Angles2D;
};

// ── Capture devices ─────────────────────────────────────────────────────────────────────────────
//
// The closed vocabulary a requirement folds down to, and the ONLY axis the metric directory filters
// on. Deliberately coarser than MetricRequirement: a reader shopping for kit asks "do I need body
// IMUs" and never "do I need a T12 IMU specifically", and a facet with one option per anatomical
// role is a filter nobody uses. The requirement stays fine-grained for resolution; this is the
// reading of it.
// DECLARATION ORDER IS DISPLAY ORDER, everywhere — every device list, column and filter chip reads
// in this sequence. Sensors you strap on, then what a camera produces, then the box you plug in;
// within the cameras, the views before the things found in them. It is grouped by WHAT KIND OF
// THING it is because that is how a reader scans for their own kit, and an order that varies per
// row (or per whichever metric happened to be counted first) reads as no order at all.
enum class CaptureDevice {
    // ── IMUs ────────────────────────────────────────────────────────────────
    WristImus,        // LeadForearm / LeadHand / LeadUpperArm — the wrist-motion kit
    // A HackMotion wG3. A SEPARATE CHIP FROM WristImus, not a variant of it, because it answers a
    // different buying question: it is one peripheral a golfer either owns or does not, filling the
    // forearm and hand slots on its own, and a reader shopping for it is not shopping for our
    // strap-on kit. Listed next to WristImus because it is worn in the same place.
    HackMotion,
    BodyImus,         // Pelvis / Thorax / T12 / thighs — the trunk-and-legs kit
    // A shaft-mounted IMU. SEPARATE from ClubTrack and it must stay separate: the design's
    // `ClubInstrumented` tier is an upgrade axis ORTHOGONAL to the second camera — "a one-camera
    // owner reaches club metrics by adding a sensor, not a camera" — so folding the two into one
    // chip would answer the buying question with the wrong hardware.
    ClubSensor,
    // ── Cameras, and what they produce ──────────────────────────────────────
    FaceOnCamera,
    DtlCamera,
    ClubTrack,        // the club found in the image — a camera product, not a device you buy
    BallTrack,        // likewise the ball
    // ── External hardware ───────────────────────────────────────────────────
    LaunchMonitor,
};

// Every device, in that order. One list, so a caller wanting the canonical sequence cannot invent a
// second one that drifts from the enum.
inline const std::vector<CaptureDevice> &allCaptureDevices()
{
    static const std::vector<CaptureDevice> kAll = {
        CaptureDevice::WristImus,    CaptureDevice::HackMotion, CaptureDevice::BodyImus,
        CaptureDevice::ClubSensor,
        CaptureDevice::FaceOnCamera, CaptureDevice::DtlCamera,  CaptureDevice::ClubTrack,
        CaptureDevice::BallTrack,    CaptureDevice::LaunchMonitor,
    };
    return kAll;
}

// Stable slug (persisted in filter state, never shown) and display label (shown, translated at the
// GUI edge — this header is Qt-only and has no QObject to tr() through).
inline QString captureDeviceId(CaptureDevice d)
{
    switch (d) {
    case CaptureDevice::FaceOnCamera:  return QStringLiteral("faceOn");
    case CaptureDevice::DtlCamera:     return QStringLiteral("dtl");
    case CaptureDevice::WristImus:     return QStringLiteral("wristImus");
    case CaptureDevice::HackMotion:    return QStringLiteral("hackMotion");
    case CaptureDevice::BodyImus:      return QStringLiteral("bodyImus");
    case CaptureDevice::ClubTrack:     return QStringLiteral("clubTrack");
    case CaptureDevice::ClubSensor:    return QStringLiteral("clubSensor");
    case CaptureDevice::BallTrack:     return QStringLiteral("ballTrack");
    case CaptureDevice::LaunchMonitor: return QStringLiteral("launchMonitor");
    }
    return QString();
}

inline QString captureDeviceLabel(CaptureDevice d)
{
    switch (d) {
    case CaptureDevice::FaceOnCamera:  return QStringLiteral("Face-on camera");
    case CaptureDevice::DtlCamera:     return QStringLiteral("Down-the-line camera");
    case CaptureDevice::WristImus:     return QStringLiteral("Wrist IMUs");
    case CaptureDevice::HackMotion:    return QStringLiteral("HackMotion");
    case CaptureDevice::BodyImus:      return QStringLiteral("Body IMUs");
    case CaptureDevice::ClubTrack:     return QStringLiteral("Club tracking");
    case CaptureDevice::ClubSensor:    return QStringLiteral("Club sensor");
    case CaptureDevice::BallTrack:     return QStringLiteral("Ball tracking");
    case CaptureDevice::LaunchMonitor: return QStringLiteral("Launch monitor");
    }
    return QString();
}

// The devices one requirement folds down to, in vocabulary order and without duplicates.
//
// Built by WALKING THE VOCABULARY and asking the requirement, not by walking the requirement's
// fields. The difference shows on `imuRoles`, which is an unordered set the manifest authors in
// whatever sequence reads best: asking "does anything here mean wrist IMUs" gives the same answer
// whatever order the roles were written in, where switching on each role in turn would let
// `{Pelvis, LeadHand}` and `{LeadHand, Pelvis}` produce two different lists for the same kit.
inline std::vector<CaptureDevice> captureDevicesFor(const MetricRequirement &r)
{
    const auto hasImu = [&r](std::initializer_list<SegmentRole> family) {
        for (SegmentRole want : family)
            for (SegmentRole have : r.imuRoles)
                if (have == want) return true;
        return false;
    };

    std::vector<CaptureDevice> out;
    for (CaptureDevice d : allCaptureDevices()) {
        bool needed = false;
        switch (d) {
        case CaptureDevice::WristImus:
            // ⚠ A HACKMOTION ROUTE DOES NOT ALSO NEED OUR STRAP-ON WRIST KIT. The wG3 fills the
            // forearm and hand slots itself, so a route stating both `hackMotion` and those roles
            // would read on the directory as "you need a wG3 AND a pair of Witmotions", which is
            // both wrong and the one configuration the product refuses to run.
            needed = !r.hackMotion
                     && hasImu({ SegmentRole::LeadUpperArm, SegmentRole::LeadForearm,
                                 SegmentRole::LeadHand });
            break;
        case CaptureDevice::HackMotion:    needed = r.hackMotion;                   break;
        case CaptureDevice::BodyImus:
            needed = hasImu({ SegmentRole::Pelvis, SegmentRole::Thorax, SegmentRole::T12,
                              SegmentRole::TrailThigh, SegmentRole::LeadThigh });
            break;
        case CaptureDevice::ClubSensor:    needed = hasImu({ SegmentRole::Club }); break;
        case CaptureDevice::FaceOnCamera:  needed = r.faceOnCamera;                break;
        case CaptureDevice::DtlCamera:     needed = r.dtlCamera;                   break;
        case CaptureDevice::ClubTrack:     needed = r.clubTrack;                   break;
        case CaptureDevice::BallTrack:     needed = r.ballTrack;                   break;
        case CaptureDevice::LaunchMonitor: needed = r.launchMonitor;               break;
        }
        if (needed) out.push_back(d);
    }
    return out;
}

// ── Acquisition routes ──────────────────────────────────────────────────────────────────────────
//
// A metric is rarely obtainable exactly one way. Axial pelvis turn can be estimated from the
// collapse of the hip span in a face-on image, triangulated from a calibrated camera pair, or
// measured outright by a pelvis IMU — the same number, three methods, three fidelities. Until this
// existed the descriptor could hold only the FLOOR of that ladder, and the rest lived in three
// places that could not be queried and drifted apart: an if/else inside BodyRotationProvider, a
// sentence in the metric's own `description`, and the Capture column of the developer guide.
//
// Now the ladder is data. What follows from that, and is the reason it is worth the manifest churn:
//   * Availability RESOLVES through it — the best satisfied route decides Measured vs Bridged, and
//     the route's own words explain which method produced the number.
//   * The upgrade is derivable — "you are reading this off the face-on camera; a pelvis IMU would
//     measure it directly" needs no second piece of hand-written prose.
//   * "Planned" becomes a fact about a ROUTE, not about a metric. `clubPath` is not work we have
//     failed to do — it needs a camera pointing down the target line. Both statements are now
//     expressible at once, which the single `.planned` flag could not do, and nine metrics were
//     mis-stated as roadmap items because of it.
enum class RouteMethod {
    Projected,      // read in one camera's image plane
    Triangulated,   // reconstructed from two calibrated views
    Inertial,       // integrated from an IMU on the segment itself
    Fused,          // inertial + vision together
    Device,         // read from external hardware we integrate rather than compute
    Derived,        // computed from phase events / other metrics — needs no capture of its own
};

// Stable slug for the method — a glyph key and a persisted filter value, not display text.
inline QString routeMethodName(RouteMethod m)
{
    switch (m) {
    case RouteMethod::Projected:    return QStringLiteral("projected");
    case RouteMethod::Triangulated: return QStringLiteral("triangulated");
    case RouteMethod::Inertial:     return QStringLiteral("inertial");
    case RouteMethod::Fused:        return QStringLiteral("fused");
    case RouteMethod::Device:       return QStringLiteral("device");
    case RouteMethod::Derived:      return QStringLiteral("derived");
    }
    return QString();
}

// What a route yields when it is the one that fires. Two values, because this is what the shot-level
// answer already has room for: Direct → Measured, Estimated → Bridged. A three-rung fidelity enum
// would have to arbitrate whether stereo beats an IMU, and the honest answer is "it depends on the
// metric" — which is exactly what the ORDER of the routes vector says, per metric, instead.
enum class RouteQuality { Estimated, Direct };

struct MetricRoute {
    QString           id;                                   // "faceOn", "faceOn+dtl", "pelvisImu"
    RouteMethod       method  = RouteMethod::Projected;
    RouteQuality      quality = RouteQuality::Direct;
    MetricRequirement requirement;
    // How this route gets the number, in the reader's language. Shown verbatim as the availability
    // reason when the route yields Estimated ("estimated from the collapse of the hip span in the
    // image"), and on the detail page for every route. Name the METHOD, not the missing device: a
    // Bridged metric IS produced, and "needs a pelvis IMU" reads as a refusal.
    QString           summary;
    // No producer implements THIS route yet. A metric is planned exactly when every one of its
    // routes is — which is derived below rather than stored, so the two can never disagree.
    bool              planned = false;
};

// A metric descriptor carries NO normative values.
//
// Until stage 9 of the diagnostics-norms work it did: `MetricNormative` held a DOF to delegate to
// the compiled band table, or a per-phase inline corridor, plus a hand-written note naming the
// context those numbers assumed. All three are gone. Corridors are now content in the norm set,
// keyed on a MEASURE (post-reducer, so "Δ from address at the top" and "absolute at impact" cannot
// be confused for one another) and resolved in the shot's own context through the tree — see
// `Diagnostics/metric_corridor.h`, which is what every metric surface calls.
//
// The descriptor states what a metric IS. It no longer judges it.

// The metric descriptor. `key` equals the existing MetricSeries.key / ScoredMetric.key.
struct MetricDescriptor {
    QString    key;                        // == MetricSeries.key (stable identity)
    MetricType type = MetricType::TimeSeries;
    QString    label;                      // "Lead wrist — bow / cup"
    QString    shortLabel;                 // "Bow/cup" — what ChartMetrics::shortLabel serves
    QString    unit;                       // "°", "mph", "×frame", …
    QString    group;                      // "Wrist & forearm" | "Club & speed" | "Setup" | …

    // CROSS-CUTTING reading lists, empty for almost every metric. A metric belongs to exactly one
    // `group`, and the chart's preset combo is derived from that group (ChartMetrics::seriesGroups)
    // — so a group is also the unit a reader plots together, and a metric cannot be in two of them
    // without being taken out of one.
    //
    // That is the right constraint for the DIRECTORY, where a metric filed twice reads as two
    // metrics, and the wrong one for a COACHING READ that deliberately spans groups. The plumb bob
    // is one: the hip centre over the stance and the tilt of the hip line are read together and
    // graded together, while hipLineTilt's home stays with pelvis sway and lift, which is what it is
    // read AGAINST. Presets are additive and never move a metric out of its group.
    //
    // Order within a preset follows manifest order, the same as groups, so the chart and the Metric
    // Library list the same metrics in the same sequence.
    std::vector<QString> presets;          // { "Plumb Bob" } — usually empty

    QString    description;                // what it means (consolidated from docs/)
    QString    howToRead;                  // sign convention, when to read, what good looks like
    bool       flexPositive = true;        // sign polarity, mirrors MetricSeries.flexPositive

    // ── What the sign means (design: sign conventions) ──────────────────────────────────────────
    //
    // THE RULE: A SIGN'S MEANING NEVER CHANGES WITH HANDEDNESS. Whatever transform is needed to
    // hold that fixed is the producer's job, not the reader's. Two frames express it:
    //
    //   WORLD FRAME — referenced to the TARGET LINE, the GROUND and the HORIZON. Positive is to
    //     the RIGHT of the target line, or UPWARD. Nothing is mirrored, because the reference is
    //     the world and the world does not care which way the golfer stands. `clubPath`,
    //     `launchDirection`, `faceAngle`, `attackAngle`, every `lm.*` reading.
    //
    //   ANATOMICAL FRAME — referenced to the golfer's own LEAD side. Positive is extension
    //     (cupping), or rotation toward the target. Producers DO mirror a left-handed golfer, and
    //     that mirror is what keeps the meaning fixed: a bowed lead wrist must read negative for
    //     both golfers, which is the HackMotion convention and the reason
    //     pose_wrist_angle_source.cpp applies an image flip. Removing it would invert every
    //     left-handed swing's archetype.
    //
    // WHAT DOES FLIP IS THE GLOSS, NEVER THE SIGN. "In-to-out", "open", "draw" are right-handed
    // readings of a world-frame number: positive `clubPath` is right of the target line for
    // everyone, which is in-to-out for a right-hander and out-to-in for a left-hander. State the
    // frame-referenced meaning first and the gloss second, or a left-handed reader is misled by
    // prose that looks authoritative.
    //
    // THE CANONICAL STATEMENT IS docs/design/pinpoint_sign_conventions.md — rules 0/1/2 and the
    // per-metric tables. Rule 0 (a published standard outranks a popular product) is why the four
    // ISB joint angles keep ISB polarity even though a market-leading wrist sensor reports the
    // inverse. These fields are that document made checkable at the point of authoring.
    //
    // Both strings are lower-case fragments completing "positive means …" / "negative means …", so
    // they read inside a sentence and inside a table cell.
    //
    // `signPositive` is REQUIRED for anything in a signable unit — every such metric has a
    // direction, even an unsigned one, whose direction is what the magnitude counts.
    // `signNegative` MAY be empty, and empty means "cannot go negative": a carry, a spin rate, a
    // duration. Writing prose there anyway would invent a meaning the metric does not have.
    // metric_catalogue_test enforces both halves, and additionally refuses a world-frame metric
    // stated ONLY as a right-handed gloss — "in-to-out" flips for a left-handed golfer where
    // "right of the target line" does not, and naming only the gloss misleads half the readership
    // while looking authoritative.
    QString    signPositive;               // "right of the target line — in-to-out for a right-hander"
    QString    signNegative;               // "left of the target line — out-to-in for a right-hander"

    std::vector<Phase> phases;             // phases sampled (PointInTime) / where peak matters (TimeSeries)
    bool               scored = false;     // has a band and contributes to a score

    // Every way this metric can be acquired, ORDERED BEST FIRST — and the last rung is the
    // least-demanding way to get it at all. Both halves of that ordering are load-bearing: the first
    // decides which route wins on a capable shot, the last is what the directory reports as "needs".
    // At least one route, always; a metric with none can be produced no way and should not exist.
    std::vector<MetricRoute> routes;

    // Reverse index of consumers — static, hand-authored in the manifest (design §13.2 decision).
    // Powers the detail page's "Where it's used". e.g. {"score:wrist","fault:cuppedAtTop"}.
    QStringList        usedBy;

    // ── Readings of the ladder ──────────────────────────────────────────────────────────────────
    // Derived, never stored. `planned` used to be a field beside `requirement`, and the pair could
    // contradict each other — a metric flagged planned while its requirement described hardware, so
    // the directory promised work we were not doing about a gap that was really the golfer's kit.

    // Nothing produces this metric by ANY route yet.
    bool planned() const
    {
        for (const MetricRoute &r : routes)
            if (!r.planned) return false;
        return true;
    }

    // The best route we have actually built, or null when every route is planned.
    const MetricRoute *bestLiveRoute() const
    {
        for (const MetricRoute &r : routes)
            if (!r.planned) return &r;
        return nullptr;
    }

    // The FLOOR: the least-demanding route — the cheapest kit that gets a number at all. Live where
    // one exists, otherwise the cheapest planned one, because a planned metric still has to be able
    // to say what it will need. Null only for the degenerate empty-routes case.
    const MetricRoute *baselineRoute() const
    {
        const MetricRoute *live = nullptr;
        for (const MetricRoute &r : routes)
            if (!r.planned) live = &r;                 // best-first, so the last live one is cheapest
        if (live) return live;
        return routes.empty() ? nullptr : &routes.back();
    }

    // What you must have for any reading at all. This is the "Needs" facet, and the descriptor-level
    // requirement every pre-routes caller was really asking for.
    MetricRequirement baselineRequirement() const
    {
        const MetricRoute *r = baselineRoute();
        return r ? r->requirement : MetricRequirement{};
    }

    // ── What a second camera would do ───────────────────────────────────────────────────────────
    //
    // Three of these are read off authored routes. The fourth, `Refines`, is DERIVED, and it has to
    // be: it is a property of projective geometry, not a fact about any one metric, and authoring it
    // thirty-two times would be thirty-two copies of one sentence that a thirty-third metric would
    // then silently miss.
    //
    // The geometry: a face-on camera measures `atan2(Δz, Δx_projected)` where the truth is
    // `atan2(Δz, √(Δx² + Δy²))`. Those agree only while the measured segment lies in the frontal
    // plane. A golf swing rotates the body about the vertical axis, so a `Projected` reading taken
    // anywhere past Address is measuring a foreshortened quantity — and the same applies to
    // translations, where turning the pelvis moves the APPARENT hip centre sideways with no sway at
    // all. Only two families escape: readings taken while the golfer is still square, and ratios
    // between two spans at the same depth.
    //
    // KNOWN LIMIT, and it is why `Refines` is graded weakest rather than promoted: the error is
    // PHASE-dependent and a route is per-metric. `shoulderPlaneAngle` declares Address, Top and
    // Impact — exact at the first, badly projected at the second, one rung for both. Modelling that
    // would mean per-phase routes, which is a great deal of machinery for a caveat that belongs in
    // prose.
    enum class StereoGain { None, Refines, Improves, Unlocks };

    StereoGain stereoGain() const
    {
        const MetricRoute *floor = baselineRoute();
        if (!floor) return StereoGain::None;

        // Cannot be had at all without the second camera.
        if (floor->requirement.dtlCamera) return StereoGain::Unlocks;

        // Somebody authored a better rung that needs it — a real, stated improvement.
        for (CaptureDevice d : upgradeDevices())
            if (d == CaptureDevice::DtlCamera) return StereoGain::Improves;

        // Otherwise: is this a projection taken while the body is off-square?
        if (floor->method != RouteMethod::Projected) return StereoGain::None;
        for (Phase p : phases)
            if (p != Phase::Address) return StereoGain::Refines;
        return StereoGain::None;
    }

    // Devices that only appear on routes ABOVE the floor — what more kit would buy. Counts PLANNED
    // routes too, unlike the per-shot upgrade hint in metric_provider.h, and the difference is
    // deliberate: this answers a catalogue question ("what is this metric's ceiling"), while a hint
    // shown against a real swing may only advertise something a golfer could actually go and use.
    std::vector<CaptureDevice> upgradeDevices() const
    {
        const MetricRoute *floor = baselineRoute();
        std::vector<CaptureDevice> have = floor ? captureDevicesFor(floor->requirement)
                                                : std::vector<CaptureDevice>{};
        std::vector<CaptureDevice> out;
        for (const MetricRoute &r : routes) {
            if (&r == floor) break;                    // ordered best-first: the floor ends the walk
            for (CaptureDevice d : captureDevicesFor(r.requirement)) {
                bool known = false;
                for (CaptureDevice x : have) if (x == d) known = true;
                for (CaptureDevice x : out)  if (x == d) known = true;
                if (!known) out.push_back(d);
            }
        }
        return out;
    }
};

// Stable slug for the stereo verdict — a persisted filter value, never display text.
inline QString stereoGainId(MetricDescriptor::StereoGain g)
{
    switch (g) {
    case MetricDescriptor::StereoGain::None:     return QStringLiteral("none");
    case MetricDescriptor::StereoGain::Refines:  return QStringLiteral("refines");
    case MetricDescriptor::StereoGain::Improves: return QStringLiteral("improves");
    case MetricDescriptor::StereoGain::Unlocks:  return QStringLiteral("unlocks");
    }
    return QString();
}

} // namespace pinpoint::analysis
