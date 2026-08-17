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

#include "imu_manager.h"
#include "shot_processor.h"

#include "ble_adapter_pool.h"
#include "event_buffer.h"
#include "hm_instance.h"
#include "imu_instance.h"
#include "pp_os_metrics.h"

ImuManager::ImuManager(pinpoint::EventBuffer *buffer, AppSettings *appSettings, QObject *parent)
    : QObject(parent)
    , m_eventBuffer(buffer)
    , m_appSettings(appSettings)
{
    // Enumerate local BT adapters before scanning so multi-adapter discovery
    // and round-robin connection assignment are both ready from the start.
    BleAdapterPool::instance()->initialize();

    // The shared IMU I/O thread — always running (an idle event loop is free);
    // every instance's driver + worker is moved onto it at creation.
    m_ioThread.setObjectName(QStringLiteral("ImuIoThread"));
    // Register the shared IMU I/O thread with the resource profiler. The
    // context-free 3-arg connect runs the functor directly in the emitting
    // (started) context — i.e. on the I/O thread itself.
    connect(&m_ioThread, &QThread::started, []() {
        pinpoint::osmetrics::registerThread("IMU.IO");
    });
    m_ioThread.start();

    // Seed the per-session exclusion list from the persisted global enablement
    // (mirrors CameraManager). Track the global list so mid-session Settings
    // changes can be diffed into the session list below.
    {
        AppSettings  fallback;
        AppSettings *s = m_appSettings ? m_appSettings : &fallback;
        m_sessionExcluded     = s->imuExcluded();
        m_lastGlobalExcluded  = m_sessionExcluded;
    }
    if (m_appSettings) {
        // Keep the session list consistent with global changes made mid-session
        // in Settings → IMUs: globally disabling a device disables it for this
        // session too (and disconnects it), and vice versa. Deliberate session
        // overrides of UNCHANGED devices are preserved (diff-based).
        connect(m_appSettings, &AppSettings::imuExcludedChanged, this, [this]() {
            const QStringList now  = m_appSettings->imuExcluded();
            const QStringList prev = m_lastGlobalExcluded;
            for (const QString &id : now)
                if (!prev.contains(id)) setSessionImuEnabled(id, false);
            for (const QString &id : prev)
                if (!now.contains(id)) setSessionImuEnabled(id, true);
            m_lastGlobalExcluded = now;
        });
    }

    // Surface BLE discovery errors (Bluetooth off / no adapter) so the IMU UI can
    // show an actionable message rather than an empty list.
    connect(DeviceEnumerator::instance(), &DeviceEnumerator::imuScanError,
            this, [this](const QString &msg) { setImuScanError(msg); });

    // A completed scan may have left a previously-seen device behind (powered
    // off / out of range). Re-emit so the chip lists re-evaluate each device's
    // "present" flag and hide/dim the ones absent from this scan. A still-
    // selected device stays visible (isImuPresent), so this can't drop a
    // connected IMU that simply stopped advertising.
    connect(DeviceEnumerator::instance(), &DeviceEnumerator::imuScanFinished,
            this, [this]() {
        emit imuListChanged();
        emit imuDeviceListChanged();
    });

    // Start the async BLE scan.  Results arrive via deviceAdded and are
    // automatically reflected by imuList() / imuDeviceList() reading directly
    // from DeviceEnumerator — no local list copy needed.
    DeviceEnumerator::instance()->scanImu();

    // Emit property-change signals when new devices are registered so QML
    // Repeaters rebuild their chips / rows.
    connect(DeviceEnumerator::instance(), &DeviceEnumerator::deviceAdded,
            this, [this](const Device &dev) {
        if (dev.type != DeviceType::Imu) return;
        setImuScanError(QString());   // a device appeared — discovery is healthy
        // Seed alias for newly-seen device so it always has a value.
        const QString imuKey = dev.description + QStringLiteral("|") + dev.id;
        AppSettings  fallback;
        AppSettings *s = m_appSettings ? m_appSettings : &fallback;
        QVariantMap aliasMap = s->imuAlias();
        if (!aliasMap.contains(imuKey)) {
            aliasMap[imuKey] = QString(imuKey).replace(QLatin1Char('|'), QLatin1Char(' '));
            s->setImuAlias(aliasMap);
        }
        emit imuListChanged();
        emit imuDeviceListChanged();
        emit imuEnumeratedCountChanged();
    });
}

