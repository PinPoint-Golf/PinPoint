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

#include "app_info.h"

#include <QSysInfo>
#include <QVariantMap>
#include <QtGlobal>

#include "version.h"      // PINPOINT_VERSION_STRING / _BUILD_STRING  (src/Core)
#include "pp_version.h"   // PP_GIT_SHA                               (generated)
#include "pp_deps.h"      // PP_<LIB>_VERSION                         (generated)

namespace {

// Append { name, version } iff `version` is non-empty. Keeps the About list free
// of blank rows when an optional dependency resolved without a version string.
void addDep(QVariantList &list, const QString &name, const QString &version,
            const QString &fallback = QString())
{
    QString v = version.trimmed();
    if (v.isEmpty())
        v = fallback;                 // e.g. Spinnaker exposes no configure-time version
    if (v.isEmpty())
        return;
    QVariantMap m;
    m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("version"), v);
    list.append(m);
}

} // namespace

AppInfo::AppInfo(QObject *parent)
    : QObject(parent)
{
    // Ordered most-recognisable first. Each optional library is gated by the same
    // HAVE_* / WITH_* compile define the rest of the codebase uses, so only what is
    // actually linked into this build shows up. Version strings come from the
    // CMake-generated pp_deps.h (captured at configure time); Qt reports its real
    // linked version at runtime.
    addDep(m_dependencies, QStringLiteral("Qt"), QString::fromLatin1(qVersion()));

#ifdef HAVE_OPENCV
    addDep(m_dependencies, QStringLiteral("OpenCV"), QStringLiteral(PP_OPENCV_VERSION));
#endif
#ifdef HAVE_ONNXRUNTIME
    addDep(m_dependencies, QStringLiteral("ONNX Runtime"), QStringLiteral(PP_ORT_VERSION));
#endif
#ifdef HAVE_ORTGENAI
    addDep(m_dependencies, QStringLiteral("ONNX Runtime GenAI"), QStringLiteral(PP_ORTGENAI_VERSION));
#endif
#ifdef HAVE_FFMPEG
    addDep(m_dependencies, QStringLiteral("FFmpeg (libavcodec)"), QStringLiteral(PP_FFMPEG_VERSION));
#endif
    // whisper.cpp, Eigen and libsamplerate are fetched + linked unconditionally.
    addDep(m_dependencies, QStringLiteral("whisper.cpp"), QStringLiteral(PP_WHISPER_VERSION));
    addDep(m_dependencies, QStringLiteral("Eigen"), QStringLiteral(PP_EIGEN_VERSION));
    addDep(m_dependencies, QStringLiteral("libsamplerate"), QStringLiteral(PP_SAMPLERATE_VERSION));
#ifdef HAVE_WRIST
    addDep(m_dependencies, QStringLiteral("libwrist"), QStringLiteral(PP_LIBWRIST_VERSION));
#endif
#ifdef HAVE_PPCP
    addDep(m_dependencies, QStringLiteral("libppcp"), QStringLiteral(PP_LIBPPCP_VERSION));
#endif
#ifdef HAVE_ESPEAK_NG
    addDep(m_dependencies, QStringLiteral("espeak-ng"), QStringLiteral(PP_ESPEAK_VERSION));
#endif
#ifdef HAVE_ARAVIS
    addDep(m_dependencies, QStringLiteral("Aravis"), QStringLiteral(PP_ARAVIS_VERSION));
#endif
#ifdef HAVE_SPINNAKER
    addDep(m_dependencies, QStringLiteral("Spinnaker SDK"), QStringLiteral(PP_SPINNAKER_VERSION),
           /*fallback*/ QStringLiteral("installed"));
#endif
#ifdef WITH_CUDA
    addDep(m_dependencies, QStringLiteral("CUDA Toolkit"), QStringLiteral(PP_CUDA_VERSION),
           /*fallback*/ QStringLiteral("enabled"));
#endif
    // Vulkan is a whisper GPU backend only — no C++ define, so surface it purely from
    // whether CMake's FindVulkan resolved a version.
    addDep(m_dependencies, QStringLiteral("Vulkan"), QStringLiteral(PP_VULKAN_VERSION));
#ifdef HAVE_WINSPARKLE
    addDep(m_dependencies, QStringLiteral("WinSparkle"), QStringLiteral(PP_WINSPARKLE_VERSION));
#endif
#ifdef HAVE_SPARKLE
    addDep(m_dependencies, QStringLiteral("Sparkle"), QStringLiteral(PP_SPARKLE_VERSION));
#endif
}

QString AppInfo::versionString() const
{
    return QStringLiteral(PINPOINT_VERSION_STRING);
}

QString AppInfo::buildNumber() const
{
    return QStringLiteral(PINPOINT_VERSION_BUILD_STRING);
}

QString AppInfo::gitSha() const
{
    return QStringLiteral(PP_GIT_SHA);
}

QString AppInfo::buildDate() const
{
    // Compile date of this translation unit — recompiled on every build, so it
    // tracks "when this binary was built" closely enough for an About box.
    return QString::fromLatin1(__DATE__);
}

QString AppInfo::osName() const
{
    return QSysInfo::prettyProductName();
}

QString AppInfo::architecture() const
{
    return QSysInfo::currentCpuArchitecture();
}

QString AppInfo::qtVersion() const
{
    return QString::fromLatin1(qVersion());
}

QString AppInfo::iconSource() const
{
    // 512px PNG compiled into the app_icons Qt resource (BASE "src/Resources/icons").
    return QStringLiteral("qrc:/icons/pinpointstudio_512.png");
}
