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
// suites are C++ model tests that pull in nothing of the app; this one asserts a QML component's
// behaviour, so it needs the app's QML module — and it DECLARES that module rather than borrowing
// it (src/Gui/tests/CMakeLists.txt, from the lists in cmake/PinPointQmlModule.cmake). It used to
// live in the app build for want of any other way to reach the module, which meant it could only
// run after whisper, FFmpeg, OpenCV and espeak had built, and so it ran in no release gate at all.
//
// Two things the setup still has to supply, and they are why this is a binary rather than a bare
// qmltestrunner invocation:
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

// AppSettings only. ChartMetrics and TimelineLabels were included to register them by hand; the
// module registers its own types now, so naming them here would claim a dependency this file
// does not have.
#include "app_settings.h"

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

        // NO addImportPath, AND NO qmlRegisterType. Both were needed while this binary borrowed a
        // staged copy of the app's module: the copy carried .qml and a qmldir but none of the C++,
        // so ChartMetrics and TimelineLabels had to be registered here by hand and everything else
        // in the module stayed "unavailable" — which is why the chart family had UI coverage and
        // the rest of the app's types had none.
        //
        // The module is now built into this target from the same PP_QML_SOURCES the app uses, so
        // every QML_ELEMENT type registers itself exactly as it does in the app, and the import
        // resolves out of this binary's own resources. A component needing another type needs
        // nothing added here.
    }

private:
    // Cleaned up with the process. Nothing here is meant to survive a run: the point is that each
    // one starts from the defaults compiled into AppSettings.
    QTemporaryDir m_scratch;
    AppSettings  *m_settings = nullptr;
};

QUICK_TEST_MAIN_WITH_SETUP(qml_ui, QmlUiTestSetup)

#include "qml_ui_test.moc"
