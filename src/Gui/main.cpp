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

#include <QGuiApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <cmath>

#include "pp_debug.h"
#include "app_settings.h"
#include "app_info.h"
#ifdef HAVE_OPENCV
#  include <opencv2/core.hpp>
#  include <opencv2/core/utils/logger.hpp>
#endif
#include "SecretsManager.h"
#include "SecretsBridge.h"
#include "film_controller.h"
#include "imu_manager.h"
#include "transcription_controller.h"
#include "tts_controller.h"
#include "camera_manager.h"
#include "buffer_controller.h"
#include "event_buffer.h"
#include "athlete_controller.h"
#include "navigation_controller.h"
#include "resource_monitor_controller.h"
#include "profiler_controller.h"
#include "update_controller.h"
#include "cuda_runtime_controller.h"
#include "motion_capture_probe.h"
#include "pp_os_metrics.h"
#include "clipboard_helper.h"
#include "llm_controller.h"
#include "session_controller.h"
#include "session_review_controller.h"
#include "shot_controller.h"
#include "shot_list_model.h"
#include "../Export/swing_doc.h"
#include "../Export/swing_zip_exporter.h"
#include "shot_processor.h"
#include "shot_replay_controller.h"
#include "reanalysis_controller.h"
#include "live_wrist_angles.h"
#include "markup_controller.h"
#include "markup_image_provider.h"

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // Single-instance / updater mutex. The Windows installer (Inno Setup's
    // AppMutex directive — see cmake/PinPointPackaging.cmake) checks for this
    // named mutex so it can detect a running instance and replace files in place
    // during a future (auto-)update. We only create and hold it; the app does not
    // itself reject a second instance. The handle is intentionally leaked — the OS
    // releases it on process exit. The name MUST match CPACK_INNOSETUP_SETUP_AppMutex.
    ::CreateMutexW(nullptr, FALSE, L"PinPointStudio.SingleInstance.Mutex");
#endif
    PinPointDebug::install();
#ifdef HAVE_OPENCV
    // WARN+ logger output still goes to std::cerr (no writer hook before
    // OpenCV 4.11) — captured into PpMessageLog by the cerr tee in
    // PinPointDebug::install() above.
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);
    cv::redirectError([](int status, const char *func, const char *msg,
                         const char * /*file*/, int /*line*/, void *) -> int {
        ppError() << "[OpenCV]" << (func ? func : "?") << "-" << (msg ? msg : "") << "(status" << status << ")";
        return 0;
    }, nullptr);
#endif

