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

// A connected capture peer's camera Source, behind the camera factory.  Work
// package H4.  CORE §5.11, §5.11.2, §6.1.
//
// ⚠ READ F7 BEFORE ASSUMING THIS CLASS IS THE WHOLE INTEGRATION.  The design
// review (`libppcp/docs/specification/reviews/pps-design-review-ppcp-2026-08-22.md`
// F7) found that REQ-HOST-1 names the wrong seam, and the disposition endorsed
// it: "a PPCP peer is a session-layer participant, not a frame source, and
// putting it behind a live-push camera interface would either strip what the
// protocol carries or smuggle a session model in behind a camera."  The same
// finding says where VideoInputBase IS right: "VideoInputBase is the right home
// only for a live *preview* stream".
//
// So this class is scoped to exactly that, and says out loud what it will not
// do:
//
//   * The LIVE TILE is a `preview` Stream (5.11.2) and reaches the application
//     as videoFrameReady() — an ordinary VideoInputBase frame source, which is
//     what a preview is.
//   * A CLIP is a shot- or candidate-anchored Capture with per-frame
//     `achieved_frames`, a completeness, gaps, a digest and a transfer state.
//     None of that fits through videoFrameReady(), and the consumer on the
//     other side of that signal stamps `EventBuffer::nowMicros()` on ARRIVAL
//     (`camera_instance.cpp`, connectVideoInput) — which for a peer's clip is
//     the time the bytes finished crossing a socket and has nothing to do with
//     when the frame was exposed.  Emitting a clip as a live frame would
//     therefore DESTROY the canonical instant this work package exists to
//     apply.  Clips leave here as clipReady() with their instants intact, for
//     the session layer of H5/H7 to place on a timeline.
//
// WHAT "THE CANONICAL INSTANT IS APPLIED" MEANS HERE (CORE §6.1, I17, CT-S1).
// `payload_begin` carries `achieved_frames` (I30) — the frame timestamps in the
// SOURCE's timebase, on the SOURCE's convention, with that frame's exposure.
// The conversion to the canonical instant needs all three inputs and no two of
// them are sufficient, so this class never assembles it by hand: it calls
// ppcp_achieved_frames_canonical_at(), which is L3's one-call form of I17, with
// the `timing` of the CaptureProfile the Stream named.  Nothing here reads a
// convention out of a product string, a role or a platform (CT-S3 assertion 3);
// the timing comes from the declaration and from nowhere else.
//
// ⚠ AND THE CANONICAL INSTANT IS NOT YET A HOST TIMESTAMP.  §6.1 converts
// WITHIN a Source's timebase; carrying it across to `tb:host` needs a
// TimebaseRelation (CORE 5.4, 6.3), which is the sync prober of H5 over
// libppcp's L9.  Neither has landed.  So lastFrameInstantUs() answers 0 —
// "I have no instant on your clock" — until setTimebaseOffsetNs() is given a
// real mapping, and a zero offset is never assumed to be one.  That is I31's
// discipline (an unmeasured constant is not a measured zero) applied to clocks:
// a fabricated mapping is exactly the exposure-dependent, drift-shaped error
// §6.1 opens by warning about.

#include <cstdint>
#include <memory>
#include <vector>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

#include <ppcp/model.h>
#include <ppcp/peer.h>
#include <ppcp/timing.h>

#include "video_input_base.h"

// One Capture's worth of clip, with the timing the protocol carried and this
// class converted.  A struct and not a QVideoFrame because a Capture is not a
// frame: it is an interval, a completeness, a set of per-frame instants and a
// payload that may be `absent` altogether.
struct PpcpClip {
    QString captureId;
    QString streamId;
    QString peerId;
    QString sourceId;

    // CORE 5.14 — `complete`, `partial` or `absent`.  `absent` carries a reason
    // and NO payload, and is a first-class answer (I10), never an error.
    ppcp_completeness completeness = PPCP_COMPLETE;
    QString           absentReason;

    // CORE §6.1 applied, one per frame, in `timebaseId` — NOT on the host
    // clock.  See the header note above.
    QVector<qint64> canonicalNs;
    QString         timebaseId;

    // The exposure durations the conversion used, so a consumer can see that
    // §6.1 was fed the per-frame value and not a profile range (CT-S1
    // assertion 3).
    QVector<qint64> exposureNs;

    // The payload as it arrived.  Empty for `absent`, and empty for a Capture
    // whose transfer failed or was refused.
    QByteArray payload;

