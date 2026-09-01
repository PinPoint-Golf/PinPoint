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

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariantMap>
#include <QTimer>
#include <QVector>

#include "event_buffer.h"
#include "shot_arbiter.h"

class SessionController;

#ifdef HAVE_PPCP
// Forward-declared, not included: ppcp_shot_bridge.h pulls the whole libppcp
// entity vocabulary in, and this header is included by half of src/Gui.
namespace Ppcp { class PpcpShotBridge; }
#endif

// Central application-level shot trigger. The toolbar SHOT button calls
// triggerShot() (direct commit — manual bypasses the arbiter hold); the auto
// detectors (IMU impact, acoustic onset, ball launch) call reportCandidate(),
// which funnels through the ShotArbiter's candidate→hold→fuse→commit window
// (shot_arbiter.h). shotDetected is the single signal ShotProcessor
// (post-roll → buffer freeze → shot window → analysis ∥ export → replay) is
// driven from.
//
// On every committed shot a shot-marker event is written into the
// EventBuffer (source "shot_controller", schema "shot_marker_v1") so the
// precise impact instant lives on the same steady_clock-µs timeline as the
// video frames and IMU samples. A later captureSwingWindow() contains the
// marker via entriesFor(), letting the processor align the window to impact.
class ShotController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool armed READ armed NOTIFY armedChanged)

public:
    // ⚠ THE ORDINALS ARE PERSISTED AND ONE OF THEM IS LOAD-BEARING.  `Acoustic`
    // is 4, and it is written into `swing.json` as `capture.shotSource`;
    // `swing_reanalyzer` gates a microphone time-of-flight de-bias on
    // `shotSource == 4`.  So `Ppcp` is APPENDED and an arbitrated shot carries
    // its own value: labelling one `Acoustic` would offer it to that de-bias,
    // and shifting a `t0` the protocol treats as never-revisable (I7) is the
    // one correction that must never happen by accident.
    enum class Source { Manual, Imu, Pose, Ball, Acoustic, Ppcp };
    Q_ENUM(Source)

    // Marker payload stored in the EventBuffer ring (schema "shot_marker_v1").
    struct ShotMarker {
        uint32_t version;       // 1
        uint16_t source;        // ShotController::Source
        int16_t  session_type;  // SessionController::Type (-1 = none)
        int64_t  impact_ts_us;  // same value as the entry timestamp
    };
    static_assert(sizeof(ShotMarker) == 16, "shot_marker_v1 is 16 bytes");

    explicit ShotController(pinpoint::EventBuffer *buffer,
                            SessionController     *session,
                            QObject               *parent = nullptr);
    // Declared only because the class needs an out-of-line one; there is
    // nothing to undo.  See the note beside the definition — an earlier version
    // reached for the PPCP bridge here and that was the 27 Aug crash.
    ~ShotController() override;

    // True while the buffer is capturing AND the shot processor is idle — the
    // only state a shot can fire in. Every source inherits the gate.
    bool armed() const;

    // Direct commit — bypasses the arbiter hold. timestampUs: precise impact
    // instant in EventBuffer::nowMicros() domain (steady_clock µs); -1 →
    // "now" (the manual button). A pending arbiter window is cancelled and
    // the arbiter refractory is noted, so auto candidates cannot double-fire
    // around a manual shot.
    Q_INVOKABLE void triggerShot(Source source = Source::Manual,
                                 qint64 timestampUs = -1);

    // Auto-detector funnel (shot detection P3): estImpactUs is the
    // detector's back-dated true-impact estimate, confidence its gate
    // strength. The first candidate opens a hold window (kArbHoldMs); at the
    // deadline the arbiter fuses candidates and commits at most one shot
    // (>=2 agreeing modalities, or one strong one — see shot_arbiter.h).
    // Inherits the armed() gate; Manual is rerouted to triggerShot().
    void reportCandidate(Source source, qint64 estImpactUs, float confidence);

