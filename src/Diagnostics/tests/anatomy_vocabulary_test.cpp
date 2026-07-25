// Standalone tests for the anatomy vocabulary (src/Diagnostics/anatomy_vocabulary.*):
// role metadata integrity, handedness resolution in BOTH directions, derived midpoints,
// the two pose layouts, and the hard guarantee that no admitted role reaches into the
// face or finger keypoint ranges. Qt-only (QPointF/QString), no fixture, own main().
//
//   cmake --build build/analyzer-tests --target anatomy_vocabulary_test
//   ctest --test-dir build/analyzer-tests -R anatomy_vocabulary --output-on-failure

#include "../anatomy_vocabulary.h"

#include <cmath>
#include <cstdio>
#include <set>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// A synthetic frame: every keypoint at (index, index*2) with full confidence, so a resolved
// position uniquely identifies which index was read. That is the property the handedness and
// face-range tests depend on.
struct Frame {
    std::vector<QPointF> kp;
    std::vector<float>   conf;

    explicit Frame(int n, float c = 1.0f) : kp(n), conf(n, c)
    {
        for (int i = 0; i < n; ++i) kp[i] = QPointF(i, i * 2);
    }
    KeypointFrame view() const { return KeypointFrame{ kp.data(), conf.data(), int(kp.size()) }; }
};

