#pragma once

#include <QObject>
#include <QAudioFormat>

class AudioProcessorBase;

// Abstract base for microphone / line-in audio capture.
//
// Subclasses implement the transport (Qt6 QAudioSource, ASIO, etc.) and call
// emit audioDataReady() whenever a buffer of interleaved PCM samples arrives.
//
// Typical usage:
//   AudioInput *in = new AudioInput(this);
//   in->connectProcessor(myProcessor);
//   in->start();                        // default device
//   in->start("Built-in Microphone");   // named device

class AudioInputBase : public QObject
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

    explicit AudioInputBase(QObject *parent = nullptr);
    ~AudioInputBase() override = default;

    // -----------------------------------------------------------------------
    // Transport control
    // -----------------------------------------------------------------------

    // Start capture on the named device; empty string selects the system default.
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
    // Processor wiring
    // -----------------------------------------------------------------------

    // Connects audioDataReady → processor->processAudio via a queued connection
    // so the processor runs on its own thread if moved there.
    void connectProcessor(AudioProcessorBase *processor);

signals:
    // Emitted for every captured buffer of raw PCM data.
    void audioDataReady(const QByteArray &data, const QAudioFormat &format);

    void stateChanged(AudioInputBase::State state);
    void errorOccurred(const QString &message);

protected:
    State m_state = State::Stopped;
};
