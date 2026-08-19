// Standalone tests for the timeline arbiter (src/Analysis/timeline_fusion.h):
// the §4.3 decision table exactly — class precedence (Anchor > Measured > Proxy
// > Fallback), estimand ownership breaking Measured-vs-Measured, ties retaining
// the incumbent — plus the guards (anchor window, strict time neighbours, the
// Measured-only dispute cap), the Impact invariant, the all-abstain
// byte-identity contract, the dark refine.fusion / refine.fusionP1 defaults, and
// the camera-only claim that fusion reproduces emitPositionsLadder event-for-
// event. Two fixtures are shaped from the corpus the design was written against:
// an eleven-swing-typical wG3 IMU ladder (P6 proxy 44 ms early, P8 proxy +91 ms,
// P10 window-edge clamp) and the Wrist_01/0003 degenerate, where the club P8
// sits 669 ms from the proxy and MUST still win — the swing that rewrote the
// dispute cap. Synthetic ladders with hand-computable expectations — no fixture,
// no decode. (Slot order + canRun skip live in analysis_stage_test's generic
// orchestrator coverage — the TimelineFusionStage glue is file-local in
// wrist_analyzer.cpp.)
//
//   cmake --build build/analyzer-tests --target timeline_fusion_test
//   ctest --test-dir build/analyzer-tests -R timeline_fusion --output-on-failure

#include "../timeline_fusion.h"
#include "../positions_ladder.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// ── synthetic builders (times in ms for readable fixtures) ──────────────────
static void addEvent(Segmentation &seg, Phase p, int64_t tMs, TimingClass cls,
                     SegmentRole prov, float conf = 0.7f)
{
    PhaseEvent e;
    e.phase      = p;
    e.t_us       = tMs * 1000;
    e.conf       = conf;
    e.provenance = prov;
    e.timing     = cls;
    seg.events.push_back(e);
}

static ShaftPosition pos(int p, int64_t tMs, TimingClass cls = TimingClass::Measured,
                         float conf = 0.55f)
{
    ShaftPosition sp;
    sp.p      = p;
    sp.t_us   = tMs * 1000;
    sp.conf   = conf;
    sp.timing = cls;
    return sp;
}

static TimelineFusionConfig on(bool p1 = false, int disputeMs = 300)
{
    TimelineFusionConfig c;
    c.enabled   = true;
    c.p1        = p1;
    c.disputeMs = disputeMs;
    return c;
}

// The wG3 IMU ladder the eleven 2026-08-18 swings actually carry: Address is the
// conf-0.30 "continuous waggle" FALLBACK co-timed to Takeaway, P6/P8 are the
// self-capped proxies (P6 firing ~44 ms early, P8 ~91 ms late), P10 is the
// window-edge clamp ~1.7 s past the real finish, and P3/P4/P5 are the segmenter's
// genuine measurements.
static Segmentation imuLadder()
{
    Segmentation seg;
    addEvent(seg, Phase::Address,              100, TimingClass::Fallback, SegmentRole::LeadHand, 0.30f);
    addEvent(seg, Phase::Takeaway,             100, TimingClass::Measured, SegmentRole::LeadHand, 0.81f);
    addEvent(seg, Phase::MidBackswing,         500, TimingClass::Measured, SegmentRole::LeadForearm);
    addEvent(seg, Phase::Top,                 1000, TimingClass::Measured, SegmentRole::LeadHand, 0.86f);
    addEvent(seg, Phase::ArmParallelDown,     1150, TimingClass::Measured, SegmentRole::LeadForearm);
    addEvent(seg, Phase::Delivery,            1200, TimingClass::Proxy,    SegmentRole::LeadHand,  0.35f);
    addEvent(seg, Phase::MaxSpeed,            1280, TimingClass::Measured, SegmentRole::LeadHand, 0.85f);
    addEvent(seg, Phase::Impact,              1300, TimingClass::Anchor,   SegmentRole::Unknown,  1.00f);
    addEvent(seg, Phase::ShaftParallelThrough,1500, TimingClass::Proxy,    SegmentRole::LeadForearm, 0.35f);
    addEvent(seg, Phase::FollowThrough,       1600, TimingClass::Measured, SegmentRole::LeadForearm, 0.65f);
    addEvent(seg, Phase::Finish,              3000, TimingClass::Fallback, SegmentRole::LeadHand,  0.20f);
    seg.conf    = 0.2f;
    seg.version = 2;
    return seg;
}

