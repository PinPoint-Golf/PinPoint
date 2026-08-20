// Standalone truth-table test for impact_detector.h (shot detection P1).
// Run via CTest (src/IMU/tests/CMakeLists.txt):
//   cmake -S src/IMU/tests -B build/imu-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/imu-tests && ctest --test-dir build/imu-tests --output-on-failure
//
// The truth table IS P1's real value: a sharp jerk with a preceding gyro ramp
// at a plausible orientation fires exactly once; mat/ground taps, waggles and
// slow swells never fire; the refractory collapses double-hits; est_t is the
// back-dated peak time. Windows are ms-based, so 100 Hz and 200 Hz traces
// must behave identically.

#include "../impact_detector.h"

#include <cmath>
#include <cstdio>

using pinpoint::ImpactDetector;
using pinpoint::ImpactDetectorConfig;
using pinpoint::ImpactResult;
using pinpoint::ImpactSample;

static int g_fail = 0;

#define CHECK(label, cond)                                                    \
    do {                                                                      \
        const bool ok = (cond);                                               \
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);              \
        if (!ok) ++g_fail;                                                    \
    } while (0)

#define CHECK_EQ_I64(label, got, want)                                        \
    do {                                                                      \
        const long long g = (long long)(got), w = (long long)(want);          \
        const bool ok = (g == w);                                             \
        std::printf("  [%s] %-40s got %lld  want %lld\n",                     \
                    ok ? "PASS" : "FAIL", label, g, w);                       \
        if (!ok) ++g_fail;                                                    \
    } while (0)

// Synthetic-trace driver: pushes samples at a fixed rate, counts impacts.
struct Sim {
    ImpactDetector det;
    int64_t t;
    int64_t dtUs;
    int impacts = 0;
    ImpactResult last;

    explicit Sim(double rateHz, const ImpactDetectorConfig &cfg = {})
        : det(cfg), t(1'000'000), dtUs(std::llround(1e6 / rateHz)) {}

    ImpactResult push(float accel, float gyro, float vertical = 0.9f)
    {
        const ImpactResult r = det.push({accel, gyro, vertical, t});
        t += dtUs;
        if (r.impact) { ++impacts; last = r; }
        return r;
    }

    // Address: ~1 g, near-zero rotation.
    void quiet(double ms)
    {
        for (const int64_t end = t + (int64_t)(ms * 1000); t < end; )
            push(1.0f, 5.0f);
    }

    // Linear gyro ramp into transition (the swing-energy signature).
    void downswing(double ms, float gyroPeak, float accelEnd = 2.5f)
    {
        const int n = (int)(ms * 1000 / dtUs);
        for (int i = 0; i < n; ++i) {
            const float f = (float)(i + 1) / (float)n;
            push(1.0f + (accelEnd - 1.0f) * f, gyroPeak * f);
        }
    }

    // One sharp spike + two lookahead samples so the local max resolves.
    // Returns the peak sample's timestamp.
    int64_t strike(float accelPeak = 14.0f, float gyro = 800.0f,
                   float vertical = 0.8f)
    {
        const int64_t peakT = t;
        push(accelPeak, gyro, vertical);
        push(2.0f, 400.0f, vertical);
        push(1.5f, 200.0f, vertical);
        return peakT;
    }
};

