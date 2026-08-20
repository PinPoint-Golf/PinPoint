// Standalone sanity test for the shaft v3.0-r1 evidence engines
// (src/Analysis/shaft_tracker_math — E2 ridge_sweep + E1 frame_band_match).
// Synthetic ridges/bands with known geometry; checks the peak θ, degenerate
// returns, and determinism. Full NUMERIC parity vs the Python exemplar is the
// separate shaft_parity_test (Phase 5, against per-frame dumps).
//
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests --target shaft_evidence_test
//   ctest --test-dir build/analyzer-tests -R shaft_evidence --output-on-failure

#include "../shaft_tracker_math.h"

#include <opencv2/imgproc.hpp>

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

static constexpr double kPi = 3.14159265358979323846;

// grid of 360 rays, 1°/step (as club_track_v3 TR)
static std::vector<float> thetaGrid()
{
    std::vector<float> t(360);
    for (int i = 0; i < 360; ++i) t[i] = float(i * kPi / 180.0);
    return t;
}

static int argmaxIdx(const std::vector<float> &v)
{
    int bi = 0;
    for (int i = 1; i < int(v.size()); ++i) if (v[i] > v[bi]) bi = i;
    return bi;
}

static double wrapDeg(double a) { a = std::fmod(a + 180.0, 360.0); if (a < 0) a += 360.0; return a - 180.0; }

// np.percentile with linear interpolation — mirrors the private helper in
// shaft_track_assembly.cpp (evAbsFloor is calibrated against exactly this).
static float percentile(std::vector<float> v, double p)
{
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * (v.size() - 1);
    const int lo = int(std::floor(idx)), hi = int(std::ceil(idx));
    return float(v[lo] + (v[hi] - v[lo]) * (idx - lo));
}

