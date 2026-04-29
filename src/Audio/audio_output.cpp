#include "audio_output.h"

#include <QAudioSink>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>

// ---------------------------------------------------------------------------
// AudioPlaybackDevice
//
// A read-only QIODevice wired between our writeAudio() slot and QAudioSink.
// The sink pulls data via readData() on the audio thread; appendData() may
// be called from any thread, so the buffer is guarded by a mutex.
//
// Returning 0 (not -1) from readData() when the buffer is empty tells the
// sink to idle rather than stop, matching the behaviour of QAudioSource
// going IdleState when no data arrives on the capture side.
// ---------------------------------------------------------------------------

class AudioPlaybackDevice : public QIODevice
{
    Q_OBJECT

public:
    explicit AudioPlaybackDevice(QObject *parent = nullptr)
        : QIODevice(parent) {}

    bool open()
    {
        return QIODevice::open(QIODevice::ReadOnly);
    }

    void appendData(const QByteArray &data)
    {
        QMutexLocker lock(&m_mutex);
        m_buffer.append(data);
    }

    qint64 bytesAvailable() const override
    {
        QMutexLocker lock(&m_mutex);
        return static_cast<qint64>(m_buffer.size()) + QIODevice::bytesAvailable();
    }

    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxLen) override
    {
        QMutexLocker lock(&m_mutex);
        const qint64 toRead = qMin(maxLen, static_cast<qint64>(m_buffer.size()));
        if (toRead > 0) {
            memcpy(data, m_buffer.constData(), static_cast<size_t>(toRead));
            m_buffer.remove(0, static_cast<int>(toRead));
        }
        return toRead;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    mutable QMutex m_mutex;
    QByteArray     m_buffer;
};

// AudioPlaybackDevice is defined in this .cpp, so its MOC output must be
// included here for the meta-object system to pick it up.
#include "audio_output.moc"

// ---------------------------------------------------------------------------
// AudioOutput
// ---------------------------------------------------------------------------

AudioOutput::AudioOutput(QObject *parent)
    : AudioOutputBase(parent)
{
}

AudioOutput::~AudioOutput()
{
    stop();
}

QList<QAudioDevice> AudioOutput::availableDevices()
{
    return QMediaDevices::audioOutputs();
}

bool AudioOutput::start(const QString &deviceName)
{
    stop();

    // ---- Select device -------------------------------------------------------
    const QList<QAudioDevice> devices = QMediaDevices::audioOutputs();
    if (devices.isEmpty()) {
        emit errorOccurred(tr("No audio output devices available"));
        return false;
    }

    m_activeDevice = QMediaDevices::defaultAudioOutput();
    if (!deviceName.isEmpty()) {
        for (const QAudioDevice &dev : devices) {
            if (dev.description() == deviceName) {
                m_activeDevice = dev;
                break;
            }
        }
    }

    // ---- Negotiate format ----------------------------------------------------
    QAudioFormat fmt;
    if (m_preferredFormat.isValid() && m_activeDevice.isFormatSupported(m_preferredFormat))
        fmt = m_preferredFormat;
    else
        fmt = m_activeDevice.preferredFormat();

    // ---- Open playback pipeline ----------------------------------------------
    m_playbackDevice = new AudioPlaybackDevice(this);
    if (!m_playbackDevice->open()) {
        emit errorOccurred(tr("Failed to open internal playback device"));
        delete m_playbackDevice;
        m_playbackDevice = nullptr;
        return false;
    }

    m_sink = new QAudioSink(m_activeDevice, fmt, this);
    connect(m_sink, &QAudioSink::stateChanged,
            this, &AudioOutput::onSinkStateChanged);

    m_sink->start(m_playbackDevice);

    if (m_sink->error() != QAudio::NoError) {
        emit errorOccurred(tr("QAudioSink failed to start (error %1)")
                               .arg(static_cast<int>(m_sink->error())));
        stop();
        return false;
    }

    return true;
}

void AudioOutput::stop()
{
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    if (m_playbackDevice) {
        m_playbackDevice->close();
        delete m_playbackDevice;
        m_playbackDevice = nullptr;
    }
    if (m_state != State::Stopped) {
        m_state = State::Stopped;
        emit stateChanged(State::Stopped);
    }
}

void AudioOutput::suspend()
{
    if (m_sink && m_state == State::Active)
        m_sink->suspend();
}

void AudioOutput::resume()
{
    if (m_sink && m_state == State::Suspended)
        m_sink->resume();
}

bool AudioOutput::isActive() const
{
    return m_state == State::Active;
}

QAudioFormat AudioOutput::format() const
{
    return m_sink ? m_sink->format() : m_preferredFormat;
}

void AudioOutput::setPreferredFormat(const QAudioFormat &format)
{
    m_preferredFormat = format;
}

void AudioOutput::setVolume(qreal volume)
{
    if (m_sink)
        m_sink->setVolume(volume);
}

qreal AudioOutput::volume() const
{
    return m_sink ? m_sink->volume() : 1.0;
}

void AudioOutput::writeAudio(const QByteArray &data, const QAudioFormat &)
{
    if (m_playbackDevice)
        m_playbackDevice->appendData(data);
}

void AudioOutput::onSinkStateChanged(QAudio::State qtState)
{
    State next = m_state;

    switch (qtState) {
    case QAudio::ActiveState:
        next = State::Active;
        break;
    case QAudio::SuspendedState:
        next = State::Suspended;
        break;
    case QAudio::StoppedState:
        next = (m_sink && m_sink->error() != QAudio::NoError)
                   ? State::Error
                   : State::Stopped;
        break;
    case QAudio::IdleState:
        return;
    }

    if (next == State::Error)
        emit errorOccurred(tr("Audio sink error %1")
                               .arg(static_cast<int>(m_sink->error())));

    if (m_state != next) {
        m_state = next;
        emit stateChanged(m_state);
    }
}