ImuManager::~ImuManager()
{
    // Join the shot workers and destroy any live SwingWindow before freeing
    // ring memory under them (main.cpp declares the processor after the
    // managers, so normally ~ShotProcessor already ran and cleared this).
    if (m_shotProcessor)
        m_shotProcessor->finishNowBlocking();

    // Producer stop-barrier for EVERY live instance, UNCONDITIONALLY. stop()
    // (sever driver→worker, BLE disconnect, detachBuffer) is valid in any buffer
    // state and MUST run even on app-quit — where aboutToQuit has already called
    // eventBuffer.stop() (buffer → Idle), so the state-gated deregister block
    // below is skipped. Without this the BLE link would be dropped only by
    // ~BleImuTransport instead of a clean disconnectFromDevice(), and the worker's
    // detachBuffer() barrier would rely on the ring's isCapturing() guard rather
    // than running by contract. It also severs the queued impactDetected wire
    // before the synchronous delete below (P2-7).
    for (auto &entry : m_selected)
        if (entry.instance) entry.instance->stop();

    // Deregistration requires the buffer paused (EventBuffer producer contract —
    // deregisterSource() asserts it). Only reachable while the buffer is still
    // live; on app-quit it is already Idle and there is nothing to deregister.
    if (m_eventBuffer) {
        const bool wasCapturing =
            m_eventBuffer->state() == pinpoint::BufferState::Capturing;
        if (wasCapturing) m_eventBuffer->pause();

        if (m_eventBuffer->state() == pinpoint::BufferState::Paused) {
            for (auto &entry : m_selected)
                if (entry.instance) entry.instance->deregisterFromBuffer();
        }
    }
    for (auto &entry : m_selected)
        delete entry.instance;

    // Join the I/O thread LAST: the instance destructors queue their driver/
    // worker deleteLater events onto its loop, and quit() is processed after
    // those (posted later), so everything living on the thread is destroyed
    // there before it exits.
    m_ioThread.quit();
    m_ioThread.wait();
}

// ---------------------------------------------------------------------------
// Properties — both list accessors read from DeviceEnumerator directly
// ---------------------------------------------------------------------------

bool ImuManager::isImuPresent(const Device &dev) const
{
    // Seen in the most recently completed scan (or a later in-progress one), or
    // currently selected — a connected device stops advertising and so would be
    // absent from the scan, but must stay visible.
    const int completedGen = DeviceEnumerator::instance()->completedImuScanGeneration();
    return dev.lastSeenScanGeneration >= completedGen
        || m_selected.value(dev.id).selected;
}

QVariantList ImuManager::imuList() const
{
    const QList<Device> devs = DeviceEnumerator::instance()->devices(DeviceType::Imu);
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;
    const QVariantMap aliasMap = s->imuAlias();
    QVariantList list;
    for (int i = 0; i < devs.size(); ++i) {
        const Device &dev = devs[i];
        const ImuEntry &entry = m_selected.value(dev.id);
        const bool connected  = entry.instance && entry.instance->imuConnected();
        const bool connecting = entry.instance && entry.instance->busy() && !connected;
        const QString imuKey  = dev.description + QStringLiteral("|") + dev.id;
        QVariantMap m;
        m[QStringLiteral("index")]       = i;
        m[QStringLiteral("id")]          = dev.id;
        m[QStringLiteral("description")] = dev.description;
        m[QStringLiteral("alias")]       = aliasMap.value(imuKey).toString();
        m[QStringLiteral("transport")]   = (dev.imuTransport == ImuBase::Transport::Ble)
                                               ? QStringLiteral("BLE")
                                               : QStringLiteral("Serial");
        m[QStringLiteral("selected")]    = entry.selected;
        m[QStringLiteral("connected")]   = connected;
        m[QStringLiteral("connecting")]  = connecting;
        m[QStringLiteral("present")]     = isImuPresent(dev);
        list.append(m);
    }
    return list;
}

