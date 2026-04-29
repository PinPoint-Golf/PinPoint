#pragma once
#include <QAudioFormat>
#include <QByteArray>
#include <vector>

class AudioConverter {
public:
    // Convert raw audio bytes from Qt capture to 16kHz mono float32.
    // sourceSampleRate, sourceChannels, and sourceSampleFormat are read
    // from the QAudioFormat active at capture time — do NOT hardcode these.
    static std::vector<float> toWhisperFormat(
        const QByteArray& rawBytes,
        int sourceSampleRate,
        int sourceChannels,
        QAudioFormat::SampleFormat sourceSampleFormat);
};
