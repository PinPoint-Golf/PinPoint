/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

// CT-S3 (host-side declaration) and CT-I19, the host half.  Work package H2.
//
// CT-S3's own preamble is the reason this file exists: "a single-vendor
// implementation satisfies I19 BY ACCIDENT: its host conventions are correct in
// hardcoded form, so every test passes and nothing is on the wire.  This is the
// failure most likely to survive to release, because the reference host will
// always pass it."
//
// Assertions 1 and 3 are the host's; assertion 2 is paired and needs the
// synthetic peer of CONF §2c (L13, session 3), so it is not here and is not
// claimed.
//
//   1. The host emits `declare` carrying `timing`, `geometry` and `intrinsics`
//      for EVERY Source it owns, and emits `declare` with an empty `sources`
//      list when it owns none.
//   3. No code path infers a convention, geometry or readout time from
//      `Peer.product`, from `role`, or from a platform identifier.
//
// Every validator called here is libppcp's, not this repository's: I19, I22,
// I28 and I31 are the library's to enforce and the host's to satisfy.

#include "ppcp_source_declaration.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Ppcp;

namespace {

// A FLIR/Aravis-shaped camera: a free ROI, several Bayer formats, a frame rate
// range.  Modelled on what VideoInputFactory::enumerateDevices() actually reads
// off an Aravis device, not on what would be convenient.
PpcpSourceDeclaration::Camera machineVisionCamera(const char *serial = "22334455")
{
    PpcpSourceDeclaration::Camera c;
    c.backend = VideoInputFactory::Backend::Aravis;
    c.id = "Basler-acA1300-200uc-22334455";
    c.label = "Down the line";
    c.caps.vendorName = "Basler";
    c.caps.modelName = "acA1300-200uc";
    c.caps.serialNumber = serial;
    c.caps.connectionInterface = CameraCapabilities::Interface::USB3;

    c.caps.resolution.kind = CapabilityKind::Range;
    c.caps.resolution.widthRange = { 64, 1280, 4, 1280 };
    c.caps.resolution.heightRange = { 64, 1024, 2, 1024 };
    c.caps.resolution.defaultResolution = { 1280, 1024 };

    c.caps.pixelFormat.kind = CapabilityKind::Discrete;
    PixelFormat bayer;
    bayer.nativeKey = "BayerRG8";
    bayer.encoding = PixelEncoding::BayerRG8;
    bayer.bitsPerPixel = 8;
    PixelFormat mono;
    mono.nativeKey = "Mono8";
    mono.encoding = PixelEncoding::Mono8;
    mono.bitsPerPixel = 8;
    c.caps.pixelFormat.supported = { bayer, mono };
    c.caps.pixelFormat.defaultFormat = bayer;

    c.caps.frameRate.kind = CapabilityKind::Range;
    c.caps.frameRate.range = { 20.0, 200.0, 0.0, 200.0 };
    c.caps.frameRate.readable = true;

    // Gain in dB, and no ISO at all — which is why `optical` is withheld
    // rather than half-invented.  See the note in the .cpp.
    c.caps.gain.kind = CapabilityKind::Range;
    c.caps.gain.range = { 0.0, 24.0, 0.1, 0.0 };
    c.caps.exposureTime.kind = CapabilityKind::Range;
    c.caps.exposureTime.range = { 20.0, 10000.0, 1.0, 500.0 };   // microseconds
    return c;
}

// An AVFoundation/Qt-shaped camera: discrete presets, a preferred format.
PpcpSourceDeclaration::Camera platformCamera()
{
    PpcpSourceDeclaration::Camera c;
    c.backend = VideoInputFactory::Backend::AppleAVFoundation;
    c.id = "0x1234000005ac8600";
    c.label = "FaceTime HD Camera";
    c.caps.modelName = "FaceTime HD Camera";
    c.caps.serialNumber = "0x1234000005ac8600";
    c.caps.driverVersion = "Qt6 Multimedia";

    c.caps.resolution.kind = CapabilityKind::Discrete;
    c.caps.resolution.presets = { { 1920, 1080 }, { 1280, 720 } };
    c.caps.resolution.defaultResolution = { 1920, 1080 };

    c.caps.pixelFormat.kind = CapabilityKind::Discrete;
    PixelFormat nv12;
    nv12.nativeKey = "NV12";
    nv12.encoding = PixelEncoding::YUV420_NV12;
    c.caps.pixelFormat.supported = { nv12 };
    c.caps.pixelFormat.defaultFormat = nv12;

    c.caps.frameRate.kind = CapabilityKind::Range;
    c.caps.frameRate.range = { 1.0, 60.0, 0.0, 30.0 };
    return c;
}

const ppcp_capture_profile *profileOfSource(const ppcp_source &s, std::size_t i)
{
    return (i < s.profile_count) ? &s.profiles[i] : nullptr;
}

}  // namespace

