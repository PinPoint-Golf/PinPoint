// Standalone truth-table test for onset_detector.h (shot detection P2).
// Run via CTest (src/Audio/tests/CMakeLists.txt):
//   cmake -S src/Audio/tests -B build/audio-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/audio-tests && ctest --test-dir build/audio-tests --output-on-failure
//
// An impact "click" (broadband transient that collapses within the decay
// window) fires exactly once, sample-accurately; sustained tones, their
// abrupt cutoffs, speech-like bursts and ambient noise never fire; the
// refractory sets the minimum inter-onset spacing; estimateImpactUs is the
// back-dating contract used by the live wrapper.

#include "../onset_detector.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using pinpoint::OnsetDetector;
using pinpoint::OnsetDetectorConfig;
using pinpoint::OnsetResult;

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

static constexpr double kRate = 48000.0;

// Deterministic LCG noise in [-amp, amp] (no <random>, no global seed state).
struct Lcg {
    uint32_t s = 0x12345678u;
    float next(float amp)
    {
        s = s * 1664525u + 1013904223u;
        return amp * (2.0f * (float)(s >> 8) / 16777216.0f - 1.0f);
    }
};

// Feeds a detector and counts onsets.
struct Sim {
    OnsetDetector det;
    Lcg noise;
    int onsets = 0;
    OnsetResult last;

    explicit Sim(const OnsetDetectorConfig &cfg = {}) : det(cfg) { det.reset(kRate); }

    void push(float x)
    {
        const OnsetResult r = det.push(x);
        if (r.onset) { ++onsets; last = r; }
    }

    // Room tone: low-level broadband noise.
    void quiet(double ms, float amp = 0.002f)
    {
        const int n = (int)(ms * 1e-3 * kRate);
        for (int i = 0; i < n; ++i) push(noise.next(amp));
    }

    // Impact click: a short broadband burst. Returns its start sample index.
    int64_t click(double ms = 1.0, float amp = 0.8f)
    {
        const int64_t start = det.samplesProcessed();
        const int n = (int)(ms * 1e-3 * kRate);
        for (int i = 0; i < n; ++i) push((i % 2 ? -amp : amp));
        return start;
    }

    // Sustained 2 kHz tone (passes the high-pass) at constant amplitude.
    void tone(double ms, float amp = 0.5f)
    {
        const int n = (int)(ms * 1e-3 * kRate);
        for (int i = 0; i < n; ++i)
            push(amp * std::sin(2.0 * 3.14159265 * 2000.0 * i / kRate));
    }

    // Reverberant impact: sharp onset then an exponentially-decaying 2.5 kHz
    // ring — what a real strike sounds like in a live indoor space (the tail
    // outlasts an anechoic click). tauMs is the ring decay time constant.
    int64_t reverbClick(double tauMs, float amp = 0.8f, double ringMs = 400.0)
    {
        const int64_t start = det.samplesProcessed();
        const int n = (int)(ringMs * 1e-3 * kRate);
        for (int i = 0; i < n; ++i) {
            const double t = i / kRate;
            const float  e = amp * (float)std::exp(-t / (tauMs * 1e-3));
            push(e * (float)std::sin(2.0 * 3.14159265 * 2500.0 * t));
        }
        return start;
    }
};

