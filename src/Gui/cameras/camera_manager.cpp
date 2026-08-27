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

#include "camera_manager.h"

#include "camera_instance.h"
#include "shot_processor.h"
#include "video_input_factory.h"
#include "event_buffer.h"
#include "../Core/pp_debug.h"
#include "../Core/pp_settings.h"
#include "../Video/camera_capabilities.h"
#include "../Video/frame_crop.h"
#include <algorithm>

CameraManager::CameraManager(pinpoint::EventBuffer *buffer, AppSettings *appSettings, QObject *parent)
    : QObject(parent)
    , m_eventBuffer(buffer)
    , m_appSettings(appSettings)
{
    VideoInputFactory::enumerateDevices();

    const QList<Device> allDevices = DeviceEnumerator::instance()->devices();
    for (const Device &dev : allDevices) {
        if (dev.type != DeviceType::VideoInput) continue;
        CameraEntry entry;
        entry.device = dev;
        m_cameras.append(entry);
    }

    // Apply persisted per-device settings keyed by serial number.
    // Use m_appSettings when available so the live QML-bound object stays in sync.
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;

    const QStringList excluded    = s->cameraExcluded();
    const QVariantMap targetFps   = s->cameraTargetFps();
    const QVariantMap triggerMode = s->cameraTriggerMode();

    // Per-session enablement starts as a copy of the global exclusion list;
    // session edits never flow back to settings.
    m_sessionExcluded = excluded;

    // Seed alias for any newly-seen device so it always has a value.
    QVariantMap aliasMap   = s->cameraAlias();
    bool        aliasDirty = false;

    for (auto &cam : m_cameras) {
        const QString key = cameraKey(cam);
        if (!key.isEmpty()) {
            if (excluded.contains(key))    cam.excluded    = true;
            if (targetFps.contains(key))   cam.targetFps   = targetFps.value(key).toDouble();
            if (triggerMode.contains(key)) cam.triggerMode = triggerMode.value(key).toString();
            if (!aliasMap.contains(key)) {
                aliasMap[key] = QString(key).replace(QLatin1Char('|'), QLatin1Char(' '));
                aliasDirty = true;
            }
        }
    }
    if (aliasDirty)
        s->setCameraAlias(aliasMap);

    // cameraList() derives initialWidth/initialHeight from the persisted
    // crop, so a crop edit must refresh the list — disconnected placeholder
    // tiles resize to the new crop immediately, not on the next reconnect.
    if (m_appSettings)
        connect(m_appSettings, &AppSettings::cameraRoiChanged,
                this, &CameraManager::cameraListChanged);
}

