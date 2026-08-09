// Standalone tests for the analyzer-layer P-position bridge
// (src/Analysis/positions_ladder.h): the p→Phase mapping (2/3/5/6/8 only —
// never the 1/4/7/10 anchors), conf/provenance carry-through, time-ordered
// insertion, the insert-if-monotone/abstain-otherwise contract (missing anchor,
// outside window, tie with a neighbour), the duplicate-phase skip (IMU proxy
// wins), the version-4 bump on emission vs byte-identical no-op, and the
// refine.positionsLadder key mapping with its dark default. Synthetic ladders
// with hand-computable expectations — no fixture, no decode. (Slot order +
// canRun skip live in analysis_stage_test's generic orchestrator coverage —
// the PositionsLadderStage glue is file-local in wrist_analyzer.cpp.)
//
//   cmake --build build/analyzer-tests --target positions_ladder_test
//   ctest --test-dir build/analyzer-tests -R positions_ladder --output-on-failure

#include "../positions_ladder.h"

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
static void addEvent(Segmentation &seg, Phase p, int64_t tMs, float conf = 0.5f)
{
    PhaseEvent e;
    e.phase = p;
    e.t_us  = tMs * 1000;
    e.conf  = conf;
    seg.events.push_back(e);
}

static ShaftPosition pos(int p, int64_t tMs, float conf = 0.9f)
{
    ShaftPosition sp;
    sp.p    = p;
    sp.t_us = tMs * 1000;
    sp.conf = conf;
    return sp;
}