QVariantList ImuManager::imuDeviceList() const
{
    const QList<Device> devs = DeviceEnumerator::instance()->devices(DeviceType::Imu);
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;
    const QVariantMap aliasMap = s->imuAlias();
    QVariantList list;
    for (int i = 0; i < devs.size(); ++i) {
        const Device &dev = devs[i];
        const ImuCapabilities &cap = dev.imuCapabilities;
        const QString imuKey = dev.description + QStringLiteral("|") + dev.id;
        QVariantMap entry;
        entry[QStringLiteral("index")]       = i;
        entry[QStringLiteral("id")]          = dev.id;
        entry[QStringLiteral("imuKey")]      = imuKey;
        entry[QStringLiteral("description")] = dev.description;
        entry[QStringLiteral("alias")]       = aliasMap.value(imuKey).toString();
        entry[QStringLiteral("transport")]   = (dev.imuTransport == ImuBase::Transport::Ble)
                                                   ? QStringLiteral("BLE")
                                                   : QStringLiteral("Serial");
        // The device KIND, not the capability struct's free-text vendorName —
        // QML branches on this string (ImusPanel's isHackMotion) rather than
        // guessing from the model name.
        entry[QStringLiteral("vendor")]      = (dev.imuVendor == ImuVendor::HackMotion)
                                                   ? QStringLiteral("hackmotion")
                                                   : QStringLiteral("witmotion");
        entry[QStringLiteral("vendorName")]     = cap.vendorName;
        entry[QStringLiteral("modelName")]      = cap.modelName;
        entry[QStringLiteral("serialNumber")]   = cap.serialNumber;
        entry[QStringLiteral("firmwareVersion")] = cap.firmwareVersion;
        entry[QStringLiteral("hasAccelerometer")]  = cap.hasAccelerometer;
        entry[QStringLiteral("hasGyroscope")]      = cap.hasGyroscope;
        entry[QStringLiteral("hasMagnetometer")]   = cap.hasMagnetometer;
        entry[QStringLiteral("hasBattery")]        = cap.hasBattery;
        entry[QStringLiteral("accelRangeMax")]     = cap.accelRange.max;
        entry[QStringLiteral("gyroRangeMax")]      = cap.gyroRange.max;
        QVariantList ratesList;
        for (int r : cap.supportedRatesHz) ratesList.append(r);
        entry[QStringLiteral("supportedRatesHz")]  = ratesList;
        entry[QStringLiteral("defaultRateHz")]     = cap.defaultRateHz;
        entry[QStringLiteral("supportsSixAxisFusion")]       = cap.supportsSixAxisFusion;
        entry[QStringLiteral("supportsNineAxisFusion")]      = cap.supportsNineAxisFusion;
        entry[QStringLiteral("supportsHorizontalMount")]     = cap.supportsHorizontalMount;
        entry[QStringLiteral("supportsVerticalMount")]       = cap.supportsVerticalMount;
        entry[QStringLiteral("supportsAngleReference")]      = cap.supportsAngleReference;
        entry[QStringLiteral("supportsHeadingZero")]         = cap.supportsHeadingZero;
        entry[QStringLiteral("supportsMagCalibration")]      = cap.supportsMagCalibration;
        entry[QStringLiteral("supportsAccelGyroCalibration")] = cap.supportsAccelGyroCalibration;
        entry[QStringLiteral("supportsConfigPersistence")]   = cap.supportsConfigPersistence;
        entry[QStringLiteral("sessionEnabled")] = !m_sessionExcluded.contains(dev.id);
        entry[QStringLiteral("present")]        = isImuPresent(dev);
        list.append(entry);
    }
    return list;
}

QStringList ImuManager::sessionImuExcluded() const
{
    return m_sessionExcluded;
}

void ImuManager::setSessionImuEnabled(const QString &deviceId, bool on)
{
    if (deviceId.isEmpty()) return;
    const bool excluded = m_sessionExcluded.contains(deviceId);
    if (on != excluded) return;   // already in the requested state

    if (on)
        m_sessionExcluded.removeAll(deviceId);
    else
        m_sessionExcluded.append(deviceId);

    // Disabling a selected/connected device must actually disconnect it so it
    // leaves the session. Enabling never auto-connects — Connect does that.
    if (!on && m_selected.value(deviceId).selected) {
        const QList<Device> devs = DeviceEnumerator::instance()->devices(DeviceType::Imu);
        for (int i = 0; i < devs.size(); ++i)
            if (devs[i].id == deviceId) { setSelected(i, false); break; }
    }

    emit sessionImuExcludedChanged();
    emit imuDeviceListChanged();
}

