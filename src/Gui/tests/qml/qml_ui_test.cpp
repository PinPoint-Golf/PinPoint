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

#include <QtQuickTest>

#include <QDir>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QTemporaryDir>

#include "app_settings.h"
#include "chart_metrics.h"
#include "timeline_labels.h"

class QmlUiTestSetup : public QObject
{
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
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
