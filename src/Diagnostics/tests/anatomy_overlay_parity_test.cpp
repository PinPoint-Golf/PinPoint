// Parity between the anatomy vocabulary and the skeleton overlay.
//
// The overlay currently owns its own anatomy maths: src/Video/video_overlay_pose.cpp computes the
// neck and pelvis midpoints as local expressions inside a render function, and its bone topology is
// hand-duplicated in src/Gui/cameras/PpCameraFrame.qml (kBlueprintBones) with a source comment
// instructing that the two be kept in sync manually. Two hand-synced copies of the same anatomy is
// exactly the arrangement that drifts.
//
// This test pins the vocabulary to what the overlay draws TODAY, so that re-pointing the overlay at
// the shared resolver is provably a no-op rather than a visual regression, and so drift on either
// side fails the build in the meantime. What is drawn must be definitionally what is measured.
//
// The expectations below are transcribed from those two files. If a bone table legitimately
// changes, this test is where the change is acknowledged.
//
//   cmake --build build/analyzer-tests --target anatomy_overlay_parity_test
//   ctest --test-dir build/analyzer-tests -R anatomy_overlay_parity --output-on-failure

#include "../anatomy_vocabulary.h"

#include <cmath>
#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// The overlay's own admission threshold (video_overlay_pose.cpp: kMinScore).
static constexpr float kOverlayMinScore = 0.25f;

struct Frame {
    std::vector<QPointF> kp;
    std::vector<float>   conf;
    explicit Frame(int n, float c = 1.0f) : kp(n), conf(n, c)
    {
        for (int i = 0; i < n; ++i) kp[i] = QPointF(i * 3 + 1, i * 7 + 2);
    }
    KeypointFrame view() const { return KeypointFrame{ kp.data(), conf.data(), int(kp.size()) }; }
};

// A bone as the overlay/QML tables spell it: an unordered COCO index pair.
struct BonePair { int a, b; };
static bool sameUnordered(const BonePair &p, int x, int y)
{
    return (p.a == x && p.b == y) || (p.a == y && p.b == x);
}

