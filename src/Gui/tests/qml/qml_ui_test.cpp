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

// Driver for the QML UI suite — the tst_*.qml files beside this one.
//
// These are the tests that load a real component, render it offscreen and press it. The other Gui
// suites are C++ model tests and deliberately pull in nothing of the app; this one cannot be, because
// what it asserts is a QML component's behaviour and that component imports the app's QML module. So
// it lives in the app build, where the module already is, rather than under src/Gui/tests/CMakeLists
// with the decoupled ones.
//
// Two things the setup has to supply, and both are the reason this is a binary rather than a bare
// qmltestrunner invocation:
//
//   THE MODULE. Its qmldir prefers a resource path that only the app binary carries, so the build
//   stages a copy with that line stripped and hands the path in as PP_QML_TEST_IMPORTS. See
//   cmake/StripQmlPrefer.cmake for what that trade is.
//
//   appSettings. Theme is a singleton every component under test depends on, and it reads its
//   values off the `appSettings` context property in Component.onCompleted. Without one, Theme
//   half-initialises with a ReferenceError and every test runs against a theme that failed — which
//   would either mask real failures or invent ones. The REAL AppSettings is used rather than a stub,
//   so it cannot drift from what the app hands the same singleton, with its QSettings redirected to
//   a scratch directory first so a test run cannot write to the developer's own preferences.
//
//   THE BUNDLED FONTS, and this suite ran for months without them. See loadBundledFonts() below —
//   a layout test that measures a fallback face is measuring a font the application never renders
//   with, and it reached two different wrong answers on two platforms.

#include <QtQuickTest>

#include <QDir>
#include <QFontDatabase>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QTemporaryDir>

#include "app_settings.h"
#include "chart_metrics.h"
#include "timeline_labels.h"

// ⚠ THE FONTS ARE THE UNITS THIS SUITE MEASURES IN, AND IT USED TO RUN WITHOUT THEM.
//
// Theme.qml names families — "Geist Mono", "Hanken Grotesk" — and notes that it "falls back to the
// system default if the font file is not installed". main.cpp loads the bundled faces at startup
// (QFontDatabase::addApplicationFont over :/fonts) precisely so those names resolve everywhere. This
// binary does not run main.cpp, so it never did.
//
// That is not a cosmetic difference for a suite whose central assertion is "no annotation escapes
// its card": a fallback face has different advance widths, so the text being measured is not the
// text the application draws. And because the fallback is chosen by the HOST, the suite reached two
// different wrong answers — neither this Mac nor the studio PC has Geist Mono installed, macOS fell
// back to something narrow and passed, Windows fell back to something wider and failed four
// containment assertions in tst_lm_graphics. The failures were real; the cause was here, not in
// PpLmGraphicsBody.
//
// ENUMERATED, NOT LISTED. The faces come from the same cmake/PinPointFonts.cmake list the app
// compiles into :/fonts, and reading the directory back means adding a face does not require
// remembering this file. A missing directory is a hard failure rather than a warning: silently
// measuring the wrong font is the exact failure being fixed, so it must never be the quiet path.
static void loadBundledFonts()
{
    const QDir dir(QStringLiteral(":/fonts"));
    const QStringList faces = dir.entryList({ QStringLiteral("*.ttf") }, QDir::Files, QDir::Name);
    if (faces.isEmpty())
        qFatal("qml_ui_test: no fonts under :/fonts — the layout assertions would measure a "
               "host-chosen fallback face rather than the faces the app ships. Check that the "
               "target compiles in the app_fonts resource (cmake/PinPointFonts.cmake).");

    for (const QString &face : faces) {
        if (QFontDatabase::addApplicationFont(dir.filePath(face)) < 0)
            qFatal("qml_ui_test: failed to load bundled font %s", qPrintable(face));
    }
}

class QmlUiTestSetup : public QObject
{
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        // Before anything renders — a face registered after a component has laid out does not
        // re-measure it.
        loadBundledFonts();

        // Redirected BEFORE the first AppSettings is constructed — its constructor reads every key
        // through ppSettings(), and QSettings::setPath only affects instances made after it. A test
        // that quietly rewrote ui/themeIndex in the developer's own ini would be a poor trade for
        // the convenience of using the real class.
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_scratch.path());

        m_settings = new AppSettings(this);
        engine->rootContext()->setContextProperty(QStringLiteral("appSettings"), m_settings);
        engine->addImportPath(QStringLiteral(PP_QML_TEST_IMPORTS));

        // The staged module is .qml files and a qmldir; the module's C++ QML_ELEMENT types are
        // registered by the APP binary, which this target deliberately does not link. So a
        // component that declares one — PpMetricChart and the whole chart family declare
        // ChartMetrics and TimelineLabels — was simply "unavailable" here, which is why the chart
        // had no UI coverage at all rather than failing coverage.
        //
        // Registering them by hand under the same URI is the smallest thing that opens those
        // components up. It is not a stub: these are the real classes, compiled from the real
        // sources, so a test can only pass here for the reason it would pass in the app. Add to
        // this list (and to the target's sources) when a component under test needs another type.
        qmlRegisterType<ChartMetrics>("PinPointStudio", 1, 0, "ChartMetrics");
        qmlRegisterType<TimelineLabels>("PinPointStudio", 1, 0, "TimelineLabels");
    }

private:
    // Cleaned up with the process. Nothing here is meant to survive a run: the point is that each
    // one starts from the defaults compiled into AppSettings.
    QTemporaryDir m_scratch;
    AppSettings  *m_settings = nullptr;
};

QUICK_TEST_MAIN_WITH_SETUP(qml_ui, QmlUiTestSetup)

#include "qml_ui_test.moc"
