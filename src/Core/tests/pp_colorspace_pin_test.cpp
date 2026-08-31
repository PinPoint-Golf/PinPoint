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

// The colour-space pin, which exists to keep a Qt refcount bug from crashing the
// application — see src/Core/pp_colorspace_pin.cpp for the defect and the
// measurements.  What is asserted here is the ONE property the workaround needs:
// after pinning, the interned CGColorSpace for a profile Qt uses carries a
// reference that is not the caller's and not the CoreGraphics cache's.
//
// ⚠ THIS TEST EXISTS BECAUSE THE FIX LOOKS LIKE A LEAK AND IS ONE ON PURPOSE.
// Wrapping those objects in a QCFType, or releasing them at shutdown, restores
// exactly the window the pin closes and would otherwise pass review.

#include "pp_colorspace_pin.h"

#include <gtest/gtest.h>

#include <QtGlobal>

#ifdef Q_OS_MACOS
#include <QByteArray>
#include <QColorSpace>
#include <CoreGraphics/CoreGraphics.h>

namespace {

// The object CoreGraphics interns for a named space's ICC profile — the very one
// `qt_mac_cgImageFormatForImage()` hands to CGImageCreate for any image carrying
// it.  Returns a reference the caller owns.
CGColorSpaceRef internedFor(QColorSpace::NamedColorSpace named)
{
    const QColorSpace cs(named);
    if (!cs.isValid()) return nullptr;
    const QByteArray icc = cs.iccProfile();
    if (icc.isEmpty()) return nullptr;
    CFDataRef data = icc.toCFData();
    if (!data) return nullptr;
    CGColorSpaceRef out = CGColorSpaceCreateWithICCData(data);
    CFRelease(data);
    return out;
}

} // namespace

TEST(ColourSpacePin, AProfileQtUsesIsHeldBySomebodyOtherThanUsAndTheCache)
{
    PinPointColour::pinNativeColourSpaces();

    CGColorSpaceRef sRgb = internedFor(QColorSpace::SRgb);
    ASSERT_NE(sRgb, nullptr) << "no ICC profile for sRGB — the pin cannot work";

    // cache + pin + the reference we are holding.  Without the pin this is 2,
    // which is the state in which Qt's unbalanced release can reach zero.
    const CFIndex held = CFGetRetainCount(sRgb);
    CFRelease(sRgb);
    EXPECT_GE(held, 3)
        << "sRGB is held only by the CoreGraphics cache and this test — the pin "
           "is gone, and QImage::toCGImage() can free a colour space it is about "
           "to hand to CGImageCreate";
}

TEST(ColourSpacePin, TheSameProfileAlwaysYieldsTheSameObject)
{
    // The premise the whole workaround rests on: CoreGraphics interns by profile
    // bytes, so ONE reference covers every image carrying that profile.  If this
    // ever stops holding, pinning stops meaning anything and the note in
    // pp_colorspace_pin.cpp needs revisiting rather than the pin extending.
    CGColorSpaceRef a = internedFor(QColorSpace::SRgb);
    CGColorSpaceRef b = internedFor(QColorSpace::SRgb);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a, b) << "CoreGraphics no longer interns colour spaces by ICC data";
    CFRelease(a);
    CFRelease(b);
}

TEST(ColourSpacePin, PinningTwiceIsHarmless)
{
    // main() calls it once; a second call must not be a second leak per profile
    // nor a crash.  (CoreGraphics returns the interned object, so this simply
    // adds one more reference to something already immortal for this process.)
    PinPointColour::pinNativeColourSpaces();
    PinPointColour::pinNativeColourSpaces();
    CGColorSpaceRef sRgb = internedFor(QColorSpace::SRgb);
    ASSERT_NE(sRgb, nullptr);
    EXPECT_GE(CFGetRetainCount(sRgb), 3);
    CFRelease(sRgb);
}

#else  // not macOS

TEST(ColourSpacePin, IsANoOpOffMacOs)
{
    PinPointColour::pinNativeColourSpaces();   // must link and do nothing
    SUCCEED();
}

#endif
