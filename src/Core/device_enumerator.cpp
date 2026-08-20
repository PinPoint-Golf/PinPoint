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

#include "device_enumerator.h"
#include "ble_adapter_pool.h"
#include "wt9011dcl_base.h"
#include "pp_debug.h"
#include <QCoreApplication>

#include <QBluetoothAddress>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QTimer>

#include <wrist/device.h>
#include <wrist/types.h>

// ---------------------------------------------------------------------------
// Converts the service UUIDs Qt discovered off an advertisement into the
// wr_uuid array wr_looks_like_sensor() wants. The library owns UUID
// parsing (wr_uuid_parse), not us — we only marshal Qt's type into it.
//
// QBluetoothUuid::toString() renders the canonical 8-4-4-4-12 form wrapped in
// braces ("{xxxxxxxx-...}"); wr_uuid_parse() accepts that form with or
// without braces, so no stripping is required here (contrast the
// BleImuTransport::.mid(1, 36) callsite, which feeds a parser that does not).
// ---------------------------------------------------------------------------
static QList<wr_uuid> toHmUuids(const QList<QBluetoothUuid> &qtUuids)
{
    QList<wr_uuid> out;
    out.reserve(qtUuids.size());
    for (const QBluetoothUuid &u : qtUuids) {
        wr_uuid parsed;
        const QByteArray text = u.toString().toUtf8();
        if (wr_uuid_parse(text.constData(), &parsed) == WR_OK)
            out.append(parsed);
        // Anything unparsable (shouldn't happen for a QBluetoothUuid) is
        // silently skipped — wr_looks_like_sensor() just sees one fewer
        // candidate service, not a crash.
    }
    return out;
}

// ---------------------------------------------------------------------------
// ImuBleScanner — internal, runs in a QThread started by DeviceEnumerator.
//
// Accept filter, evaluated per discovered advertisement:
//   - WT901 (WitMotion): name starts with "WT901" OR service UUID contains
//     "ffe5". This is the ONLY WT901 discovery filter in the codebase, and it
//     is unconditional — the hackmotion/enabled flag never touches it.
//   - HackMotion (wG3): wr_looks_like_sensor(), owned by libwrist so
//     the match logic lives in one place instead of being hand-rolled here.
//     Skipped entirely when HackMotion discovery is disabled — see
//     m_hackMotionEnabled below.
// (BleImuTransport's connect-phase scan is a different concern — it matches
// one already-selected device by address/UUID in matchesPendingDevice(), not
// a discovery filter.)
// ---------------------------------------------------------------------------

