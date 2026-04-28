#pragma once

#include <QAudioFormat>
#include <QFile>
#include <QObject>

// Saves incoming PCM audio buffers to a timestamped WAV file on disk.
//
// Connect AudioInputBase::audioDataReady to onAudioData(). The file is created
// on the first buffer received and the WAV header is written on stopSaving()
// (or destruction).  The WAV format (sample rate, channels, bit depth) is
// determined from the QAudioFormat carried by the first buffer.
//
// File location: Desktop (falls back to home directory).
// File name:     pinpoint_audio_<yyyyMMdd_HHmmss>.wav
class AudioStreamSaver : public QObject
{
    Q_OBJECT

public:
    explicit AudioStreamSaver(QObject *parent = nullptr);
    ~AudioStreamSaver() override;

    void    stopSaving();
    bool    isSaving()  const { return m_file.isOpen(); }
    QString filePath()  const { return m_file.fileName(); }

public slots:
    void onAudioData(const QByteArray &data, const QAudioFormat &format);

private:
    void openFile(const QAudioFormat &format);
    void finaliseWav();

    QFile        m_file;
    QAudioFormat m_format;
};