// ── CT-S3 assertion 1, second half — a host owning no Sources ──────────────
// MSG 3.3d: "A host owning no Sources sends `declare` with an empty `sources`
// list — IT DOES NOT SKIP THE MESSAGE."  CORE 5.6.1: "Source count is not a
// role marker; a host owning no capture Sources participates fully."
//
// This is the case a developer machine cannot produce by having hardware, and
// it is the one a single-vendor implementation is least likely to have tried.
TEST(PpcpDeclaration, AHostWithNoSourcesStillDeclares)
{
    PpcpSourceDeclaration d;
    std::string err;
    ASSERT_TRUE(d.build("host-1", PpcpSourceDeclaration::Inventory{}, &err)) << err;

    const ppcp_peer_desc *p = d.peer();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->source_count, 0u);
    EXPECT_EQ(p->role, PPCP_ROLE_HOST);

    // The declaration is not merely non-empty, it is VALID — the library says
    // so, which is the difference between "we sent something" and "we sent a
    // declaration a foreign peer can act on".
    std::string where;
    EXPECT_EQ(d.validate(&where), PPCP_OK) << where;

    // 3.3b — every timebase a Source references is declared, and the host's
    // clock is declared whether or not anything is sampled against it yet.
    ASSERT_EQ(p->timebase_count, 1u);
    EXPECT_STREQ(p->timebases[0].id.v, "tb:host");
}

// ── CT-S3 assertion 1, first half / CT-I19 ─────────────────────────────────
// EVERY Source, EVERY profile, `timing` + `geometry` + `intrinsics`, whichever
// peer owns the Source (CORE 5.6a).  Asserted field by field rather than by
// calling validate() alone, because a validator that grew a hole would take
// this test with it.
TEST(PpcpDeclaration, EveryCameraProfileCarriesTimingGeometryAndIntrinsics)
{
    PpcpSourceDeclaration::Inventory inv;
    inv.cameras.push_back(machineVisionCamera());
    inv.cameras.push_back(platformCamera());

    PpcpSourceDeclaration d;
    std::string err;
    ASSERT_TRUE(d.build("host-1", inv, &err)) << err;

    const ppcp_peer_desc *p = d.peer();
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(p->source_count, 2u);

    for (std::size_t i = 0; i < p->source_count; ++i) {
        const ppcp_source &s = p->sources[i];
        SCOPED_TRACE(s.id.v);

        // 5.6a — every Source declares `timebase_id`.
        EXPECT_STREQ(s.timebase_id.v, "tb:host");
        ASSERT_GT(s.profile_count, 0u);

        for (std::size_t j = 0; j < s.profile_count; ++j) {
            const ppcp_capture_profile *cp = profileOfSource(s, j);
            ASSERT_NE(cp, nullptr);
            SCOPED_TRACE(cp->id.v);

            // I19, in the three fields it names.
            EXPECT_TRUE(cp->has_geometry) << "a camera profile with no geometry";
            EXPECT_TRUE(cp->has_intrinsics) << "a camera profile with no intrinsics";
            EXPECT_EQ(ppcp_timing_validate(&cp->timing), PPCP_OK);
            EXPECT_EQ(ppcp_geometry_validate(&cp->geometry), PPCP_OK);

            // I28 — absence means not measured, and nothing here synthesised a
            // MeasuredCapability from a claimed value or a device table.
            EXPECT_FALSE(cp->has_measured)
                << "a `measured` appeared with no self-test behind it";

            // I31 / plan A12 — every timing constant nobody has measured is
            // `assumed`.  `measured` means measured on THIS device model by
            // this project (5.7f), and no model has been through a rig.
            if (cp->timing.has_offset)
                EXPECT_EQ(cp->timing.offset_provenance, PPCP_PROV_ASSUMED);
            if (cp->geometry.kind == PPCP_GEOM_ROLLING_SHUTTER)
                EXPECT_EQ(cp->geometry.readout_provenance, PPCP_PROV_ASSUMED);
        }
    }

    std::string where;
    EXPECT_EQ(d.validate(&where), PPCP_OK) << where;
}

