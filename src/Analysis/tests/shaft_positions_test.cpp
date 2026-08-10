// Standalone tests for Layer B "B-time location" (src/Analysis/shaft_positions.h):
// image-plane shaft/arm parallel crossings of θ(t)/φ(t) with hysteresis. Pure 1-D
// math, no fixture, no OpenCV. Synthetic profiles with hand-placed crossings.
//
//   cmake --build build/analyzer-tests --target shaft_positions_test
//   ctest --test-dir build/analyzer-tests -R shaft_positions --output-on-failure

#include "../shaft_positions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// Timebase: 200 Hz (dt = 5000 µs), 121 frames. Landmarks: address 10, top 60,
// impact 110, finish 118, last frame 120. Elevation ≡ angle whenever the angle stays
// in (−90°,90°) (cos > 0), so the constructed profiles below cross HORIZONTAL exactly
// where their value passes through 0 — the P-times are hand-placeable.
static constexpr int    kNf  = 121;
static constexpr int    kAF  = 10, kTopF = 60, kImpF = 110, kFinF = 118;
static constexpr int64_t kDt  = 5000;

static std::vector<int64_t> times()
{
    std::vector<int64_t> t(kNf);
    for (int f = 0; f < kNf; ++f) t[f] = int64_t(f) * kDt;
    return t;
}

// θ(t): +80° (down-right) at address → 0 (horizontal, P2≈f35) → −80 (up) at top →
// 0 (P6≈f85) → +80 at impact → 0 (P8≈f115) → −80 at the last frame. `fullThrough`
// false flattens the follow-through at +80 (a punch that never reaches horizontal).
static std::vector<double> buildTheta(bool fullThrough)
{
    std::vector<double> th(kNf, 0.0);
    for (int f = 0; f < kNf; ++f) {
        double v;
        if (f <= 10)       v = 80.0;
        else if (f <= 60)  v = 80.0 - 160.0 * double(f - 10) / 50.0;   // backswing (P2 @ 35)
        else if (f <= 110) v = -80.0 + 160.0 * double(f - 60) / 50.0;  // downswing (P6 @ 85)
        else               v = fullThrough ? 80.0 - 160.0 * double(f - 110) / 10.0   // through (P8 @ 115)
                                           : 80.0;                                    // punch — stays low
        th[f] = v;
    }
    return th;
}

// φ(t): lead arm — horizontal (P3) at f45 in the backswing, horizontal (P5) at
// f75 in the downswing; armed beyond ±8° on both sides of each. `nan` fills the
// whole track with NaN (pose lead-arm absent).
static std::vector<double> buildPhi(bool nan)
{
    std::vector<double> ph(kNf, 0.0);
    if (nan) { std::fill(ph.begin(), ph.end(), std::numeric_limits<double>::quiet_NaN()); return ph; }
    for (int f = 0; f < kNf; ++f) {
        double v;
        if (f <= 10)       v = 80.0;
        else if (f <= 45)  v = 80.0 - 80.0 * double(f - 10) / 35.0;    // → 0 at 45 (P3)
        else if (f <= 60)  v = -40.0 * double(f - 45) / 15.0;          // → −40 at top
        else if (f <= 75)  v = -40.0 + 40.0 * double(f - 60) / 15.0;   // → 0 at 75 (P5)
        else if (f <= 110) v = 80.0 * double(f - 75) / 35.0;           // → +80 toward impact
        else               v = 80.0;
        ph[f] = v;
    }
    return ph;
}

static const PTime *find(const std::vector<PTime> &v, int p)
{
    for (const PTime &t : v) if (t.p == p) return &t;
    return nullptr;
}

