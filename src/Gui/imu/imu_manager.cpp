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
#include "pp_debug.h"
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
        //
        // ⚠ QUEUED, NOT DIRECT. The signal is emitted from inside the QML write
        // `appSettings.imuExcluded = list` (the Enable pill's handler), and a
        // direct connection runs the whole disconnect → instance teardown →
        // imuDeviceListChanged → Repeater row-rebuild cascade NESTED inside that
        // write. Tearing down the very row whose handler is mid-statement makes
        // the engine report a spurious "ReferenceError: appSettings is not
        // defined" against the write line on every first exclusion change — the
        // write itself lands, so the noise reads as a broken toggle when nothing
        // is broken. One tick later, the handler has returned and the same
        // cascade is ordinary. The diff below reads current state at run time,
        // so queued coalescing of rapid changes is safe: a no-op diff does
        // nothing.
        connect(m_appSettings, &AppSettings::imuExcludedChanged, this, [this]() {
            const QStringList now  = m_appSettings->imuExcluded();
            const QStringList prev = m_lastGlobalExcluded;
            for (const QString &id : now)
                if (!prev.contains(id)) setSessionImuEnabled(id, false);
            for (const QString &id : prev)
                if (!now.contains(id)) setSessionImuEnabled(id, true);
            m_lastGlobalExcluded = now;
        }, Qt::QueuedConnection);
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
        if (m_imuScanActive) {
            m_imuScanActive = false;
            emit imuScanActiveChanged();
        }
    });

    // Normalise any HackMotion placement persisted by Phase A's interim
    // bare-device-id pin BEFORE the first consumer can read it. DeviceEnumerator
    // is a singleton that never forgets a registered device, so a wG3 may already
    // be in its list here (an earlier scan in this process); those devices never
    // emit deviceAdded again and would be missed by the hook below on its own.
    for (const Device &dev : DeviceEnumerator::instance()->devices(DeviceType::Imu))
        migrateHackMotionPlacement(dev);

    // Start the async BLE scan.  Results arrive via deviceAdded and are
    // automatically reflected by imuList() / imuDeviceList() reading directly
    // from DeviceEnumerator — no local list copy needed.
    //
    // ⚠ Push the HackMotion discovery flag HERE TOO, not only in rescanImu().
    // This is the first-run scan and it happens before any coach can press
    // Rescan, so a startup that skipped the push would run the whole 90 s
    // HackMotion-enabled window against a setting the user had turned off —
    // and the enumerator's own default (true) would make that look deliberate.
    DeviceEnumerator::instance()->setHackMotionEnabled(
        m_appSettings ? m_appSettings->hackmotionEnabled() : true);
    // Recorded, not emitted: this runs before QML loads, so the property's
    // first read picks the value up. isImuScanActive() may be true already if
    // another manager instance armed a scan (tests); either way the flag
    // reflects the enumerator's truth at construction.
    m_imuScanActive = DeviceEnumerator::instance()->scanImu()
                      || DeviceEnumerator::instance()->isImuScanActive();

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
        // ⚠ THE MIGRATION HOOK, and this is the right one. A wG3 must be
        // unit-keyed the moment it becomes KNOWN, not the moment it connects: the
        // start wizard's requirement rows and ArmVizView both resolve slots from
        // placement alone, with no live instance in sight. Hanging this off
        // createInstance() would leave an enumerated-but-unconnected wG3 showing
        // slot A filled and slot B empty — the exact misreading Phase C removes.
        // Alias seeding above is here for the same reason and has the same shape.
        migrateHackMotionPlacement(dev);
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
    // Push the HackMotion discovery flag immediately before arming the scan.
    // Every path that starts a scan does this — here and the first-run scan in
    // the constructor — because the value must be READ AT ARM TIME, not latched.
    // A value captured once at construction would go stale the first time a coach
    // toggled the feature in Settings and then hit Rescan: the scan would filter
    // on the old setting with nothing anywhere to say so.
    DeviceEnumerator::instance()->setHackMotionEnabled(
        m_appSettings ? m_appSettings->hackmotionEnabled() : true);
    // Arm-aware: a call swallowed by the enumerator's re-entry guard must not
    // touch the flag — setting it with no scan behind it leaves the buttons
    // saying "Scanning…" forever, and clearing it while a scan IS running lies
    // the other way.
    if (DeviceEnumerator::instance()->scanImu() && !m_imuScanActive) {
        m_imuScanActive = true;
        emit imuScanActiveChanged();
    }
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