// ── CORE 5.6.1's own table, both rows ──────────────────────────────────────
// The table is normative about what each kind of Source declares, and it is the
// thing a third-party host has to be able to read off the wire.  A machine
// vision camera and a platform camera declare DIFFERENT conventions from the
// same host, which is the property that makes the conversion of §6.1 possible
// at all — and it is exactly what a hardcoded host loses.
TEST(PpcpDeclaration, MachineVisionAndPlatformCamerasDeclareDifferentConventions)
{
    PpcpSourceDeclaration::Inventory inv;
    inv.cameras.push_back(machineVisionCamera());
    inv.cameras.push_back(platformCamera());

    PpcpSourceDeclaration d;
    std::string err;
    ASSERT_TRUE(d.build("host-1", inv, &err)) << err;
    ASSERT_EQ(d.sources().size(), 2u);

    // FLIR/Aravis: `start` + `global` + `fixed` (CORE 5.6.1, plan A12).
    const ppcp_source &mv = d.sources()[0];
    ASSERT_GT(mv.profile_count, 0u);
    for (std::size_t j = 0; j < mv.profile_count; ++j) {
        EXPECT_EQ(mv.profiles[j].timing.convention, PPCP_CONV_START);
        // I22 made structural: the offset exists if and only if the convention
        // is `nominal_frame_start`, so a `start` profile cannot carry one.
        EXPECT_FALSE(mv.profiles[j].timing.has_offset);
        EXPECT_EQ(mv.profiles[j].geometry.kind, PPCP_GEOM_GLOBAL);
        EXPECT_EQ(mv.profiles[j].intrinsics, PPCP_INTR_FIXED);
    }

    // Platform camera: `nominal_frame_start` + `rolling_shutter` + a DECLARED
    // zero offset with `assumed` provenance (5.7b — a declared zero is a
    // checkable claim; an omitted field is not).
    const ppcp_source &plat = d.sources()[1];
    ASSERT_GT(plat.profile_count, 0u);
    for (std::size_t j = 0; j < plat.profile_count; ++j) {
        EXPECT_EQ(plat.profiles[j].timing.convention, PPCP_CONV_NOMINAL_FRAME_START);
        ASSERT_TRUE(plat.profiles[j].timing.has_offset);
        EXPECT_EQ(plat.profiles[j].timing.frame_start_to_exposure_offset_ns, 0);
        EXPECT_EQ(plat.profiles[j].timing.offset_provenance, PPCP_PROV_ASSUMED);
        EXPECT_EQ(plat.profiles[j].geometry.kind, PPCP_GEOM_ROLLING_SHUTTER);
        EXPECT_EQ(plat.profiles[j].geometry.readout_provenance, PPCP_PROV_ASSUMED);
    }

    // And the two really do differ, which is the assertion CT-S3's preamble is
    // about: a host that hardcoded one convention would pass everything above
    // and fail this.
    EXPECT_NE(mv.profiles[0].timing.convention, plat.profiles[0].timing.convention);
}

// ── The microphone (CORE 5.6, plan H2) ─────────────────────────────────────
// The host nominates from sound, so its microphone is a Source like any other:
// `convention: mid`, no format, and on the same declared timebase.  Its
// calibration WOULD be its position — "which IS the acoustic time-of-flight
// constant" (CORE 5.6) — and it is absent because nobody has measured it and
// 5.9 makes uncertainty mandatory.
TEST(PpcpDeclaration, TheHostMicrophoneIsADeclaredSource)
{
    PpcpSourceDeclaration::Inventory inv;
    inv.hasMicrophone = true;
    inv.microphone.id = "BuiltInMicrophoneDevice";
    inv.microphone.label = "MacBook Pro Microphone";

    PpcpSourceDeclaration d;
    std::string err;
    ASSERT_TRUE(d.build("host-1", inv, &err)) << err;
    ASSERT_EQ(d.sources().size(), 1u);

    const ppcp_source &mic = d.sources()[0];
    EXPECT_STREQ(mic.kind.v, "microphone");
    EXPECT_STREQ(mic.timebase_id.v, "tb:host");
    EXPECT_FALSE(ppcp_source_kind_is_camera(&mic));
    ASSERT_EQ(mic.profile_count, 1u);
    EXPECT_EQ(mic.profiles[0].timing.convention, PPCP_CONV_MID);
    EXPECT_FALSE(mic.profiles[0].format.present) << "audio is not a framed source (5.7)";
    EXPECT_FALSE(mic.has_calibration)
        << "a calibration offered with no measured uncertainty (5.9)";

    std::string where;
    EXPECT_EQ(d.validate(&where), PPCP_OK) << where;
}

