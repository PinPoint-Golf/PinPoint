#include "wt9011dcl.h"

WT9011DCL::WT9011DCL(QObject *parent)
    : WT9011DCL_Base(parent)
    , m_serial(new QSerialPort(this))
{
    connect(m_serial, &QSerialPort::readyRead,
            this,     &WT9011DCL::onReadyRead);
    connect(m_serial, &QSerialPort::errorOccurred,
            this,     &WT9011DCL::onSerialError);
}

WT9011DCL::~WT9011DCL()
{
    close();
}

bool WT9011DCL::open(const QString &portName, qint32 baudRate)
{
    if (m_serial->isOpen())
        m_serial->close();

    m_serial->setPortName(portName);
    m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        emit errorOccurred(m_serial->errorString());
        return false;
    }

    emit connected();
    return true;
}

void WT9011DCL::close()
{
    if (m_serial->isOpen()) {
        m_serial->close();
        emit disconnected();
    }
}

bool WT9011DCL::isOpen() const
{
    return m_serial->isOpen();
}

QString WT9011DCL::portName() const
{
    return m_serial->portName();
}

void WT9011DCL::writeToDevice(const QByteArray &data)
{
    m_serial->write(data);
}

void WT9011DCL::onReadyRead()
{
    receiveData(m_serial->readAll());
}

void WT9011DCL::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;

    emit errorOccurred(m_serial->errorString());

    if (error == QSerialPort::ResourceError)
        close();
}
