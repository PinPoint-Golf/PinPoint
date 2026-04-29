#pragma once

#include "wt9011dcl_base.h"
#include <QSerialPort>

// WT9011DCL driver — UART/serial transport.
//
// Default settings: 115200 baud, 8N1.
//
// Usage:
//   WT9011DCL imu;
//   connect(&imu, &WT9011DCL::eulerAnglesUpdated, ...);
//   imu.open("/dev/ttyUSB0");

class WT9011DCL : public WT9011DCL_Base
{
    Q_OBJECT

public:
    explicit WT9011DCL(QObject *parent = nullptr);
    ~WT9011DCL() override;

    bool    open(const QString &portName, qint32 baudRate = 115200);
    void    close();
    bool    isOpen() const;
    QString portName() const;

protected:
    void writeToDevice(const QByteArray &data) override;

private slots:
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);

private:
    QSerialPort *m_serial;
};
