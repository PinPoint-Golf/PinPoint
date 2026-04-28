#pragma once

#include "wt9011dcl_base.h"
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QLowEnergyCharacteristic>

// WT9011DCL driver — Bluetooth BLE transport.
//
// The device exposes a custom BLE UART bridge service; the binary packet
// protocol is identical to the UART version, only the transport differs.
//
// BLE UUIDs (WITMOTION custom UART service):
//   Service  : 0000ffe5-0000-1000-8000-00805f9b34fb
//   Notify   : 0000ffe4-0000-1000-8000-00805f9b34fb  (device → host)
//   Write    : 0000ffe9-0000-1000-8000-00805f9b34fb  (host → device)
//
// Usage:
//   WT9011DCL_BLE imu;
//   connect(&imu, &WT9011DCL_BLE::deviceDiscovered, this, [&](auto &info){
//       imu.connectToDevice(info);
//   });
//   connect(&imu, &WT9011DCL_BLE::eulerAnglesUpdated, this, &MyClass::onAngles);
//   imu.scan();

class WT9011DCL_BLE : public WT9011DCL_Base
{
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Scanning,
        Connecting,
        DiscoveringServices,
        Ready,
        Error,
    };
    Q_ENUM(State)

    // Override these statics before scanning if your firmware uses different UUIDs.
    static QBluetoothUuid ServiceUuid;
    static QBluetoothUuid NotifyCharUuid;
    static QBluetoothUuid WriteCharUuid;

    explicit WT9011DCL_BLE(QObject *parent = nullptr);
    ~WT9011DCL_BLE() override;

    State state()   const { return m_state; }
    bool  isReady() const { return m_state == State::Ready; }

    // Scan for nearby BLE devices; durationMs=0 scans until stopScan().
    void scan(int durationMs = 10000);
    void stopScan();

    void connectToDevice(const QBluetoothDeviceInfo &device);
    void disconnectFromDevice();

signals:
    void stateChanged(WT9011DCL_BLE::State state);
    void deviceDiscovered(const QBluetoothDeviceInfo &device);
    void scanFinished();

protected:
    void writeToDevice(const QByteArray &data) override;

private slots:
    void onDeviceDiscovered(const QBluetoothDeviceInfo &device);
    void onScanFinished();
    void onScanError(QBluetoothDeviceDiscoveryAgent::Error error);

    void onControllerConnected();
    void onControllerDisconnected();
    void onServiceDiscovered(const QBluetoothUuid &uuid);
    void onServiceDiscoveryFinished();
    void onControllerError(QLowEnergyController::Error error);

    void onServiceStateChanged(QLowEnergyService::ServiceState newState);
    void onCharacteristicChanged(const QLowEnergyCharacteristic &c,
                                 const QByteArray &value);
    void onServiceError(QLowEnergyService::ServiceError error);

private:
    void setState(State s);
    void teardownController();
    void setupService();
    void enableNotifications();

    State m_state = State::Disconnected;

    QBluetoothDeviceDiscoveryAgent *m_scanner    = nullptr;
    QLowEnergyController           *m_controller = nullptr;
    QLowEnergyService              *m_service    = nullptr;

    QLowEnergyCharacteristic m_writeChar;
    QLowEnergyCharacteristic m_notifyChar;
};