    bool preview = false;
};
Q_DECLARE_METATYPE(PpcpClip)

class VideoInputPpcp : public VideoInputBase
{
    Q_OBJECT

public:
    explicit VideoInputPpcp(QObject *parent = nullptr);
    ~VideoInputPpcp() override;

    // ── VideoInputBase ─────────────────────────────────────────────────────
    bool               start(const QString &deviceId = {}) override;
    void               stop()    override;
    void               suspend() override;
    void               resume()  override;
    bool               isActive()    const override;
    QVideoFrameFormat  frameFormat() const override;
    CameraCapabilities queryCapabilities() const override;
    void               prepareDevice(const QString &deviceId) override;
    // The per-frame instant side channel.  It exists for the same reason
    // lastMeasuredExposureUs() does — a fact about the frame that cannot travel
    // ON the frame — and answers 0 while there is no timebase mapping.
    qint64             lastFrameInstantUs() const override;

    // ── Identity ───────────────────────────────────────────────────────────
    // "ppcp:<peer_id>/<source_id>".  A device id, because DeviceEnumerator and
    // CameraInstance key everything on one and a PPCP Source needs both halves
    // to be unique: CORE 8.5c scopes ids by the MINTING PEER, so two devices
    // may legitimately both call a Source "cam-0".
    static QString deviceIdFor(const QString &peerId, const QString &sourceId);
    static bool    parseDeviceId(const QString &deviceId, QString *peerId, QString *sourceId);

    // ── The PPCP half ──────────────────────────────────────────────────────
    // Ground rule 7 applied to this class as it is to PpcpHostPeer: it owns no
    // socket, no engine and no thread.  The embedding hands it the peer whose
    // link this Source is reached over, and drives drainEvents() from wherever
    // its pump lives.
    void attach(ppcp_peer *peer, const QString &sessionId);
    void detach();
    ppcp_peer *peer() const { return m_peer; }

    // Everything the engine queued since the last call.  ⚠ MUST be called once
    // per fed frame on a socket path, and is by PpcpHostPeer's pump: the event
    // ring is four deep and `payload_chunk.data` points into the buffer the
    // transport fed, so a chunk's bytes are valid only until the next frame is
    // presented (the same rule PpcpImportSink is built around).
    void drainEvents();

    // CORE 5.4 / 6.3 — host_ns = source_ns + offset.  H5's sync prober over
    // L9's TimebaseRelation supplies it; until then there is none and
    // lastFrameInstantUs() says so by answering 0.
    void setTimebaseOffsetNs(qint64 offsetNs);
    void clearTimebaseMapping();
    bool hasTimebaseMapping() const { return m_hasOffset; }

    // ── What the last Capture carried, for evidence ────────────────────────
    // CORE §6.1 applied, in the SOURCE's timebase.  This is CT-S1's observable
    // on the host path: the conversion, not the mapping.
    QVector<qint64> lastCanonicalNs() const { return m_lastCanonicalNs; }
    QString         lastCanonicalTimebase() const { return m_lastCanonicalTb; }

    // CORE §6.2 — what time row `r` of frame `frame` was exposed, in the
    // SOURCE's timebase.  A consumer of a rolling-shutter Source has to ask:
    // the frame's canonical instant is the FIRST ROW's (6.2c), and a clubhead
    // crossing the bottom of a 1080-row sensor was seen up to `readout_ns`
    // later than one crossing the top.  `global` geometry answers the frame's
    // own instant rather than refusing, so nothing downstream has to branch on
    // geometry to ask the question.  Returns false where the last Capture
    // carried no convertible frames, or the index is out of range.
    bool rowInstantNs(int frame, quint32 row, qint64 *outNs) const;

    struct Counters {
        std::size_t previewCaptures = 0;
        std::size_t previewFrames   = 0;     // emitted as videoFrameReady
        std::size_t clips           = 0;     // emitted as clipReady
        std::size_t absentSegments  = 0;     // 5.11c3 — the honest account
        // CT-I36a, host as consumer.  5.11j: "a consumer therefore never sees
        // `transfer: pending` on a preview Capture".  Counted rather than
        // assumed away, because the engine does NOT catch it on the receiving
        // side — see the note in the .cpp.
        std::size_t previewPendingRefused = 0;
        // I11 — `gaps` mean LOSS.  A preview peer that records a shed frame as
        // a gap is reporting a dropout it did not have (5.11c3); counted so the
        // conflation is visible rather than absorbed.
        std::size_t previewGapsSeen = 0;
        // A payload whose Capture named a Stream this Source does not own, or a
        // profile the declaration does not carry: no timing, so no conversion.
        std::size_t unconvertible = 0;
        std::size_t decodeFailures = 0;
    };
    const Counters &counters() const { return m_counters; }