class ImuBleScanner : public QObject
{
    Q_OBJECT
public:
    // hackMotionEnabled is read from DeviceEnumerator ONCE, here, when the
    // scan is armed — never re-read for the duration of the scan. A scan
    // whose accept filter (or window) changed mid-flight would accept a
    // device the window wasn't sized for, and the resulting "sometimes it
    // finds it" would be unreproducible.
    explicit ImuBleScanner(bool hackMotionEnabled, QObject *parent = nullptr)
        : QObject(parent)
        , m_hackMotionEnabled(hackMotionEnabled)
        , m_timeoutMs(hackMotionEnabled ? kHackMotionScanWindowMs : kWitmotionOnlyScanWindowMs)
    {}

public slots:
    void start()
    {
        // On Linux with multiple adapters, start one scan agent per adapter so
        // devices in range of any adapter are discovered.  On single-adapter
        // setups (or non-Linux), a single default-adapter agent is used.
        const QList<QBluetoothAddress> adapterAddrs = BleAdapterPool::instance()->adapters();
        if (adapterAddrs.size() > 1) {
            for (const QBluetoothAddress &addr : adapterAddrs)
                createAgent(addr);
        } else {
            createAgent(QBluetoothAddress()); // null → default constructor
        }
        m_pendingAgents = m_agents.size();
    }

signals:
    // vendor tells the main-thread handler which capability set to build —
    // see the deviceFound lambda in DeviceEnumerator::scanImu().
    void deviceFound(const QBluetoothDeviceInfo &info, ImuVendor vendor);
    void finished();
    void scanError(const QString &message);

private slots:
    void onDeviceDiscovered(const QBluetoothDeviceInfo &device)
    {
        // WT901 accept filter (name prefix OR ffe5 service UUID).
        const bool hasKnownName   = device.name().startsWith(
                                        QStringLiteral("WT901"), Qt::CaseInsensitive);
        const bool hasServiceUuid = device.serviceUuids().contains(
                                        QBluetoothUuid(QStringLiteral("0000ffe5-0000-1000-8000-00805f9b34fb")));
        if (hasKnownName || hasServiceUuid) {
            ppInfo() << "[IMU] BLE candidate (WitMotion):" << device.name()
                     << (device.address().isNull()
                             ? device.deviceUuid().toString()
                             : device.address().toString());
            emit deviceFound(device, ImuVendor::WitMotion);
            return;
        }

        // HackMotion wG3 accept filter, owned by libwrist. Gated on the
        // flag captured at scan-arm time: when discovery is disabled,
        // wr_looks_like_sensor() is not even consulted, so no wG3 is
        // offered — this is discovery-only, and does not touch a wG3 that is
        // already connected or persisted in placement settings.
        if (!m_hackMotionEnabled) return;

        // local_name may legitimately be null/empty and services may be empty
        // (many stacks don't report service UUIDs in the advertisement) — the
        // library handles both, matching on name alone when services are absent.
        const QByteArray nameUtf8 = device.name().toUtf8();
        const QList<wr_uuid> services = toHmUuids(device.serviceUuids());
        if (wr_looks_like_sensor(nameUtf8.isEmpty() ? nullptr : nameUtf8.constData(),
                                     services.isEmpty() ? nullptr : services.constData(),
                                     static_cast<size_t>(services.size()))) {
            ppInfo() << "[IMU] BLE candidate (HackMotion):" << device.name()
                     << (device.address().isNull()
                             ? device.deviceUuid().toString()
                             : device.address().toString());
            emit deviceFound(device, ImuVendor::HackMotion);
        }
    }

    void onScanFinished()
    {
        ppInfo() << "[IMU] BLE scan agent finished";
        if (--m_pendingAgents <= 0) {
            ppInfo() << "[IMU] All BLE scan agents finished";
            emit finished();
        }
    }

    void onScanError(QBluetoothDeviceDiscoveryAgent::Error error)
    {
        auto *agent = qobject_cast<QBluetoothDeviceDiscoveryAgent *>(sender());
        const QString msg = agent ? agent->errorString()
                                  : QStringLiteral("Bluetooth discovery error");
        ppWarn() << "[IMU] BLE scan error:" << error << msg;
        // Surface to the UI (e.g. Bluetooth off / no adapter) so an empty list is
        // distinguishable from "no IMUs in range". Forwarded by DeviceEnumerator.
        emit scanError(msg);
        if (--m_pendingAgents <= 0)
            emit finished();
    }

private:
    void createAgent(const QBluetoothAddress &addr)
    {
        QBluetoothDeviceDiscoveryAgent *agent =
            addr.isNull() ? new QBluetoothDeviceDiscoveryAgent(this)
                          : new QBluetoothDeviceDiscoveryAgent(addr, this);
        agent->setLowEnergyDiscoveryTimeout(m_timeoutMs);

        connect(agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
                this,  &ImuBleScanner::onDeviceDiscovered);
        connect(agent, &QBluetoothDeviceDiscoveryAgent::finished,
                this,  &ImuBleScanner::onScanFinished);
        connect(agent, &QBluetoothDeviceDiscoveryAgent::canceled,
                this,  &ImuBleScanner::onScanFinished);
        connect(agent,
                QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(
                    &QBluetoothDeviceDiscoveryAgent::errorOccurred),
                this, &ImuBleScanner::onScanError);

        ppInfo() << "[IMU] BLE scan started on"
                 << (addr.isNull() ? QStringLiteral("default adapter")
                                   : addr.toString())
                 << "(timeout" << m_timeoutMs / 1000 << "s)";
        agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
        m_agents.append(agent);
    }

