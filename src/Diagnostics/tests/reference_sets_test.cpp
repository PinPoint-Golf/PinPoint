// The two reference registries a characteristic points at: the physical screens behind `screenRef`
// and the drills behind `drills`.
//
// Both joins are EXACT STRING MATCHES into a separate file, which is the failure mode worth a test
// of its own: a typo or a renamed row does not throw, does not warn at load, and does not stop the
// condition rendering. It just leaves the panel that was meant to tell a coach how to run the test
// blank, with nothing anywhere to say why. So this gates the shipped content in BOTH directions and
// each validator in both — firing when it should, and silent when it should not, since half the
// value of a validator test is the negative case.
//
//   cmake --build build/analyzer-tests --target reference_sets_test
//   ctest --test-dir build/analyzer-tests -R reference_sets --output-on-failure

#include "../characteristic_pack.h"
#include "../drill_pack.h"
#include "../screen_pack.h"

#include <QFile>
#include <QSet>

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static bool hasCode(const ValidationReport &r, const char *code)
{
    return !r.withCode(QString::fromLatin1(code)).empty();
}

static CharacteristicPack shippedPack()
{
    QFile f(QString::fromLocal8Bit(qgetenv("PINPOINT_CORE_PACK")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return loadPack(f.readAll(), QStringLiteral("core")).pack;
}

int main()
{
    std::printf("=== the shipped registries load ===\n");
    const ScreenSet &screens = sharedScreenSet();
    const DrillSet  &drills  = sharedDrillSet();

    check(!screens.screens.empty(), "the screen set loaded");
    check(!drills.drills.empty(), "the drill set loaded");
    check(validateScreenSet(screens).ok(), "the shipped screen set has no errors");
    check(validateDrillSet(drills).ok(), "the shipped drill set has no errors");
    std::printf("      (%d screens, %d drills)\n",
                int(screens.screens.size()), int(drills.drills.size()));

    std::printf("=== every reference resolves ===\n");
    {
        const CharacteristicPack p = shippedPack();
        check(!p.conditions.empty(), "the shipped pack loaded");

        int danglingScreens = 0, danglingDrills = 0, withScreen = 0, withDrill = 0;
        for (const Condition &c : p.conditions) {
            if (!c.screenRef.isEmpty()) {
                ++withScreen;
                if (!screens.screen(c.screenRef)) {
                    ++danglingScreens;
                    std::printf("        '%s' -> unknown screen '%s'\n",
                                qPrintable(c.id), qPrintable(c.screenRef));
                }
            }
            for (const QString &d : c.drills) {
                ++withDrill;
                if (!drills.drill(d)) {
                    ++danglingDrills;
                    std::printf("        '%s' -> unknown drill '%s'\n",
                                qPrintable(c.id), qPrintable(d));
                }
            }
        }
        check(danglingScreens == 0, "every screenRef in the pack names a real screen");
        check(danglingDrills == 0, "every drill reference in the pack names a real drill");
        check(withScreen > 0, "…and screens are actually referenced, so the check can fail");
        check(withDrill > 0, "…and so are drills");
        std::printf("        (%d screen references, %d drill references)\n", withScreen, withDrill);
    }

    std::printf("=== no branded screening system reached the content ===\n");
    {
        // The same rule the pack is held to, checked against the raw bytes so a note or a stray
        // field cannot slip past. The MOVEMENTS are common property of the clinical literature; the
        // packaging of them into a named system is somebody's product and must not enter the repo
        // in any form — not a citation, not an author, not an explanatory aside.
        const char *forbidden[] = { "titleist", "tpi", "trackman", "flightscope",
                                    "performance institute", "hackmotion", "k-vest", "kvest" };
        int hits = 0;
        for (const QByteArray &blob : { saveScreenSet(screens), saveDrillSet(drills) }) {
            const QString lower = QString::fromUtf8(blob).toLower();
            for (const char *needle : forbidden)
                if (lower.contains(QLatin1String(needle))) {
                    ++hits;
                    std::printf("        brand token '%s'\n", needle);
                }
        }
        check(hits == 0, "no commercial screening system is named anywhere in the registries");
    }

    std::printf("=== a screen says how to run it and what passing looks like ===\n");
    {
        int noProtocol = 0, noPass = 0, numericNoUnit = 0;
        for (const Screen &s : screens.screens) {
            if (s.protocol.trimmed().isEmpty()) ++noProtocol;
            if (s.passCriterion.trimmed().isEmpty()) ++noPass;
            if (s.passAtLeast.has_value() && s.unit.isEmpty()) ++numericNoUnit;
        }
        check(noProtocol == 0, "every shipped screen states a protocol");
        check(noPass == 0, "every shipped screen states what passing looks like");
        check(numericNoUnit == 0, "every numeric pass floor carries its unit");

        // Several screens are deliberately QUALITATIVE — "achieves neutral extension", "smooth
        // independent rotation". Inventing a number for those would read as more certain than the
        // words, so the absence is intentional and asserted rather than tolerated.
        const int qualitative = int(std::count_if(
            screens.screens.begin(), screens.screens.end(),
            [](const Screen &s) { return !s.passAtLeast.has_value(); }));
        check(qualitative > 0, "some screens are qualitative on purpose, not merely unfinished");
    }

    std::printf("=== a drill says what to do and what it is for ===\n");
    {
        int noInstruction = 0, noTarget = 0;
        for (const Drill &d : drills.drills) {
            if (d.instruction.trimmed().isEmpty()) ++noInstruction;
            if (d.targets.trimmed().isEmpty()) ++noTarget;
        }
        check(noInstruction == 0, "every shipped drill says what the golfer does");
        check(noTarget == 0, "every shipped drill says what it is trying to change");
    }

    std::printf("=== the validators fire, and stay quiet ===\n");
    {
        ScreenSet s;
        s.screens.push_back(Screen{ QStringLiteral("screen.a"), QStringLiteral("A"), {},
                                    QStringLiteral("do it"), QStringLiteral("pass"), {}, {}, {}, {} });
        check(validateScreenSet(s).ok(), "a well-formed screen set passes");

        ScreenSet dup = s;
        dup.screens.push_back(dup.screens.front());
        check(hasCode(validateScreenSet(dup), "duplicateId"), "a duplicate screen id is refused");

        ScreenSet ns = s;
        ns.screens.front().id = QStringLiteral("a");
        check(hasCode(validateScreenSet(ns), "screenIdNamespace"),
              "an id outside the screen. namespace is refused — nothing could point at it");

        ScreenSet nu = s;
        nu.screens.front().passAtLeast = 30.0;
        check(hasCode(validateScreenSet(nu), "screenUnitMissing"),
              "a numeric pass floor with no unit is refused");

        ScreenSet np = s;
        np.screens.front().protocol.clear();
        check(hasCode(validateScreenSet(np), "screenNoProtocol"), "a screen nobody could run warns");

        DrillSet d;
        d.drills.push_back(Drill{ QStringLiteral("drill.a"), QStringLiteral("A"),
                                  QStringLiteral("do it"), QStringLiteral("for this"), {}, {} });
        check(validateDrillSet(d).ok(), "a well-formed drill set passes");

        DrillSet dn = d;
        dn.drills.front().id = QStringLiteral("a");
        check(hasCode(validateDrillSet(dn), "drillIdNamespace"),
              "a drill id outside the drill. namespace is refused");

        DrillSet dt = d;
        dt.drills.front().targets.clear();
        check(hasCode(validateDrillSet(dt), "drillNoTarget"),
              "a drill that does not say what it changes warns");
    }

    std::printf("=== round-trip ===\n");
    {
        const ScreenLoadResult rs = loadScreenSet(saveScreenSet(screens), QStringLiteral("rt"));
        check(rs.loaded && rs.set.screens.size() == screens.screens.size(),
              "a screen set survives save and load");
        const Screen *before = screens.screen(QStringLiteral("screen.ankleDorsiflexion"));
        const Screen *after  = rs.set.screen(QStringLiteral("screen.ankleDorsiflexion"));
        check(before && after && after->passAtLeast.has_value()
                  && *after->passAtLeast == *before->passAtLeast && after->unit == before->unit,
              "…including the optional numeric floor and its unit");

        const DrillLoadResult rd = loadDrillSet(saveDrillSet(drills), QStringLiteral("rt"));
        check(rd.loaded && rd.set.drills.size() == drills.drills.size(),
              "a drill set survives save and load");
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