int main()
{
    std::printf("=== impact_detector.h truth table ===\n\n");

    std::printf("-- A. real strike: jerk + gyro ramp + orientation -> exactly one Impact --\n");
    {
        ImpactDetectorConfig cfg;
        cfg.bleLatencyUs = 15'000;
        Sim s(100.0, cfg);
        s.quiet(1000);
        s.downswing(350, 800.0f);
        const int64_t peakT = s.strike();
        s.quiet(500);
        CHECK("strike fires exactly once", s.impacts == 1);
        CHECK_EQ_I64("est_t == peak_t - kImuBleLatencyUs",
                     s.last.est_t_us, peakT - cfg.bleLatencyUs);
        CHECK("confidence in (0,1]",
              s.last.confidence > 0.0f && s.last.confidence <= 1.0f);
    }

    std::printf("\n-- B. mat/ground tap: accel spike, flat gyro -> None --\n");
    {
        Sim s(100.0);
        s.quiet(1000);
        s.strike(10.0f, /*gyro=*/8.0f);   // hard knock, no swing rotation
        s.quiet(500);
        CHECK("tap rejected", s.impacts == 0);
    }

    std::printf("\n-- C. waggle/address: low energy -> None --\n");
    {
        Sim s(100.0);
        s.quiet(500);
        for (int i = 0; i < 200; ++i) {   // 2 s oscillation
            const float ph = (float)i * 0.4f;
            s.push(1.0f + 0.8f * std::fabs(std::sin(ph)),
                   100.0f * std::fabs(std::cos(ph)));
        }
        s.quiet(500);
        CHECK("waggle rejected", s.impacts == 0);
    }

    std::printf("\n-- D. slow swell: crests above threshold without a jerk -> None --\n");
    {
        Sim s(100.0);
        s.quiet(1000);
        // Sustained rotation so ONLY the jerk gate separates this from a strike.
        for (int i = 0; i < 100; ++i)     // 1 s rise to 6 g
            s.push(1.0f + 5.0f * (float)(i + 1) / 100.0f, 400.0f);
        for (int i = 0; i < 100; ++i)     // 1 s decay
            s.push(6.0f - 5.0f * (float)(i + 1) / 100.0f, 400.0f);
        s.quiet(500);
        CHECK("slow swell rejected", s.impacts == 0);
    }

    std::printf("\n-- E. two impacts within refractory -> one --\n");
    {
        Sim s(100.0);
        s.quiet(1000);
        s.downswing(350, 800.0f);
        s.strike();
        // Second hit ~100 ms later (refractory is 200 ms), rotation still high.
        for (int i = 0; i < 7; ++i) s.push(1.5f, 500.0f, 0.8f);
        s.strike();
        s.quiet(500);
        CHECK("double-hit collapsed to one", s.impacts == 1);
    }

    std::printf("\n-- F. club-orientation gate --\n");
    {
        ImpactDetectorConfig cfg;
        Sim s(100.0, cfg);
        s.quiet(1000);
        s.downswing(350, 800.0f);
        s.strike(14.0f, 800.0f, /*vertical=*/0.05f);   // shaft horizontal
        s.quiet(500);
        CHECK("implausible orientation rejected", s.impacts == 0);

        cfg.orientationGate = false;
        Sim s2(100.0, cfg);
        s2.quiet(1000);
        s2.downswing(350, 800.0f);
        s2.strike(14.0f, 800.0f, 0.05f);
        s2.quiet(500);
        CHECK("gate disabled -> fires", s2.impacts == 1);
    }

    std::printf("\n-- G. ms-based windows: 100 Hz and 200 Hz behave identically --\n");
    {
        for (const double rate : {100.0, 200.0}) {
            Sim s(rate);
            s.quiet(1000);
            s.downswing(350, 800.0f);
            s.strike();
            s.quiet(500);
            char label[64];
            std::snprintf(label, sizeof label, "strike fires once at %.0f Hz", rate);
            CHECK(label, s.impacts == 1);
        }
    }

    std::printf("\n-- H. startup guard: no swing-energy history -> None --\n");
    {
        Sim s(100.0);
        s.quiet(50);          // far less than half the 400 ms gyro window
        s.strike();
        CHECK("immediate post-connect spike rejected", s.impacts == 0);
    }

    std::printf("\n-- I. sensitivity scale --\n");
    {
        // A weaker swing (350 dps peak) sits between the High (0.7x) and
        // Low (1.5x) gyro thresholds of 210 / 450 dps.
        ImpactDetectorConfig low;
        low.thresholdScale = 1.5f;
        Sim s(100.0, low);
        s.quiet(1000);
        s.downswing(350, 350.0f);
        s.strike(14.0f, 350.0f);
        s.quiet(500);
        CHECK("Low sensitivity rejects weak swing", s.impacts == 0);

        ImpactDetectorConfig high;
        high.thresholdScale = 0.7f;
        Sim s2(100.0, high);
        s2.quiet(1000);
        s2.downswing(350, 350.0f);
        s2.strike(14.0f, 350.0f);
        s2.quiet(500);
        CHECK("High sensitivity accepts weak swing", s2.impacts == 1);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "OK",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
