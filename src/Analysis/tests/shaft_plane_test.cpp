// Standalone tests for the face-on swing-plane conic (src/Analysis/shaft_plane.h
// — the port of tools/shaftlab/plane_probe.py fit_ellipse/head_path_plane).
// Pure std, no fixture.
//
//   cmake --build build/analyzer-tests --target shaft_plane_test
//   ctest --test-dir build/analyzer-tests -R shaft_plane_test --output-on-failure

#include "../shaft_plane.h"

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
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// n points on (A cos φ, B sin φ), rotated by rotDeg, scaled by s, translated.
// φ is swept over the full turn unless arcTurns says otherwise.
static void ellipse(std::vector<double> &x, std::vector<double> &y, int n,
                    double A, double B, double rotDeg,
                    double s = 1.0, double tx = 0.0, double ty = 0.0,
                    double arcTurns = 1.0)
{
    x.clear(); y.clear();
    const double c = std::cos(rotDeg * M_PI / 180.0), sn = std::sin(rotDeg * M_PI / 180.0);
    for (int k = 0; k < n; ++k) {
        const double phi = 2.0 * M_PI * arcTurns * k / n;
        const double u = A * std::cos(phi), v = B * std::sin(phi);
        x.push_back(s * (u * c - v * sn) + tx);
        y.push_back(s * (u * sn + v * c) + ty);
    }
}

// The node line the fit should report for a major axis lying along rotDeg:
// the bearing folded into (−90, +90].
static double foldNode(double rotDeg)
{
    double m = std::fmod(rotDeg + 90.0, 180.0);
    if (m < 0) m += 180.0;
    return m - 90.0;
}