int main()
{
    std::printf("anatomy_overlay_parity_test\n");

    const Frame f(133);

    // ── Threshold parity ────────────────────────────────────────────────────────
    {
        check(near(kMinKeypointConf, kOverlayMinScore, 1e-6),
              "vocabulary admission threshold equals the overlay's kMinScore (0.25)");
    }

    // ── Derived midpoint parity ─────────────────────────────────────────────────
    // video_overlay_pose.cpp:
    //     neck   = (kpPoint(5)  + kpPoint(6))  * 0.5
    //     pelvis = (kpPoint(11) + kpPoint(12)) * 0.5
    //     neckScore = min(kpScore(5), kpScore(6))
    // and each is visible only when BOTH parents clear the threshold.
    {
        const QPointF expectNeck   = (f.kp[5] + f.kp[6]) * 0.5;
        const QPointF expectPelvis = (f.kp[11] + f.kp[12]) * 0.5;

        const ResolvedPoint neck   = resolvePoint(AnatomyRole::Neck, f.view(), true);
        const ResolvedPoint pelvis = resolvePoint(AnatomyRole::PelvisCentre, f.view(), true);

        check(neck.valid && near(neck.p.x(), expectNeck.x()) && near(neck.p.y(), expectNeck.y()),
              "neck midpoint matches the overlay formula exactly");
        check(pelvis.valid && near(pelvis.p.x(), expectPelvis.x()) && near(pelvis.p.y(), expectPelvis.y()),
              "pelvis midpoint matches the overlay formula exactly");

        // Handedness must not touch a derived midpoint — the overlay has no handedness at all, so
        // any dependence here would be an immediate visual divergence.
        const ResolvedPoint neckR = resolvePoint(AnatomyRole::Neck, f.view(), false);
        check(near(neck.p.x(), neckR.p.x()) && near(neck.p.y(), neckR.p.y()),
              "derived midpoints are handedness-invariant, as the overlay requires");

        // Visibility: both parents must clear the threshold, and the score is the weaker parent.
        Frame oneWeak(133);
        oneWeak.conf[5] = 0.10f;
        check(!resolvePoint(AnatomyRole::Neck, oneWeak.view(), true).valid,
              "neck is invisible when either shoulder is below threshold");

        Frame graded(133);
        graded.conf[5] = 0.55f;
        graded.conf[6] = 0.80f;
        const ResolvedPoint gn = resolvePoint(AnatomyRole::Neck, graded.view(), true);
        check(gn.valid && near(gn.conf, 0.55f, 1e-6),
              "neck score = min(parents), so the overlay's confidence alpha is unchanged");
    }

    // ── Bone topology parity ────────────────────────────────────────────────────
    // Transcribed from kBones (video_overlay_pose.cpp) and kBlueprintBones
    // (PpCameraFrame.qml) — the two are required to agree with each other, and now with this.
    // Left-handed lead (leadIsLeft = true) maps lead -> COCO left, which is how the tables read.
    {
        struct Expect { AnatomyRole role; int a, b; const char *label; };
        const Expect kExpect[] = {
            { AnatomyRole::ShoulderLine,   5,  6, "shoulder line = {5,6} crossbar" },
            { AnatomyRole::HipLine,       11, 12, "hip line = {11,12} crossbar" },
            { AnatomyRole::LeadUpperArm,   5,  7, "lead upper arm = {5,7}" },
            { AnatomyRole::LeadForearm,    7,  9, "lead forearm = {7,9}" },
            { AnatomyRole::TrailUpperArm,  6,  8, "trail upper arm = {6,8}" },
            { AnatomyRole::TrailForearm,   8, 10, "trail forearm = {8,10}" },
            { AnatomyRole::LeadThigh,     11, 13, "lead thigh = {11,13}" },
            { AnatomyRole::LeadShin,      13, 15, "lead shin = {13,15}" },
            { AnatomyRole::TrailThigh,    12, 14, "trail thigh = {12,14}" },
            { AnatomyRole::TrailShin,     14, 16, "trail shin = {14,16}" },
        };

        for (const Expect &e : kExpect) {
            const ResolvedSegment seg = resolveSegment(e.role, f.view(), /*leadIsLeft=*/true);
            if (!seg.valid) { check(false, e.label); continue; }

            // Recover which indices the resolver used by matching against the synthetic positions.
            int ia = -1, ib = -1;
            for (int i = 0; i < int(f.kp.size()); ++i) {
                if (near(f.kp[i].x(), seg.a.x()) && near(f.kp[i].y(), seg.a.y())) ia = i;
                if (near(f.kp[i].x(), seg.b.x()) && near(f.kp[i].y(), seg.b.y())) ib = i;
            }
            check(sameUnordered(BonePair{ ia, ib }, e.a, e.b), e.label);
        }
    }

    // ── The spine is the vocabulary's own, and the overlay draws the same line ──
    {
        const ResolvedSegment spine = resolveSegment(AnatomyRole::Spine, f.view(), true);
        const QPointF neck   = (f.kp[5] + f.kp[6]) * 0.5;
        const QPointF pelvis = (f.kp[11] + f.kp[12]) * 0.5;
        check(spine.valid && near(spine.a.x(), pelvis.x()) && near(spine.b.x(), neck.x()),
              "spine spans the same two derived midpoints the overlay's spine gradient does");
    }

    // ── Documented, intentional divergences ─────────────────────────────────────
    // These are NOT parity failures. The overlay is a picture; the vocabulary is a measurement
    // surface, and it deliberately admits less.
    {
        // The overlay carries hand bones over COCO-WholeBody 91-132. The vocabulary excludes every
        // finger joint: they are low-confidence and motion-blurred at swing speeds, so a
        // characteristic built on one would be noise wearing a name. Lead/trail hand resolve to the
        // WRIST on both layouts instead.
        check(rolePrimaryIndex(AnatomyRole::LeadHand, true) == kp::LeftWrist,
              "lead hand resolves to the wrist, not a finger joint (intentional divergence)");
        bool noFingers = true;
        for (AnatomyRole r : allRoles())
            for (bool lead : { true, false }) {
                const int i = rolePrimaryIndex(r, lead);
                if (i >= kp::HandFirst && i <= kp::HandLast) noFingers = false;
            }
        check(noFingers, "no role reaches the finger range the overlay happily draws");

        // The overlay drops eyes and ears and draws a head circle off the nose; the vocabulary
        // keeps only the nose as `head` and admits no other face point.
        check(rolePrimaryIndex(AnatomyRole::Head, true) == kp::Nose,
              "head is the nose only — no other face keypoint is admitted");
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
