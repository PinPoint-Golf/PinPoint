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

#include <QObject>
#include <QString>
#include <QVariantList>

// Read-only "about the running application" facade exposed to QML as the
// `appInfo` context property. Surfaces the app version / build stats, the host
// OS, and the versions of the third-party libraries this build links against —
// everything the About box (src/Gui/shell/PpAboutDialog.qml) renders.
//
// It deliberately depends only on version.h + the CMake-generated pp_version.h /
// pp_deps.h headers and QSysInfo/qVersion(), so it pulls in none of the heavy
// third-party headers (OpenCV, ONNX Runtime, FFmpeg, Spinnaker, …) — keeping it
// cheap to compile and free of link-order / delay-load hazards. Library presence
// is gated by the existing HAVE_* / WITH_* compile defines; only libraries
// actually compiled into this build appear in `dependencies`.
class AppInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString versionString READ versionString CONSTANT)   // "v0.1-alpha10"
    Q_PROPERTY(QString buildNumber   READ buildNumber   CONSTANT)   // "10010"
    Q_PROPERTY(QString gitSha        READ gitSha        CONSTANT)   // short sha / "unknown"
    Q_PROPERTY(QString buildDate     READ buildDate     CONSTANT)   // compile date of this TU
    Q_PROPERTY(QString osName        READ osName        CONSTANT)   // QSysInfo::prettyProductName()
    Q_PROPERTY(QString architecture  READ architecture  CONSTANT)   // e.g. "x86_64" / "arm64"
    Q_PROPERTY(QString qtVersion     READ qtVersion     CONSTANT)   // actually-linked Qt (qVersion())
    Q_PROPERTY(QString iconSource    READ iconSource    CONSTANT)   // qrc path for the app icon
    // False only when configured with -DPP_SHIPPING_BUILD=ON — the distributable
    // artefact. Gates dev-only affordances (the Main.qml --probe-qml hook) so they
    // are compiled out of anything handed to a coach.
    Q_PROPERTY(bool    devBuild      READ devBuild      CONSTANT)
    // Ordered list of { "name": <lib>, "version": <string> } maps — the About box
    // renders one row per entry. Built once in the constructor.
    Q_PROPERTY(QVariantList dependencies READ dependencies CONSTANT)

public:
    explicit AppInfo(QObject *parent = nullptr);

    QString versionString() const;
    QString buildNumber() const;
    QString gitSha() const;
    QString buildDate() const;
    QString osName() const;
    QString architecture() const;
    QString qtVersion() const;
    QString iconSource() const;
    bool devBuild() const
    {
#ifdef PP_SHIPPING_BUILD
        return false;
#else
        return true;
#endif
    }
    QVariantList dependencies() const { return m_dependencies; }

private:
    QVariantList m_dependencies;
};
