// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// hm_unit_id_test — one spelling, persisted three times, and unrecoverable if it
// ever becomes two.
// ---------------------------------------------------------------------------
//
// Run via CTest (src/IMU/tests/CMakeLists.txt):
//   cmake --build build/imu-tests --target hm_unit_id_test
//   ctest --test-dir build/imu-tests -R hm_unit_id --output-on-failure
//
// "<deviceId>#lowerArm" / "<deviceId>#palm" is how one wG3 peripheral's two units
// are told apart, and it is written to disk in three independent places:
//
//   the EventBuffer SourceDescriptor::identifier   (Phase B)
//   the AppSettings::imuPlacement key              (Phase C)
//   source.serial in every exported swing.json     (the exporter)
//
// ⚠ WHICH IS WHY A SECOND SPELLING WOULD NOT FAIL — IT WOULD ORPHAN. A build that
// generated one form and read another would find no placement for a connected
// device (so the wizard asks for one that is already set) and no unit for a
// recorded lane (so re-analysis binds nothing and the swing holds two honest IMU
// streams and no wrist metric). Nothing errors in either state.
//
// ⚠ AND IT IS THE ONLY ROUTE BACK from a recorded lane to the segment it measured.
// A wG3 carries no per-user calibration to rebuild a binding from, so the serial
// IS the provenance — see hm_binding_recon.h, which parses exactly these strings.
//
// Pure and header-only: the unit is an int, not an `wr_unit`, so this needs
// neither the vendor SDK nor a device.

#include <QString>

#include <cstdio>

#include "hm_unit_id.h"

using namespace pinpoint::hm_unit_id;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

namespace {

// The real shape of a wG3 device id — a BLE address or a platform UUID.
const QString kAddr = QStringLiteral("D4:22:CD:00:9B:1F");
const QString kUuid = QStringLiteral("{2c2f9a11-6d1e-4f0a-9f3b-7a5c8e2d1044}");

void test_the_two_units_get_two_distinct_ids()
{
    check(unitIdFor(kAddr, kLowerArm) == kAddr + QStringLiteral("#lowerArm"),
          "lower arm spells #lowerArm");
    check(unitIdFor(kAddr, kPalm) == kAddr + QStringLiteral("#palm"),
          "palm spells #palm");
    check(unitIdFor(kAddr, kLowerArm) != unitIdFor(kAddr, kPalm),
          "and the two are distinct");
}

void test_it_round_trips_for_every_unit()
{
    // The property the three persisted copies rely on: whatever was written can be
    // read back to the same device and the same unit.
    for (const QString &dev : { kAddr, kUuid }) {
        for (int u = 0; u < kCount; ++u) {
            QString gotDev;
            int     gotUnit = -1;
            const bool ok = parse(unitIdFor(dev, u), &gotDev, &gotUnit);
            char label[160];
            std::snprintf(label, sizeof(label), "round trip: unit %d of %s", u,
                          dev.left(12).toLatin1().constData());
            check(ok && gotDev == dev && gotUnit == u, label);
        }
    }
}

void test_a_bare_device_id_is_not_a_unit_id()
{
    // A Witmotion, or a wG3 whose interim Phase A settings entry was never
    // migrated. FALSE is the answer, not lowerArm — guessing a unit here would
    // silently bind a Witmotion lane to a HackMotion segment role.
    QString dev = QStringLiteral("untouched");
    int     unit = 99;
    check(!parse(kAddr, &dev, &unit), "a bare device id does not parse");
    check(!parse(QString(), nullptr, nullptr), "an empty key does not parse");
    check(dev == QStringLiteral("untouched") && unit == 99,
          "and a refused parse writes nothing to the outputs");
}

void test_an_unrecognised_suffix_stays_unresolved_rather_than_guessed()
{
    // ⚠ THE POINT OF parse() REGENERATING AND COMPARING rather than matching the
    // suffix text. A future third unit, or a typo, must read as UNRESOLVED — a
    // build that fell back to lowerArm would bind a palm lane to the forearm and
    // publish a wrist angle computed from the wrong two segments, which looks
    // entirely plausible.
    check(!parse(kAddr + QStringLiteral("#upperArm"), nullptr, nullptr),
          "an unknown suffix does not resolve");
    check(!parse(kAddr + QStringLiteral("#PALM"), nullptr, nullptr),
          "the suffix is case-sensitive");
    check(!parse(kAddr + QStringLiteral("#"), nullptr, nullptr),
          "a bare separator does not resolve");
    check(!parse(kAddr + QStringLiteral("#palm2"), nullptr, nullptr),
          "a near-miss suffix does not resolve");
}

void test_the_split_is_at_the_LAST_separator()
{
    // A device id may itself contain '#'. Splitting at the first one would take the
    // wrong device id and then fail the regenerate-and-compare, so the lane would
    // go unresolved rather than mis-resolved — but it would go unresolved for a
    // device that is perfectly identifiable.
    const QString odd = QStringLiteral("dev#1");
    QString gotDev;
    int     gotUnit = -1;
    check(parse(unitIdFor(odd, kPalm), &gotDev, &gotUnit) && gotDev == odd && gotUnit == kPalm,
          "a device id containing '#' still round trips");
}

void test_a_leading_separator_is_not_a_device()
{
    // sep <= 0 — there is no device id to the left of it.
    check(!parse(QStringLiteral("#palm"), nullptr, nullptr),
          "a key with no device id does not parse");
}

void test_null_outputs_are_allowed()
{
    // parseBinding() calls this purely as a predicate ("is this serial a wG3
    // lane?") and passes nullptr for both. Pinned so that becoming an unconditional
    // write is caught here rather than as a crash on a Witmotion swing.
    check(parse(unitIdFor(kAddr, kPalm), nullptr, nullptr),
          "parse works as a bare predicate");
}

void test_the_unit_constants_match_the_vendor_enum_positions()
{
    // wrist/sample.h: WR_UNIT_LOWER_ARM = 0, WR_UNIT_PALM = 1. This header is
    // deliberately free of the SDK so the analysis layer can include it, so the
    // values are asserted against the enum at the one site that has both
    // (hm_instance.cpp) and stated here.
    check(kLowerArm == 0 && kPalm == 1 && kCount == 2, "0 = lower arm, 1 = palm, 2 units");
}

} // namespace

int main()
{
    std::printf("=== HackMotion unit id: exactly one spelling ===\n");

    test_the_two_units_get_two_distinct_ids();
    test_it_round_trips_for_every_unit();
    test_a_bare_device_id_is_not_a_unit_id();
    test_an_unrecognised_suffix_stays_unresolved_rather_than_guessed();
    test_the_split_is_at_the_LAST_separator();
    test_a_leading_separator_is_not_a_device();
    test_null_outputs_are_allowed();
    test_the_unit_constants_match_the_vendor_enum_positions();

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
