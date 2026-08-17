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
#include <QBluetoothAddress>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QElapsedTimer>
#include <QTimer>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QLowEnergyCharacteristic>

// Generic BLE connection state machine — device-independent.
//
// Handles scanning, GATT service discovery, characteristic setup, and the
// Linux 6.x BlueZ scan-before-connect requirement.  Does not know about any
// specific IMU protocol; data bytes arrive via dataReceived() and are written
// back via writeToDevice().
//
// UUID discovery comes in two forms, chosen per device by UuidConfig: short
// fragment strings that are matched case-insensitively and substituted into the
// service's own base (Witmotion), or full 128-bit UUIDs matched exactly
// (HackMotion wG3, whose data characteristic does not share the service's base).
// See UuidConfig for why the second form exists and why its match is exact.
//
// Ownership: create one per BLE connection.  The owning class wires up its
// signals:
//
//   connect(transport, &BleImuTransport::gattReady,    owner, [owner]{ owner->initDevice(); });
//   connect(transport, &BleImuTransport::dataReceived, owner, [owner](const QByteArray &d){ owner->receiveData(d); });
//   transport->connectToDevice(deviceInfo);

class BleImuTransport : public QObject
{
    Q_OBJECT

public:
    // How the GATT service and its two characteristics are located on the device.
    // A device uses ONE of the two forms below; explicit wins when present.
    //
    // FRAGMENT FORM (Witmotion WT901 and relatives).  The notify and write
    // characteristics live under the SAME 128-bit base as the service, so a short
    // hex fragment ("ffe5") finds the service by substring match and the
    // characteristic UUIDs are derived by substituting the notify/write fragment
    // into the base of whichever service actually matched.  Deriving rather than
    // hard-coding is what absorbs firmware revisions that ship a non-standard base
    // suffix — the fragments are stable across them, the full UUIDs are not.
    //
    // EXPLICIT FORM (HackMotion wG3).  The data characteristic does NOT sit under
    // the service's base — service 49535343-fe7d-4ae5-8fa9-9fafd205e455, data
    // 413c3893-b7e8-4231-9673-7af7aed06ddc — so no substitution maps one to the
    // other and the fragment form simply cannot express the device.  Supply the
    // full UUIDs instead; they are matched by EQUALITY, never by substring.
    //
    // ⚠ The exact match is load-bearing, not stylistic.  The wG3 also exposes the
    // stock ISSC pipe characteristic 49535343-1e4d-4bd9-ba61-23c647249616, which
    // shares the transparent-UART service's 49535343 base.  It accepts writes and
    // never replies, so anything looser than equality can resolve it instead and
    // hand back a link that looks connected and is silently dead.
    //
    // New members go AFTER the three fragments on purpose: the only aggregate
    // initialiser in the tree is WT9011DCL_BLE's BleImuTransport({"ffe5","ffe4",
    // "ffe9"}, this), and it must keep compiling untouched.
    struct UuidConfig {
        // Fragment form.
        QString serviceFragment;   // short hex ID embedded in the 128-bit service UUID (e.g. "ffe5")
        QString notifyFragment;    // short hex ID for the notify characteristic        (e.g. "ffe4")
        QString writeFragment;     // short hex ID for the write characteristic         (e.g. "ffe9")

        // Explicit form.  Null (the default) means "use the fragments above".
        QBluetoothUuid serviceUuid;
        QBluetoothUuid notifyUuid;
        // Leave null when notify and write are the SAME bidirectional characteristic.
        // The wG3 runs its entire protocol — commands out, stream and replies in —
        // through one characteristic; a null write resolves onto notifyUuid.
        QBluetoothUuid writeUuid;

        // Minimum negotiated ATT MTU this device needs to function, or 0 for
        // "don't care" (the Witmotion default — its frames fit the 20-byte payload
        // of the default 23-byte MTU).  The wG3's calibration reply is 65 bytes and
        // its stream notifications reach 93, so libhackmotion refuses to run below
        // HM_MIN_ATT_MTU (96).  Nothing in Qt lets an application REQUEST an MTU on
        // any platform, so this is a check that produces a diagnosis, never a knob.
        int minAttMtu = 0;

        // True when the explicit UUIDs are in play.  writeUuid is deliberately not
        // consulted — a null write is the legitimate single-characteristic case.
        bool usesExplicitUuids() const
        { return !serviceUuid.isNull() && !notifyUuid.isNull(); }
    };

    // Builder for the explicit form, so call sites read as UUIDs rather than as
    // three empty strings followed by designated initialisers.  Pass a null
    // `write` (the default) when one bidirectional characteristic carries both
    // directions.
    static UuidConfig explicitUuids(const QBluetoothUuid &service,
                                    const QBluetoothUuid &notify,
                                    const QBluetoothUuid &write = QBluetoothUuid(),
                                    int minAttMtu = 0);

    enum class State {
        Disconnected,
        Scanning,
        Connecting,
        DiscoveringServices,
        Ready,
        Error,
    };
    Q_ENUM(State)

    explicit BleImuTransport(const UuidConfig &uuids, QObject *parent = nullptr);
    ~BleImuTransport() override;

    State state()   const { return m_state; }
    bool  isReady() const { return m_state == State::Ready; }

    // Last ATT MTU the platform reported for the current link, in bytes, or -1 if
    // no link is up or this platform never reported one.  Sourced from
    // QLowEnergyController::mtu() and refreshed from its mtuChanged() signal.
    // Reset to -1 by teardown, so it always describes the CURRENT attempt.
    int mtu() const { return m_mtu; }