int main()
{
    const auto TR = thetaGrid();

    // ── E2 ridge sweep: bright line over dark background ─────────────────────
    std::printf("=== E2 ridgeSweep (bright line) ===\n");
    {
        const int W = 640, H = 480;
        cv::Mat g8(H, W, CV_8UC1, cv::Scalar(40));
        const double gx = 300, gy = 260, thTrue = 35.0 * kPi / 180.0;
        const cv::Point p0{int(gx), int(gy)};
        const cv::Point p1{int(gx + 220 * std::cos(thTrue)), int(gy + 220 * std::sin(thTrue))};
        cv::line(g8, p0, p1, cv::Scalar(230), 3, cv::LINE_8);
        cv::Mat g32; g8.convertTo(g32, CV_32F);

        RidgeConfig cfg;
        const RidgeResult r = ridgeSweep(g32, gx, gy, TR, cfg, /*brightOnly=*/false);
        const int bi = argmaxIdx(r.score);
        check(std::abs(wrapDeg(bi - 35.0)) < 3.0, "peak θ within 3° of 35°");
        check(r.score[bi] > 0.f, "peak score positive");
        // one-sided: the opposite direction (θ+180) must not win
        check(r.score[(bi + 180) % 360] < r.score[bi], "reverse ray weaker");

        const RidgeResult r2 = ridgeSweep(g32, gx, gy, TR, cfg, false);
        bool same = true;
        for (int i = 0; i < 360; ++i) if (r.score[i] != r2.score[i]) { same = false; break; }
        check(same, "deterministic (byte-identical rerun)");
    }

    // ── E2 brightOnly path (diff image) ──────────────────────────────────────
    std::printf("=== E2 ridgeSweep (brightOnly) ===\n");
    {
        const int W = 640, H = 480;
        cv::Mat g8(H, W, CV_8UC1, cv::Scalar(0));
        const double gx = 320, gy = 240, thTrue = 200.0 * kPi / 180.0;
        cv::line(g8, cv::Point(int(gx), int(gy)),
                 cv::Point(int(gx + 200 * std::cos(thTrue)), int(gy + 200 * std::sin(thTrue))),
                 cv::Scalar(180), 3, cv::LINE_8);
        cv::Mat g32; g8.convertTo(g32, CV_32F);
        const RidgeResult r = ridgeSweep(g32, gx, gy, TR, RidgeConfig{}, /*brightOnly=*/true);
        check(std::abs(wrapDeg(argmaxIdx(r.score) - 200.0)) < 3.0, "brightOnly peak θ within 3° of 200°");
    }

    // ── E1 band match: degenerate returns ────────────────────────────────────
    std::printf("=== E1 frameBandMatch (degenerate) ===\n");
    {
        cv::Mat blank(480, 640, CV_8UC1, cv::Scalar(100));
        BandMatchConfig cfg;
        check(!frameBandMatch(blank, 320, 240, 297, {}, cfg).ok, "empty bands ⇒ !ok");
        check(!frameBandMatch(blank, 320, 240, 297, {0, 54, 104, 300}, cfg).ok, "no blobs ⇒ !ok");
    }

    // ── E1 band match: synthetic 4-band lock ─────────────────────────────────
    std::printf("=== E1 frameBandMatch (synthetic lock) ===\n");
    {
        const int W = 640, H = 480;
        cv::Mat g8(H, W, CV_8UC1, cv::Scalar(100));   // grey steel (dark gaps)
        const double gx = 200, gy = 300, s = 0.35, thTrue = 30.0 * kPi / 180.0;
        const std::vector<double> bands = {0, 54, 104, 300};
        for (double r : bands) {
            const int bx = int(gx + s * r * std::cos(thTrue));
            const int by = int(gy + s * r * std::sin(thTrue));
            cv::circle(g8, cv::Point(bx, by), 3, cv::Scalar(255), -1);   // saturated band
        }
        const double rmax = 0.62 * H;
        const BandMatch bm = frameBandMatch(g8, gx, gy, rmax, bands, BandMatchConfig{});
        check(bm.ok, "locks on the 4-band pattern");
        check(bm.n >= 4, "n ≥ 4 matched");
        if (bm.ok) check(std::abs(wrapDeg(bm.thetaDeg - 30.0)) < 4.0, "θ within 4° of 30°");
    }

    // ── S1 evidence honesty: raw-unit pins (shaft_wedge_p6_impl.md "Session 1
    //    spec"). These calibrate the UNITS evAbsFloor/raySupportMin are read in —
    //    raw (pre-normalisation) ridgeSweep score/support, not normScores()'s
    //    [0,1] rescale. ═══════════════════════════════════════════════════════
    std::printf("=== S1 evidence honesty: raw-unit pins ===\n");
    {
        // 1) Noise-only frame: no line anywhere ⇒ raw p97 stays near the noise
        // floor, well under any credible evAbsFloor setting (doc sweep grid
        // starts at 10).
        const int W = 512, H = 512;
        cv::Mat noise8u(H, W, CV_8UC1);
        cv::RNG rng(12345);                       // deterministic seed
        rng.fill(noise8u, cv::RNG::NORMAL, 128, 5);
        cv::Mat g32; noise8u.convertTo(g32, CV_32F);
        const double gx = W / 2.0, gy = H / 2.0;   // grip at centre
        const RidgeResult r = ridgeSweep(g32, gx, gy, TR, RidgeConfig{}, /*brightOnly=*/false);
        check(percentile(r.score, 97.0) < 10.f, "noise-only raw p97 < 10 (evAbsFloor units)");
    }
    {
        // 2) Painted 3px bright line, contrast +60, length 200px at θ=40°: a
        // frame that genuinely contains a shaft must clear a generous floor with
        // support to match — the RAY tier's SUP[i][θdp] ≥ raySupportMin gate.
        const int W = 512, H = 512;
        cv::Mat g8(H, W, CV_8UC1, cv::Scalar(128));
        const double gx = 200, gy = 200, thTrue = 40.0 * kPi / 180.0;
        const cv::Point p0{int(gx), int(gy)};
        const cv::Point p1{int(gx + 200 * std::cos(thTrue)), int(gy + 200 * std::sin(thTrue))};
        cv::line(g8, p0, p1, cv::Scalar(128 + 60), 3, cv::LINE_8);
        cv::Mat g32; g8.convertTo(g32, CV_32F);
        const RidgeResult r = ridgeSweep(g32, gx, gy, TR, RidgeConfig{}, /*brightOnly=*/false);
        check(r.score[40] > 60.f, "painted +60 line: raw score at 40° > 60");
        check(r.support[40] > 0.5f, "painted +60 line: support at 40° > 0.5");
    }
    {
        // 3) Weak line, contrast +25, same geometry: genuinely-dim evidence (an
        // address/backswing frame lit poorly, not blur) must still clear the
        // chosen default floor (20, per the doc sweep grid's low end) — the S1
        // floor must not punish real-but-faint shafts.
        const int W = 512, H = 512;
        cv::Mat g8(H, W, CV_8UC1, cv::Scalar(128));
        const double gx = 200, gy = 200, thTrue = 40.0 * kPi / 180.0;
        const cv::Point p0{int(gx), int(gy)};
        const cv::Point p1{int(gx + 200 * std::cos(thTrue)), int(gy + 200 * std::sin(thTrue))};
        cv::line(g8, p0, p1, cv::Scalar(128 + 25), 3, cv::LINE_8);
        cv::Mat g32; g8.convertTo(g32, CV_32F);
        const RidgeResult r = ridgeSweep(g32, gx, gy, TR, RidgeConfig{}, /*brightOnly=*/false);
        check(r.score[40] >= 20.f, "weak +25 line: raw score at 40° clears the floor-20 default");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
