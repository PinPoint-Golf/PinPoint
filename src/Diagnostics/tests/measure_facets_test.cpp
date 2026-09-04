// Standalone tests for the measure facet layer (src/Diagnostics/measure_facets.*):
// the validity table indexed on (what-class, quantity, reference-class), deterministic
// canonical naming across series AND reducer, and structural duplicate / near-duplicate
// detection on the series tuple. Own main(), no fixture.
//
//   cmake --build build/analyzer-tests --target measure_facets_test
//   ctest --test-dir build/analyzer-tests -R measure_facets --output-on-failure

#include "../measure_facets.h"

#include <cstdio>
#include <set>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static Series ser(AnatomyRole w, Quantity q, AnatomyRole r) { return Series{ w, q, r }; }

static Reducer at(Phase p)
{
    Reducer r;
    r.kind   = ReducerKind::At;
    r.anchor = p;
    return r;
}
static Reducer delta(Phase a, Phase b)
{
    Reducer r;
    r.kind   = ReducerKind::Delta;
    r.anchor = a;
    r.window = { a, b };
    return r;
}
static Reducer peak(Phase a, Phase b, std::optional<Phase> anchor = std::nullopt)
{
    Reducer r;
    r.kind   = ReducerKind::Extremum;
    r.window = { a, b };
    r.anchor = anchor;
    return r;
}

