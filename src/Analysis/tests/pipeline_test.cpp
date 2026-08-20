// Standalone pipeline test: synthetic FusedStreams -> PhaseSegmenter -> MetricExtractor.
// Pure (no SwingWindow). Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure

#include "../phase_segmenter.h"
#include "../metric_extractor.h"

#include <QQuaternion>
#include <QVector3D>
#include <cmath>
#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;
static QVector3D Z(0, 0, 1);   // flexion axis (hardware-locked: FE about Z)

// Synthetic lead-wrist flexion profile (deg) over a 1.5 s swing: settle, backswing set
// to +50° at the top (t=0.8 s), through to −10° at impact (t=1.2 s), then relax.
static double feProfile(double t)
{
    if (t < 0.2) return 0.0;
    if (t < 0.8) return 50.0 * (t - 0.2) / 0.6;
    if (t < 1.2) return 50.0 + (-60.0) * (t - 0.8) / 0.4;     // 50 -> -10
    return -10.0 + 10.0 * std::min((t - 1.2) / 0.3, 1.0);     // -10 -> 0
}

static const MetricSeries *find(const std::vector<MetricSeries> &v, const char *key)
{
    for (const auto &m : v) if (m.key == QString::fromLatin1(key)) return &m;
    return nullptr;
}

// Fill gyroDps/accelG consistently with the quaternion track: body-frame gyro
// by finite difference (the v2 segmenter runs on the measured inertials), and
// gravity rotated into the body frame (unit magnitude -> "still" accel).
static void fillInertials(SegmentStream &s, const std::vector<int64_t> &grid)
{
    const size_t n = s.qAnat.size();
    s.gyroDps.assign(n, QVector3D());
    s.accelG.assign(n, QVector3D());
    for (size_t i = 0; i < n; ++i) {
        s.accelG[i] = s.qAnat[i].conjugated().rotatedVector(QVector3D(0, 0, 1));
        if (i + 1 >= n) { if (i) s.gyroDps[i] = s.gyroDps[i - 1]; continue; }
        const QQuaternion dq = (s.qAnat[i].conjugated() * s.qAnat[i + 1]).normalized();
        const QVector3D v(dq.x(), dq.y(), dq.z());
        const double ang = 2.0 * std::atan2(double(v.length()), double(std::abs(dq.scalar())));
        const double dt  = double(grid[i + 1] - grid[i]) * 1e-6;
        if (v.length() > 1e-9 && dt > 0)
            s.gyroDps[i] = v.normalized()
                         * float(ang / dt * 180.0 / M_PI * (dq.scalar() < 0 ? -1.0 : 1.0));
    }
}
static double phaseVal(const MetricSeries &m, Phase p)
{
    for (const auto &s : m.phaseSamples) if (s.phase == p) return s.value;
    return std::nan("");
}
#define CHECK_NEAR(label, got, want, tol)                                              \
    do { double g = (got), w = (want); bool ok = std::abs(g - w) <= (tol);             \
         std::printf("  [%s] %-34s got %8.2f  want %8.2f (tol %.1f)\n",                \
                     ok ? "PASS" : "FAIL", label, g, w, double(tol));                  \
         if (!ok) ++g_fail; } while (0)

int main()
{
    // 200 Hz grid over 1.5 s; forearm + upper arm static (identity), hand flexes.
    FusedStreams fs;
    const int64_t dt = 5000;
    for (int64_t t = 0; t <= 1500000; t += dt) fs.timeGrid.push_back(t);
    const int N = int(fs.timeGrid.size());

    SegmentStream fore, hand, upper;
    fore.role = SegmentRole::LeadForearm;
    hand.role = SegmentRole::LeadHand;
    upper.role = SegmentRole::LeadUpperArm;
    for (int i = 0; i < N; ++i) {
        const double ts = fs.timeGrid[i] * 1e-6;
        fore.qAnat.push_back(QQuaternion());                              // identity
        hand.qAnat.push_back(QQuaternion::fromAxisAndAngle(Z, float(feProfile(ts))));
        upper.qAnat.push_back(QQuaternion());                            // identity
    }
    fillInertials(fore,  fs.timeGrid);
    fillInertials(hand,  fs.timeGrid);
    fillInertials(upper, fs.timeGrid);
    fs.segments = { fore, hand, upper };

    const int64_t impactUs = 1200000;

    std::printf("=== PhaseSegmenter ===\n");
    const Segmentation seg = PhaseSegmenter::segment(fs, impactUs);
    const std::vector<PhaseEvent> &phases = seg.events;
    double addrT = -1, topT = -1, impT = -1;
    for (auto &e : phases) {
        if (e.phase == Phase::Address) addrT = e.t_us * 1e-6;
        if (e.phase == Phase::Top)     topT  = e.t_us * 1e-6;
        if (e.phase == Phase::Impact)  impT  = e.t_us * 1e-6;
    }
    CHECK_NEAR("Address (s)", addrT, 0.20, 0.10);     // motion onset ~0.2 s
    CHECK_NEAR("Top (s)",     topT,  0.80, 0.05);     // backswing apex
    CHECK_NEAR("Impact (s)",  impT,  1.20, 0.001);    // hard anchor

    std::printf("\n=== MetricExtractor (handedness=2 -> leftArm=false, no sign flip) ===\n");
    auto metrics = MetricExtractor::extract(fs, phases, /*handedness=*/2);
    std::printf("  emitted %zu series\n", metrics.size());
    const MetricSeries *fe = find(metrics, "leadWristFlexExt");
    if (!fe) { std::printf("  [FAIL] leadWristFlexExt missing\n"); ++g_fail; }
    else {
        CHECK_NEAR("FE @ Address", phaseVal(*fe, Phase::Address),  0.0, 1.5);
        CHECK_NEAR("FE @ Top",     phaseVal(*fe, Phase::Top),     50.0, 1.5);
        CHECK_NEAR("FE @ Impact",  phaseVal(*fe, Phase::Impact),  -10.0, 1.5);
        CHECK_NEAR("FE series len", double(fe->value.size()), double(N), 0.5);
    }
    const MetricSeries *rud = find(metrics, "leadWristRadUln");
    if (rud) CHECK_NEAR("RUD @ Top (no deviation)", phaseVal(*rud, Phase::Top), 0.0, 1.0);
    const MetricSeries *pron = find(metrics, "forearmPronation");
    if (pron) CHECK_NEAR("pronation (static)", phaseVal(*pron, Phase::Impact), 0.0, 1.0);

    std::printf("\n=== degradation: no upper-arm IMU -> pronation/elbow suppressed ===\n");
    {
        FusedStreams fs2 = fs;
        fs2.segments = { fore, hand };          // drop the upper arm
        auto m2 = MetricExtractor::extract(fs2, phases, 2);
        bool hasWrist = find(m2, "leadWristFlexExt") && find(m2, "leadWristRadUln");
        bool noPron   = !find(m2, "forearmPronation") && !find(m2, "leadArmFlexion");
        std::printf("  [%s] wrist FE/RUD present\n", hasWrist ? "PASS" : "FAIL");
        std::printf("  [%s] pronation/elbow suppressed\n", noPron ? "PASS" : "FAIL");
        if (!hasWrist) ++g_fail;
        if (!noPron)   ++g_fail;
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES PRESENT" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