// ── The DᵀD oracle (§1.1) ────────────────────────────────────────────────────
// A cyclic Jacobi eigendecomposition of the 6×6 normal matrix, i.e. the shortcut
// shaft_plane.h deliberately does NOT ship. Kept here so the claim that the two
// agree on well-posed input is asserted rather than asserted-in-a-comment.
static double iotaViaNormalMatrix(const std::vector<double> &x, const std::vector<double> &y)
{
    const int n = int(x.size());
    double mx = 0.0, my = 0.0;
    for (int i = 0; i < n; ++i) { mx += x[std::size_t(i)]; my += y[std::size_t(i)]; }
    mx /= n; my /= n;
    double s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double dx = x[std::size_t(i)] - mx, dy = y[std::size_t(i)] - my;
        s2 += dx * dx + dy * dy;
    }
    double sc = std::sqrt(s2 / n);
    if (sc == 0.0) sc = 1.0;

    std::vector<double> d(std::size_t(6) * n);
    for (int i = 0; i < n; ++i) {
        const double X = (x[std::size_t(i)] - mx) / sc, Y = (y[std::size_t(i)] - my) / sc;
        d[std::size_t(0) * n + i] = X * X;   d[std::size_t(1) * n + i] = X * Y;
        d[std::size_t(2) * n + i] = Y * Y;   d[std::size_t(3) * n + i] = X;
        d[std::size_t(4) * n + i] = Y;       d[std::size_t(5) * n + i] = 1.0;
    }
    double a[36], v[36];
    for (int p = 0; p < 6; ++p)
        for (int q = 0; q < 6; ++q) {
            double acc = 0.0;
            for (int i = 0; i < n; ++i) acc += d[std::size_t(p) * n + i] * d[std::size_t(q) * n + i];
            a[p * 6 + q] = acc;
        }
    for (int i = 0; i < 36; ++i) v[i] = 0.0;
    for (int i = 0; i < 6; ++i) v[i * 6 + i] = 1.0;

    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < 6; ++p) for (int q = p + 1; q < 6; ++q) off += a[p * 6 + q] * a[p * 6 + q];
        if (off < 1e-30) break;
        for (int p = 0; p < 5; ++p) {
            for (int q = p + 1; q < 6; ++q) {
                if (std::fabs(a[p * 6 + q]) < 1e-300) continue;
                const double theta = 0.5 * (a[q * 6 + q] - a[p * 6 + p]) / a[p * 6 + q];
                const double sgn   = (theta >= 0.0) ? 1.0 : -1.0;
                const double t     = sgn / (std::fabs(theta) + std::sqrt(1.0 + theta * theta));
                const double c     = 1.0 / std::sqrt(1.0 + t * t);
                const double s     = c * t;
                for (int k = 0; k < 6; ++k) {
                    const double akp = a[k * 6 + p], akq = a[k * 6 + q];
                    a[k * 6 + p] = c * akp - s * akq;
                    a[k * 6 + q] = s * akp + c * akq;
                }
                for (int k = 0; k < 6; ++k) {
                    const double apk = a[p * 6 + k], aqk = a[q * 6 + k];
                    a[p * 6 + k] = c * apk - s * aqk;
                    a[q * 6 + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 6; ++k) {
                    const double vkp = v[k * 6 + p], vkq = v[k * 6 + q];
                    v[k * 6 + p] = c * vkp - s * vkq;
                    v[k * 6 + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    int k = 0;
    for (int j = 1; j < 6; ++j) if (a[j * 6 + j] < a[k * 6 + k]) k = j;
    const double ca = v[0 * 6 + k], cb = v[1 * 6 + k], cc = v[2 * 6 + k];
    const double cd = v[3 * 6 + k], ce = v[4 * 6 + k], cf = v[5 * 6 + k];
    if (cb * cb - 4.0 * ca * cc >= 0.0) return -1.0;
    const plane_detail::Eig2 e = plane_detail::eigSym2(ca, 0.5 * cb, cc);
    const double det = 4.0 * ca * cc - cb * cb;
    const double cx = (-2.0 * cc * cd + cb * ce) / det, cy = (-2.0 * ca * ce + cb * cd) / det;
    const double konst = ca * cx * cx + cb * cx * cy + cc * cy * cy + cd * cx + ce * cy + cf;
    const double a0 = std::sqrt(std::fabs(-konst / e.ev[0])), a1 = std::sqrt(std::fabs(-konst / e.ev[1]));
    const double maj = std::max(a0, a1), min_ = std::min(a0, a1);
    return std::acos(std::clamp(min_ / maj, 0.0, 1.0)) * 180.0 / M_PI;
}

// A window's worth of shaft-vector samples on an ellipse, at 1 ms spacing from t0.
static void pushArc(std::vector<ShaftPlanePoint> &out, std::int64_t t0, int n,
                    double A, double B, double rotDeg)
{
    std::vector<double> x, y;
    ellipse(x, y, n, A, B, rotDeg);
    for (int i = 0; i < n; ++i)
        out.push_back({ t0 + std::int64_t(i) * 1000, x[std::size_t(i)], y[std::size_t(i)] });
}

int main()
{
    const double kCos40 = std::cos(40.0 * M_PI / 180.0);

    // ── Exact ι on a synthetic ellipse ──────────────────────────────────────
    std::printf("=== exact iota ===\n");
    {
        std::vector<double> x, y;

        ellipse(x, y, 60, 1.0, kCos40, 0.0);
        ConicFit f = fitConic(x.data(), y.data(), 60);
        check(f.ok, "axis-aligned ellipse fits");
        check(near(f.iotaDeg, 40.0, 1e-6), "iota = 40.000 exactly (B = cos 40)");
        check(near(f.nodeDeg, 0.0, 1e-6), "node = 0 for a major axis along +x");
        check(f.n == 60, "n reported = 60");
        // An EXACT conic has sigma_6 = 0, so the residual is pure rounding. This
        // is also the assertion that catches a coefficient vector left unnormalised.
        check(f.conicResid >= 0.0 && f.conicResid <= 1e-12, "conic residual ~0 on an exact ellipse");

        ellipse(x, y, 60, 1.0, kCos40, 35.0);
        f = fitConic(x.data(), y.data(), 60);
        check(near(f.iotaDeg, 40.0, 1e-9), "iota is rotation-invariant (35 deg)");
        check(near(f.nodeDeg, 35.0, 1e-6), "node follows the major axis (35 deg)");

        // Similarity invariance: isotropic normalisation makes translation and
        // uniform scale exactly free.
        ellipse(x, y, 60, 1.0, kCos40, 17.0);
        const ConicFit plain = fitConic(x.data(), y.data(), 60);
        ellipse(x, y, 60, 1.0, kCos40, 17.0, 137.0, 3000.0, -900.0);
        const ConicFit moved = fitConic(x.data(), y.data(), 60);
        check(near(plain.iotaDeg, moved.iotaDeg, 1e-9), "iota survives translate+scale by (3000,-900)x137");
        check(near(plain.nodeDeg, moved.nodeDeg, 1e-9), "node survives translate+scale");
        check(near(plain.conicResid, moved.conicResid, 1e-9), "conic residual survives translate+scale");

        // A circle. The axes tie EXACTLY (ratio 1.0), which is the one input that
        // actually reaches the argmax tie-break. iota is 0 and well defined; the
        // node line is NOT — equal axes mean there is no major axis, so the
        // reported bearing is decided by the sign of a ~1e-18 rounding term in b
        // and is arbitrary (numpy is equally arbitrary here). Pin what is true.
        ellipse(x, y, 60, 1.0, 1.0, 0.0);
        f = fitConic(x.data(), y.data(), 60);
        check(f.ok && near(f.iotaDeg, 0.0, 1e-6), "circle -> iota = 0");
        check(f.ratioMinorMajor == 1.0, "circle -> axis ratio is exactly 1 (the argmax tie really ties)");
        check(f.nodeDeg > -90.0 && f.nodeDeg <= 90.0,
              "circle -> node is undefined but stays inside the folded range");
    }

    // ── The node line's floored modulo ──────────────────────────────────────
    // std::fmod truncates where Python's % floors. The fold only diverges when
    // the raw bearing is below -90, which depends on which quadrant the major-axis
    // eigenvector happens to land in — so sweep the whole half-turn rather than
    // hand-picking a case. A truncated fmod fails several of these by 180.
    std::printf("=== node line, folded to (-90, +90] ===\n");
    {
        bool allOk = true;
        std::vector<double> x, y;
        for (int deg = 0; deg < 180; deg += 5) {
            ellipse(x, y, 60, 1.0, 0.55, double(deg));
            const ConicFit f = fitConic(x.data(), y.data(), 60);
            const double want = foldNode(double(deg));
            if (!f.ok || !near(f.nodeDeg, want, 1e-6)) {
                allOk = false;
                std::printf("      rot %3d deg: node %.6f, want %.6f\n", deg, f.nodeDeg, want);
            }
        }
        check(allOk, "node == folded major-axis bearing at every 5 deg over the half-turn");
    }

    // ── Rejections, all deterministic ───────────────────────────────────────
    std::printf("=== rejections ===\n");
    {
        std::vector<double> x, y;

        ellipse(x, y, 11, 1.0, kCos40, 0.0);
        ConicFit f = fitConic(x.data(), y.data(), 11);
        check(!f.ok && f.reject == ConicReject::TooFewSamples, "n = 11 -> TooFewSamples");

        ellipse(x, y, 12, 1.0, kCos40, 0.0);
        f = fitConic(x.data(), y.data(), 12);
        check(f.ok, "n = 12 is the boundary and DOES fit");

        // xy = 1: an exact hyperbola, discriminant +1.
        x.clear(); y.clear();
        for (int i = 0; i < 30; ++i) { const double t = 0.5 + 0.1 * i; x.push_back(t); y.push_back(1.0 / t); }
        f = fitConic(x.data(), y.data(), 30);
        check(!f.ok && f.reject == ConicReject::NotAnEllipse, "xy = 1 -> NotAnEllipse");

        // y = x^2: an exact parabola, discriminant 0. The gate is `>= 0`, so a
        // parabola must never fit. Whether it lands on NotAnEllipse or, when
        // rounding puts the discriminant a hair below zero, DegenerateAxes (M has
        // a zero eigenvalue) is not worth pinning — that it does not FIT is.
        x.clear(); y.clear();
        for (int i = 0; i < 30; ++i) { const double t = -1.5 + 0.1 * i; x.push_back(t); y.push_back(t * t); }
        f = fitConic(x.data(), y.data(), 30);
        check(!f.ok, "y = x^2 -> no fit (the discriminant gate is >= 0, not > 0)");

        ellipse(x, y, 30, 1.0, kCos40, 0.0);
        x[5] = std::nan("");
        f = fitConic(x.data(), y.data(), 30);
        check(!f.ok && f.reject == ConicReject::NonFinite, "a NaN sample -> NonFinite");
        check(!std::isnan(f.iotaDeg), "...and iota is not NaN");
    }

    // ── The conditioning floor: a needle is not a plane ─────────────────────
    // An arc too short or sparse to constrain the second axis still admits a
    // conic — an arbitrarily elongated one — and iota then runs toward 90.
    // Three corpus fits did exactly this (iota 81.7 / 82.9 / 89.0), producing
    // deltas near 50 degrees that no golf swing performs.
    std::printf("=== ill-conditioned fits are refused ===\n");
    {
        std::vector<double> x, y;

        // Axis ratio either side of the floor. kMinAxisRatio = 0.26 <=> iota 75.
        ellipse(x, y, 60, 1.0, 0.30, 0.0);          // ratio 0.30 -> iota 72.5
        ConicFit f = fitConic(x.data(), y.data(), 60);
        check(f.ok && f.ratioMinorMajor > kMinAxisRatio, "ratio 0.30 is above the floor and fits");

        ellipse(x, y, 60, 1.0, 0.20, 0.0);          // ratio 0.20 -> iota 78.5
        f = fitConic(x.data(), y.data(), 60);
        check(!f.ok && f.reject == ConicReject::IllConditioned, "ratio 0.20 -> IllConditioned");
        check(near(f.ratioMinorMajor, 0.20, 1e-6),
              "...and the refusal still CARRIES the ratio that caused it");

        // The corpus needle: an axis ratio of 0.017 is a straight line to 2%.
        ellipse(x, y, 60, 1.0, 0.017, 0.0);
        f = fitConic(x.data(), y.data(), 60);
        check(!f.ok && f.reject == ConicReject::IllConditioned,
              "the 2026-07-04 needle (ratio 0.017, iota 89) is refused");

        // WHY THE GATE IS ON THE RATIO AND NOT THE SPLIT-HALF. Both halves of a
        // needle collapse to the same needle, so they agree perfectly: the corpus
        // fit at iota 89.03 reported a split-half of 0.00, the best score there is.
        // Split-half is precision, never validity — assert that directly so nobody
        // reintroduces it as a conditioning check.
        ellipse(x, y, 60, 1.0, 0.017, 0.0);
        std::vector<double> ax, ay, bx, by;
        for (int i = 0; i < 60; ++i) {
            if (i % 2 == 0) { ax.push_back(x[std::size_t(i)]); ay.push_back(y[std::size_t(i)]); }
            else            { bx.push_back(x[std::size_t(i)]); by.push_back(y[std::size_t(i)]); }
        }
        // Fit the halves with the floor bypassed, exactly as the reference would.
        const ConicFit ha = fitConic(ax.data(), ay.data(), 30);
        const ConicFit hb = fitConic(bx.data(), by.data(), 30);
        check(ha.reject == ConicReject::IllConditioned && hb.reject == ConicReject::IllConditioned,
              "both halves of a needle are needles — they AGREE");
        check(std::fabs(ha.iotaDeg - hb.iotaDeg) < 0.5,
              "...so their split-half would read ~0: precision is not validity");
    }

    // ── The anisotropic-normalisation regression pin ────────────────────────
    // You cannot call the wrong implementation, so pin a property the wrong one
    // cannot have. Per-axis-sigma whitening maps ANY axis-aligned ellipse to a
    // circle, so it is invariant under per-axis scaling: E1 and E2 below have
    // identical whitened point clouds, and an anisotropic fit answers 0 for both.
    // The correct fit answers 40 and 0. That gap IS the documented 40 deg error.
    std::printf("=== anisotropic normalisation stays dead ===\n");
    {
        std::vector<double> x1, y1, x2, y2;
        ellipse(x1, y1, 60, 1.0, kCos40, 0.0);              // true iota = 40
        ellipse(x2, y2, 60, 1.0, 1.0, 0.0);                 // the same points, y scaled by 1/cos40
        const ConicFit e1 = fitConic(x1.data(), y1.data(), 60);
        const ConicFit e2 = fitConic(x2.data(), y2.data(), 60);
        check(near(e1.iotaDeg, 40.0, 1e-6), "E1 (axis ratio cos 40) -> iota 40");
        check(near(e2.iotaDeg,  0.0, 1e-6), "E2 (E1 with y scaled by 1/cos 40) -> iota 0");
        check(std::fabs(e1.iotaDeg - e2.iotaDeg) > 39.0,
              "E1 and E2 differ by ~40 deg — the anisotropic failure mode is dead");

        // Two more an anisotropic version also fails: whitening a ROTATED ellipse
        // changes the ratio, and it cannot reproduce a residual under uniform scale.
        std::vector<double> xr, yr;
        ellipse(xr, yr, 60, 1.0, kCos40, 35.0);
        check(near(fitConic(xr.data(), yr.data(), 60).iotaDeg, e1.iotaDeg, 1e-9),
              "rotating E1 by 35 deg leaves iota unchanged");
        ellipse(xr, yr, 60, 1.0, kCos40, 0.0, 137.0);
        const ConicFit scaled = fitConic(xr.data(), yr.data(), 60);
        check(near(scaled.iotaDeg, e1.iotaDeg, 1e-9) && near(scaled.conicResid, e1.conicResid, 1e-9),
              "scaling E1 by 137 leaves iota AND the residual unchanged");
    }

    // ── The DtD oracle agrees on an ill-conditioned arc ─────────────────────
    std::printf("=== normal-matrix cross-check ===\n");
    {
        // A 90 deg ARC — a quarter turn, which is what makes the SVD ill-conditioned
        // (sigma_6/sigma_1 collapses on a partial arc). The axis ratio is 0.4, kept
        // clear of kMinAxisRatio on purpose: this case is about numerical
        // conditioning of the solver, not about the conditioning FLOOR, and letting
        // the two overlap would leave the oracle asserting nothing.
        std::vector<double> x, y;
        ellipse(x, y, 40, 1.0, 0.4, 20.0, 1.0, 0.0, 0.0, 0.25);
        const ConicFit f = fitConic(x.data(), y.data(), 40);
        const double oracle = iotaViaNormalMatrix(x, y);
        check(f.ok, "the 90 deg thin arc still fits");
        check(oracle >= 0.0 && near(f.iotaDeg, oracle, 1e-9),
              "one-sided Jacobi agrees with the DtD eigendecomposition to 1e-9 deg");
    }

    // ── Windows, split-half, and the two-channel policy ─────────────────────
    std::printf("=== windows and channels ===\n");
    const std::int64_t kTakeaway = 1'000'000, kTop = 2'000'000, kImpact = 2'500'000;
    {
        ShaftPlaneInput in;
        in.takeawayUs = kTakeaway; in.topUs = kTop; in.impactUs = kImpact;
        in.haveWindows = true;
        pushArc(in.measured, kTakeaway + 1000, 60, 1.0, kCos40, 0.0);   // back:  iota 40
        pushArc(in.measured, kTop      + 1000, 40, 1.0, 0.5,    10.0);  // down:  iota 60

        const ShaftPlaneResult r = fitShaftPlane(in);
        check(r.valid && r.channel == PlaneChannel::Measured, "measured fits both windows -> channel = measured");
        check(r.measured.back.fit.n == 60 && r.measured.down.fit.n == 40, "each window sees only its own samples");
        check(near(r.iotaBackDeg, 40.0, 1e-6), "iota_back = 40");
        check(near(r.iotaDownDeg, 60.0, 1e-6), "iota_down = 60");
        check(near(r.deltaDeg, -20.0, 1e-6), "delta = back - down = -20 (the club FLATTENED)");
        check(r.measured.back.splitHalfDeg >= 0.0, "back window (n = 60) gets a split-half");
        check(r.measured.down.splitHalfDeg >= 0.0, "down window (n = 40) gets a split-half");
        check(!r.synth.fitted && r.synth.back.fit.reject == ConicReject::TooFewSamples,
              "an empty synth channel is recorded as unfitted, not as absent");
    }
    {
        // The window bounds are INCLUSIVE at both ends, so the sample exactly at
        // top belongs to BOTH windows. Reference behaviour — changing it shifts
        // every sample count in the golden file.
        ShaftPlaneInput in;
        in.takeawayUs = kTakeaway; in.topUs = kTop; in.impactUs = kImpact;
        in.haveWindows = true;
        pushArc(in.measured, kTakeaway + 1000, 60, 1.0, kCos40, 0.0);
        pushArc(in.measured, kTop      + 1000, 40, 1.0, 0.5,    10.0);
        in.measured.push_back({ kTop, 0.25, -0.25 });
        const ShaftPlaneResult r = fitShaftPlane(in);
        check(r.measured.back.fit.n == 61 && r.measured.down.fit.n == 41,
              "a sample exactly at top is counted in BOTH windows");
    }
    {
        // Split-half needs >= 24 so each half still clears the 12-sample gate.
        ShaftPlaneInput in;
        in.takeawayUs = kTakeaway; in.topUs = kTop; in.impactUs = kImpact;
        in.haveWindows = true;
        pushArc(in.measured, kTakeaway + 1000, 24, 1.0, kCos40, 0.0);
        pushArc(in.measured, kTop      + 1000, 23, 1.0, 0.5,    10.0);
        const ShaftPlaneResult r = fitShaftPlane(in);
        check(r.measured.back.splitHalfDeg >= 0.0, "n = 24 -> split-half computed (each half gets exactly 12)");
        check(r.measured.down.splitHalfDeg == -1.0, "n = 23 -> split-half is -1, never 0");
    }
    {
        // THE SPLIT-HALF TRAP. Odd and even synth samples lie on the same Hermite,
        // so a refit there measures interpolation smoothness and would fake
        // excellent quality. The synth channel must never carry one.
        ShaftPlaneInput in;
        in.takeawayUs = kTakeaway; in.topUs = kTop; in.impactUs = kImpact;
        in.haveWindows = true;
        pushArc(in.synth, kTakeaway + 1000, 200, 1.0, kCos40, 0.0);
        pushArc(in.synth, kTop      + 1000, 200, 1.0, 0.5,    10.0);
        in.anchors = { { kTakeaway + 5000, 0.9f }, { kTop - 5000, 0.4f },
                       { kTop + 5000, 0.7f }, { kImpact - 5000, 0.8f } };
        const ShaftPlaneResult r = fitShaftPlane(in);
        check(r.valid && r.channel == PlaneChannel::Synth, "measured absent -> the synth fit is the emitted value");
        check(r.synth.back.splitHalfDeg == -1.0 && r.synth.down.splitHalfDeg == -1.0,
              "the synth channel NEVER carries a split-half, however dense it is");
        check(r.synth.back.anchors == 2 && r.synth.down.anchors == 2, "anchors counted per window");
        check(near(double(r.synth.anchorConfMin), 0.4, 1e-6), "anchorConfMin = the weakest anchor over both windows");
        check(r.measured.back.splitHalfDeg == -1.0, "an unfitted measured channel carries no split-half either");
    }
    {
        // Fallback must stay VISIBLE: the synth-tagged emission carries which
        // measured window failed and how.
        ShaftPlaneInput in;
        in.takeawayUs = kTakeaway; in.topUs = kTop; in.impactUs = kImpact;
        in.haveWindows = true;
        pushArc(in.measured, kTakeaway + 1000, 11, 1.0, kCos40, 0.0);   // too short
        pushArc(in.measured, kTop      + 1000, 40, 1.0, 0.5,    10.0);
        pushArc(in.synth,    kTakeaway + 1000, 200, 1.0, kCos40, 0.0);
        pushArc(in.synth,    kTop      + 1000, 200, 1.0, 0.5,    10.0);
        const ShaftPlaneResult r = fitShaftPlane(in);
        check(r.valid && r.channel == PlaneChannel::Synth, "measured fails one window -> synth is emitted");
        check(r.measured.back.fit.reject == ConicReject::TooFewSamples,
              "...and the failing measured window names its reason");
        check(r.measured.down.fit.ok, "...while the measured window that DID fit is still recorded");
    }
    {
        // Both channels fit: the pair is recorded even though only one is emitted.
        // That record IS the mirror experiment (brief section 8.3).
        ShaftPlaneInput in;
        in.takeawayUs = kTakeaway; in.topUs = kTop; in.impactUs = kImpact;
        in.haveWindows = true;
        pushArc(in.measured, kTakeaway + 1000, 60, 1.0, kCos40, 0.0);
        pushArc(in.measured, kTop      + 1000, 40, 1.0, 0.5,    10.0);
        pushArc(in.synth,    kTakeaway + 1000, 200, 1.0, 0.6,   0.0);
        pushArc(in.synth,    kTop      + 1000, 200, 1.0, 0.4,   10.0);
        const ShaftPlaneResult r = fitShaftPlane(in);
        check(r.channel == PlaneChannel::Measured, "measured is the headline when both fit");
        check(r.measured.fitted && r.synth.fitted, "...and BOTH channels are recorded");
        check(std::fabs(r.measured.deltaDeg - r.synth.deltaDeg) > 1e-6,
              "the two channels keep their own deltas — never merged");
        check(near(r.deltaDeg, r.measured.deltaDeg, 1e-12), "the headline delta mirrors the SELECTED channel");
    }
    {
        ShaftPlaneInput in;
        in.takeawayUs = kTakeaway; in.topUs = kTop; in.impactUs = kImpact;
        in.haveWindows = true;
        pushArc(in.measured, kTakeaway + 1000, 11, 1.0, kCos40, 0.0);
        const ShaftPlaneResult r = fitShaftPlane(in);
        check(!r.valid && r.channel == PlaneChannel::None, "neither channel fits -> no emission");
        check(r.deltaDeg == 0.0 && r.iotaBackDeg == 0.0, "...and the headline stays zero, not stale");
    }
    {
        // No ladder, no computation, and every window says so.
        ShaftPlaneInput in;
        pushArc(in.measured, kTakeaway + 1000, 60, 1.0, kCos40, 0.0);
        const ShaftPlaneResult r = fitShaftPlane(in);   // haveWindows == false
        check(!r.valid, "no ladder -> no emission");
        check(r.measured.back.fit.reject == ConicReject::NoWindow
              && r.measured.down.fit.reject == ConicReject::NoWindow
              && r.synth.back.fit.reject == ConicReject::NoWindow,
              "...and every window reports NoWindow, not a fit failure");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail;
}