QVariantList ImuManager::instances() const
{
    QVariantList list;
    for (const auto &entry : m_selected) {
        if (entry.selected && entry.instance)
            list.append(QVariant::fromValue(static_cast<QObject *>(entry.instance)));
    }
    return list;
}

bool ImuManager::anySelected() const
{
    for (const auto &entry : m_selected)
        if (entry.selected) return true;
    return false;
}

int ImuManager::imuEnumeratedCount() const
{
    return DeviceEnumerator::instance()->devices(DeviceType::Imu).size();
}

bool ImuManager::imuConnected() const
{
    for (const auto &entry : m_selected)
        if (entry.instance && entry.instance->imuConnected()) return true;
    return false;
}

int ImuManager::imuCount() const
{
    int n = 0;
    for (const auto &entry : m_selected)
        if (entry.instance && entry.instance->imuConnected()) ++n;
    return n;
}

bool ImuManager::anyConnecting() const
{
    for (const auto &entry : m_selected)
        if (entry.instance && entry.instance->busy()) return true;
    return false;
}

int ImuManager::lowBatteryPercent() const
{
    int lowest = -1;
    for (const auto &entry : m_selected) {
        if (!entry.instance || !entry.instance->imuConnected()) continue;
        const int p = entry.instance->batteryPercent();
        if (p < 0) continue;                          // no reading yet
        if (lowest < 0 || p < lowest) lowest = p;
    }
    return lowest;
}

// ---------------------------------------------------------------------------
// Invokables
// ---------------------------------------------------------------------------

void ImuManager::setSelected(int index, bool selected)
{
    const QList<Device> devs = DeviceEnumerator::instance()->devices(DeviceType::Imu);
    if (index < 0 || index >= devs.size()) return;

    const Device &device = devs[index];
    const QString  id    = device.id;

    // Re-entrancy guard: a previous instance for this device may still be
    // tearing down (deleteLater pending from a just-issued deselect). Ignore a
    // re-select until that settles — otherwise two instances briefly coexist and
    // can overlap connects on the same HCI adapter. The user can re-tap once the
    // deferred deletion has run (next event-loop turn).
    if (selected && m_pendingDelete.contains(id)) return;

    ImuEntry &entry = m_selected[id];   // creates entry with defaults if absent
    if (entry.selected == selected) return;

    entry.selected = selected;

    // deregisterSource() asserts that no SwingWindow is live, and the shot
    // workers read ring memory through it — including this IMU's ring. The
    // processor joins its workers and destroys the window before we touch
    // source registration (blocking — the correctness barrier; same contract
    // as CameraManager::setSelected). Also covers the postroll pause/resume:
    // resume clears all rings, which would otherwise gut the pending shot.
    if (!selected && m_shotProcessor)
        m_shotProcessor->finishNowBlocking();

    // Pause the buffer around EventBuffer register/deregister (same pattern as CameraManager).
    const bool wasCapturing = m_eventBuffer &&
                              m_eventBuffer->state() == pinpoint::BufferState::Capturing;
    if (wasCapturing) m_eventBuffer->pause();

    if (selected) {
        entry.instance = createInstance(device);
        entry.instance->start();
        if (wasCapturing) m_eventBuffer->resume();
        emit imuListChanged();
        emit instancesChanged();
        emit batteryChanged();
        // registerSource() may have silently auto-resumed the buffer (first
        // source); let CameraManager re-apply the user capture intent.
        emit bufferStateChanged();
    } else {
        ImuDeviceBase *inst = entry.instance;
        entry.instance = nullptr;
        if (inst) {
            disconnect(inst, nullptr, this, nullptr);
            inst->stop();
            inst->deregisterFromBuffer();
        }
        if (wasCapturing)
            m_eventBuffer->resume();
        emit imuListChanged();
        emit batteryChanged();   // entry.instance already cleared above
        if (inst) {
            m_pendingDelete.insert(id);   // block re-select until teardown settles
            QTimer::singleShot(0, this, [this, inst, id]() {
                emit instancesChanged();
                inst->deleteLater();
                m_pendingDelete.remove(id);
            });
        } else {
            emit instancesChanged();
        }
        // deregisterSource() auto-pauses when the last source is removed.
        emit bufferStateChanged();
        return;
    }
}

