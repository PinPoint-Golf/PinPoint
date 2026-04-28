#pragma once

#include <QObject>
#include <QAudioFormat>

// Abstract base for classes that consume a stream of raw PCM audio frames.
//
// Subclasses override processAudio() and are connected to an AudioInputBase
// via AudioInputBase::connectProcessor(), or manually via:
//   connect(input, &AudioInputBase::audioDataReady,
//           processor, &AudioProcessorBase::processAudio);
//
// processAudio() is called with whatever buffer size the platform delivers;
// subclasses must not assume a fixed chunk size.

class AudioProcessorBase : public QObject
{
    Q_OBJECT

public:
    explicit AudioProcessorBase(QObject *parent = nullptr);
    ~AudioProcessorBase() override = default;

public slots:
    virtual void processAudio(const QByteArray &data, const QAudioFormat &format) = 0;
};
