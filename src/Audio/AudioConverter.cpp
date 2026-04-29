#include "AudioConverter.h"
#include <samplerate.h>
#include <cstdint>
#include <cstring>

std::vector<float> AudioConverter::toWhisperFormat(
    const QByteArray& rawBytes,
    int sourceSampleRate,
    int sourceChannels,
    QAudioFormat::SampleFormat sourceSampleFormat)
{
    // Step 1 — convert raw bytes to float32 per sample
    std::vector<float> floatSamples;

    switch (sourceSampleFormat) {
    case QAudioFormat::Int16: {
        const int count = rawBytes.size() / 2;
        floatSamples.resize(count);
        const auto* src = reinterpret_cast<const int16_t*>(rawBytes.constData());
        for (int i = 0; i < count; ++i)
            floatSamples[i] = src[i] / 32768.0f;
        break;
    }
    case QAudioFormat::Int32: {
        const int count = rawBytes.size() / 4;
        floatSamples.resize(count);
        const auto* src = reinterpret_cast<const int32_t*>(rawBytes.constData());
        for (int i = 0; i < count; ++i)
            floatSamples[i] = src[i] / 2147483648.0f;
        break;
    }
    case QAudioFormat::Float: {
        const int count = rawBytes.size() / 4;
        floatSamples.resize(count);
        std::memcpy(floatSamples.data(), rawBytes.constData(),
                    static_cast<std::size_t>(rawBytes.size()));
        break;
    }
    case QAudioFormat::UInt8: {
        const int count = rawBytes.size();
        floatSamples.resize(count);
        const auto* src = reinterpret_cast<const uint8_t*>(rawBytes.constData());
        for (int i = 0; i < count; ++i)
            floatSamples[i] = (src[i] / 128.0f) - 1.0f;
        break;
    }
    default:
        return {};
    }

    // Step 2 — downmix to mono if sourceChannels > 1
    std::vector<float> monoFloat;
    if (sourceChannels <= 1) {
        monoFloat = std::move(floatSamples);
    } else {
        const int frames = static_cast<int>(floatSamples.size()) / sourceChannels;
        monoFloat.resize(frames);
        for (int i = 0; i < frames; ++i) {
            float sum = 0.0f;
            for (int ch = 0; ch < sourceChannels; ++ch)
                sum += floatSamples[i * sourceChannels + ch];
            monoFloat[i] = sum / static_cast<float>(sourceChannels);
        }
    }

    // Step 3 — resample to 16000 Hz if sourceSampleRate != 16000
    if (sourceSampleRate == 16000)
        return monoFloat;

    SRC_DATA data{};
    data.data_in       = monoFloat.data();
    data.input_frames  = static_cast<long>(monoFloat.size());
    data.src_ratio     = 16000.0 / sourceSampleRate;
    data.output_frames = static_cast<long>(monoFloat.size() * data.src_ratio + 1);
    std::vector<float> resampled(static_cast<std::size_t>(data.output_frames));
    data.data_out      = resampled.data();
    src_simple(&data, SRC_SINC_MEDIUM_QUALITY, 1);
    resampled.resize(static_cast<std::size_t>(data.output_frames_gen));
    return resampled;
}