    QList<QBluetoothDeviceDiscoveryAgent *> m_agents;
    int m_pendingAgents = 0;

    // Captured once at construction (scan-arm time) — see the ctor comment.
    const bool m_hackMotionEnabled;

    // The HackMotion wG3 only advertises for a few seconds after a physical
    // button press (see WR_RECOMMENDED_SCAN_WINDOW_US in wrist/device.h),
    // so a short window routinely misses the button-press race entirely. 90 s
    // is the library's own recommended scan window; it costs nothing when a
    // device (of either vendor) appears immediately, and it also improves
    // WT901 discovery odds versus the old 30 s.
    static constexpr int kHackMotionScanWindowMs = WR_RECOMMENDED_SCAN_WINDOW_US / 1000;
    // Historical Witmotion-only window. With HackMotion discovery disabled
    // there is no button-press race to win, so a Witmotion-only user should
    // not be made to wait three times as long for a scan they cannot benefit
    // from — restore the pre-HackMotion 30 s.
    static constexpr int kWitmotionOnlyScanWindowMs = 30000;
    // Chosen once, from m_hackMotionEnabled, in the ctor init list — never
    // recomputed mid-scan.
    const int m_timeoutMs;
};

// ---------------------------------------------------------------------------
// DeviceEnumerator
// ---------------------------------------------------------------------------

DeviceEnumerator* DeviceEnumerator::instance()
{
    static DeviceEnumerator inst;
    return &inst;
}

DeviceEnumerator::DeviceEnumerator(QObject *parent)
    : QObject(parent)
{
    // Stop any running scan thread before Qt's parent-child destruction runs,
    // otherwise destroying a running QThread causes a fatal abort.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this]() {
        if (m_imuScanThread && m_imuScanThread->isRunning()) {
            m_imuScanThread->quit();
            if (!m_imuScanThread->wait(3000))
                m_imuScanThread->terminate();
        }
    });
}

void DeviceEnumerator::enumerate()
{
    // Individual camera / audio backends register themselves on demand.
}

QList<Device> DeviceEnumerator::devices() const
{
    return m_devices;
}

QList<Device> DeviceEnumerator::devices(DeviceType type) const
{
    QList<Device> result;
    for (const Device &dev : m_devices) {
        if (dev.type == type)
            result.append(dev);
    }
    return result;
}

void DeviceEnumerator::registerDevice(DeviceType type, VideoInputFactory::Backend backend,
                                       const QString &id, const QString &description,
                                       const CameraCapabilities &capabilities)
{
    for (const auto &dev : m_devices) {
        if (dev.type == type && dev.backend == backend && dev.id == id) return;
    }
    Device dev;
    dev.type         = type;
    dev.backend      = backend;
    dev.id           = id;
    dev.description  = description;
    dev.capabilities = capabilities;
    m_devices.append(dev);
    ppInfo() << "[Device] Registered:" << description;
    emit deviceAdded(dev);
}

void DeviceEnumerator::setHackMotionEnabled(bool on)
{
    // Just a stored value — ImuManager pushes AppSettings' hackmotion/enabled
    // in here before every scan (see setHackMotionEnabled() call site there).
    // Deliberately does nothing to an already-discovered, connected or
    // persisted wG3: this gates discovery only.
    m_hackMotionEnabled = on;
}

