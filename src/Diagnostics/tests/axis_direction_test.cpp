// Every corridor signal points the way its own condition claims.
//
// This is the test that makes the direction audit non-recurring. A signal whose direction is
// inverted does not fail loudly — it fires happily on the wrong swings, with correct-sounding
// consequence text attached, and looks exactly like a working detector. Three of these shipped in
// the seed pack and none was caught by anything.
//
// The fixture below is the authority: it states, per signal, which end of its measure means the
// condition is PRESENT, and why. Adding a corridor signal without adding a row here fails the test
// — a new signal cannot slip in unaudited.
//
//   cmake --build build/analyzer-tests --target axis_direction_test
//   ctest --test-dir build/analyzer-tests -R axis_direction --output-on-failure

#include "../characteristic_pack.h"

#include <QFile>

#include <cstdio>
#include <map>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// Expected direction per signal, with the sign convention that decides it. Every `why` quotes the
// metric catalogue, not an opinion — where the catalogue does not state a convention, the signal is
// listed as UNDEFINED below rather than guessed at.
struct Expect {
    const char *signalId;
    Direction   direction;
    const char *why;
};

static const Expect kExpected[] = {
    // ── setup ───────────────────────────────────────────────────────────────
    { "sig_postureUpright",   Direction::Low,
      "spineForwardBend is 'the forward tilt of the trunk'; upright is less of it" },
    { "sig_ballTooClose",     Direction::Low,   "ballBodyDistance: 'higher means further away'" },
    { "sig_ballTooFar",       Direction::High,  "ballBodyDistance: 'higher means further away'" },
    { "sig_alignmentOpen",    Direction::Low,
      "shoulderAlignment: 'open is negative and closed is positive' — the club-path convention" },
    { "sig_alignmentClosed",  Direction::High,  "shoulderAlignment: closed is the positive end" },
    { "sig_ballForward",      Direction::High,
      "ballPosition: 'positive is toward the lead foot'; forward IS the lead side" },
    { "sig_ballBack",         Direction::Low,
      "ballPosition: '-50 % level with the trail heel'" },
    { "sig_stanceWide",       Direction::High,  "stanceWidth: 'higher is wider'" },
    { "sig_stanceNarrow",     Direction::Low,   "stanceWidth: 'higher is wider'" },
    { "sig_sPosture",         Direction::High,  "lumbarExtension: 'higher means more arched'" },
    { "sig_cPosture",         Direction::High,  "thoracicFlexion: 'higher means more rounded'" },

    // ── backswing / top ─────────────────────────────────────────────────────
    { "sig_reverseSpine",     Direction::Low,
      "secondaryAxisTilt: players 'set roughly 6-8 deg at address and increase it'; reverse is too little" },
    { "sig_lossOfWidth",      Direction::Low,   "leadHandWidth: 'lower means narrower'" },
    { "sig_flatShoulderPlane",Direction::Low,   "shoulderPlaneAngle: 'lower means flatter'" },
    { "sig_flyingElbow",      Direction::High,
      "trailElbowHeight: 'higher means the elbow has risen further above the shoulder line'" },
    { "sig_trailHipHike",     Direction::High,  "pelvisLift measures a rise; a hike is more of it" },
    { "sig_xfactorDeficit",   Direction::Low,
      "xFactorStretch: 'roughly 5 deg of added stretch is typical'; a deficit is less" },

    // ── transition / downswing ──────────────────────────────────────────────
    { "sig_transitionRush",   Direction::Low,
      "tempoRatio: 'a quick, snatchy transition dropping it well below 3:1'" },
    { "sig_casting",          Direction::Low,
      "lagAngle: 'a larger retained angle ... means more stored lag'; casting spends it" },
    { "sig_earlyExtension",   Direction::High,
      "pelvisThrust: 'a rising, toward-ball trace through the downswing IS early extension'" },
    { "sig_overTheTop",       Direction::Low,
      "clubPath: 'in-to-out (+) or out-to-in (-)'; over the top delivers out-to-in" },
    { "sig_forwardLunge",     Direction::High,
      "thoraxLateralDrift: 'positive is toward the lead side'" },
    { "sig_sway",             Direction::Low,
      "pelvisSway: 'positive is toward the lead side'; sway goes AWAY from it, so negative" },
    { "sig_slide",            Direction::High,
      "pelvisSway: a slide runs toward the lead side in the downswing" },
    { "sig_hangingBack",      Direction::Low,
      "pelvisSway: hanging back is NOT having moved to the lead side by impact" },

    // ── impact / follow-through ─────────────────────────────────────────────
    { "sig_scooping",         Direction::Low,
      "leadWristFlexExt: '+ is bowed/flexed, - is cupped/extended'; scooping ADDS loft, so cupped" },
    { "sig_insufficientSet",  Direction::Low,
      "leadWristRadUln is the HINGE; less set is less of it" },
    { "sig_lossOfPosture",    Direction::Low,
      "spineForwardBend: losing posture is standing UP, the negative deviation from address" },
    { "sig_chickenWing",      Direction::High,
      "leadArmToTorso: 'a rising angle there means the arm is separating from the body'" },
    { "sig_lateBuckle",       Direction::High,  "leadKneeFlexion: 'higher means more bend'" },
};

// Signals whose metric states NO sign convention anywhere, so their direction cannot be checked
// against anything. These are not passes — they are a standing debt, and the count is asserted so
// it can only go down. Every one is on a `planned` metric, so the convention is still free to be
// chosen; it must be written into the descriptor when the producer is.
// Empty, and it must stay that way: every metric carrying a corridor signal now states which way
// is positive. A new entry here is a metric that shipped without saying, which is exactly how the
// three inversions got in.
static const char *kUndefinedConvention[] = { nullptr };