int main()
{
    const std::vector<int64_t> t = times();
    PositionsConfig cfg;   // enabled=false, hysteresisDeg=8

    // ── (1) full swing: all 8 + P10 located, crossings within 1 sample ────────
    std::printf("=== full swing (all 8 + P10) ===\n");
    {
        const std::vector<double> th = buildTheta(true), ph = buildPhi(false);
        const std::vector<PTime> ps = locatePTimes(t, th, ph, kAF, kTopF, kImpF, kFinF, cfg);
        check(ps.size() == 9, "all eight P-positions + P10 (Finish) found");
        // ordered by p AND monotone in t_us (P10/Finish falls after the P8 crossing here)
        bool ordered = true;
        for (size_t i = 1; i < ps.size(); ++i)
            if (ps[i].p <= ps[i - 1].p || ps[i].tUs < ps[i - 1].tUs) ordered = false;
        check(ordered, "result ordered (p strictly ↑, t_us ↑)");
        const PTime *p1 = find(ps, 1), *p4 = find(ps, 4), *p7 = find(ps, 7), *p10 = find(ps, 10);
        check(p1 && p1->tUs == t[kAF],    "P1 = address landmark");
        check(p4 && p4->tUs == t[kTopF],  "P4 = top landmark");
        check(p7 && p7->tUs == t[kImpF],  "P7 = impact landmark");
        check(p10 && p10->tUs == t[kFinF], "P10 = finish landmark");
        auto within1 = [&](const PTime *p, int f) {
            return p && std::llabs(p->tUs - t[f]) <= kDt;
        };
        check(within1(find(ps, 2), 35), "P2 shaft-parallel (backswing) ≈ f35");
        check(within1(find(ps, 3), 45), "P3 arm-parallel (backswing) ≈ f45");
        check(within1(find(ps, 5), 75), "P5 arm-parallel (downswing) ≈ f75");
        check(within1(find(ps, 6), 85), "P6 shaft-parallel (downswing) ≈ f85");
        check(within1(find(ps, 8), 115), "P8 shaft-parallel (through) ≈ f115");
    }

    // ── (2) abbreviated punch: no P8 ──────────────────────────────────────────
    std::printf("=== abbreviated punch (no P8) ===\n");
    {
        const std::vector<double> th = buildTheta(false), ph = buildPhi(false);
        const std::vector<PTime> ps = locatePTimes(t, th, ph, kAF, kTopF, kImpF, kFinF, cfg);
        check(find(ps, 8) == nullptr, "P8 absent (follow-through never reaches horizontal)");
        check(ps.size() == 8 && find(ps, 7) != nullptr && find(ps, 10) != nullptr,
              "the other seven + P10 (Finish) still located");
    }

    // ── (3) NaN φ: no P3/P5, shaft positions still found ──────────────────────
    std::printf("=== NaN phi (no P3/P5) ===\n");
    {
        const std::vector<double> th = buildTheta(true), ph = buildPhi(true);
        const std::vector<PTime> ps = locatePTimes(t, th, ph, kAF, kTopF, kImpF, kFinF, cfg);
        check(find(ps, 3) == nullptr && find(ps, 5) == nullptr, "arm-parallel P3/P5 absent (NaN φ)");
        check(find(ps, 2) && find(ps, 6) && find(ps, 8), "shaft-parallel P2/P6/P8 unaffected");
        check(find(ps, 10) != nullptr, "P10 (Finish) unaffected by absent φ");
        check(ps.size() == 7, "seven positions (P1,2,4,6,7,8,10)");
    }

    // ── (4) hysteresis: noise around horizontal cannot double-fire / false-fire ─
    std::printf("=== hysteresis ===\n");
    {
        using namespace positions_detail;
        // (a) a genuine transit with ±5° noise near horizontal fires ONCE.
        std::vector<double> th = buildTheta(true);
        for (int f = 30; f <= 40; ++f) th[f] += (f % 2 == 0 ? 5.0 : -5.0);   // ±5 < hyst 8
        const std::vector<PTime> ps = locatePTimes(t, th, buildPhi(false), kAF, kTopF, kImpF, kFinF, cfg);
        int p2count = 0;
        const PTime *p2 = nullptr;
        for (const PTime &p : ps) if (p.p == 2) { ++p2count; p2 = &p; }
        check(p2count == 1, "noisy horizontal fires P2 exactly once");
        check(p2 && p2->tUs > t[kAF] && p2->tUs < t[kTopF], "P2 stays inside the backswing window");

        // (b) a signal that only wiggles ±5° about horizontal (no real transit)
        // NEVER arms ⇒ no crossing (hysteresis blocks the false fire).
        std::vector<double> wig(kNf, 0.0);
        for (int f = 0; f < kNf; ++f) wig[f] = (f % 2 == 0 ? 5.0 : -5.0);
        const Crossing c = findHorizontalCrossing(t, wig, 0, kNf - 1, cfg.hysteresisDeg);
        check(!c.found, "sub-band wiggle never fires (never armed both sides)");

        // (c) a clean monotone transit fires and interpolates the zero sub-frame.
        std::vector<double> ramp(kNf, 0.0);
        for (int f = 0; f < kNf; ++f) ramp[f] = 40.0 - 4.0 * double(f);   // 0 at f=10
        const Crossing r = findHorizontalCrossing(t, ramp, 0, kNf - 1, cfg.hysteresisDeg);
        check(r.found && r.tUs == t[10], "clean ramp crosses at the exact zero frame");
    }

    std::printf("=== addressHoldEndFrame (camera-first P1) ===\n");
    {
        // 100 frames: still hold f0..f59 (sub-px jitter), motion from f60 on;
        // a LATE bs0 at f75 (the slow-takeaway failure Mark's markup exposed).
        const int nf = 100;
        std::vector<double> hgx(nf), hgy(nf);
        for (int f = 0; f < nf; ++f) {
            if (f < 60) { hgx[f] = 300.0 + 0.3 * ((f % 2 == 0) ? 1.0 : -1.0); hgy[f] = 500.0; }
            else        { hgx[f] = 300.0 - 4.0 * double(f - 59); hgy[f] = 500.0 - 6.0 * double(f - 59); }
        }
        std::vector<char> noBa;
        const int he = addressHoldEndFrame(hgx, hgy, noBa, /*bs0=*/75, cfg);
        check(he >= 57 && he <= 60, "hold end lands at the last still frame (~f59), not late bs0");

        // Ball-anchored corroboration: BA only on f10..f40 ⇒ the chosen frame
        // must be pulled back inside the BA span.
        std::vector<char> ba(size_t(nf), 0);
        for (int f = 10; f <= 40; ++f) ba[size_t(f)] = 1;
        const int heBa = addressHoldEndFrame(hgx, hgy, ba, 75, cfg);
        check(heBa >= 10 && heBa <= 40, "BA corroboration clamps the hold end into the BA span");

        // Never still (motion from frame 0) ⇒ bs0 fallback (legacy behaviour).
        std::vector<double> mgx(nf), mgy(nf);
        for (int f = 0; f < nf; ++f) { mgx[f] = 5.0 * f; mgy[f] = 3.0 * f; }
        check(addressHoldEndFrame(mgx, mgy, noBa, 75, cfg) == 75,
              "no sustained stillness ⇒ bs0 unchanged");
    }

    std::printf("=== addressHoldEndFrame: W3 club-quiet mask ===\n");
    {
        // Grip STILL for the whole pre-takeaway period (sub-px jitter) — the
        // grip-only test alone lands the hold-end at bs0. The CLUB, however, is
        // bobbing about the frozen wrist; the quiet mask is what sees that.
        const int nf = 100, bs0 = 80;
        std::vector<double> qx(nf), qy(nf);
        for (int f = 0; f < nf; ++f) { qx[f] = 300.0 + 0.3 * ((f % 2 == 0) ? 1.0 : -1.0); qy[f] = 500.0; }
        std::vector<char> noBa;
        const int legacy = addressHoldEndFrame(qx, qy, noBa, bs0, cfg);   // no mask
        check(legacy == bs0, "no mask: grip still throughout ⇒ hold-end at bs0 (inside the club bob)");

        // (A) club quiet f0..40, then an ACTIVE bob f41..end (right up to takeaway):
        // the mask must pull the hold-end back to the last quiet frame (f40).
        std::vector<char> quietA(size_t(nf), 0);
        for (int f = 0; f <= 40; ++f) quietA[size_t(f)] = 1;
        check(addressHoldEndFrame(qx, qy, noBa, bs0, cfg, &quietA) == 40,
              "club-quiet mask pulls the hold-end back to the last quiet frame (f40)");

        // (B) two quiet holds (A: f0..30, B: f56..end) with an ACTIVE band between:
        // the hold-end must land in the LATER quiet hold, never in the active band.
        std::vector<char> quietB(size_t(nf), 0);
        for (int f = 0;  f <= 30;     ++f) quietB[size_t(f)] = 1;
        for (int f = 56; f <  nf;     ++f) quietB[size_t(f)] = 1;
        const int he = addressHoldEndFrame(qx, qy, noBa, bs0, cfg, &quietB);
        check(he >= 56 && he <= bs0, "active band between two quiet holds ⇒ hold-end in the later hold");

        // (C) all-active mask ⇒ nothing passes still+quiet ⇒ grip-still-only fallback.
        std::vector<char> allActive(size_t(nf), 0);
        check(addressHoldEndFrame(qx, qy, noBa, bs0, cfg, &allActive) == legacy,
              "all-active mask ⇒ grip-still-only fallback (never degrades below today)");

        // (D) nullptr / empty mask ⇒ byte-identical to the legacy (no-mask) answer.
        std::vector<char> emptyMask;
        check(addressHoldEndFrame(qx, qy, noBa, bs0, cfg, nullptr) == legacy
              && addressHoldEndFrame(qx, qy, noBa, bs0, cfg, &emptyMask) == legacy,
              "nullptr / empty mask ⇒ byte-identical to the legacy answer");

        // (E) BA corroboration composes with quiet: quiet f0..40 but BA only on
        // f10..25 ⇒ the chosen frame is the last still+quiet+ball-anchored one (f25).
        std::vector<char> ba(size_t(nf), 0);
        for (int f = 10; f <= 25; ++f) ba[size_t(f)] = 1;
        check(addressHoldEndFrame(qx, qy, ba, bs0, cfg, &quietA) == 25,
              "BA corroboration composes with the quiet mask (still+quiet+BA ⇒ f25)");
    }

    std::printf("=== addressHoldEndFrame: onset-veto floor (PhaseModel::onsetFloor) ===\n");
    {
        // Real-capture shape: deep pre-fidget hold f0..f39 (sub-px jitter), then
        // a fidget drifting 0.8 px/f to bs0 — above the swLow scale but below
        // swSpd, and NEVER passing stillAt (net 8 px per 10-frame window). The
        // legacy walk-back skips the whole fidget to the deep hold; the floor
        // (the veto's no-return boundary) is the only thing that stops it.
        const int nf = 120, bs0 = 100;
        std::vector<double> fx(nf), fy(nf);
        for (int f = 0; f < nf; ++f) {
            if (f < 40) { fx[f] = 300.0 + 0.3 * ((f % 2 == 0) ? 1.0 : -1.0); fy[f] = 500.0; }
            else        { fx[f] = 300.0; fy[f] = 500.0 + 0.8 * double(f - 40); }
        }
        std::vector<char> noBa;
        const int legacy = addressHoldEndFrame(fx, fy, noBa, bs0, cfg);
        check(legacy < 46, "no floor: walk-back skips the fidget to the deep hold (defect)");
        // (ii) floorFrame = -1 explicitly ⇒ byte-identical legacy answer.
        check(addressHoldEndFrame(fx, fy, noBa, bs0, cfg, nullptr, -1) == legacy,
              "floor -1 ⇒ byte-identical legacy answer");
        // (i) nothing in [floor, bs0] passes stillAt ⇒ the floor ITSELF (the
        // last settle) is the answer — never the deep hold below it.
        check(addressHoldEndFrame(fx, fy, noBa, bs0, cfg, nullptr, 70) == 70,
              "nothing still above the floor ⇒ Address = floor (the last settle)");
        // Floor below the still region ⇒ the normal still answer stands (the
        // floor only excludes frames BELOW it, it never forces itself).
        check(addressHoldEndFrame(fx, fy, noBa, bs0, cfg, nullptr, 20) == legacy,
              "floor below the still region ⇒ normal still answer unchanged");
        // Floor above bs0 (defensive; the min(boundary, onset) contract should
        // prevent it) ⇒ clamped to bs0, not returned out of range.
        check(addressHoldEndFrame(fx, fy, noBa, bs0, cfg, nullptr, bs0 + 10) == bs0,
              "floor above bs0 clamps to bs0 (defensive)");

        // (iii) floor + clubQuiet interaction. Add a still plateau f70..f89
        // above the floor so the quiet tier has a candidate.
        std::vector<double> px = fx, py = fy;
        for (int f = 70; f < 90; ++f) { px[f] = 300.0; py[f] = 524.0; }
        for (int f = 90; f < nf; ++f) { py[f] = 524.0 + 0.8 * double(f - 89); }
        // quiet everywhere ⇒ the plateau passes still+quiet above the floor.
        std::vector<char> quietAll(size_t(nf), 1);
        const int heQ = addressHoldEndFrame(px, py, noBa, bs0, cfg, &quietAll, 45);
        check(heQ >= 80 && heQ <= 92, "floor + all-quiet: still+quiet plateau above the floor wins");
        // quiet ONLY below the floor ⇒ the below-floor quiet frames are never
        // considered; the quiet tier finds nothing and falls back to the
        // (floored) grip-still answer — the plateau, not the deep hold.
        std::vector<char> quietLow(size_t(nf), 0);
        for (int f = 0; f < 40; ++f) quietLow[size_t(f)] = 1;
        const int heQL = addressHoldEndFrame(px, py, noBa, bs0, cfg, &quietLow, 45);
        check(heQL >= 80 && heQL <= 92,
              "quiet only below the floor ⇒ floored grip-still fallback (never below the floor)");
        // all-active mask + floor + nothing still above the floor ⇒ floor.
        std::vector<char> active(size_t(nf), 0);
        check(addressHoldEndFrame(fx, fy, noBa, bs0, cfg, &active, 70) == 70,
              "active mask + floor: two-tier fallback still lands on the floor");
    }

    std::printf("=== p6LastCrossing: fold dip after a shallow top ===\n");
    {
        // The 0703_0008/0009 shape: the top-of-swing shaft sits just above the
        // elevation fold, dips through horizontal shortly after P4 (the FIRST
        // transit), then the true delivery parallel arrives near impact (the
        // LAST). Elevation profile over the downswing window [60, 110]:
        // +20° at top → −60° by f=80 (first transit ≈ f=65) → back up through
        // 0 at f=95 (the delivery transit) → +40° at impact.
        const std::vector<int64_t> t = times();
        std::vector<double> th(kNf, 80.0);
        for (int f = 0; f < kNf; ++f) {
            if (f < kTopF) th[f] = 80.0 - 60.0 * double(f) / double(kTopF);   // settle to +20 at top
            else if (f <= 80)  th[f] = 20.0 - 80.0 * double(f - 60) / 20.0;   // dip: 0-transit at f=65
            else if (f <= 110) th[f] = -60.0 + 100.0 * double(f - 80) / 30.0; // rise: 0-transit at f=98
        }
        check(PositionsConfig().p6LastCrossing, "default is last-crossing (FLIPPED ON 2026-08-10)");
        PositionsConfig cfg;
        cfg.p6LastCrossing = false;                // the legacy first-crossing pick, pinned
        const std::vector<PTime> first =
            locatePTimes(t, th, {}, kAF, kTopF, kImpF, kFinF, cfg);
        cfg.p6LastCrossing = true;
        const std::vector<PTime> last =
            locatePTimes(t, th, {}, kAF, kTopF, kImpF, kFinF, cfg);
        auto p6Of = [](const std::vector<PTime>& v) -> int64_t {
            for (const PTime& p : v) if (p.p == 6) return p.tUs;
            return -1;
        };
        const int64_t tFirst = p6Of(first), tLast = p6Of(last);
        check(tFirst >= 0 && std::llabs(tFirst - 65 * kDt) <= kDt,
              "explicit first-crossing keeps the legacy transit (~f65)");
        check(tLast >= 0 && std::llabs(tLast - 98 * kDt) <= kDt,
              "p6LastCrossing picks the delivery transit (~f98)");

        // Single-crossing window: the two policies must return the identical
        // instant (the standard full-swing profile crosses once in (P4,P7)).
        const std::vector<double> mono = buildTheta(true);
        PositionsConfig cOff, cOn; cOn.p6LastCrossing = true;
        const int64_t a = p6Of(locatePTimes(t, mono, {}, kAF, kTopF, kImpF, kFinF, cOff));
        const int64_t b = p6Of(locatePTimes(t, mono, {}, kAF, kTopF, kImpF, kFinF, cOn));
        check(a >= 0 && a == b, "single transit: last == first, byte-identical P6");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
