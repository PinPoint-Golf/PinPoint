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

// ── Working around a Qt refcount bug that crashed the application ────────────
//
// 31 August 2026, alpha11 on macOS 26: PinPointStudio died on the main thread
// while the mouse moved over the camera preview's crop overlay.  The stack is a
// cursor shape change — `QQuickItem::setCursor` → libqcocoa → `QImage::toCGImage`
// → `CGImageCreate` → a POINTER-AUTHENTICATION TRAP in `__CF_IS_OBJC` validating
// the image's colour space.  A PAC failure on a CFType's isa means that object
// was already freed.
//
// ⛔ THE CURSOR IS THE VICTIM, NOT THE CAUSE.  In Qt 6.11.1's shipped QtGui,
// `qt_mac_cgImageFormatForImage()` builds the colour space and then drops its
// only reference to it (arm64, offsets from the framework's __text):
//
//     0x3cb600  bl   _CGColorSpaceCreateWithICCData   ; or ...CreateWithName
//     0x3cb648  str  x20, [x19, #0x8]                 ; stored in the out struct
//     0x3cb65c  strb w8,  [x19, #0x28]                ; ok = true
//     0x3cb668  bl   _CFRelease                       ; ...and released anyway
//
// There is no CFRetain anywhere in that function.  `QImage::toCGImage()` then
// passes that pointer to `CGImageCreate`, which retains it — one call too late.
// Between the release and the retain the object is kept alive only by
// CoreGraphics' own cache, and losing that race is the crash.
//
// ⚠ MEASURED, NOT INFERRED.  On this machine:
//   • `CGColorSpaceCreateWithName(kCGColorSpaceSRGB)` has retain count
//     0xFFFFFFFF — an immortal constant, so an image with NO colour space takes
//     a branch the bug cannot hurt.
//   • The ICC branch is different: two calls with the same profile return the
//     SAME object (CoreGraphics interns by profile bytes) with the cache holding
//     one reference, which is the only reason this is a rare crash rather than
//     an immediate one.
//   • One extra reference, never given back, makes it immortal too: 1000
//     `toCGImage()` calls afterwards all used the same pinned object.
//   • A PNG round trip — which is what Qt's own cursor bitmaps are — lands on
//     the pinned object as well, for both sRGB and Display P3.
//
// So this pins the named colour spaces at startup: a handful of objects, a few
// kilobytes of ICC data, held for the life of the process.  It does not fix the
// bug, and anything carrying a profile NOT pinned here still runs the race — but
// it removes the exposure for every image Qt converts in this application,
// whose cursors, icons and drag pixmaps are its own sRGB resources.
//
// ⚠ THE LEAK IS THE MECHANISM AND MUST NOT BE "FIXED".  A future reader tidying
// these into a QCFType, or releasing them at shutdown, restores exactly the
// window this closes.
//
// Reported upstream; remove this once Qt balances that function.

#include "pp_colorspace_pin.h"

#include "pp_debug.h"

#include <QtGlobal>

#ifdef Q_OS_MACOS
#include <QByteArray>
#include <QColorSpace>
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace PinPointColour {

void pinNativeColourSpaces()
{
#ifdef Q_OS_MACOS
    // Every named space Qt can attach to an image.  All eight carry an ICC
    // profile in Qt 6.11.1, and CoreGraphics accepts six of them: it REFUSES
    // Bt2100Pq and Bt2100Hlg (measured), whose transfer functions ICC cannot
    // express.  Those two are not a gap in the cover — a profile CoreGraphics
    // will not turn into a colour space is one `toCGImage()` cannot hand to
    // `CGImageCreate` either: it passes null, and CGImageCreate refuses to build
    // an image rather than freeing anything.  So the startup line says six.
    //
    // An unknown or unsupported entry simply yields nothing and is skipped, which
    // keeps this list safe to carry forward across Qt versions.
    static const QColorSpace::NamedColorSpace kNamed[] = {
        QColorSpace::SRgb,
        QColorSpace::SRgbLinear,
        QColorSpace::AdobeRgb,
        QColorSpace::DisplayP3,
        QColorSpace::ProPhotoRgb,
        QColorSpace::Bt2020,
        QColorSpace::Bt2100Pq,
        QColorSpace::Bt2100Hlg,
    };

    int pinned = 0;
    for (const QColorSpace::NamedColorSpace named : kNamed) {
        const QColorSpace cs(named);
        if (!cs.isValid()) continue;
        const QByteArray icc = cs.iccProfile();
        if (icc.isEmpty()) continue;

        CFDataRef data = icc.toCFData();
        if (!data) continue;
        // ⚠ DELIBERATELY NEVER RELEASED — see the note at the top of this file.
        // CoreGraphics interns by profile bytes, so this is the very object Qt
        // will hand to CGImageCreate for any image carrying this profile.
        if (CGColorSpaceCreateWithICCData(data)) ++pinned;
        CFRelease(data);
    }

    // ⚠ ONE LINE, AND IT EARNS ITS PLACE.  A workaround whose only symptom when
    // it is working is a crash that does not happen is a workaround nobody can
    // confirm is live — in a dev build, in a shipped one, or in a log exported
    // from a range.  This says it is.
    ppWarn() << "[cg] pinned" << pinned
             << "colour space(s) against QTBUG (QImage::toCGImage releases the "
                "colour space it hands to CGImageCreate)";
#endif
}

} // namespace PinPointColour
