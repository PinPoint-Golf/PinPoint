#pragma once

#include <QObject>
#include <QAudioFormat>

class AudioInputBase;

// Abstract base for speaker / line-out audio playback.
//
// Subclasses implement the transport (Qt6 QAudioSink, ASIO, etc.) and consume
// data delivered via the writeAudio() slot.
//
// Typical usage:
//   AudioOutput *out = new AudioOutput(this);
//   out->connectSource(audioInput);      // wire an input directly
//   out->start();                        // default device
//   out->writeAudio(data, format);       // or push PCM data manually

class AudioOutputBase : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Active,
        Suspended,
        Error,
    };
    Q_ENUM(State)

    explicit AudioOutputBase(QObject *parent = nullptr);
    ~AudioOutputBase() override = default;

    // -----------------------------------------------------------------------
    // Transport control
    // -----------------------------------------------------------------------

    // Start playback on the named device; empty string selects the system default.
    virtual bool start(const QString &deviceName = {}) = 0;
    virtual void stop() = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;

    // -----------------------------------------------------------------------
    // Status
    // -----------------------------------------------------------------------

    virtual bool         isActive() const = 0;
    virtual QAudioFormat format()   const = 0;
    virtual State        state()    const;

    // -----------------------------------------------------------------------
    // Source wiring
    // -----------------------------------------------------------------------

    // Connects source->audioDataReady → writeAudio via a queued connection
    // so delivery is thread-safe when the source lives on a different thread.
    void connectSource(AudioInputBase *source);

public slots:
    // Feed raw PCM data into the playback pipeline.
    // Data must be in the format negotiated by start(); query format() to confirm.
    virtual void writeAudio(const QByteArray &data, const QAudioFormat &format) = 0;

signals:
    void stateChanged(AudioOutputBase::State state);
    void errorOccurred(const QString &message);

protected:
    State m_state = State::Stopped;
};