CameraManager::~CameraManager()
{
    // Shot workers read ring memory through the live SwingWindow; abandoning
    // them would be a use-after-free. Normally ~ShotProcessor already ran
    // (declared after us in main.cpp) and nulled the pointer — this is the
    // defensive path.
    if (m_shotProcessor)
        m_shotProcessor->finishNowBlocking();

    // Stop all capture threads first so no frame events can be posted to the
    // main thread after we free ring memory or delete the controllers.
    for (auto &cam : m_cameras) {
        if (cam.controller)
            cam.controller->stopCapture();
    }

    if (m_eventBuffer) {
        if (m_eventBuffer->state() == pinpoint::BufferState::Capturing)
            m_eventBuffer->pause();
        if (m_eventBuffer->state() == pinpoint::BufferState::Paused) {
            for (auto &cam : m_cameras) {
                if (cam.controller)
                    cam.controller->deregisterFromBuffer();
            }
        }
        // If Idle (stop() already called via aboutToQuit), EventBuffer cleans
        // up its own sources — skip deregistration.
    }
    for (auto &cam : m_cameras) {
        delete cam.controller;
        cam.controller = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

QVariantList CameraManager::cameraList() const
{
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;
    const QVariantMap perspMap  = s->cameraPerspective();
    const QVariantMap aliasMap  = s->cameraAlias();
    const QVariantMap roiMap    = s->cameraRoi();

    QVariantList list;
    for (int i = 0; i < m_cameras.size(); ++i) {
        const CameraEntry &cam = m_cameras[i];
        const CameraCapabilities &cap = cam.device.capabilities;
        const QString key = cameraKey(cam);
        QVariantMap entry;
        entry[QStringLiteral("index")]       = i;
        entry[QStringLiteral("description")] = cam.device.description;
        entry[QStringLiteral("alias")]       = aliasMap.value(key).toString();
        entry[QStringLiteral("selected")]    = cam.selected;
        entry[QStringLiteral("vendorName")]  = cap.vendorName;
        entry[QStringLiteral("modelName")]   = cap.modelName;
        entry[QStringLiteral("serialNumber")] = cap.serialNumber;
        entry[QStringLiteral("cameraKey")]   = key;
        entry[QStringLiteral("interface")]   = interfaceString(cap.connectionInterface);
        // Full-sensor dims: GenICam cameras report a Range whose
        // defaultResolution is the CURRENT region (possibly a stale crop) —
        // the crop editor's pixel fields must reference the range maximum.
        int sensorW = cap.resolution.defaultResolution.width;
        int sensorH = cap.resolution.defaultResolution.height;
        if (cap.resolution.kind == CapabilityKind::Range
            && cap.resolution.widthRange.max > 0) {
            sensorW = cap.resolution.widthRange.max;
            sensorH = cap.resolution.heightRange.max;
        }
        entry[QStringLiteral("maxWidth")]    = sensorW;
        entry[QStringLiteral("maxHeight")]   = sensorH;
        // Initial (pre-connect) frame dims: the persisted crop applied to the
        // full sensor — mirrors CameraInstance's constructor seeding so a
        // disconnected placeholder tile already opens at the aspect the
        // stream will actually have once connected.
        int initW = sensorW, initH = sensorH;
        if (roiMap.contains(key) && sensorW > 0 && sensorH > 0) {
            const QVariantMap r = roiMap.value(key).toMap();
            const QRectF roi(r.value(QStringLiteral("x")).toDouble(),
                             r.value(QStringLiteral("y")).toDouble(),
                             r.value(QStringLiteral("w")).toDouble(),
                             r.value(QStringLiteral("h")).toDouble());
            if (pp_crop::cropIsActive(roi)) {
                const QRect c = pp_crop::snapCropRect(
                    roi.intersected(QRectF(0.0, 0.0, 1.0, 1.0)), sensorW, sensorH);
                if (!c.isEmpty()) { initW = c.width(); initH = c.height(); }
            }
        }
        entry[QStringLiteral("initialWidth")]  = initW;
        entry[QStringLiteral("initialHeight")] = initH;
        entry[QStringLiteral("pixelFormat")] = cap.pixelFormat.defaultFormat.nativeKey;
        entry[QStringLiteral("bitsPerPixel")] = cap.pixelFormat.defaultFormat.bitsPerPixel;
        entry[QStringLiteral("maxFps")]      = cap.frameRate.range.max;
        entry[QStringLiteral("roiSupported")] = cap.roi.supported;
        entry[QStringLiteral("hwTrigger")]   = cap.trigger.hasHardwareInput;
        entry[QStringLiteral("enabled")]     = !cam.excluded;
        entry[QStringLiteral("sessionEnabled")] = !m_sessionExcluded.contains(key);
        entry[QStringLiteral("perspective")] = perspMap.value(key, 0).toInt();

        // --- Ring buffer sizing fields (mirror CameraInstance's allocation logic) ---
        // Slot width/height: largest supported resolution, not just the default.
        int slotW = 0, slotH = 0;
        for (const Resolution &r : cap.resolution.presets) {
            if (r.width * r.height > slotW * slotH) { slotW = r.width; slotH = r.height; }
        }
        if (slotW == 0) { slotW = sensorW; slotH = sensorH; } // Range max / default
        if (slotW == 0) { slotW = 1920; slotH = 1080; }

        // Slot BPP: worst-case across all supported formats, minimum 2 bytes/pixel.
        int maxBpp = 2;
        for (const PixelFormat &pf : cap.pixelFormat.supported) {
            int bpp = pf.bitsPerPixel > 0 ? (pf.bitsPerPixel + 7) / 8 : 0;
            if (bpp == 0) {
                switch (pf.encoding) {
                case PixelEncoding::RGBA8:
                case PixelEncoding::BGR8:          bpp = 4; break;
                case PixelEncoding::BayerRG16:
                case PixelEncoding::BayerGB16:
                case PixelEncoding::YUV422_YUYV:
                case PixelEncoding::YUV422_UYVY:   bpp = 2; break;
                default:                           bpp = 2; break;
                }
            }
            maxBpp = std::max(maxBpp, bpp);
        }

        // Slot FPS: GenICam cameras use 200 fps as the conservative upper bound.
        double slotFps = cap.frameRate.range.max > 0.0 ? cap.frameRate.range.max : 60.0;
        if (cam.device.backend == VideoInputFactory::Backend::Aravis ||
            cam.device.backend == VideoInputFactory::Backend::Spinnaker)
            slotFps = 200.0;

        entry[QStringLiteral("slotWidth")]        = slotW;
        entry[QStringLiteral("slotHeight")]       = slotH;
        entry[QStringLiteral("slotBytesPerPixel")] = maxBpp;
        entry[QStringLiteral("slotFps")]          = slotFps;

        list.append(entry);
    }
    return list;
}

QVariantList CameraManager::instances() const
{
    QVariantList list;
    for (const auto &cam : m_cameras) {
        if (cam.selected && cam.controller)
            list.append(QVariant::fromValue(static_cast<QObject *>(cam.controller)));
    }
    return list;
}

CameraInstance *CameraManager::instanceFor(const QString &deviceId) const
{
    for (const auto &cam : m_cameras)
        if (cam.device.id == deviceId && cam.controller)
            return cam.controller;
    return nullptr;
}

CameraManager::CameraDeviceStats CameraManager::liveDeviceStats(const QString &deviceId) const
{
    CameraDeviceStats stats;
    for (const CameraEntry &e : m_cameras) {
        if (e.device.id != deviceId)
            continue;
        if (!e.controller)
            break;
        stats.sourceId  = e.controller->sourceId();
        stats.fps       = e.controller->cameraFps();
        stats.width     = e.controller->frameWidth();
        stats.height    = e.controller->frameHeight();
        stats.recording = e.controller->isRecording();
        stats.cropRoi   = e.controller->cropRoi();
        break;
    }
    return stats;
}

bool CameraManager::isRecording() const
{
    return m_recording;
}

bool CameraManager::anySelected() const
{
    for (const auto &cam : m_cameras)
        if (cam.selected) return true;
    return false;
}

bool CameraManager::anyConnecting() const
{
    for (const auto &cam : m_cameras)
        if (cam.controller && cam.controller->isConnecting()) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Invokables
// ---------------------------------------------------------------------------

void CameraManager::setSelected(int index, bool selected)
{
    if (index < 0 || index >= m_cameras.size()) return;
    if (m_cameras[index].selected == selected) return;

    m_cameras[index].selected = selected;

    // deregisterSource() asserts that no SwingWindow is live, and the shot
    // workers read ring memory through it. The processor joins its workers and
    // destroys the window before we touch source registration (blocking — the
    // correctness barrier).
    if (m_shotProcessor)
        m_shotProcessor->finishNowBlocking();

    // Snapshot buffer state before touching anything.
    bool wasCapturing = m_eventBuffer &&
                        m_eventBuffer->state() == pinpoint::BufferState::Capturing;

    // Any change to device selection requires the buffer to be Paused so that
    // registerSource() / deregisterSource() are called in a safe state.
    // Note: pause() → resume() clears all ring buffers. Any data captured
    // before this device change is discarded. This is intentional.
    if (wasCapturing) m_eventBuffer->pause();

    if (selected) {
        m_cameras[index].controller = createController(m_cameras[index].device);
        if (m_recording && m_cameras[index].controller)
            m_cameras[index].controller->startRecording();
    } else {
        CameraInstance *ctrl = m_cameras[index].controller;
        m_cameras[index].controller = nullptr;
        if (ctrl) {
            // Stop the capture thread synchronously before freeing ring memory.
            // The capture thread posts drainDisplayFrame events via QueuedConnection;
            // if we call deleteLater() while it is still running, a stale event can
            // fire on the freed object and corrupt the heap.  Stopping first ensures
            // no new frame events are queued after this point.
            ctrl->stopCapture();
            ctrl->deregisterFromBuffer();
            // Notify QML so the Repeater removes the delegate while the
            // controller is still alive; deleteLater() defers the actual
            // destruction until bindings have already been torn down.
            emit cameraListChanged();
            emit instancesChanged();
            ctrl->deleteLater();
        }
    }

    // Restore the buffer to the user's capture intent. This also corrects the
    // EventBuffer's silent first-source auto-resume inside registerSource():
    // intent off → re-paused immediately; intent on → capture continues.
    applyCaptureIntent();

    emit cameraListChanged();
    emit instancesChanged();
    emit anyConnectingChanged();   // a connecting instance may have just been created/destroyed
}

void CameraManager::startAll()
{
    if (m_recording) return;
    m_recording = true;
    emit isRecordingChanged();

    // Starts the camera recording pipelines only. Buffer state is owned by the
    // user capture intent (startCapture()/stopCapture()) — starting recording
    // neither pauses nor resumes the ring.
    for (auto &cam : m_cameras) {
        if (cam.selected && cam.controller)
            cam.controller->startRecording();
    }
}

void CameraManager::stopAll()
{
    if (!m_recording) return;
    // Note: an in-progress shot replay is NOT cancelled here — it reads the
    // frozen SwingWindow, not the live pipelines, so stopping recording is
    // harmless to it. Buffer intent stays owned by Capture/Stop.
    for (auto &cam : m_cameras) {
        if (cam.selected && cam.controller)
            cam.controller->stopRecording();
    }
    m_recording = false;
    emit isRecordingChanged();
}

void CameraManager::disconnectAll()
{
    stopAll();
    // setSelected owns the full per-camera teardown (shot-processor stop
    // barrier, capture-thread stop, deregister, intent restore) — reuse it
    // rather than batching, so the producer contract stays in one place.
    for (int i = 0; i < m_cameras.size(); ++i)
        if (m_cameras[i].selected)
            setSelected(i, false);
}

void CameraManager::pauseBuffer()
{
    if (m_eventBuffer && m_eventBuffer->state() == pinpoint::BufferState::Capturing) {
        m_eventBuffer->pause();
        emit bufferStateChanged();
    }
}

void CameraManager::resumeBuffer()
{
    // Hard backstop: the SwingWindow (and the shot workers reading through it)
    // is only valid while the buffer stays Paused. All resume paths funnel
    // through here — including QML — so a live window always blocks
    // resumption; ShotProcessor::finishShot() re-applies the capture intent
    // once the window dies.
    if (m_eventBuffer && m_eventBuffer->swingWindowLive())
        return;
    if (m_eventBuffer && m_eventBuffer->state() == pinpoint::BufferState::Paused) {
        m_eventBuffer->resume();
        emit bufferStateChanged();
    }
}

void CameraManager::startCapture()
{
    const bool changed = !m_captureUserEnabled;
    m_captureUserEnabled = true;
    applyCaptureIntent();
    if (changed) emit captureIntentChanged();
}

void CameraManager::stopCapture()
{
    const bool changed = m_captureUserEnabled;
    m_captureUserEnabled = false;
    applyCaptureIntent();
    if (changed) emit captureIntentChanged();
}

void CameraManager::applyCaptureIntent()
{
    // Funnels through the wrappers: pauseBuffer() no-ops unless Capturing,
    // resumeBuffer() no-ops unless Paused, is blocked while a SwingWindow is
    // live, and stays paused when no sources are registered (the buffer's
    // no_source_paused_ contract).
    if (m_captureUserEnabled)
        resumeBuffer();
    else
        pauseBuffer();
    // Unconditional: callers invoke this after register/deregister, where the
    // buffer may have auto-resumed/auto-paused without a wrapper transition —
    // guarantee QML re-reads bufferState regardless.
    emit bufferStateChanged();
}

QString CameraManager::bufferState() const
{
    if (!m_eventBuffer) return QStringLiteral("unavailable");
    switch (m_eventBuffer->state()) {
    case pinpoint::BufferState::Idle:      return QStringLiteral("idle");
    case pinpoint::BufferState::Capturing: return QStringLiteral("capturing");
    case pinpoint::BufferState::Paused:    return QStringLiteral("paused");
    case pinpoint::BufferState::Stopping:  return QStringLiteral("stopping");
    }
    return QStringLiteral("unknown");
}

void CameraManager::enumerate()
{
    VideoInputFactory::enumerateDevices();

    // Merge any newly discovered video devices into m_cameras.
    // Existing entries are matched by device id and left untouched so their
    // controllers, settings and selected state are preserved.
    const QList<Device> allDevices = DeviceEnumerator::instance()->devices();
    bool added = false;
    for (const Device &dev : allDevices) {
        if (dev.type != DeviceType::VideoInput) continue;
        bool found = false;
        for (const auto &cam : m_cameras) {
            if (cam.device.id == dev.id) { found = true; break; }
        }
        if (!found) {
            CameraEntry entry;
            entry.device = dev;
            // Apply any persisted settings for this device.
            AppSettings  efallback;
            AppSettings *es = m_appSettings ? m_appSettings : &efallback;
            const QString key = cameraKey(entry);
            if (es->cameraExcluded().contains(key))    entry.excluded    = true;
            if (es->cameraTargetFps().contains(key))   entry.targetFps   = es->cameraTargetFps().value(key).toDouble();
            if (es->cameraTriggerMode().contains(key)) entry.triggerMode = es->cameraTriggerMode().value(key).toString();
            m_cameras.append(entry);
            added = true;
        }
    }

    if (added) {
        emit cameraListChanged();
        emit instancesChanged();
    }
}

void CameraManager::setExcluded(int index, bool excluded)
{
    if (index < 0 || index >= m_cameras.size()) return;
    if (m_cameras[index].excluded == excluded) return;

    m_cameras[index].excluded = excluded;

    // Keep the session list consistent with a global change made mid-session:
    // globally disabling a camera disables it for this session too, and vice versa.
    const QString sessionKey = cameraKey(m_cameras[index]);
    if (!sessionKey.isEmpty()) {
        if (excluded && !m_sessionExcluded.contains(sessionKey)) {
            m_sessionExcluded.append(sessionKey);
            emit sessionCameraExcludedChanged();
        } else if (!excluded && m_sessionExcluded.contains(sessionKey)) {
            m_sessionExcluded.removeAll(sessionKey);
            emit sessionCameraExcludedChanged();
        }
    }

    if (excluded && m_cameras[index].selected) {
        setSelected(index, false);
    } else if (!excluded && !m_cameras[index].selected) {
        setSelected(index, true);
    }

    // Persist to AppSettings keyed by serial number.
    const QString key = cameraKey(m_cameras[index]);
    if (!key.isEmpty()) {
        AppSettings  xfallback;
        AppSettings *xs = m_appSettings ? m_appSettings : &xfallback;
        QStringList excList = xs->cameraExcluded();
        if (excluded)
            excList.append(key);
        else
            excList.removeAll(key);
        xs->setCameraExcluded(excList);
    }

    emit cameraListChanged();
}

QStringList CameraManager::sessionCameraExcluded() const
{
    return m_sessionExcluded;
}

void CameraManager::setSessionCameraEnabled(const QString &cameraKey, bool on)
{
    if (cameraKey.isEmpty())
        return;
    const bool excluded = m_sessionExcluded.contains(cameraKey);
    if (on != excluded)
        return;   // already in the requested state

    if (on)
        m_sessionExcluded.removeAll(cameraKey);
    else
        m_sessionExcluded.append(cameraKey);

    // Disabling a connected camera disconnects it; enabling never auto-connects
    // (Connect in the toolbar panel does that explicitly).
    if (!on) {
        for (int i = 0; i < m_cameras.size(); ++i) {
            if (CameraManager::cameraKey(m_cameras[i]) == cameraKey) {
                if (m_cameras[i].selected)
                    setSelected(i, false);
                break;
            }
        }
    }

    emit sessionCameraExcludedChanged();
    emit cameraListChanged();
}

bool CameraManager::livePoseEnabled() const
{
    return m_livePoseEnabled;
}

void CameraManager::setLivePoseEnabled(bool on)
{
    if (m_livePoseEnabled == on)
        return;
    m_livePoseEnabled = on;
    for (auto &cam : m_cameras) {
        if (cam.controller)
            cam.controller->setPoseEnabled(on);
    }
    emit livePoseEnabledChanged();
}

void CameraManager::setTargetFps(int index, double fps)
{
    if (index < 0 || index >= m_cameras.size()) return;
    m_cameras[index].targetFps = fps;

    // ── "Config from PPS": for a PPCP camera this now DOES something ────────
    //
    // ⚠ AND FOR EVERY OTHER BACKEND IT STILL DOES NOT.  `VideoInputBase` has no
    // frame-rate setter for any local camera, so the chips in Settings ->
    // Cameras have always written a number nothing read.  That is unchanged and
    // is a separate piece of work; what is closed here is the PHONE case, which
    // is the one the protocol always carried and the one "config from PPS"
    // turned out to mean.
    //
    // A phone's rate is not a knob — it is a choice among the CaptureProfiles
    // that peer DECLARED (5.11), so the request is resolved to a declared
    // profile and refused to nothing if none matches.  Persisted per camera so
    // the choice survives the phone reconnecting, which it will do constantly.
#ifdef HAVE_PPCP
    CameraEntry &cam = m_cameras[index];
    if (cam.device.backend != VideoInputFactory::Backend::Ppcp) return;

    // Only a LIVE instance can resolve a rate to a profile, because the
    // profiles are the counterpart's declaration and nothing else holds them.
    // With no instance running there is nothing to resolve against and the
    // request is refused rather than guessed at.
    const QString profileId =
        cam.controller ? cam.controller->ppcpProfileForRate(fps) : QString();
    if (profileId.isEmpty()) {
        ppWarn() << "[ppcp] no declared capture profile near" << fps
                 << "fps for" << cam.device.description
                 << (cam.controller ? "" : "(no live instance to ask)");
        return;
    }
    // Keyed on the PPCP device id, which is what CameraInstance reads back.
    QSettings s = ppSettings();
    s.setValue(QStringLiteral("ppcp/captureProfile/") + cam.device.id, profileId);
    cam.controller->setPpcpCaptureProfile(profileId);
    ppWarn() << "[ppcp] capture profile for" << cam.device.description << "->" << profileId
             << "(" << fps << "fps requested)";
#endif
}

void CameraManager::setTriggerMode(int index, const QString &mode)
{
    if (index < 0 || index >= m_cameras.size()) return;
    m_cameras[index].triggerMode = mode;
}

void CameraManager::setPerspective(QObject *rawController, int perspective)
{
    auto *target = qobject_cast<CameraInstance *>(rawController);
    if (!target)
        return;

    // Any number of cameras may share a perspective (e.g. two face-on cameras
    // in one session) — assignment is per-camera, nothing is cleared.
    target->setPerspective(perspective);
    emit cameraListChanged();
}

void CameraManager::setIsMirrored(QObject *rawController, bool mirrored)
{
    auto *target = qobject_cast<CameraInstance *>(rawController);
    if (!target)
        return;

    target->setIsMirrored(mirrored);

    AppSettings  fallback;
    AppSettings *ps = m_appSettings ? m_appSettings : &fallback;
    QVariantMap map = ps->cameraIsMirrored();
    for (const auto &cam : m_cameras) {
        if (cam.controller == target) {
            if (mirrored)
                map[cameraKey(cam)] = true;
            else
                map.remove(cameraKey(cam));
            break;
        }
    }
    ps->setCameraIsMirrored(map);
}

void CameraManager::relearnBallBaseline(QObject *rawController)
{
    if (auto *target = qobject_cast<CameraInstance *>(rawController))
        target->relearnBallBaseline();
}

void CameraManager::setBallRoi(QObject *rawController, QRectF roi)
{
    auto *target = qobject_cast<CameraInstance *>(rawController);
    if (!target)
        return;

    roi = roi.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (roi.isEmpty()) {
        clearBallRoi(rawController);
        return;
    }

    // Profile invalidation on ROI change lives in CameraInstance::setRoi —
    // the live drag-resize path updates the instance directly, so the
    // manager cannot be the only gate.
    target->setRoi(roi);

    AppSettings  fallback;
    AppSettings *ps = m_appSettings ? m_appSettings : &fallback;
    QVariantMap map = ps->cameraBallRoi();
    for (const auto &cam : m_cameras) {
        if (cam.controller == target) {
            map[cameraKey(cam)] = QVariantMap{
                {QStringLiteral("x"), roi.x()},
                {QStringLiteral("y"), roi.y()},
                {QStringLiteral("w"), roi.width()},
                {QStringLiteral("h"), roi.height()},
            };
            break;
        }
    }
    ps->setCameraBallRoi(map);
}

void CameraManager::clearBallRoi(QObject *rawController)
{
    auto *target = qobject_cast<CameraInstance *>(rawController);
    if (!target)
        return;

    target->clearRoi();

    AppSettings  fallback;
    AppSettings *ps = m_appSettings ? m_appSettings : &fallback;
    QVariantMap map = ps->cameraBallRoi();
    for (const auto &cam : m_cameras) {
        if (cam.controller == target) {
            map.remove(cameraKey(cam));
            break;
        }
    }
    ps->setCameraBallRoi(map);
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

QObject *CameraManager::createPreviewInstance(int index)
{
    if (index < 0 || index >= m_cameras.size()) return nullptr;

    // nullptr buffer → no ring-buffer registration, no pose/throttle pipeline
    auto *ctrl = new CameraInstance(m_cameras[index].device, nullptr, m_appSettings, this);
#ifdef HAVE_PPCP_TRANSPORT
    // Only this preview path is wired to a live PPCP peer for now — real
    // capture (createController(), below) stays exactly as unable to start a
    // PPCP camera as it already is; wider capture integration is separate,
    // later work.
    ctrl->setPpcpHostService(m_ppcpHostService);
#endif

    const QString key = cameraKey(m_cameras[index]);
    AppSettings  rfallback;
    AppSettings *rs = m_appSettings ? m_appSettings : &rfallback;

    const QVariantMap roiMap = rs->cameraRoi();
    if (roiMap.contains(key)) {
        const QVariantMap r = roiMap.value(key).toMap();
        double x = r.value(QStringLiteral("x")).toDouble();
        double y = r.value(QStringLiteral("y")).toDouble();
        double w = r.value(QStringLiteral("w")).toDouble();
        double h = r.value(QStringLiteral("h")).toDouble();
        if (w > 0 && h > 0)
            ctrl->setCropRoi(QRectF(x, y, w, h));
    }

    return ctrl;
}

void CameraManager::destroyPreviewInstance(QObject *ctrl)
{
    auto *inst = qobject_cast<CameraInstance *>(ctrl);
    if (!inst) return;
    // Stop the capture thread synchronously BEFORE deleteLater — same
    // rationale as the controller teardown in setSelected (stale queued
    // frame events firing on a freed object), plus one more: it serializes
    // the preview's camera release ahead of any reconnect that follows this
    // call. Otherwise a stale Spinnaker handle can DeInit the device after a
    // new instance has begun streaming it (-1004), or worse, deinitialize it
    // mid-start under the new owner.
    inst->stopCapture();
    inst->deleteLater();
}

CameraInstance *CameraManager::createController(const Device &device)
{
    auto *ctrl = new CameraInstance(device, m_eventBuffer, m_appSettings, this);
    // Ball presence is signal-only: CameraInstance::ballPresentChanged feeds the
    // QML overlays (and future shot detection) but never touches buffer state.

    // Re-emit the aggregate connecting state whenever this instance's connect
    // window opens or closes, so the aggregate Connect button animates.
    connect(ctrl, &CameraInstance::isConnectingChanged,
            this, &CameraManager::anyConnectingChanged);

    // Forward struck-ball launches to the shot-detection funnel (main.cpp wires
    // CameraManager::ballLaunched → shotController.reportCandidate(Source::Ball)).
    connect(ctrl, &CameraInstance::ballLaunched, this, &CameraManager::ballLaunched);

    // Apply the session-wide pipeline configuration to the new instance.
    ctrl->setPoseEnabled(m_livePoseEnabled);

    // Restore persisted ROI and perspective for this device.
    const QString key = device.description + QStringLiteral("|")
                        + (device.capabilities.serialNumber.isEmpty()
                               ? device.id
                               : device.capabilities.serialNumber);

    AppSettings  cfallback;
    AppSettings *cs = m_appSettings ? m_appSettings : &cfallback;

    const QVariantMap roiMap = cs->cameraRoi();
    if (roiMap.contains(key)) {
        const QVariantMap r = roiMap.value(key).toMap();
        double x = r.value(QStringLiteral("x")).toDouble();
        double y = r.value(QStringLiteral("y")).toDouble();
        double w = r.value(QStringLiteral("w")).toDouble();
        double h = r.value(QStringLiteral("h")).toDouble();
        if (w > 0 && h > 0)
            ctrl->setCropRoi(QRectF(x, y, w, h));
    }

    const QVariantMap perspMap = cs->cameraPerspective();
    if (perspMap.contains(key)) {
        int p = perspMap.value(key).toInt();
        if (p > 0)
            ctrl->setPerspective(p);
    }

    const QVariantMap mirrorMap = cs->cameraIsMirrored();
    if (mirrorMap.contains(key))
        ctrl->setIsMirrored(mirrorMap.value(key).toBool());

    // Hitting-area ROI — only restored for a fixed-in-place camera (a moved
    // camera makes the stored area meaningless; the map entry is kept so
    // re-fixing the camera brings it back).
    if (cs->cameraFixedInPlace().value(key).toBool()) {
        const QVariantMap ballRoiMap = cs->cameraBallRoi();
        if (ballRoiMap.contains(key)) {
            const QVariantMap r = ballRoiMap.value(key).toMap();
            const QRectF roi(r.value(QStringLiteral("x")).toDouble(),
                             r.value(QStringLiteral("y")).toDouble(),
                             r.value(QStringLiteral("w")).toDouble(),
                             r.value(QStringLiteral("h")).toDouble());
            if (!roi.isEmpty()) {
                // Restore the saved hitting area; the v2 temporal detector
                // self-calibrates its baseline from the empty mat at runtime
                // (no saved profile to apply).
                ctrl->setRoi(roi);
            }
        }
    }

    return ctrl;
}

std::vector<CameraInstance *> CameraManager::liveCameraInstances() const
{
    std::vector<CameraInstance *> out;
    for (const auto &cam : m_cameras) {
        if (cam.selected && cam.controller)
            out.push_back(cam.controller);
    }
    return out;
}

void CameraManager::setCameraAlias(const QString &key, const QString &alias)
{
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;

    QVariantMap map = s->cameraAlias();
    const QString trimmed = alias.trimmed();
    const QString current = map.value(key).toString();

    // No-op if the alias hasn't actually changed.
    const bool changed = trimmed.isEmpty() ? map.contains(key) : (current != trimmed);
    if (!changed) return;

    if (trimmed.isEmpty())
        map.remove(key);
    else
        map[key] = trimmed;
    s->setCameraAlias(map);

    // Push the new alias to any live controller so PpCameraFrame.qml updates immediately.
    for (const auto &cam : m_cameras) {
        if (cameraKey(cam) == key && cam.controller)
            cam.controller->setDeviceAlias(trimmed);
    }

    emit cameraListChanged();
}
