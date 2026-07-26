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
#include <QStringList>
#include <QVariantMap>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QUrl>
#include <QRegularExpression>
#include "pp_settings.h"
#include "version.h"

// Must be declared outside AppSettings — moc does not support Q_GADGET in nested classes.
struct StorageInfo {
    Q_GADGET
    Q_PROPERTY(qint64  totalBytes   MEMBER totalBytes)
    Q_PROPERTY(qint64  freeBytes    MEMBER freeBytes)
    Q_PROPERTY(qint64  sessionBytes MEMBER sessionBytes)
    Q_PROPERTY(QString volumeName   MEMBER volumeName)
public:
    qint64  totalBytes   = 0;
    qint64  freeBytes    = 0;
    qint64  sessionBytes = 0;
    QString volumeName;
};
Q_DECLARE_METATYPE(StorageInfo)

class AppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString appVersion    READ appVersion    CONSTANT)
    Q_PROPERTY(int     themeIndex   READ themeIndex   WRITE setThemeIndex   NOTIFY themeIndexChanged)
    Q_PROPERTY(int     windowWidth  READ windowWidth  WRITE setWindowWidth  NOTIFY windowWidthChanged)
    Q_PROPERTY(int     windowHeight READ windowHeight WRITE setWindowHeight NOTIFY windowHeightChanged)
    Q_PROPERTY(double  fontScale    READ fontScale    WRITE setFontScale    NOTIFY fontScaleChanged)
    Q_PROPERTY(QString density      READ density      WRITE setDensity      NOTIFY densityChanged)
    Q_PROPERTY(QString timelineOrientation READ timelineOrientation WRITE setTimelineOrientation NOTIFY timelineOrientationChanged)
    // The on-disk swing_NNNN dir designated as the wrist diagnostics "compare to reference" swing.
    Q_PROPERTY(QString wristReferenceSwingDir READ wristReferenceSwingDir WRITE setWristReferenceSwingDir NOTIFY wristReferenceSwingDirChanged)
    Q_PROPERTY(bool    timelineSnapToPhases READ timelineSnapToPhases WRITE setTimelineSnapToPhases NOTIFY timelineSnapToPhasesChanged)
    // Metric catalogue (Settings → Metrics) directory filter: hide roadmap-
    // placeholder (PLANNED) metrics. A view preference, persisted in the ui/ group.
    Q_PROPERTY(bool    metricsHidePlanned READ metricsHidePlanned WRITE setMetricsHidePlanned NOTIFY metricsHidePlannedChanged)
    // Diagnostics grade policy (Settings -> Diagnostics -> Measures & norms), by NAME:
    // "lenient" | "standard" | "strict". Applied PACK-WIDE and never per norm or per context — if
    // "Ideal" meant one thing in one pack and something else in another, no grade would be
    // comparable across athletes or across shared packs. The names, and the z thresholds behind
    // them, live in norm_model.cpp; this stores only which one is chosen.
    Q_PROPERTY(QString diagnosticsGradePolicy READ diagnosticsGradePolicy WRITE setDiagnosticsGradePolicy NOTIFY diagnosticsGradePolicyChanged)
    // Global replay behaviour (surfaced in the View menu alongside the timeline
    // options, not per-mode): whether a just-captured shot auto-replays, and
    // whether replay playback is trimmed to the detected swing (Address → Finish).
    Q_PROPERTY(bool    autoReplayAfterCapture READ autoReplayAfterCapture WRITE setAutoReplayAfterCapture NOTIFY autoReplayAfterCaptureChanged)
    Q_PROPERTY(bool    replayTrimToSwing    READ replayTrimToSwing    WRITE setReplayTrimToSwing    NOTIFY replayTrimToSwingChanged)
    Q_PROPERTY(bool    reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged)
    Q_PROPERTY(bool    gradientTitles READ gradientTitles WRITE setGradientTitles NOTIFY gradientTitlesChanged)
    Q_PROPERTY(double  overlayOpacity  READ overlayOpacity  WRITE setOverlayOpacity  NOTIFY overlayOpacityChanged)
    Q_PROPERTY(bool    windowMaximized READ windowMaximized WRITE setWindowMaximized NOTIFY windowMaximizedChanged)
    Q_PROPERTY(int     windowX         READ windowX         WRITE setWindowX         NOTIFY windowXChanged)
    Q_PROPERTY(int     windowY         READ windowY         WRITE setWindowY         NOTIFY windowYChanged)
    Q_PROPERTY(QString language                    READ language                    WRITE setLanguage                    NOTIFY languageChanged)
    Q_PROPERTY(QString units                       READ units                       WRITE setUnits                       NOTIFY unitsChanged)
    Q_PROPERTY(QString athleteLibraryPath          READ athleteLibraryPath          WRITE setAthleteLibraryPath          NOTIFY athleteLibraryPathChanged)
    Q_PROPERTY(bool    autoSaveSession             READ autoSaveSession             WRITE setAutoSaveSession             NOTIFY autoSaveSessionChanged)
    Q_PROPERTY(bool    autoDetectSwing             READ autoDetectSwing             WRITE setAutoDetectSwing             NOTIFY autoDetectSwingChanged)
    Q_PROPERTY(QString swingDetectionSensitivity   READ swingDetectionSensitivity   WRITE setSwingDetectionSensitivity   NOTIFY swingDetectionSensitivityChanged)
    Q_PROPERTY(QString motionCaptureQuality        READ motionCaptureQuality        WRITE setMotionCaptureQuality        NOTIFY motionCaptureQualityChanged)
    Q_PROPERTY(int     audioDeviceLatencyUs        READ audioDeviceLatencyUs        WRITE setAudioDeviceLatencyUs        NOTIFY audioDeviceLatencyUsChanged)
    Q_PROPERTY(QString audioInputDevice            READ audioInputDevice            WRITE setAudioInputDevice            NOTIFY audioInputDeviceChanged)
    Q_PROPERTY(bool    acousticShotDetectionEnabled READ acousticShotDetectionEnabled WRITE setAcousticShotDetectionEnabled NOTIFY acousticShotDetectionEnabledChanged)
    Q_PROPERTY(double  acousticSensitivity         READ acousticSensitivity         WRITE setAcousticSensitivity         NOTIFY acousticSensitivityChanged)
    Q_PROPERTY(bool    aiCoachingOnSessionEnd      READ aiCoachingOnSessionEnd      WRITE setAiCoachingOnSessionEnd      NOTIFY aiCoachingOnSessionEndChanged)
    // Force cloud fallback for each AI subsystem even when a local GPU is present
    // (user preference; the relevant API key must be configured). See the
    // controllers' applyCloudFallbackPref() slots and GeneralPanel.qml.
    Q_PROPERTY(bool    cloudFallbackStt            READ cloudFallbackStt            WRITE setCloudFallbackStt            NOTIFY cloudFallbackSttChanged)
    Q_PROPERTY(bool    cloudFallbackTts            READ cloudFallbackTts            WRITE setCloudFallbackTts            NOTIFY cloudFallbackTtsChanged)
    Q_PROPERTY(bool    cloudFallbackLlm            READ cloudFallbackLlm            WRITE setCloudFallbackLlm            NOTIFY cloudFallbackLlmChanged)
    Q_PROPERTY(bool    checkForUpdates             READ checkForUpdates             WRITE setCheckForUpdates             NOTIFY checkForUpdatesChanged)
    Q_PROPERTY(QString skippedUpdateVersion        READ skippedUpdateVersion        WRITE setSkippedUpdateVersion        NOTIFY skippedUpdateVersionChanged)
    Q_PROPERTY(bool    sendDiagnostics             READ sendDiagnostics             WRITE setSendDiagnostics             NOTIFY sendDiagnosticsChanged)
    Q_PROPERTY(QString mainDisplayMode             READ mainDisplayMode             WRITE setMainDisplayMode             NOTIFY mainDisplayModeChanged)
    Q_PROPERTY(bool    rememberWindowGeometry      READ rememberWindowGeometry      WRITE setRememberWindowGeometry      NOTIFY rememberWindowGeometryChanged)
    Q_PROPERTY(QString secondaryDisplayMode        READ secondaryDisplayMode        WRITE setSecondaryDisplayMode        NOTIFY secondaryDisplayModeChanged)
    Q_PROPERTY(QString postShotContent             READ postShotContent             WRITE setPostShotContent             NOTIFY postShotContentChanged)
    Q_PROPERTY(double  postShotDelay               READ postShotDelay               WRITE setPostShotDelay               NOTIFY postShotDelayChanged)
    Q_PROPERTY(bool    postShotMirror              READ postShotMirror              WRITE setPostShotMirror              NOTIFY postShotMirrorChanged)
    // How the post-shot dashboard is surfaced on the target display: "panel"
    // (persistent in-app stage panel), "window" (auto-closing overlay window on
    // the secondary screen), or "kiosk" (persistent full-screen). Default "panel".
    Q_PROPERTY(QString postShotDisplayMode         READ postShotDisplayMode         WRITE setPostShotDisplayMode         NOTIFY postShotDisplayModeChanged)
    // Seconds the post-shot WINDOW stays before auto-closing (only meaningful when
    // postShotDisplayMode == "window"). Distinct from postShotDelay (the pre-show delay).
    Q_PROPERTY(double  postShotDwell               READ postShotDwell               WRITE setPostShotDwell               NOTIFY postShotDwellChanged)
    // Dashboard content scale on the cast surface: "small" | "medium" | "large".
    // Purely a viewing-distance preference — a 13" laptop mirrored at arm's length
    // and a 65" TV across a bay want very different type, and neither is "correct".
    // The window turns this into a uniform zoom (PpDashboardWindow.contentScale), so
    // proportions are identical at every setting. Default "medium".
    Q_PROPERTY(QString dashboardScale              READ dashboardScale              WRITE setDashboardScale              NOTIFY dashboardScaleChanged)
    Q_PROPERTY(QString uiFrameRateCap              READ uiFrameRateCap              WRITE setUiFrameRateCap              NOTIFY uiFrameRateCapChanged)
    Q_PROPERTY(bool    hardwareAcceleration        READ hardwareAcceleration        WRITE setHardwareAcceleration        NOTIFY hardwareAccelerationChanged)
    Q_PROPERTY(QStringList cameraExcluded  READ cameraExcluded  WRITE setCameraExcluded  NOTIFY cameraExcludedChanged)
    Q_PROPERTY(QVariantMap cameraTargetFps READ cameraTargetFps WRITE setCameraTargetFps NOTIFY cameraTargetFpsChanged)
    Q_PROPERTY(QVariantMap cameraTriggerMode READ cameraTriggerMode WRITE setCameraTriggerMode NOTIFY cameraTriggerModeChanged)
    Q_PROPERTY(QVariantMap cameraRoi         READ cameraRoi         WRITE setCameraRoi         NOTIFY cameraRoiChanged)
    Q_PROPERTY(QVariantMap cameraPerspective READ cameraPerspective WRITE setCameraPerspective NOTIFY cameraPerspectiveChanged)
    Q_PROPERTY(QVariantMap cameraIsMirrored  READ cameraIsMirrored  WRITE setCameraIsMirrored  NOTIFY cameraIsMirroredChanged)
    Q_PROPERTY(QVariantMap cameraBallRoi     READ cameraBallRoi     WRITE setCameraBallRoi     NOTIFY cameraBallRoiChanged)
    Q_PROPERTY(double      cameraPreroll       READ cameraPreroll       WRITE setCameraPreroll       NOTIFY cameraPrerollChanged)
    Q_PROPERTY(bool        cameraSyncEnabled   READ cameraSyncEnabled   WRITE setCameraSyncEnabled   NOTIFY cameraSyncEnabledChanged)
    Q_PROPERTY(QVariantMap cameraFixedInPlace  READ cameraFixedInPlace  WRITE setCameraFixedInPlace  NOTIFY cameraFixedInPlaceChanged)
    Q_PROPERTY(QVariantMap cameraAlias         READ cameraAlias         WRITE setCameraAlias         NOTIFY cameraAliasChanged)
    Q_PROPERTY(QStringList imuExcluded            READ imuExcluded            WRITE setImuExcluded            NOTIFY imuExcludedChanged)
    Q_PROPERTY(QVariantMap imuAlias               READ imuAlias               WRITE setImuAlias               NOTIFY imuAliasChanged)
    Q_PROPERTY(QVariantMap imuCalibration         READ imuCalibration         WRITE setImuCalibration         NOTIFY imuCalibrationChanged)
    Q_PROPERTY(QVariantMap imuPlacement           READ imuPlacement           WRITE setImuPlacement           NOTIFY imuPlacementChanged)
    Q_PROPERTY(QVariantMap imuOutputRateHz        READ imuOutputRateHz        WRITE setImuOutputRateHz        NOTIFY imuOutputRateHzChanged)
    Q_PROPERTY(QVariantMap imuFusionMode          READ imuFusionMode          WRITE setImuFusionMode          NOTIFY imuFusionModeChanged)
    Q_PROPERTY(QVariantMap imuMountOrientation    READ imuMountOrientation    WRITE setImuMountOrientation    NOTIFY imuMountOrientationChanged)
    Q_PROPERTY(bool        imuAutoConnect         READ imuAutoConnect         WRITE setImuAutoConnect         NOTIFY imuAutoConnectChanged)
    Q_PROPERTY(bool        imuAutoReconnect       READ imuAutoReconnect       WRITE setImuAutoReconnect       NOTIFY imuAutoReconnectChanged)
    Q_PROPERTY(bool        imuSaveCalibrationToFlash READ imuSaveCalibrationToFlash WRITE setImuSaveCalibrationToFlash NOTIFY imuSaveCalibrationToFlashChanged)
    Q_PROPERTY(QString     imuDefaultFusionMode   READ imuDefaultFusionMode   WRITE setImuDefaultFusionMode   NOTIFY imuDefaultFusionModeChanged)
    // Software orientation-fusion algorithm: "Madgwick" (default) or "ESKF".
    // Distinct from imuFusionMode/imuDefaultFusionMode (the device's 6/9-axis
    // magnetometer mode). See iorientation_filter.h.
    Q_PROPERTY(QString     imuOrientationFilter   READ imuOrientationFilter   WRITE setImuOrientationFilter   NOTIFY imuOrientationFilterChanged)
    Q_PROPERTY(QVariantMap sessionGoalsByType READ sessionGoalsByType WRITE setSessionGoalsByType NOTIFY sessionGoalsByTypeChanged)
    // Session-toolbar View state, persisted per SessionController::Type (key =
    // String(typeInt)). panels = QStringList of enabled panel keys; arrangement =
    // "tabs"|"split"|"stage"; preset = named preset or "Custom". Resolved by the
    // ViewLayout.qml singleton. Distinct from main/secondaryDisplayMode (monitor).
    Q_PROPERTY(QVariantMap viewPanelsByType      READ viewPanelsByType      WRITE setViewPanelsByType      NOTIFY viewPanelsByTypeChanged)
    Q_PROPERTY(QVariantMap viewArrangementByType READ viewArrangementByType WRITE setViewArrangementByType NOTIFY viewArrangementByTypeChanged)
    Q_PROPERTY(QVariantMap viewPresetByType      READ viewPresetByType      WRITE setViewPresetByType      NOTIFY viewPresetByTypeChanged)
    // Session-stage layout, persisted per session MODE (key = String(mode): 0=Capture,
    // 1=Review, 2=Analyse) rather than per session type. Value = { panels: QStringList,
    // arrangement: "tabs"|"split"|"stage" }. Resolved by the ViewLayout.qml singleton;
    // supersedes the per-type view{Panels,Arrangement,Preset}ByType maps above.
    Q_PROPERTY(QVariantMap viewLayoutByMode      READ viewLayoutByMode      WRITE setViewLayoutByMode      NOTIFY viewLayoutByModeChanged)
    // Active data-viewer region preset, persisted per SessionController::Type (key
    // = String(typeInt)) so each mode remembers its default lens. Value is a region
    // name ("Axial"/"Lower"/"Upper"/"Delivery"/"Custom"). Read by PpDataViewer.
    Q_PROPERTY(QVariantMap dataRegionByType      READ dataRegionByType      WRITE setDataRegionByType      NOTIFY dataRegionByTypeChanged)
    // Configurable post-shot dashboard presets, persisted per SessionController::Type
    // (key = String(typeInt)). Value = { active:<presetId|"custom"|"<saved>">,
    // zones:[zoneKey…], perZoneMetrics:{ zoneKey:[metricKey…] },
    // saved:{ "<name>":{zones,perZoneMetrics} } }. Resolved by ViewLayout.qml.
    Q_PROPERTY(QVariantMap dashboardPresetsByType READ dashboardPresetsByType WRITE setDashboardPresetsByType NOTIFY dashboardPresetsByTypeChanged)
    // Collapsed/expanded state of the data-table and chart collapsible sections,
    // persisted per screen+mode. Key = "<sessionTypeInt>:<modeInt>:<section>"
    // (e.g. "1:1:scope"), value = bool (true = collapsed). Read/written by
    // PpDataViewer and PpMetricChart so each screen+mode remembers its layout.
    Q_PROPERTY(QVariantMap sectionCollapse      READ sectionCollapse      WRITE setSectionCollapse      NOTIFY sectionCollapseChanged)
    // Metric-chart display controls + series selections, persisted per screen+mode
    // so Replay and Analyse each remember their own chart. Key =
    // "<sessionTypeInt>:<modeInt>:<field>" (e.g. "1:1:split"); value is a bool for
    // split/dots/cursor or a { seriesKey: bool } visibility map for series. Read/
    // written by PpMetricChart, alongside sectionCollapse.
    Q_PROPERTY(QVariantMap chartPrefs           READ chartPrefs           WRITE setChartPrefs           NOTIFY chartPrefsChanged)
    Q_PROPERTY(int         lastSessionType   READ lastSessionType   WRITE setLastSessionType   NOTIFY lastSessionTypeChanged)

    Q_PROPERTY(QString sessionNamingPattern  READ sessionNamingPattern  WRITE setSessionNamingPattern  NOTIFY sessionNamingPatternChanged)
    Q_PROPERTY(QString videoResolutionMode   READ videoResolutionMode   WRITE setVideoResolutionMode   NOTIFY videoResolutionModeChanged)
    Q_PROPERTY(QString videoCodec            READ videoCodec            WRITE setVideoCodec            NOTIFY videoCodecChanged)
    Q_PROPERTY(QString videoQuality          READ videoQuality          WRITE setVideoQuality          NOTIFY videoQualityChanged)
    Q_PROPERTY(QString videoContainer        READ videoContainer        WRITE setVideoContainer        NOTIFY videoContainerChanged)
    Q_PROPERTY(bool    saveRawFrames         READ saveRawFrames         WRITE setSaveRawFrames         NOTIFY saveRawFramesChanged)
    Q_PROPERTY(bool    skipAnalysisForRawCapture READ skipAnalysisForRawCapture WRITE setSkipAnalysisForRawCapture NOTIFY skipAnalysisForRawCaptureChanged)
    Q_PROPERTY(bool    savePoseKeypoints     READ savePoseKeypoints     WRITE setSavePoseKeypoints     NOTIFY savePoseKeypointsChanged)
    Q_PROPERTY(bool    saveImuStreams        READ saveImuStreams        WRITE setImuStreams            NOTIFY saveImuStreamsChanged)
    Q_PROPERTY(QString imuDataFormat         READ imuDataFormat         WRITE setImuDataFormat         NOTIFY imuDataFormatChanged)
    Q_PROPERTY(bool    saveLaunchMonitorData READ saveLaunchMonitorData WRITE setSaveLaunchMonitorData NOTIFY saveLaunchMonitorDataChanged)
    // Persistent per-athlete·club·camera club-length prior (club_length_fusion.h /
    // plan: robust club length — starry-shimmying-wind). Key = "athleteUuid|clubName|
    // cameraKey" -> {emaPx, varPx, n, disagreeRun, lengthMm, frameW, frameH,
    // lastUpdatedUtc}. Written ONLY by the live ShotProcessor path; re-analysis reads
    // the recorded swing.json copy instead (never AppSettings — determinism).
    Q_PROPERTY(QVariantMap clubLenPrior READ clubLenPrior WRITE setClubLenPrior NOTIFY clubLenPriorChanged)

