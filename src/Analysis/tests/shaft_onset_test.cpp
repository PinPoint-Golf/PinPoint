// Standalone tests for the Stage-A "no-return" onset veto (departure-referenced
// revisit scan), the run-bridging pre-pass, and the additive vision Takeaway
// event (W2) in shaft_track_assembly.cpp.
//
// The fixtures are REAL-CAPTURE-SHAPED, per the 2026-07-17 dump diagnosis of
// the 17-swing truth set (w1s1 / w2s4 / w2s6): the address-region grip is
// low-rate pose lerped to frame rate — slow wander (piecewise-linear knots)
// plus a fast small oscillation (~12-frame period) that keeps the SMOOTHED
// speed at a 2–4 px/f floor through every fidget "settle" (true grip rest
// never happens), and the golfer settles into a final address DISPLACED from
// the pre-fidget stance. On such data the A1 walk-back runs through the WHOLE
// fidget to the deep pre-fidget stillness (the documented defect), an anchor-
// box/absolute-rest veto can never fire (both premises unsatisfiable — the
// retired 2026-07-17 veto), and only the departure-referenced revisit scan
// recovers the final settle. The real-data validation lives in the offline
// harness against the three real dumps; these fixtures pin the mechanism.
//
//   cmake --build build/analyzer-tests --target shaft_onset_test
//   ctest --test-dir build/analyzer-tests -R shaft_onset --output-on-failure

#include "../shaft_track_assembly.h"

#include <QVariantMap>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// The 2026-07-17 freeze turned the veto/bridging/Takeaway defaults ON (17-swing
// truth: Address-error median 0.564 → 0.060 s), and 2026-08-10 froze the
// top-collapse repair ON (it heals the two-longest mis-pick's `top` observable
// that the bridging fixtures document). These fixtures pin the OFF-vs-ON
// mechanics, so the dark (legacy) baseline is constructed explicitly.
static ShaftV3Config darkV3()
{
    ShaftV3Config c;
    c.onsetReturnBoxPx      = 0.0;
    c.onsetRunBridgeFrames  = 0;
    c.onsetBridgeMinNetFrac = 0.0;
    c.emitTakeaway          = false;
    c.topRepairEnabled      = false;
    return c;
}

// Real-capture-shaped track builder. The address/fidget half is knot-lerped
// wander plus a continuous-phase oscillation (period 12 frames — the lerped-
// pose beat seen in the real dumps); the swing half is velocity-integrated
// directional segments. All appends continue from the current endpoint.
struct Track {
    std::vector<double> gx, gy, phi;
    double oscK = 0.0;            // continuous oscillation phase (frames)
    static constexpr double kOscPeriod = 12.0;

    void start(double x, double y, double p = 10.0) { gx = {x}; gy = {y}; phi = {p}; }
    int  frame() const { return int(gx.size()) - 1; }

    // Lerp the wander base to (tx, ty) over n frames with oscillation amplitude
    // `amp` superposed on both coordinates (phase-shifted so the wobble is 2D).
    // φ advances by dPhi per frame (0 = still forearm).
    void wander(double tx, double ty, int n, double amp, double dPhi = 0.0)
    {
        const double x0 = gx.back() - oscX(0.0), y0 = gy.back() - oscY(0.0);
        for (int i = 1; i <= n; ++i) {
            const double t = double(i) / double(n);
            oscK += 1.0;
            gx.push_back(x0 + (tx - x0) * t + oscX(amp));
            gy.push_back(y0 + (ty - y0) * t + oscY(amp));
            phi.push_back(phi.back() + dPhi);
        }
    }
    // Velocity segment: speed ramps v0→v1 along unit direction (dx, dy).
    void vel(int n, double v0, double v1, double dx, double dy, double dPhi = 0.0)
    {
        const double len = std::hypot(dx, dy);
        for (int i = 0; i < n; ++i) {
            const double t = (n > 1) ? double(i) / double(n - 1) : 0.0;
            const double v = v0 + (v1 - v0) * t;
            gx.push_back(gx.back() + v * dx / len);
            gy.push_back(gy.back() + v * dy / len);
            phi.push_back(phi.back() + dPhi);
        }
    }
    double oscX(double amp) const { return amp * std::sin(2.0 * M_PI * oscK / kOscPeriod); }
    double oscY(double amp) const { return 0.7 * amp * std::cos(2.0 * M_PI * oscK / kOscPeriod); }
};