// The vision ladder shape phasesToSegmentation emits (Address/Takeaway/Top/
// Impact/Finish, flat conf 0.5) — the ladder the corpus swings actually carry.
static Segmentation visionLadder()
{
    Segmentation seg;
    addEvent(seg, Phase::Address,  0);
    addEvent(seg, Phase::Takeaway, 200);
    addEvent(seg, Phase::Top,      1000);
    addEvent(seg, Phase::Impact,   1300);
    addEvent(seg, Phase::Finish,   2000);
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

int main()
{
    std::printf("positions_ladder_test\n");

    // 1. Happy path: all five mapped positions insert; anchors are never
    //    re-emitted; conf/provenance carry through; ladder stays time-ordered;
    //    version bumps to 4.
    {
        std::printf("-- mapping + carry-through --\n");
        Segmentation seg = visionLadder();
        const std::vector<ShaftPosition> P = {
            pos(1, 0, 0.8f),  pos(2, 500, 0.81f),  pos(3, 700, 0.82f),
            pos(4, 1000),     pos(5, 1100, 0.83f), pos(6, 1200, 0.84f),
            pos(7, 1300),     pos(8, 1450, 0.85f), pos(10, 2000),
        };
        const PositionsLadderResult r = emitPositionsLadder(seg, P);
        check(r.emitted && r.inserted == 5 && r.abstained == 0 && r.duplicate == 0,
              "five mapped positions inserted, anchors untouched");
        check(seg.events.size() == 10, "ladder grew 5 → 10 events");
        check(seg.version == 4, "version bumped to 4");
        check(timeOrdered(seg), "ladder stays time-ordered");
        struct Want { Phase ph; int64_t tMs; float conf; };
        const Want wants[] = {{Phase::ShaftParallelBack, 500, 0.81f},
                              {Phase::MidBackswing, 700, 0.82f},
                              {Phase::ArmParallelDown, 1100, 0.83f},
                              {Phase::Delivery, 1200, 0.84f},
                              {Phase::ShaftParallelThrough, 1450, 0.85f}};
        bool ok = true;
        for (const Want &w : wants) {
            const PhaseEvent *e = seg.eventFor(w.ph);
            ok = ok && e && e->t_us == w.tMs * 1000 && e->conf == w.conf
                 && e->provenance == SegmentRole::Club;
        }
        check(ok, "each event at its position time, position conf, provenance Club");
        // The 1/4/7/10 anchors kept their original times and Unknown provenance.
        const PhaseEvent *a = seg.eventFor(Phase::Address);
        const PhaseEvent *t = seg.eventFor(Phase::Top);
        check(a && a->t_us == 0 && a->provenance == SegmentRole::Unknown
                  && t && t->t_us == 1000 * 1000,
              "existing anchors never retimed or re-attributed");
    }

    // 2. Missing anchor ⇒ abstain (no Finish ⇒ no P8 window).
    {
        std::printf("-- missing anchor --\n");
        Segmentation seg;
        addEvent(seg, Phase::Address, 0);
        addEvent(seg, Phase::Top, 1000);
        addEvent(seg, Phase::Impact, 1300);
        seg.version = 2;
        const PositionsLadderResult r = emitPositionsLadder(seg, {pos(8, 1450)});
        check(!r.emitted && r.inserted == 0 && r.abstained == 1,
              "no Finish anchor ⇒ P8 abstains");
        check(seg.version == 2 && seg.events.size() == 3,
              "all-abstain pass leaves seg untouched (version + events)");
    }

    // 3. Outside the anchor window ⇒ abstain (P2 after Top; P5 before Top).
    {
        std::printf("-- outside window --\n");
        Segmentation seg = visionLadder();
        const PositionsLadderResult r =
            emitPositionsLadder(seg, {pos(2, 1100), pos(5, 900)});
        check(!r.emitted && r.abstained == 2, "out-of-window candidates abstain");
        check(seg.events.size() == 5 && seg.version == 2, "seg untouched");
    }

    // 4. Non-monotone vision ladder (the Top==Finish degeneracy on the 7 known
    //    corpus swings): collapsed windows ⇒ abstain, never reorder.
    {
        std::printf("-- non-monotone ladder --\n");
        Segmentation seg;
        addEvent(seg, Phase::Address, 0);
        addEvent(seg, Phase::Top, 1300);
        addEvent(seg, Phase::Impact, 1300);
        addEvent(seg, Phase::Finish, 1300);
        seg.version = 2;
        const PositionsLadderResult r =
            emitPositionsLadder(seg, {pos(5, 1100), pos(6, 1200), pos(8, 1450)});
        check(!r.emitted && r.abstained == 3,
              "collapsed Top/Impact/Finish windows ⇒ all abstain");
        check(seg.events.size() == 4 && seg.version == 2, "degenerate ladder untouched");
    }

    // 5. Tie with a neighbour ⇒ abstain (P2 exactly at Takeaway's time — the
    //    window test passes but strict-between fails).
    {
        std::printf("-- neighbour tie --\n");
        Segmentation seg = visionLadder();
        const PositionsLadderResult r = emitPositionsLadder(seg, {pos(2, 200)});
        check(!r.emitted && r.abstained == 1, "t == Takeaway t ⇒ abstain, no equal-time insert");
    }

    // 6. Duplicate phase ⇒ skip (the IMU segmenter already emits a Delivery
    //    proxy — the existing event wins, no arbitration, no second Delivery).
    {
        std::printf("-- duplicate phase --\n");
        Segmentation seg = visionLadder();
        addEvent(seg, Phase::Delivery, 1150, 0.7f);
        std::stable_sort(seg.events.begin(), seg.events.end(),
                         [](const PhaseEvent &a, const PhaseEvent &b) { return a.t_us < b.t_us; });
        const PositionsLadderResult r =
            emitPositionsLadder(seg, {pos(5, 1100), pos(6, 1200)});
        check(r.inserted == 1 && r.duplicate == 1, "P6 skipped as duplicate, P5 inserted");
        const PhaseEvent *d = seg.eventFor(Phase::Delivery);
        check(d && d->t_us == 1150 * 1000 && d->conf == 0.7f,
              "existing Delivery kept its time and conf");
        check(timeOrdered(seg), "ladder stays time-ordered around the survivor");
    }

    // 7. Key mapping + dark default (the tuning_overrides_test contract, local
    //    to this module): empty map ⇒ frozen dark; the dotted key flips it.
    {
        std::printf("-- refine.positionsLadder key --\n");
        check(PositionsLadderConfig::fromOverrides({}).enabled == false,
              "empty map → dark default (A/B soak baseline)");
        QVariantMap ov;
        ov["refine.positionsLadder"] = true;
        check(PositionsLadderConfig::fromOverrides(ov).enabled == true,
              "refine.positionsLadder=true enables");
    }

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