public:
    Q_INVOKABLE StorageInfo queryStorageInfo() const;

    // Cross-platform conversion of a FolderDialog/FileDialog url to a native local
    // path. QML's JS `url` type has no toLocalFile(), and naively stripping "file://"
    // leaves a stray leading slash before a Windows drive letter ("/C:/...") which
    // QDir rejects. Use these from QML instead of string-replacing the scheme.
    Q_INVOKABLE QString urlToLocalFile(const QUrl &url) const { return url.toLocalFile(); }
    Q_INVOKABLE QString fileUrlFor(const QString &localPath) const
    {
        return QUrl::fromLocalFile(localPath).toString();
    }

    // Repairs a path that came through the old "file://"-stripping code, where a
    // Windows url ("file:///C:/...") became "/C:/..." with a leading slash before the
    // drive letter. Leaves well-formed POSIX and Windows paths untouched.
    static QString normaliseLibraryPath(const QString &raw)
    {
        QString s = raw.trimmed();
        static const QRegularExpression winDrive(QStringLiteral("^/([A-Za-z]:)"));
        s.replace(winDrive, QStringLiteral("\\1"));
        return s;
    }

    explicit AppSettings(QObject *parent = nullptr) : QObject(parent)
    {
        m_themeIndex      = ppSettings().value(QStringLiteral("ui/themeIndex"),      0).toInt();
        m_windowWidth     = ppSettings().value(QStringLiteral("ui/windowWidth"),     1120).toInt();
        m_windowHeight    = ppSettings().value(QStringLiteral("ui/windowHeight"),    700).toInt();
        m_fontScale       = ppSettings().value(QStringLiteral("ui/fontScale"),       -1.0).toDouble();
        m_density         = ppSettings().value(QStringLiteral("ui/density"),         QStringLiteral("default")).toString();
        m_timelineOrientation = ppSettings().value(QStringLiteral("ui/timelineOrientation"), QStringLiteral("horizontal")).toString();
        m_wristReferenceSwingDir = ppSettings().value(QStringLiteral("ui/wristReferenceSwingDir"), QString()).toString();
        m_timelineSnapToPhases = ppSettings().value(QStringLiteral("ui/timelineSnapToPhases"), false).toBool();
        m_metricsHidePlanned = ppSettings().value(QStringLiteral("ui/metricsHidePlanned"), false).toBool();
        m_diagnosticsGradePolicy = ppSettings().value(QStringLiteral("ui/diagnosticsGradePolicy"),
                                                      QStringLiteral("standard")).toString();
        m_autoReplayAfterCapture = ppSettings().value(QStringLiteral("ui/autoReplayAfterCapture"), true).toBool();
        m_replayTrimToSwing = ppSettings().value(QStringLiteral("ui/replayTrimToSwing"), false).toBool();
        m_reduceMotion    = ppSettings().value(QStringLiteral("ui/reduceMotion"),    false).toBool();
        m_gradientTitles  = ppSettings().value(QStringLiteral("ui/gradientTitles"),  true).toBool();
        m_overlayOpacity  = ppSettings().value(QStringLiteral("ui/overlayOpacity"),  0.7).toDouble();
        m_windowMaximized = ppSettings().value(QStringLiteral("ui/windowMaximized"), false).toBool();
        m_windowX         = ppSettings().value(QStringLiteral("ui/windowX"),         -1).toInt();
        m_windowY         = ppSettings().value(QStringLiteral("ui/windowY"),         -1).toInt();

        // NOTE: these keys use the capitalised "General/" group on purpose. QSettings
        // serialises any "General/..." key to the INI's [%General] section (to keep it
        // out of the reserved [General] section used for top-level keys). It always
        // un-escapes [%General] back to the canonical capital "General" on the next
        // launch — so a lowercase "general/..." key is written but read back as
        // "General/...", and never round-trips. Writing "General/..." matches what
        // Qt reads back, so values persist across restarts.
        m_language                  = ppSettings().value(QStringLiteral("General/language"),                  QStringLiteral("en_GB")).toString();
        m_units                     = ppSettings().value(QStringLiteral("General/units"),                     QStringLiteral("mph")).toString();
        m_athleteLibraryPath        = normaliseLibraryPath(ppSettings().value(QStringLiteral("General/athleteLibraryPath"), QStringLiteral("")).toString());
        m_autoSaveSession           = ppSettings().value(QStringLiteral("General/autoSaveSession"),           true).toBool();
        m_autoDetectSwing           = ppSettings().value(QStringLiteral("General/autoDetectSwing"),           true).toBool();
        m_swingDetectionSensitivity = ppSettings().value(QStringLiteral("General/swingDetectionSensitivity"), QStringLiteral("Medium")).toString();
        m_motionCaptureQuality      = ppSettings().value(QStringLiteral("General/motionCaptureQuality"),      QStringLiteral("Medium")).toString();
        // The Low tier was removed 2026-07-13 (it was vestigial — the offline pose
        // pass never used it; ViTPose-B == Medium already). Migrate an old install's
        // stored "Low" forward to "Medium" so it lands on a valid option; write the
        // migrated value straight back so it round-trips even if this getter isn't
        // hit again before the next read (e.g. a tool that only reads QSettings).
        if (m_motionCaptureQuality.compare(QStringLiteral("Low"), Qt::CaseInsensitive) == 0) {
            m_motionCaptureQuality = QStringLiteral("Medium");
            ppSettings().setValue(QStringLiteral("General/motionCaptureQuality"), m_motionCaptureQuality);
        }
        // Mic capture-chain delay used to back-date acoustic shot onsets
        // (shot detection P2); a fixed estimate until P4 auto-calibration.
        m_audioDeviceLatencyUs      = ppSettings().value(QStringLiteral("General/audioDeviceLatencyUs"),      20000).toInt();
        m_audioInputDevice          = ppSettings().value(QStringLiteral("General/audioInputDevice"),          QStringLiteral("")).toString();
        m_acousticShotDetectionEnabled = ppSettings().value(QStringLiteral("General/acousticShotDetectionEnabled"), true).toBool();
        m_acousticSensitivity       = ppSettings().value(QStringLiteral("General/acousticSensitivity"),       0.5).toDouble();
        m_aiCoachingOnSessionEnd    = ppSettings().value(QStringLiteral("General/aiCoachingOnSessionEnd"),    true).toBool();
        m_cloudFallbackStt          = ppSettings().value(QStringLiteral("General/cloudFallbackStt"),          false).toBool();
        m_cloudFallbackTts          = ppSettings().value(QStringLiteral("General/cloudFallbackTts"),          false).toBool();
        m_cloudFallbackLlm          = ppSettings().value(QStringLiteral("General/cloudFallbackLlm"),          false).toBool();
        m_checkForUpdates           = ppSettings().value(QStringLiteral("General/checkForUpdates"),           true).toBool();
        m_skippedUpdateVersion      = ppSettings().value(QStringLiteral("General/skippedUpdateVersion"),      QString()).toString();
        m_sendDiagnostics           = ppSettings().value(QStringLiteral("General/sendDiagnostics"),           false).toBool();

        m_mainDisplayMode        = ppSettings().value(QStringLiteral("display/mainDisplayMode"),        QStringLiteral("primary")).toString();
        m_rememberWindowGeometry = ppSettings().value(QStringLiteral("display/rememberWindowGeometry"), true).toBool();
        m_secondaryDisplayMode   = ppSettings().value(QStringLiteral("display/secondaryDisplayMode"),   QStringLiteral("none")).toString();
        m_postShotContent        = ppSettings().value(QStringLiteral("display/postShotContent"),        QStringLiteral("replay")).toString();
        m_postShotDelay          = ppSettings().value(QStringLiteral("display/postShotDelay"),          0.5).toDouble();
        m_postShotMirror         = ppSettings().value(QStringLiteral("display/postShotMirror"),         false).toBool();
        m_postShotDisplayMode    = ppSettings().value(QStringLiteral("display/postShotDisplayMode"),    QStringLiteral("panel")).toString();
        m_postShotDwell          = ppSettings().value(QStringLiteral("display/postShotDwell"),          8.0).toDouble();
        m_dashboardScale         = ppSettings().value(QStringLiteral("display/dashboardScale"),         QStringLiteral("medium")).toString();
        m_uiFrameRateCap         = ppSettings().value(QStringLiteral("display/uiFrameRateCap"),         QStringLiteral("display")).toString();
        m_hardwareAcceleration   = ppSettings().value(QStringLiteral("display/hardwareAcceleration"),   true).toBool();

        m_cameraExcluded    = ppSettings().value(QStringLiteral("camera/excluded"),    QStringList{}).toStringList();
        m_cameraTargetFps   = ppSettings().value(QStringLiteral("camera/targetFps"),   QVariantMap{}).toMap();
        m_cameraTriggerMode = ppSettings().value(QStringLiteral("camera/triggerMode"), QVariantMap{}).toMap();
        m_cameraRoi         = ppSettings().value(QStringLiteral("camera/roi"),         QVariantMap{}).toMap();
        m_cameraPerspective = ppSettings().value(QStringLiteral("camera/perspective"), QVariantMap{}).toMap();
        m_cameraIsMirrored  = ppSettings().value(QStringLiteral("camera/isMirrored"),  QVariantMap{}).toMap();
        m_cameraBallRoi     = ppSettings().value(QStringLiteral("camera/ballRoi"),     QVariantMap{}).toMap();
        m_cameraPreroll     = ppSettings().value(QStringLiteral("camera/preroll"),     1.0).toDouble();
        m_cameraSyncEnabled  = ppSettings().value(QStringLiteral("camera/syncEnabled"),    true).toBool();
        m_cameraFixedInPlace = ppSettings().value(QStringLiteral("camera/fixedInPlace"), QVariantMap{}).toMap();
        m_cameraAlias        = ppSettings().value(QStringLiteral("camera/alias"),        QVariantMap{}).toMap();

        m_imuExcluded             = ppSettings().value(QStringLiteral("imu/excluded"),             QStringList{}).toStringList();
        m_imuPlacement            = ppSettings().value(QStringLiteral("imu/placement"),            QVariantMap{}).toMap();
        m_imuOutputRateHz         = ppSettings().value(QStringLiteral("imu/outputRateHz"),         QVariantMap{}).toMap();
        m_imuFusionMode           = ppSettings().value(QStringLiteral("imu/fusionMode"),           QVariantMap{}).toMap();
        m_imuMountOrientation     = ppSettings().value(QStringLiteral("imu/mountOrientation"),     QVariantMap{}).toMap();
        m_imuAutoConnect          = ppSettings().value(QStringLiteral("imu/autoConnect"),          true).toBool();
        m_imuAutoReconnect        = ppSettings().value(QStringLiteral("imu/autoReconnect"),        true).toBool();
        m_imuSaveCalibrationToFlash = ppSettings().value(QStringLiteral("imu/saveCalibrationToFlash"), false).toBool();
        m_imuDefaultFusionMode    = ppSettings().value(QStringLiteral("imu/defaultFusionMode"),    QStringLiteral("9axis")).toString();
        m_imuOrientationFilter    = ppSettings().value(QStringLiteral("imu/orientationFilter"),    QStringLiteral("Madgwick")).toString();
        m_imuAlias                = ppSettings().value(QStringLiteral("imu/alias"),                QVariantMap{}).toMap();
        m_imuCalibration          = ppSettings().value(QStringLiteral("imu/calibration"),          QVariantMap{}).toMap();

        m_sessionGoalsByType = ppSettings().value(QStringLiteral("session/goalsByType"), QVariantMap{}).toMap();
        m_viewPanelsByType      = ppSettings().value(QStringLiteral("view/panelsByType"),      QVariantMap{}).toMap();
        m_viewArrangementByType = ppSettings().value(QStringLiteral("view/arrangementByType"), QVariantMap{}).toMap();
        m_viewPresetByType      = ppSettings().value(QStringLiteral("view/presetByType"),      QVariantMap{}).toMap();
        m_viewLayoutByMode      = ppSettings().value(QStringLiteral("view/layoutByMode"),      QVariantMap{}).toMap();
        m_dataRegionByType      = ppSettings().value(QStringLiteral("view/dataRegionByType"),  QVariantMap{}).toMap();
        m_dashboardPresetsByType = ppSettings().value(QStringLiteral("view/dashboardPresetsByType"), QVariantMap{}).toMap();
        m_sectionCollapse       = ppSettings().value(QStringLiteral("view/sectionCollapse"),   QVariantMap{}).toMap();
        m_chartPrefs            = ppSettings().value(QStringLiteral("view/chartPrefs"),         QVariantMap{}).toMap();
        m_lastSessionType    = ppSettings().value(QStringLiteral("session/lastType"), 0).toInt();

        m_sessionNamingPattern  = ppSettings().value(QStringLiteral("storage/sessionNamingPattern"),  QStringLiteral("date-name-type")).toString();
        m_videoResolutionMode   = ppSettings().value(QStringLiteral("storage/videoResolutionMode"),   QStringLiteral("native")).toString();
        m_videoCodec            = ppSettings().value(QStringLiteral("storage/videoCodec"),            QStringLiteral("h264")).toString();
        // Codecs were rationalised to the cross-platform set; coerce a persisted
        // retired value ("prores"/"raw") so the UI selection and exporter agree.
        if (m_videoCodec != QLatin1String("h264") && m_videoCodec != QLatin1String("h265"))
            m_videoCodec = QStringLiteral("h264");
        m_videoQuality          = ppSettings().value(QStringLiteral("storage/videoQuality"),          QStringLiteral("medium")).toString();
        m_videoContainer        = ppSettings().value(QStringLiteral("storage/videoContainer"),        QStringLiteral("mp4")).toString();
        m_saveRawFrames         = ppSettings().value(QStringLiteral("storage/saveRawFrames"),         false).toBool();
        m_skipAnalysisForRawCapture = ppSettings().value(QStringLiteral("storage/skipAnalysisForRawCapture"), false).toBool();
        m_savePoseKeypoints     = ppSettings().value(QStringLiteral("storage/savePoseKeypoints"),     true).toBool();
        m_saveImuStreams         = ppSettings().value(QStringLiteral("storage/saveImuStreams"),        true).toBool();
        m_imuDataFormat         = ppSettings().value(QStringLiteral("storage/imuDataFormat"),         QStringLiteral("json")).toString();
        m_saveLaunchMonitorData = ppSettings().value(QStringLiteral("storage/saveLaunchMonitorData"), true).toBool();

        m_clubLenPrior = ppSettings().value(QStringLiteral("analysis/clubLenPrior"), QVariantMap{}).toMap();
    }

    QString appVersion()     const { return QStringLiteral(PINPOINT_VERSION_STRING); }
    int     themeIndex()    const { return m_themeIndex; }
    int     windowWidth()   const { return m_windowWidth; }
    int     windowHeight()  const { return m_windowHeight; }
    double  fontScale()     const { return m_fontScale; }
    QString density()       const { return m_density; }
    QString timelineOrientation() const { return m_timelineOrientation; }
    QString wristReferenceSwingDir() const { return m_wristReferenceSwingDir; }
    bool    timelineSnapToPhases() const { return m_timelineSnapToPhases; }
    bool    metricsHidePlanned()  const { return m_metricsHidePlanned; }
    QString diagnosticsGradePolicy() const { return m_diagnosticsGradePolicy; }
    bool    autoReplayAfterCapture() const { return m_autoReplayAfterCapture; }
    bool    replayTrimToSwing()   const { return m_replayTrimToSwing; }
    bool    reduceMotion()  const { return m_reduceMotion; }
    bool    gradientTitles() const { return m_gradientTitles; }
    double  overlayOpacity()  const { return m_overlayOpacity; }
    bool    windowMaximized() const { return m_windowMaximized; }
    int     windowX()         const { return m_windowX; }
    int     windowY()         const { return m_windowY; }

    // Returns the index into Qt.application.screens of the screen containing the cursor.
    Q_INVOKABLE int cursorScreenIndex() const
    {
        const QPoint pos = QCursor::pos();
        const QList<QScreen *> screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            if (screens[i]->geometry().contains(pos))
                return i;
        }
        return 0;
    }

    QString language()                  const { return m_language; }
    QString units()                     const { return m_units; }
    QString athleteLibraryPath()        const { return m_athleteLibraryPath; }
    bool    autoSaveSession()           const { return m_autoSaveSession; }
    bool    autoDetectSwing()           const { return m_autoDetectSwing; }
    QString swingDetectionSensitivity() const { return m_swingDetectionSensitivity; }
    QString motionCaptureQuality()      const { return m_motionCaptureQuality; }
    int     audioDeviceLatencyUs()      const { return m_audioDeviceLatencyUs; }
    QString audioInputDevice()          const { return m_audioInputDevice; }
    bool    acousticShotDetectionEnabled() const { return m_acousticShotDetectionEnabled; }
    double  acousticSensitivity()       const { return m_acousticSensitivity; }
    bool    aiCoachingOnSessionEnd()    const { return m_aiCoachingOnSessionEnd; }
    bool    cloudFallbackStt()          const { return m_cloudFallbackStt; }
    bool    cloudFallbackTts()          const { return m_cloudFallbackTts; }
    bool    cloudFallbackLlm()          const { return m_cloudFallbackLlm; }
    bool    checkForUpdates()           const { return m_checkForUpdates; }
    QString skippedUpdateVersion()      const { return m_skippedUpdateVersion; }
    bool    sendDiagnostics()           const { return m_sendDiagnostics; }

    QString mainDisplayMode()        const { return m_mainDisplayMode; }
    bool    rememberWindowGeometry() const { return m_rememberWindowGeometry; }
    QString secondaryDisplayMode()   const { return m_secondaryDisplayMode; }
    QString postShotContent()        const { return m_postShotContent; }
    double  postShotDelay()          const { return m_postShotDelay; }
    bool    postShotMirror()         const { return m_postShotMirror; }
    QString postShotDisplayMode()    const { return m_postShotDisplayMode; }
    double  postShotDwell()          const { return m_postShotDwell; }
    QString dashboardScale()         const { return m_dashboardScale; }
    QString uiFrameRateCap()         const { return m_uiFrameRateCap; }
    bool    hardwareAcceleration()   const { return m_hardwareAcceleration; }

    QStringList cameraExcluded()    const { return m_cameraExcluded; }
    QVariantMap cameraTargetFps()   const { return m_cameraTargetFps; }
    QVariantMap cameraTriggerMode() const { return m_cameraTriggerMode; }
    QVariantMap cameraRoi()         const { return m_cameraRoi; }
    QVariantMap cameraPerspective() const { return m_cameraPerspective; }
    QVariantMap cameraIsMirrored()  const { return m_cameraIsMirrored; }
    QVariantMap cameraBallRoi()     const { return m_cameraBallRoi; }
    double      cameraPreroll()      const { return m_cameraPreroll; }
    bool        cameraSyncEnabled()  const { return m_cameraSyncEnabled; }
    QVariantMap cameraFixedInPlace() const { return m_cameraFixedInPlace; }
    QVariantMap cameraAlias()        const { return m_cameraAlias; }

    QVariantMap sessionGoalsByType() const { return m_sessionGoalsByType; }
    QVariantMap viewPanelsByType()      const { return m_viewPanelsByType; }
    QVariantMap viewArrangementByType() const { return m_viewArrangementByType; }
    QVariantMap viewPresetByType()      const { return m_viewPresetByType; }
    QVariantMap viewLayoutByMode()      const { return m_viewLayoutByMode; }
    QVariantMap dataRegionByType()      const { return m_dataRegionByType; }
    QVariantMap dashboardPresetsByType() const { return m_dashboardPresetsByType; }
    QVariantMap sectionCollapse()       const { return m_sectionCollapse; }
    QVariantMap chartPrefs()            const { return m_chartPrefs; }
    int         lastSessionType()    const { return m_lastSessionType; }

    QString sessionNamingPattern()  const { return m_sessionNamingPattern; }
    QString videoResolutionMode()   const { return m_videoResolutionMode; }
    QString videoCodec()            const { return m_videoCodec; }
    QString videoQuality()          const { return m_videoQuality; }
    QString videoContainer()        const { return m_videoContainer; }
    bool    saveRawFrames()         const { return m_saveRawFrames; }
    bool    skipAnalysisForRawCapture() const { return m_skipAnalysisForRawCapture; }
    bool    savePoseKeypoints()     const { return m_savePoseKeypoints; }
    bool    saveImuStreams()        const { return m_saveImuStreams; }
    QString imuDataFormat()         const { return m_imuDataFormat; }
    bool    saveLaunchMonitorData() const { return m_saveLaunchMonitorData; }

    QVariantMap clubLenPrior() const { return m_clubLenPrior; }

    QStringList imuExcluded()             const { return m_imuExcluded; }
    QVariantMap imuPlacement()            const { return m_imuPlacement; }
    QVariantMap imuOutputRateHz()         const { return m_imuOutputRateHz; }
    QVariantMap imuFusionMode()           const { return m_imuFusionMode; }
    QVariantMap imuMountOrientation()     const { return m_imuMountOrientation; }
    bool        imuAutoConnect()          const { return m_imuAutoConnect; }
    bool        imuAutoReconnect()        const { return m_imuAutoReconnect; }
    bool        imuSaveCalibrationToFlash() const { return m_imuSaveCalibrationToFlash; }
    QString     imuDefaultFusionMode()    const { return m_imuDefaultFusionMode; }
    QString     imuOrientationFilter()    const { return m_imuOrientationFilter; }
    QVariantMap imuAlias()               const { return m_imuAlias; }
    QVariantMap imuCalibration()         const { return m_imuCalibration; }

    void setThemeIndex(int v)
    {
        if (m_themeIndex == v) return;
        m_themeIndex = v;
        ppSettings().setValue(QStringLiteral("ui/themeIndex"), v);
        emit themeIndexChanged();
    }

    void setWindowWidth(int v)
    {
        if (m_windowWidth == v) return;
        m_windowWidth = v;
        ppSettings().setValue(QStringLiteral("ui/windowWidth"), v);
        emit windowWidthChanged();
    }

    void setWindowHeight(int v)
    {
        if (m_windowHeight == v) return;
        m_windowHeight = v;
        ppSettings().setValue(QStringLiteral("ui/windowHeight"), v);
        emit windowHeightChanged();
    }

    void setFontScale(double v)
    {
        if (qFuzzyCompare(m_fontScale, v)) return;
        m_fontScale = v;
        ppSettings().setValue(QStringLiteral("ui/fontScale"), v);
        emit fontScaleChanged();
    }

    void setDensity(const QString &v)
    {
        if (m_density == v) return;
        m_density = v;
        ppSettings().setValue(QStringLiteral("ui/density"), v);
        emit densityChanged();
    }

    void setTimelineOrientation(const QString &v)
    {
        if (m_timelineOrientation == v) return;
        m_timelineOrientation = v;
        ppSettings().setValue(QStringLiteral("ui/timelineOrientation"), v);
        emit timelineOrientationChanged();
    }
    void setWristReferenceSwingDir(const QString &v)
    {
        if (m_wristReferenceSwingDir == v) return;
        m_wristReferenceSwingDir = v;
        ppSettings().setValue(QStringLiteral("ui/wristReferenceSwingDir"), v);
        emit wristReferenceSwingDirChanged();
    }

    void setTimelineSnapToPhases(bool v)
    {
        if (m_timelineSnapToPhases == v) return;
        m_timelineSnapToPhases = v;
        ppSettings().setValue(QStringLiteral("ui/timelineSnapToPhases"), v);
        emit timelineSnapToPhasesChanged();
    }

    void setMetricsHidePlanned(bool v)
    {
        if (m_metricsHidePlanned == v) return;
        m_metricsHidePlanned = v;
        ppSettings().setValue(QStringLiteral("ui/metricsHidePlanned"), v);
        emit metricsHidePlannedChanged();
    }

    void setDiagnosticsGradePolicy(const QString &v)
    {
        if (m_diagnosticsGradePolicy == v) return;
        m_diagnosticsGradePolicy = v;
        ppSettings().setValue(QStringLiteral("ui/diagnosticsGradePolicy"), v);
        emit diagnosticsGradePolicyChanged();
    }

    void setAutoReplayAfterCapture(bool v)
    {
        if (m_autoReplayAfterCapture == v) return;
        m_autoReplayAfterCapture = v;
        ppSettings().setValue(QStringLiteral("ui/autoReplayAfterCapture"), v);
        emit autoReplayAfterCaptureChanged();
    }

    void setReplayTrimToSwing(bool v)
    {
        if (m_replayTrimToSwing == v) return;
        m_replayTrimToSwing = v;
        ppSettings().setValue(QStringLiteral("ui/replayTrimToSwing"), v);
        emit replayTrimToSwingChanged();
    }

    void setReduceMotion(bool v)
    {
        if (m_reduceMotion == v) return;
        m_reduceMotion = v;
        ppSettings().setValue(QStringLiteral("ui/reduceMotion"), v);
        emit reduceMotionChanged();
    }

    void setGradientTitles(bool v)
    {
        if (m_gradientTitles == v) return;
        m_gradientTitles = v;
        ppSettings().setValue(QStringLiteral("ui/gradientTitles"), v);
        emit gradientTitlesChanged();
    }

    void setOverlayOpacity(double v)
    {
        if (qFuzzyCompare(m_overlayOpacity, v)) return;
        m_overlayOpacity = v;
        ppSettings().setValue(QStringLiteral("ui/overlayOpacity"), v);
        emit overlayOpacityChanged();
    }

    void setWindowMaximized(bool v)
    {
        if (m_windowMaximized == v) return;
        m_windowMaximized = v;
        ppSettings().setValue(QStringLiteral("ui/windowMaximized"), v);
        emit windowMaximizedChanged();
    }

    void setWindowX(int v)
    {
        if (m_windowX == v) return;
        m_windowX = v;
        ppSettings().setValue(QStringLiteral("ui/windowX"), v);
        emit windowXChanged();
    }

    void setWindowY(int v)
    {
        if (m_windowY == v) return;
        m_windowY = v;
        ppSettings().setValue(QStringLiteral("ui/windowY"), v);
        emit windowYChanged();
    }

    void setLanguage(const QString &v)
    {
        if (m_language == v) return;
        m_language = v;
        ppSettings().setValue(QStringLiteral("General/language"), v);
        emit languageChanged();
    }

    void setUnits(const QString &v)
    {
        if (m_units == v) return;
        m_units = v;
        ppSettings().setValue(QStringLiteral("General/units"), v);
        emit unitsChanged();
    }

    void setAthleteLibraryPath(const QString &raw)
    {
        const QString v = normaliseLibraryPath(raw);
        if (m_athleteLibraryPath == v) return;
        m_athleteLibraryPath = v;
        ppSettings().setValue(QStringLiteral("General/athleteLibraryPath"), v);
        emit athleteLibraryPathChanged();
    }

    void setAutoSaveSession(bool v)
    {
        if (m_autoSaveSession == v) return;
        m_autoSaveSession = v;
        ppSettings().setValue(QStringLiteral("General/autoSaveSession"), v);
        emit autoSaveSessionChanged();
    }

    void setAutoDetectSwing(bool v)
    {
        if (m_autoDetectSwing == v) return;
        m_autoDetectSwing = v;
        ppSettings().setValue(QStringLiteral("General/autoDetectSwing"), v);
        emit autoDetectSwingChanged();
    }

    void setSwingDetectionSensitivity(const QString &v)
    {
        if (m_swingDetectionSensitivity == v) return;
        m_swingDetectionSensitivity = v;
        ppSettings().setValue(QStringLiteral("General/swingDetectionSensitivity"), v);
        emit swingDetectionSensitivityChanged();
    }

    void setMotionCaptureQuality(const QString &v)
    {
        if (m_motionCaptureQuality == v) return;
        m_motionCaptureQuality = v;
        ppSettings().setValue(QStringLiteral("General/motionCaptureQuality"), v);
        emit motionCaptureQualityChanged();
    }

    void setAudioDeviceLatencyUs(int v)
    {
        if (m_audioDeviceLatencyUs == v) return;
        m_audioDeviceLatencyUs = v;
        ppSettings().setValue(QStringLiteral("General/audioDeviceLatencyUs"), v);
        emit audioDeviceLatencyUsChanged();
    }

    void setAudioInputDevice(const QString &v)
    {
        if (m_audioInputDevice == v) return;
        m_audioInputDevice = v;
        ppSettings().setValue(QStringLiteral("General/audioInputDevice"), v);
        emit audioInputDeviceChanged();
    }

    void setAcousticShotDetectionEnabled(bool v)
    {
        if (m_acousticShotDetectionEnabled == v) return;
        m_acousticShotDetectionEnabled = v;
        ppSettings().setValue(QStringLiteral("General/acousticShotDetectionEnabled"), v);
        emit acousticShotDetectionEnabledChanged();
    }

    void setAcousticSensitivity(double v)
    {
        // Clamp to [0,1] — the slider's normalised range.
        v = qBound(0.0, v, 1.0);
        if (qFuzzyCompare(m_acousticSensitivity, v)) return;
        m_acousticSensitivity = v;
        ppSettings().setValue(QStringLiteral("General/acousticSensitivity"), v);
        emit acousticSensitivityChanged();
    }

    void setAiCoachingOnSessionEnd(bool v)
    {
        if (m_aiCoachingOnSessionEnd == v) return;
        m_aiCoachingOnSessionEnd = v;
        ppSettings().setValue(QStringLiteral("General/aiCoachingOnSessionEnd"), v);
        emit aiCoachingOnSessionEndChanged();
    }

    void setCloudFallbackStt(bool v)
    {
        if (m_cloudFallbackStt == v) return;
        m_cloudFallbackStt = v;
        ppSettings().setValue(QStringLiteral("General/cloudFallbackStt"), v);
        emit cloudFallbackSttChanged();
    }

    void setCloudFallbackTts(bool v)
    {
        if (m_cloudFallbackTts == v) return;
        m_cloudFallbackTts = v;
        ppSettings().setValue(QStringLiteral("General/cloudFallbackTts"), v);
        emit cloudFallbackTtsChanged();
    }

    void setCloudFallbackLlm(bool v)
    {
        if (m_cloudFallbackLlm == v) return;
        m_cloudFallbackLlm = v;
        ppSettings().setValue(QStringLiteral("General/cloudFallbackLlm"), v);
        emit cloudFallbackLlmChanged();
    }

    void setCheckForUpdates(bool v)
    {
        if (m_checkForUpdates == v) return;
        m_checkForUpdates = v;
        ppSettings().setValue(QStringLiteral("General/checkForUpdates"), v);
        emit checkForUpdatesChanged();
    }

    void setSkippedUpdateVersion(const QString &v)
    {
        if (m_skippedUpdateVersion == v) return;
        m_skippedUpdateVersion = v;
        ppSettings().setValue(QStringLiteral("General/skippedUpdateVersion"), v);
        emit skippedUpdateVersionChanged();
    }

    void setSendDiagnostics(bool v)
    {
        if (m_sendDiagnostics == v) return;
        m_sendDiagnostics = v;
        ppSettings().setValue(QStringLiteral("General/sendDiagnostics"), v);
        emit sendDiagnosticsChanged();
    }

    void setMainDisplayMode(const QString &v)
    {
        if (m_mainDisplayMode == v) return;
        m_mainDisplayMode = v;
        ppSettings().setValue(QStringLiteral("display/mainDisplayMode"), v);
        emit mainDisplayModeChanged();
    }

    void setRememberWindowGeometry(bool v)
    {
        if (m_rememberWindowGeometry == v) return;
        m_rememberWindowGeometry = v;
        ppSettings().setValue(QStringLiteral("display/rememberWindowGeometry"), v);
        emit rememberWindowGeometryChanged();
    }

    void setSecondaryDisplayMode(const QString &v)
    {
        if (m_secondaryDisplayMode == v) return;
        m_secondaryDisplayMode = v;
        ppSettings().setValue(QStringLiteral("display/secondaryDisplayMode"), v);
        emit secondaryDisplayModeChanged();
    }

    void setPostShotContent(const QString &v)
    {
        if (m_postShotContent == v) return;
        m_postShotContent = v;
        ppSettings().setValue(QStringLiteral("display/postShotContent"), v);
        emit postShotContentChanged();
    }

    void setPostShotDelay(double v)
    {
        if (qFuzzyCompare(m_postShotDelay, v)) return;
        m_postShotDelay = v;
        ppSettings().setValue(QStringLiteral("display/postShotDelay"), v);
        emit postShotDelayChanged();
    }

    void setPostShotMirror(bool v)
    {
        if (m_postShotMirror == v) return;
        m_postShotMirror = v;
        ppSettings().setValue(QStringLiteral("display/postShotMirror"), v);
        emit postShotMirrorChanged();
    }

    void setPostShotDisplayMode(const QString &v)
    {
        if (m_postShotDisplayMode == v) return;
        m_postShotDisplayMode = v;
        ppSettings().setValue(QStringLiteral("display/postShotDisplayMode"), v);
        emit postShotDisplayModeChanged();
    }

    void setPostShotDwell(double v)
    {
        if (qFuzzyCompare(m_postShotDwell, v)) return;
        m_postShotDwell = v;
        ppSettings().setValue(QStringLiteral("display/postShotDwell"), v);
        emit postShotDwellChanged();
    }

    void setDashboardScale(const QString &v)
    {
        if (m_dashboardScale == v) return;
        m_dashboardScale = v;
        ppSettings().setValue(QStringLiteral("display/dashboardScale"), v);
        emit dashboardScaleChanged();
    }

    void setUiFrameRateCap(const QString &v)
    {
        if (m_uiFrameRateCap == v) return;
        m_uiFrameRateCap = v;
        ppSettings().setValue(QStringLiteral("display/uiFrameRateCap"), v);
        emit uiFrameRateCapChanged();
    }

    void setHardwareAcceleration(bool v)
    {
        if (m_hardwareAcceleration == v) return;
        m_hardwareAcceleration = v;
        ppSettings().setValue(QStringLiteral("display/hardwareAcceleration"), v);
        emit hardwareAccelerationChanged();
    }

    void setCameraExcluded(const QStringList &v)
    {
        if (m_cameraExcluded == v) return;
        m_cameraExcluded = v;
        ppSettings().setValue(QStringLiteral("camera/excluded"), v);
        emit cameraExcludedChanged();
    }

    void setCameraTargetFps(const QVariantMap &v)
    {
        if (m_cameraTargetFps == v) return;
        m_cameraTargetFps = v;
        ppSettings().setValue(QStringLiteral("camera/targetFps"), v);
        emit cameraTargetFpsChanged();
    }

    void setCameraTriggerMode(const QVariantMap &v)
    {
        if (m_cameraTriggerMode == v) return;
        m_cameraTriggerMode = v;
        ppSettings().setValue(QStringLiteral("camera/triggerMode"), v);
        emit cameraTriggerModeChanged();
    }

    void setCameraRoi(const QVariantMap &v)
    {
        if (m_cameraRoi == v) return;
        m_cameraRoi = v;
        ppSettings().setValue(QStringLiteral("camera/roi"), v);
        emit cameraRoiChanged();
    }

    void setCameraPerspective(const QVariantMap &v)
    {
        if (m_cameraPerspective == v) return;
        m_cameraPerspective = v;
        ppSettings().setValue(QStringLiteral("camera/perspective"), v);
        emit cameraPerspectiveChanged();
    }

    void setCameraIsMirrored(const QVariantMap &v)
    {
        if (m_cameraIsMirrored == v) return;
        m_cameraIsMirrored = v;
        ppSettings().setValue(QStringLiteral("camera/isMirrored"), v);
        emit cameraIsMirroredChanged();
    }

    void setCameraBallRoi(const QVariantMap &v)
    {
        if (m_cameraBallRoi == v) return;
        m_cameraBallRoi = v;
        ppSettings().setValue(QStringLiteral("camera/ballRoi"), v);
        emit cameraBallRoiChanged();
    }

    void setCameraPreroll(double v)
    {
        if (qFuzzyCompare(m_cameraPreroll, v)) return;
        m_cameraPreroll = v;
        ppSettings().setValue(QStringLiteral("camera/preroll"), v);
        emit cameraPrerollChanged();
    }

    void setCameraSyncEnabled(bool v)
    {
        if (m_cameraSyncEnabled == v) return;
        m_cameraSyncEnabled = v;
        ppSettings().setValue(QStringLiteral("camera/syncEnabled"), v);
        emit cameraSyncEnabledChanged();
    }

    void setCameraFixedInPlace(const QVariantMap &v)
    {
        if (m_cameraFixedInPlace == v) return;
        m_cameraFixedInPlace = v;
        ppSettings().setValue(QStringLiteral("camera/fixedInPlace"), v);
        emit cameraFixedInPlaceChanged();
    }

    void setCameraAlias(const QVariantMap &v)
    {
        if (m_cameraAlias == v) return;
        m_cameraAlias = v;
        ppSettings().setValue(QStringLiteral("camera/alias"), v);
        emit cameraAliasChanged();
    }

    void setImuExcluded(const QStringList &v)
    {
        if (m_imuExcluded == v) return;
        m_imuExcluded = v;
        ppSettings().setValue(QStringLiteral("imu/excluded"), v);
        emit imuExcludedChanged();
    }

    void setImuPlacement(const QVariantMap &v)
    {
        if (m_imuPlacement == v) return;
        m_imuPlacement = v;
        ppSettings().setValue(QStringLiteral("imu/placement"), v);
        emit imuPlacementChanged();
    }

    void setImuOutputRateHz(const QVariantMap &v)
    {
        if (m_imuOutputRateHz == v) return;
        m_imuOutputRateHz = v;
        ppSettings().setValue(QStringLiteral("imu/outputRateHz"), v);
        emit imuOutputRateHzChanged();
    }

    void setImuFusionMode(const QVariantMap &v)
    {
        if (m_imuFusionMode == v) return;
        m_imuFusionMode = v;
        ppSettings().setValue(QStringLiteral("imu/fusionMode"), v);
        emit imuFusionModeChanged();
    }

    void setImuMountOrientation(const QVariantMap &v)
    {
        if (m_imuMountOrientation == v) return;
        m_imuMountOrientation = v;
        ppSettings().setValue(QStringLiteral("imu/mountOrientation"), v);
        emit imuMountOrientationChanged();
    }

    void setImuAutoConnect(bool v)
    {
        if (m_imuAutoConnect == v) return;
        m_imuAutoConnect = v;
        ppSettings().setValue(QStringLiteral("imu/autoConnect"), v);
        emit imuAutoConnectChanged();
    }

    void setImuAutoReconnect(bool v)
    {
        if (m_imuAutoReconnect == v) return;
        m_imuAutoReconnect = v;
        ppSettings().setValue(QStringLiteral("imu/autoReconnect"), v);
        emit imuAutoReconnectChanged();
    }

    void setImuSaveCalibrationToFlash(bool v)
    {
        if (m_imuSaveCalibrationToFlash == v) return;
        m_imuSaveCalibrationToFlash = v;
        ppSettings().setValue(QStringLiteral("imu/saveCalibrationToFlash"), v);
        emit imuSaveCalibrationToFlashChanged();
    }

    void setImuDefaultFusionMode(const QString &v)
    {
        if (m_imuDefaultFusionMode == v) return;
        m_imuDefaultFusionMode = v;
        ppSettings().setValue(QStringLiteral("imu/defaultFusionMode"), v);
        emit imuDefaultFusionModeChanged();
    }

    void setImuOrientationFilter(const QString &v)
    {
        if (m_imuOrientationFilter == v) return;
        m_imuOrientationFilter = v;
        ppSettings().setValue(QStringLiteral("imu/orientationFilter"), v);
        emit imuOrientationFilterChanged();
    }

    void setImuAlias(const QVariantMap &v)
    {
        if (m_imuAlias == v) return;
        m_imuAlias = v;
        ppSettings().setValue(QStringLiteral("imu/alias"), v);
        emit imuAliasChanged();
    }

    void setImuCalibration(const QVariantMap &v)
    {
        if (m_imuCalibration == v) return;
        m_imuCalibration = v;
        ppSettings().setValue(QStringLiteral("imu/calibration"), v);
        emit imuCalibrationChanged();
    }

    void setSessionGoalsByType(const QVariantMap &v)
    {
        if (m_sessionGoalsByType == v) return;
        m_sessionGoalsByType = v;
        ppSettings().setValue(QStringLiteral("session/goalsByType"), v);
        emit sessionGoalsByTypeChanged();
    }

    void setViewPanelsByType(const QVariantMap &v)
    {
        if (m_viewPanelsByType == v) return;
        m_viewPanelsByType = v;
        ppSettings().setValue(QStringLiteral("view/panelsByType"), v);
        emit viewPanelsByTypeChanged();
    }

    void setViewArrangementByType(const QVariantMap &v)
    {
        if (m_viewArrangementByType == v) return;
        m_viewArrangementByType = v;
        ppSettings().setValue(QStringLiteral("view/arrangementByType"), v);
        emit viewArrangementByTypeChanged();
    }

    void setViewPresetByType(const QVariantMap &v)
    {
        if (m_viewPresetByType == v) return;
        m_viewPresetByType = v;
        ppSettings().setValue(QStringLiteral("view/presetByType"), v);
        emit viewPresetByTypeChanged();
    }

    void setViewLayoutByMode(const QVariantMap &v)
    {
        if (m_viewLayoutByMode == v) return;
        m_viewLayoutByMode = v;
        ppSettings().setValue(QStringLiteral("view/layoutByMode"), v);
        emit viewLayoutByModeChanged();
    }

    void setDataRegionByType(const QVariantMap &v)
    {
        if (m_dataRegionByType == v) return;
        m_dataRegionByType = v;
        ppSettings().setValue(QStringLiteral("view/dataRegionByType"), v);
        emit dataRegionByTypeChanged();
    }

    void setDashboardPresetsByType(const QVariantMap &v)
    {
        if (m_dashboardPresetsByType == v) return;
        m_dashboardPresetsByType = v;
        ppSettings().setValue(QStringLiteral("view/dashboardPresetsByType"), v);
        emit dashboardPresetsByTypeChanged();
    }

    void setSectionCollapse(const QVariantMap &v)
    {
        if (m_sectionCollapse == v) return;
        m_sectionCollapse = v;
        ppSettings().setValue(QStringLiteral("view/sectionCollapse"), v);
        emit sectionCollapseChanged();
    }

    void setChartPrefs(const QVariantMap &v)
    {
        if (m_chartPrefs == v) return;
        m_chartPrefs = v;
        ppSettings().setValue(QStringLiteral("view/chartPrefs"), v);
        emit chartPrefsChanged();
    }

    void setLastSessionType(int v)
    {
        if (m_lastSessionType == v) return;
        m_lastSessionType = v;
        ppSettings().setValue(QStringLiteral("session/lastType"), v);
        emit lastSessionTypeChanged();
    }

    void setSessionNamingPattern(const QString &v)
    {
        if (m_sessionNamingPattern == v) return;
        m_sessionNamingPattern = v;
        ppSettings().setValue(QStringLiteral("storage/sessionNamingPattern"), v);
        emit sessionNamingPatternChanged();
    }

    void setVideoResolutionMode(const QString &v)
    {
        if (m_videoResolutionMode == v) return;
        m_videoResolutionMode = v;
        ppSettings().setValue(QStringLiteral("storage/videoResolutionMode"), v);
        emit videoResolutionModeChanged();
    }

    void setVideoCodec(const QString &v)
    {
        if (m_videoCodec == v) return;
        m_videoCodec = v;
        ppSettings().setValue(QStringLiteral("storage/videoCodec"), v);
        emit videoCodecChanged();
    }

    void setVideoQuality(const QString &v)
    {
        if (m_videoQuality == v) return;
        m_videoQuality = v;
        ppSettings().setValue(QStringLiteral("storage/videoQuality"), v);
        emit videoQualityChanged();
    }

    void setVideoContainer(const QString &v)
    {
        if (m_videoContainer == v) return;
        m_videoContainer = v;
        ppSettings().setValue(QStringLiteral("storage/videoContainer"), v);
        emit videoContainerChanged();
    }

    void setSaveRawFrames(bool v)
    {
        if (m_saveRawFrames == v) return;
        m_saveRawFrames = v;
        ppSettings().setValue(QStringLiteral("storage/saveRawFrames"), v);
        emit saveRawFramesChanged();
    }
    void setSkipAnalysisForRawCapture(bool v)
    {
        if (m_skipAnalysisForRawCapture == v) return;
        m_skipAnalysisForRawCapture = v;
        ppSettings().setValue(QStringLiteral("storage/skipAnalysisForRawCapture"), v);
        emit skipAnalysisForRawCaptureChanged();
    }

    void setSavePoseKeypoints(bool v)
    {
        if (m_savePoseKeypoints == v) return;
        m_savePoseKeypoints = v;
        ppSettings().setValue(QStringLiteral("storage/savePoseKeypoints"), v);
        emit savePoseKeypointsChanged();
    }

    void setImuStreams(bool v)
    {
        if (m_saveImuStreams == v) return;
        m_saveImuStreams = v;
        ppSettings().setValue(QStringLiteral("storage/saveImuStreams"), v);
        emit saveImuStreamsChanged();
    }

    void setImuDataFormat(const QString &v)
    {
        if (m_imuDataFormat == v) return;
        m_imuDataFormat = v;
        ppSettings().setValue(QStringLiteral("storage/imuDataFormat"), v);
        emit imuDataFormatChanged();
    }

    void setSaveLaunchMonitorData(bool v)
    {
        if (m_saveLaunchMonitorData == v) return;
        m_saveLaunchMonitorData = v;
        ppSettings().setValue(QStringLiteral("storage/saveLaunchMonitorData"), v);
        emit saveLaunchMonitorDataChanged();
    }

    void setClubLenPrior(const QVariantMap &v)
    {
        if (m_clubLenPrior == v) return;
        m_clubLenPrior = v;
        ppSettings().setValue(QStringLiteral("analysis/clubLenPrior"), v);
        emit clubLenPriorChanged();
    }

