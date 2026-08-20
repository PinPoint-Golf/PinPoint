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

#pragma once

// Where a bundled ONNX model actually is, for an executable that may or may not
// be an app bundle.
//
// THE BUG THIS EXISTS TO FIX. Every model consumer used to resolve its file as
// applicationDirPath() + "/../Resources/models/" on macOS — the layout inside
// PinPointStudio.app/Contents/MacOS. That is right for the app and wrong for
// every bare executable we ship beside it: for build/<dir>/swinglab_run it
// resolves to build/Resources/models/, which does not exist, while the model
// CMake downloaded sits in build/<dir>/_deps/<subdir>/ the whole time.
//
// The consequence was not a crash. PoseStage is documented to degrade to an
// empty track rather than fail the analysis, every downstream camera stage gates
// on a non-empty pose track, and the "model not found" line went to PpMessageLog
// — which only the GUI's resource monitor drains. So swinglab_run reported
// "write-back ok" with a plausible score on a swing it had never looked at, and
// the shortfall showed only as a metric count of 5 instead of 44. On Linux and
// Windows the non-bundle branch happens to be right, which is why this was only
// ever a macOS-tools failure.
//
// Order is deliberate: the SHIPPING layouts answer first, so an installed build
// can never be diverted by a stale build tree that happens to sit beside it. The
// build-tree location is the last resort and exists for developer tools only.
//
// When nothing exists the FIRST candidate comes back, not an empty string, so a
// caller's "model not found" message names the place the model was supposed to
// be rather than the last place we happened to look.

#include <QCoreApplication>
#include <QFile>
#include <QString>

namespace pinpoint {

// `file` is the bare file name ("vitpose-b-wholebody.onnx"); `buildSubdir` is the
// _deps directory CMake downloaded it into ("vitpose", "movenet", "segmenter").
inline QString modelFilePath(const QString &file, const QString &buildSubdir)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
#ifdef Q_OS_MACOS
        appDir + QStringLiteral("/../Resources/models/") + file,   // inside the .app
#endif
        appDir + QStringLiteral("/models/") + file,                // installed / non-mac layout
        appDir + QStringLiteral("/_deps/") + buildSubdir + QLatin1Char('/') + file,
    };
    for (const QString &c : candidates)
        if (QFile::exists(c))
            return c;
    return candidates.first();
}

} // namespace pinpoint