// The canonical realistic fidget swing. Deep pre-fidget stillness at (700,590),
// a wandering fidget that settles into a DISPLACED final address (700,650)
// (the w1s1 pattern: the truth-time grip sat 60 px from the pre-fidget stance),
// then a one-piece takeaway → backswing → top → downswing. settleLo/settleHi
// bracket the final settle (the truth region). phiDrift > 0 rotates the forearm
// through the whole fidget (the A2 over-reach driver).
struct FidgetSwing {
    Track t;
    int settleLo = 0, settleHi = 0, tkStart = 0;
};
static FidgetSwing makeFidgetSwing(double phiDrift = 0.0)
{
    FidgetSwing s;
    Track &t = s.t;
    t.start(700, 590);
    t.wander(700, 590, 45, 0.8, 0.0);              // deep stillness (sub-swLow)
    t.wander(688, 616, 16, 4.0, phiDrift);         // fidget: wandering excursions,
    t.wander(714, 602, 15, 4.0, phiDrift);         //   2-4 px/f smoothed floor,
    t.wander(692, 640, 16, 4.0, phiDrift);         //   drifting toward the final
    t.wander(716, 630, 15, 4.0, phiDrift);         //   address
    t.wander(700, 650, 16, 4.0, phiDrift);         // arrive at the DISPLACED address
    s.settleLo = t.frame();
    t.wander(700, 650, 34, 4.0, phiDrift);         // final settle: 2-4 px/f floor —
                                                   //   never true rest (the real dumps'
                                                   //   lerped-pose signature)
    s.settleHi = t.frame();
    s.tkStart = t.frame() + 1;
    t.vel(16, 1.0, 12.0, 0, -1, -1.5);             // one-piece takeaway (departs up)
    t.vel(36, 12.0, 12.0, 0, -1, -2.0);            // backswing
    t.wander(t.gx.back(), t.gy.back(), 18, 0.5, 0.0);  // top hold
    t.vel(10, 1.0, 14.0, 0, +1, +2.0);             // downswing accel
    t.vel(36, 14.0, 14.0, 0, +1, +2.0);            // downswing through address height
    t.vel(6, 14.0, 1.0, 0, +1, +1.0);
    t.wander(t.gx.back(), t.gy.back(), 30, 0.5, 0.0);  // finish hold
    return s;
}