#ifdef HAVE_PPCP
    // ── H5 — PPCP arbitration REPLACES the arbiter above ───────────────────
    //
    // ⚠ REPLACES.  NOT "IN ADDITION TO", AND NOT "FALLS BACK TO".  For any
    // Session with a PPCP peer in it, reportCandidate() nominates into the
    // library's Arbitrate engine and RETURNS WITHOUT TOUCHING `m_arbiter`.
    // Running both would give one Session two arbiters that disagree about
    // which Candidates exist, and the disagreement would be silent.
    //
    // The reason it must be a replacement rather than a layer is that
    // `ShotArbiter` breaks three PPCP invariants the moment a second peer
    // appears, and breaks all three quietly:
    //
    //   I8   it models THREE FIXED MODALITIES in fixed slots, so a host
    //        microphone and a device microphone — two Sources of the same
    //        `basis: acoustic` — collide for one slot and one is dropped with
    //        no record.  CT-I8 exists for exactly that failure.
    //   8.2f `decide()` throws the losing candidates away, and an issued Shot
    //        must reference EVERY contributing and excluded Candidate:
    //        "exclusion is a conclusion; the Candidate remains evidence."
    //   8.2e a 1500 ms refractory DROPS a late nomination, where the
    //        specification says it ATTACHES to the Shot already issued.
    //
    // Null on every build without libppcp, and null on every Session with no
    // PPCP peer — which is every Session this application runs today, so the
    // local path above is unchanged for the shipping product.
    void setPpcpBridge(Ppcp::PpcpShotBridge *bridge);
    // The host's OWN declared Source ids, per basis (I26 / 5.12a: a Candidate
    // names a Source THIS peer declared).  Empty means "this host has no
    // nominator of that kind", and a candidate of that kind is then not
    // nominated rather than being attributed to some other Source.
    void setPpcpSourceIds(const QString &acoustic, const QString &motion,
                          const QString &vision);
    // 8.2h — the arbiter issues on a CLOCK, not on an event: no earlier than
    // `issue_hold_ns` after the earliest contributing Candidate.  Whoever owns
    // the link calls this when the bridge reports an issued Shot, and `t0` is a
    // reading of `Session.timebase_ref` in nanoseconds, which for this host is
    // `tb:host` and therefore EventBuffer::nowMicros() * 1000.
    //
    // ⚠ AND IT IS NOT AN UNCONDITIONAL COMMIT.  See the corroboration rule
    // below: a Shot the arbiter issued may still not become a swing here.
    void commitArbitratedShot(qint64 t0HostNs, const QString &shotId = QString());

    // ── The corroboration rule (Mark, 27 Aug 2026) ─────────────────────────
    //
    // A Shot arbitrated from a DEVICE's Candidate is accepted only on evidence
    // this host can weigh:
    //
    //   - no host detector available  → ACCEPT.  We have nothing to refute it
    //     with, and refusing on no evidence would be a fabrication of the same
    //     kind the protocol refuses everywhere else.
    //   - any host detector available → accept only if at least ONE of them
    //     fired within `kCorroborationWindowUs` of `t0`.
    //
    // "Any one", not "all": ball-launch detection is corroboration-grade by
    // construction (its confidence sits below the local arbiter's lone-candidate
    // floor) and misses often, so requiring it would refuse real shots.  Every
    // available detector's verdict is recorded either way, so the stricter rule
    // can be judged from session data rather than from argument.
    //
    // ⛔ THE LAUNCH MONITOR IS NOT A DETECTOR HERE.  Its reading arrives seconds
    // after the strike, in a file rewritten in place, and cannot confirm an
    // instant it did not observe.
    void setDetectorAvailable(Source source, bool available);

    // ── Automated testing: a detection this host did not actually make ──────
    //
    // ⚠ WHY THIS EXISTS.  A capture device driven in a simulator produces no
    // ball strike and no sound, so a host with a working microphone hears
    // nothing and the corroboration rule above refuses every Shot it is sent.
    // The rule is then untestable by exactly the automation that most needs to
    // test it.  `PPCP-CONF` §2a already blesses **injected** as a conformance
    // method — "requires the injectable clock, a simulated offset/skew, or a
    // synthetic declaration the implementation would not otherwise meet" — and
    // several CT rows are only reachable that way.  This is that method,
    // applied to the one input a simulator cannot supply.
    //
    // ⛔ IT INJECTS EVIDENCE THIS HOST HOLDS, AND NEVER A CANDIDATE ON THE WIRE.
    // A fabricated `candidate` would attribute a detection to a declared
    // microphone Source that heard nothing, which is 8.1e — a peer synthesising
    // a record for something it did not observe.  So this reaches the local
    // detection ring and stops there: it can make a corroborated Shot pass, and
    // it cannot make this host lie to a counterpart.
    //
    // ⛔ DEV BUILDS ONLY.  Compiled out entirely under PP_SHIPPING_BUILD, and
    // every call says so in the log.
    Q_INVOKABLE void injectDetection(Source source, qint64 tUs);

    // What an automated run needs in order to ASSERT rather than scrape a log.
    // The corroboration verdict is otherwise a `ppDebug()` line that a release
    // build does not even emit.
    Q_INVOKABLE QVariantMap shotStats() const;

    // 5.10's coincidence window, and deliberately the same number the Session
    // is opened with — one rule stated once.  CORE §5.10's own proposal, not a
    // measurement (CORE B8 says so too).
    static constexpr qint64 kCorroborationWindowUs = 50'000;
#endif

public slots:
    // Connected to CameraManager::bufferStateChanged (the single always-notified
    // buffer-state signal) so the armed property tracks every net transition.
    void reevaluateArmed();

    // Connected to ShotProcessor::busyChanged in main.cpp — disarms the
    // trigger for the whole post-roll → processing → replay pipeline.
    void setProcessorBusy(bool busy);

    // Connected to SessionReviewController::reviewActiveChanged in main.cpp.
    // Entering review already stops live capture (which disarms via the buffer
    // state); this is the explicit belt-and-braces gate so no shot source can
    // fire while a saved session is loaded.
    void setReviewActive(bool active);