// ---------------------------------------------------------------------------
// Unit-keyed placement — the single canonical resolver
// ---------------------------------------------------------------------------

namespace {

// "<deviceId>#lowerArm" / "<deviceId>#palm".
//
// ⚠ NOT RESPELLED HERE. HmUnit owns the spelling (HmUnit::unitIdFor) because the
// string is persisted twice over — as the EventBuffer SourceDescriptor::identifier
// and as the placement key below — and a second copy of the literal is exactly the
// drift nothing would catch: the two spellings would not fail, they would silently
// orphan one device's entire placement. These are thin wrappers rather than direct
// calls only so the resolver below reads in terms of keys.
QString hmUnitKey(const QString &deviceId, wr_unit unit)
{
    return HmUnit::unitIdFor(deviceId, unit);
}

QString hmUnitLabel(wr_unit unit)
{
    return HmUnit::unitLabelFor(unit);
}

// Splits a placement key into device id + unit. False for a bare device id —
// a Witmotion, or a wG3 whose interim Phase A entry has not been migrated.
bool parseUnitKey(const QString &key, QString *deviceId, wr_unit *unit)
{
    const int sep = key.lastIndexOf(QLatin1Char('#'));
    if (sep <= 0) return false;
    const QString id = key.left(sep);
    // Regenerate and compare rather than matching the suffix text: the spelling
    // stays owned by HmUnit, and a suffix this build does not recognise stays
    // UNRESOLVED instead of being guessed at.
    for (int u = 0; u < WR_UNIT_COUNT; ++u) {
        const wr_unit candidate = static_cast<wr_unit>(u);
        if (key == hmUnitKey(id, candidate)) {
            *deviceId = id;
            *unit     = candidate;
            return true;
        }
    }
    return false;
}

// True when the enumerator can currently see this peripheral. The placement map
// accretes keys over months — dead sensors, replaced ones — and only the
// enumerator knows which owners still exist.
bool isEnumeratedImu(const QString &deviceId)
{
    const QList<Device> devs = DeviceEnumerator::instance()->devices();
    for (const Device &d : devs)
        if (d.type == DeviceType::Imu && d.id == deviceId) return true;
    return false;
}

// Removes every claim on `slot` owned by a device the enumerator CANNOT SEE,
// returning what was removed. An explicit assignment is AUTHORITATIVE over
// stale claims — a discarded sensor's key still naming the slot has no row in
// the settings panel and nothing else ever prunes it, so if assignment cannot
// displace it the slot is locked with no UI path out.
//
// ⚠ A PRESENT OWNER'S CLAIM IS SPARED, EVEN A DISABLED ONE. Present-and-enabled
// conflicts are blocked upstream (the panel greys those choices), and a
// present-but-disabled claim is a PARKED setup, not a stale one: keeping both
// claims is how a coach flips between a HackMotion and a Witmotion on the same
// letters with the enable toggles alone, and placementKeyForSlot()'s ladder
// hands the slot to whichever is enabled. Deleting the parked claim here would
// make every flip cost a re-assignment.
//
// ⚠ Displacing one of a wG3's unit keys strips BOTH: a half-assigned pair reads
// as a unit that was never strapped, not as a conflict, and no consumer is
// written for that state.
QStringList stealSlotClaims(QVariantMap &map, const QString &slot, const QString &newOwner)
{
    QStringList doomed;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (it.value().toString() != slot) continue;
        QString devId;
        wr_unit unit = WR_UNIT_LOWER_ARM;
        const bool unitKey = parseUnitKey(it.key(), &devId, &unit);
        const QString owner = unitKey ? devId : it.key();
        if (owner == newOwner) continue;
        if (isEnumeratedImu(owner)) continue;
        doomed.append(it.key());
        if (unitKey)
            for (int u = 0; u < WR_UNIT_COUNT; ++u) {
                const QString partner = hmUnitKey(devId, static_cast<wr_unit>(u));
                if (map.contains(partner)) doomed.append(partner);
            }
    }
    doomed.removeDuplicates();
    for (const QString &k : doomed) map.remove(k);
    return doomed;
}

}   // namespace