int main()
{
    std::printf("measure_facets_test\n");

    // ── Validity: the rules that keep authored measures meaningful ──────────────
    {
        // A point has no orientation. This is why the vocabulary carries limb segments: a knee
        // angle is shin-vs-thigh, and five seed rows were originally written as point angles.
        const FacetCheck pa = validateSeries(ser(AnatomyRole::LeadKnee, Quantity::Angle, AnatomyRole::Ground));
        check(!pa.valid && pa.error == FacetError::PointHasNoAngle, "a point has no angle");
        check(!pa.reason.isEmpty(), "rejection carries a human-readable reason");

        const FacetCheck ok = validateSeries(ser(AnatomyRole::LeadShin, Quantity::Angle, AnatomyRole::LeadThigh));
        check(ok.valid, "knee flexion as shin-vs-thigh is legal");

        // A segment's own distance would be its length.
        check(validateSeries(ser(AnatomyRole::Spine, Quantity::Distance, AnatomyRole::Ground)).error
                  == FacetError::SegmentHasNoDistance, "a segment has no distance");

        // A datum is a reference, never a subject.
        check(validateSeries(ser(AnatomyRole::Ground, Quantity::Angle, AnatomyRole::Spine)).error
                  == FacetError::WhatIsDatum, "a world datum cannot be the subject");

        check(validateSeries(ser(AnatomyRole::Spine, Quantity::Angle, AnatomyRole::Spine)).error
                  == FacetError::SelfReference, "nothing is measured relative to itself");

        // An angle needs a reference with a direction.
        check(validateSeries(ser(AnatomyRole::Spine, Quantity::Angle, AnatomyRole::LeadHand)).error
                  == FacetError::AngleNeedsDirection, "an angle needs a directional reference");
        check(validateSeries(ser(AnatomyRole::Spine, Quantity::Angle, AnatomyRole::Ground)).valid,
              "spine angle to ground is legal");
        check(validateSeries(ser(AnatomyRole::ShoulderLine, Quantity::Angle, AnatomyRole::TargetLine)).valid,
              "shoulder line to target line is legal");

        // The ball is a point, so the general point rule catches its angle first — the specific
        // ball rule is reachable for Height, which is the case that needs it (ball elevation
        // during a golf swing is not a thing this pack has any use for).
        check(validateSeries(ser(AnatomyRole::Ball, Quantity::Angle, AnatomyRole::Ground)).error
                  == FacetError::PointHasNoAngle, "the ball has no angle (as a point)");
        check(validateSeries(ser(AnatomyRole::Ball, Quantity::Height, AnatomyRole::Ground)).error
                  == FacetError::BallQuantity, "the ball supports distance and speed only");
        check(validateSeries(ser(AnatomyRole::Ball, Quantity::Distance, AnatomyRole::StanceCentre)).valid,
              "ball distance to stance centre is legal");
        check(validateSeries(ser(AnatomyRole::Ball, Quantity::Speed, AnatomyRole::Ground)).valid,
              "ball speed is legal");

        // A LINE as the reference with Distance must stay legal — a flat per-role ban on
        // "distance for lines" would wrongly reject this, which is why the table is indexed on
        // (what-class, quantity, reference-class).
        check(validateSeries(ser(AnatomyRole::PelvisCentre, Quantity::Distance, AnatomyRole::BallLine)).valid,
              "point distance to a LINE reference is legal");
        check(validateSeries(ser(AnatomyRole::TrailElbow, Quantity::Height, AnatomyRole::ShoulderLine)).valid,
              "point height above a line reference is legal");
    }

    // ── The two ball-position measures must stay structurally distinct ──────────
    // Distance to a LINE is perpendicular (how far the ball is from the body); distance to a POINT
    // runs along the stance (how far forward it is). An earlier draft gave both the same facets,
    // which would have merged two genuinely different measures at authoring time.
    {
        const Series tooClose = ser(AnatomyRole::Ball, Quantity::Distance, AnatomyRole::StanceLine);
        const Series forward  = ser(AnatomyRole::Ball, Quantity::Distance, AnatomyRole::StanceCentre);

        check(validateSeries(tooClose).valid && validateSeries(forward).valid, "both ball measures are legal");
        check(tooClose != forward, "ball-to-body and ball-forward are different series");
        check(facetDistance(tooClose, forward) == 1, "they differ in exactly one facet (the reference)");
        check(canonicalSeriesId(tooClose) != canonicalSeriesId(forward), "their ids differ");
    }

    // ── legalQuantitiesFor / legalReferencesFor drive the picker ────────────────
    {
        const std::vector<Quantity> knee = legalQuantitiesFor(AnatomyRole::LeadKnee);
        const bool kneeHasAngle = std::find(knee.begin(), knee.end(), Quantity::Angle) != knee.end();
        const bool kneeHasDist  = std::find(knee.begin(), knee.end(), Quantity::Distance) != knee.end();
        check(!kneeHasAngle && kneeHasDist, "picker offers distance but not angle for a point");

        const std::vector<Quantity> spine = legalQuantitiesFor(AnatomyRole::Spine);
        const bool spineHasAngle = std::find(spine.begin(), spine.end(), Quantity::Angle) != spine.end();
        const bool spineHasDist  = std::find(spine.begin(), spine.end(), Quantity::Distance) != spine.end();
        check(spineHasAngle && !spineHasDist, "picker offers angle but not distance for a segment");

        // Every offered reference must actually validate — the picker must never present a chip
        // that then fails.
        bool allOffered = true;
        for (AnatomyRole w : allRoles()) {
            if (roleClass(w) == RoleClass::Datum) continue;
            for (Quantity q : legalQuantitiesFor(w))
                for (AnatomyRole r : legalReferencesFor(w, q))
                    if (!validateSeries(ser(w, q, r)).valid) allOffered = false;
        }
        check(allOffered, "every quantity/reference the picker offers validates");

        check(!legalReferencesFor(AnatomyRole::Spine, Quantity::Angle).empty(),
              "a legal subject/quantity pair always has at least one reference");
    }

    // ── Reducer validity ────────────────────────────────────────────────────────
    {
        check(validateReducer(at(Phase::Address)).valid, "At with a phase is valid");
        check(validateReducer(delta(Phase::Address, Phase::Top)).valid, "Delta across two phases is valid");
        check(!validateReducer(delta(Phase::Top, Phase::Top)).valid, "Delta to the same phase is rejected");
        check(validateReducer(peak(Phase::ArmParallelDown, Phase::Impact)).valid, "Extremum over a window is valid");
        check(!validateReducer(peak(Phase::Impact, Phase::Impact)).valid, "Extremum over a zero window is rejected");
        check(validateReducer(peak(Phase::Top, Phase::Impact, Phase::Address)).valid,
              "anchored Extremum (peak deviation from address) is valid");

        Reducer noAnchor;
        noAnchor.kind = ReducerKind::At;
        check(!validateReducer(noAnchor).valid, "At without a phase is rejected");
    }

    // ── Reducer validity against the METRIC'S PHASE DOMAIN ──────────────────────
    //
    // The ten frontal-plane metrics are authored Address->Impact (design 5.1): past impact a
    // lateral projection is reporting the body's ROTATION in a translation's units, which is not a
    // noisy measurement but a reading of something that is not there. A measure authored outside
    // that range has to be refused at the pack, because on screen it looks entirely plausible.
    {
        const PhaseDomain p1p7{ Phase::Address, Phase::Impact };

        check(validateReducer(at(Phase::Impact), p1p7).valid, "at P7 is inside Address->Impact");
        check(validateReducer(delta(Phase::Address, Phase::Impact), p1p7).valid,
              "a P1->P7 change is inside it");
        check(validateReducer(peak(Phase::Top, Phase::Impact, Phase::Address), p1p7).valid,
              "an anchored P4->P7 peak is inside it");

        const ReducerCheck outside = validateReducer(at(Phase::Finish), p1p7);
        check(!outside.valid, "at the finish is REFUSED on an Address->Impact metric");
        // The reason has to name the domain, or an author is told "no" and not told what to change.
        check(outside.reason.contains(QStringLiteral("P1"))
                  && outside.reason.contains(QStringLiteral("P7")),
              "…and the reason names the domain in P-positions");
        check(outside.reason.contains(QStringLiteral("finish")),
              "…and names the phase that broke it");

        check(!validateReducer(delta(Phase::Address, Phase::Finish), p1p7).valid,
              "a change RUNNING PAST the domain is refused");
        check(!validateReducer(peak(Phase::Top, Phase::Release), p1p7).valid,
              "a peak window whose end is outside is refused, not clamped");
        check(!validateReducer(peak(Phase::Top, Phase::Impact, Phase::Finish), p1p7).valid,
              "an out-of-domain ANCHOR is refused too — the deviation is read there");

        // LADDER ORDER, not enum order. Delivery is P6 with enum value 9 and MaxSpeed is 10, both
        // ABOVE Impact's 5 — a numeric comparison would throw out two readings taken before the
        // strike. (P8 comes out right by luck at 14; luck is not a check.) Hence phaseInDomain.
        check(validateReducer(at(Phase::Delivery), p1p7).valid,
              "P6 is inside Address->Impact despite its higher enum value");
        check(validateReducer(at(Phase::MaxSpeed), p1p7).valid,
              "max speed sits between P6 and P7, so it is inside too");
        check(!validateReducer(at(Phase::ShaftParallelThrough), p1p7).valid,
              "P8 is outside, despite the enum putting it nowhere near");

        // A malformed reducer is reported as malformed, never as a domain violation: the author has
        // to be sent to the half that is actually wrong.
        const ReducerCheck bothWrong = validateReducer(delta(Phase::Finish, Phase::Finish), p1p7);
        check(!bothWrong.valid && bothWrong.reason.contains(QStringLiteral("two different phases")),
              "shape is checked before domain");

        // The one-argument form is the whole swing, so every existing caller answers as it did.
        check(validateReducer(at(Phase::Finish)).valid,
              "without a domain the finish is still perfectly readable");
    }

    // ── Canonical naming is deterministic and reducer-aware ─────────────────────
    {
        const Series s = ser(AnatomyRole::Spine, Quantity::Angle, AnatomyRole::Ground);

        check(canonicalSeriesLabel(s) == canonicalSeriesLabel(s), "series label is deterministic");
        check(canonicalMeasureLabel(s, at(Phase::Top)) == canonicalMeasureLabel(s, at(Phase::Top)),
              "measure label is deterministic");

        // The reducer must change the name, or two different measures over one series would share
        // a label and an id.
        check(canonicalMeasureLabel(s, at(Phase::Top)) != canonicalMeasureLabel(s, at(Phase::Address)),
              "different phases produce different labels");
        check(canonicalMeasureLabel(s, at(Phase::Top)) != canonicalMeasureLabel(s, delta(Phase::Address, Phase::Top)),
              "different reducer kinds produce different labels");
        check(canonicalMeasureId(s, at(Phase::Top)) != canonicalMeasureId(s, delta(Phase::Address, Phase::Top)),
              "different reducer kinds produce different ids");
        check(canonicalMeasureId(s, peak(Phase::Top, Phase::Impact))
                  != canonicalMeasureId(s, peak(Phase::Top, Phase::Impact, Phase::Address)),
              "an anchored peak is a different measure from an unanchored one");

        check(canonicalMeasureLabel(s, at(Phase::Top)).contains(QStringLiteral("P4")),
              "labels speak in P-positions, not internal enum names");

        // Ids must be usable as JSON keys and MetricDescriptor keys.
        bool idsClean = true;
        for (AnatomyRole w : { AnatomyRole::Spine, AnatomyRole::PelvisCentre, AnatomyRole::LeadShin })
            for (Quantity q : legalQuantitiesFor(w))
                for (AnatomyRole r : legalReferencesFor(w, q)) {
                    const QString id = canonicalMeasureId(ser(w, q, r), at(Phase::Top));
                    if (id.contains(QLatin1Char(' ')) || id.isEmpty()) idsClean = false;
                }
        check(idsClean, "generated ids contain no spaces and are never empty");

        // Distinct series must never collide on an id.
        std::set<QString> ids;
        bool              noCollision = true;
        for (AnatomyRole w : allRoles()) {
            if (roleClass(w) == RoleClass::Datum) continue;
            for (Quantity q : legalQuantitiesFor(w))
                for (AnatomyRole r : legalReferencesFor(w, q))
                    if (!ids.insert(canonicalSeriesId(ser(w, q, r))).second) noCollision = false;
        }
        check(noCollision, "no two distinct legal series share a canonical id");
    }

    // ── Structural identity is the SERIES, not the reduced measure ──────────────
    {
        const Series a = ser(AnatomyRole::PelvisCentre, Quantity::Distance, AnatomyRole::TrailAnkle);
        const Series b = ser(AnatomyRole::PelvisCentre, Quantity::Distance, AnatomyRole::TrailAnkle);
        const Series c = ser(AnatomyRole::ThoraxCentre, Quantity::Distance, AnatomyRole::TrailAnkle);
        const Series d = ser(AnatomyRole::LeadHand, Quantity::Speed, AnatomyRole::Ground);

        check(a == b && facetDistance(a, b) == 0, "identical facets are the same series");
        check(facetDistance(a, c) == 1 && isNearDuplicate(a, c), "one facet different = near-duplicate");
        check(facetDistance(a, d) == 3 && !isNearDuplicate(a, d), "three facets different is not a duplicate");

        const std::vector<Series>      existing{ b, c, d };
        const std::vector<SeriesMatch> hits = findSimilarSeries(a, existing);
        check(hits.size() == 2, "only exact and one-away series are reported");
        check(hits[0].distance == 0, "exact match is ranked first");
        check(hits[1].distance == 1, "near-duplicate follows");

        // The point of ranking on the series: four reducers over one series are ONE roadmap item.
        check(a == b, "reducers do not participate in series identity");
    }

    // ── Capture gaps are distinguishable from roadmap items ─────────────────────
    {
        const Series thoracic = ser(AnatomyRole::ThoracicSegment, Quantity::Angle, AnatomyRole::Ground);
        const Series lumbar   = ser(AnatomyRole::LumbarSegment, Quantity::Angle, AnatomyRole::Ground);
        const Series hinge    = ser(AnatomyRole::Spine, Quantity::Angle, AnatomyRole::LeadThigh);

        check(validateSeries(thoracic).valid, "a spinal-region series is well-formed...");
        check(seriesNeedsNonPoseSensor(thoracic), "...but flagged as needing another sensor");
        check(seriesNeedsNonPoseSensor(lumbar), "lumbar likewise");
        check(!seriesNeedsNonPoseSensor(hinge),
              "hip hinge (spine vs thigh) resolves from pose — it is NOT a capture gap");
    }

    // ── Derived view requirement ────────────────────────────────────────────────
    {
        check(deriveViewNeeded(ser(AnatomyRole::ShoulderLine, Quantity::Angle, AnatomyRole::TargetLine))
                  == ViewNeeded::DownTheLine, "target-line references need down-the-line");
        check(deriveViewNeeded(ser(AnatomyRole::ThoracicSegment, Quantity::Angle, AnatomyRole::Ground))
                  == ViewNeeded::DownTheLine, "spinal curvature needs down-the-line");
        check(deriveViewNeeded(ser(AnatomyRole::Ball, Quantity::Distance, AnatomyRole::StanceCentre))
                  == ViewNeeded::FaceOn, "distance ALONG the stance reads face-on");
        // The two ball measures differ only in the reference, and that one facet decides which
        // camera can see them: distance ACROSS the stance line runs along the face-on camera's own
        // axis, so it needs down-the-line.
        check(deriveViewNeeded(ser(AnatomyRole::Ball, Quantity::Distance, AnatomyRole::StanceLine))
                  == ViewNeeded::DownTheLine, "distance ACROSS the stance line needs down-the-line");
        check(deriveViewNeeded(ser(AnatomyRole::LeadHand, Quantity::Distance, AnatomyRole::ThoraxCentre))
                  == ViewNeeded::Any, "unclear cases claim nothing");
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
