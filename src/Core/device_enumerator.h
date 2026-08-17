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

#include <QString>
#include <QList>
#include <QObject>
#include <QVariant>
#include <QThread>
#include "../Video/camera_capabilities.h"
#include "../Video/video_input_factory.h"
#include "imu_base.h"
#include "imu_capabilities.h"

enum class DeviceType {
    VideoInput,
    AudioInput,
    AudioOutput,
    Imu
};

// Distinguishes the physical vendor of an IMU device. imuTransport alone
// cannot do this: both WitMotion (WT901) and HackMotion (wG3) devices are
// discovered over Ble, so transport is the same for either. WitMotion is the
// default so every existing construction site — none of which sets this
// field — keeps meaning "WitMotion", its historical (only) meaning.
enum class ImuVendor {
    WitMotion,
    HackMotion
};

struct Device {
    DeviceType type = DeviceType::VideoInput;

    // --- Camera / audio fields (valid when type == VideoInput/AudioInput/AudioOutput) ---
    VideoInputFactory::Backend backend = VideoInputFactory::Backend::Auto;
    CameraCapabilities capabilities;

    // --- IMU fields (valid when type == Imu) ---
    ImuBase::Transport imuTransport    = ImuBase::Transport::Serial;
    ImuVendor          imuVendor       = ImuVendor::WitMotion;
    ImuCapabilities    imuCapabilities;
    // Opaque transport handle. For BLE: stores QBluetoothDeviceInfo via QVariant::fromValue().
    // Callers that need it: #include <QBluetoothDeviceInfo> and use .value<QBluetoothDeviceInfo>().
    QVariant           platformHandle;

    // --- Common ---
    QString id;           // camera: device id; IMU BLE: MAC address or UUID; serial: port name
    QString description;  // human-readable label

    // IMU only: the scan generation in which this device was last discovered
    // (see DeviceEnumerator::scanImu). Lets a consumer tell whether the device
    // appeared in the most recently completed scan; -1 = never seen.
    int lastSeenScanGeneration = -1;
};

class DeviceEnumerator : public QObject
{
    Q_OBJECT

public:
    static DeviceEnumerator* instance();

    // Synchronous enumeration — individual backends register themselves on demand.
    void enumerate();

    // Starts an asynchronous BLE IMU scan (90 s timeout) in a worker thread.
    // Serial scan is a stub that finds nothing (TODO: probe serial ports).
    // Safe to call multiple times; ignored if a scan is already in progress.
    void scanImu();
    bool isImuScanActive() const { return m_imuScanActive; }

    // Generation of the most recently COMPLETED IMU scan (0 before the first
    // scan finishes). A device is "current" if its lastSeenScanGeneration is
    // >= this value; a lower value means it was absent from the last scan.
    int completedImuScanGeneration() const { return m_imuScanGenerationCompleted; }

    // Returns all registered devices, optionally filtered by type.
    QList<Device> devices() const;
    QList<Device> devices(DeviceType type) const;

    // Register a camera / audio device (used by video/audio backends).
    void registerDevice(DeviceType type, VideoInputFactory::Backend backend,
                        const QString &id, const QString &description,
                        const CameraCapabilities &capabilities = {});

    // Register an IMU device (used by scanImu() and future IMU backends).
    void registerImuDevice(ImuBase::Transport transport,
                           const QString &id,
                           const QString &description,
                           const ImuCapabilities &capabilities = {},
                           const QVariant &platformHandle = {},
                           ImuVendor vendor = ImuVendor::WitMotion);

signals:
    // Emitted whenever a new device is added (any type).
    void deviceAdded(const Device &device);

    // Emitted when the asynchronous IMU BLE scan finishes (timeout or explicit stop).
    void imuScanFinished();

    // Emitted when an IMU BLE discovery agent reports an error (e.g. Bluetooth
    // powered off, no adapter, permission denied). Lets the IMU UI distinguish a
    // genuine "no devices in range" from "discovery couldn't run".
    void imuScanError(const QString &message);

private:
    explicit DeviceEnumerator(QObject *parent = nullptr);

    QList<Device> m_devices;
    bool m_videoEnumerated      = false;
    bool m_audioInputEnumerated = false;
    bool m_audioOutputEnumerated = false;
    bool m_imuScanActive        = false;
    QThread *m_imuScanThread    = nullptr;

    // IMU scan generation. Incremented at the START of each scan and stamped onto
    // every device registered during it; the value of the just-finished scan is
    // copied into m_imuScanGenerationCompleted when the scan completes. Together
    // these let consumers prune devices absent from the latest completed scan.
    int m_imuScanGeneration          = 0;
    int m_imuScanGenerationCompleted = 0;
};