int main()
{
    // ── (a) realistic fidget, grip-driven (A1 over-reach + veto recovery) ──────
    std::printf("=== (a) realistic fidget (A1 over-reach) ===\n");
    {
        FidgetSwing s = makeFidgetSwing(0.0);
        const int nf = int(s.t.gx.size());
        const ShaftV3Config off = darkV3();         // explicit legacy baseline
        const PhaseModel pOff = segmentPhases(s.t.gx, s.t.gy, nf, 150.0, -1, off, nullptr);
        std::printf("    settle=[%d,%d] tkStart=%d | OFF bs0=%d\n",
                    s.settleLo, s.settleHi, s.tkStart, pOff.bs0);
        // DEFECT: the fidget's oscillation floor keeps spdS above swLow through
        // the settle, so A1 walks back out of it into the fidget (~0.2-0.4 s
        // early — the real w2s6 scale; the DEEP over-reach is φ-driven, see a2).
        check(pOff.bs0 < s.settleLo - 15, "OFF: onset walked back out of the settle into the fidget");

        ShaftV3Config on = off; on.onsetReturnBoxPx = 7.0;
        const PhaseModel pOn = segmentPhases(s.t.gx, s.t.gy, nf, 150.0, -1, on, nullptr);
        std::printf("    ON bs0=%d\n", pOn.bs0);
        check(pOn.bs0 >= s.settleLo && pOn.bs0 <= s.settleHi,
              "ON: no-return boundary lands inside the final settle");
        check(pOn.bs0 > pOff.bs0, "ON: onset only moved later");
        // Floor plumbing: dark ⇒ unset; fired without an impact clamp ⇒ the
        // floor IS the boundary (== the final onset).
        check(pOff.onsetFloor == -1, "OFF: onsetFloor unset (veto dark)");
        check(pOn.onsetFloor == pOn.bs0, "ON: onsetFloor == boundary == final onset (no A3)");
        for (double box : {6.0, 8.0}) {             // stable across the sweep window
            ShaftV3Config c = off; c.onsetReturnBoxPx = box;
            const PhaseModel p = segmentPhases(s.t.gx, s.t.gy, nf, 150.0, -1, c, nullptr);
            check(p.bs0 >= s.settleLo && p.bs0 <= s.settleHi, "ON: stable inside the settle (box 6-8)");
        }
    }

    // ── (a2) same fidget with a rotating forearm (A2 over-reach path) ──────────
    std::printf("=== (a2) realistic fidget + phi drift (A2 over-reach) ===\n");
    {
        FidgetSwing s = makeFidgetSwing(0.6);        // |dphi| 0.6 deg/f through the fidget
        const int nf = int(s.t.gx.size());
        const ShaftV3Config off = darkV3();
        const PhaseModel pOff = segmentPhases(s.t.gx, s.t.gy, nf, 150.0, -1, off, &s.t.phi);
        check(pOff.bs0 < s.settleLo - 60, "OFF: A2 walks through the fidget too");
        ShaftV3Config on = off; on.onsetReturnBoxPx = 7.0;
        const PhaseModel pOn = segmentPhases(s.t.gx, s.t.gy, nf, 150.0, -1, on, &s.t.phi);
        std::printf("    OFF bs0=%d ON bs0=%d settle=[%d,%d]\n", pOff.bs0, pOn.bs0, s.settleLo, s.settleHi);
        check(pOn.bs0 >= s.settleLo && pOn.bs0 <= s.settleHi,
              "ON: veto recovers the settle on the A2 path");
    }

    // ── (b/e) fragmented backswing: bridging + the bs0 scan horizon ────────────
    std::printf("=== (b/e) fragmented backswing / top dwell / bridging ===\n");
    {
        // Fidget + settle, then a backswing FRAGMENTED into two >swSpd bursts
        // separated by a short sub-swSpd lull (the lerped-pose fragmentation),
        // a LONG top dwell (> gap frames, far from address), a downswing run
        // (the longest), and a follow-through fragment longer than either
        // backswing fragment — the w2s4-class two-longest mis-pick.
        FidgetSwing s = makeFidgetSwing(0.6);
        Track &t = s.t;
        // strip the canonical swing tail: rebuild from the settle
        const int keep = s.settleHi + 1;
        t.gx.resize(keep); t.gy.resize(keep); t.phi.resize(keep);
        const int frag1 = t.frame() + 1;
        t.vel(9, 12.0, 12.0, 0, -1, -1.0);           // backswing fragment 1
        t.vel(6, 2.0, 2.0, 0, -1, -0.8);             // sub-swSpd lull (6 quiet frames)
        t.vel(9, 12.0, 12.0, 0, -1, -1.0);           // backswing fragment 2
        const int dwellLo = t.frame() + 1;
        t.wander(t.gx.back(), t.gy.back(), 26, 0.5, -0.4);  // top dwell (26 f, phi still creeps)
        const int dwellHi = t.frame();
        const int downStart = t.frame() + 1;
        t.vel(10, 1.0, 14.0, 0, +1, +2.0);
        t.vel(30, 14.0, 14.0, 0, +1, +2.0);          // downswing (longest run)
        t.vel(8, 14.0, 1.0, 0, +1, +1.0);
        t.wander(t.gx.back(), t.gy.back(), 8, 0.5, 0.0);
        t.vel(13, 12.0, 12.0, +1, 0, +0.5);          // follow-through fragment (13 f)
        t.wander(t.gx.back(), t.gy.back(), 20, 0.5, 0.0);
        const int nf = int(t.gx.size());
        const int impact = downStart + 30;

        // (e) bridging: OFF mis-picks the downswing as bs0; ON merges the two
        // backswing fragments (gap 6 < 10) into a run that wins the ranking.
        const ShaftV3Config veto0 = darkV3();
        const PhaseModel pNoBr = segmentPhases(t.gx, t.gy, nf, 150.0, impact, veto0, &t.phi);
        ShaftV3Config br = veto0; br.onsetRunBridgeFrames = 10;
        const PhaseModel pBr = segmentPhases(t.gx, t.gy, nf, 150.0, impact, br, &t.phi);
        std::printf("    frag1=%d dwell=[%d,%d] downStart=%d impact=%d | "
                    "OFF-veto: noBr bs0=%d top=%d  br bs0=%d top=%d\n",
                    frag1, dwellLo, dwellHi, downStart, impact,
                    pNoBr.bs0, pNoBr.top, pBr.bs0, pBr.top);
        check(pNoBr.bs0 < frag1, "no bridge: walk-back from the mis-picked run lands before the fragments");
        // The ranking observable is `top`: with the mis-pick (downswing +
        // follow-through win) the top lands AFTER the downswing; with the
        // fragments bridged into one backswing run it lands in the real dwell.
        check(pNoBr.top >= downStart, "no bridge: top mis-detected after the downswing (mis-pick documented)");
        check(pBr.top >= dwellLo && pBr.top <= downStart,
              "bridge=10: top recovered into the real top dwell (fragments won the ranking)");

        // (b) scan horizon: with bridging ON the veto scan is capped at the
        // bridged bs0 (fragment side) — the top dwell can never be the boundary.
        ShaftV3Config both = veto0; both.onsetReturnBoxPx = 7.0; both.onsetRunBridgeFrames = 10;
        const PhaseModel pBoth = segmentPhases(t.gx, t.gy, nf, 150.0, impact, both, &t.phi);
        std::printf("    veto+bridge bs0=%d (settle=[%d,%d])\n", pBoth.bs0, s.settleLo, s.settleHi);
        check(pBoth.bs0 < dwellLo, "veto+bridge: boundary is NOT in the top dwell");
        check(pBoth.bs0 >= s.settleLo && pBoth.bs0 <= s.settleHi + 2,
              "veto+bridge: onset lands at the final settle");
        // Unbridged + veto: the mis-picked bs0 (downswing) puts the dwell in
        // range, but the A3 near-edge clamp (impact − bsMin) sits BEFORE the
        // dwell (dwell + downswing < bsMin frames), so the onset still cannot
        // land inside it — the documented backstop.
        ShaftV3Config vetoOnly = veto0; vetoOnly.onsetReturnBoxPx = 7.0;
        const PhaseModel pV = segmentPhases(t.gx, t.gy, nf, 150.0, impact, vetoOnly, &t.phi);
        std::printf("    veto only (no bridge) bs0=%d\n", pV.bs0);
        check(pV.bs0 < dwellLo, "veto w/o bridge: A3 backstop keeps the onset out of the top dwell");
    }

    // ── (c) slow one-piece creep: the veto must NOT clip a genuine takeaway ────
    std::printf("=== (c) slow one-piece creep ===\n");
    {
        Track t;
        t.start(700, 620);
        t.wander(700, 620, 60, 0.8, 0.0);            // still address (low noise)
        t.vel(70, 0.8, 0.8, 0, -1, -0.4);            // ~0.8 px/f monotone creep, never returns
        t.vel(18, 0.8, 12.0, 0, -1, -1.5);           // accel into backswing
        t.vel(30, 12.0, 12.0, 0, -1, -2.0);
        t.wander(t.gx.back(), t.gy.back(), 16, 0.5, 0.0);   // top hold
        t.vel(10, 1.0, 14.0, 0, +1, +2.0);
        t.vel(34, 14.0, 14.0, 0, +1, +2.0);
        t.vel(6, 14.0, 1.0, 0, +1, +1.0);
        const int nf = int(t.gx.size());
        const ShaftV3Config off = darkV3();
        const PhaseModel pOff = segmentPhases(t.gx, t.gy, nf, 150.0, -1, off, &t.phi);
        ShaftV3Config on = off; on.onsetReturnBoxPx = 7.0;
        const PhaseModel pOn = segmentPhases(t.gx, t.gy, nf, 150.0, -1, on, &t.phi);
        std::printf("    OFF bs0=%d ON bs0=%d (creep starts at 61)\n", pOff.bs0, pOn.bs0);
        check(pOn.bs0 <= pOff.bs0 + 2, "ON: creep onset <= OFF + 2 (no clipping)");
    }

    // ── (d) A3 impact clamp wins over the veto ────────────────────────────────
    std::printf("=== (d) A3 clamp wins ===\n");
    {
        FidgetSwing s = makeFidgetSwing(0.0);
        const int nf = int(s.t.gx.size());
        const int impact = s.tkStart + 90;           // inside the downswing

        ShaftV3Config free = darkV3(); free.onsetReturnBoxPx = 7.0;
        const PhaseModel pFree = segmentPhases(s.t.gx, s.t.gy, nf, 150.0, -1, free, nullptr);
        check(pFree.bs0 >= s.settleLo, "veto-only onset sits at the settle (test non-vacuous)");

        ShaftV3Config on = free;                     // clamp near edge tight: hiEdge < settleLo
        on.bsMinBeforeImpactUs = int64_t(double(impact - (s.settleLo - 20)) / 150.0 * 1e6);
        on.bsMaxBeforeImpactUs = int64_t(double(impact) / 150.0 * 1e6);
        const PhaseModel pClamp = segmentPhases(s.t.gx, s.t.gy, nf, 150.0, impact, on, nullptr);
        const int framesMin = int(std::lround(double(on.bsMinBeforeImpactUs) * 1e-6 * 150.0));
        const int hiEdge = std::max(0, pClamp.impact - framesMin);
        std::printf("    veto-free=%d impact=%d hiEdge=%d clamped=%d\n",
                    pFree.bs0, pClamp.impact, hiEdge, pClamp.bs0);
        check(pFree.bs0 > hiEdge, "veto result really violates the near-edge clamp (non-vacuous)");
        check(pClamp.bs0 == hiEdge, "A3 clamp pins the onset to hiEdge (clamp wins over the veto)");
        // Floor/A3 interaction: A3 pushed the onset EARLIER than the boundary ⇒
        // the floor follows the onset (min rule — a floor above the walk-back
        // start would be unreachable by addressHoldEndFrame).
        check(pClamp.onsetFloor == pClamp.bs0,
              "A3 pushed the onset below the boundary ⇒ onsetFloor follows the onset");
    }

    // ── (f) full chain: the boundary floors the Address walk-back ─────────────
    std::printf("=== (f) onsetFloor floors addressHoldEndFrame ===\n");
    {
        // Fidget swing whose final settle DRIFTS slowly (0.5 px/f + the osc
        // floor): net displacement ≥ ~2.7 px per 10-frame window, so NOTHING in
        // the settle ever passes the absolute stillAt thresholds (net 2.0 /
        // spike 2.5) — the round-2 studio failure shape: even from a corrected
        // bs0 the legacy walk-back skips the whole fidget to the deep hold.
        Track t;
        t.start(700, 590);
        t.wander(700, 590, 45, 0.8, 0.0);            // deep stillness
        t.wander(688, 616, 16, 4.0, 0.0);            // fidget wander
        t.wander(714, 602, 15, 4.0, 0.0);
        t.wander(692, 640, 16, 4.0, 0.0);
        t.wander(716, 630, 15, 4.0, 0.0);
        t.wander(700, 650, 16, 4.0, 0.0);
        const int settleLo = t.frame();
        t.wander(700, 665, 30, 4.0, 0.0);            // DRIFTING settle (~0.5 px/f)
        const int settleHi = t.frame();
        t.vel(16, 1.0, 12.0, 0, -1, 0.0);            // takeaway
        t.vel(36, 12.0, 12.0, 0, -1, 0.0);
        t.wander(t.gx.back(), t.gy.back(), 18, 0.5, 0.0);
        t.vel(10, 1.0, 14.0, 0, +1, 0.0);
        t.vel(36, 14.0, 14.0, 0, +1, 0.0);
        t.vel(6, 14.0, 1.0, 0, +1, 0.0);
        const int nf = int(t.gx.size());

        ShaftV3Config on = darkV3(); on.onsetReturnBoxPx = 7.0;
        const PhaseModel pm = segmentPhases(t.gx, t.gy, nf, 150.0, -1, on, nullptr);
        std::printf("    settle=[%d,%d] ON bs0=%d floor=%d\n", settleLo, settleHi, pm.bs0, pm.onsetFloor);
        check(pm.onsetFloor >= settleLo - 2 && pm.onsetFloor <= settleHi,
              "onsetFloor lands in the drifting settle");

        const PositionsConfig pc;                    // defaults (net 2.0 / spike 2.5 / w 10)
        std::vector<char> noBa;
        const int heNoFloor = addressHoldEndFrame(t.gx, t.gy, noBa, pm.bs0, pc, nullptr, -1);
        std::printf("    holdEnd no-floor=%d with-floor=%d\n",
                    heNoFloor, addressHoldEndFrame(t.gx, t.gy, noBa, pm.bs0, pc, nullptr, pm.onsetFloor));
        check(heNoFloor < settleLo - 60,
              "no floor: hold-end walks through the fidget to the deep hold (defect)");
        check(addressHoldEndFrame(t.gx, t.gy, noBa, pm.bs0, pc, nullptr, pm.onsetFloor) == pm.onsetFloor,
              "with floor: nothing above it passes stillAt ⇒ Address = the floor (last settle)");
    }

    // ── (k) s0002-shaped presentation flap chain — the m3gate ─────────────────
    std::printf("=== (k) flap-chain mis-pick / m3gate ===\n");
    {
        // Grip-anchor pose FLAPPING (the s0002 presentation move): seven ~8-frame
        // >swSpd oscillation bursts with short sub-swSpd lulls — bridging chains
        // them into a going-nowhere run (net/path ~0.02) long enough to beat the
        // real runs, and bs0 parks in the flap. The real swing follows: quiet
        // patch, fragmented takeaway/backswing (m=2 merge), top hold, downswing.
        Track t;
        t.start(760, 650);
        t.wander(760, 650, 50, 0.8, 0.0);            // deep still address
        const int flapLo = t.frame() + 1;
        for (int c = 0; c < 7; ++c) {                // flap burst: 5 up + 5 down @12
            t.vel(5, 12.0, 12.0, 0, -1, 0.0);
            t.vel(5, 12.0, 12.0, 0, +1, 0.0);
            t.vel(6, 0.5, 0.5, 0, (c % 2) ? -1 : +1, 0.0);   // deep lull ⇒ runs split; net ~0
        }
        const int flapHi = t.frame();
        const int quietLo = t.frame() + 1;
        t.vel(12, 1.2, 1.2, 0, +1, 0.0);             // quiet patch (dips below swLow, the s0002 shape)
        const int tkStart = t.frame() + 1;
        t.vel(20, 9.0, 12.0, 0, -1, 0.0);            // takeaway run
        t.vel(6, 2.0, 2.0, 0, -1, 0.0);              // lull fragmenting the backswing
        t.vel(10, 12.0, 12.0, 0, -1, 0.0);           // backswing fragment (m=2 merge)
        const int bsEnd = t.frame();
        t.wander(t.gx.back(), t.gy.back(), 20, 0.5, 0.0);   // top hold
        const int downStart = t.frame() + 1;
        t.vel(10, 1.0, 14.0, 0, +1, 0.0);
        t.vel(40, 14.0, 14.0, 0, +1, 0.0);           // downswing
        t.vel(8, 14.0, 1.0, 0, +1, 0.0);
        t.wander(t.gx.back(), t.gy.back(), 8, 0.5, 0.0);
        t.vel(13, 12.0, 12.0, +1, 0, 0.0);           // follow-through fragment
        t.wander(t.gx.back(), t.gy.back(), 15, 0.5, 0.0);
        const int nf = int(t.gx.size());
        const int impact = downStart + 35;

        ShaftV3Config off; off.onsetBridgeMinNetFrac = 0.0;   // m3gate darked out (frozen ON 2026-07-18)
        const PhaseModel pOff = segmentPhases(t.gx, t.gy, nf, 150.0, impact, off, nullptr);
        ShaftV3Config on = off; on.onsetBridgeMinNetFrac = 0.2;   // the frozen value
        const PhaseModel pOn = segmentPhases(t.gx, t.gy, nf, 150.0, impact, on, nullptr);
        std::printf("    flap=[%d,%d] quiet=%d tk=%d bsEnd=%d down=%d impact=%d\n"
                    "    OFF bs0=%d top=%d fin0=%d | ON bs0=%d top=%d fin0=%d\n",
                    flapLo, flapHi, quietLo, tkStart, bsEnd, downStart, impact,
                    pOff.bs0, pOff.top, pOff.fin0, pOn.bs0, pOn.top, pOn.fin0);
        // DEFECT (gate dark): the flap chain wins the ranking; bs0 parks in or
        // before the flap, far from the real takeaway.
        check(pOff.bs0 <= flapHi, "gate OFF: bs0 parks in/before the flap (mis-pick documented)");
        // gate ON: the true run selection is restored — onset lands at the quiet
        // patch / takeaway foot, with top and fin0 intact.
        check(pOn.bs0 >= quietLo && pOn.bs0 <= tkStart + 4,
              "gate 0.2: bs0 recovers to the quiet patch / takeaway foot");
        check(pOn.top > bsEnd && pOn.top < downStart + 2, "gate 0.2: top lands in the real top hold");
        check(pOn.fin0 == pOff.fin0, "gate 0.2: fin0 unchanged (downswing end)");
        // The gate only ever REMOVES a going-nowhere chain — the onset can only
        // move later.
        check(pOn.bs0 >= pOff.bs0, "gate 0.2: onset only moved later");
    }

    // ── (l) m=2 exemption: the frozen w2s4 rescue is untouchable ──────────────
    std::printf("=== (l) m=2 bridged runs exempt from the m3gate ===\n");
    {
        // Reuse the (b/e) shape: fidget, settle, two backswing fragments (m=2
        // merge), top dwell, downswing, follow-through fragment. At ANY kFrac
        // the m=2 merges must be exempt ⇒ selection identical to gate-off.
        FidgetSwing s = makeFidgetSwing(0.6);
        Track &t = s.t;
        const int keep = s.settleHi + 1;
        t.gx.resize(keep); t.gy.resize(keep); t.phi.resize(keep);
        t.vel(9, 12.0, 12.0, 0, -1, -1.0);
        t.vel(6, 2.0, 2.0, 0, -1, -0.8);
        t.vel(9, 12.0, 12.0, 0, -1, -1.0);
        t.wander(t.gx.back(), t.gy.back(), 26, 0.5, -0.4);
        const int downStart = t.frame() + 1;
        t.vel(10, 1.0, 14.0, 0, +1, +2.0);
        t.vel(30, 14.0, 14.0, 0, +1, +2.0);
        t.vel(8, 14.0, 1.0, 0, +1, +1.0);
        t.wander(t.gx.back(), t.gy.back(), 8, 0.5, 0.0);
        t.vel(13, 12.0, 12.0, +1, 0, +0.5);
        t.wander(t.gx.back(), t.gy.back(), 20, 0.5, 0.0);
        const int nf = int(t.gx.size());
        const int impact = downStart + 30;

        ShaftV3Config noGate; noGate.onsetBridgeMinNetFrac = 0.0;   // gate darked out (frozen ON 2026-07-18)
        const PhaseModel p0 = segmentPhases(t.gx, t.gy, nf, 150.0, impact, noGate, &t.phi);
        for (double k : {0.2, 0.5, 0.9}) {
            ShaftV3Config g = noGate; g.onsetBridgeMinNetFrac = k;
            const PhaseModel p = segmentPhases(t.gx, t.gy, nf, 150.0, impact, g, &t.phi);
            check(p.bs0 == p0.bs0 && p.top == p0.top && p.impact == p0.impact && p.fin0 == p0.fin0
                  && p.onsetFloor == p0.onsetFloor,
                  "m=2 merges exempt at any kFrac (w2s4 rescue pinned)");
        }
    }

    // ── (m) a legitimate m=3 chain with high net survives the gate ────────────
    std::printf("=== (m) high-net m=3 chain survives ===\n");
    {
        // A slow monotone backswing fragmented into THREE bursts (m=3 chain,
        // net/path ~0.85 — the legitimate case the >= 3 qualifier must not harm).
        Track t;
        t.start(700, 650);
        t.wander(700, 650, 50, 0.8, 0.0);
        const int tkStart = t.frame() + 1;
        for (int c = 0; c < 3; ++c) {
            t.vel(9, 12.0, 12.0, 0, -1, 0.0);        // monotone fragment
            t.vel(5, 2.0, 2.0, 0, -1, 0.0);          // lull (gap 5 < bridge 10)
        }
        const int bsEnd = t.frame();
        t.wander(t.gx.back(), t.gy.back(), 20, 0.5, 0.0);   // top hold
        const int downStart = t.frame() + 1;
        t.vel(10, 1.0, 14.0, 0, +1, 0.0);
        t.vel(40, 14.0, 14.0, 0, +1, 0.0);
        t.vel(8, 14.0, 1.0, 0, +1, 0.0);
        t.wander(t.gx.back(), t.gy.back(), 20, 0.5, 0.0);
        const int nf = int(t.gx.size());
        const int impact = downStart + 35;

        ShaftV3Config noGate; noGate.onsetBridgeMinNetFrac = 0.0;   // gate darked out (frozen ON 2026-07-18)
        const PhaseModel p0 = segmentPhases(t.gx, t.gy, nf, 150.0, impact, noGate, nullptr);
        ShaftV3Config g = noGate; g.onsetBridgeMinNetFrac = 0.2;
        const PhaseModel p = segmentPhases(t.gx, t.gy, nf, 150.0, impact, g, nullptr);
        std::printf("    tk=%d bsEnd=%d down=%d | noGate bs0=%d top=%d fin0=%d | gate bs0=%d\n",
                    tkStart, bsEnd, downStart, p0.bs0, p0.top, p0.fin0, p.bs0);
        check(p0.bs0 <= tkStart && p0.bs0 > tkStart - 30,
              "m=3 monotone chain wins the ranking without the gate (fixture sane)");
        check(p.bs0 == p0.bs0 && p.top == p0.top && p.impact == p0.impact && p.fin0 == p0.fin0,
              "gate 0.2: high-net m=3 chain survives (selection identical)");
    }

    // ── (n) capture-hole clip: post-impact frame drop must not collapse top ───
    std::printf("=== (n) capture-hole clip (2026-08-18 s3 shape) ===\n");
    {
        // Backswing (m=2 fragments), top dwell, downswing INTO a follow-through
        // that climbs back toward the top (one continuous run, net low), then a
        // capture hole after impact, then three sideways settle bursts (gap 5 <
        // bridge 10) as the stalled host's backlog drains. Without the clip the
        // bursts chain onto the downswing run (m=4), the gate rejects it (net/
        // path ~0.03), the backswing is the only run left and its grip apex —
        // the run END — becomes top. With the clip the downswing run stays a
        // chain-1 candidate and the two-run rule finds the dwell.
        Track t;
        t.start(700, 650);
        t.wander(700, 650, 50, 0.8, 0.0);
        t.vel(9, 12.0, 12.0, 0, -1, 0.0);
        t.vel(2, 2.0, 2.0, 0, -1, 0.0);
        t.vel(9, 12.0, 12.0, 0, -1, 0.0);
        const int bsEnd = t.frame();
        t.wander(t.gx.back(), t.gy.back(), 20, 0.5, 0.0);   // top dwell (the truth)
        const int downStart = t.frame() + 1;
        t.vel(10, 1.0, 14.0, 0, +1, 0.0);
        t.vel(30, 14.0, 14.0, 0, +1, 0.0);
        const int impact = t.frame();
        t.vel(28, 14.0, 14.0, 0, -1, 0.0);                  // follow-through climbs back
        const int hole = t.frame() + 1;                     // first post-hole frame
        for (int c = 0; c < 3; ++c) {
            t.vel(9, 12.0, 12.0, (c % 2) ? +1 : -1, 0, 0.0);   // settle burst
            t.vel(5, 1.0, 1.0, (c % 2) ? +1 : -1, 0, 0.0);     // lull (gap 5 < bridge 10)
        }
        t.wander(t.gx.back(), t.gy.back(), 20, 0.5, 0.0);
        const int nf = int(t.gx.size());
        std::vector<int64_t> uniform(nf), holed(nf);
        for (int i = 0; i < nf; ++i) {
            uniform[i] = int64_t(i) * 6667;
            holed[i]   = (i < hole) ? uniform[i]
                                    : uniform[hole - 1] + 600000 + int64_t(i - hole) * 1000;
        }
        ShaftV3Config cfg;   // shipping defaults (bridge 10, gate 0.2, clip 3.0)
        const PhaseModel pNull = segmentPhases(t.gx, t.gy, nf, 150.0, impact, cfg, nullptr);
        const PhaseModel pUni  = segmentPhases(t.gx, t.gy, nf, 150.0, impact, cfg, nullptr, &uniform);
        const PhaseModel pHole = segmentPhases(t.gx, t.gy, nf, 150.0, impact, cfg, nullptr, &holed);
        ShaftV3Config off = cfg; off.captureHolePeriods = 0.0;
        const PhaseModel pOff  = segmentPhases(t.gx, t.gy, nf, 150.0, impact, off, nullptr, &holed);
        const PhaseModel pNoImp = segmentPhases(t.gx, t.gy, nf, 150.0, -1, cfg, nullptr, &holed);
        std::printf("    bsEnd=%d down=%d impact=%d hole=%d | null top=%d fin0=%d | holed top=%d fin0=%d hole=%d\n",
                    bsEnd, downStart, impact, hole, pNull.top, pNull.fin0, pHole.top, pHole.fin0,
                    pHole.captureHole);
        check(pNull.top <= bsEnd && pNull.captureHole < 0,
              "fixture sane: without the clip the top collapses onto the backswing run end");
        check(pHole.captureHole == hole, "holed timestamps: clip fires at the first post-impact hole");
        check(pHole.top > bsEnd && pHole.top < downStart,
              "holed timestamps: top lands in the dwell (two-run rule restored)");
        check(pHole.fin0 == hole - 1, "holed timestamps: fin0 = the frame before the hole");
        auto same = [](const PhaseModel &a, const PhaseModel &b) {
            return a.bs0 == b.bs0 && a.top == b.top && a.impact == b.impact && a.fin0 == b.fin0
                && a.onsetFloor == b.onsetFloor && a.captureHole == b.captureHole && a.phase == b.phase;
        };
        check(same(pUni, pNull),   "uniform timestamps: byte-identical to no timestamps (inert)");
        check(same(pOff, pNull),   "captureHolePeriods=0: byte-identical (dark idiom)");
        check(same(pNoImp, segmentPhases(t.gx, t.gy, nf, 150.0, -1, cfg, nullptr)),
              "no impact anchor: byte-identical (clip needs the anchor)");
    }

    // ── fromOverrides: the four keys reach ShaftV3Config ──────────────────────
    std::printf("=== fromOverrides key plumbing ===\n");
    {
        // Defaults FROZEN ON 2026-07-17 (17-swing truth: Address-error median
        // 0.564 → 0.060 s); the 0-valued overrides below are the legacy dark-out.
        const ShaftV3Config def = ShaftV3Config::fromOverrides(QVariantMap{});
        check(def.onsetReturnBoxPx == 7.0 && def.onsetReturnGapFrames == 15
              && def.onsetRunBridgeFrames == 10 && def.emitTakeaway == true,
              "empty map → frozen ON defaults (box 7 / gap 15 / bridge 10 / Takeaway)");
        check(def.onsetBridgeMinNetFrac == 0.2, "empty map → m3gate frozen ON 0.2 (2026-07-18 freeze)");
        check(def.captureHolePeriods == 3.0, "empty map → capture-hole clip ON at 3 periods");
        QVariantMap ov;
        ov["shaft.onsetReturnBoxPx"] = 0.0;
        ov["shaft.onsetReturnGapFrames"] = 20;
        ov["shaft.onsetRunBridgeFrames"] = 0;
        ov["shaft.onsetBridgeMinNetFrac"] = 0.0;   // dark-out
        ov["shaft.emitTakeaway"] = false;
        ov["shaft.captureHolePeriods"] = 0.0;
        const ShaftV3Config c = ShaftV3Config::fromOverrides(ov);
        check(c.captureHolePeriods == 0.0, "shaft.captureHolePeriods override reaches the config");
        check(c.onsetReturnBoxPx == 0.0 && c.onsetReturnGapFrames == 20
              && c.onsetRunBridgeFrames == 0 && c.onsetBridgeMinNetFrac == 0.0
              && c.emitTakeaway == false,
              "shaft.onsetReturn*/onsetRunBridgeFrames/onsetBridgeMinNetFrac/emitTakeaway overrides reach the config");
    }

    // ── W2: the additive vision Takeaway event ────────────────────────────────
    std::printf("=== W2 Takeaway event (phasesToSegmentation) ===\n");
    {
        PhaseModel pm;
        pm.bs0 = 40; pm.top = 80; pm.impact = 110; pm.fin0 = 150;
        const int nf = 200;
        std::vector<int64_t> tUs(nf);
        for (int i = 0; i < nf; ++i) tUs[i] = int64_t(i) * 6700;   // ~149 fps

        auto hasPhase = [](const Segmentation &s, Phase p) { return s.eventFor(p) != nullptr; };
        auto monotone = [](const Segmentation &s) {
            for (size_t i = 1; i < s.events.size(); ++i)
                if (s.events[i].t_us < s.events[i - 1].t_us) return false;
            return true;
        };

        // OFF (default): {Address, Top, Impact, Finish}, no Takeaway.
        const Segmentation segOff = phasesToSegmentation(pm, tUs, 0.5f, -1, /*emitTakeaway=*/false);
        check(segOff.events.size() == 4, "OFF: four-event ladder");
        check(hasPhase(segOff, Phase::Address) && hasPhase(segOff, Phase::Top)
              && hasPhase(segOff, Phase::Impact) && hasPhase(segOff, Phase::Finish),
              "OFF: {Address, Top, Impact, Finish}");
        check(!hasPhase(segOff, Phase::Takeaway), "OFF: no Takeaway event");
        check(monotone(segOff), "OFF: events time-ordered");

        // ON: adds Takeaway at bs0.
        const Segmentation segOn = phasesToSegmentation(pm, tUs, 0.5f, -1, /*emitTakeaway=*/true);
        check(segOn.events.size() == 5, "ON: five-event ladder");
        check(hasPhase(segOn, Phase::Address) && hasPhase(segOn, Phase::Takeaway)
              && hasPhase(segOn, Phase::Top) && hasPhase(segOn, Phase::Impact)
              && hasPhase(segOn, Phase::Finish),
              "ON: {Address, Takeaway, Top, Impact, Finish}");
        check(monotone(segOn), "ON: events time-ordered");
        check(segOn.eventFor(Phase::Takeaway)->t_us == tUs[pm.bs0], "ON: Takeaway t_us == bs0 time");
        check(segOn.eventFor(Phase::Address)->t_us <= segOn.eventFor(Phase::Takeaway)->t_us,
              "ON: Address <= Takeaway");
        check(segOn.events[0].phase == Phase::Address && segOn.events[1].phase == Phase::Takeaway,
              "ON: Address ordered before Takeaway at equal time");

        // With a located hold end (addressFrame < bs0), Address strictly precedes Takeaway.
        const Segmentation segHold = phasesToSegmentation(pm, tUs, 0.5f, /*addressFrame=*/20, true);
        check(segHold.eventFor(Phase::Address)->t_us == tUs[20]
              && segHold.eventFor(Phase::Takeaway)->t_us == tUs[40],
              "ON+hold: Address at hold end, Takeaway at bs0");
        check(segHold.eventFor(Phase::Address)->t_us < segHold.eventFor(Phase::Takeaway)->t_us,
              "ON+hold: Address strictly before Takeaway");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