void ImuManager::disconnectAll()
{
    // setSelected owns the full per-device teardown (stop barrier, BLE
    // disconnect, deregister, buffer-intent notify) — reuse it per device.
    // The enumerator list is stable across the loop (no scan runs here).
    const QList<Device> devs = DeviceEnumerator::instance()->devices(DeviceType::Imu);
    for (int i = 0; i < devs.size(); ++i) {
        const auto it = m_selected.constFind(devs[i].id);
        if (it != m_selected.cend() && it->selected)
            setSelected(i, false);
    }
}

void ImuManager::rescanImu()
{
    setImuScanError(QString());   // clear any stale error; a fresh scan may succeed
    DeviceEnumerator::instance()->scanImu();
}

void ImuManager::setImuScanError(const QString &msg)
{
    if (m_imuScanError == msg) return;
    m_imuScanError = msg;
    emit imuScanErrorChanged();
}

QObject *ImuManager::instanceFor(const QString &deviceId) const
{
    const ImuEntry &entry = m_selected.value(deviceId);
    if (entry.selected && entry.instance)
        return static_cast<QObject *>(entry.instance);
    return nullptr;
}

ImuManager::ImuDeviceStats ImuManager::liveDeviceStats(const QString &deviceId) const
{
    ImuDeviceStats stats;
    const ImuEntry &e = m_selected.value(deviceId);
    stats.selected = e.selected;
    if (e.selected && e.instance) {
        stats.sourceIds       = e.instance->sourceIds();
        stats.dataRateHz      = e.instance->dataRateHz();
        stats.batteryPercent  = e.instance->batteryPercent();
        stats.connected       = e.instance->imuConnected();
        stats.busy            = e.instance->busy();
        // Gimbal-lock drop counting is a Euler-gimbal artefact of the
        // Witmotion fusion path (imu_device.h's "WHAT DELIBERATELY IS NOT
        // HERE") — a HackMotion fuses on-device and has no equivalent, so it
        // reports 0 rather than a number that looks measured but isn't.
        if (auto *wt = qobject_cast<ImuInstance *>(e.instance))
            stats.gimbalDropCount = wt->gimbalDropCount();
    }
    return stats;
}

QString ImuManager::saveLog()
{
    QStringList paths;
    for (const auto &entry : m_selected) {
        if (entry.instance)
            paths.append(entry.instance->saveLog());
    }
    return paths.isEmpty() ? QStringLiteral("No active IMU instances") : paths.join(QStringLiteral("\n"));
}

void ImuManager::zeroAll()
{
    // No host-side zero on a HackMotion (it fuses on-device) — skip anything
    // that isn't a Witmotion rather than asserting a control we don't have.
    for (const auto &entry : m_selected)
        if (auto *wt = qobject_cast<ImuInstance *>(entry.instance))
            wt->zeroOrientation();
}