signals:
    // sessionType: SessionController::Type active at the moment of the shot
    // (-1 = none — unreachable through the UI, where capture implies a session).
    void shotDetected(ShotController::Source source, qint64 timestampUs, int sessionType);
    void armedChanged();

    // A device's Shot that this host declined to record, and why in one line
    // fit for a person: the golfer hit a ball and nothing came of it, so
    // something has to say so.  `reason` is already translated.
    //
    // ⚠ `id` IS NOT DECORATION.  These are two different faults — nothing
    // corroborated the phone's shot, versus the pipeline was still busy with the
    // last one — and they used to arrive as one signal carrying two different
    // strings, so the screen could only ever show whichever landed last.  A
    // stable id is what lets the notification model count them separately; on
    // 1 September 2026 that was four of the first and two of the second.
    void shotRefused(const QString &reason, const QString &id);

    // ⭐ CORE §8.4 — a committed shot for which a phone should be asked for its
    // footage.  `t0HostNs` is a reading of `Session.timebase_ref`: the SAME
    // steady_clock the event buffer reads, times 1000, which is asserted by a
    // test in the shot bridge rather than left as a comment.
    //
    // ⚠ EMITTED AT THE SHOT, not when the swing folder exists.  The folder is
    // allocated 4-11 s later, behind the post-roll and the history gather, and
    // every second of waiting spends a phone ring that is finite.  What the
    // clip is eventually filed against is settled later, by correlation.
    void captureRequested(const QString &shotId, qint64 t0HostNs);

private:
    // The single commit path — writes the marker and emits shotDetected.
    // Re-checks armed() (the processor may have gone busy mid-hold).
    //
    // Answers whether the shot was actually committed.  A dropped shot has no
    // swing to file anything against, so the capture request must not go out
    // for one — and the old `void` gave the caller no way to tell.
    bool commitShot(Source source, qint64 timestampUs);
    void onArbHoldExpired();
    void writeShotMarker(Source source, int64_t impactUs, int sessionType);

    pinpoint::EventBuffer *m_buffer   = nullptr;
    SessionController     *m_session  = nullptr;
    pinpoint::SourceId     m_sourceId = pinpoint::kInvalidSourceId;
    bool                   m_processorBusy = false;
    bool                   m_reviewActive  = false;
    bool                   m_lastArmed = false;

    pinpoint::ShotArbiter  m_arbiter;
    QTimer                 m_arbTimer;   // single-shot hold-window deadline

#ifdef HAVE_PPCP
    Ppcp::PpcpShotBridge  *m_ppcpBridge = nullptr;
    QString                m_ppcpAcousticSourceId;
    QString                m_ppcpMotionSourceId;
    QString                m_ppcpVisionSourceId;

    // ── What this host saw, so it can weigh what a device claims ────────────
    //
    // Every local detection, recorded as `reportCandidate()` sees it and BEFORE
    // any gate — including the modalities I26 refuses to nominate, because a
    // detection this host cannot put on the wire is still evidence this host
    // holds.  That is the whole reason the ring exists rather than reading the
    // arbiter's own candidate list: an IMU impact cannot be a PPCP Candidate
    // until the host declares a motion Source, and until then the corroboration
    // rule would silently lose it.
    //
    // ⚠ The consequence is worth stating: an issued Shot will not REFERENCE
    // those detections (8.2f), because they never became Candidates.  Declaring
    // a motion Source is the proper fix and is deliberately a separate change.
    struct Detection {
        Source source;
        qint64 tUs;
    };
    // Small and fixed: the rule only ever looks 50 ms either side of a t0, so
    // anything but the last handful of detections is dead weight.
    static constexpr int kDetectionRing = 16;
    QVector<Detection>     m_detections;
    // Availability is CONFIGURATION, not history: a detector that is switched
    // on and attached is available even before it has ever fired, which is what
    // makes the first shot of a session subject to the rule rather than exempt
    // from it.
    QSet<int>              m_availableDetectors;
    // Counters an automated run reads back; see shotStats().
    struct Counters {
        int detections       = 0;
        int injected         = 0;
        int arbitratedSeen   = 0;
        int corroboratePass  = 0;
        int corroborateFail  = 0;
        int droppedBusy      = 0;
        int committed        = 0;
    } m_counters;
    QString                m_lastVerdict;

    void   noteDetection(Source source, qint64 tUs);
    // The rule of §setDetectorAvailable, evaluated.  Fills `outWhy` with the
    // per-detector verdicts for the testing log.
    bool   corroborated(qint64 t0Us, QString *outWhy) const;
#endif
};
