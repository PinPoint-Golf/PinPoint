#include "audio_input.h"

#include <QAudioSource>
#include <QMediaDevices>

// ---------------------------------------------------------------------------
// AudioCaptureDevice
//
// A write-only QIODevice wired between QAudioSource and our signal.
// QAudioSource::start(device) calls writeData() on whichever thread the
// platform audio backend uses; the signal is connected with AutoConnection
// so cross-thread delivery becomes a queued connection automatically.
// ---------------------------------------------------------------------------

class AudioCaptureDevice : public QIODevice
{
    Q_OBJECT

public:
    explicit AudioCaptureDevice(QObject *parent = nullptr)
        : QIODevice(parent) {}

    bool open()
    {
        return QIODevice::open(QIODevice::WriteOnly);
    }

signals:
    void dataReady(const QByteArray &data);

protected:
    qint64 readData(char *, qint64) override
    {
        return -1;
    }

    qint64 writeData(const char *data, qint64 len) override
    {
        emit dataReady(QByteArray(data, static_cast<int>(len)));
        return len;
    }
};

// AudioCaptureDevice is defined in this .cpp, so its MOC output must be
// included here for the meta-object system to pick it up.
#include "audio_input.moc"

// ---------------------------------------------------------------------------
// AudioInput
// ---------------------------------------------------------------------------

AudioInput::AudioInput(QObject *parent)
    : AudioInputBase(parent)
{
    m_preferredFormat.setChannelCount(1);
    m_preferredFormat.setSampleRate(44100);
    m_preferredFormat.setSampleFormat(QAudioFormat::Int16);
}

AudioInput::~AudioInput()
{
    stop();
}

QList<QAudioDevice> AudioInput::availableDevices()
{
    return QMediaDevices::audioInputs();
}

bool AudioInput::start(const QString &deviceName)
{
    stop();

    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
    if (devices.isEmpty()) {
        emit errorOccurred(tr("No audio input devices available"));
        return false;
    }

    m_activeDevice = QMediaDevices::defaultAudioInput();
    if (!deviceName.isEmpty()) {
        for (const QAudioDevice &dev : devices) {
            if (dev.description() == deviceName) {
                m_activeDevice = dev;
                break;
            }
        }
    }

    QAudioFormat fmt = m_preferredFormat;
    if (!m_activeDevice.isFormatSupported(fmt))
        fmt = m_activeDevice.preferredFormat();

    m_captureDevice = new AudioCaptureDevice(this);
    if (!m_captureDevice->open()) {
        emit errorOccurred(tr("Failed to open internal capture device"));
        delete m_captureDevice;
        m_captureDevice = nullptr;
        return false;
    }
    connect(m_captureDevice, &AudioCaptureDevice::dataReady,
            this, &AudioInput::onCapturedData);

    m_source = new QAudioSource(m_activeDevice, fmt, this);
    connect(m_source, &QAudioSource::stateChanged,
            this, &AudioInput::onSourceStateChanged);

    m_source->start(m_captureDevice);

    if (m_source->error() != QAudio::NoError) {
        emit errorOccurred(tr("QAudioSource failed to start (error %1)")
                               .arg(static_cast<int>(m_source->error())));
        stop();
        return false;
    }

    return true;
}

void AudioInput::stop()
{
    if (m_source) {
        m_source->stop();
        delete m_source;
        m_source = nullptr;
    }
    if (m_captureDevice) {
        m_captureDevice->close();
        delete m_captureDevice;
        m_captureDevice = nullptr;
    }
    if (m_state != State::Stopped) {
        m_state = State::Stopped;
        emit stateChanged(State::Stopped);
    }
}

void AudioInput::suspend()
{
    if (m_source && m_state == State::Active)
        m_source->suspend();
}

void AudioInput::resume()
{
    if (m_source && m_state == State::Suspended)
        m_source->resume();
}

bool AudioInput::isActive() const
{
    return m_state == State::Active;
}

QAudioFormat AudioInput::format() const
{
    return m_source ? m_source->format() : m_preferredFormat;
}

void AudioInput::setPreferredFormat(const QAudioFormat &format)
{
    m_preferredFormat = format;
}

void AudioInput::setVolume(qreal volume)
{
    if (m_source)
        m_source->setVolume(volume);
}

qreal AudioInput::volume() const
{
    return m_source ? m_source->volume() : 1.0;
}

void AudioInput::onSourceStateChanged(QAudio::State qtState)
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
        next = (m_source && m_source->error() != QAudio::NoError)
                   ? State::Error
                   : State::Stopped;
        break;
    case QAudio::IdleState:
        // Running but no data yet — not a state change worth surfacing.
        return;
    }

    if (next == State::Error)
        emit errorOccurred(tr("Audio source error %1")
                               .arg(static_cast<int>(m_source->error())));

    if (m_state != next) {
        m_state = next;
        emit stateChanged(m_state);
    }
}

void AudioInput::onCapturedData(const QByteArray &data)
{
    emit audioDataReady(data, m_source->format());
}