void ImuManager::setOrientationFilter(const QString &name)
{
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;
    s->setImuOrientationFilter(name);

    const OrientationFilterType type =
        orientationFilterFromString(name.toUtf8().constData());
    // The wG3 fuses on-device — there is no host filter to swap, so a
    // HackMotion instance is simply skipped rather than handed a call it has
    // no way to honour.
    for (const auto &entry : m_selected)
        if (auto *wt = qobject_cast<ImuInstance *>(entry.instance))
            wt->setOrientationFilter(type);
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

ImuDeviceBase *ImuManager::createInstance(const Device &device)
{
    // The wG3 is a second device KIND, not a second Witmotion — output rate,
    // orientation filter and impact sensitivity are all Witmotion concepts
    // that simply don't exist on this device (adaptive rate, on-device
    // fusion, no impact detector yet — see imu_device.h and hm_instance.h).
    // Forcing them on would be asserting control we do not have, so this
    // branch wires up only what IS generic across both kinds.
    if (device.imuVendor == ImuVendor::HackMotion) {
        auto *inst = new HmInstance(device, m_eventBuffer, &m_ioThread, this);

        // Forward log entries to any QML log view listening to imuManager.
        connect(inst, &ImuDeviceBase::logEntryAdded,
                this, &ImuManager::logEntryAdded);

        // Re-emit imuListChanged when connection state changes so chip colours update.
        connect(inst, &ImuDeviceBase::imuConnectedChanged, this, [this]() {
            emit imuListChanged();
            emit instancesChanged(); // instanceFor() rebinds in QML
            emit batteryChanged();   // connect/disconnect changes the aggregate min
        });
        connect(inst, &ImuDeviceBase::busyChanged, this, [this]() {
            emit imuListChanged();
            emit anyConnectingChanged();   // aggregate connect-in-flight changed
        });
        // Forward live battery updates to the aggregate lowBatteryPercent property.
        connect(inst, &ImuDeviceBase::batteryPercentChanged, this, &ImuManager::batteryChanged);

        return inst;
    }

    auto *inst = new ImuInstance(device, m_eventBuffer, &m_ioThread, this);

    // Restore persisted output rate so it is applied from the very first
    // initializeDevice() call rather than only after the State::Ready reinit.
    // Devices without a persisted rate default to 200 Hz (shot detection P1 —
    // sharper impact detection; the device hardware default is 100 Hz, so any
    // other value must be sent).
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;
    const int savedRate = s->imuOutputRateHz().value(device.id, 200).toInt();
    if (savedRate != 100)
        inst->setOutputRateHz(savedRate);

    // Apply the persisted orientation-fusion algorithm so it is in effect from the
    // first packet (the deferred swap in the driver picks it up on connect).
    inst->setOrientationFilter(
        orientationFilterFromString(s->imuOrientationFilter().toUtf8().constData()));

    // Forward log entries to any QML log view listening to imuManager.
    connect(inst, &ImuInstance::logEntryAdded,
            this, &ImuManager::logEntryAdded);

    // Re-emit imuListChanged when connection state changes so chip colours update.
    connect(inst, &ImuInstance::imuConnectedChanged, this, [this]() {
        emit imuListChanged();
        emit instancesChanged(); // instanceFor() rebinds in QML
        emit batteryChanged();   // connect/disconnect changes the aggregate min
    });
    connect(inst, &ImuInstance::busyChanged, this, [this]() {
        emit imuListChanged();
        emit anyConnectingChanged();   // aggregate connect-in-flight changed
    });
    // Forward live battery updates to the aggregate lowBatteryPercent property.
    connect(inst, &ImuInstance::batteryPercentChanged, this, &ImuManager::batteryChanged);

    // IMU impact auto-trigger (shot detection P1): forward to the app-level
    // funnel and keep the detector sensitivity tracking the setting live.
    connect(inst, &ImuInstance::impactDetected, this, &ImuManager::impactDetected);
    inst->setImpactSensitivity(impactScaleFor(s->swingDetectionSensitivity()));
    if (m_appSettings) {
        connect(m_appSettings, &AppSettings::swingDetectionSensitivityChanged,
                inst, [this, inst]() {
            inst->setImpactSensitivity(
                impactScaleFor(m_appSettings->swingDetectionSensitivity()));
        });
    }

    return inst;
}

float ImuManager::impactScaleFor(const QString &sensitivity)
{
    // Threshold scale: >1 = less sensitive. "High" sensitivity fires on
    // weaker swings, "Low" demands more energy.
    if (sensitivity == QLatin1String("Low"))  return 1.5f;
    if (sensitivity == QLatin1String("High")) return 0.7f;
    return 1.0f;
}

void ImuManager::setImuAlias(const QString &key, const QString &alias)
{
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;
    QVariantMap map = s->imuAlias();
    const QString trimmed = alias.trimmed();
    const QString current = map.value(key).toString();

    const bool changed = trimmed.isEmpty() ? map.contains(key) : (current != trimmed);
    if (!changed) return;

    if (trimmed.isEmpty())
        map.remove(key);
    else
        map[key] = trimmed;
    s->setImuAlias(map);
    emit imuListChanged();
    emit imuDeviceListChanged();
}
