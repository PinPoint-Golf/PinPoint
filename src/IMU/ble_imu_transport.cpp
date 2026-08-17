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

#include "ble_imu_transport.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

BleImuTransport::BleImuTransport(const UuidConfig &uuids, QObject *parent)
    : QObject(parent)
    , m_uuids(uuids)
{
    // Connect/discovery watchdog — see kConnectWatchdogMs. Started in doConnect(),
    // stopped on reaching Ready (descriptorWritten) or on any teardown. If it
    // fires the attempt is wedged: fail it so the owner's retry path engages.
    //
    // Parent it to this transport so it MOVES WITH US to the I/O thread. The
    // transport is constructed on the GUI thread and then moveToThread'd (as a
    // child of WT9011DCL_BLE) onto the IMU I/O thread; moveToThread carries
    // QObject children but NOT parentless value members, so without this the
    // timer would keep GUI-thread affinity and start()ing it from doConnect() (on
    // the I/O thread) trips "QObject::startTimer: Timers cannot be started from
    // another thread" — and a fire would run failConnection() on the wrong thread.
    m_connectWatchdog.setParent(this);
    m_connectWatchdog.setSingleShot(true);
    connect(&m_connectWatchdog, &QTimer::timeout, this, [this]() {
        failConnection(
            QStringLiteral("Connect/discovery timed out after %1 ms — GATT never became ready")
                .arg(m_connectTimer.isValid() ? m_connectTimer.elapsed() : kConnectWatchdogMs));
    });
}

BleImuTransport::~BleImuTransport()
{
    stopScan();
    teardownController();
}