QString ImuManager::placementKeyForSlot(const QString &slot) const
{
    if (slot.isEmpty()) return {};   // "" means unassigned; it is never a slot to look up

    // ⚠ THE FALLBACK IS CONSTRUCTED ONLY WHEN IT IS ACTUALLY NEEDED, unlike the
    // `AppSettings fallback; ... ? : &fallback` idiom used elsewhere in this file.
    // This is the hottest placement accessor in the tree — every instanceForSlot()
    // / deviceIdForSlot() / unitLabelForSlot() call funnels through it, and
    // LiveWristAngles resolves three slots at 30 Hz — while the AppSettings
    // constructor reads several hundred QSettings values. Constructing one
    // unconditionally would pay a full settings load ninety times a second for a
    // branch that is never taken outside tests (main.cpp always passes one).
    const QVariantMap placement = m_appSettings ? m_appSettings->imuPlacement()
                                                : AppSettings().imuPlacement();

    // ⚠ A LIVE, ENABLED DEVICE BEATS EVERYTHING ELSE. The map accretes claims
    // from sensors that no longer exist, and it deliberately KEEPS claims from
    // sensors that exist but are switched off this session — that pair of claims
    // is how a coach flips between a HackMotion and a Witmotion on the same
    // letters without re-assigning anything. So resolution is a ladder, not a
    // lookup:
    //
    //   1. enumerated AND session-enabled — the sensor actually in play;
    //   2. enumerated but disabled — a parked setup, still a better answer
    //      than a ghost (its device row exists, its state is inspectable);
    //   3. any claimant at all — the wizard reads placement before the first
    //      scan completes, and "assigned but not discovered yet" must keep
    //      reading as assigned.
    //
    // Without the ladder, a dead sensor's key that happens to sort first takes
    // the slot from a connected one — and the only symptom is a wrist readout
    // that silently never appears. Within each rung, QVariantMap KEY order
    // keeps the answer deterministic; two ENABLED, PRESENT claimants on one
    // letter is a real conflict the coach has to resolve, so that one is warned
    // about (once) rather than silently arbitrated.
    QString     firstKey;      // rung 3 — any claimant
    QString     presentKey;    // rung 2 — enumerated but disabled this session
    QStringList enabledKeys;   // rung 1 — enumerated and enabled
    for (auto it = placement.cbegin(); it != placement.cend(); ++it) {
        if (it.value().toString() != slot) continue;
        if (firstKey.isEmpty()) firstKey = it.key();
        QString devId;
        wr_unit unit = WR_UNIT_LOWER_ARM;
        const QString owner = parseUnitKey(it.key(), &devId, &unit) ? devId : it.key();
        if (!isEnumeratedImu(owner)) continue;
        if (m_sessionExcluded.contains(owner)) {
            if (presentKey.isEmpty()) presentKey = it.key();
        } else {
            enabledKeys.append(it.key());
        }
    }
    if (enabledKeys.size() > 1) {
        // Once per distinct conflict: this funnels a ~30 Hz readout timer.
        const QString sig = slot + QLatin1Char('|') + enabledKeys.join(QLatin1Char(','));
        if (!m_warnedSlotConflicts.contains(sig)) {
            m_warnedSlotConflicts.insert(sig);
            ppWarn() << "[ImuManager] slot" << slot << "is claimed by more than one enabled,"
                        " present sensor (" << enabledKeys.join(QStringLiteral(", "))
                     << ") — using" << enabledKeys.first()
                     << ". Disable or unassign one in Settings → IMUs.";
        }
    }
    if (!enabledKeys.isEmpty()) return enabledKeys.first();
    if (!presentKey.isEmpty())  return presentKey;
    return firstKey;
}

QString ImuManager::deviceIdForSlot(const QString &slot) const
{
    const QString key = placementKeyForSlot(slot);
    if (key.isEmpty()) return {};
    QString devId;
    wr_unit unit = WR_UNIT_LOWER_ARM;
    // Both key shapes name the peripheral unambiguously — it is only the UNIT that
    // a bare key fails to name.
    return parseUnitKey(key, &devId, &unit) ? devId : key;
}