    // ── Capabilities, as a pure function of a declaration ──────────────────
    // Static and free of any live state so CT-S3's discipline holds here too:
    // everything below is read from what arrived on the wire.  A profile
    // declaring `intrinsics: none` is a PREVIEW profile (5.11m) and is excluded
    // from the capture capability set, because 5.11l forbids a consumer
    // selecting one for capture.
    static CameraCapabilities capabilitiesFor(const ppcp_peer_desc *peer,
                                              const ppcp_source   *source);

    // PPCP cameras are not discovered by scanning — they appear when a capture
    // peer connects and declares (MSG 3.3).  So registration is driven from the
    // declaration and NOT from VideoInputFactory::enumerateDevices(), which has
    // no hardware to look at.  Returns how many camera Sources were registered.
    // Defined in VideoInputPpcp_inventory.cpp, which is the one translation
    // unit here that touches DeviceEnumerator — kept apart for exactly the
    // reason ppcp_host_inventory.cpp is (H2), so this class stays testable with
    // no device registry in the link.
    static int registerSources(const ppcp_peer_desc *peer);

signals:
    // A Capture that is NOT a preview frame, with its canonical instants
    // intact.  Deliberately not videoFrameReady() — see the header note.
    void clipReady(const PpcpClip &clip);

private:
    // Re-resolved on every use rather than cached: MSG 3.3a makes a `declare` a
    // complete snapshot that WHOLLY REPLACES the previous one, so any pointer
    // into the old arena is stale the moment the counterpart declares again.
    const ppcp_peer_desc *counterpart() const;
    const ppcp_source    *source() const;
    const ppcp_capture_profile *profileById(const char *id) const;
    // The profile a Stream named, via the Stream the engine recorded.
    const ppcp_capture_profile *profileForStream(const QString &streamId) const;

    // The best capture profile (highest nominal rate, `intrinsics` not `none`)
    // and the best preview profile (`intrinsics: none`), or null.
    const ppcp_capture_profile *bestCaptureProfile() const;
    const ppcp_capture_profile *bestPreviewProfile() const;

    bool openStream(const QString &streamId, const char *kind, const char *profileId,
                    ppcp_continuity continuity);
    void closeStream(const QString &streamId, const char *reason);

    void onCaptureAnnounce(const ppcp_msg *m);
    void onPayloadBegin(const ppcp_msg *m);
    void onPayloadChunk(const ppcp_msg *m);
    void onPayloadEnd(const ppcp_msg *m);
    void onPayloadAbort(const ppcp_msg *m);
    void deliver();
    void emitPreviewFrame(const PpcpClip &clip);

    ppcp_peer *m_peer = nullptr;
    QString    m_sessionId;
    QString    m_peerId;
    QString    m_sourceId;

    QString m_captureStreamId;
    QString m_previewStreamId;

    // capture id -> the Stream it named, from `capture_announce`.  A
    // `payload_begin` carries only a capture id (ENC §6), so the Stream — and
    // through it the profile, and through that the `timing` — is only reachable
    // through what the announce said.
    std::vector<std::pair<QString, QString>> m_captureStream;
    // capture ids on a preview Stream, so 5.11j is checked on the way in.
    std::vector<QString> m_previewCaptures;

    struct Open {
        bool            active = false;
        PpcpClip        clip;
        std::uint64_t   declaredBytes = 0;
        std::vector<std::int64_t> framesNs;
        std::vector<std::int64_t> exposureNs;
    } m_open;

    QVector<qint64> m_lastCanonicalNs;
    QString         m_lastCanonicalTb;
    // The geometry of the profile the last Capture's Stream named.  Copied
    // rather than pointed at: MSG 3.3a lets the counterpart replace its whole
    // declaration between the payload arriving and a consumer asking.
    bool            m_haveLastGeometry = false;
    ppcp_geometry   m_lastGeometry{};
    qint64          m_lastInstantSourceNs = 0;
    bool            m_haveLastInstant = false;

    bool   m_hasOffset = false;
    qint64 m_offsetNs  = 0;

    int      m_frameWidth = 0;
    int      m_frameHeight = 0;
    QString  m_previewPixelFormat;
    Counters m_counters;
};