    // localAdapter: the HCI adapter to use for scanning and GATT connection.
    // Pass QBluetoothAddress() (null) to use the system default (single-adapter path).
    void connectToDevice(const QBluetoothDeviceInfo &device,
                         const QBluetoothAddress &localAdapter = QBluetoothAddress());
    void disconnectFromDevice();

    // Write raw bytes to the connected device.  Safe to call only when isReady().
    // The write mode is chosen from the characteristic's own properties.
    void writeToDevice(const QByteArray &data);

    // As above, but the CALLER states whether it wants an acknowledgement.
    //
    // Exists because a protocol can care per write.  libhackmotion's
    // hm_write_request carries `without_response` and sets it per command — the
    // wG3's data characteristic advertises Write and WriteNoResponse both, so the
    // properties alone cannot express the choice and the single-argument overload
    // above would silently answer it the same way every time.
    //
    // The preference is honoured only where the characteristic can actually
    // satisfy it; a device offering just one mode gets that one, since a write in
    // the wrong mode is refused outright by the stack.
    void writeToDevice(const QByteArray &data, bool withoutResponse);

signals:
    void stateChanged(BleImuTransport::State state);

    // Emitted when CCCD descriptor has been written and the device is ready to
    // receive commands.  The owning class should perform device initialisation
    // in response to this signal.
    void gattReady();

    // Emitted when a notify characteristic value arrives from the device.
    // Connect to the protocol parser's receiveData() equivalent.
    void dataReceived(const QByteArray &data);

    void diagnosticInfo(const QString &message);
    void errorOccurred(const QString &message);

    // The negotiated ATT MTU changed (including the first value seen on link-up).
    void mtuChanged(int mtu);

    // The link came up and GATT resolved, but the negotiated MTU is below the
    // device's UuidConfig::minAttMtu, so its frames would arrive truncated.
    //
    // Distinct from errorOccurred() on purpose. This is not "the connection
    // failed" — everything up to here succeeded, and nothing the application can
    // do will fix it (no Qt platform exposes an MTU request), so a UI that treats
    // it as a transient connect error will retry forever against a wall. A
    // matching errorOccurred() still follows via failConnection(), carrying both
    // numbers in its text for logs that only capture the message.
    void mtuTooSmall(int negotiated, int required);

    // Emitted after gattReady() + setState(Ready).  Owning class can forward
    // to ImuBase::connected() if desired.
    void connected();
    void disconnected();

private slots:
    void onDeviceDiscovered(const QBluetoothDeviceInfo &device);
    void onScanFinished();
    void onScanError(QBluetoothDeviceDiscoveryAgent::Error error);

    void onControllerConnected();
    void onControllerDisconnected();
    void onServiceDiscovered(const QBluetoothUuid &uuid);
    void onServiceDiscoveryFinished();
    void onControllerError(QLowEnergyController::Error error);
    void onMtuChanged(int mtu);

    void onServiceStateChanged(QLowEnergyService::ServiceState newState);
    void onCharacteristicChanged(const QLowEnergyCharacteristic &c,
                                 const QByteArray &value);
    void onServiceError(QLowEnergyService::ServiceError error);

private:
    void setState(State s);
    void teardownController();
    // Single failure path for every connect/discovery error: tears the
    // controller/service down, drops any connect-phase scan, transitions to
    // Error and emits errorOccurred(). Leaves the transport in a clean state so
    // the owner's retry starts from scratch (no leaked HCI link / armed lambda).
    void failConnection(const QString &message);
    void setupService();
    void enableNotifications();
    // Gate on UuidConfig::minAttMtu. Returns false (and has already failed the
    // connection) when the negotiated MTU is too small for the device.
    bool checkMtuRequirement();
    void ensureScannerCreated();
    // Stops the discovery agent if active. Internal-only — used by teardown,
    // failConnection, the non-Linux connect path, and once the HCI link is up.
    void stopScan();
    void doConnect();
    bool matchesPendingDevice(const QBluetoothDeviceInfo &d) const;

    static constexpr int kConnectScanMs = 10'000;
    // Watchdog covering the post-scan connect→service-discovery→CCCD-write phase.
    // BlueZ/WinRT can wedge mid-discovery (esp. the CCCD write) without ever
    // emitting errorOccurred(), leaving the UI stuck in "Connecting…"/"Discovering
    // services…" forever. On expiry we fail the attempt so the owner's retry runs.
    static constexpr int kConnectWatchdogMs = 20'000;

    UuidConfig m_uuids;
    State      m_state = State::Disconnected;

    QBluetoothDeviceDiscoveryAgent *m_scanner    = nullptr;
    QLowEnergyController           *m_controller = nullptr;
    QLowEnergyService              *m_service    = nullptr;

    QLowEnergyCharacteristic m_writeChar;
    QLowEnergyCharacteristic m_notifyChar;
    QBluetoothUuid           m_resolvedServiceUuid;
    QBluetoothUuid           m_resolvedNotifyUuid;
    QBluetoothUuid           m_resolvedWriteUuid;
    QElapsedTimer            m_connectTimer;
    int                      m_mtu = -1;          // last MTU the platform reported; -1 = none yet
    QTimer                   m_connectWatchdog;   // fires if connect/discovery never reaches Ready

    QBluetoothDeviceInfo m_pendingDevice;
    QBluetoothAddress    m_localAdapter;
    bool                 m_waitingForScanConfirm = false;
};