// The club's own P-ladder for the same swing — every rung a real crossing or
// milestone on measured samples.
static std::vector<ShaftPosition> clubLadder()
{
    return { pos(1, 200),  pos(2, 400),  pos(3, 470),  pos(4, 1000), pos(5, 1145),
             pos(6, 1244), pos(7, 1300), pos(8, 1409), pos(10, 2000) };
}

// The vision ladder phasesToSegmentation emits (Address/Takeaway/Top/Impact/
// Finish, flat conf 0.5, all Measured, provenance Unknown) — a camera-only swing.
static Segmentation visionLadder()
{
    Segmentation seg;
    addEvent(seg, Phase::Address,     0, TimingClass::Measured, SegmentRole::Unknown, 0.5f);
    addEvent(seg, Phase::Takeaway,  200, TimingClass::Measured, SegmentRole::Unknown, 0.5f);
    addEvent(seg, Phase::Top,      1000, TimingClass::Measured, SegmentRole::Unknown, 0.5f);
    addEvent(seg, Phase::Impact,   1300, TimingClass::Measured, SegmentRole::Unknown, 0.5f);
    addEvent(seg, Phase::Finish,   2000, TimingClass::Measured, SegmentRole::Unknown, 0.5f);
    seg.conf    = 0.5f;
    seg.version = 2;
    return seg;
}

static bool timeOrdered(const Segmentation &seg)
{
    for (size_t i = 1; i < seg.events.size(); ++i)
        if (seg.events[i].t_us < seg.events[i - 1].t_us) return false;
    return true;
}

static int64_t timeOf(const Segmentation &seg, Phase p)
{
    const PhaseEvent *e = seg.eventFor(p);
    return e ? e->t_us : -1;
}

static const FusionDecision *decisionFor(const TimelineFusionResult &r, Phase p)
{
    for (const FusionDecision &d : r.decisions)
        if (d.phase == p) return &d;
    return nullptr;
}

// Every field a consumer or the serializer can see — the byte-identity predicate.
static bool sameLadder(const Segmentation &a, const Segmentation &b)
{
    if (a.events.size() != b.events.size() || a.version != b.version
        || a.swingStartUs != b.swingStartUs || a.swingEndUs != b.swingEndUs
        || a.conf != b.conf || a.fusion.size() != b.fusion.size())
        return false;
    for (size_t i = 0; i < a.events.size(); ++i) {
        const PhaseEvent &x = a.events[i], &y = b.events[i];
        if (x.phase != y.phase || x.t_us != y.t_us || x.conf != y.conf
            || x.provenance != y.provenance || x.timing != y.timing)
            return false;
    }
    return true;
}

