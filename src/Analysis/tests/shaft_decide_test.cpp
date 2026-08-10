// Standalone tests for the shaft v3.0-r1 deciding half
// (src/Analysis/shaft_track_assembly — phase model, φ smoothing, C2 geometry,
// per-frame DP emission, banded Viterbi, ψ-isotonic reconcile). Synthetic
// inputs with hand-computable expectations. Full NUMERIC parity vs the Python
// exemplar is the separate shaft_parity_test (Phase 5).
//
//   cmake --build build/analyzer-tests --target shaft_decide_test
//   ctest --test-dir build/analyzer-tests -R shaft_decide --output-on-failure

#include "../shaft_track_assembly.h"

#include <opencv2/imgproc.hpp>   // cv::line/circle for the Phase-B synthetic frames

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }
static constexpr double kPi = 3.14159265358979323846;

int main()
{
    const ShaftV3Config cfg;

    // ── exact weighted PAVA ──────────────────────────────────────────────────
    std::printf("=== pava ===\n");
    {
        // y=[1,3,2,4] increasing: 3>2 pool → [1,2.5,2.5,4]
        const std::vector<double> y = {1, 3, 2, 4}, w = {1, 1, 1, 1};
        const std::vector<double> x = pava(y, w, true);
        check(near(x[0], 1, 1e-9) && near(x[1], 2.5, 1e-9) && near(x[2], 2.5, 1e-9) && near(x[3], 4, 1e-9),
              "increasing pool of a single violation");
        // already-monotone increasing input is unchanged
        const std::vector<double> mono = {0, 1, 2, 3};
        const std::vector<double> xm = pava(mono, w, true);
        check(near(xm[3], 3, 1e-9) && near(xm[0], 0, 1e-9), "monotone input unchanged");
        // decreasing direction
        const std::vector<double> yd = {4, 2, 3, 1};
        const std::vector<double> xd = pava(yd, w, false);
        check(near(xd[1], 2.5, 1e-9) && near(xd[2], 2.5, 1e-9), "decreasing pool");
    }

    // ── robust isotonic down-weights an outlier ──────────────────────────────
    std::printf("=== robustIsotonic ===\n");
    {
        std::vector<double> y = {0, 1, 2, 3, 4, 5, 6, 7};
        std::vector<double> w(8, 1.0);
        y[4] = 40.0;   // single wild outlier
        const std::vector<double> x = robustIsotonic(y, w, true, cfg);
        // the fit stays near the clean line at the flanks, not dragged to 40
        check(x[3] < 6 && x[5] < 10, "outlier down-weighted (flanks stay low)");
        check(x[7] >= x[0], "still monotone increasing");
    }

    // ── banded Viterbi picks the decreasing well path (downswing) ────────────
    std::printf("=== viterbiDP ===\n");
    {
        const int NS = 16, nf = 3;
        std::vector<std::vector<float>> emis(nf, std::vector<float>(NS, 10.f));
        emis[0][8] = 0.f; emis[1][7] = 0.f; emis[2][6] = 0.f;   // decreasing bins
        const std::vector<SwingPhase> phase(nf, SwingPhase::Downswing);
        const DPResult dp = viterbiDP(emis, phase, cfg);
        check(dp.thstar.size() == 3 && dp.thstar[0] == 8 && dp.thstar[1] == 7 && dp.thstar[2] == 6,
              "path follows the decreasing wells 8→7→6");
    }

    // ── frameEmission: band well LAST + arm veto ─────────────────────────────
    std::printf("=== frameEmission ===\n");
    {
        const int NS = 360;
        std::vector<float> gridRad(NS), gridDeg(NS);
        for (int k = 0; k < NS; ++k) { gridDeg[k] = float(k); gridRad[k] = float(k * kPi / 180.0); }
        std::vector<float> evMax(NS, 0.2f), rawNorm(NS, 0.f);
        evMax[100] = 0.9f;
        BandMatch bm; bm.ok = true; bm.thetaDeg = 100.f; bm.n = 5;
        std::vector<float> em, inside;
        frameEmission(em, inside, evMax, rawNorm, bm, /*phiSDeg=*/0.0, SwingPhase::Downswing,
                      /*chir=*/1, /*gx=*/300, /*gy=*/300, /*poly=*/nullptr, cv::Mat(),
                      gridRad, gridDeg, cfg);
        check(near(em[100], -cfg.wBand, 1e-4), "band bin = -wBand (applied last)");
        check(near(em[180], cfg.wE2 * 0.8 + cfg.wArm, 1e-3), "arm-veto bin (φ+180) = wE2·(1-ev)+wArm");
        check(near(em[90], cfg.wE2 * 0.8, 1e-3), "neutral bin = wE2·(1-ev)");
    }

    // ── body geometry: half-plane inside/outside ─────────────────────────────
    std::printf("=== bodyPolys ===\n");
    {
        // a square torso 100..300 in x and y (one frame)
        std::vector<std::vector<cv::Point2d>> joints = {{{100, 100}, {300, 100}, {300, 300}, {100, 300}}};
        const std::vector<BodyPoly> polys = bodyPolys(joints);
        check(polys.size() == 1 && polys[0].n.size() == 4, "one 4-edge hull");
        auto insideFrac = [&](double px, double py) {
            double mx = -1e30;
            for (size_t e = 0; e < polys[0].n.size(); ++e)
                mx = std::max(mx, polys[0].n[e][0] * px + polys[0].n[e][1] * py - polys[0].d[e]);
            return mx;   // ≤ margin ⇒ inside
        };
        check(insideFrac(200, 200) <= cfg.bodyMargin, "centre inside");
        check(insideFrac(500, 200) > cfg.bodyMargin, "far-right point outside");
    }

    // ── segmentPhases on a synthetic swing ───────────────────────────────────
    std::printf("=== segmentPhases ===\n");
    {
        const int nf = 170;
        std::vector<double> gx(nf), gy(nf);
        double x = 200, y = 300;
        for (int f = 0; f < nf; ++f) {
            if (f >= 40 && f < 70)      { x -= 4; y -= 9; }   // backswing (speed ≈ 9.8 > swSpd)
            else if (f >= 77 && f < 109){ x += 3; y += 9; }   // downswing
            gx[f] = x; gy[f] = y;
        }
        const PhaseModel pm = segmentPhases(gx, gy, nf, 150.0, -1, cfg);
        check(pm.phase.size() == size_t(nf), "phase per frame");
        check(pm.phase.front() == SwingPhase::Addr, "starts at address");
        check(pm.phase.back() == SwingPhase::Finish, "ends at finish");
        check(pm.bs0 > 30 && pm.bs0 < 50, "takeaway near f40");
        check(pm.top > pm.bs0 && pm.impact > pm.top && pm.fin0 >= pm.impact, "landmarks ordered");
        bool hasImpact = false, hasBackswing = false;
        for (SwingPhase p : pm.phase) { if (p == SwingPhase::Impact) hasImpact = true; if (p == SwingPhase::Backswing) hasBackswing = true; }
        check(hasImpact && hasBackswing, "impact + backswing phases present");
    }

    // ── reconcilePsi: monotone ψ untouched; impact counterfeit reconstructed ─
    std::printf("=== reconcilePsi ===\n");
    {
        const int nf = 20;
        std::vector<double> theta(nf), phi(nf, 0.0), evAt(nf, 0.6);
        std::vector<SwingPhase> phase(nf, SwingPhase::Downswing);
        std::vector<char> bandOk(nf, 0);
        for (int f = 0; f < nf; ++f) theta[f] = 100.0 - 5.0 * f;   // strictly decreasing ψ (φ=0)
        const ReconResult a = reconcilePsi(theta, phi, phase, bandOk, evAt, /*top=*/1000, nf, cfg);
        bool noRecon = true; for (char r : a.recon) if (r) noRecon = false;
        check(noRecon, "monotone ψ ⇒ no reconstruction");
        check(near(a.thetaOut[10], theta[10], 1e-6), "θ preserved when monotone");

        std::vector<double> theta2 = theta;
        theta2[10] = 200.0;                     // wild impact-frame counterfeit
        std::vector<SwingPhase> phase2 = phase; phase2[10] = SwingPhase::Impact;
        const ReconResult b = reconcilePsi(theta2, phi, phase2, bandOk, evAt, 1000, nf, cfg);
        check(b.recon[10] == 1, "impact counterfeit flagged recon");
        check(std::abs(b.thetaOut[10] - 200.0) > 20.0, "θ pulled back toward the monotone rail");
    }

    // ── vision-only segmentation mapping ─────────────────────────────────────
    std::printf("=== phasesToSegmentation ===\n");
    {
        PhaseModel pm;
        pm.bs0 = 40; pm.top = 80; pm.impact = 110; pm.fin0 = 150;
        const int nf = 200;
        std::vector<int64_t> tUs(nf);
        for (int i = 0; i < nf; ++i) tUs[i] = int64_t(i) * 6700;   // ~149 fps
        const Segmentation seg = phasesToSegmentation(pm, tUs, 0.5f);
        check(seg.events.size() == 4, "four ladder events");
        check(seg.conf == 0.5f, "vision-grade conf");
        bool ordered = true;
        for (size_t i = 1; i < seg.events.size(); ++i) if (seg.events[i].t_us < seg.events[i - 1].t_us) ordered = false;
        check(ordered, "events time-ordered");
        check(seg.events[0].phase == Phase::Address && seg.events[0].t_us == tUs[40], "Address at bs0");
        check(seg.events[2].phase == Phase::Impact && seg.events[2].t_us == tUs[110], "Impact at impact frame");
        check(seg.swingStartUs >= tUs.front() && seg.swingStartUs < tUs[40], "swingStart padded + clamped");
        check(seg.swingEndUs <= tUs.back() && seg.swingEndUs > tUs[150], "swingEnd padded + clamped");
        // degenerate (conf 0) still returns bounds but no swing claim
        const Segmentation deg = phasesToSegmentation(pm, tUs, 0.0f);
        check(deg.conf == 0.0f, "conf 0 passthrough (no swing)");
    }

    // ── A2 length ladder: rung precedence + clamps (projectedClubLenPx) ───────
    std::printf("=== projectedClubLenPx ===\n");
    {
        const double frameH = 1000.0, clubLenMm = 1120.0;
        int rung = 0;
        // rung 1 — ball measurement wins over every lower source
        double L = projectedClubLenPx(/*meas=*/400, /*sTypical=*/0.5, /*r0Med=*/100,
                                      /*poseExtent=*/705.5, /*armFloor=*/0, clubLenMm, frameH, cfg, rung);
        check(rung == 1 && near(L, 400, 1e-6), "rung 1 = ball L_px (400)");
        // rung 2 — band scale, grip-corrected: 0.5·(1120−100) = 510
        L = projectedClubLenPx(-1, 0.5, 100, 705.5, 0, clubLenMm, frameH, cfg, rung);
        check(rung == 2 && near(L, 510, 1e-6), "rung 2 = sTypical·(clubLenMm−r0Med) (510)");
        // rung 3 — pose scale: 705.5/(0.83·1.70)=500 px/m; 500·(1.12−0.13)=495
        L = projectedClubLenPx(-1, 0, 0, 705.5, 0, clubLenMm, frameH, cfg, rung);
        check(rung == 3 && near(L, 495, 0.5), "rung 3 = pose-scale surrogate (495)");
        // rung 4 — frame-height fallback: 0.45·1000
        L = projectedClubLenPx(-1, 0, 0, 0, 0, clubLenMm, frameH, cfg, rung);
        check(rung == 4 && near(L, 450, 1e-6), "rung 4 = 0.45·frameH (450)");
        // arm floor raises a short fallback length
        L = projectedClubLenPx(-1, 0, 0, 0, /*armFloor=*/600, clubLenMm, frameH, cfg, rung);
        check(rung == 4 && near(L, 600, 1e-6), "arm floor lifts the length (600)");
        // fallback ceiling caps a runaway band scale at 0.62·frameH = 620
        L = projectedClubLenPx(-1, 2.0, 0, 0, 0, clubLenMm, frameH, cfg, rung);
        check(rung == 2 && near(L, 620, 1e-6), "fallback ceiling caps at 0.62·frameH (620)");
        // ball ceiling (1.1·L_px) is authoritative even over a longer arm floor
        L = projectedClubLenPx(400, 0, 0, 0, /*armFloor=*/1000, clubLenMm, frameH, cfg, rung);
        check(rung == 1 && near(L, 440, 1e-6), "ball ceiling 1.1·L_px wins over the floor (440)");
    }

    // ── θ invariance: the ball only changes head/length, never θ ─────────────
    std::printf("=== decideTrack θ invariance vs ball ===\n");
    {
        const int nf = 60;
        const int W = 1000, H = 1000;
        const double fps = 100.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(f) * 10000;
            // long still address hold (0..39) so staticRuns clears stillMin=25
            // even after speed-smoothing bleed near the takeaway.
            if (f >= 40 && f < 53)      { x -= 4; y -= 9; }   // backswing (speed ≈ 9.8 > swSpd)
            else if (f >= 53 && f < 60) { x += 4; y += 9; }   // downswing
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};  // sh/hip/knee/ankle
        }
        // ball fixed below the ANKLE line (y=950 vs ankles at 900) and between
        // the feet, so the golf-prior gate accepts it; still-hold grip→ball
        // = |(500,950)−(500,600)| = 350 px
        BallTrack2D ball;
        for (int f = 0; f < nf; ++f) {
            BallSample2D b; b.t_us = tUs[f]; b.found = true;
            b.center = QPointF(0.50, 0.95); b.radiusNorm = 0.02f; b.conf = 1.f;
            ball.frames.push_back(b);
        }
        const FrameSource noFrames = [](int) -> cv::Mat { return cv::Mat(); };

        const ShaftTrack2D a = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           /*bandsMm=*/{}, /*clubLenMm=*/1120.0, /*impactFrame=*/-1,
                                           cfg, nullptr, /*ball=*/nullptr);
        const ShaftTrack2D b = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfg, nullptr, &ball);

        bool sameCount = a.samples.size() == b.samples.size() && !a.samples.empty();
        bool thetaIdentical = sameCount;
        bool headDiffers = false;
        for (size_t i = 0; sameCount && i < a.samples.size(); ++i) {
            if (a.samples[i].thetaRad != b.samples[i].thetaRad) thetaIdentical = false;
            if (a.samples[i].headPx != b.samples[i].headPx) headDiffers = true;
        }
        check(sameCount, "same sample count with/without ball");
        check(thetaIdentical, "θ bit-identical with/without ball");
        check(a.measuredClubLenPx < 0 && near(b.measuredClubLenPx, 350.0, 2.0),
              "ball populates measuredClubLenPx (~350 px); null leaves −1");
        check(headDiffers, "projected head length changed (length ladder used the ball)");
    }

    // ── Phase B: tier hoist is a pure refactor + head pass never perturbs θ ──
    std::printf("=== decideTrack head pass (Phase B) ===\n");
    {
        const int nf = 60, W = 480, H = 480;
        const double fps = 100.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 240, y = 150;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(f) * 10000;
            // long still address hold (0..39) so staticRuns clears stillMin=25,
            // then a short backswing/downswing (grip speed ≈ 6.7 > swSpd).
            if (f >= 40 && f < 53)      { x -= 3; y += 6; }
            else if (f >= 53 && f < 60) { x += 3; y -= 6; }
            gx[f] = x; gy[f] = y;
            joints[f] = {{210, 90}, {270, 90}, {215, 200}, {265, 200},
                         {218, 300}, {262, 300}, {220, 380}, {260, 380}};  // sh/hip/knee/ankle
        }
        // ball fixed below the ANKLE line (y=408 vs ankles at 380 — the A1
        // golf-prior gate rejects locks above it) so still-hold grip→ball
        // ≈ 258 px is measurable and the head pass gets its L_px prior
        BallTrack2D ball;
        for (int f = 0; f < nf; ++f) {
            BallSample2D b; b.t_us = tUs[f]; b.found = true;
            b.center = QPointF(0.50, 0.85); b.radiusNorm = 0.02f; b.conf = 1.f;
            ball.frames.push_back(b);
        }
        // Deterministic synthetic frames: mid-grey with a downward club line from
        // the grip + a ball blob, so sceneMed is non-empty (else the head pass
        // no-ops) and the pass does real Sobel/measure work. Measurement QUALITY
        // is irrelevant — the invariants below hold regardless.
        const FrameSource render = [&](int f) -> cv::Mat {
            if (f < 0 || f >= nf) return cv::Mat();
            cv::Mat img(H, W, CV_8UC1, cv::Scalar(30));
            const cv::Point g{int(gx[f]), int(gy[f])};
            cv::line(img, g, cv::Point(g.x, std::min(H - 1, g.y + 150)), cv::Scalar(220), 3);
            cv::circle(img, cv::Point(int(0.50 * W), int(0.85 * H)), 7, cv::Scalar(240), -1);
            return img;
        };

        ShaftV3Config cfgOff = cfg; cfgOff.head.enabled = false;   // explicit — default is ON since the gate flip
        ShaftV3Config cfgOn  = cfg; cfgOn.head.enabled = true;
        ShaftDecideTrace trOff, trOn;
        const ShaftTrack2D a = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfgOff, &trOff, &ball);
        const ShaftTrack2D b = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfgOn,  &trOn,  &ball);

        // (b) θ bit-identical head off vs on, on every sample — the head pass
        // must never perturb the stage-1 θ path.
        bool sameCount = a.samples.size() == b.samples.size() && !a.samples.empty();
        bool thetaIdentical = sameCount;
        for (size_t i = 0; sameCount && i < a.samples.size(); ++i)
            if (a.samples[i].thetaRad != b.samples[i].thetaRad) thetaIdentical = false;
        check(sameCount, "same sample count head off/on");
        check(thetaIdentical, "θ bit-identical head off/on");

        // (a) the tier hoist is a pure refactor: tier/conf/θ trace identical off
        // vs on (the head pass reads tierOf[], never rewrites it).
        bool traceMatch = trOff.tier.size() == trOn.tier.size()
                       && trOff.conf.size() == trOn.conf.size()
                       && trOff.thetaDeg.size() == trOn.thetaDeg.size() && !trOff.tier.empty();
        for (size_t i = 0; traceMatch && i < trOff.tier.size(); ++i) {
            if (trOff.tier[i]     != trOn.tier[i])     traceMatch = false;
            if (trOff.conf[i]     != trOn.conf[i])     traceMatch = false;
            if (trOff.thetaDeg[i] != trOn.thetaDeg[i]) traceMatch = false;
        }
        check(traceMatch, "tier/conf/θ trace identical head off/on (pure hoist)");

        // (b) headMs + head trace populated only when enabled.
        check(trOff.headMs == 0.0 && trOff.headTier.empty(), "head trace empty when disabled");
        check(trOn.headMs > 0.0 && trOn.headTier.size() == size_t(nf),
              "headMs + headTier populated when enabled");

        // (c) the off-frame flag is only ever set when headPx lies on the frame
        // boundary (edge-clamped, not a head) and always co-set with 0x10.
        bool offInvariant = true;
        for (const ShaftSample2D &sm : b.samples) {
            if (!(sm.flags & ShaftHeadOffFrame)) continue;
            const double hx = sm.headPx.x(), hy = sm.headPx.y();
            const double dEdge = std::min(std::min(std::abs(hx), std::abs(hx - (W - 1))),
                                          std::min(std::abs(hy), std::abs(hy - (H - 1))));
            if (dEdge > 0.5) offInvariant = false;
            if (!(sm.flags & ShaftHeadProjected)) offInvariant = false;
        }
        check(offInvariant, "off-frame flag ⇒ headPx on the frame boundary + projected");
    }

    // ── Phase B: backswing streak confidence cap (wiring, gateB iter-2) ──────
    // The FINAL emitted sample headConf is capped at head.streakConfCap for
    // frames in [bs0, top]; outside that window it must be untouched. Two runs
    // differing ONLY in the cap value (default 0.45 vs 0.0) prove both the cap
    // and its window-scoping without depending on measurement quality: with a
    // ball present the head pass always writes headConf on non-off frames (the
    // pred tier bridges at L_px when no smoothed r exists), so the window is
    // guaranteed non-vacuous.
    std::printf("=== decideTrack streak confidence cap ===\n");
    {
        // Motion profile copied from the segmentPhases test above (nf=170,
        // fps=150, speeds 9.8/9.5 px/f > swSpd=8), which asserts bs0<top<impact
        // on exactly this shape — the earlier 6.7 px/f profile never crossed
        // swSpd and left the phase model degenerate (whole-clip address).
        // Geometry (joints/ball) from the θ-invariance test: grip→ball 350 px.
        const int nf = 170, W = 1000, H = 1000;
        const double fps = 150.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(std::llround(f * 1e6 / fps));
            if (f >= 40 && f < 70)       { x -= 4; y -= 9; }   // backswing
            else if (f >= 77 && f < 109) { x += 3; y += 9; }   // downswing
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};  // sh/hip/knee/ankle
        }
        BallTrack2D ball;
        for (int f = 0; f < nf; ++f) {
            BallSample2D b; b.t_us = tUs[f]; b.found = true;
            b.center = QPointF(0.50, 0.95); b.radiusNorm = 0.02f; b.conf = 1.f;   // (500,950), below ankles
            ball.frames.push_back(b);
        }
        // 300-px club line: clears the phase-ramped floor (0.8→0.5 of
        // L_px = 350 ⇒ 280→175) across the backswing window, inside the
        // 1.15·L_px = 402 ceiling.
        const FrameSource render = [&](int f) -> cv::Mat {
            if (f < 0 || f >= nf) return cv::Mat();
            cv::Mat img(H, W, CV_8UC1, cv::Scalar(30));
            const cv::Point g{int(gx[f]), int(gy[f])};
            cv::line(img, g, cv::Point(g.x, std::min(H - 1, g.y + 300)), cv::Scalar(220), 3);
            cv::circle(img, cv::Point(int(0.50 * W), int(0.95 * H)), 7, cv::Scalar(240), -1);
            return img;
        };

        ShaftV3Config cfgCap = cfg; cfgCap.head.enabled = true;    // streakConfCap default 0.45
        ShaftV3Config cfgZero = cfgCap; cfgZero.head.streakConfCap = 0.0;
        ShaftDecideTrace trA, trZ;
        const ShaftTrack2D A = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfgCap,  &trA, &ball);
        const ShaftTrack2D Z = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfgZero, &trZ, &ball);
        const int bs0 = trA.phases.bs0, top = trA.phases.top;
        check(bs0 > 0 && top > bs0, "phase model found the swing (bs0 < top)");
        const bool sizeOk = A.samples.size() == Z.samples.size()
                         && A.samples.size() == trA.frameIdx.size() && !A.samples.empty();
        check(sizeOk, "sample/trace sizes agree across cap settings");

        int inWinWritten = 0;
        bool capHonoured = true, zeroCapped = true, outsideIdentical = true;
        for (size_t k = 0; sizeOk && k < A.samples.size(); ++k) {
            const int i = trA.frameIdx[k];
            const float ca = A.samples[k].headConf, cz = Z.samples[k].headConf;
            if (i >= bs0 && i <= top) {
                if (ca >= 0.f) {
                    ++inWinWritten;
                    if (ca > float(cfgCap.head.streakConfCap) + 1e-6f) capHonoured = false;
                }
                if (cz > 0.f) zeroCapped = false;   // cap 0 forces every written conf to 0
            } else if (ca != cz) {
                outsideIdentical = false;           // the cap must never leak outside the window
            }
        }
        check(inWinWritten > 0, "head results written inside [bs0, top] (non-vacuous)");
        check(capHonoured, "in-window emitted headConf ≤ streakConfCap");
        check(zeroCapped, "cap = 0 forces every in-window written headConf to 0");
        check(outsideIdentical, "headConf outside the window identical across cap settings");
    }

    // ── A2b fusion: OFF ⇒ ladder unchanged + prior ignored ───────────────────
    // With cfg.fusion.enabled=false the length ladder is byte-identical whether or
    // not a (strong) prior is supplied — the fusion pre/post-pass never run and
    // out.lengths stays default. Reuses the θ-invariance geometry (grip→ball 350).
    std::printf("=== decideTrack fusion OFF ignores prior ===\n");
    {
        const int nf = 60, W = 1000, H = 1000;
        const double fps = 100.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(f) * 10000;
            if (f >= 40 && f < 53)      { x -= 4; y -= 9; }
            else if (f >= 53 && f < 60) { x += 4; y += 9; }
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};
        }
        BallTrack2D ball;
        for (int f = 0; f < nf; ++f) {
            BallSample2D b; b.t_us = tUs[f]; b.found = true;
            b.center = QPointF(0.50, 0.95); b.radiusNorm = 0.02f; b.conf = 1.f;
            ball.frames.push_back(b);
        }
        const FrameSource noFrames = [](int) -> cv::Mat { return cv::Mat(); };

        ShaftV3Config cfgOff = cfg; cfgOff.fusion.enabled = false;
        LengthPriorState strong; strong.emaPx = 470.0; strong.varPx = 600.0; strong.n = 8;

        const ShaftTrack2D withP = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                               {}, 1120.0, -1, cfgOff, nullptr, &ball, &strong);
        const ShaftTrack2D noP   = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                               {}, 1120.0, -1, cfgOff, nullptr, &ball, nullptr);
        bool sameCount = withP.samples.size() == noP.samples.size() && !withP.samples.empty();
        bool identical = sameCount;
        for (size_t i = 0; sameCount && i < withP.samples.size(); ++i)
            if (withP.samples[i].thetaRad != noP.samples[i].thetaRad
                || withP.samples[i].headPx != noP.samples[i].headPx) identical = false;
        check(sameCount && identical, "fusion off ⇒ track identical with/without a prior");
        check(withP.lengths.fusedPx < 0.0 && noP.lengths.fusedPx < 0.0,
              "fusion off ⇒ no fused length recorded (out.lengths default)");
    }

    // ── A2b fusion: a strong prior drives the ladder to rung 0 ────────────────
    // No ball, no band (untaped) ⇒ instantaneous fusion abstains and the ladder
    // falls to the pose surrogate (rung 3). A matured prior (n≥2) inside the pose
    // sanity band makes the prior-only fusion clear ladderConfMin ⇒ rung 0.
    std::printf("=== decideTrack strong prior ⇒ rung 0 ===\n");
    {
        const int nf = 60, W = 1000, H = 1000;
        const double fps = 100.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(f) * 10000;
            if (f >= 40 && f < 53)      { x -= 4; y -= 9; }
            else if (f >= 53 && f < 60) { x += 4; y += 9; }
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};
        }
        const FrameSource noFrames = [](int) -> cv::Mat { return cv::Mat(); };

        // no ball ⇒ instantaneous set is empty; prior ema 400 ∈ pose band [~379, 620]
        LengthPriorState prior; prior.emaPx = 400.0; prior.varPx = 576.0; prior.n = 4;
        ShaftDecideTrace trNoPrior, trPrior;
        const ShaftTrack2D a = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfg, &trNoPrior, nullptr, nullptr);
        const ShaftTrack2D b = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfg, &trPrior,   nullptr, &prior);
        check(trNoPrior.projLenRung != 0, "no ball / no prior ⇒ ladder falls to a pose/height rung");
        check(trPrior.projLenRung == 0, "strong prior ⇒ pre-pass fusion sets rung 0");
        check(b.lengths.priorN == 4 && b.lengths.fusedConf >= cfg.fusion.ladderConfMin,
              "prior joined the fuse (priorN=4, conf ≥ ladderConfMin)");
        check(b.lengths.fusedPx > 0.0 && a.lengths.fusedPx < 0.0,
              "prior ⇒ fused length recorded; no estimators ⇒ abstain");
    }

    // ── Layer A snap: the drawn line re-registers onto an offset shaft ────────
    // Render a bright shaft deliberately offset +12 px perpendicular from the
    // injected grip anchor. Off the shaft, the v3 evidence engines lock a
    // COMPROMISE ray that clips it (design §2A finding #1) — here a ~15° tilt —
    // so the drawn line is a measured tier but sits off the club. With snap on
    // (override cfg, wide enough to admit that compromise), every measured line
    // registers onto the true shaft and lineConf reports it; with snap off the
    // samples carry no lineConf and are untouched.
    std::printf("=== decideTrack snap re-registration (Layer A) ===\n");
    {
        const int nf = 170, W = 1000, H = 1000;
        const double fps = 150.0;
        const int kOffset = 12;   // shaft rendered +12 px perpendicular (in −x) from the grip
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(std::llround(f * 1e6 / fps));
            if (f >= 40 && f < 70)       { x -= 4; y -= 9; }
            else if (f >= 77 && f < 109) { x += 3; y += 9; }
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};
        }
        const FrameSource render = [&](int f) -> cv::Mat {
            if (f < 0 || f >= nf) return cv::Mat();
            cv::Mat img(H, W, CV_8UC1, cv::Scalar(30));
            const cv::Point top{int(gx[f]) - kOffset, int(gy[f])};   // MSVC MVP: brace-init the Point
            cv::line(img, top, cv::Point(top.x, std::min(H - 1, top.y + 400)), cv::Scalar(230), 3);
            return img;
        };
        // Head pass off ⇒ every measured frame draws a Phase-A projected head, so
        // the snap moves grip+θ+head together (the projected-head branch).
        ShaftV3Config cfgOff = cfg; cfgOff.snap.enabled = false; cfgOff.head.enabled = false;
        ShaftV3Config cfgOn  = cfgOff; cfgOn.snap.enabled = true;
        cfgOn.snap.maxOffsetPx = 22.0; cfgOn.snap.maxDeltaDeg = 20.0;  // admit the ~15° compromise tilt
        ShaftDecideTrace trOn;
        const ShaftTrack2D off = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgOff, nullptr, nullptr);
        const ShaftTrack2D on  = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgOn,  &trOn, nullptr);

        const bool sizeOk = off.samples.size() == on.samples.size()
                         && on.samples.size() == trOn.frameIdx.size() && !on.samples.empty();
        check(sizeOk, "snap off/on emit the same sample count");

        int measured = 0, moved = 0;
        bool distOk = true, confOk = true, offInert = true, offNoConf = true, onConfSet = true;
        for (size_t k = 0; sizeOk && k < on.samples.size(); ++k) {
            const ShaftSample2D &so = off.samples[k], &sn = on.samples[k];
            const bool vision = (sn.flags & ShaftMeasured) || (sn.flags & ShaftWedge);
            // (b) snap off never writes lineConf.
            if (so.lineConf != -1.f) offNoConf = false;
            if (!vision) {
                // (b) snap only ever touches vision-tier samples — everything else is
                // byte-identical off vs on, including the (absent) lineConf.
                if (sn.gripPx != so.gripPx || sn.headPx != so.headPx
                    || sn.thetaRad != so.thetaRad || sn.lineConf != -1.f) offInert = false;
                continue;
            }
            ++measured;
            if (sn.lineConf < 0.f) onConfSet = false;           // measured ⇒ lineConf recorded
            const int i = trOn.frameIdx[k];
            const double ux = std::cos(sn.thetaRad), uy = std::sin(sn.thetaRad);
            // ⊥ distance from the true shaft anchor (grip − kOffset in x) to the
            // snapped drawn line.
            const double px = gx[i] - kOffset - sn.gripPx.x(), py = gy[i] - sn.gripPx.y();
            const double dist = std::abs(px * uy - py * ux);
            if (dist > 2.0) distOk = false;
            if (sn.lineConf <= 0.7f) confOk = false;
            if (sn.gripPx != so.gripPx) ++moved;
        }
        check(measured > 0, "measured frames exist off an offset anchor (compromise rays)");
        check(distOk, "(a) every snapped line lands within 2 px ⊥ of the true shaft");
        check(confOk, "(a) lineConf > 0.7 on every measured frame");
        check(onConfSet, "measured frames all carry a recorded lineConf (≥0)");
        check(moved == measured, "snap moved the anchor on every measured frame");
        check(offNoConf, "(b) snap off leaves lineConf = -1 on every sample");
        check(offInert, "(b) snap touches only vision-tier samples (rest byte-identical off vs on)");
        check(trOn.snapAppliedN == measured && trOn.medianSnapOffsetPx > 10.0
              && trOn.medianSnapOffsetPx < 14.0,
              "trace: snapAppliedN + median ⊥ offset ≈ the injected 12 px");
        check(trOn.medianLineConf > 0.7, "trace: median lineConf > 0.7");
    }

    // ── Layer B positions: report-only, additive (shaft_position_first §2B) ────
    // positions.enabled=true fills out.positions from the emitted track (ordered,
    // monotone t_us); default (false) leaves it empty AND does not perturb samples
    // (byte-identical contract). Reuses the segmentPhases-validated full-swing
    // geometry (nf=170, fps=150) so bs0<top<impact and the P1/P4/P7 landmarks
    // (address/top/impact) are always emitted regardless of ray evidence.
    std::printf("=== decideTrack Layer B positions ===\n");
    {
        const int nf = 170, W = 1000, H = 1000;
        const double fps = 150.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(std::llround(f * 1e6 / fps));
            if (f >= 40 && f < 70)       { x -= 4; y -= 9; }   // backswing
            else if (f >= 77 && f < 109) { x += 3; y += 9; }   // downswing
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};
        }
        const FrameSource noFrames = [](int) -> cv::Mat { return cv::Mat(); };

        // Pin both states explicitly — this tests the off-contract mechanism, not
        // the shipped default (ON since B4).
        ShaftV3Config cfgOff = cfg; cfgOff.positions.enabled = false;
        ShaftV3Config cfgOn  = cfg; cfgOn.positions.enabled = true;
        const ShaftTrack2D off = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgOff, nullptr, nullptr);
        const ShaftTrack2D on  = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgOn,  nullptr, nullptr);

        check(off.positions.empty(), "positions off ⇒ empty vector");
        check(!on.positions.empty(), "positions on ⇒ non-empty vector");
        bool ordered = true;
        for (size_t i = 1; i < on.positions.size(); ++i)
            if (on.positions[i].p <= on.positions[i - 1].p
                || on.positions[i].t_us < on.positions[i - 1].t_us) ordered = false;
        check(ordered, "positions ordered (p strictly ↑, t_us ↑ — incl. P10/Finish)");
        bool pInRange = true, srcOk = true;
        for (const ShaftPosition &p : on.positions) {
            if (p.p < 1 || p.p > 10) pInRange = false;
            if (p.source != uint8_t(PositionSource::TrackSample) || p.stackN != 0
                || p.sigmaThetaDeg != -1.f || p.sigmaLenPx != -1.f) srcOk = false;
        }
        check(pInRange, "every p ∈ [1,10]");
        check(srcOk, "B1 provenance: TrackSample, stackN 0, σ = −1");

        // (byte-identical contract) positions extraction never touches samples.
        bool sameCount = off.samples.size() == on.samples.size() && !off.samples.empty();
        bool identical = sameCount;
        for (size_t i = 0; sameCount && i < off.samples.size(); ++i)
            if (off.samples[i].thetaRad != on.samples[i].thetaRad
                || off.samples[i].gripPx != on.samples[i].gripPx
                || off.samples[i].headPx != on.samples[i].headPx
                || off.samples[i].flags  != on.samples[i].flags
                || off.samples[i].conf   != on.samples[i].conf) identical = false;
        check(sameCount && identical, "positions on/off ⇒ samples byte-identical");
    }

    // ── Layer B B2 milestone fit: fitEnabled=false ⇒ B1 byte-identical, and the
    //    fit NEVER touches samples[] (soak contract, shaft_position_first §2B) ────
    // Real (decodable) frames so the fit COULD run — the gate, not an empty stack,
    // is what keeps a fit-off run identical.
    std::printf("=== decideTrack Layer B fit (B2) off-contract ===\n");
    {
        const int nf = 170, W = 1000, H = 1000;
        const double fps = 150.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(std::llround(f * 1e6 / fps));
            if (f >= 40 && f < 70)       { x -= 4; y -= 9; }
            else if (f >= 77 && f < 109) { x += 3; y += 9; }
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};
        }
        const FrameSource render = [&](int f) -> cv::Mat {
            if (f < 0 || f >= nf) return cv::Mat();
            cv::Mat img(H, W, CV_8UC1, cv::Scalar(30));
            const cv::Point g{int(gx[f]), int(gy[f])};
            cv::line(img, g, cv::Point(g.x, std::min(H - 1, g.y + 400)), cv::Scalar(230), 3);
            return img;
        };

        ShaftV3Config cfgB1 = cfg; cfgB1.positions.enabled = true;                  // fit off (default)
        ShaftV3Config cfgB2 = cfg; cfgB2.positions.enabled = true;
        cfgB2.positions.fit.fitEnabled = true;                                      // fit on
        const ShaftTrack2D b1  = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgB1, nullptr, nullptr);
        const ShaftTrack2D b1b = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgB1, nullptr, nullptr);
        const ShaftTrack2D b2  = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgB2, nullptr, nullptr);

        bool b1IsTrack = !b1.positions.empty();
        for (const ShaftPosition &p : b1.positions)
            if (p.source != uint8_t(PositionSource::TrackSample) || p.stackN != 0
                || p.sigmaThetaDeg != -1.f || p.sigmaLenPx != -1.f) b1IsTrack = false;
        check(b1IsTrack, "fitEnabled=false ⇒ all positions TrackSample (fit gated)");

        bool byteEq = b1.positions.size() == b1b.positions.size();
        for (size_t i = 0; byteEq && i < b1.positions.size(); ++i) {
            const ShaftPosition &a = b1.positions[i], &c = b1b.positions[i];
            if (a.p != c.p || a.t_us != c.t_us || a.thetaRad != c.thetaRad || a.lenPx != c.lenPx
                || a.gripPx != c.gripPx || a.headPx != c.headPx || a.conf != c.conf
                || a.source != c.source || a.stackN != c.stackN) byteEq = false;
        }
        check(byteEq, "fitEnabled=false ⇒ positions byte-identical run-to-run (B1 soak)");

        bool sameSamples = b1.samples.size() == b2.samples.size() && !b1.samples.empty();
        for (size_t i = 0; sameSamples && i < b1.samples.size(); ++i)
            if (b1.samples[i].thetaRad != b2.samples[i].thetaRad
                || b1.samples[i].gripPx != b2.samples[i].gripPx
                || b1.samples[i].headPx != b2.samples[i].headPx
                || b1.samples[i].flags  != b2.samples[i].flags) sameSamples = false;
        check(sameSamples, "fit on/off ⇒ samples[] byte-identical (fit upgrades positions[] only)");
    }

    // ── Layer C synthesis: synth.enabled=false (default) ⇒ out.synth empty AND
    //    samples[]/positions[] byte-identical; enabled ⇒ synth between anchors only
    //    (shaft_position_first §2 Layer C) ────────────────────────────────────────
    // Reuses the full-swing geometry so ≥2 P-anchors are located; noFrames is fine —
    // synthesis interpolates the located positions, it never decodes frames.
    std::printf("=== decideTrack Layer C synthesis ===\n");
    {
        const int nf = 170, W = 1000, H = 1000;
        const double fps = 150.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(std::llround(f * 1e6 / fps));
            if (f >= 40 && f < 70)       { x -= 4; y -= 9; }
            else if (f >= 77 && f < 109) { x += 3; y += 9; }
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};
        }
        const FrameSource noFrames = [](int) -> cv::Mat { return cv::Mat(); };

        // Pin both states explicitly — synth is OFF by default; positions ON.
        ShaftV3Config cfgOff = cfg; cfgOff.positions.enabled = true; cfgOff.synth.enabled = false;
        ShaftV3Config cfgOn  = cfg; cfgOn.positions.enabled  = true; cfgOn.synth.enabled  = true;
        const ShaftTrack2D off = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgOff, nullptr, nullptr);
        const ShaftTrack2D on  = decideTrack(noFrames, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgOn,  nullptr, nullptr);

        check(off.synth.empty(), "synth off ⇒ empty vector");
        check(on.positions.size() >= 2 && !on.synth.empty(), "synth on ⇒ non-empty between anchors");

        // Every synthesized sample carries only ShaftSynthesized, is strictly inside
        // the anchor span, and is monotone in t_us.
        const int64_t lo = on.positions.front().t_us, hi = on.positions.back().t_us;
        bool flagsOk = true, windowOk = true, monoOk = true;
        for (size_t i = 0; i < on.synth.size(); ++i) {
            if (on.synth[i].flags != ShaftSynthesized) flagsOk = false;
            if (on.synth[i].t_us <= lo || on.synth[i].t_us >= hi) windowOk = false;
            if (i && on.synth[i].t_us <= on.synth[i - 1].t_us) monoOk = false;
        }
        check(flagsOk, "synth: every sample flagged ShaftSynthesized (0x100)");
        check(windowOk, "synth: nothing outside (first, last) anchor");
        check(monoOk, "synth: t_us strictly increasing");

        // (byte-identical contract) synthesis never touches samples[] or positions[].
        bool sameSamples = off.samples.size() == on.samples.size() && !off.samples.empty();
        for (size_t i = 0; sameSamples && i < off.samples.size(); ++i)
            if (off.samples[i].thetaRad != on.samples[i].thetaRad
                || off.samples[i].gripPx != on.samples[i].gripPx
                || off.samples[i].headPx != on.samples[i].headPx
                || off.samples[i].flags  != on.samples[i].flags
                || off.samples[i].conf   != on.samples[i].conf) sameSamples = false;
        check(sameSamples, "synth on/off ⇒ samples[] byte-identical");

        bool samePos = off.positions.size() == on.positions.size() && !off.positions.empty();
        for (size_t i = 0; samePos && i < off.positions.size(); ++i) {
            const ShaftPosition &a = off.positions[i], &b = on.positions[i];
            if (a.p != b.p || a.t_us != b.t_us || a.thetaRad != b.thetaRad || a.lenPx != b.lenPx
                || a.gripPx != b.gripPx || a.headPx != b.headPx || a.conf != b.conf
                || a.source != b.source || a.stackN != b.stackN) samePos = false;
        }
        check(samePos, "synth on/off ⇒ positions[] byte-identical");
    }

    // ── S1 evidence honesty: keys-on demotion (shaft_wedge_p6_impl.md "Session 1
    //    spec"). Sharp rendered shaft line through the backswing, PURE NOISE
    //    (no painted line) through mid-downswing — the phantom-evidence shape
    //    the floor/support gate exists to kill. Default (keys off) is the
    //    fabricated-evidence baseline; evAbsFloor=20/raySupportMin=0.25 (the
    //    doc's chosen values) must demote every noise-only downswing frame off
    //    ShaftMeasured while the genuinely-evidenced backswing stays measured. ─
    std::printf("=== decideTrack S1 evidence honesty: keys-on demotion ===\n");
    {
        const int nf = 170, W = 1000, H = 1000;
        const double fps = 150.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(std::llround(f * 1e6 / fps));
            if (f >= 40 && f < 70)       { x -= 4; y -= 9; }   // backswing (speed ≈ 9.8 > swSpd)
            else if (f >= 77 && f < 109) { x += 3; y += 9; }   // downswing
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};  // sh/hip/knee/ankle
        }
        // Backswing frames [40,70): a sharp bright shaft line through the grip
        // (contrast 190, well past any credible floor). Downswing frames
        // [77,109): pure per-frame Gaussian noise, NO painted line — nothing for
        // an honest evidence engine to see. Everywhere else: flat background
        // (address/top/finish — not asserted on).
        const FrameSource render = [&](int f) -> cv::Mat {
            if (f < 0 || f >= nf) return cv::Mat();
            cv::Mat img(H, W, CV_8UC1, cv::Scalar(30));
            if (f >= 40 && f < 70) {
                const cv::Point g{int(gx[f]), int(gy[f])};
                cv::line(img, g, cv::Point(g.x, std::min(H - 1, g.y + 300)), cv::Scalar(220), 3);
            } else if (f >= 77 && f < 109) {
                cv::RNG rng(uint64(1000 + f));            // deterministic per-frame
                rng.fill(img, cv::RNG::NORMAL, 30, 5);
            }
            return img;
        };

        // Post-flip (2026-08-10) the defaults carry the S1/S2 keys ON, so the
        // legacy fabricating tracker is now the EXPLICITLY-zeroed config.
        ShaftV3Config cfgOff;
        cfgOff.evAbsFloor = 0.0; cfgOff.raySupportMin = 0.0; cfgOff.wedge.enabled = false;
        ShaftV3Config cfgOn = cfgOff;
        cfgOn.evAbsFloor = 20.0;
        cfgOn.raySupportMin = 0.25;

        const ShaftTrack2D off = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgOff, nullptr, nullptr);
        const ShaftTrack2D on  = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                             {}, 1120.0, -1, cfgOn,  nullptr, nullptr);
        const bool sizeOk = off.samples.size() == size_t(nf) && on.samples.size() == size_t(nf);
        check(sizeOk, "full per-frame sample coverage (no NaN grip) both runs");

        int measDownOff = 0, measDownOn = 0, measBackOn = 0, measTotalOff = 0, measTotalOn = 0;
        for (int f = 0; sizeOk && f < nf; ++f) {
            const bool measOff = (off.samples[size_t(f)].flags & ShaftMeasured) != 0;
            const bool measOn  = (on.samples[size_t(f)].flags  & ShaftMeasured) != 0;
            if (measOff) ++measTotalOff;
            if (measOn)  ++measTotalOn;
            if (f >= 77 && f < 109) {
                if (measOff) ++measDownOff;
                if (measOn)  ++measDownOn;
            }
            if (f >= 40 && f < 70 && measOn) ++measBackOn;
        }
        std::printf("  (measured: off-total=%d on-total=%d off-downswing=%d on-downswing=%d on-backswing=%d)\n",
                    measTotalOff, measTotalOn, measDownOff, measDownOn, measBackOn);
        check(measDownOn == 0, "keys on: no mid-downswing (noise-only) frame carries ShaftMeasured");
        check(measBackOn > 0, "keys on: at least one backswing frame stays ShaftMeasured");
        check(measTotalOn < measTotalOff, "keys on strictly drops the Measured-frame count vs default");
    }

    // ── S1 evidence honesty: keys-off byte-identity ──────────────────────────
    // evAbsFloor<=0 / raySupportMin<=0 must collapse to the pre-S1 expressions
    // exactly. Post-flip the implicit default is no longer dark, so the pin is
    // now: a directly-assigned dark config and a fromOverrides-built dark
    // config (the tuning-key route the params files use) must match
    // field-by-field on every emitted sample — the dark idiom AND the override
    // plumbing, in one contract.
    std::printf("=== decideTrack S1 evidence honesty: keys-off byte-identity ===\n");
    {
        const int nf = 60, W = 1000, H = 1000;
        const double fps = 100.0;
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf, 90.0);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(f) * 10000;
            if (f >= 40 && f < 53)      { x -= 4; y -= 9; }   // backswing
            else if (f >= 53 && f < 60) { x += 4; y += 9; }   // downswing
            gx[f] = x; gy[f] = y;
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};
        }
        const FrameSource render = [&](int f) -> cv::Mat {
            if (f < 0 || f >= nf) return cv::Mat();
            cv::Mat img(H, W, CV_8UC1, cv::Scalar(30));
            const cv::Point g{int(gx[f]), int(gy[f])};
            cv::line(img, g, cv::Point(g.x, std::min(H - 1, g.y + 150)), cv::Scalar(220), 3);
            return img;
        };

        ShaftV3Config cfgDirectOff;                   // dark by direct field assignment
        cfgDirectOff.evAbsFloor    = 0.0;
        cfgDirectOff.evAbsFloorDif = -1.0;
        cfgDirectOff.raySupportMin = 0.0;
        cfgDirectOff.wedge.enabled = false;
        QVariantMap darkKeys;                         // dark via the tuning-key route
        darkKeys.insert("shaft.evAbsFloor", 0.0);
        darkKeys.insert("shaft.evAbsFloorDif", -1.0);
        darkKeys.insert("shaft.raySupportMin", 0.0);
        darkKeys.insert("shaft.wedge.enabled", 0);
        const ShaftV3Config cfgExplicitOff = ShaftV3Config::fromOverrides(darkKeys);

        const ShaftTrack2D a = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfgDirectOff,   nullptr, nullptr);
        const ShaftTrack2D b = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                           {}, 1120.0, -1, cfgExplicitOff, nullptr, nullptr);

        bool sameCount = a.samples.size() == b.samples.size() && !a.samples.empty();
        bool identical = sameCount;
        for (size_t i = 0; sameCount && i < a.samples.size(); ++i) {
            const ShaftSample2D &sa = a.samples[i], &sb = b.samples[i];
            if (sa.t_us != sb.t_us || sa.gripPx != sb.gripPx || sa.thetaRad != sb.thetaRad
                || sa.conf != sb.conf || sa.flags != sb.flags) identical = false;
        }
        check(sameCount, "same sample count keys off vs default");
        check(identical, "samples[] field-by-field identical (t_us/gripPx/thetaRad/conf/flags)");
        check(a.coverage == b.coverage, "coverage identical");
        check(a.valid == b.valid, "track.valid identical");
    }

    // ── P7 impact geometry: shaft.impactGeom.* key plumbing + dark default ───
    // (impact_geom_test covers the detector/decision math; this pins the
    // fromOverrides route the params files use, and the dark-at-merge default.)
    std::printf("=== impactGeom key plumbing ===\n");
    {
        const ShaftV3Config def = ShaftV3Config::fromOverrides(QVariantMap{});
        check(!def.impactGeom.enabled, "impactGeom.enabled dark by default");
        check(!def.impactGeom.retime, "impactGeom.retime dark by default");
        QVariantMap ov;
        ov.insert("shaft.impactGeom.enabled", 1);
        ov.insert("shaft.impactGeom.retime", 1);
        ov.insert("shaft.impactGeom.hystDeg", 6.0);
        ov.insert("shaft.impactGeom.maxStepDeg", 90.0);
        ov.insert("shaft.impactGeom.overrideUs", 50000);
        const ShaftV3Config on = ShaftV3Config::fromOverrides(ov);
        check(on.impactGeom.enabled && on.impactGeom.retime, "enabled/retime keys applied");
        check(on.impactGeom.hystDeg == 6.0 && on.impactGeom.maxStepDeg == 90.0
                  && on.impactGeom.overrideUs == 50000,
              "hystDeg/maxStepDeg/overrideUs keys applied");
    }

    // ── S2 blur-wedge: the long-path fixture (shaft_wedge_p6_impl.md "Session 2
    //    spec"). Sharp rotating shaft line through the backswing; a painted
    //    semi-transparent FAN (5° sector, proximal r ≤ 80 px — too short for the
    //    frozen 90 px main-sweep line gate, exactly the R5/R8 blur shape) sweeping
    //    the LONG way 240° → 57° through the downswing. With the wedge dark the
    //    fan frames are fabricated ShaftMeasured (percentile normalisation);
    //    with S1 honesty + the wedge, they demote off Measured, the WEDGE tier
    //    claims them, the DP funds the long path, and locatePTimes pins P6 at
    //    the single horizontal crossing near truth. ─────────────────────────────
    std::printf("=== decideTrack S2 blur-wedge: long path + WEDGE tier + P6 ===\n");
    {
        const int nf = 170, W = 1000, H = 1000;
        const double fps = 150.0;
        const int bs0N = 40, topN = 70, dsN = 77, impN = 109;   // nominal (paint) anchors
        std::vector<int64_t> tUs(nf);
        std::vector<double> gx(nf), gy(nf), phiRaw(nf);
        std::vector<std::vector<cv::Point2d>> joints(nf, std::vector<cv::Point2d>(8));
        double x = 500, y = 600;
        // Lead-arm truth: 90° at address, ramp to 160° over the backswing
        // (chir = +1), hold, ramp to 50° through the downswing (−515°/s — a
        // realistic delivery arm rate, which the R6 predictor turns into a
        // >720°/s club rate mid-downswing via the wrist-cock release slope).
        auto phiOf = [&](int f) -> double {
            if (f < bs0N) return 90.0;
            if (f < topN) return 90.0 + 70.0 * double(f - bs0N) / double(topN - bs0N);
            if (f < dsN)  return 160.0;
            if (f < impN) return 160.0 - 110.0 * double(f - dsN) / double(impN - dsN);
            return 50.0;
        };
        for (int f = 0; f < nf; ++f) {
            tUs[f] = int64_t(std::llround(f * 1e6 / fps));
            if (f >= bs0N && f < topN)     { x -= 4; y -= 9; }   // backswing
            else if (f >= dsN && f < impN) { x += 3.75; y += 8.44; }  // downswing (returns to start)
            gx[f] = x; gy[f] = y;
            phiRaw[f] = phiOf(f);
            joints[f] = {{450, 300}, {550, 300}, {460, 600}, {540, 600},
                         {465, 750}, {535, 750}, {470, 900}, {530, 900}};
        }
        // Painted club truth: backswing line rotates 98° → 240° (the DP's
        // backswing sign allows only increase); hold at 240°; downswing fan
        // sweeps 240° → 57° the LONG way (monotone decreasing — the DP's
        // downswing sign) with a single horizontal (elevation-zero) transit.
        auto thetaPaint = [&](int f) -> double {
            if (f < topN) return 98.0 + 142.0 * double(f - bs0N) / double(topN - bs0N);
            if (f < dsN)  return 240.0;
            return 240.0 - 183.0 * double(f - dsN) / double(impN - dsN);
        };
        const FrameSource render = [&](int f) -> cv::Mat {
            if (f < 0 || f >= nf) return cv::Mat();
            cv::Mat img(H, W, CV_8UC1, cv::Scalar(30));
            const cv::Point2d g{gx[f], gy[f]};
            const double th = thetaPaint(f) * kPi / 180.0;
            if (f >= bs0N && f < dsN) {
                // sharp bright line, 300 px — a genuine thin-ridge measurement
                cv::line(img, cv::Point(int(g.x), int(g.y)),
                         cv::Point(int(g.x + 300 * std::cos(th)), int(g.y + 300 * std::sin(th))),
                         cv::Scalar(220), 3);
            } else if (f >= dsN && f < impN) {
                // semi-transparent fan: 5° sector about θ, proximal only (r ≤ 85)
                const double h = 2.5 * kPi / 180.0;
                const std::vector<cv::Point> tri = {
                    cv::Point(int(g.x), int(g.y)),
                    cv::Point(int(g.x + 85 * std::cos(th - h)), int(g.y + 85 * std::sin(th - h))),
                    cv::Point(int(g.x + 85 * std::cos(th + h)), int(g.y + 85 * std::sin(th + h)))};
                cv::fillConvexPoly(img, tri, cv::Scalar(55));
            }
            return img;
        };

        // Post-flip the defaults are the S2 arm, so the fabricating "dark"
        // baseline is reconstructed by explicitly zeroing the flipped keys.
        ShaftV3Config cfgDark;
        cfgDark.evAbsFloor = 0.0; cfgDark.raySupportMin = 0.0; cfgDark.wedge.enabled = false;
        ShaftV3Config cfgS2 = cfgDark;               // S1 honesty + S2 wedge + kinCone arm
        cfgS2.evAbsFloor    = 20.0;
        cfgS2.raySupportMin = 0.25;
        cfgS2.wedge.enabled = true;
        cfgS2.wedge.kinCone = true;

        ShaftDecideTrace trace;
        const ShaftTrack2D dark = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                              {}, 1120.0, -1, cfgDark, nullptr, nullptr);
        const ShaftTrack2D s2   = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                              {}, 1120.0, -1, cfgS2, &trace, nullptr);
        const bool sizeOk = dark.samples.size() == size_t(nf) && s2.samples.size() == size_t(nf);
        check(sizeOk, "full per-frame sample coverage both runs");

        const int top = trace.phases.top, impact = trace.phases.impact;
        auto elev = [&](double thDeg) {
            const double r = thDeg * kPi / 180.0;
            return std::atan2(std::sin(r), std::abs(std::cos(r))) * 180.0 / kPi;
        };

        int nWedge = 0, nWedgeAndMeas = 0, nDarkWedge = 0;
        int measDownDark = 0, measDownS2 = 0, measBackS2 = 0;
        for (int f = 0; sizeOk && f < nf; ++f) {
            const uint16_t fd = dark.samples[size_t(f)].flags, fs = s2.samples[size_t(f)].flags;
            if (fd & ShaftWedge) ++nDarkWedge;
            if (fs & ShaftWedge) { ++nWedge; if (fs & ShaftMeasured) ++nWedgeAndMeas; }
            // fan-painted frames only — the hold line [70,77) is a genuine sharp
            // measurement and legitimately stays RAY in both arms
            if (f >= dsN && f < impN) {
                if (fd & ShaftMeasured) ++measDownDark;
                if (fs & ShaftMeasured) ++measDownS2;
            }
            if (f >= bs0N && f < topN && (fs & ShaftMeasured)) ++measBackS2;
        }
        int tier4 = 0;
        for (size_t i = 0; i < trace.tier.size(); ++i) if (trace.tier[i] == 4) ++tier4;
        std::printf("  (dark: downswing-meas=%d; s2: wedge=%d tier4=%d downswing-meas=%d "
                    "backswing-meas=%d tExp=%.4fs top=%d impact=%d)\n",
                    measDownDark, nWedge, tier4, measDownS2, measBackS2,
                    trace.wedgeTExpS, top, impact);
        check(nDarkWedge == 0, "wedge dark ⇒ no ShaftWedge flag anywhere");
        check(measDownDark > 0, "wedge dark ⇒ fan frames fabricate ShaftMeasured (the defect)");
        check(nWedge >= 5, "S2: the WEDGE tier claims ≥5 fan frames");
        check(nWedgeAndMeas == 0, "S2: wedge frames carry ShaftWedge, deliberately NOT ShaftMeasured");
        check(tier4 == nWedge, "S2: trace tier 4 count matches the emitted flag count");
        check(measDownS2 == 0, "S2: no mid-downswing frame fabricates ShaftMeasured");
        check(measBackS2 > 0, "S2: the sharp backswing line stays genuinely Measured");
        check(trace.wedgeTExpS >= kTExpLoS && trace.wedgeTExpS <= kTExpHiS,
              "t_exp calibrated from the fan widths, inside the plausibility clamp");

        // Long path: the emitted θ sweeps ≥150° DOWN between top and impact and
        // transits horizontal exactly once (the wrapped-grid degeneracy broken).
        double sweep = 0.0; int crossings = 0;
        for (int f = top + 1; sizeOk && f <= impact; ++f) {
            const double a = s2.samples[size_t(f - 1)].thetaRad * 180.0 / kPi;
            const double b = s2.samples[size_t(f)].thetaRad * 180.0 / kPi;
            double d = std::fmod(b - a + 180.0, 360.0); if (d < 0) d += 360.0; d -= 180.0;
            sweep += d;
            if (f > top + 1 && elev(a) < 0.0 && elev(b) >= 0.0) ++crossings;
            if (f > top + 1 && elev(a) > 0.0 && elev(b) <= 0.0) ++crossings;
        }
        std::printf("  (downswing sweep %.0f°, horizontal crossings %d)\n", sweep, crossings);
        // ≥120°, not the painted 183°: the fan's final ~30° points down THROUGH
        // the synthetic body box, where the C2 veto (wC2 13/frame) outprices the
        // wedge well (wWell 6) and the DP holds short — the impact-zone θ there
        // is reconcilePsi's job (arm witness), not the wedge's. The load-bearing
        // proofs are the single horizontal transit and the P6 pin below, both of
        // which live ~90° before that sector. (The dark defect's short path
        // sweeps ~100° the WRONG way with no transit at all.)
        check(sweep <= -120.0, "S2: DP takes the long path (≥120° downswing sweep)");
        check(crossings == 1, "S2: exactly one horizontal transit in (top, impact)");

        // P6 from locatePTimes lands within 40 ms of the painted truth crossing.
        const double fCross = dsN + (240.0 - 180.0) / 183.0 * double(impN - dsN);
        const int64_t tTruth = int64_t(std::llround(fCross * 1e6 / fps));
        bool p6Found = false; int64_t p6Err = -1;
        for (const ShaftPosition& p : s2.positions)
            if (p.p == 6) { p6Found = true; p6Err = std::llabs(p.t_us - tTruth); }
        std::printf("  (P6 err %.1f ms)\n", p6Found ? double(p6Err) * 1e-3 : -1.0);
        check(p6Found, "S2: locatePTimes pins a P6 in (P4, P7)");
        check(p6Found && p6Err <= 40000, "S2: P6 within 0.04 s of the painted truth crossing");

        // Never-fabricate: wedge ENABLED but with no absolute floor reference
        // (evAbsFloor = 0) measures nothing — output field-identical to dark.
        ShaftV3Config cfgNoFloor = cfgDark;
        cfgNoFloor.wedge.enabled = true;
        const ShaftTrack2D nofl = decideTrack(render, tUs, gx, gy, phiRaw, joints, W, H, fps,
                                              {}, 1120.0, -1, cfgNoFloor, nullptr, nullptr);
        bool identical = nofl.samples.size() == dark.samples.size() && !dark.samples.empty();
        for (size_t i = 0; identical && i < dark.samples.size(); ++i) {
            const ShaftSample2D &sa = dark.samples[i], &sb = nofl.samples[i];
            if (sa.t_us != sb.t_us || sa.gripPx != sb.gripPx || sa.thetaRad != sb.thetaRad
                || sa.conf != sb.conf || sa.flags != sb.flags) identical = false;
        }
        check(identical && nofl.coverage == dark.coverage && nofl.valid == dark.valid,
              "wedge on WITHOUT evAbsFloor ⇒ no candidate can exist ⇒ identical to dark");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
