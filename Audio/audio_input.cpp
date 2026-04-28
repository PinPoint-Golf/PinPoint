#include "audio_input.h"

#include <QAudioSource>
#include <QDebug>
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
    m_preferredFormat.setSampleRate(16000);
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

static QString sampleFormatName(QAudioFormat::SampleFormat f)
{
    switch (f) {
    case QAudioFormat::UInt8:  return QStringLiteral("UInt8");
    case QAudioFormat::Int16:  return QStringLiteral("Int16");
    case QAudioFormat::Int32:  return QStringLiteral("Int32");
    case QAudioFormat::Float:  return QStringLiteral("Float");
    default:                   return QStringLiteral("Unknown");
    }
}

bool AudioInput::start(const QString &deviceName)
{
    stop();

    // ---- Enumerate devices ------------------------------------------------
    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
    const QAudioDevice defaultDevice  = QMediaDevices::defaultAudioInput();

    qDebug() << "[AudioInput] Available input devices:" << devices.size();
    for (const QAudioDevice &dev : devices) {
        const QAudioFormat pref = dev.preferredFormat();
        qDebug() << "  " << (dev.id() == defaultDevice.id() ? "*" : " ")
                 << dev.description()
                 << "| id:" << dev.id()
                 << "| preferred:" << pref.sampleRate() << "Hz"
                 << pref.channelCount() << "ch"
                 << sampleFormatName(pref.sampleFormat())
                 << "| sampleRate range:" << dev.minimumSampleRate()
                 << "-" << dev.maximumSampleRate()
                 << "| channels range:" << dev.minimumChannelCount()
                 << "-" << dev.maximumChannelCount();
    }

    if (devices.isEmpty()) {
        qDebug() << "[AudioInput] ERROR: no audio input devices found";
        emit errorOccurred(tr("No audio input devices available"));
        return false;
    }

    // ---- Select device ----------------------------------------------------
    m_activeDevice = defaultDevice;
    if (!deviceName.isEmpty()) {
        bool found = false;
        for (const QAudioDevice &dev : devices) {
            if (dev.description() == deviceName) {
                m_activeDevice = dev;
                found = true;
                break;
            }
        }
        if (!found)
            qDebug() << "[AudioInput] WARNING: requested device" << deviceName
                     << "not found, falling back to default";
    }
    qDebug() << "[AudioInput] Selected device:" << m_activeDevice.description()
             << "(id:" << m_activeDevice.id() << ")";

    // ---- Negotiate format -------------------------------------------------
    QAudioFormat fmt = m_preferredFormat;
    const bool preferred = m_activeDevice.isFormatSupported(fmt);
    if (!preferred) {
        fmt = m_activeDevice.preferredFormat();
        qDebug() << "[AudioInput] Preferred format not supported — using device default";
    }
    qDebug() << "[AudioInput] Requested format:"
             << m_preferredFormat.sampleRate() << "Hz,"
             << m_preferredFormat.channelCount() << "ch,"
             << sampleFormatName(m_preferredFormat.sampleFormat())
             << (preferred ? "  [supported]" : "  [not supported]");
    qDebug() << "[AudioInput] Negotiated format:"
             << fmt.sampleRate() << "Hz,"
             << fmt.channelCount() << "ch,"
             << sampleFormatName(fmt.sampleFormat());

    // ---- Open capture pipeline --------------------------------------------
    m_captureDevice = new AudioCaptureDevice(this);
    if (!m_captureDevice->open()) {
        qDebug() << "[AudioInput] ERROR: failed to open internal capture device";
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

    qDebug() << "[AudioInput] Starting QAudioSource (buffer size:" << m_source->bufferSize() << "bytes)";
    m_source->start(m_captureDevice);

    if (m_source->error() != QAudio::NoError) {
        qDebug() << "[AudioInput] ERROR: QAudioSource failed to start, error code:"
                 << static_cast<int>(m_source->error());
        emit errorOccurred(tr("QAudioSource failed to start (error %1)")
                               .arg(static_cast<int>(m_source->error())));
        stop();
        return false;
    }

    qDebug() << "[AudioInput] Capture started — buffer size after open:"
             << m_source->bufferSize() << "bytes";
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
    static const auto stateName = [](QAudio::State s) -> const char * {
        switch (s) {
        case QAudio::ActiveState:    return "Active";
        case QAudio::SuspendedState: return "Suspended";
        case QAudio::StoppedState:   return "Stopped";
        case QAudio::IdleState:      return "Idle";
        default:                     return "Unknown";
        }
    };

    qDebug() << "[AudioInput] QAudioSource state ->" << stateName(qtState)
             << "| error:" << (m_source ? static_cast<int>(m_source->error()) : -1);

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
        return;
    }

    if (next == State::Error) {
        qDebug() << "[AudioInput] ERROR: audio source error code"
                 << static_cast<int>(m_source->error());
        emit errorOccurred(tr("Audio source error %1")
                               .arg(static_cast<int>(m_source->error())));
    }

    if (m_state != next) {
        m_state = next;
        emit stateChanged(m_state);
    }
}

void AudioInput::onCapturedData(const QByteArray &data)
{
    emit audioDataReady(data, m_source->format());
}
