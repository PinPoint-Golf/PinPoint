#pragma once

#include <QObject>
#include <QByteArray>

// Abstract base class for WT9011DCL (MPU9250) IMU drivers.
//
// Packet protocol (transport-agnostic):
//   [0]     0x55  (header)
//   [1]     type  (PacketType)
//   [2..9]  four signed int16 payload words, little-endian
//   [10]    checksum = (sum of bytes [0..9]) & 0xFF
//
// Config/read commands sent to the device:
//   FF AA [reg] [lo] [hi]          — write register
//   FF AA 27   [reg] [count]       — read register(s)
//
// Subclasses implement writeToDevice() to send bytes over their transport,
// and call receiveData() whenever bytes arrive from the device.

class WT9011DCL_Base : public QObject
{
    Q_OBJECT

public:
    // -----------------------------------------------------------------------
    // Data structures
    // -----------------------------------------------------------------------

    struct AccelData {
        float x = 0, y = 0, z = 0;   // g-force
        float temperature = 0;         // °C
    };

    struct GyroData {
        float x = 0, y = 0, z = 0;   // °/s
        float temperature = 0;         // °C
    };

    struct EulerAngles {
        float roll = 0, pitch = 0, yaw = 0;  // degrees
    };

    struct MagData {
        float x = 0, y = 0, z = 0;   // raw ADC (÷120 ≈ μT)
        float temperature = 0;         // °C
    };

    struct QuaternionData {
        float w = 1, x = 0, y = 0, z = 0;  // unit quaternion
    };

    // -----------------------------------------------------------------------
    // Configuration enumerations
    // -----------------------------------------------------------------------

    enum class OutputRate : quint8 {
        Hz_0_1  = 0x01,
        Hz_0_5  = 0x02,
        Hz_1    = 0x03,
        Hz_2    = 0x04,
        Hz_5    = 0x05,
        Hz_10   = 0x06,
        Hz_20   = 0x07,
        Hz_50   = 0x08,
        Hz_100  = 0x09,
        Hz_200  = 0x0A,
        Off     = 0x0B,
    };

    enum class BaudRate : quint8 {
        Baud_4800   = 0x00,
        Baud_9600   = 0x01,
        Baud_19200  = 0x02,
        Baud_38400  = 0x03,
        Baud_57600  = 0x04,
        Baud_115200 = 0x05,
        Baud_230400 = 0x06,
        Baud_460800 = 0x07,
        Baud_921600 = 0x08,
    };

    enum OutputFlag : quint16 {
        OutputTime        = 0x0001,
        OutputAccel       = 0x0002,
        OutputGyro        = 0x0004,
        OutputAngle       = 0x0008,
        OutputMag         = 0x0010,
        OutputPortStatus  = 0x0020,
        OutputBarometric  = 0x0040,
        OutputGPS         = 0x0080,
        OutputGroundSpeed = 0x0100,
        OutputQuaternion  = 0x0200,
        OutputGPSAccuracy = 0x0400,
    };
    Q_DECLARE_FLAGS(OutputFlags, OutputFlag)

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    explicit WT9011DCL_Base(QObject *parent = nullptr);
    ~WT9011DCL_Base() override = default;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    void saveConfiguration();
    void setOutputRate(OutputRate rate);
    void setDeviceBaudRate(BaudRate rate);
    void setOutputData(OutputFlags flags);

    // -----------------------------------------------------------------------
    // Calibration
    // -----------------------------------------------------------------------

    // Keep device flat and still; call endCalibration() after ~5 s.
    void startAccelGyroCalibration();

    // Rotate through all orientations for ~30 s; call endCalibration() when done.
    void startMagCalibration();

    void endCalibration();
    void resetAltitude();

    // -----------------------------------------------------------------------
    // On-demand register read
    // -----------------------------------------------------------------------

    void readRegisters(quint8 regAddr, quint8 regCount = 1);

    // -----------------------------------------------------------------------
    // Latest cached values
    // -----------------------------------------------------------------------

    AccelData      accelData()      const { return m_accel; }
    GyroData       gyroData()       const { return m_gyro;  }
    EulerAngles    eulerAngles()    const { return m_euler; }
    MagData        magData()        const { return m_mag;   }
    QuaternionData quaternionData() const { return m_quat;  }

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);

    void accelUpdated(const WT9011DCL_Base::AccelData &data);
    void gyroUpdated(const WT9011DCL_Base::GyroData &data);
    void eulerAnglesUpdated(const WT9011DCL_Base::EulerAngles &angles);
    void magUpdated(const WT9011DCL_Base::MagData &data);
    void quaternionUpdated(const WT9011DCL_Base::QuaternionData &quat);

protected:
    // Subclasses call this whenever bytes arrive from the device.
    void receiveData(const QByteArray &data);

    // Subclasses implement this to write bytes to their transport.
    virtual void writeToDevice(const QByteArray &data) = 0;

    // Shared command builder — calls writeToDevice().
    void sendCommand(quint8 reg, quint16 value);

private:
    enum PacketType : quint8 {
        PktTime        = 0x50,
        PktAccel       = 0x51,
        PktGyro        = 0x52,
        PktAngle       = 0x53,
        PktMag         = 0x54,
        PktPortStatus  = 0x55,
        PktBarometric  = 0x56,
        PktGPS         = 0x57,
        PktGroundSpeed = 0x58,
        PktQuaternion  = 0x59,
        PktGPSAccuracy = 0x5A,
    };

    enum Register : quint8 {
        RegSave     = 0x00,
        RegCalSw    = 0x01,
        RegRSW      = 0x02,
        RegRRate    = 0x03,
        RegBaud     = 0x04,
        RegAxOffset = 0x05,
        RegAyOffset = 0x06,
        RegAzOffset = 0x07,
        RegGxOffset = 0x08,
        RegGyOffset = 0x09,
        RegGzOffset = 0x0A,
        RegHxOffset = 0x0B,
        RegHyOffset = 0x0C,
        RegHzOffset = 0x0D,
    };

    void processBuffer();
    bool verifyChecksum(const QByteArray &packet) const;
    void dispatchPacket(const QByteArray &packet);

    void parseAccel(const QByteArray &d);
    void parseGyro(const QByteArray &d);
    void parseEuler(const QByteArray &d);
    void parseMag(const QByteArray &d);
    void parseQuaternion(const QByteArray &d);

    static qint16 le16(const QByteArray &d, int offset);
    static float  toTemperature(qint16 raw);

    static constexpr quint8 FrameHeader = 0x55;
    static constexpr int    FrameSize   = 11;

    QByteArray     m_buffer;
    AccelData      m_accel;
    GyroData       m_gyro;
    EulerAngles    m_euler;
    MagData        m_mag;
    QuaternionData m_quat;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(WT9011DCL_Base::OutputFlags)
