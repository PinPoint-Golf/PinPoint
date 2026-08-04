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

#pragma once

#include <QByteArray>
#include <QObject>
#include <QtQml/qqml.h>

class QAudioSink;
class QBuffer;

// Plays a short synthesised ting tone on the default audio output.
// The tone is pre-rendered at construction time; play() is safe to call
// from the main thread at any time.  If already playing, the call is ignored.
// frequency defaults to C6 (1046.5 Hz); set to C8 (4186 Hz) for a high ting.
class TingPlayer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double frequency READ frequency WRITE setFrequency)
    // Output level, 0..1, mapped to QAudioSink::setVolume. Defaults to full scale, so
    // every caller that predates this property sounds exactly as it did.
    Q_PROPERTY(double volume READ volume WRITE setVolume)

public:
    explicit TingPlayer(QObject *parent = nullptr);
    ~TingPlayer() override;

    double frequency() const { return m_frequency; }
    void   setFrequency(double hz);

    double volume() const { return m_volume; }
    void   setVolume(double v);

    Q_INVOKABLE void play();

private:
    static QByteArray synthesize(double freq);

    double      m_frequency = 1046.5;   // C6
    double      m_volume    = 1.0;      // full scale — the pre-existing behaviour
    QByteArray  m_pcm;
    QAudioSink *m_sink = nullptr;
    QBuffer    *m_buf  = nullptr;
};
