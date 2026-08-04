/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "ting_player.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>

TingPlayer::TingPlayer(QObject *parent)
    : QObject(parent)
    , m_pcm(synthesize(m_frequency))
{}

TingPlayer::~TingPlayer()
{
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
    }
    delete m_buf;
}

void TingPlayer::play()
{
    // If the previous ting is still playing, don't interrupt it.
    if (m_sink && m_sink->state() == QAudio::ActiveState)
        return;

    // Release any previous sink/buffer before creating fresh ones.
    // Using plain delete (not deleteLater + parent) to avoid re-entrant
    // stateChanged signals that can occur when stop() is called from within
    // a stateChanged handler.
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    delete m_buf;
    m_buf = nullptr;

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull())
        return;

    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);

    m_buf = new QBuffer();
    m_buf->setData(m_pcm);
    m_buf->open(QIODevice::ReadOnly);

    m_sink = new QAudioSink(device, fmt);
    m_sink->setVolume(m_volume);
    m_sink->start(m_buf);
}

void TingPlayer::setVolume(double v)
{
    // Applied when the next ting starts rather than to the sink in flight: these are
    // 600 ms one-shots, so there is nothing to gain from ducking one mid-play.
    m_volume = std::clamp(v, 0.0, 1.0);
}

void TingPlayer::setFrequency(double hz)
{
    if (qFuzzyCompare(hz, m_frequency))
        return;
    m_frequency = hz;
    m_pcm = synthesize(hz);
}

// Synthesises a soft ting: sine wave with a short linear attack and an
// exponential decay, 44100 Hz / 16-bit mono / 600 ms.
QByteArray TingPlayer::synthesize(double freq)
{
    constexpr int    kSampleRate  = 44100;
    constexpr double kDuration    = 0.6;          // seconds
    constexpr double kDecay       = 5.0;           // natural units per second
    constexpr int    kAttackMs    = 5;             // linear attack duration
    constexpr double kAmplitude   = 0.70 * 32767.0;

    const int nSamples    = static_cast<int>(kSampleRate * kDuration);
    const int attackSamps = kAttackMs * kSampleRate / 1000;

    QByteArray data(nSamples * 2, '\0');
    auto *out = reinterpret_cast<int16_t *>(data.data());

    for (int i = 0; i < nSamples; ++i) {
        const double t      = static_cast<double>(i) / kSampleRate;
        const double attack = (i < attackSamps) ? static_cast<double>(i) / attackSamps : 1.0;
        const double env    = attack * std::exp(-kDecay * t);
        const double s      = kAmplitude * env * std::sin(2.0 * M_PI * freq * t);
        out[i] = static_cast<int16_t>(std::clamp(s, -32767.0, 32767.0));
    }

    return data;
}