QString ImuManager::unitLabelForSlot(const QString &slot) const
{
    const QString key = placementKeyForSlot(slot);
    QString devId;
    wr_unit unit = WR_UNIT_LOWER_ARM;
    if (key.isEmpty() || !parseUnitKey(key, &devId, &unit)) return {};
    return hmUnitLabel(unit);
}

QObject *ImuManager::instanceForSlot(const QString &slot) const
{
    const QString key = placementKeyForSlot(slot);
    if (key.isEmpty()) return nullptr;

    QString devId;
    wr_unit unit = WR_UNIT_LOWER_ARM;
    const bool unitKey = parseUnitKey(key, &devId, &unit);
    if (!unitKey) devId = key;

    const ImuEntry &entry = m_selected.value(devId);
    // Assigned but not live (enumerated, never selected, or torn down). The
    // placement accessors above still answer for this case — they are what the
    // start wizard reads before anything has connected.
    if (!entry.selected || !entry.instance) return nullptr;

    auto *hm = qobject_cast<HmInstance *>(entry.instance);

    if (unitKey) {
        // A unit key naming a device that is not a HackMotion is a stale or
        // hand-edited entry. There is no unit to hand back, so nothing is.
        if (!hm) return nullptr;
        // ⚠ WHICH UNIT IS ON WHICH SEGMENT IS FIXED BY THE CABLE — wire block 0 is
        // the lower arm, block 1 is the palm — so it is read out of the KEY and
        // never inferred from the slot letter or from the order the two views were
        // created in. A consumer that swaps the two produces a plausible-looking
        // wrist angle that is simply MIRRORED, which every plausibility check
        // passes.
        return unit == WR_UNIT_PALM ? hm->unitPalmObject() : hm->unitLowerArmObject();
    }

    if (hm) {
        // ⚠ A BARE DEVICE-ID KEY ON A HACKMOTION IS *NOT* "THE LOWER ARM". It is
        // Phase A's interim pin, which under-describes the device, and reading it
        // as one unit rather than as an unmigrated entry is precisely how a
        // mirrored wrist angle ships. migrateHackMotionPlacement() rewrites these
        // on the device-list path, so reaching here means something re-wrote a bare
        // key afterwards — the peripheral is resolvable (see deviceForSlot()) but
        // the UNIT is not, so this returns nothing and says why. Once per key: this
        // is called from a ~30 Hz readout timer.
        if (!m_warnedBarePlacementKeys.contains(key)) {
            m_warnedBarePlacementKeys.insert(key);
            ppWarn() << "[ImuManager] placement slot" << slot << "holds bare device id" << key
                     << "for a HackMotion — that key names the peripheral, not one of its two"
                        " units, so no unit can be resolved. Reassign the device in"
                        " Settings → IMUs (one wG3 fills A and B).";
        }
        return nullptr;
    }

    return static_cast<QObject *>(entry.instance);
}

QObject *ImuManager::deviceForSlot(const QString &slot) const
{
    // ⚠ DELIBERATELY MORE PERMISSIVE THAN instanceForSlot(). An unmigrated bare
    // key still identifies the PERIPHERAL unambiguously, and device-level
    // operations — calibrate, connect, battery, firmware — do not address a unit,
    // so refusing them here would break the very flow a coach would use to fix the
    // placement. It is only the anatomical reading that must not guess.
    const QString devId = deviceIdForSlot(slot);
    if (devId.isEmpty()) return nullptr;
    const ImuEntry &entry = m_selected.value(devId);
    if (!entry.selected || !entry.instance) return nullptr;
    return static_cast<QObject *>(entry.instance);
}