int main()
{
    std::printf("anatomy_vocabulary_test\n");

    const Frame f17(17);
    const Frame f133(133);

    // ── Table integrity ─────────────────────────────────────────────────────────
    {
        check(allRoles().size() == size_t(AnatomyRole::Count_), "allRoles covers every enumerator");

        std::set<QString> names;
        bool              allUnique = true, allNonEmpty = true;
        for (AnatomyRole r : allRoles()) {
            const QString n = roleName(r);
            if (n.isEmpty() || roleLabel(r).isEmpty()) allNonEmpty = false;
            if (!names.insert(n).second) allUnique = false;
        }
        check(allUnique, "role ids are unique");
        check(allNonEmpty, "every role has an id and a label");

        bool roundTrips = true;
        for (AnatomyRole r : allRoles()) {
            AnatomyRole back{};
            if (!roleFromName(roleName(r), back) || back != r) roundTrips = false;
        }
        check(roundTrips, "roleName/roleFromName round-trips for every role");

        AnatomyRole ignored{};
        check(!roleFromName(QStringLiteral("noSuchRole"), ignored), "unknown role id is rejected");
    }

    // ── The face/finger guarantee ───────────────────────────────────────────────
    // The whole point of a curated vocabulary: 133 keypoints is too many, and the face contour
    // (23–90) and finger joints (91–132) are low-confidence and motion-blurred at swing speeds.
    // A characteristic built on one of those is noise wearing a name. No admitted role may map
    // into either range, under either handedness.
    {
        bool clean = true;
        for (AnatomyRole r : allRoles()) {
            for (bool leadIsLeft : { true, false }) {
                const int idx = rolePrimaryIndex(r, leadIsLeft);
                if (idx >= kp::FaceFirst && idx <= kp::HandLast) clean = false;
            }
        }
        check(clean, "no admitted role maps into the face (23-90) or finger (91-132) ranges");
    }

    // ── Handedness: lead/trail resolve to mirrored keypoints ────────────────────
    {
        const ResolvedPoint lsL = resolvePoint(AnatomyRole::LeadShoulder,  f17.view(), true);
        const ResolvedPoint tsL = resolvePoint(AnatomyRole::TrailShoulder, f17.view(), true);
        const ResolvedPoint lsR = resolvePoint(AnatomyRole::LeadShoulder,  f17.view(), false);
        const ResolvedPoint tsR = resolvePoint(AnatomyRole::TrailShoulder, f17.view(), false);

        check(lsL.valid && near(lsL.p.x(), kp::LeftShoulder),  "leadIsLeft: lead shoulder = COCO left");
        check(tsL.valid && near(tsL.p.x(), kp::RightShoulder), "leadIsLeft: trail shoulder = COCO right");
        check(lsR.valid && near(lsR.p.x(), kp::RightShoulder), "leadIsRight: lead shoulder = COCO right");
        check(tsR.valid && near(tsR.p.x(), kp::LeftShoulder),  "leadIsRight: trail shoulder = COCO left");

        // Every paired role must mirror under handedness — this is what halves the pack.
        bool mirrorsAll = true;
        for (AnatomyRole r : allRoles()) {
            if (!roleIsPaired(r) || roleClass(r) != RoleClass::Point) continue;
            if (rolePrimaryIndex(r, true) != rolePrimaryIndex(mirroredRole(r), false)) mirrorsAll = false;
        }
        check(mirrorsAll, "every paired point role mirrors exactly under handedness");
        check(mirroredRole(AnatomyRole::Spine) == AnatomyRole::Spine, "unpaired role mirrors to itself");
    }

    // ── Derived points ──────────────────────────────────────────────────────────
    {
        const ResolvedPoint neck = resolvePoint(AnatomyRole::Neck, f17.view(), true);
        check(neck.valid && near(neck.p.x(), (kp::LeftShoulder + kp::RightShoulder) / 2.0),
              "neck = midpoint of the shoulders");

        const ResolvedPoint pelvis = resolvePoint(AnatomyRole::PelvisCentre, f17.view(), true);
        check(pelvis.valid && near(pelvis.p.x(), (kp::LeftHip + kp::RightHip) / 2.0),
              "pelvis centre = midpoint of the hips");

        const ResolvedPoint stance = resolvePoint(AnatomyRole::StanceCentre, f17.view(), true);
        check(stance.valid && near(stance.p.x(), (kp::LeftAnkle + kp::RightAnkle) / 2.0),
              "stance centre = midpoint of the ankles");

        // Derived points are handedness-invariant — they average both sides.
        const ResolvedPoint neckR = resolvePoint(AnatomyRole::Neck, f17.view(), false);
        check(near(neck.p.x(), neckR.p.x()), "derived midpoints are handedness-invariant");

        // Confidence takes the weaker parent, so the overlay's confidence-driven alpha is unchanged
        // by routing through this resolver.
        Frame weak(17);
        weak.conf[kp::LeftShoulder] = 0.40f;
        weak.conf[kp::RightShoulder] = 0.90f;
        const ResolvedPoint wn = resolvePoint(AnatomyRole::Neck, weak.view(), true);
        check(wn.valid && near(wn.conf, 0.40f, 1e-6), "derived point confidence = weaker parent");
    }

    // ── Layout gating: the foot tail exists only in WholeBody133 ────────────────
    {
        const ResolvedPoint toe17  = resolvePoint(AnatomyRole::LeadToe, f17.view(), true);
        const ResolvedPoint toe133 = resolvePoint(AnatomyRole::LeadToe, f133.view(), true);

        check(!toe17.valid && toe17.reason == UnavailableReason::NotInLayout,
              "lead toe is NotInLayout under Coco17");
        check(toe133.valid && near(toe133.p.x(), kp::LeftBigToe),
              "lead toe resolves under WholeBody133");

        // Indices 0-16 are identical across layouts, so every body role agrees.
        bool bodyAgrees = true;
        for (AnatomyRole r : allRoles()) {
            if (roleClass(r) != RoleClass::Point || roleSource(r) != RoleSource::Pose) continue;
            const int idx = rolePrimaryIndex(r, true);
            if (idx < 0 || idx > 16) continue;
            const ResolvedPoint a = resolvePoint(r, f17.view(), true);
            const ResolvedPoint b = resolvePoint(r, f133.view(), true);
            if (!a.valid || !b.valid || !near(a.p.x(), b.p.x())) bodyAgrees = false;
        }
        check(bodyAgrees, "every COCO-body role resolves identically in both layouts");

        check(f17.view().layout() == PoseLayout::Coco17, "layout inferred from array length (17)");
        check(f133.view().layout() == PoseLayout::WholeBody133, "layout inferred from array length (133)");
    }

    // ── Unavailability is reported, never faked ─────────────────────────────────
    {
        Frame dim(17, 0.10f);   // every keypoint below the admission threshold
        const ResolvedPoint p = resolvePoint(AnatomyRole::LeadWrist, dim.view(), true);
        check(!p.valid && p.reason == UnavailableReason::LowConfidence,
              "below-threshold keypoint reports LowConfidence, not a position");

        const ResolvedSegment s = resolveSegment(AnatomyRole::Spine, dim.view(), true);
        check(!s.valid && s.reason == UnavailableReason::LowConfidence,
              "a segment fails when an endpoint fails");

        // Club and ball are real and measured — by a different producer. The distinct reason is
        // what lets the roadmap say "ask the club track" rather than "improve the pose model".
        const ResolvedPoint ball = resolvePoint(AnatomyRole::Ball, f133.view(), true);
        check(!ball.valid && ball.reason == UnavailableReason::NotFromPose,
              "ball reports NotFromPose");
        const ResolvedSegment shaft = resolveSegment(AnatomyRole::Shaft, f133.view(), true);
        check(!shaft.valid && shaft.reason == UnavailableReason::NotFromPose,
              "shaft reports NotFromPose");

        // The spinal regions: no keypoint exists in ANY layout, so this is permanent.
        for (AnatomyRole r : { AnatomyRole::ThoracicSegment, AnatomyRole::LumbarSegment }) {
            const ResolvedSegment seg = resolveSegment(r, f133.view(), true);
            check(!seg.valid && seg.reason == UnavailableReason::NoKeypoint,
                  "spinal region reports NoKeypoint even on the full 133-point layout");
            check(roleNeedsNonPoseSensor(r), "spinal region is flagged as needing another sensor");
        }
        check(!roleNeedsNonPoseSensor(AnatomyRole::Spine),
              "gross spine is NOT flagged — it resolves fine, it just cannot separate the regions");

        // Asking for the wrong shape is a programming error, reported not guessed.
        const ResolvedPoint asPoint = resolvePoint(AnatomyRole::Spine, f17.view(), true);
        check(!asPoint.valid, "resolvePoint refuses a segment role");
        const ResolvedSegment asSeg = resolveSegment(AnatomyRole::LeadWrist, f17.view(), true);
        check(!asSeg.valid, "resolveSegment refuses a point role");
    }

    // ── Segments ────────────────────────────────────────────────────────────────
    {
        const ResolvedSegment spine = resolveSegment(AnatomyRole::Spine, f17.view(), true);
        check(spine.valid, "spine resolves");
        check(near(spine.a.x(), (kp::LeftHip + kp::RightHip) / 2.0),
              "spine runs pelvis -> neck (endpoint order fixes every angle's sign)");
        check(near(spine.b.x(), (kp::LeftShoulder + kp::RightShoulder) / 2.0), "spine ends at the neck");

        const ResolvedSegment thigh = resolveSegment(AnatomyRole::LeadThigh, f17.view(), true);
        check(thigh.valid && near(thigh.a.x(), kp::LeftHip) && near(thigh.b.x(), kp::LeftKnee),
              "lead thigh = hip -> knee");

        const ResolvedSegment shin = resolveSegment(AnatomyRole::LeadShin, f17.view(), true);
        check(shin.valid && near(shin.a.x(), kp::LeftKnee) && near(shin.b.x(), kp::LeftAnkle),
              "lead shin = knee -> ankle (knee flexion is shin vs thigh, not a point angle)");

        // Every pose segment must resolve on a full-confidence body frame — a segment in the
        // vocabulary that can never resolve is a trap for authors.
        bool allSegmentsResolve = true;
        for (AnatomyRole r : allRoles()) {
            if (roleClass(r) != RoleClass::Segment || roleSource(r) != RoleSource::Pose) continue;
            if (roleNeedsNonPoseSensor(r)) continue;
            if (!resolveSegment(r, f133.view(), true).valid) allSegmentsResolve = false;
        }
        check(allSegmentsResolve, "every resolvable pose segment resolves on a good frame");
    }

    // ── Datums are references only ──────────────────────────────────────────────
    {
        for (AnatomyRole r : { AnatomyRole::Ground, AnatomyRole::TargetLine, AnatomyRole::BallLine })
            check(roleClass(r) == RoleClass::Datum && roleSource(r) == RoleSource::World,
                  "world datum is classed Datum/World");
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