signals:
    void themeIndexChanged();
    void windowWidthChanged();
    void windowHeightChanged();
    void fontScaleChanged();
    void densityChanged();
    void timelineOrientationChanged();
    void wristReferenceSwingDirChanged();
    void timelineSnapToPhasesChanged();
    void metricsHidePlannedChanged();
    void diagnosticsGradePolicyChanged();
    void autoReplayAfterCaptureChanged();
    void replayTrimToSwingChanged();
    void reduceMotionChanged();
    void gradientTitlesChanged();
    void overlayOpacityChanged();
    void windowMaximizedChanged();
    void windowXChanged();
    void windowYChanged();
    void languageChanged();
    void unitsChanged();
    void athleteLibraryPathChanged();
    void autoSaveSessionChanged();
    void autoDetectSwingChanged();
    void swingDetectionSensitivityChanged();
    void motionCaptureQualityChanged();
    void audioDeviceLatencyUsChanged();
    void audioInputDeviceChanged();
    void acousticShotDetectionEnabledChanged();
    void acousticSensitivityChanged();
    void aiCoachingOnSessionEndChanged();
    void cloudFallbackSttChanged();
    void cloudFallbackTtsChanged();
    void cloudFallbackLlmChanged();
    void checkForUpdatesChanged();
    void skippedUpdateVersionChanged();
    void sendDiagnosticsChanged();
    void mainDisplayModeChanged();
    void rememberWindowGeometryChanged();
    void secondaryDisplayModeChanged();
    void postShotContentChanged();
    void postShotDelayChanged();
    void postShotMirrorChanged();
    void postShotDisplayModeChanged();
    void postShotDwellChanged();
    void dashboardScaleChanged();
    void uiFrameRateCapChanged();
    void hardwareAccelerationChanged();
    void cameraExcludedChanged();
    void cameraTargetFpsChanged();
    void cameraTriggerModeChanged();
    void cameraRoiChanged();
    void cameraPerspectiveChanged();
    void cameraIsMirroredChanged();
    void cameraPrerollChanged();
    void cameraSyncEnabledChanged();
    void cameraFixedInPlaceChanged();
    void cameraBallRoiChanged();
    void cameraAliasChanged();
    void imuExcludedChanged();
    void imuPlacementChanged();
    void imuOutputRateHzChanged();
    void imuFusionModeChanged();
    void imuMountOrientationChanged();
    void imuAutoConnectChanged();
    void imuAutoReconnectChanged();
    void imuSaveCalibrationToFlashChanged();
    void imuDefaultFusionModeChanged();
    void imuOrientationFilterChanged();
    void imuAliasChanged();
    void imuCalibrationChanged();
    void sessionGoalsByTypeChanged();
    void viewPanelsByTypeChanged();
    void viewArrangementByTypeChanged();
    void viewPresetByTypeChanged();
    void viewLayoutByModeChanged();
    void dataRegionByTypeChanged();
    void dashboardPresetsByTypeChanged();
    void sectionCollapseChanged();
    void chartPrefsChanged();
    void lastSessionTypeChanged();
    void sessionNamingPatternChanged();
    void videoResolutionModeChanged();
    void videoCodecChanged();
    void videoQualityChanged();
    void videoContainerChanged();
    void saveRawFramesChanged();
    void skipAnalysisForRawCaptureChanged();
    void savePoseKeypointsChanged();
    void saveImuStreamsChanged();
    void imuDataFormatChanged();
    void saveLaunchMonitorDataChanged();
    void clubLenPriorChanged();