void ImuManager::setPlacementForDevice(const QString &deviceId, const QString &slot)
{
    if (deviceId.isEmpty()) return;

    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;
    QVariantMap map = s->imuPlacement();

    if (isHackMotionDevice(deviceId)) {
        const QString lowerKey = hmUnitKey(deviceId, WR_UNIT_LOWER_ARM);
        const QString palmKey  = hmUnitKey(deviceId, WR_UNIT_PALM);

        // ⚠ A HACKMOTION'S ASSIGNMENT IS NOT A CHOICE OF ONE LETTER, AND THE COACH
        // DOES NOT GET TO PICK WHICH SEGMENTS IT COVERS. One wG3 is a single BLE
        // peripheral carrying TWO sensor units on a cable, and the cable fixes
        // which unit sits on which segment: block 0 on the lower arm, block 1 on
        // the palm. So assigning it fills slot A (lead forearm) with the lower-arm
        // unit and slot B (lead hand) with the palm unit, TOGETHER — which
        // anatomical segments the device covers is a property of the HARDWARE, not
        // a setting. There is no strap arrangement that makes any other pairing
        // true, and a settings map that allowed one would yield a wrist angle that
        // is exactly MIRRORED and passes every plausibility check.
        //
        // Hence "A" is the only non-empty slot accepted here, and any other letter
        // is a CALLER ERROR: refused and logged, never coerced into something
        // plausible.
        if (slot.isEmpty()) {
            map.remove(lowerKey);
            map.remove(palmKey);
            map.remove(deviceId);   // Phase A's interim pin, if still present
        } else if (slot == QLatin1String("A")) {
            // ⚠ AND IT TAKES TWO LETTERS, SO IT CAN COLLIDE TWICE. Filling A and B
            // together displaces whatever held either letter — the assignment is
            // an explicit choice in the settings panel (there is no auto-pin), so
            // it is authoritative, and leaving a double claim would hand the
            // answer to placementKeyForSlot()'s key ordering: deterministic,
            // deliberately, but arbitrary as an ANSWER, with the dropped sensor
            // never mentioned. Displacement only ever touches ABSENT owners
            // (stealSlotClaims spares present ones): an enabled present holder
            // greys this choice out in the panel, and a disabled present
            // holder's claim is a parked setup that stays — the resolver's
            // enabled-first ladder decides who drives the slot, so a coach
            // flips between this wG3 and parked Witmotions with the enable
            // toggles alone.
            const QStringList displaced =
                stealSlotClaims(map, QStringLiteral("A"), deviceId)
                + stealSlotClaims(map, QStringLiteral("B"), deviceId);
            if (!displaced.isEmpty())
                ppWarn() << "[ImuManager] placing HackMotion" << deviceId
                         << "on slots A+B displaced placement claim(s) from absent device(s):"
                         << displaced.join(QStringLiteral(", "));
            map[lowerKey] = QStringLiteral("A");
            map[palmKey]  = QStringLiteral("B");
            map.remove(deviceId);
        } else {
            ppWarn() << "[ImuManager] refusing to place HackMotion" << deviceId << "at slot"
                     << slot << "— one wG3 fills slots A (lower arm) and B (palm) together,"
                        " fixed by its cable, so \"A\" and \"\" are the only assignments"
                        " that exist for it. Placement unchanged.";
            return;
        }
    } else {
        // Witmotion: the bare device id, exactly as before Phase C — one device,
        // one segment, one letter. Existing entries keep working untouched.
        // Assignment displaces ABSENT owners' claims on the letter only (see
        // stealSlotClaims — an enabled present holder greys the choice in the
        // panel, and a disabled present holder's claim is a parked setup the
        // resolver's enabled-first ladder arbitrates).
        if (slot.isEmpty()) {
            map.remove(deviceId);
        } else {
            const QStringList displaced = stealSlotClaims(map, slot, deviceId);
            if (!displaced.isEmpty())
                ppWarn() << "[ImuManager] placing" << deviceId << "on slot" << slot
                         << "displaced placement claim(s) from absent device(s):"
                         << displaced.join(QStringLiteral(", "));
            map[deviceId] = slot;
        }
    }

    // ONE write, after the whole map is built. AppSettings::setImuPlacement guards
    // on equality, so an unchanged map emits nothing and this function is
    // idempotent; writing key-by-key would emit imuPlacementChanged up to three
    // times and let a QML consumer observe the device half-assigned — slot A filled
    // and slot B not yet, which is the exact state Phase C exists to stop showing.
    s->setImuPlacement(map);
}