#if defined(Q_OS_LINUX)
    // Wayland/mutter does not let a client choose which monitor a window opens on
    // (see the geometry notes in Main.qml) — a secondary/kiosk post-shot display or
    // a pinned/cursor main window is placed by the compositor, not honoured. Under
    // XWayland (the xcb backend) Qt's setScreen()/geometry ARE honoured, so prefer
    // it WHEN the user actually relies on multi-display placement. Gated tightly so
    // single-display Wayland users keep native Wayland (and its better HiDPI on the
    // retina panel): only on a Wayland session with XWayland present (DISPLAY set),
    // only when QT_QPA_PLATFORM was not pinned by the user, and only when a
    // multi-display placement is configured. The platform is chosen once here, so a
    // change to these settings applies on the next launch.
    const bool onWayland = qgetenv("XDG_SESSION_TYPE") == "wayland"
                           || qEnvironmentVariableIsSet("WAYLAND_DISPLAY");
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")
        && onWayland
        && qEnvironmentVariableIsSet("DISPLAY")) {   // DISPLAY set ⇒ XWayland available
        QSettings s = ppSettings();
        const QString mainMode = s.value(QStringLiteral("display/mainDisplayMode"),
                                         QStringLiteral("primary")).toString();
        const QString secMode  = s.value(QStringLiteral("display/secondaryDisplayMode"),
                                         QStringLiteral("none")).toString();
        if (mainMode != QLatin1String("primary") || secMode != QLatin1String("none"))
            qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif

    QGuiApplication app(argc, argv);

    // Force the Basic Qt Quick Controls style on all platforms. Without this, files
    // that import plain `QtQuick.Controls` fall back to the platform-native style; on
    // Windows that style paints an opaque ScrollBar groove (palette.window → white)
    // that ignores our dark Theme. Basic's track is transparent, matching macOS/Linux.
    // Most QML already imports QtQuick.Controls.Basic explicitly; this enforces the
    // same intent app-wide. Must be called before the engine instantiates any Control.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Register the GUI thread with the resource profiler so it shows a labelled
    // per-thread CPU row (the sampler that reads it runs on this thread too).
    pinpoint::osmetrics::registerThread("UI");

    // Must run after the app object exists: loads the Qt Multimedia FFmpeg
    // plugin (which clobbers any earlier av_log callback) and re-installs the
    // FFmpeg → PpMessageLog capture on top of it.
    PinPointDebug::installFfmpegLogCapture();

    // Load bundled fonts so Theme.qml family names resolve on all platforms.
    const QStringList fontResources = {
        ":/fonts/Georgia.ttf",
        ":/fonts/Georgiab.ttf",
        ":/fonts/Georgiai.ttf",
        ":/fonts/Georgiaz.ttf",
        ":/fonts/DMSans-Variable.ttf",
        ":/fonts/DMSans-Italic-Variable.ttf",
        ":/fonts/DMMono-Regular.ttf",
        ":/fonts/DMMono-Medium.ttf",
        ":/fonts/DMSerifDisplay-Regular.ttf",
        ":/fonts/Fraunces-Variable.ttf",
        ":/fonts/Fraunces-Italic-Variable.ttf",
        // Static Regular (wght=400) and SemiBold (wght=600) instances: macOS/CoreText
        // won't interpolate the variable weight axis, so terrain body/tile text
        // (Font.Normal) and titles (Font.DemiBold) fell back to the system font — with
        // no concrete 400/600 face registered under family "Fraunces", the variable
        // file's default instance is 9pt Black (900). Both static faces are pinned to
        // opsz=9, SOFT=0, WONK=0 to match visually.
        ":/fonts/Fraunces-Regular.ttf",
        ":/fonts/Fraunces-SemiBold.ttf",
        ":/fonts/SourceSerif4-Variable.ttf",
        ":/fonts/SourceSerif4-Italic-Variable.ttf",
        ":/fonts/HankenGrotesk-Variable.ttf",
        ":/fonts/HankenGrotesk-Italic-Variable.ttf",
        ":/fonts/Literata-Variable.ttf",
        ":/fonts/Literata-Italic-Variable.ttf",
        // Static Regular (wght=400) and Medium (wght=500) instances for the Links
        // theme serif: macOS/CoreText won't interpolate the variable weight axis, so
        // Links body/display text (Font.Normal) and the few Medium labels/active-tabs
        // (Font.Medium) would fall back to the system font without concrete 400/500
        // faces registered under family "Literata". Same fix as the Fraunces statics
        // above; both pinned to opsz=12 (Literata's default optical size).
        ":/fonts/Literata-Regular.ttf",
        ":/fonts/Literata-Medium.ttf",
        ":/fonts/InstrumentSans-Variable.ttf",
        ":/fonts/JetBrainsMono-Variable.ttf",
        ":/fonts/PlayfairDisplay-Variable.ttf",
        ":/fonts/Geist-Variable.ttf",
        ":/fonts/GeistMono-Variable.ttf",
        ":/fonts/SpaceGrotesk-Variable.ttf",
        ":/fonts/SpaceMono-Regular.ttf",
        ":/fonts/SpaceMono-Bold.ttf",
    };
    for (const QString &path : fontResources) {
        if (QFontDatabase::addApplicationFont(path) < 0)
            ppWarn() << "[Fonts] failed to load" << path;
    }

    app.setWindowIcon(QIcon(":/icons/pinpointstudio_256.png"));
    app.setDesktopFileName(QStringLiteral("pinpointstudio"));
    SecretsManager::initializeDefaults();

    // EventBuffer declared first — destroyed last (stack unwinds in reverse).
    // All controllers that hold a pointer to it must be destroyed first.
    pinpoint::EventBuffer   eventBuffer;
    eventBuffer.setLogCallback([](pinpoint::LogSeverity sev, const char *msg) {
        if (sev >= pinpoint::LogSeverity::Error)
            ppError() << msg;
        else
            ppWarn() << msg;
    });
    // Register the merger thread with the resource profiler (decoupled hook so the
    // Buffer library and its standalone tests stay free of any Core dependency).
    eventBuffer.setThreadRegisterHook([](const char *name) {
        pinpoint::osmetrics::registerThread(name);
    });
    // Merger runs for app lifetime. With no sources registered yet it
    // auto-pauses; the first registerSource() call auto-resumes it.
    eventBuffer.start();

    AppSettings              appSettings;
    // Read-only app/build/dependency info for the About box (appInfo context property).
    AppInfo                  appInfo;
    SecretsBridge            secrets;
    ImuManager              imuManager(&eventBuffer, &appSettings);
    TranscriptionController controller(&appSettings);
    TtsController           ttsController(&appSettings);
    LlmController           llmController(&appSettings);
    CameraManager           cameraManager(&eventBuffer, &appSettings);
    FilmController          filmController;
    BufferController        bufferController(&eventBuffer);
    AthleteController       athleteController;
    // Live lead-wrist metrics (vs neutral) for the wizard "Check your sensor" overlay.
    LiveWristAngles         liveWrist(&imuManager, &appSettings, &athleteController);
    SessionController       sessionController(&athleteController);
    NavigationController    navController(&athleteController, &sessionController);
    // In-app updater façade: Linux AppImage engine, WinSparkle on Windows, Sparkle on
    // macOS (all behind this one QML context property). Constructed on all platforms
    // for QML uniformity; inert ("unsupported"/"devbuild") in a dev/build-tree run or
    // where no engine is compiled in. See docs/design/{linux,windows,macos}_update.md.
    UpdateController        updateController(&appSettings, &sessionController);
    // Hardware-adaptive CUDA-runtime offer (Windows): detects an NVIDIA GPU and offers
    // the separately-packaged GPU runtime when present but not installed, so users who
    // add a GPU later adapt without reinstalling. Inert off Windows. See §4.4.
    CudaRuntimeController   cudaRuntime;
    // Hardware probe behind the "Motion capture quality" setting: gates the High
    // tier on an accelerated pose backend + enough graphics memory, and measures
    // per-swing analysis time. Lazy — refresh() is kicked from the General panel,
    // and it defers while a session is active (passed sessionController for that).
    MotionCaptureProbe      motionCaptureProbe(&sessionController);
    ResourceMonitorController resourceMonitor(&eventBuffer, &cameraManager, &imuManager);
    // Resource profiler bridge — owns the single 1 s gauge sampler and the 60 s
    // stats-dump cadence. Per-session profile: reset at session start, dump at end.
    ProfilerController        profilerController;
    QObject::connect(&sessionController, &SessionController::runningChanged,
                     &profilerController, [&sessionController, &profilerController]() {
        if (sessionController.running()) profilerController.reset();
        else                             profilerController.dumpToLog();
    });
    // Registers the shot-marker EventBuffer source; registering a first source
    // auto-resumes the buffer, so restore the user capture intent right after.
    ShotController            shotController(&eventBuffer, &sessionController);
    cameraManager.applyCaptureIntent();
    ShotListModel             shotModel;
    // Bulk "export selected shots to a zip" for the carousel ⋯ menu — derives
    // everything from the shot dirs it is handed, so it needs no dependencies.
    pinpoint::SwingZipExporter swingZipExporter;
    // Reload the current athlete's most recent session so prior shots survive a
    // restart (read-only for now — SwingDocReader parses the unified swing.json;
    // rating/note aren't persisted yet so they come back cleared).
    {
        const QString sessionDir = pinpoint::SwingDocReader::latestSessionDir(
            appSettings.athleteLibraryPath(), athleteController.currentName());
        shotModel.loadSessionDir(sessionDir);   // one disk-load path (also used on wrist entry)
        if (shotModel.activeCount())
            ppInfo() << "[Reload] restored" << shotModel.activeCount() << "shots from" << sessionDir;
    }
    // Session review: enumerate saved sessions + load one into a private shot
    // model. Constructed after the reload above so its synthesized live-session
    // row reflects the just-restored shots.
    SessionReviewController   sessionReviewController(&shotModel, &appSettings,
                                                      &athleteController);
    // Sessions and the live carousel are both athlete-scoped, but the reload above only
    // ever ran once, at startup — so switching athlete left BOTH showing the previous
    // athlete's shots. Re-point them whenever the current athlete changes.
    QObject::connect(&athleteController, &AthleteController::currentAthleteChanged,
                     &sessionReviewController, &SessionReviewController::onAthleteChanged);
    QObject::connect(&athleteController, &AthleteController::currentAthleteChanged,
                     &shotModel, [&shotModel, &appSettings, &athleteController] {
        shotModel.loadSessionDir(pinpoint::SwingDocReader::latestSessionDir(
            appSettings.athleteLibraryPath(), athleteController.currentName()));
    });
    // DATA-SOURCE transition (a past session opened/closed from disk). This is the
    // ONLY axis that pauses capture: opening a loaded session stops live capture
    // (which disarms the shot trigger via the buffer state) and gates ShotController;
    // the session/clock keep running so returning to live resumes the SAME session.
    // The live-capture record state is remembered here and restored on return, so
    // pressing Capture (→ resumeLive) reinstates exactly what was running — and stays
    // idle if capture was idle. The orthogonal session MODE (Capture/Replay/Analyse)
    // is owned by the SessionMode QML singleton; Main.qml lands a loaded session in
    // Replay and a return-to-live in Capture.
    QObject::connect(&sessionReviewController, &SessionReviewController::reviewActiveChanged,
                     [&sessionReviewController, &shotController, &cameraManager,
                      wasCapturing = false]() mutable {
        const bool review = sessionReviewController.reviewActive();
        shotController.setReviewActive(review);
        if (review) {
            wasCapturing = cameraManager.captureIntent();   // remember before pausing
            cameraManager.stopCapture();
        } else if (wasCapturing) {
            wasCapturing = false;
            cameraManager.startCapture();                   // restore on return to live
        }
    });
    // Declared after cameraManager so it is destroyed FIRST: ~ShotProcessor
    // joins the shot workers and destroys the SwingWindow before
    // ~CameraManager deregisters sources and ~EventBuffer frees ring memory.
    ShotProcessor             shotProcessor(&eventBuffer, &cameraManager, &imuManager,
                                            &appSettings, &athleteController,
                                            &sessionController, &shotModel);
    cameraManager.setShotProcessor(&shotProcessor);   // teardown stop-barrier
    imuManager.setShotProcessor(&shotProcessor);      // same barrier for IMU deselect
    // Disk-backed replay of saved shots (MP4 + swing.json) — independent of the
    // live SwingWindow that ShotProcessor owns for the just-captured shot.
    ShotReplayController      shotReplay(&appSettings);
    // Any review-state transition tears down an on-screen disk replay, so the
    // previous shot's replay stage + metric graph don't linger over the newly
    // selected (or resumed-live) session. reviewActiveChanged fires on every
    // loadSession()/resumeLive() — including session→session while already in
    // review — so this covers entering review, switching sessions, and resuming
    // live. stop() is idempotent (no-ops when nothing is replaying).
    QObject::connect(&sessionReviewController, &SessionReviewController::reviewActiveChanged,
                     &shotReplay, &ShotReplayController::stop);
    ClipboardHelper           clipboardHelper;

    // Re-analyse funnel for the carousel action bar: reloads each exported swing
    // (streaming SwingDiskLoader), re-runs the analyzer on a worker, writes the
    // result back, and emits reanalysed(dir) so the carousel refreshes the row in
    // whichever model is active (live or review). Owns no live shot state.
    ReanalysisController      reanalysisController;

    // Markup Lab — in-app ground-truth labelling of recorded swings; reads its own
    // swing.json/MP4 from disk (no buffer/SessionMode coupling), writes a
    // SwingLab-compatible truth.json sibling. The image provider is owned by the
    // engine (addImageProvider, below).
    MarkupController          markupController;

    // IMU source register/deregister can change the shared EventBuffer state
    // (first-source auto-resume / last-source auto-pause). Re-apply the user
    // capture intent so cameraManager.bufferState — the QML-facing buffer
    // state — stays correct and notifies.
    QObject::connect(&imuManager, &ImuManager::bufferStateChanged,
                     &cameraManager, &CameraManager::applyCaptureIntent);

    // Shot trigger arms/disarms with the buffer state. bufferStateChanged is
    // the single always-notified buffer-state signal (IMU-caused transitions
    // are forwarded through applyCaptureIntent above).
    QObject::connect(&cameraManager, &CameraManager::bufferStateChanged,
                     &shotController, &ShotController::reevaluateArmed);

    // Detected shots drive the processor pipeline (post-roll → window capture
    // → analysis ∥ export → carousel → replay); the processor's busy state
    // disarms the trigger for the whole pipeline.
    QObject::connect(&shotController, &ShotController::shotDetected,
                     &shotProcessor,  &ShotProcessor::onShotDetected);
    QObject::connect(&shotProcessor,  &ShotProcessor::busyChanged,
                     &shotController, [&shotController, &shotProcessor] {
        shotController.setProcessorBusy(shotProcessor.busy());
    });
    // Hold any in-progress re-analysis batch between swings while a live shot is
    // processing — keeps two ViTPose passes from oversubscribing the CPU / OOMing.
    QObject::connect(&shotProcessor,      &ShotProcessor::busyChanged,
                     &reanalysisController, [&reanalysisController, &shotProcessor] {
        reanalysisController.setLiveBusy(shotProcessor.busy());
    });

    // IMU impact auto-detection (shot detection P1, re-pointed at the P3
    // arbiter): candidates funnel through reportCandidate's hold/fuse window
    // rather than committing directly. Gated behind the autoDetectSwing
    // setting. Whole chain is GUI-thread, so a direct connection; the
    // armed() gate inside reportCandidate handles capturing/busy/review.
    QObject::connect(&imuManager, &ImuManager::impactDetected, &shotController,
                     [&shotController, &appSettings](qint64 estImpactUs, float conf) {
        if (appSettings.autoDetectSwing())
            shotController.reportCandidate(ShotController::Source::Imu,
                                           estImpactUs, conf);
    });

    // Acoustic onset auto-detection (shot detection P2, re-pointed at the P3
    // arbiter like the IMU path). TranscriptionController::impactDetected is
    // emitted on the AUDIO thread with est_t* already computed; the
    // &shotController context makes this a queued hop onto the GUI thread.
    controller.setAcousticLatencyUs(appSettings.audioDeviceLatencyUs());
    QObject::connect(&appSettings, &AppSettings::audioDeviceLatencyUsChanged,
                     &controller, [&controller, &appSettings] {
        controller.setAcousticLatencyUs(appSettings.audioDeviceLatencyUs());
    });
    // Microphone selection + acoustic sensitivity — pushed at startup and kept
    // live. setInputDevice before the first capture so the saved device is used.
    controller.setInputDevice(appSettings.audioInputDevice());
    QObject::connect(&appSettings, &AppSettings::audioInputDeviceChanged,
                     &controller, [&controller, &appSettings] {
        controller.setInputDevice(appSettings.audioInputDevice());
    });
    // Acoustic "sensitivity" [0,1] sets the absolute amplitude gate — the
    // candidate-open threshold floor that keeps quiet ticks/ambient from firing
    // (and from masking real impacts). Mapped on a log scale across the useful
    // envelope range: s=0 → 0.30 (only loud events), s=1 → 0.01 (very sensitive),
    // s=0.5 → ~0.055. The meter draws this level so the user can sit it between
    // their ambient/keyboard noise and their club impacts.
    const auto applyAcousticSensitivity = [&controller, &appSettings] {
        const double s = appSettings.acousticSensitivity();   // already clamped [0,1]
        controller.setAcousticMinLevel(0.01 * std::pow(30.0, 1.0 - s));
    };
    applyAcousticSensitivity();
    QObject::connect(&appSettings, &AppSettings::acousticSensitivityChanged,
                     &controller, applyAcousticSensitivity);

    // Run the microphone (hence the acoustic detector + its calibration) while a
    // capturing session is live. Without this the mic only ran on the Audio page
    // / calibration view, so acoustic detection — and the calibrated gate — never
    // applied during real sessions. captureIntent is session-stable (doesn't
    // toggle per-shot). Gated by the acoustic enable + autoDetectSwing so the mic
    // stays off whenever acoustic can't contribute a candidate.
    const auto applyShotAudio = [&controller, &cameraManager, &appSettings] {
        controller.setShotDetectionActive(cameraManager.captureIntent()
                                          && appSettings.acousticShotDetectionEnabled()
                                          && appSettings.autoDetectSwing());
    };
    applyShotAudio();
    QObject::connect(&cameraManager, &CameraManager::captureIntentChanged,
                     &controller, applyShotAudio);
    QObject::connect(&appSettings, &AppSettings::acousticShotDetectionEnabledChanged,
                     &controller, applyShotAudio);
    QObject::connect(&appSettings, &AppSettings::autoDetectSwingChanged,
                     &controller, applyShotAudio);
    QObject::connect(&controller, &TranscriptionController::impactDetected,
                     &shotController,
                     [&shotController, &appSettings](qint64 estImpactUs, float conf) {
        // Raw detector fires always (so calibration sees them); only feed the
        // arbiter when shot detection is on AND acoustic is enabled. Voice/STT
        // is independent and unaffected by acousticShotDetectionEnabled.
        if (appSettings.autoDetectSwing() && appSettings.acousticShotDetectionEnabled())
            shotController.reportCandidate(ShotController::Source::Acoustic,
                                           estImpactUs, conf);
    });
    // Vision corroboration (P3-G5): the v2 temporal ball detector's launch cliff
    // → the same funnel. CameraManager::ballLaunched already carries an absolute
    // impact time on the EventBuffer clock (stamped in CameraInstance). conf 0.6
    // is below the arbiter's 0.8 lone-candidate floor, so vision can only
    // corroborate IMU/acoustic, never commit a shot alone. (Precise frame
    // timestamps — vs today's age-from-now estimate — are a later refinement.)
    QObject::connect(&cameraManager, &CameraManager::ballLaunched, &shotController,
                     [&shotController, &appSettings](qint64 estImpactUs, float conf) {
        if (appSettings.autoDetectSwing())
            shotController.reportCandidate(ShotController::Source::Ball,
                                           estImpactUs, conf);
    });

    // Voice input: completed STT transcription → coach chat (when voice input enabled).
    QObject::connect(&controller, &TranscriptionController::transcriptionReceived,
                     &llmController, [&llmController](const QString &text) {
        if (llmController.voiceInputEnabled())
            llmController.sendMessage(text);
    });

    // Voice output: completed LLM response → TTS (when voice output enabled).
    QObject::connect(&llmController, &LlmController::responseReady,
                     &ttsController, [&ttsController, &llmController](const QString &text) {
        if (llmController.voiceOutputEnabled())
            ttsController.speak(text);
    });

    // Entering/clearing an API key in Settings → Cloud Fallback re-evaluates each
    // subsystem's cloud-fallback availability and re-applies the user's preference
    // (so a force-cloud toggle that was waiting on a key engages immediately).
    QObject::connect(&secrets, &SecretsBridge::keysChanged,
                     &controller,    &TranscriptionController::refreshCloudAvailability);
    QObject::connect(&secrets, &SecretsBridge::keysChanged,
                     &ttsController,  &TtsController::refreshCloudAvailability);
    QObject::connect(&secrets, &SecretsBridge::keysChanged,
                     &llmController,  &LlmController::refreshCloudAvailability);

    QQmlApplicationEngine engine;
    // Markup Lab frame source — engine takes ownership of the provider; the
    // controller keeps a non-owning pointer to push decoded frames into it.
    auto *markupProvider = new MarkupImageProvider();
    markupController.setImageProvider(markupProvider);
    engine.addImageProvider(QStringLiteral("markup"), markupProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("appSettings"),       &appSettings);
    engine.rootContext()->setContextProperty(QStringLiteral("appInfo"),           &appInfo);
    engine.rootContext()->setContextProperty(QStringLiteral("secrets"),           &secrets);
    engine.rootContext()->setContextProperty(QStringLiteral("athleteController"), &athleteController);
    engine.rootContext()->setContextProperty(QStringLiteral("navController"),     &navController);
    engine.rootContext()->setContextProperty(QStringLiteral("imuManager"),       &imuManager);
    engine.rootContext()->setContextProperty(QStringLiteral("controller"),       &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("ttsController"),    &ttsController);
    engine.rootContext()->setContextProperty(QStringLiteral("llmController"),    &llmController);
    engine.rootContext()->setContextProperty(QStringLiteral("cameraManager"),    &cameraManager);
    engine.rootContext()->setContextProperty(QStringLiteral("filmController"),   &filmController);
    engine.rootContext()->setContextProperty(QStringLiteral("bufferController"), &bufferController);
    engine.rootContext()->setContextProperty(QStringLiteral("resourceMonitor"),  &resourceMonitor);
    engine.rootContext()->setContextProperty(QStringLiteral("profiler"),         &profilerController);
    engine.rootContext()->setContextProperty(QStringLiteral("sessionController"), &sessionController);
    engine.rootContext()->setContextProperty(QStringLiteral("updateController"),   &updateController);
    engine.rootContext()->setContextProperty(QStringLiteral("cudaRuntime"),        &cudaRuntime);
    engine.rootContext()->setContextProperty(QStringLiteral("motionCaptureProbe"), &motionCaptureProbe);
    engine.rootContext()->setContextProperty(QStringLiteral("sessionReviewController"), &sessionReviewController);
    engine.rootContext()->setContextProperty(QStringLiteral("shotController"),    &shotController);
    engine.rootContext()->setContextProperty(QStringLiteral("shotProcessor"),     &shotProcessor);
    engine.rootContext()->setContextProperty(QStringLiteral("shotReplay"),        &shotReplay);
    engine.rootContext()->setContextProperty(QStringLiteral("shotModel"),         &shotModel);
    engine.rootContext()->setContextProperty(QStringLiteral("reanalysisController"), &reanalysisController);
    engine.rootContext()->setContextProperty(QStringLiteral("swingExporter"),     &swingZipExporter);
    engine.rootContext()->setContextProperty(QStringLiteral("liveWrist"),         &liveWrist);
    engine.rootContext()->setContextProperty(QStringLiteral("markupController"),  &markupController);
    engine.rootContext()->setContextProperty(QStringLiteral("clipboard"),         &clipboardHelper);

    // Clean merger shutdown before Qt tears down its event loop. Shut the platform
    // updater (WinSparkle on Windows) down first so its helper threads stop cleanly.
    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&eventBuffer, &updateController]() {
        updateController.shutdownUpdater();
        eventBuffer.stop();
    });

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("PinPointStudio", "Main");

    return QCoreApplication::exec();
}