void DeviceEnumerator::registerImuDevice(ImuBase::Transport transport,
                                          const QString &id,
                                          const QString &description,
                                          const ImuCapabilities &capabilities,
                                          const QVariant &platformHandle,
                                          ImuVendor vendor)
{
    for (Device &dev : m_devices) {
        // Dedupe key includes vendor: transport+id alone is not unique across
        // vendors in principle (e.g. two different BLE stacks could in theory
        // hand out colliding ids), and a Device's vendor must never silently
        // flip on a refresh.
        if (dev.type == DeviceType::Imu && dev.imuTransport == transport &&
            dev.imuVendor == vendor && dev.id == id) {
            // Already known — refresh the platform handle. A re-scan (or a second
            // advertisement) carries newer data (RSSI, and on some platforms the
            // address type), and ImuInstance::start() connects on whatever handle
            // is stored here. Keep the entry + alias stable; the device list is
            // unchanged, so no deviceAdded re-emit. Stamp the current scan
            // generation so consumers know it was seen this scan.
            dev.platformHandle        = platformHandle;
            dev.lastSeenScanGeneration = m_imuScanGeneration;
            return;
        }
    }
    Device dev;
    dev.type           = DeviceType::Imu;
    dev.imuTransport   = transport;
    dev.imuVendor      = vendor;
    dev.id             = id;
    dev.description    = description;
    dev.imuCapabilities = capabilities;
    dev.platformHandle = platformHandle;
    dev.lastSeenScanGeneration = m_imuScanGeneration;
    m_devices.append(dev);
    ppInfo() << "[IMU] Device found:" << description << id;
    emit deviceAdded(dev);
}

