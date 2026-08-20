// Standalone truth-table test for shot_arbiter.h (shot detection P3).
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure
//
// Candidate sequences → decisions: two agreeing modalities commit once with
// the authoritative (Acoustic > Imu > Ball) timestamp; a lone weak candidate
// is rejected; a lone strong one commits; disagreement beyond the match
// tolerance does not fuse; the post-commit refractory drops echoes; a manual
// commit (noteCommit) arms the same refractory; cancel() voids a window.

#include "../../Gui/shot/shot_arbiter.h"

#include <cstdio>

using pinpoint::ArbCandidate;
using pinpoint::ArbiterConfig;
using pinpoint::ArbSource;
using pinpoint::ShotArbiter;

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
        std::printf("  [%s] %-44s got %lld  want %lld\n",                     \
                    ok ? "PASS" : "FAIL", label, g, w);                       \
        if (!ok) ++g_fail;                                                    \
    } while (0)

static constexpr int64_t kMs = 1000;   // µs per ms

int main()
{
    std::printf("=== shot_arbiter.h truth table ===\n\n");

    std::printf("-- A. two modalities agree -> commit once, acoustic timestamp --\n");
    {
        ShotArbiter arb;
        const int64_t t = 1'000'000;
        CHECK("first report opens the window",
              arb.report({ArbSource::Imu, t, 0.6f}, t + 30 * kMs));
        CHECK("second report joins it",
              !arb.report({ArbSource::Acoustic, t + 5 * kMs, 0.6f}, t + 50 * kMs));
        CHECK_EQ_I64("deadline = first report + holdMs",
                     arb.deadlineUs(), t + 30 * kMs + 200 * kMs);

        const auto d = arb.decide(arb.deadlineUs());
        CHECK("commit", d.commit);
        CHECK("authoritative source is Acoustic", d.src == ArbSource::Acoustic);
        CHECK_EQ_I64("committed timestamp is acoustic's", d.t_us, t + 5 * kMs);
        CHECK("two modalities", d.modalities == 2);
        CHECK("window consumed — second decide is empty",
              !arb.decide(arb.deadlineUs() + kMs).commit);
    }

    std::printf("\n-- B. lone weak candidate -> reject --\n");
    {
        ShotArbiter arb;
        arb.report({ArbSource::Imu, 1'000'000, 0.5f}, 1'030'000);
        CHECK("no commit", !arb.decide(arb.deadlineUs()).commit);
    }

    std::printf("\n-- C. lone strong candidate -> commit --\n");
    {
        ShotArbiter arb;
        arb.report({ArbSource::Acoustic, 1'000'000, 0.9f}, 1'020'000);
        const auto d = arb.decide(arb.deadlineUs());
        CHECK("commit", d.commit);
        CHECK("one modality", d.modalities == 1);
        CHECK_EQ_I64("its own timestamp", d.t_us, 1'000'000);
    }

    std::printf("\n-- D. authority order without acoustic: Imu > Ball --\n");
    {
        ShotArbiter arb;
        arb.report({ArbSource::Ball, 1'010'000, 0.6f}, 1'040'000);
        arb.report({ArbSource::Imu,  1'000'000, 0.6f}, 1'050'000);
        const auto d = arb.decide(arb.deadlineUs());
        CHECK("commit", d.commit);
        CHECK("authoritative source is Imu", d.src == ArbSource::Imu);
        CHECK_EQ_I64("committed timestamp is IMU's", d.t_us, 1'000'000);
    }

    std::printf("\n-- E. disagreement beyond matchTol does not fuse --\n");
    {
        ShotArbiter arb;   // matchTolMs = 40
        arb.report({ArbSource::Imu,      1'000'000,          0.6f}, 1'030'000);
        arb.report({ArbSource::Acoustic, 1'000'000 + 100 * kMs, 0.6f}, 1'050'000);
        CHECK("both weak, no agreement -> reject",
              !arb.decide(arb.deadlineUs()).commit);

        ShotArbiter arb2;
        arb2.report({ArbSource::Imu,      1'000'000,          0.6f}, 1'030'000);
        arb2.report({ArbSource::Acoustic, 1'000'000 + 100 * kMs, 0.9f}, 1'050'000);
        const auto d = arb2.decide(arb2.deadlineUs());
        CHECK("disagreeing strong acoustic commits alone", d.commit && d.modalities == 1);
        CHECK("with its own timestamp", d.t_us == 1'000'000 + 100 * kMs);
    }

    std::printf("\n-- F. refractory after commit drops echo candidates --\n");
    {
        ShotArbiter arb;   // refractoryMs = 1500
        arb.report({ArbSource::Acoustic, 1'000'000, 0.9f}, 1'020'000);
        const int64_t commitNow = arb.deadlineUs();
        CHECK("commit", arb.decide(commitNow).commit);

        CHECK("echo 500 ms later dropped (no window opened)",
              !arb.report({ArbSource::Imu, commitNow + 500 * kMs, 0.9f},
                          commitNow + 500 * kMs));
        CHECK("nothing collecting", !arb.collecting());

        const int64_t later = commitNow + 2000 * kMs;
        CHECK("after refractory a new window opens",
              arb.report({ArbSource::Imu, later, 0.9f}, later));
        CHECK("and commits", arb.decide(arb.deadlineUs()).commit);
    }

    std::printf("\n-- G. manual commit (noteCommit) arms the refractory --\n");
    {
        ShotArbiter arb;
        arb.noteCommit(1'000'000);
        CHECK("auto candidate 100 ms after manual shot dropped",
              !arb.report({ArbSource::Acoustic, 1'100'000, 0.9f}, 1'100'000));
        CHECK("nothing collecting", !arb.collecting());
    }

    std::printf("\n-- H. cancel() voids a pending window --\n");
    {
        ShotArbiter arb;
        arb.report({ArbSource::Acoustic, 1'000'000, 0.9f}, 1'020'000);
        arb.cancel();
        CHECK("not collecting after cancel", !arb.collecting());
        CHECK("decide finds nothing", !arb.decide(1'300'000).commit);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "OK",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
