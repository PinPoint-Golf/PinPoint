#pragma once

#include "audio_input_base.h"
#include <QAudioDevice>
#include <QAudioFormat>

class QAudioSource;
class AudioCaptureDevice;  // internal QIODevice — defined in audio_input.cpp

// Qt6 Multimedia implementation of AudioInputBase.
//
// Wraps QAudioSource and a custom QIODevice (AudioCaptureDevice) that
// forwards every write into the audioDataReady signal.
//
// Audio format preference:
//   The caller may supply a preferred format via setPreferredFormat() before
//   start().  If the chosen device does not support that format, the device's
//   preferred format is used instead; query format() after start() to see
//   what was actually negotiated.
//
// Device selection:
//   Pass an empty string (default) to use QMediaDevices::defaultAudioInput().
//   Pass a description string to match against QAudioDevice::description().
//   Use AudioInput::availableDevices() to enumerate candidates.
//
// Volume:
//   0.0 = silent, 1.0 = full scale (maps to QAudioSource::setVolume).

class AudioInput : public AudioInputBase
{
    Q_OBJECT

public:
    explicit AudioInput(QObject *parent = nullptr);
    ~AudioInput() override;

    // Returns every available audio input device on this platform.
    static QList<QAudioDevice> availableDevices();

    // -----------------------------------------------------------------------
    // AudioInputBase interface
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

private slots:
    void onSourceStateChanged(QAudio::State qtState);
    void onCapturedData(const QByteArray &data);

private:
    QAudioSource       *m_source        = nullptr;
    AudioCaptureDevice *m_captureDevice = nullptr;
    QAudioFormat        m_preferredFormat;
    QAudioDevice        m_activeDevice;
};
