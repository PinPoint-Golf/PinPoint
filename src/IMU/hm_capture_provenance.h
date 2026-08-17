// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// hm_capture_provenance.h — what a recorded HackMotion window can say about
// itself, and why so little of it survives without this.
// ---------------------------------------------------------------------------
//
// ⚠ THE PROBLEM THIS EXISTS FOR. A HackMotion reading reaches swing.json as a
// 40-byte pinpoint::ImuSample — ten floats — plus a host timestamp. The NUMBERS
// survive that intact: accel is raw counts × 0.001 exactly, the quaternion is
// i16/16384 which is exact in float32, and gyro comes from the library's
// config-aware scaled field rather than a guessed divisor. What does NOT survive
// is every field of hm_sample that says whether to trust those numbers. Two of
// them are load-bearing and neither can be reconstructed afterwards:
//
//   CALIBRATION STATE. The device applies its own anatomical transform, and
//     hackmotion/sample.h is explicit about the cost of dropping the flag: it
//     "is applied ON-DEVICE and is not recoverable later, so if the recording
//     does not carry this flag the mistake is permanent and invisible."
//     Pre-calibration quaternions are valid geometry and meaningless anatomy —
//     spec §8.1 measures 11-15° of raw mounting offset at a straight wrist — and
//     a calibrated and an uncalibrated sample are byte-indistinguishable.
//
//   PINNING. int16 fields SATURATE rather than wrap, so a clipped channel is a
//     plausible flat-topped waveform rather than an obvious fault, and nothing
//     in the protocol reports it. §6.4 measures a struck golf swing at 53-58 %
//     of full scale and a deliberate wrist flick at 83 %, so this is reachable
//     in ordinary use rather than only in abuse.
//
// ⚠ WHY THIS IS NOT SOLVED BY WIDENING THE SAMPLE. A prefix-compatible record
// (imu_sample_v2 followed by extra fields) is invisible to every consumer in the
// tree except swing_window.cpp, which compares the record size to
// sizeof(ImuSample) with EXACT EQUALITY and would silently return false —
// dropping the HackMotion lane out of analysis with nothing reporting it, in the
// interpolation pre-stage shared with Witmotion. That change is the more correct
// architecture and remains available; it is not one a pre-Phase-D addendum should
// carry. See the plan's Phase B′.
//
// ⚠ SO THE SPLIT IS BY HOW THE DATA BEHAVES, NOT BY HOW IMPORTANT IT IS:
//
//   window-CONSTANT  calibration state, the stream config byte. These cannot
//                    change inside a swing — the link drop that would change the
//                    calibration also ends the stream — so they are carried as a
//                    span with a transition flag.
//   window-SELECTABLE pinning, suspect quaternion norms. Rare by nature, so they
//                    are kept as TIMESTAMPED ENTRIES a window selects from. A
//                    session total would answer "did this session ever clip",
//                    which is not the question the analysis of one swing asks.
//
// Pure and header-only on purpose — no Qt, no session, no clock, no threading —
// so the window arithmetic that decides what a swing is allowed to claim is
// testable without a device. The caller owns any locking.
#ifndef PINPOINT_HM_CAPTURE_PROVENANCE_H
#define PINPOINT_HM_CAPTURE_PROVENANCE_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pinpoint::hm {

// One sample-level condition worth remembering, with the time that lets a window
// claim or disclaim it.
struct SampleException {
    enum Reason : uint8_t {
        Pinned          = 0,  // hm_unit_sample::pinned_mask — a channel saturated
        QuatNormSuspect = 1   // HM_SAMPLE_QUAT_NORM_SUSPECT — decode may be misaligned
    };
    int64_t hostTimeUs = 0;
    // hm_unit for Pinned. ⚠ kSampleLevel for QuatNormSuspect, which is a property
    // of the RECORD rather than of one unit — reporting it per unit would count one
    // condition twice and imply the decode located one block and not the other.
    uint8_t unit        = 0;
    uint8_t reason      = Pinned;
    uint8_t channelMask = 0;   // hm_unit_sample::pinned_mask; 0 unless Pinned
};

inline constexpr uint8_t kSampleLevel = 0xFF;

// Mirrors the library's own hm_calibration_span, and for its reason: "calibrated
// at the start and gone by the end" is a different fact from "never calibrated",
// and a single state value cannot express both.
struct CalibrationSpan {
    int  stateAtStart    = -1;   // hm_calibration_state; -1 = no sample in the window
    int  stateAtEnd      = -1;
    bool spansTransition = false;
};

struct CaptureProvenance {
    CalibrationSpan calibration;
    int configBits = -1;         // the `a0 01 <cfg>` byte; -1 = no sample seen
    std::vector<SampleException> exceptions;   // only those inside the window
    // ⚠ Ring overflow. Non-zero means pinning was too dense for the ring, so any
    // count derived from `exceptions` is a FLOOR rather than a count — and the
    // rarity assumption this design rests on has failed. The answer then is a wider
    // per-sample record, not a bigger ring.
    uint64_t exceptionsDropped = 0;
    // ⚠ SESSION TOTAL, NOT WINDOWED, and that is not an oversight: a sample skipped
    // for having no mapped host time has, by definition, no host time to place it in
    // a window with. A cumulative count is the honest form.
    uint64_t noFitSkipped = 0;
};

