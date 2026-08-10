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

#include <QAudioFormat>
#include <QObject>

#include "onset_detector.h"

#include <atomic>

// Acoustic shot detector (shot detection P2) — thin QObject around the pure
// OnsetDetector math. A *second consumer* of AudioInputBase::audioDataReady
// (the AudioStreamSaver pattern), running at the device's native rate in
// parallel with STT; adding it never disturbs the STT pipeline.
//
// Owned by TranscriptionController and moved to its audio thread, so
// onAudioData runs as a same-thread direct call: nowMicros() is stamped at
// buffer receipt (small, stable latency — this is what makes audio the
// pinpoint modality) and est_t* is computed before any thread hop.
// impactDetected is emitted on the audio thread; consumers connect with
// their own context object and get the queued hop automatically.
class AcousticShotDetector : public QObject
{
    Q_OBJECT

public:
    explicit AcousticShotDetector(QObject *parent = nullptr);

    // Total back-date between the IMPACT and its buffer arriving here: residual
    // device latency (~0 for a wired mic — the sample-counting reconstruction
    // already removes buffer-period latency) plus the hitting-strip -> mic
    // acoustic travel (AppSettings::micTravelUs; main.cpp wires the sum).
    // Atomic — set from the GUI thread while onAudioData reads it on the
    // audio thread.
    void setDeviceLatencyUs(qint64 us) { m_deviceLatencyUs.store(us, std::memory_order_relaxed); }

    // Candidate trigger threshold = factor × noise floor; higher = less
    // sensitive. Atomic — set from the GUI thread (microphone calibration)
    // while onAudioData applies it on the audio thread at buffer boundaries.
    // Default mirrors OnsetDetectorConfig::thresholdFactor (8.0).
    void setThresholdFactor(float f) { m_thresholdFactor.store(f, std::memory_order_relaxed); }

    // Absolute amplitude gate (OnsetDetectorConfig::minLevelAbs): a candidate
    // only opens when the envelope exceeds this, so quiet ticks/ambient never
    // fire and never mask a real impact. This is the primary calibration knob.
    // Atomic, applied per-buffer like the threshold factor. 0 = disabled.
    void setMinLevel(float lvl) { m_minLevel.store(lvl, std::memory_order_relaxed); }

public slots:
    // Audio-thread context (direct connection from audioDataReady).
    void onAudioData(const QByteArray &data, const QAudioFormat &format);

signals:
    // estImpactUs is the back-dated true-impact estimate in
    // EventBuffer::nowMicros() domain — sample-accurate up to the device
    // latency constant.
    void impactDetected(qint64 estImpactUs, float confidence);

    // Emitted once per processed buffer (~50–100 Hz) for the calibration meter:
    // the peak envelope over the buffer, plus the current noise floor and
    // trigger threshold. Audio-thread context — connect with a receiver.
    void levelSample(float level, float noiseFloor, float threshold);

private:
    pinpoint::OnsetDetector m_detector;
    QAudioFormat            m_format;       // last-seen; reset() on change
    std::atomic<qint64>     m_deviceLatencyUs { 0 };
    std::atomic<float>      m_thresholdFactor { 8.0f };
    std::atomic<float>      m_minLevel        { 0.0f };
};
