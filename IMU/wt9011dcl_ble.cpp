#include "wt9011dcl_ble.h"

QBluetoothUuid WT9011DCL_BLE::ServiceUuid =
    QBluetoothUuid(QStringLiteral("0000ffe5-0000-1000-8000-00805f9b34fb"));

QBluetoothUuid WT9011DCL_BLE::NotifyCharUuid =
    QBluetoothUuid(QStringLiteral("0000ffe4-0000-1000-8000-00805f9b34fb"));

QBluetoothUuid WT9011DCL_BLE::WriteCharUuid =
    QBluetoothUuid(QStringLiteral("0000ffe9-0000-1000-8000-00805f9b34fb"));

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WT9011DCL_BLE::WT9011DCL_BLE(QObject *parent)
    : WT9011DCL_Base(parent)
{}

WT9011DCL_BLE::~WT9011DCL_BLE()
{
    stopScan();
    teardownController();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void WT9011DCL_BLE::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void WT9011DCL_BLE::teardownController()
{
    if (m_service) {
        m_service->deleteLater();
        m_service = nullptr;
    }
    if (m_controller) {
        m_controller->disconnectFromDevice();
        m_controller->deleteLater();
        m_controller = nullptr;
    }
    m_writeChar  = QLowEnergyCharacteristic{};
    m_notifyChar = QLowEnergyCharacteristic{};
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void WT9011DCL_BLE::scan(int durationMs)
{
    if (m_state == State::Scanning)
        return;

    if (!m_scanner) {
        m_scanner = new QBluetoothDeviceDiscoveryAgent(this);
        connect(m_scanner, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
                this,      &WT9011DCL_BLE::onDeviceDiscovered);
        connect(m_scanner, &QBluetoothDeviceDiscoveryAgent::finished,
                this,      &WT9011DCL_BLE::onScanFinished);
        connect(m_scanner, &QBluetoothDeviceDiscoveryAgent::canceled,
                this,      &WT9011DCL_BLE::onScanFinished);
        connect(m_scanner,
                QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(
                    &QBluetoothDeviceDiscoveryAgent::errorOccurred),
                this, &WT9011DCL_BLE::onScanError);
    }

    m_scanner->setLowEnergyDiscoveryTimeout(durationMs);
    setState(State::Scanning);
    m_scanner->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void WT9011DCL_BLE::stopScan()
{
    if (m_scanner && m_scanner->isActive())
        m_scanner->stop();
}

void WT9011DCL_BLE::onDeviceDiscovered(const QBluetoothDeviceInfo &device)
{
    if (device.serviceUuids().contains(ServiceUuid) ||
        device.name().contains(QStringLiteral("WT"), Qt::CaseInsensitive) ||
        device.name().contains(QStringLiteral("WITMOTION"), Qt::CaseInsensitive)) {
        emit deviceDiscovered(device);
    }
}

void WT9011DCL_BLE::onScanFinished()
{
    if (m_state == State::Scanning)
        setState(State::Disconnected);
    emit scanFinished();
}

void WT9011DCL_BLE::onScanError(QBluetoothDeviceDiscoveryAgent::Error error)
{
    Q_UNUSED(error)
    setState(State::Error);
    emit errorOccurred(m_scanner->errorString());
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void WT9011DCL_BLE::connectToDevice(const QBluetoothDeviceInfo &device)
{
    stopScan();
    teardownController();

    m_controller = QLowEnergyController::createCentral(device, this);

    connect(m_controller, &QLowEnergyController::connected,
            this,         &WT9011DCL_BLE::onControllerConnected);
    connect(m_controller, &QLowEnergyController::disconnected,
            this,         &WT9011DCL_BLE::onControllerDisconnected);
    connect(m_controller, &QLowEnergyController::serviceDiscovered,
            this,         &WT9011DCL_BLE::onServiceDiscovered);
    connect(m_controller, &QLowEnergyController::discoveryFinished,
            this,         &WT9011DCL_BLE::onServiceDiscoveryFinished);
    connect(m_controller,
            QOverload<QLowEnergyController::Error>::of(
                &QLowEnergyController::errorOccurred),
            this, &WT9011DCL_BLE::onControllerError);

    setState(State::Connecting);
    m_controller->connectToDevice();
}

void WT9011DCL_BLE::disconnectFromDevice()
{
    teardownController();
    setState(State::Disconnected);
    emit disconnected();
}

// ---------------------------------------------------------------------------
// Controller slots
// ---------------------------------------------------------------------------

void WT9011DCL_BLE::onControllerConnected()
{
    setState(State::DiscoveringServices);
    m_controller->discoverServices();
}

void WT9011DCL_BLE::onControllerDisconnected()
{
    teardownController();
    setState(State::Disconnected);
    emit disconnected();
}

void WT9011DCL_BLE::onServiceDiscovered(const QBluetoothUuid &uuid)
{
    Q_UNUSED(uuid)
}

void WT9011DCL_BLE::onServiceDiscoveryFinished()
{
    setupService();
}

void WT9011DCL_BLE::onControllerError(QLowEnergyController::Error error)
{
    Q_UNUSED(error)
    setState(State::Error);
    emit errorOccurred(m_controller->errorString());
}

// ---------------------------------------------------------------------------
// Service setup
// ---------------------------------------------------------------------------

void WT9011DCL_BLE::setupService()
{
    if (!m_controller->services().contains(ServiceUuid)) {
        setState(State::Error);
        emit errorOccurred(QStringLiteral("WITMOTION BLE service not found on device"));
        return;
    }

    m_service = m_controller->createServiceObject(ServiceUuid, this);
    if (!m_service) {
        setState(State::Error);
        emit errorOccurred(QStringLiteral("Failed to create BLE service object"));
        return;
    }

    connect(m_service, &QLowEnergyService::stateChanged,
            this,      &WT9011DCL_BLE::onServiceStateChanged);
    connect(m_service, &QLowEnergyService::characteristicChanged,
            this,      &WT9011DCL_BLE::onCharacteristicChanged);
    connect(m_service,
            QOverload<QLowEnergyService::ServiceError>::of(
                &QLowEnergyService::errorOccurred),
            this, &WT9011DCL_BLE::onServiceError);

    m_service->discoverDetails();
}

void WT9011DCL_BLE::enableNotifications()
{
    m_notifyChar = m_service->characteristic(NotifyCharUuid);
    m_writeChar  = m_service->characteristic(WriteCharUuid);

    if (!m_notifyChar.isValid() || !m_writeChar.isValid()) {
        setState(State::Error);
        emit errorOccurred(QStringLiteral("Required BLE characteristics not found"));
        return;
    }

    const QLowEnergyDescriptor cccd = m_notifyChar.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);

    if (!cccd.isValid()) {
        setState(State::Error);
        emit errorOccurred(QStringLiteral("Notify characteristic has no CCCD descriptor"));
        return;
    }

    m_service->writeDescriptor(cccd, QByteArray::fromHex("0100"));
    setState(State::Ready);
    emit connected();
}

// ---------------------------------------------------------------------------
// Service slots
// ---------------------------------------------------------------------------

void WT9011DCL_BLE::onServiceStateChanged(QLowEnergyService::ServiceState newState)
{
    if (newState == QLowEnergyService::RemoteServiceDiscovered)
        enableNotifications();
}

void WT9011DCL_BLE::onCharacteristicChanged(const QLowEnergyCharacteristic &c,
                                             const QByteArray &value)
{
    if (c.uuid() == NotifyCharUuid)
        receiveData(value);
}

void WT9011DCL_BLE::onServiceError(QLowEnergyService::ServiceError error)
{
    setState(State::Error);
    emit errorOccurred(QStringLiteral("BLE service error (%1)").arg(static_cast<int>(error)));
}

// ---------------------------------------------------------------------------
// Transport write
// ---------------------------------------------------------------------------

void WT9011DCL_BLE::writeToDevice(const QByteArray &data)
{
    if (!m_service || !m_writeChar.isValid())
        return;

    const QLowEnergyService::WriteMode mode =
        (m_writeChar.properties() & QLowEnergyCharacteristic::WriteNoResponse)
            ? QLowEnergyService::WriteWithoutResponse
            : QLowEnergyService::WriteWithResponse;

    m_service->writeCharacteristic(m_writeChar, data, mode);
}
