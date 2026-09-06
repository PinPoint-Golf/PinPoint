// Capture data-integrity check (capture_integrity_check.h).
//
// The shape it exists for is 2026-08-18 swing_0003: 150 fps face-on lane,
// frames stop 33 ms after impact, three lone frames arrive during a ~0.8 s host
// stall, then ~50 backlog frames stamped ~1 ms apart, then normal pacing. Cases:
//
//   A. A jittery but complete lane (intervals 0.7–1.6 periods) is clean.
//   B. The s3 shape: holes after impact only — warns, postImpact, not preImpact,
//      framesLost ≈ the frames the 594 ms hole swallowed, the burst is not a hole.
//   C. The same hole BEFORE impact — preImpact.
//   D. No impact anchor — every hole counts as pre-impact (conservative).
//   E. A short lane (2 frames) is not checkable: camerasChecked 0, warns() false.
//   F. holePeriods <= 0 disables the check (no claim).
//   G. Merging: one clean lane + one holed lane warns; firstHoleUs is the earliest.

#include "../capture_integrity_check.h"

#include <cmath>
#include <cstdio>
#include <vector>

using pinpoint::CaptureIntegrityVerdict;
using pinpoint::captureIntegrityOf;
using pinpoint::mergeCaptureIntegrity;

static int g_fail = 0;
static void check(const char *label, bool ok, const char *detail = "")
{
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", label, detail);
    if (!ok) ++g_fail;
}

static constexpr int64_t kPeriod = 6667;   // 150 fps

// n frames at kPeriod with deterministic jitter in [0.7, 1.6] periods (the real rig).
static std::vector<int64_t> jitteryLane(int n, int64_t t0 = 0)
{
    std::vector<int64_t> t;
    int64_t cur = t0;
    for (int i = 0; i < n; ++i) {
        t.push_back(cur);
        const double f = 0.7 + 0.9 * (0.5 + 0.5 * std::sin(0.37 * i));   // 0.7..1.6
        cur += int64_t(kPeriod * f);
    }
    return t;
}

int main()
{
    std::printf("=== capture data-integrity check ===\n\n");
    char buf[200];

    std::printf("-- A. complete jittery lane --\n");
    {
        const std::vector<int64_t> t = jitteryLane(600);
        const CaptureIntegrityVerdict v = captureIntegrityOf(t, 2400000);
        std::snprintf(buf, sizeof buf, "checked %d holes %d", v.camerasChecked, v.holes);
        check("lane checked",  v.camerasChecked == 1, buf);
        check("no holes",      v.ok && v.holes == 0 && !v.warns(), buf);
        check("no first hole", v.firstHoleUs < 0.0, buf);
    }

    std::printf("\n-- B. the s3 shape: stall after impact --\n");
    std::vector<int64_t> s3;
    int64_t impact = 0;
    {
        // 240 normal frames, impact at frame 235, then 594 ms hole, 3 lone frames
        // 69/138 ms apart, 50 burst frames 1 ms apart, then normal pacing.
        int64_t cur = 0;
        for (int i = 0; i < 240; ++i) { s3.push_back(cur); cur += kPeriod; }
        impact = s3[235];
        const int64_t lastGood = s3.back();
        cur = lastGood + 594400; s3.push_back(cur);
        cur += 69100;  s3.push_back(cur);
        cur += 137900; s3.push_back(cur);
        for (int i = 0; i < 50; ++i) { cur += 1000; s3.push_back(cur); }
        for (int i = 0; i < 200; ++i) { cur += kPeriod; s3.push_back(cur); }
        const CaptureIntegrityVerdict v = captureIntegrityOf(s3, impact);
        std::snprintf(buf, sizeof buf, "holes %d lost %d worst %.0f ms first %.0f us",
                      v.holes, v.framesLost, v.worstHoleMs, v.firstHoleUs);
        check("warns",                     v.warns(), buf);
        check("three holes",               v.holes == 3, buf);
        check("post-impact only",          v.postImpact && !v.preImpact, buf);
        check("worst hole is the stall",   std::fabs(v.worstHoleMs - 594.4) < 0.5, buf);
        check("frames lost ~ 88+9+20",     v.framesLost >= 110 && v.framesLost <= 120, buf);
        check("first hole opens at the last good frame", int64_t(v.firstHoleUs) == lastGood, buf);
    }

    std::printf("\n-- C. same hole before impact --\n");
    {
        const CaptureIntegrityVerdict v = captureIntegrityOf(s3, s3.back());
        check("pre-impact", v.warns() && v.preImpact && !v.postImpact);
    }

    std::printf("\n-- D. no impact anchor --\n");
    {
        const CaptureIntegrityVerdict v = captureIntegrityOf(s3, -1);
        check("holes counted as pre-impact (conservative)", v.warns() && v.preImpact);
    }

    std::printf("\n-- E. short lane not checkable --\n");
    {
        const CaptureIntegrityVerdict v = captureIntegrityOf({0, 6667}, -1);
        check("camerasChecked 0, no warning", v.camerasChecked == 0 && !v.warns() && v.ok);
    }

    std::printf("\n-- F. holePeriods <= 0 disables --\n");
    {
        const CaptureIntegrityVerdict v = captureIntegrityOf(s3, impact, 0.0);
        check("no claim", v.camerasChecked == 0 && !v.warns());
    }

    std::printf("\n-- G. merge clean + holed --\n");
    {
        const CaptureIntegrityVerdict clean = captureIntegrityOf(jitteryLane(600, 5000), impact);
        const CaptureIntegrityVerdict holed = captureIntegrityOf(s3, impact);
        const CaptureIntegrityVerdict m = mergeCaptureIntegrity(clean, holed);
        std::snprintf(buf, sizeof buf, "checked %d holes %d first %.0f", m.camerasChecked, m.holes, m.firstHoleUs);
        check("two lanes checked", m.camerasChecked == 2, buf);
        check("warns with the holed lane's counts", m.warns() && m.holes == 3 && m.postImpact, buf);
        check("firstHoleUs from the holed lane", int64_t(m.firstHoleUs) == int64_t(holed.firstHoleUs), buf);
        const CaptureIntegrityVerdict m2 = mergeCaptureIntegrity(holed, clean);
        check("merge is order-independent", m2.holes == m.holes && m2.firstHoleUs == m.firstHoleUs
                                             && m2.ok == m.ok && m2.camerasChecked == m.camerasChecked);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