// Signals wrong in a way a DIRECTION cannot express. Both original entries are fixed:
// sig_insufficientSet now reads the hinge (leadWristRadUln) rather than bow/cup, and
// m_spineBendLoss takes the minimum rather than the maximum. Empty, and it should stay that way.
static const char *kKnownDefective[] = { nullptr };

static bool listed(const char *const *arr, size_t n, const QString &id)
{
    for (size_t i = 0; i < n; ++i)
        if (id == QLatin1String(arr[i])) return true;
    return false;
}

int main()
{
    QFile f(QStringLiteral(PP_CORE_PACK_PATH));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("  [FAIL] cannot open %s\n", PP_CORE_PACK_PATH);
        return 1;
    }
    const PackLoadResult res = loadPack(f.readAll(), QStringLiteral("core.json"));
    check(res.parsed, "the shipped pack parses");
    const CharacteristicPack &p = res.pack;

    std::map<QString, Direction> expect;
    for (const Expect &e : kExpected)
        expect[QLatin1String(e.signalId)] = e.direction;

    std::printf("=== every corridor signal points the way its condition claims ===\n");
    {
        int checked = 0, wrong = 0, unaudited = 0;
        for (const Signal &s : p.signalDefs) {
            if (s.test != SignalTest::OutsideCorridor) continue;

            if ((kUndefinedConvention[0] != nullptr
                 && listed(kUndefinedConvention, std::size(kUndefinedConvention), s.id))
                || (kKnownDefective[0] != nullptr
                    && listed(kKnownDefective, std::size(kKnownDefective), s.id)))
                continue;

            const auto it = expect.find(s.id);
            if (it == expect.end()) {
                ++unaudited;
                std::printf("        '%s' has no row in the fixture — audit it\n", qPrintable(s.id));
                continue;
            }
            ++checked;
            if (!s.direction.has_value() || *s.direction != it->second) {
                ++wrong;
                std::printf("        '%s' points %s, its condition needs %s\n", qPrintable(s.id),
                            s.direction ? qPrintable(directionName(*s.direction)) : "nowhere",
                            qPrintable(directionName(it->second)));
            }
        }
        std::printf("      %d audited\n", checked);
        check(wrong == 0, "every audited signal points the way its own condition claims");
        check(unaudited == 0, "no corridor signal is missing from the fixture");
    }

    std::printf("=== both tails of an axis point opposite ways ===\n");
    {
        // One norm, two conditions. If both tails pointed the same way they would fire together on
        // every deviation, and the axis would report a golfer as simultaneously too far forward and
        // too far back.
        std::map<QString, std::vector<const Signal *>> byAxis;
        for (const Condition &c : p.conditions) {
            if (c.axis.isEmpty()) continue;
            for (const QString &sid : c.detectedBy)
                if (const Signal *s = p.signal(sid); s && s->test == SignalTest::OutsideCorridor)
                    byAxis[c.axis].push_back(s);
        }

        int pairs = 0, bad = 0;
        for (const auto &[axis, sigs] : byAxis) {
            if (sigs.size() != 2) continue;
            ++pairs;
            if (!sigs[0]->direction || !sigs[1]->direction
                || *sigs[0]->direction == *sigs[1]->direction) {
                ++bad;
                std::printf("        axis '%s' has both tails pointing the same way\n",
                            qPrintable(axis));
            }
            if (sigs[0]->measures != sigs[1]->measures) {
                ++bad;
                std::printf("        axis '%s' reads two different measures\n", qPrintable(axis));
            }
        }
        std::printf("      %d two-tailed axes\n", pairs);
        check(pairs >= 4, "the known two-tailed axes are present");
        check(bad == 0, "each two-tailed axis reads ONE measure from opposite ends");
    }

    std::printf("=== the outstanding debt is tracked, not forgotten ===\n");
    {
        // These counts may only go DOWN. A rise means a new signal was added without a stated sign
        // convention, or a new defect was accepted.
        int undefined = 0, defective = 0;
        for (const Signal &s : p.signalDefs) {
            if (s.test != SignalTest::OutsideCorridor) continue;
            if ((kUndefinedConvention[0] != nullptr
                 && listed(kUndefinedConvention, std::size(kUndefinedConvention), s.id))) ++undefined;
            if (kKnownDefective[0] != nullptr
                && listed(kKnownDefective, std::size(kKnownDefective), s.id))            ++defective;
        }
        check(undefined == 0,
              "every signal's metric states which way is positive (must stay at zero)");
        check(defective == 0,
              "no signal is left wrong in a way direction cannot express (must stay at zero)");
    }

    std::printf("=== every signal-bearing measure says what HIGH means ===\n");
    {
        // The words an author reads instead of "High"/"Low". Without them the direction choice is
        // made against a convention the author has to remember correctly, which is how the three
        // inversions got in — so a measure carrying a corridor signal must carry these too.
        int measures = 0, silent = 0;
        for (const Signal &s : p.signalDefs) {
            if (s.test != SignalTest::OutsideCorridor) continue;
            for (const QString &mid : s.measures) {
                const Measure *m = p.measure(mid);
                if (m == nullptr) continue;
                ++measures;
                if (m->highMeans.isEmpty()) {
                    ++silent;
                    std::printf("        '%s' does not say what a high value means\n",
                                qPrintable(mid));
                }
            }
        }
        std::printf("      %d signal-bearing measure references\n", measures);
        check(silent == 0, "every measure a corridor signal reads says what HIGH means");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