// ---------------------------------------------------------------------------
// The log itself
// ---------------------------------------------------------------------------
//
// ⚠ NOT THREAD-SAFE, DELIBERATELY. The caller holds a lock it already owns for
// other reasons; adding one here would mean two locks on the same data or a
// second, divergent idea of which one protects it.
class CaptureProvenanceLog
{
public:
    // Ring capacity. 512 entries is ~5 s of continuous pinning at the nominal
    // 100 Hz motion rate — far past anything a swing should produce, and bounded so
    // that a session pinning constantly cannot grow without limit.
    static constexpr size_t kMaxExceptions = 512;
    // ⚠ OVERFLOW HERE KEEPS THE OLDEST, opposite to the exception ring. The first
    // entries establish what state the session STARTED in, and a window that opens
    // before any later change resolves against them; dropping those to keep recent
    // churn would leave early windows unable to say anything at all.
    static constexpr size_t kMaxCalChanges = 64;

    // One recorded sample. ⚠ CALL ONLY FOR SAMPLES THAT ARE ACTUALLY RECORDED —
    // past the capture gate and past the no-fit gate. Provenance describing samples
    // the window does not contain is worse than none: it attributes a clip to a
    // swing that never saw one.
    void noteSample(int64_t hostTimeUs,
                    uint8_t pinnedMaskLowerArm,
                    uint8_t pinnedMaskPalm,
                    bool    quatNormSuspect,
                    int     calibrationState,
                    int     configBits)
    {
        if (pinnedMaskLowerArm != 0)
            pushException({ hostTimeUs, 0 /*HM_UNIT_LOWER_ARM*/,
                            SampleException::Pinned, pinnedMaskLowerArm });
        if (pinnedMaskPalm != 0)
            pushException({ hostTimeUs, 1 /*HM_UNIT_PALM*/,
                            SampleException::Pinned, pinnedMaskPalm });
        if (quatNormSuspect)
            pushException({ hostTimeUs, kSampleLevel,
                            SampleException::QuatNormSuspect, 0 });

        m_configBits = configBits;
        if (calibrationState != m_calCurrent) {
            m_calCurrent = calibrationState;
            if (m_calChanges.size() < kMaxCalChanges)
                m_calChanges.push_back({ hostTimeUs, calibrationState });
        }
    }

    // A sample the recording path dropped for having no mapped host time.
    void noteNoFitSkipped() { ++m_noFitSkipped; }

    // Everything the window [startUs, endUs] is entitled to claim. Inclusive at
    // both ends: a sample landing exactly on a boundary belongs to the window the
    // exporter built from those same timestamps.
    CaptureProvenance inWindow(int64_t startUs, int64_t endUs) const
    {
        CaptureProvenance p;
        p.configBits        = m_configBits;
        p.exceptionsDropped = m_exceptionsDropped;
        p.noFitSkipped      = m_noFitSkipped;

        for (const SampleException &e : m_exceptions) {
            if (e.hostTimeUs >= startUs && e.hostTimeUs <= endUs)
                p.exceptions.push_back(e);
        }
        // ⚠ The ring is out of time order once it has wrapped, and a caller reading
        // these as a sequence would otherwise see time run backwards mid-list.
        if (m_wrapped) {
            std::sort(p.exceptions.begin(), p.exceptions.end(),
                      [](const SampleException &a, const SampleException &b) {
                          return a.hostTimeUs < b.hostTimeUs;
                      });
        }

        // Every stored entry IS a change, so the state at an instant is the last
        // entry at or before it, and "did it change inside the window" is simply the
        // existence of an entry in (start, end].
        for (const auto &c : m_calChanges) {
            if (c.first <= startUs) {
                p.calibration.stateAtStart = c.second;
                p.calibration.stateAtEnd   = c.second;
            } else if (c.first <= endUs) {
                // ⚠ A change strictly inside the window. With nothing before it the
                // stream began inside this window, and that first state is the best
                // available answer for the start — not -1, which reads as "no data".
                if (p.calibration.stateAtStart < 0)
                    p.calibration.stateAtStart = c.second;
                else
                    p.calibration.spansTransition = true;
                p.calibration.stateAtEnd = c.second;
            }
        }
        return p;
    }

private:
    void pushException(const SampleException &e)
    {
        if (m_exceptions.size() < kMaxExceptions) {
            m_exceptions.push_back(e);
            return;
        }
        // Full: overwrite the oldest and say so. A swing is exported shortly after
        // it happens, so the recent end is the end a window needs.
        m_exceptions[m_head] = e;
        m_head    = (m_head + 1) % kMaxExceptions;
        m_wrapped = true;
        ++m_exceptionsDropped;
    }

    std::vector<SampleException> m_exceptions;
    size_t   m_head              = 0;
    bool     m_wrapped           = false;
    uint64_t m_exceptionsDropped = 0;

    std::vector<std::pair<int64_t, int>> m_calChanges;
    int m_calCurrent = -1;
    int m_configBits = -1;

    uint64_t m_noFitSkipped = 0;
};

} // namespace pinpoint::hm

#endif // PINPOINT_HM_CAPTURE_PROVENANCE_H