// ── The declared timebase (CORE 5.3) ───────────────────────────────────────
TEST(PpcpDeclaration, TheHostClockIsDeclaredAsItActuallyBehaves)
{
    const ppcp_timebase tb = hostTimebase();
    EXPECT_EQ(ppcp_timebase_validate(&tb), PPCP_OK);
    EXPECT_STREQ(tb.id.v, "tb:host");
    EXPECT_FALSE(ppcp_timebase_is_wall(&tb)) << "I15 — no interval is ever computed from wall";

    // `monotonic`, deliberately, on every platform: the claim that cannot
    // mislead a consumer where the behaviour is unmeasured.  See the .cpp.
    EXPECT_EQ(tb.kind, PPCP_TB_MONOTONIC);
    EXPECT_FALSE(tb.epoch_stable) << "steady_clock's epoch does not survive a restart";
    EXPECT_GT(tb.resolution_ns, 0);

    // The clock the peer engine will be handed (ground rule 7: the library owns
    // no clock).  I1: it answers for a NAMED timebase and for no other.
    const ppcp_clock c = hostClock();
    ppcp_instant now{};
    ASSERT_EQ(ppcp_clock_read(&c, "tb:host", &now), PPCP_OK);
    EXPECT_STREQ(now.tb.v, "tb:host");
    EXPECT_GT(now.ns, 0);

    ppcp_instant elsewhere{};
    EXPECT_EQ(ppcp_clock_read(&c, "tb:device", &elsewhere), PPCP_ERR_NOT_FOUND)
        << "the host answered for a clock it does not have";
}

// ── CT-S3 assertion 3 — asserted over the source, because it is a negative ──
//
// "No code path infers a convention, geometry or readout time from
// `Peer.product`, from `role`, or from a platform identifier."
//
// The positive half is by construction: the ONLY calls to the convention and
// geometry constructors in this whole subsystem are in
// ppcp_source_declaration.cpp, and they map THIS host's own backends, which is
// where the mapping is legitimate.  What this test does is make that a checked
// fact rather than a claim — the day somebody adds a `if (product == "…")` to
// the ingest path, it goes red.
//
// PP_PPCP_SRC_DIR is set by CMake.
TEST(PpcpDeclaration, NoCodePathDerivesAConventionFromAProductOrARole)
{
#ifndef PP_PPCP_SRC_DIR
    GTEST_SKIP() << "PP_PPCP_SRC_DIR not set — the static half of CT-S3 cannot run";
#else
    static const char *kConstructors[] = {
        "ppcp_timing_make", "ppcp_timing_make_nominal_frame_start",
        "ppcp_geometry_make_global", "ppcp_geometry_make_rolling_shutter",
    };
    // Every file in src/Ppcp that is not the declaration builder itself.
    static const char *kOtherFiles[] = {
        "ppcp_transport.cpp", "ppcp_transport.h",
        "ppcp_source_declaration.h", "ppcp_host_inventory.cpp",
    };

    for (const char *name : kOtherFiles) {
        const std::string path = std::string(PP_PPCP_SRC_DIR) + "/" + name;
        std::ifstream in(path);
        if (!in) continue;   // a file this session has not written yet
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        for (const char *ctor : kConstructors) {
            // A mention in a comment is fine and is how the rule is explained;
            // a CALL is not.  The distinguisher is the open bracket.
            const std::string call = std::string(ctor) + "(";
            EXPECT_EQ(text.find(call), std::string::npos)
                << name << " calls " << ctor << " — a convention or geometry must come "
                   "from the wire everywhere except where this host describes its own "
                   "hardware (CT-S3 assertion 3)";
        }
    }

    // And the one file that IS allowed to call them decides from the BACKEND
    // that produced the frames, never from a product string, a model name or a
    // role.  The three are named here so that adding one is a deliberate act.
    const std::string path = std::string(PP_PPCP_SRC_DIR) + "/ppcp_source_declaration.cpp";
    std::ifstream in(path);
    ASSERT_TRUE(in.good()) << path;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    for (const char *forbidden : { "modelName ==", "vendorName ==", "PPCP_ROLE_CAPTURE",
                                   "product.model", "product.vendor" }) {
        EXPECT_EQ(text.find(forbidden), std::string::npos)
            << "the declaration builder consulted `" << forbidden
            << "` — a convention derived from a product or a role is CT-S3 assertion 3";
    }
#endif
}