BleImuTransport::UuidConfig BleImuTransport::explicitUuids(const QBluetoothUuid &service,
                                                           const QBluetoothUuid &notify,
                                                           const QBluetoothUuid &write,
                                                           int minAttMtu)
{
    UuidConfig cfg;
    // The fragments stay empty: usesExplicitUuids() is what selects the path, and
    // setupService() never reads them once it is true.
    cfg.serviceUuid = service;
    cfg.notifyUuid  = notify;
    cfg.writeUuid   = write;      // null == one bidirectional characteristic
    cfg.minAttMtu   = minAttMtu;
    return cfg;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void BleImuTransport::setState(State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void BleImuTransport::teardownController()
{
    m_connectWatchdog.stop();   // controller is going away — disarm the connect watchdog
    // On Windows/WinRT, QLowEnergyController::disconnectFromDevice() emits
    // disconnected() *synchronously*, which re-enters this function via
    // onControllerDisconnected(). (On Linux/BlueZ the signal is async, so the
    // re-entry sees null members and no-ops.) Guard against re-entrancy two ways:
    //   1. Disconnect the object's signals before acting, severing the
    //      disconnected -> onControllerDisconnected -> teardownController wire.
    //   2. Move each member to a local and null the member *before* calling into
    //      the object, so any re-entry sees null and bails while we finish the
    //      teardown via our private local handle.
    if (m_service) {
        QLowEnergyService *service = m_service;
        m_service = nullptr;
        disconnect(service, nullptr, this, nullptr);
        service->deleteLater();
    }
    if (m_controller) {
        QLowEnergyController *controller = m_controller;
        m_controller = nullptr;
        disconnect(controller, nullptr, this, nullptr);
        controller->disconnectFromDevice();
        controller->deleteLater();
    }
    m_writeChar  = QLowEnergyCharacteristic{};
    m_notifyChar = QLowEnergyCharacteristic{};
    // The MTU belongs to the link that just went away. Clearing it here (rather
    // than leaving the last value as a souvenir) keeps mtu() describing the
    // CURRENT attempt — connectToDevice() tears down first, so a retry never
    // inherits the previous link's number.
    m_mtu = -1;
}

void BleImuTransport::failConnection(const QString &message)
{
    // Every connect/discovery/service error funnels here so the failure leaves a
    // CLEAN slate: drop the connect-phase scan, tear the controller/service down
    // (releasing the HCI link and severing the still-armed descriptorWritten
    // lambda), then transition to Error. Previously the error exits only
    // setState(Error)+emit, leaking the link and lambda until the next connect.
    m_waitingForScanConfirm = false;
    stopScan();
    teardownController();
    setState(State::Error);
    emit errorOccurred(message);
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void BleImuTransport::ensureScannerCreated()
{
    if (m_scanner) return;
#ifdef Q_OS_LINUX
    if (!m_localAdapter.isNull())
        m_scanner = new QBluetoothDeviceDiscoveryAgent(m_localAdapter, this);
    else
#endif
        m_scanner = new QBluetoothDeviceDiscoveryAgent(this);
    connect(m_scanner, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
            this,      &BleImuTransport::onDeviceDiscovered);
    connect(m_scanner, &QBluetoothDeviceDiscoveryAgent::finished,
            this,      &BleImuTransport::onScanFinished);
    connect(m_scanner, &QBluetoothDeviceDiscoveryAgent::canceled,
            this,      &BleImuTransport::onScanFinished);
    connect(m_scanner,
            QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(
                &QBluetoothDeviceDiscoveryAgent::errorOccurred),
            this, &BleImuTransport::onScanError);
}

void BleImuTransport::stopScan()
{
    if (m_scanner && m_scanner->isActive())
        m_scanner->stop();
}

void BleImuTransport::onDeviceDiscovered(const QBluetoothDeviceInfo &device)
{
#ifdef Q_OS_LINUX
    // If we are waiting for the target device to appear in the scan (connect phase),
    // call doConnect() as soon as we see it — the scan is still active so the BlueZ
    // advertising cache is warm at the moment connectToDevice() is issued.
    if (m_waitingForScanConfirm && matchesPendingDevice(device)) {
        m_waitingForScanConfirm = false;
        emit diagnosticInfo(QStringLiteral("Target seen in scan — connecting now (scan still active)"));
        doConnect();
    }
#else
    Q_UNUSED(device)
#endif
}

void BleImuTransport::onScanFinished()
{
    // The discovery agent is only ever started here for the Linux connect-phase
    // scan (warming the BlueZ advertising cache before connectToDevice). If it
    // finishes/cancels while we were still waiting for the target, the device
    // never appeared — fail so the owning class can retry. Any other finish
    // (e.g. cancel after the link is up) is a no-op.
#ifdef Q_OS_LINUX
    if (m_waitingForScanConfirm) {
        m_waitingForScanConfirm = false;
        setState(State::Error);
        emit errorOccurred(QStringLiteral("Device not found during connection scan — check device is powered on and advertising"));
    }
#endif
}

void BleImuTransport::onScanError(QBluetoothDeviceDiscoveryAgent::Error error)
{
    Q_UNUSED(error)
    // m_scanner outlives failConnection() (teardownController only touches the
    // controller/service), so its errorString() is still valid here.
    failConnection(m_scanner->errorString());
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void BleImuTransport::connectToDevice(const QBluetoothDeviceInfo &device,
                                       const QBluetoothAddress &localAdapter)
{
    setState(State::Connecting);
    teardownController();

    // Reset the scanner if the adapter has changed (e.g. a retry was assigned a
    // different adapter by BleAdapterPool). ensureScannerCreated() uses m_localAdapter,
    // so it must be updated before the scanner is (re)created.
    if (localAdapter != m_localAdapter) {
        if (m_scanner) {
            m_scanner->stop();
            m_scanner->deleteLater();
            m_scanner = nullptr;
        }
        m_localAdapter = localAdapter;
    }

    m_pendingDevice = device;

#ifdef Q_OS_LINUX
    // BlueZ 6.x requires an active HCI scan when connectToDevice() is called —
    // the kernel's advertising cache expires quickly when no scan is running.
    // Start a scan now; doConnect() is called from onDeviceDiscovered() once the
    // target is confirmed in the cache, while the scan is still active.
    m_waitingForScanConfirm = true;
    ensureScannerCreated();
    if (!m_scanner->isActive()) {
        m_scanner->setLowEnergyDiscoveryTimeout(kConnectScanMs);
        m_scanner->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    }
    emit diagnosticInfo(QStringLiteral("Scan started to warm BlueZ cache — will connect to %1 when seen")
                        .arg(device.address().toString()));
#else
    stopScan();
    doConnect();
#endif
}

void BleImuTransport::doConnect()
{
    // On Linux the scan is still active here — BlueZ cache is warm.
    // On other platforms stopScan() was already called before this.
#ifdef Q_OS_LINUX
    if (!m_localAdapter.isNull())
        m_controller = QLowEnergyController::createCentral(m_pendingDevice, m_localAdapter, this);
    else
#endif
        m_controller = QLowEnergyController::createCentral(m_pendingDevice, this);

    emit diagnosticInfo(
        QStringLiteral("Connecting to %1 via adapter %2 (RSSI=%3 dBm)")
            .arg(m_pendingDevice.address().toString())
            .arg(m_localAdapter.isNull() ? QStringLiteral("default")
                                         : m_localAdapter.toString())
            .arg(m_pendingDevice.rssi()));

    m_connectTimer.start();
    m_connectWatchdog.start(kConnectWatchdogMs);   // armed until Ready or teardown

    connect(m_controller, &QLowEnergyController::connected,
            this,         &BleImuTransport::onControllerConnected);
    connect(m_controller, &QLowEnergyController::disconnected,
            this,         &BleImuTransport::onControllerDisconnected);
    connect(m_controller, &QLowEnergyController::serviceDiscovered,
            this,         &BleImuTransport::onServiceDiscovered);
    connect(m_controller, &QLowEnergyController::discoveryFinished,
            this,         &BleImuTransport::onServiceDiscoveryFinished);
    connect(m_controller,
            QOverload<QLowEnergyController::Error>::of(
                &QLowEnergyController::errorOccurred),
            this, &BleImuTransport::onControllerError);
    // MTU negotiation is not part of the connect handshake on every backend — the
    // value can land after connected() and change again during service discovery.
    // Track the signal rather than sampling once, so mtu() is never stale.
    connect(m_controller, &QLowEnergyController::mtuChanged,
            this,         &BleImuTransport::onMtuChanged);

    m_controller->connectToDevice();
}

bool BleImuTransport::matchesPendingDevice(const QBluetoothDeviceInfo &d) const
{
    if (!m_pendingDevice.isValid()) return false;
    if (!d.address().isNull() && !m_pendingDevice.address().isNull())
        return d.address() == m_pendingDevice.address();
    if (!d.deviceUuid().isNull() && !m_pendingDevice.deviceUuid().isNull())
        return d.deviceUuid() == m_pendingDevice.deviceUuid();
    return false;
}

void BleImuTransport::disconnectFromDevice()
{
    // Only signal disconnected() if we were actually connected/connecting — a
    // disconnectFromDevice() call from an already-Disconnected state (e.g. owner
    // teardown after a controller-initiated disconnect already fired) must not
    // emit a second, spurious disconnected(). setState already dedupes the state
    // transition; mirror that for the raw signal.
    const bool notify = (m_state != State::Disconnected);
    teardownController();
    setState(State::Disconnected);
    if (notify) emit disconnected();
}

// ---------------------------------------------------------------------------
// Controller slots
// ---------------------------------------------------------------------------

void BleImuTransport::onControllerConnected()
{
    // HCI connection established — the scan is no longer needed. GATT service
    // discovery proceeds over the L2CAP link and does not require advertising.
    stopScan();
    emit diagnosticInfo(
        QStringLiteral("BLE link established in %1 ms — discovering services")
        .arg(m_connectTimer.elapsed()));
    // Seed the MTU from the controller in case it was negotiated before we could
    // connect to mtuChanged (or the backend never emits it at all). onMtuChanged
    // de-dupes, so a backend that reports nothing yet (-1) stays silent here and
    // the real value still arrives by signal.
    onMtuChanged(m_controller->mtu());

    setState(State::DiscoveringServices);
    m_controller->discoverServices();
}

void BleImuTransport::onControllerDisconnected()
{
    const bool notify = (m_state != State::Disconnected);
    teardownController();
    setState(State::Disconnected);
    if (notify) emit disconnected();
}

void BleImuTransport::onServiceDiscovered(const QBluetoothUuid &uuid)
{
    emit diagnosticInfo(QStringLiteral("BLE service: %1").arg(uuid.toString()));
}

void BleImuTransport::onServiceDiscoveryFinished()
{
    setupService();
}

void BleImuTransport::onMtuChanged(int newMtu)
{
    if (newMtu == m_mtu) return;
    m_mtu = newMtu;
    emit diagnosticInfo(QStringLiteral("ATT MTU now %1 bytes (%2 usable payload)")
                        .arg(newMtu).arg(newMtu - 3));
    emit mtuChanged(newMtu);
}

void BleImuTransport::onControllerError(QLowEnergyController::Error error)
{
    // Read the errorString() BEFORE failConnection() tears the controller down.
    const QString errStr = m_controller ? m_controller->errorString()
                                        : QStringLiteral("(controller already torn down)");
    failConnection(
        QStringLiteral("Controller error %1: %2 (after %3 ms)")
        .arg(static_cast<int>(error))
        .arg(errStr)
        .arg(m_connectTimer.elapsed()));
}

// ---------------------------------------------------------------------------
// Service setup
// ---------------------------------------------------------------------------

void BleImuTransport::setupService()
{
    const bool explicitMode = m_uuids.usesExplicitUuids();

    QBluetoothUuid svcUuid;
    if (explicitMode) {
        // Exact 128-bit equality — see UuidConfig. A substring match here could
        // land on the wG3's inert ISSC pipe, which shares the transparent-UART
        // service's 49535343 base, accepts every write and answers none.
        for (const QBluetoothUuid &uuid : m_controller->services()) {
            if (uuid == m_uuids.serviceUuid) {
                svcUuid = uuid;
                break;
            }
        }
    } else {
        // Search by the 16-bit fragment from UuidConfig rather than an exact 128-bit
        // match — different hardware revisions use different base UUID suffixes.
        for (const QBluetoothUuid &uuid : m_controller->services()) {
            if (uuid.toString().contains(m_uuids.serviceFragment, Qt::CaseInsensitive)) {
                svcUuid = uuid;
                break;
            }
        }
    }

    if (svcUuid.isNull()) {
        QString found;
        for (const QBluetoothUuid &uuid : m_controller->services())
            found += QStringLiteral("\n  ") + uuid.toString();
        // Name what was actually looked for. In explicit mode serviceFragment is
        // empty, and "BLE service '' not found" is a bug report nobody can act on.
        failConnection(QStringLiteral("BLE service '%1' not found. Device has:%2")
                       .arg(explicitMode ? m_uuids.serviceUuid.toString()
                                         : m_uuids.serviceFragment,
                            found));
        return;
    }

    if (explicitMode) {
        // No derivation: the characteristics were given to us in full precisely
        // because they do not share the service's base.
        m_resolvedServiceUuid = svcUuid;
        m_resolvedNotifyUuid  = m_uuids.notifyUuid;
        // A null writeUuid declares one bidirectional characteristic (the wG3),
        // so write resolves onto the notify UUID and both handles below end up
        // referring to the same characteristic. Nothing downstream assumes they
        // differ — see enableNotifications() and writeToDevice().
        m_resolvedWriteUuid   = m_uuids.writeUuid.isNull() ? m_uuids.notifyUuid
                                                           : m_uuids.writeUuid;
    } else {
        // Derive characteristic UUIDs from the same base as the discovered service.
        const QString base = svcUuid.toString().mid(1, 36); // strip Qt's surrounding braces
        QString notifyStr = base;
        notifyStr.replace(m_uuids.serviceFragment, m_uuids.notifyFragment, Qt::CaseInsensitive);
        QString writeStr = base;
        writeStr.replace(m_uuids.serviceFragment, m_uuids.writeFragment, Qt::CaseInsensitive);
        m_resolvedServiceUuid = svcUuid;
        m_resolvedNotifyUuid  = QBluetoothUuid(notifyStr);
        m_resolvedWriteUuid   = QBluetoothUuid(writeStr);
    }

    m_service = m_controller->createServiceObject(m_resolvedServiceUuid, this);
    if (!m_service) {
        failConnection(QStringLiteral("Failed to create BLE service object"));
        return;
    }

    connect(m_service, &QLowEnergyService::stateChanged,
            this,      &BleImuTransport::onServiceStateChanged);
    connect(m_service, &QLowEnergyService::characteristicChanged,
            this,      &BleImuTransport::onCharacteristicChanged);
    connect(m_service,
            QOverload<QLowEnergyService::ServiceError>::of(
                &QLowEnergyService::errorOccurred),
            this, &BleImuTransport::onServiceError);

    m_service->discoverDetails();
}

bool BleImuTransport::checkMtuRequirement()
{
    if (m_uuids.minAttMtu <= 0)
        return true;                      // device doesn't care — the Witmotion path

    const int negotiated = m_mtu;

    // Some backends only produce an MTU once ATT traffic has flowed. We are past
    // service-detail discovery here, so a non-positive value means the platform
    // never told us — NOT that the MTU is small. Say so and let the link proceed:
    // failing on a number we could not read would break links that work.
    if (negotiated <= 0) {
        emit diagnosticInfo(
            QStringLiteral("ATT MTU not reported by this platform — cannot verify the "
                           "%1-byte minimum this device needs; continuing")
                .arg(m_uuids.minAttMtu));
        return true;
    }

    if (negotiated < m_uuids.minAttMtu) {
        // Its own failure mode, deliberately not folded into the generic connect
        // error: the link came up, GATT resolved, the characteristics are right —
        // only the payload size is wrong, and NOTHING the application can do fixes
        // it, because no Qt platform exposes an MTU request. Emit the typed signal
        // for callers that can react, then funnel through failConnection() so the
        // teardown is the same as every other failure and the message alone is
        // enough to diagnose it from a log.
        emit mtuTooSmall(negotiated, m_uuids.minAttMtu);
        failConnection(QStringLiteral(
            "ATT MTU too small: negotiated %1 bytes (%2 usable payload), this device "
            "requires at least %3. Its longer frames would arrive truncated and parse "
            "as garbage. The link is otherwise healthy and no application-side request "
            "can raise the MTU — this is a platform/adapter limitation.")
            .arg(negotiated).arg(negotiated - 3).arg(m_uuids.minAttMtu));
        return false;
    }

    emit diagnosticInfo(QStringLiteral("ATT MTU %1 bytes meets the %2-byte minimum")
                        .arg(negotiated).arg(m_uuids.minAttMtu));
    return true;
}

void BleImuTransport::enableNotifications()
{
    // Gate on the MTU here rather than at connected(): by this point service
    // details have been discovered, so ATT traffic has flowed and every backend
    // that will ever report an MTU has reported one. It is also still BEFORE
    // gattReady(), so the owner never sends an init command over a link whose
    // replies could not fit. checkMtuRequirement() has already failed the
    // connection when it returns false.
    if (!checkMtuRequirement())
        return;

    // These may resolve to the SAME characteristic — the explicit form allows one
    // bidirectional characteristic carrying Notify | Write | WriteNoResponse at
    // once. Everything below reads properties off whichever handle it needs, so a
    // combined characteristic satisfies both the notify checks and writeToDevice().
    m_notifyChar = m_service->characteristic(m_resolvedNotifyUuid);
    m_writeChar  = m_service->characteristic(m_resolvedWriteUuid);

    if (!m_notifyChar.isValid() || !m_writeChar.isValid()) {
        failConnection(QStringLiteral("Required BLE characteristics not found"));
        return;
    }

    const bool canNotify   = m_notifyChar.properties() & QLowEnergyCharacteristic::Notify;
    const bool canIndicate = m_notifyChar.properties() & QLowEnergyCharacteristic::Indicate;

    if (!canNotify && !canIndicate) {
        failConnection(QStringLiteral("Notify characteristic supports neither Notify nor Indicate (props=0x%1)")
                       .arg(static_cast<int>(m_notifyChar.properties()), 0, 16));
        return;
    }

    const QLowEnergyDescriptor cccd = m_notifyChar.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);

    if (!cccd.isValid()) {
        failConnection(QStringLiteral("Notify characteristic has no CCCD descriptor"));
        return;
    }

    // On macOS, setNotifyValue:YES is asynchronous — data won't flow until
    // peripheral:didUpdateNotificationStateForCharacteristic: fires. Defer
    // Ready state until descriptorWritten confirms the subscription is live.
    auto *svc = m_service;
    connect(svc, &QLowEnergyService::descriptorWritten, this,
            [this, svc](const QLowEnergyDescriptor &d, const QByteArray &) {
        if (d.type() != QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration)
            return;
        disconnect(svc, &QLowEnergyService::descriptorWritten, this, nullptr);
        m_connectWatchdog.stop();   // reached Ready — disarm (controller stays alive)
        // Notify the owning class so it can send device-specific init commands
        // before we advertise ourselves as Ready.
        emit gattReady();
        setState(State::Ready);
        emit connected();
    });

    const QByteArray cccdValue = canNotify ? QByteArray::fromHex("0100")
                                           : QByteArray::fromHex("0200");
    m_service->writeDescriptor(cccd, cccdValue);
}

// ---------------------------------------------------------------------------
// Service slots
// ---------------------------------------------------------------------------

void BleImuTransport::onServiceStateChanged(QLowEnergyService::ServiceState newState)
{
    if (newState == QLowEnergyService::RemoteServiceDiscovered)
        enableNotifications();
}

void BleImuTransport::onCharacteristicChanged(const QLowEnergyCharacteristic &c,
                                               const QByteArray &value)
{
    Q_UNUSED(c)
    emit dataReceived(value);
}

void BleImuTransport::onServiceError(QLowEnergyService::ServiceError error)
{
    failConnection(QStringLiteral("BLE service error (%1)").arg(static_cast<int>(error)));
}

// ---------------------------------------------------------------------------
// Transport write
// ---------------------------------------------------------------------------

void BleImuTransport::writeToDevice(const QByteArray &data)
{
    if (!m_service || !m_writeChar.isValid())
        return;

    // m_writeChar may be the very same characteristic as m_notifyChar (explicit
    // single-characteristic devices). That is harmless: the mode below is chosen
    // from this handle's own properties, and a characteristic carrying
    // Notify | Write | WriteNoResponse simply takes the no-response branch, which
    // is what a transparent-UART pipe wants anyway.
    const QLowEnergyService::WriteMode mode =
        (m_writeChar.properties() & QLowEnergyCharacteristic::WriteNoResponse)
            ? QLowEnergyService::WriteWithoutResponse
            : QLowEnergyService::WriteWithResponse;

    m_service->writeCharacteristic(m_writeChar, data, mode);
}

void BleImuTransport::writeToDevice(const QByteArray &data, bool withoutResponse)
{
    if (!m_service || !m_writeChar.isValid())
        return;

    // Deliberately NOT implemented in terms of the overload above, and the
    // overload above deliberately unchanged: the Witmotion path goes through it
    // on every command and must keep behaving exactly as it does today.
    const QLowEnergyCharacteristic::PropertyTypes props = m_writeChar.properties();
    const bool canWithoutResponse = props & QLowEnergyCharacteristic::WriteNoResponse;
    const bool canWithResponse    = props & QLowEnergyCharacteristic::Write;

    // Fall back to whatever the characteristic does support: a write issued in a
    // mode the characteristic lacks is rejected by the stack, so honouring an
    // unsupported preference would drop the command rather than downgrade it.
    const QLowEnergyService::WriteMode mode = withoutResponse
        ? (canWithoutResponse ? QLowEnergyService::WriteWithoutResponse
                              : QLowEnergyService::WriteWithResponse)
        : (canWithResponse    ? QLowEnergyService::WriteWithResponse
                              : QLowEnergyService::WriteWithoutResponse);

    m_service->writeCharacteristic(m_writeChar, data, mode);
}
