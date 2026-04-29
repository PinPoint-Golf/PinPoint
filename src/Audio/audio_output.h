#pragma once

#include "audio_output_base.h"
#include <QAudioDevice>
#include <QAudioFormat>

class QAudioSink;
class AudioPlaybackDevice;  // internal QIODevice — defined in audio_output.cpp

// Qt6 Multimedia implementation of AudioOutputBase.
//
// Wraps QAudioSink and a custom QIODevice (AudioPlaybackDevice) that buffers
// incoming PCM data and serves it to the sink on demand.
//
// Audio format preference:
//   The caller may supply a preferred format via setPreferredFormat() before
//   start().  If the chosen device does not support that format, the device's
//   preferred format is used instead; query format() after start() to see
//   what was actually negotiated.  Data passed to writeAudio() must match
//   the negotiated format.
//
// Device selection:
//   Pass an empty string (default) to use QMediaDevices::defaultAudioOutput().
//   Pass a description string to match against QAudioDevice::description().
//   Use AudioOutput::availableDevices() to enumerate candidates.
//
// Volume:
//   0.0 = silent, 1.0 = full scale (maps to QAudioSink::setVolume).

class AudioOutput : public AudioOutputBase
{
    Q_OBJECT

public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput() override;

    // Returns every available audio output device on this platform.
    static QList<QAudioDevice> availableDevices();

    // -----------------------------------------------------------------------
    // AudioOutputBase interface
    // -----------------------------------------------------------------------

    bool         start(const QString &deviceName = {}) override;
    void         stop()    override;
    void         suspend() override;
    void         resume()  override;
    bool         isActive() const override;
    QAudioFormat format()   const override;

    // -----------------------------------------------------------------------
    // Configuration (call before start())
    // -----------------------------------------------------------------------

    void setPreferredFormat(const QAudioFormat &format);

    // -----------------------------------------------------------------------
    // Runtime control
    // -----------------------------------------------------------------------

    void  setVolume(qreal volume);   // 0.0–1.0
    qreal volume() const;

public slots:
    void writeAudio(const QByteArray &data, const QAudioFormat &format) override;

private slots:
    void onSinkStateChanged(QAudio::State qtState);

private:
    QAudioSink          *m_sink           = nullptr;
    AudioPlaybackDevice *m_playbackDevice = nullptr;
    QAudioFormat         m_preferredFormat;
    QAudioDevice         m_activeDevice;
};