ImuManager::ImuDeviceStats ImuManager::liveDeviceStats(const QString &deviceId) const
{
    ImuDeviceStats stats;
    const ImuEntry &e = m_selected.value(deviceId);
    stats.selected = e.selected;
    if (e.selected && e.instance) {
        stats.sourceIds       = e.instance->sourceIds();
        stats.sourceLabels    = e.instance->sourceLabels();
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

bool ImuManager::isHackMotionDevice(const QString &deviceId) const
{
    for (const Device &dev : DeviceEnumerator::instance()->devices(DeviceType::Imu))
        if (dev.id == deviceId) return dev.imuVendor == ImuVendor::HackMotion;
    return false;
}

void ImuManager::migrateHackMotionPlacement(const Device &device)
{
    if (device.type != DeviceType::Imu || device.imuVendor != ImuVendor::HackMotion)
        return;

    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;
    QVariantMap map = s->imuPlacement();

    const QString lowerKey = hmUnitKey(device.id, WR_UNIT_LOWER_ARM);
    const QString palmKey  = hmUnitKey(device.id, WR_UNIT_PALM);
    const bool alreadyUnitKeyed = map.contains(lowerKey) || map.contains(palmKey);

    if (!map.contains(device.id)) {
        // Nothing is keyed by bare device id, so there is nothing to migrate:
        // either this device is already unit-keyed, or it was never assigned.
        //
        // ⚠ THOSE TWO ARE INDISTINGUISHABLE FROM AN ASSIGNMENT A COACH
        // DELIBERATELY CLEARED, and both are left exactly as they are. This
        // function migrates; it never assigns. An unassigned wG3 reads as
        // unassigned until someone assigns it — a migration that helpfully filled
        // A and B would silently undo a clearing every time the app restarted.
        return;
    }

    const QString bare = map.value(device.id).toString();

    if (alreadyUnitKeyed || bare.isEmpty()) {
        // The unit keys are the authority the moment they exist, and an empty bare
        // value means unassigned. Either way the bare entry now carries no
        // information worth preserving, so it is dropped — that is all this branch
        // does, and it is what makes a second run of this function a no-op.
        map.remove(device.id);
        s->setImuPlacement(map);
        return;
    }

    if (bare != QLatin1String("A")) {
        // ⚠ NOT REINTERPRETED. Phase A pinned a wG3 to slot A and locked the
        // control, so "A" is the only value it could have written; anything else
        // came from an older build or a hand-edited file, and there is no defined
        // pair of slots for it — B+C is not an arrangement this hardware can be in.
        // Silently mapping it onto lowerArm/palm anyway is exactly how a mirrored
        // wrist angle gets shipped, so the entry is left unresolved and said out
        // loud instead.
        ppWarn() << "[ImuManager]" << device.id << "— persisted HackMotion placement" << bare
                 << "cannot be migrated: one wG3 fills slots A (lower arm) and B (palm), and no"
                    " other pair is defined. Left unresolved — reassign it in Settings → IMUs.";
        return;
    }

    // ⚠ THE INTERIM PIN CLAIMED ONE LETTER; THE MIGRATION CLAIMS TWO, so it can
    // collide on the second one even though the first is already this device's.
    // Phase A's own comment named this: the bare pin "will collide if a Witmotion
    // already holds A" — and B is the half nobody was holding a letter for yet.
    // Writing the palm key on top of a Witmotion's B would leave that letter
    // claimed twice, and placementKeyForSlot() would answer with whichever key
    // sorted first: deterministic, but an arbitrary ANSWER, and the dropped sensor
    // is never mentioned. Refusing instead leaves the bare entry in place, which
    // instanceForSlot() already reports once per key as unresolvable — a dead end
    // the coach can see and fix, rather than a wrist angle from the wrong sensor.
    const QString holderB = deviceIdForSlot(QStringLiteral("B"));
    if (!holderB.isEmpty() && holderB != device.id) {
        ppWarn() << "[ImuManager]" << device.id
                 << "— cannot migrate HackMotion placement: the palm unit needs slot B, which is"
                    " already held by" << holderB
                 << ". Left on the old bare-device-id key and therefore unresolved — unassign"
                    " that sensor in Settings → IMUs, then reassign the wG3.";
        return;
    }

    // Fully determined otherwise: the lower-arm unit keeps the letter that was
    // stored and the palm unit takes B, in the cable's fixed order.
    map[lowerKey] = QStringLiteral("A");
    map[palmKey]  = QStringLiteral("B");
    map.remove(device.id);
    s->setImuPlacement(map);

    ppInfo() << "[ImuManager]" << device.id
             << "— migrated HackMotion placement to unit keys: lower arm → A, palm → B";
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