int main()
{
    std::printf("=== onset_detector.h truth table ===\n\n");

    std::printf("-- A. impact click fires exactly once, sample-accurately --\n");
    {
        Sim s;
        s.quiet(1000);
        const int64_t start = s.click();
        s.quiet(200);
        CHECK("click fires exactly once", s.onsets == 1);
        const long long err = (long long)(s.last.onsetSample - start);
        std::printf("       onset error: %lld samples (%.2f ms)\n",
                    err, 1000.0 * (double)err / kRate);
        CHECK("onset within 1 ms of click start",
              std::llabs(err) <= (long long)(kRate / 1000.0));
        CHECK("confidence in (0,1]",
              s.last.confidence > 0.0f && s.last.confidence <= 1.0f);
    }

    std::printf("\n-- B. sustained tone: fails the decay gate -> None --\n");
    {
        Sim s;
        s.quiet(1000);
        s.tone(500);
        s.quiet(200);
        CHECK("tone start rejected (no decay)", s.onsets == 0);
        CHECK("tone CUTOFF rejected (not an attack)", s.onsets == 0);
    }

    std::printf("\n-- C. speech-like bursts: 150 ms on / 100 ms off -> None --\n");
    {
        Sim s;
        s.quiet(1000);
        for (int b = 0; b < 3; ++b) {
            s.tone(150, 0.4f);
            s.quiet(100);
        }
        s.quiet(200);
        CHECK("modulated bursts rejected", s.onsets == 0);
    }

    std::printf("\n-- D. ambient noise alone -> None --\n");
    {
        Sim s;
        s.quiet(2000, 0.01f);
        CHECK("ambient rejected", s.onsets == 0);
    }

    std::printf("\n-- E. refractory: min inter-onset spacing --\n");
    {
        Sim s;
        s.quiet(1000);
        s.click();
        s.quiet(20);    // second hit inside the 30 ms decay window
        s.click();
        s.quiet(200);
        CHECK("clicks 20 ms apart -> one onset", s.onsets == 1);

        Sim s2;
        s2.quiet(1000);
        s2.click();
        s2.quiet(100);  // well past decay window + refractory
        s2.click();
        s2.quiet(200);
        CHECK("clicks 100 ms apart -> two onsets", s2.onsets == 2);
    }

    std::printf("\n-- G. reverberant impact (ringing tail) still fires --\n");
    {
        // Ring decay constant ~45 ms: the tail outlasts the old 30 ms gate but
        // confirms within the 45 ms window. Guards against regressing the gate
        // back to a horizon that drops live-room strikes.
        Sim s;
        s.quiet(1000);
        s.reverbClick(45.0);
        s.quiet(300);
        CHECK("reverberant impact fires", s.onsets >= 1);
    }

    std::printf("\n-- H. absolute amplitude gate: quiet rejected, loud fires --\n");
    {
        // minLevelAbs gates candidate-opening on absolute level. A quiet tick
        // (keyboard click) below the gate must never fire; a loud strike above
        // it must. This is the fix for "quiet clicks detected, loud impacts
        // ignored" — quiet sounds no longer open candidates that mask impacts.
        OnsetDetectorConfig cfg;
        cfg.minLevelAbs = 0.06f;
        {
            Sim s(cfg); s.quiet(1000); s.click(1.0, 0.03f); s.quiet(200);
            CHECK("quiet click (0.03) below gate ignored", s.onsets == 0);
        }
        {
            Sim s(cfg); s.quiet(1000); s.click(1.0, 0.80f); s.quiet(200);
            CHECK("loud click (0.80) above gate fires", s.onsets == 1);
        }
        {
            // A loud sustained tone above the gate is still rejected by the
            // decay gate — the amplitude gate does NOT bypass it.
            Sim s(cfg); s.quiet(1000); s.tone(500); s.quiet(200);
            CHECK("loud tone above gate still rejected", s.onsets == 0);
        }
    }

    std::printf("\n-- F. est_t* back-dating math --\n");
    {
        // recv_now − samplesAfterOnset/rate − deviceLatency:
        // 480 samples at 48 kHz = 10 ms.
        CHECK_EQ_I64("estimateImpactUs(1e6, 480, 48k, 20ms)",
                     pinpoint::estimateImpactUs(1'000'000, 480, 48000.0, 20'000),
                     1'000'000 - 10'000 - 20'000);
        // Zero latency, onset at buffer end.
        CHECK_EQ_I64("estimateImpactUs(5e5, 0, 48k, 0)",
                     pinpoint::estimateImpactUs(500'000, 0, 48000.0, 0),
                     500'000);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "OK",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
