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

#ifndef PP_COLORSPACE_PIN_H
#define PP_COLORSPACE_PIN_H

namespace PinPointColour {

// Holds one permanent reference to each colour space CoreGraphics interns for
// the ICC profiles Qt hands it, so a Qt refcount bug cannot free one under a
// CGImageCreate() that is about to use it.  A no-op off macOS.  See the .cpp
// for the defect, the disassembly and the measurements.
//
// ⚠ CALL IT EARLY — before anything can convert a QImage to a CGImage.  It needs
// no QGuiApplication.
void pinNativeColourSpaces();

} // namespace PinPointColour

#endif // PP_COLORSPACE_PIN_H