bool DeviceEnumerator::scanImu()
{
    if (m_imuScanActive) return false;
    m_imuScanActive = true;
    // New scan generation — devices re-discovered below get stamped with it, so
    // anything left on an older generation was absent from this scan.
    ++m_imuScanGeneration;

    // --- Serial ---
    // TODO: enumerate serial ports (e.g. /dev/ttyUSB*, /dev/ttyACM*, COM*) and
    //       probe each one with WT9011DCL to confirm a WitMotion device responds.
    ppInfo() << "[IMU] Serial scan: stub — no serial devices enumerated";

    // --- BLE ---
    // Run in a worker thread so the discovery window (90 s, or 30 s with
    // HackMotion disabled) doesn't block the
    // main thread. The ImuBleScanner object lives on the worker thread.
    m_imuScanThread = new QThread(this);
    // Capture m_hackMotionEnabled here, once, as the scan is armed — the
    // scanner never re-reads it (see ImuBleScanner's ctor comment).
    auto *scanner = new ImuBleScanner(m_hackMotionEnabled);
    scanner->moveToThread(m_imuScanThread);

    // Kick the scanner once the thread's event loop is running
    connect(m_imuScanThread, &QThread::started,  scanner, &ImuBleScanner::start);
    // scanner is cleaned up when the thread finishes
    connect(m_imuScanThread, &QThread::finished, scanner, &QObject::deleteLater);

    // Surface discovery errors (Bluetooth off / no adapter) to the main thread so
    // the IMU UI can show an actionable message instead of an empty list.
    connect(scanner, &ImuBleScanner::scanError,
            this, [this](const QString &msg) { emit imuScanError(msg); },
            Qt::QueuedConnection);

    // Each matched BLE device is forwarded to the main thread via queued connection
    connect(scanner, &ImuBleScanner::deviceFound,
            this, [this](const QBluetoothDeviceInfo &info, ImuVendor vendor) {
                const QString id = info.address().isNull()
                    ? info.deviceUuid().toString()
                    : info.address().toString();

                ImuCapabilities caps;
                QString name;

                if (vendor == ImuVendor::HackMotion) {
                    name = info.name().isEmpty()
                        ? QStringLiteral(WR_ADVERTISED_LOCAL_NAME) : info.name();

                    caps.vendorName = QStringLiteral("HackMotion");
                    caps.modelName  = QStringLiteral(WR_ADVERTISED_LOCAL_NAME);
                    caps.transport  = ImuBase::Transport::Ble;
                    // The device reports battery every 30 s as a side effect of
                    // its mandatory keepalive (WR_KEEPALIVE_PERIOD_US) — always
                    // readable, unlike everything below.
                    caps.hasBattery = true;
                    // Both units carry an accelerometer and a gyroscope, and the
                    // device fuses them ON-DEVICE into a quaternion — which is the
                    // primary output and the reason there is no host-side filter
                    // choice for this vendor. It streams no Euler angles.
                    //
                    // ⚠ The accelerometer reports LINEAR acceleration with gravity
                    // already removed (≈0 at rest), which a Witmotion lane's accel
                    // is not. The flag says the sensor is present; it does not say
                    // the two vendors' accel channels are the same quantity.
                    caps.hasAccelerometer = true;
                    caps.hasGyroscope     = true;
                    caps.hasQuaternion    = true;
                    caps.hasEulerAngles   = false;
                    caps.dataIsFiltered   = true;
                    // Ranges are fixed by the stream configuration the device
                    // accepts (0x7e): accel ±32.8 g at 1 mg/LSB, and the extended
                    // gyro divisor of 8 giving 32768/8 °/s full scale.
                    caps.accelRange = { -32.8f, 32.8f };
                    caps.gyroRange  = { -4096.0f, 4096.0f };
                    // 6-axis only, and not as a choice: see hasMagnetometer below.
                    caps.supportsSixAxisFusion  = true;
                    caps.supportsNineAxisFusion = false;
                    // The wG3's output rate is adaptive and NOT settable by a
                    // client — leave both empty/zero so the UI has nothing to
                    // offer (contrast the WT901 branch below, which lists real
                    // selectable rates).
                    caps.supportedRatesHz.clear();
                    caps.defaultRateHz              = 0;
                    caps.supportsOutputRateControl  = false;
                    // No host-side zero of any kind on this device, and its
                    // magnetometer is unreachable in every configuration it
                    // accepts — so neither heading-zero nor mag calibration
                    // (nor the underlying sensor) can be advertised as present.
                    caps.hasMagnetometer            = false;
                    caps.supportsHeadingZero        = false;
                    caps.supportsMagCalibration     = false;
                    caps.supportsAngleReference     = false;
                    caps.queriedAt                  = QDateTime::currentDateTime();
                } else {
                    name = info.name().isEmpty()
                        ? QStringLiteral("WT901 Series") : info.name();

                    // Build capabilities using the shared WT901 defaults
                    caps = WT9011DCL_Base::wt901Defaults();
                    caps.modelName                    = QStringLiteral("WT901 Series");
                    caps.transport                    = ImuBase::Transport::Ble;
                    caps.hasMagnetometer              = false;
                    caps.hasTemperature               = false;
                    caps.hasBattery                   = true;
                    caps.supportsBaudRateControl      = false;
                    caps.supportsOutputDataSelection  = false;
                    caps.queriedAt                    = QDateTime::currentDateTime();
                }

                registerImuDevice(ImuBase::Transport::Ble, id, name, caps,
                                  QVariant::fromValue(info), vendor);
            }, Qt::QueuedConnection);

    connect(scanner, &ImuBleScanner::finished,
            this, [this]() {
                // Do NOT clear m_imuScanActive here. quit() only POSTS the exit
                // request; the thread keeps running until its loop processes it
                // and QThread::finished fires (below). Clearing the guard now
                // opens a window where rescanImu() passes the m_imuScanActive
                // gate and overwrites m_imuScanThread — leaking the old thread,
                // and then the OLD thread's finished slot nulls the NEW thread's
                // pointer (so aboutToQuit can't stop it → fatal abort). Keep the
                // guard set until the thread has fully stopped.
                if (m_imuScanThread) m_imuScanThread->quit();
                // Publish the just-finished generation so consumers can prune
                // devices that didn't re-appear this scan.
                m_imuScanGenerationCompleted = m_imuScanGeneration;
                emit imuScanFinished();
                ppInfo() << "[IMU] Scan complete —"
                          << devices(DeviceType::Imu).count() << "IMU device(s) registered";
            }, Qt::QueuedConnection);

    // Clear the bookkeeping only after the thread has FULLY stopped. Capture the
    // thread by value and null the member only if it still points at this very
    // thread, so a (defensively) newer scan can't have its pointer clobbered.
    QThread *scanThread = m_imuScanThread;
    connect(m_imuScanThread, &QThread::finished, this, [this, scanThread]() {
        if (m_imuScanThread == scanThread)
            m_imuScanThread = nullptr;
        m_imuScanActive = false;
    });

    m_imuScanThread->start();
    return true;
}

#include "device_enumerator.moc"
