#include "audio_stream_saver.h"

#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>

AudioStreamSaver::AudioStreamSaver(QObject *parent)
    : QObject(parent)
{}

AudioStreamSaver::~AudioStreamSaver()
{
    stopSaving();
}

void AudioStreamSaver::stopSaving()
{
    if (!m_file.isOpen())
        return;

    finaliseWav();
    m_file.close();
}

void AudioStreamSaver::onAudioData(const QByteArray &data, const QAudioFormat &format)
{
    if (!m_file.isOpen())
        openFile(format);
    if (!m_file.isOpen())
        return;

    m_file.write(data);
}

void AudioStreamSaver::openFile(const QAudioFormat &format)
{
    m_format = format;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();

    const QString ts   = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString path = QDir(dir).filePath(QStringLiteral("pinpoint_audio_%1.wav").arg(ts));

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly)) {
        qWarning() << "[AudioStreamSaver] Cannot open file for writing:" << path;
        return;
    }

    // Reserve 44 bytes for the WAV header; real values written in finaliseWav().
    m_file.write(QByteArray(44, '\0'));
}

void AudioStreamSaver::finaliseWav()
{
    const qint64 dataBytes = m_file.size() - 44;
    if (dataBytes <= 0)
        return;

    const quint32 sampleRate    = static_cast<quint32>(m_format.sampleRate());
    const quint16 channels      = static_cast<quint16>(m_format.channelCount());
    const quint16 bitsPerSample = static_cast<quint16>(m_format.bytesPerSample() * 8);
    const quint32 byteRate      = sampleRate * channels * m_format.bytesPerSample();
    const quint16 blockAlign    = static_cast<quint16>(channels * m_format.bytesPerSample());
    const quint16 audioFormat   = (m_format.sampleFormat() == QAudioFormat::Float) ? 3 : 1;
    const quint32 chunkSize     = static_cast<quint32>(qMin(dataBytes, qint64(0xFFFFFFFFLL)));

    m_file.seek(0);
    QDataStream ds(&m_file);
    ds.setByteOrder(QDataStream::LittleEndian);

    ds.writeRawData("RIFF", 4);
    ds << static_cast<quint32>(36 + chunkSize);
    ds.writeRawData("WAVE", 4);

    ds.writeRawData("fmt ", 4);
    ds << quint32(16);
    ds << audioFormat;
    ds << channels;
    ds << sampleRate;
    ds << byteRate;
    ds << blockAlign;
    ds << bitsPerSample;

    ds.writeRawData("data", 4);
    ds << chunkSize;
}