private:
    int     m_themeIndex      = 0;
    int     m_windowWidth     = 1120;
    int     m_windowHeight    = 700;
    double  m_fontScale       = -1.0;
    QString m_density         = QStringLiteral("default");
    QString m_timelineOrientation = QStringLiteral("horizontal");
    QString m_wristReferenceSwingDir;
    bool    m_timelineSnapToPhases = false;
    bool    m_metricsHidePlanned = false;
    QString m_diagnosticsGradePolicy = QStringLiteral("standard");
    bool    m_autoReplayAfterCapture = true;
    bool    m_replayTrimToSwing = false;
    bool    m_reduceMotion    = false;
    bool    m_gradientTitles  = true;
    double  m_overlayOpacity  = 0.7;
    bool    m_windowMaximized = false;
    int     m_windowX         = -1;
    int     m_windowY         = -1;

    QString m_language                  = QStringLiteral("en_GB");
    QString m_units                     = QStringLiteral("mph");
    QString m_athleteLibraryPath;
    bool    m_autoSaveSession           = true;
    bool    m_autoDetectSwing           = true;    // ON since the P3 arbiter
    QString m_swingDetectionSensitivity = QStringLiteral("Medium");
    QString m_motionCaptureQuality      = QStringLiteral("Medium");
    int     m_audioDeviceLatencyUs      = 20000;
    QString m_audioInputDevice;
    bool    m_acousticShotDetectionEnabled = true;
    double  m_acousticSensitivity       = 0.5;   // 0 = least sensitive, 1 = most
    bool    m_aiCoachingOnSessionEnd    = true;
    bool    m_cloudFallbackStt          = false;
    bool    m_cloudFallbackTts          = false;
    bool    m_cloudFallbackLlm          = false;
    bool    m_checkForUpdates           = true;
    QString m_skippedUpdateVersion;
    bool    m_sendDiagnostics           = false;

    QString m_mainDisplayMode        = QStringLiteral("primary");
    bool    m_rememberWindowGeometry = true;
    QString m_secondaryDisplayMode   = QStringLiteral("none");
    QString m_postShotContent        = QStringLiteral("replay");
    double  m_postShotDelay          = 0.5;
    bool    m_postShotMirror         = false;
    QString m_postShotDisplayMode    = QStringLiteral("panel");
    double  m_postShotDwell          = 8.0;
    QString m_dashboardScale         = QStringLiteral("medium");
    QString m_uiFrameRateCap         = QStringLiteral("display");
    bool    m_hardwareAcceleration   = true;

    QStringList m_cameraExcluded;
    QVariantMap m_cameraTargetFps;
    QVariantMap m_cameraTriggerMode;
    QVariantMap m_cameraRoi;
    QVariantMap m_cameraPerspective;
    QVariantMap m_cameraIsMirrored;
    double      m_cameraPreroll     = 1.0;
    bool        m_cameraSyncEnabled = true;
    QVariantMap m_cameraFixedInPlace;
    QVariantMap m_cameraBallRoi;
    QVariantMap m_cameraAlias;

    QStringList m_imuExcluded;
    QVariantMap m_imuPlacement;
    QVariantMap m_imuOutputRateHz;
    QVariantMap m_imuFusionMode;
    QVariantMap m_imuMountOrientation;
    QVariantMap m_imuAlias;
    QVariantMap m_imuCalibration;
    bool        m_imuAutoConnect          = true;
    bool        m_imuAutoReconnect        = true;
    bool        m_imuSaveCalibrationToFlash = false;
    QString     m_imuDefaultFusionMode    = QStringLiteral("9axis");
    QString     m_imuOrientationFilter    = QStringLiteral("Madgwick");

    QVariantMap m_sessionGoalsByType;
    QVariantMap m_viewPanelsByType;
    QVariantMap m_viewArrangementByType;
    QVariantMap m_viewPresetByType;
    QVariantMap m_viewLayoutByMode;
    QVariantMap m_dataRegionByType;
    QVariantMap m_dashboardPresetsByType;
    QVariantMap m_sectionCollapse;
    QVariantMap m_chartPrefs;
    int         m_lastSessionType = 0;

    QString m_sessionNamingPattern  = QStringLiteral("date-name-type");
    QString m_videoResolutionMode   = QStringLiteral("native");
    QString m_videoCodec            = QStringLiteral("h264");
    QString m_videoQuality          = QStringLiteral("medium");
    QString m_videoContainer        = QStringLiteral("mp4");
    bool    m_saveRawFrames         = false;
    bool    m_skipAnalysisForRawCapture = false;
    bool    m_savePoseKeypoints     = true;
    bool    m_saveImuStreams        = true;
    QString m_imuDataFormat         = QStringLiteral("json");
    bool    m_saveLaunchMonitorData = true;

    QVariantMap m_clubLenPrior;
};