int main()
{
    std::printf("timeline_fusion_test\n");

    // 1. The §4.4 table on a wG3 IMU ladder: P6/P8 proxies and the P10 clamp
    //    lose to the club's measurements; P3/P5 are held by ownership; P4 ties;
    //    P7 is the anchor and never moves; P2 is a plain insertion.
    {
        std::printf("-- §4.4 decision table, IMU-bound swing --\n");
        Segmentation seg = imuLadder();
        const TimelineFusionResult r = fuseTimeline(seg, clubLadder(), on());

        check(r.emitted && r.inserted == 1 && r.replaced == 3 && r.retained == 4
                  && r.disputed == 0 && r.abstained == 0,
              "1 inserted (P2), 3 replaced (P6/P8/P10), 4 retained (P3/P4/P5/P7)");
        check(seg.version == 5, "version bumped to 5 (fusion arbitrated)");
        check(timeOrdered(seg), "ladder stays time-ordered");
        check(seg.events.size() == 12, "ladder grew 11 → 12 (P2 inserted, nothing dropped)");

        check(timeOf(seg, Phase::Delivery) == 1244 * 1000
                  && seg.eventFor(Phase::Delivery)->provenance == SegmentRole::Club
                  && seg.eventFor(Phase::Delivery)->timing == TimingClass::Measured
                  && seg.eventFor(Phase::Delivery)->conf == 0.55f,
              "P6: club measurement displaced the 0.35 proxy, carrying its own conf/class");
        check(timeOf(seg, Phase::ShaftParallelThrough) == 1409 * 1000,
              "P8: club measurement displaced the forearm proxy");
        check(timeOf(seg, Phase::Finish) == 2000 * 1000,
              "P10: club finish displaced the window-edge clamp");
        check(timeOf(seg, Phase::ShaftParallelBack) == 400 * 1000
                  && seg.eventFor(Phase::ShaftParallelBack)->provenance == SegmentRole::Club,
              "P2: inserted (the IMU has no P2 at all)");

        check(timeOf(seg, Phase::MidBackswing) == 500 * 1000
                  && timeOf(seg, Phase::ArmParallelDown) == 1150 * 1000,
              "P3/P5: arm-defined, IMU-owned — retained despite equal class");
        check(timeOf(seg, Phase::Top) == 1000 * 1000, "P4: identical instant, incumbent retained");
        check(timeOf(seg, Phase::Impact) == 1300 * 1000
                  && seg.eventFor(Phase::Impact)->timing == TimingClass::Anchor
                  && seg.eventFor(Phase::Impact)->provenance == SegmentRole::Unknown,
              "P7: the acoustic anchor is untouched");
        check(timeOf(seg, Phase::Address) == 100 * 1000
                  && seg.eventFor(Phase::Address)->timing == TimingClass::Fallback,
              "P1: dark by default — the Address fallback keeps the slot");

        // The published P5→P6 interval un-collapses: ~50 ms → ~94 ms here (the
        // corpus figure is ~14 ms → ~64 ms).
        check(timeOf(seg, Phase::Delivery) - timeOf(seg, Phase::ArmParallelDown) == 94 * 1000,
              "P5→P6 interval widens (the collapse the proxy caused is undone)");
    }

    // 2. Every slot with a candidate is recorded — RETENTIONS INCLUDED, with the
    //    winner−loser delta, because that is the V2 calibration data.
    {
        std::printf("-- audit trail --\n");
        Segmentation seg = imuLadder();
        const TimelineFusionResult r = fuseTimeline(seg, clubLadder(), on());
        check(r.decisions.size() == 8, "8 decisions (P2,P3,P4,P5,P6,P7,P8,P10 — P1 dark, no club P9)");
        check(seg.fusion.size() == r.decisions.size(), "the trail is published on the segmentation");

        const FusionDecision *p6 = decisionFor(r, Phase::Delivery);
        check(p6 && p6->reason == FusionReason::ClassBeat && p6->winner == SegmentRole::Club
                  && p6->loser == SegmentRole::LeadHand && p6->deltaUs == 44 * 1000,
              "P6 recorded as ClassBeat, Club over LeadHand, Δ +44 ms");
        const FusionDecision *p3 = decisionFor(r, Phase::MidBackswing);
        check(p3 && p3->reason == FusionReason::OwnerHeld && p3->winner == SegmentRole::LeadForearm
                  && p3->loser == SegmentRole::Club && p3->deltaUs == 30 * 1000,
              "P3 recorded as OwnerHeld with its +30 ms delta kept, not discarded");
        const FusionDecision *p4 = decisionFor(r, Phase::Top);
        check(p4 && p4->reason == FusionReason::TieHeld && p4->deltaUs == 0,
              "P4 recorded as TieHeld (identical instants)");
        const FusionDecision *p7 = decisionFor(r, Phase::Impact);
        check(p7 && p7->reason == FusionReason::AnchorHeld, "P7 recorded as AnchorHeld");
        const FusionDecision *p2 = decisionFor(r, Phase::ShaftParallelBack);
        check(p2 && p2->reason == FusionReason::Inserted && p2->loser == SegmentRole::Unknown
                  && p2->deltaUs == 0,
              "P2 recorded as an uncontested insert (loser Unknown, Δ 0)");
        check(decisionFor(r, Phase::FollowThrough) == nullptr,
              "P9 absent from the trail — the camera defers it, so nothing is contested");
    }

    // 3. Wrist_01/0003, the degenerate: the club P8 sits 669 ms from the proxy
    //    and STILL wins (a proxy's disagreement is evidence against the proxy —
    //    the capped draft would have preserved a 667 ms error), while the club's
    //    167 ms-wrong P4 loses to the IMU on ownership. One broken slot per
    //    witness, both called correctly.
    {
        std::printf("-- degenerate swing (Wrist_01/0003 shape) --\n");
        Segmentation seg;
        addEvent(seg, Phase::Address,               0, TimingClass::Fallback, SegmentRole::LeadHand, 0.30f);
        addEvent(seg, Phase::Top,                1000, TimingClass::Measured, SegmentRole::LeadHand, 0.86f);
        addEvent(seg, Phase::Impact,             1300, TimingClass::Anchor,   SegmentRole::Unknown,  1.00f);
        addEvent(seg, Phase::ShaftParallelThrough,1400, TimingClass::Proxy,   SegmentRole::LeadForearm, 0.35f);
        addEvent(seg, Phase::FollowThrough,      2500, TimingClass::Measured, SegmentRole::LeadForearm, 0.65f);
        addEvent(seg, Phase::Finish,             3000, TimingClass::Fallback, SegmentRole::LeadHand,  0.20f);
        seg.version = 2;

        const TimelineFusionResult r =
            fuseTimeline(seg, { pos(4, 833), pos(7, 1300), pos(8, 2069) }, on());
        check(timeOf(seg, Phase::ShaftParallelThrough) == 2069 * 1000 && r.replaced == 1,
              "club P8 wins UNCAPPED across a 669 ms gap (the proxy carries no floor)");
        check(r.disputed == 0, "a Proxy incumbent is never 'disputed' — its class already decided");
        check(timeOf(seg, Phase::Top) == 1000 * 1000 && r.retained == 2,
              "IMU P4 retained by ownership despite the club being 167 ms out");
        check(timeOf(seg, Phase::Impact) == 1300 * 1000, "Impact untouched on the broken swing too");
        check(timeOrdered(seg), "ladder stays time-ordered");
    }

    // 4. The dispute cap binds ONLY between two Measured witnesses, and only
    //    above the cap. (No V1 replacement reaches it — P1 in Phase 2 is its
    //    first real user — so this is the rule's own test, not a corpus effect.)
    {
        std::printf("-- dispute cap (Measured vs Measured only) --\n");
        const auto build = [] {
            Segmentation s;
            addEvent(s, Phase::Address,   0, TimingClass::Measured, SegmentRole::LeadHand);
            addEvent(s, Phase::Top,     500, TimingClass::Measured, SegmentRole::LeadHand);
            addEvent(s, Phase::Delivery,700, TimingClass::Measured, SegmentRole::LeadHand);
            addEvent(s, Phase::Impact, 1500, TimingClass::Anchor,   SegmentRole::Unknown);
            s.version = 2;
            return s;
        };
        Segmentation capped = build();
        const TimelineFusionResult rc = fuseTimeline(capped, { pos(6, 1150) }, on(false, 300));
        check(rc.disputed == 1 && rc.replaced == 0 && timeOf(capped, Phase::Delivery) == 700 * 1000,
              "450 ms > 300 ms cap ⇒ disputed, incumbent retained");
        const FusionDecision *d = decisionFor(rc, Phase::Delivery);
        check(d && d->reason == FusionReason::Disputed && d->deltaUs == -450 * 1000,
              "the disagreement itself is persisted, whatever the outcome");

        Segmentation loose = build();
        const TimelineFusionResult rl = fuseTimeline(loose, { pos(6, 1150) }, on(false, 600));
        check(rl.replaced == 1 && timeOf(loose, Phase::Delivery) == 1150 * 1000,
              "under the cap the camera-owned slot flips on ownership");

        Segmentation off = build();
        const TimelineFusionResult ro = fuseTimeline(off, { pos(6, 1150) }, on(false, 0));
        check(ro.replaced == 1, "disputeMs = 0 disables the cap entirely");
    }

    // 5. Class precedence is symmetric and ORDINAL, not binary: a Proxy
    //    candidate never displaces a measurement (however far apart they are),
    //    but it does displace a clamp; and where the classes tie, ownership
    //    decides regardless of WHICH class tied.
    {
        std::printf("-- class precedence, both directions --\n");
        Segmentation held;
        addEvent(held, Phase::Address,   0, TimingClass::Measured, SegmentRole::LeadHand);
        addEvent(held, Phase::Top,     500, TimingClass::Measured, SegmentRole::LeadHand);
        addEvent(held, Phase::Delivery,700, TimingClass::Measured, SegmentRole::LeadHand);
        addEvent(held, Phase::Impact, 1500, TimingClass::Anchor,   SegmentRole::Unknown);
        held.version = 2;
        const TimelineFusionResult rh =
            fuseTimeline(held, { pos(6, 900, TimingClass::Proxy) }, on());
        check(timeOf(held, Phase::Delivery) == 700 * 1000 && rh.retained == 1
                  && decisionFor(rh, Phase::Delivery)->reason == FusionReason::ClassHeld,
              "a Proxy candidate cannot displace a Measured incumbent, even on its own slot");

        Segmentation clamp = imuLadder();
        std::vector<ShaftPosition> Q = clubLadder();
        for (ShaftPosition &p : Q)
            if (p.p == 10) p.timing = TimingClass::Proxy;
        const TimelineFusionResult rc = fuseTimeline(clamp, Q, on());
        check(timeOf(clamp, Phase::Finish) == 2000 * 1000
                  && decisionFor(rc, Phase::Finish)->reason == FusionReason::ClassBeat,
              "the same Proxy still beats a Fallback clamp — the ranking is ordinal");

        Segmentation both = imuLadder();
        std::vector<ShaftPosition> P = clubLadder();
        for (ShaftPosition &p : P)
            if (p.p == 6) p.timing = TimingClass::Proxy;   // crossing on a predicted sample
        const TimelineFusionResult rb = fuseTimeline(both, P, on());
        check(timeOf(both, Phase::Delivery) == 1244 * 1000
                  && decisionFor(rb, Phase::Delivery)->reason == FusionReason::OwnerBeat,
              "Proxy vs Proxy on a shaft-defined slot goes to the instrument that sees the shaft");
    }

    // 6. Guards: a winner outside its anchor window, or one that would cross a
    //    retained neighbour, abstains — the incumbent stays and it is counted.
    {
        std::printf("-- window + neighbour guards --\n");
        Segmentation seg = imuLadder();
        std::vector<ShaftPosition> P = clubLadder();
        for (ShaftPosition &p : P)
            if (p.p == 6) p.t_us = 1350 * 1000;            // past Impact ⇒ outside (Top, Impact)
        const TimelineFusionResult r = fuseTimeline(seg, P, on());
        check(timeOf(seg, Phase::Delivery) == 1200 * 1000 && r.abstained == 1,
              "P6 outside the Top..Impact window ⇒ abstain, proxy kept");
        const FusionDecision *d = decisionFor(r, Phase::Delivery);
        check(d && d->reason == FusionReason::GuardWindow, "abstention reason recorded");

        Segmentation seg2 = imuLadder();
        std::vector<ShaftPosition> Q = clubLadder();
        for (ShaftPosition &p : Q)
            if (p.p == 6) p.t_us = 1280 * 1000;            // exactly MaxSpeed ⇒ not strict
        const TimelineFusionResult r2 = fuseTimeline(seg2, Q, on());
        check(timeOf(seg2, Phase::Delivery) == 1200 * 1000 && r2.abstained == 1
                  && decisionFor(r2, Phase::Delivery)->reason == FusionReason::GuardNeighbour,
              "a tie with a retained neighbour is not strict ⇒ abstain");
    }

    // 7. All-abstain ⇒ the segmentation is byte-identical, version, events and
    //    audit trail alike. (Collapsed Top/Impact/Finish — the known non-monotone
    //    vision ladder — is the natural way to produce it.)
    {
        std::printf("-- all-abstain byte identity --\n");
        Segmentation seg;
        addEvent(seg, Phase::Address, 0,    TimingClass::Measured, SegmentRole::Unknown, 0.5f);
        addEvent(seg, Phase::Top,     1300, TimingClass::Measured, SegmentRole::Unknown, 0.5f);
        addEvent(seg, Phase::Impact,  1300, TimingClass::Measured, SegmentRole::Unknown, 0.5f);
        addEvent(seg, Phase::Finish,  1300, TimingClass::Measured, SegmentRole::Unknown, 0.5f);
        seg.version = 2;
        const Segmentation before = seg;
        const TimelineFusionResult r =
            fuseTimeline(seg, { pos(5, 1100), pos(6, 1200), pos(8, 1450) }, on());
        check(!r.emitted && r.inserted == 0 && r.replaced == 0 && r.abstained == 3,
              "collapsed windows ⇒ all three abstain");
        check(sameLadder(seg, before) && seg.fusion.empty(),
              "seg untouched — version, events, and no audit trail published");
    }

    // 8. refine.fusion=false is a no-op INSIDE the function too (the stage is
    //    skipped in the pipeline; this is the belt to that braces).
    {
        std::printf("-- dark gate --\n");
        Segmentation seg = imuLadder();
        const Segmentation before = seg;
        TimelineFusionConfig dark;
        dark.enabled = false;        // the soak baseline, whatever the frozen default is
        const TimelineFusionResult r = fuseTimeline(seg, clubLadder(), dark);
        check(!r.emitted && r.decisions.empty() && sameLadder(seg, before),
              "disabled ⇒ nothing decided, nothing touched");
    }

    // 9. Camera-only: fusion reproduces the positions ladder event-for-event.
    //    This is the claim §4.4 makes about the 61-swing corpus, checked here
    //    rather than assumed — every interior slot is a pure insertion and every
    //    anchor slot ties, because the club positions and the vision ladder are
    //    born from the same phase-model frames.
    {
        std::printf("-- camera-only parity with emitPositionsLadder --\n");
        const std::vector<ShaftPosition> P = {
            pos(1, 0), pos(2, 500), pos(3, 700), pos(4, 1000), pos(5, 1100),
            pos(6, 1200), pos(7, 1300), pos(8, 1450), pos(10, 2000) };
        Segmentation fused = visionLadder();
        Segmentation laddered = visionLadder();
        const TimelineFusionResult r = fuseTimeline(fused, P, on());
        const PositionsLadderResult lr = emitPositionsLadder(laddered, P);

        check(r.inserted == 5 && lr.inserted == 5, "both emit the same five interior rungs");
        check(r.retained == 3 && r.replaced == 0,
              "P4/P7/P10 tie or anchor-hold — no anchor is ever moved on a camera-only swing");
        check(fused.events.size() == laddered.events.size(), "same ladder length");
        bool identical = fused.events.size() == laddered.events.size();
        for (size_t i = 0; identical && i < fused.events.size(); ++i)
            identical = fused.events[i].phase == laddered.events[i].phase
                     && fused.events[i].t_us == laddered.events[i].t_us
                     && fused.events[i].conf == laddered.events[i].conf
                     && fused.events[i].provenance == laddered.events[i].provenance;
        check(identical, "every event identical in phase, time, conf and provenance");
        check(fused.version == 5 && laddered.version == 4,
              "the only difference is the version stamp (and the additive audit trail)");
    }

    // 10. P1 behind refine.fusionP1: dark means ABSENT (not "decided and
    //     skipped"), and when lit it obeys the Address ≤ Takeaway rule that
    //     Phase 2 will gate on.
    {
        std::printf("-- refine.fusionP1 --\n");
        Segmentation dark = imuLadder();
        const TimelineFusionResult rd = fuseTimeline(dark, clubLadder(), on(/*p1=*/false));
        check(decisionFor(rd, Phase::Address) == nullptr && timeOf(dark, Phase::Address) == 100 * 1000,
              "dark ⇒ no Address decision at all");

        // The wG3 shape: club P1 lands AFTER the IMU Takeaway (Address is
        // co-timed to it), so the pair is refused rather than crossed.
        Segmentation late = imuLadder();
        const TimelineFusionResult rl = fuseTimeline(late, clubLadder(), on(/*p1=*/true));
        check(timeOf(late, Phase::Address) == 100 * 1000
                  && decisionFor(rl, Phase::Address)->reason == FusionReason::GuardWindow,
              "club P1 after Takeaway ⇒ refused (Address must not cross Takeaway)");

        Segmentation early = imuLadder();
        std::vector<ShaftPosition> P = clubLadder();
        for (ShaftPosition &p : P)
            if (p.p == 1) p.t_us = 50 * 1000;
        const TimelineFusionResult re = fuseTimeline(early, P, on(/*p1=*/true));
        check(timeOf(early, Phase::Address) == 50 * 1000
                  && early.eventFor(Phase::Address)->timing == TimingClass::Measured
                  && decisionFor(re, Phase::Address)->reason == FusionReason::ClassBeat,
              "the club stack fit displaces the conf-0.30 Address fallback on class");
        check(timeOrdered(early), "ladder stays time-ordered around the moved Address");
    }

    // 11. Anchors are arbitrated, never INVENTED: a ladder missing Top or Finish
    //     does not gain one from the club track (those events are the windows
    //     every other slot measures against).
    {
        std::printf("-- anchors are never invented --\n");
        Segmentation seg;
        addEvent(seg, Phase::Address, 0,    TimingClass::Measured, SegmentRole::Unknown, 0.5f);
        addEvent(seg, Phase::Impact,  1300, TimingClass::Anchor,   SegmentRole::Unknown, 1.0f);
        seg.version = 2;
        const TimelineFusionResult r =
            fuseTimeline(seg, { pos(4, 1000), pos(7, 1300), pos(10, 2000) }, on());
        check(!r.emitted && seg.events.size() == 2 && seg.eventFor(Phase::Top) == nullptr
                  && seg.eventFor(Phase::Finish) == nullptr,
              "absent Top/Finish stay absent; P7 has an incumbent and is anchor-held");
        check(r.retained == 1, "only the Impact slot was contested");
    }

    // 12. Key mapping + frozen defaults (the tuning_overrides_test contract,
    //     local to this module). refine.fusion is ON since the 2026-08-19 corpus
    //     gate, so ITS override direction is DARK-OUT — false restores the soak
    //     baseline. refine.fusionP1 is still dark, awaiting its own Phase-2 gate.
    {
        std::printf("-- refine.fusion* keys --\n");
        check(TimelineFusionConfig::fromOverrides({}).enabled == true,
              "empty map → frozen ON default (2026-08-19 corpus gate)");
        check(TimelineFusionConfig::fromOverrides({}).p1 == false,
              "refine.fusionP1 stays dark until the Phase-2 Address gate");
        check(TimelineFusionConfig::fromOverrides({}).disputeMs == 300, "dispute cap default 300 ms");
        QVariantMap ov;
        ov["refine.fusion"]          = false;
        ov["refine.fusionP1"]        = true;
        ov["refine.fusionDisputeMs"] = 120;
        const TimelineFusionConfig c = TimelineFusionConfig::fromOverrides(ov);
        check(!c.enabled && c.p1 && c.disputeMs == 120,
              "all three dotted keys map (fusion=false restores the soak baseline)");
    }

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
