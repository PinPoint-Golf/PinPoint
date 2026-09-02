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
#include <QVariantList>
#include <QVariantMap>
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

    // I34 — a Capture's identity is `Capture.id` SCOPED BY `Session.id` and the
    // minting `Peer.id`, and all three are needed to key it.  Without this a
    // consumer would have to invent a scope, and two Sessions from one phone
    // could collide on a capture id the device was entitled to reuse.
    QString sessionId;

    // I27 / ENC 4.1d — a Capture is anchored to EXACTLY ONE key, and where that
    // key is a Shot this is its id.  It is the only honest way to say which shot
    // a clip belongs to: the alternative is matching on arrival order or on
    // (peer, source), and both are guesses that a second phone or a re-request
    // breaks.  Empty when the Capture is anchored to a Candidate or to its own
    // Stream rather than to a Shot.
    QString shotId;
    // CORE 5.14 `Capture.digest` as the announce carried it, hex; empty where
    // the owner had not computed one.  8.4a's `capture_committed` sends it
    // back, and libppcp refuses a commit without it -- so a clip filed without
    // this stays "owed" for ever.
    QString digestHex;

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

    // ⛔ ENC 6g / erratum E7 — THE PAYLOAD'S CONTAINER, AND THE ONLY LEGITIMATE
    // SOURCE OF A FILE EXTENSION.  An IANA media type ("video/quicktime"),
    // carried on `payload_begin` and REQUIRED whenever the bytes are a
    // container-framed file.  ENC 6h forbids a receiver inferring one from
    // `format.codec`, from `Stream.kind`, or by sniffing the bytes -- so a
    // consumer that writes this clip to disk MUST name the file from here or
    // not name it at all.
    //
    // Empty for raw samples the Stream's profile describes in full, and empty
    // for `absent`.  ⚠ The import path has read this since E7
    // (ppcp_import_sink.cpp); the live path dropped it on the floor until now,
    // and a stale comment in this file's deliver() claimed ENC §6 declared no
    // container at all.
    QString container;

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

    // ⛔ **A PREVIEW CONSUMER THAT DOES NOT WAIT FOR A PANEL TO OPEN.**
    // start() is a *tile*: it opens the capture Stream too and refuses outright
    // if the Source declares no capture profile.  This opens the `preview`
    // Stream and nothing else, so PpcpHostService can hold one per camera
    // Source from `declare` until the phone goes — which is what makes a
    // picture available "the instant you connect" rather than the instant
    // somebody opens Settings.
    //
    // ⚠ It exists because nothing was listening.  dispatchEvent() broadcasts to
    // LIVE instances, and until 27 Aug the only thing that ever constructed one
    // was the crop editor: every preview Capture announced before an operator
    // opened that panel was dropped on the floor, and so was the `stream_close`
    // that said preview had stopped.  Returns false, quietly and without an
    // error state, for a Source that declares no preview profile — 5.11.2 makes
    // that a conformant answer and not a fault.
    bool               startPreviewOnly();
    bool               isPreviewOnly() const { return m_previewOnly; }

    // The two calls PpcpHostService makes to keep one preview consumer per
    // camera Source alive for a phone's connection.
    //
    // ⚠ **Static, like every other seam this class offers the Ppcp layer.**
    // `ppcp_host_service.cpp` reaches `src/Video` through free/static functions
    // only, so `ppcp_host_service_test` can stub the lot without linking the
    // camera backends (see ppcp_host_service_stubs.cpp).  Constructing an
    // instance there would drag a QObject vtable and its moc output into a
    // suite whose whole subject is the pairing code's clock.
    //
    // startPreviewConsumers() appends one live instance per camera Source that
    // declares a preview profile and returns how many; it skips Sources that
    // decline preview, which 5.11.2 makes conformant.
    static int  startPreviewConsumers(QObject *parent, ppcp_peer *peer,
                                      const QString &peerId, const QString &sessionId,
                                      const ppcp_peer_desc *desc,
                                      std::vector<VideoInputPpcp *> *out);
    // ⛔ Call while the peer is still alive: stop() closes the preview Stream,
    // which is a wire message (5.1a1 — say why).
    static void stopPreviewConsumers(std::vector<VideoInputPpcp *> *consumers);
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

    // The PPCP-wire Stream id start()/resume() open under — "st:<hash of
    // peerId+sourceId>:<kind>", kind being "video" or "preview". Exposed so a
    // caller (a test correlating a Capture's `stream` field, chiefly) can
    // compute the exact id this class will use, instead of hand-deriving it
    // and drifting the moment the format changes — see the 64-byte
    // PPCP_ID_MAX comment beside the implementation.
    static QString streamIdFor(const QString &peerId, const QString &sourceId,
                                const QString &kind);

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
        // Reassemblies thrown away to make room (see kMaxOpenPayloads).  Three
        // channels carry at most three at once, so anything here at all says
        // the table is leaking, and a test can say so.
        std::size_t payloadsEvicted = 0;
        // Chunks and ends that matched no open reassembly.  Non-zero means a
        // payload's begin was never opened here, or was closed early.
        std::size_t chunksUnmatched = 0;
        std::size_t endsUnmatched   = 0;
        std::size_t absentSegments  = 0;     // 5.11c3 — the honest account
        // CT-I36a, host as consumer.  5.11j: "a consumer therefore never sees
        // `transfer: pending` on a preview Capture".  Counted rather than
        // assumed away, because the engine does NOT catch it on the receiving
        // side — see the note in the .cpp.
        std::size_t previewPendingRefused = 0;
        // Captures announced on a Stream the DEVICE opened for this Source
        // rather than one this host opened.  MSG 5.1c is silent on who opens a
        // Stream in a hosted Session, so this is a legal shape and not an
        // anomaly — counted because "the device opens its own" and "the host
        // opens them" are two different deployments and this is the only place
        // that can tell them apart.
        std::size_t foreignStreamCaptures = 0;
        // MSG 5.1 — the owner's verdict on a Stream this host REQUESTED.  A
        // refusal is a conformant answer (5.11l makes one mandatory for a
        // consumer selecting a preview profile for capture), so it is counted
        // rather than treated as a fault.
        std::size_t streamsOpened  = 0;
        std::size_t streamsRefused = 0;
        // 5.1d / 5.11i — the OWNER closed a Stream we were reading.  Conformant
        // behaviour (a hot phone drops its preview first), so counted rather
        // than treated as a fault.
        std::size_t streamsClosedByOwner = 0;
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

    // ── 6.1f, and the join it needs ────────────────────────────────────────
    // "A `relation_update` was published or received, so every VideoInputPpcp
    // bound to this peer can be re-fed its offset."  The owner of the LINK has
    // the relation; the owner of the CAMERAS is CameraManager, several layers
    // away and holding VideoInputBase pointers it has no reason to downcast.
    // So the live instances keep a register of themselves and the link's owner
    // pushes to it by `Peer.id`.  Returns how many instances were re-fed.
    //
    // ⚠ AND THE SEAM STILL CANNOT CARRY THE WHOLE RELATION.  setTimebaseOffsetNs
    // takes a SCALAR and a TimebaseRelation is affine, so this is the relation
    // evaluated at one instant and the skew term goes stale at the rate it was
    // measured.  That is why it is re-fed on every publish rather than set
    // once, and why PpcpLiveSession::offsetToRefNs() returns an uncertainty
    // beside the offset.  Widening it to take a relation is a change to this
    // class and is not made blind.
    // `lookup` is asked for each live instance's OWN Source timebase (5.6a),
    // because a peer with a camera clock and an audio clock has one relation
    // per clock and a single scalar for the peer would be a fabrication for
    // whichever Source it did not describe.  A lookup that answers false — no
    // direct relation, or `unrelated` (5.4b, 8.2i1) — CLEARS that instance's
    // mapping rather than leaving a stale one, because a stale offset is
    // shaped exactly like drift and is indistinguishable downstream from a
    // measured one.
    using TimebaseOffsetLookup = std::function<bool(const QString &timebaseId, qint64 *outNs)>;
    static int applyTimebaseOffsets(const QString &peerId, const TimebaseOffsetLookup &lookup);
    static int clearTimebaseMappings(const QString &peerId);
    static int liveInstanceCount(const QString &peerId);

    // ── The second-consumer event path ──────────────────────────────────────
    // For a peer a `PpcpHostPeer` already pumps: this class's own
    // drainEvents() MUST NOT be called on it (exactly one drainer).  The
    // embedding instead feeds each event here as its single drainer's
    // `addEventHook` sees it, broadcast to every live instance bound to
    // `peerId` — same registry and filter as applyTimebaseOffsets() above.
    static int dispatchEvent(const QString &peerId, const ppcp_event &ev);

    // The peer went away at the transport's own disconnect.  detach()es every
    // live instance bound to `peerId` so none is left holding a pointer the
    // caller is about to destroy.  Same registry and filter as
    // clearTimebaseMappings() above.
    static int detachAll(const QString &peerId);

    // Every live instance's counters for one peer, as data.  The question this
    // answers is "where does the preview stop?", and it answers it in order:
    // no instance, no Capture, no frame, or a frame that would not decode.
    // Machine-readable because a log line a release build does not emit is no
    // use to somebody standing in a bay with a phone.
    static QVariantList countersFor(const QString &peerId);

    // ── CORE 5.11.2 — preview from the moment the link is up ────────────────
    //
    // Requests a `kind: preview` Stream for every camera Source `desc` declares
    // that offers a preview profile (`intrinsics: none`, 5.11m).  Returns how
    // many were asked for.
    //
    // ⚠ CALLED WHEN THE PHONE DECLARES, NOT WHEN A TILE OPENS.  5.11.2 says
    // "opening one is an ordinary `stream_open` from the consumer that wants
    // it", and names preview-alone "during setup and framing" as its MAIN USE.
    // A preview that appears only once capture has started cannot do the job it
    // exists for: by then the framing decision has been made blind.
    //
    // ⛔ AND THE CONSUMER MAY OWN THIS ONE.  The argument that a device must
    // originate `stream_open` — ENC 7a/7b, a host-opened Stream cannot appear
    // in the device's bundle — does not reach preview: 5.11j makes a preview
    // Capture live-only and never written to a bundle, so there is no file to
    // keep consistent.
    //
    // Ids are `streamIdFor(peer, source, "preview")`, so whichever object opens
    // one, every other names the same Stream rather than opening a second.
    static int openPreviewStreams(ppcp_peer *peer, const QString &peerId,
                                  const QString &sessionId, const ppcp_peer_desc *desc);
    // The counterpart declared again — a reconnection.  Re-attaches and
    // re-opens the Streams of every instance detachAll() stopped that was
    // RUNNING at the time, and leaves the rest alone.  Returns how many came
    // back.  See the note beside the implementation for why nothing else in
    // this application could do it.
    static int reattachAll(const QString &peerId, ppcp_peer *peer,
                           const QString &sessionId);

    // CORE 5.6a — the Timebase this Source stamps in.  Empty until the
    // counterpart has declared, which is also when the offset seam can first
    // mean anything.
    QString timebaseId() const;

    // ── "Config from PPS" — which declared CaptureProfile this camera uses ──
    //
    // The one thing that question turned out to mean, and the protocol has
    // carried it all along: `stream_open` names any declared `profile_id`, and
    // every profile a peer declares is already parsed into `CameraCapabilities`
    // above.  Only the choosing was missing.
    //
    // Empty restores the automatic pick (the fastest non-preview profile).
    // ⚠ Applied at the next `stream_open`, never to a Stream that is running:
    // 5.1a fixes a Stream's identity for its life, and its profile with it.
    void setPreferredCaptureProfile(const QString &profileId);
    const QString &preferredCaptureProfile() const { return m_preferredProfileId; }

    // Every non-preview profile this peer declared, in declaration order, with
    // their nominal rates in Hz where `outRatesHz` is given (0.0 for a profile
    // that declared no rate — which is legal, and different from "0 fps").
    QStringList declaredCaptureProfiles(double *outRatesHz = nullptr,
                                        int maxRates = 0) const;
    // The declared profile nearest `fps` from below, for a caller whose control
    // is a frame rate rather than a profile id.  Empty where none is declared.
    QString profileForRate(double fps) const;

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
    // Which internal step openStream() refused at, and libppcp's own word for
    // why — start()'s "stream_open refused" was otherwise a dead end to
    // diagnose from outside this class.  Cleared at the top of every
    // openStream() call, so it only ever reflects the most recent attempt.
    QString m_lastStreamOpenError;

    // Stops every OTHER live instance already active for this exact
    // (peerId, sourceId) AND attached to the SAME live `peer` — see the note
    // at its call site in start().  The peer check matters: two different
    // ppcp_peer connections can coincidentally share a peerId/sourceId
    // string (any test harness that hardcodes both ends' ids is exactly
    // this), and those are not the race this exists to resolve — only two
    // instances that would otherwise both be talking to the SAME phone are.
    // A no-op when no such sibling exists (the ordinary case).
    static void reclaimStream(const QString &peerId, const QString &sourceId,
                              ppcp_peer *peer, VideoInputPpcp *keep);

    // The dispatch one drained event gets — factored out of drainEvents() so
    // dispatchEvent() (a second-consumer path, see below) can hand an event
    // to a specific instance one at a time, in the same shape.
    void processEvent(const ppcp_event &ev);

    void onStreamOpenAck(const ppcp_msg *m);
    void onStreamClose(const ppcp_msg *m);
    void onCaptureAnnounce(const ppcp_msg *m);
    void onPayloadBegin(const ppcp_msg *m);
    void onPayloadChunk(const ppcp_msg *m);
    void onPayloadEnd(const ppcp_msg *m);
    void onPayloadAbort(const ppcp_msg *m);
    void deliver(const QString &captureId);
    void emitPreviewFrame(const PpcpClip &clip);

    ppcp_peer *m_peer = nullptr;
    QString    m_sessionId;
    QString    m_peerId;
    QString    m_sourceId;

    // Set by startPreviewOnly().  ⚠ Read by reclaimStream(): a preview-only
    // instance owns no capture Stream, so it collides with nobody and must
    // neither evict a tile nor be evicted by one.
    bool    m_previewOnly = false;
    QString m_captureStreamId;
    QString m_previewStreamId;
    // ⛔ DID THIS INSTANCE OPEN THE PREVIEW STREAM, OR MERELY ADOPT IT?  There
    // is ONE preview Stream per Source and several consumers of it — the host's
    // own, alive from `declare`, and any tile an operator opens — because 5.1a
    // fixes a Stream's identity for its life and `onCaptureAnnounce()` resolves
    // by `source_id` rather than by who opened what.  The open side of that has
    // been right for a while (`ppcp_peer_stream_find` before `openStream`); the
    // CLOSE side was not, and `stop()` shut the shared Stream whenever any
    // consumer finished with it.  The owner then saw the device's
    // `stream_close` and reported "the device closed the preview stream
    // (not_needed)" for a Stream this host had asked to close — which is what a
    // crop editor closing (or its delegate being rebuilt) did to a live preview,
    // reported 31 Aug 2026 on a cabled phone.
    bool    m_previewOwned = false;
    // Empty means "the fastest declared", which is what every instance did
    // before an operator could say otherwise.
    QString m_preferredProfileId;
    // The OWNER's `opened_at`, on the Stream's own timebase — theirs, not ours.
    // -1 until the ack arrives, which is a different answer from 0.
    qint64  m_captureOpenedAtNs = -1;
    qint64  m_previewOpenedAtNs = -1;
    // Set by detach() when a RUNNING instance lost its peer, so a reconnection
    // resumes exactly what was running and nothing else.
    bool    m_resumeOnReattach = false;

    // capture id -> the Stream it named, from `capture_announce`.  A
    // `payload_begin` carries only a capture id (ENC §6), so the Stream — and
    // through it the profile, and through that the `timing` — is only reachable
    // through what the announce said.
    std::vector<std::pair<QString, QString>> m_captureStream;
    // capture id -> the SHOT it is anchored to (I27), from the same announce.
    // `payload_begin` carries neither, so both are only reachable through what
    // the announce said, and a consumer needs the shot to know which swing the
    // bytes belong to.
    std::vector<std::pair<QString, QString>> m_captureShot;
    // capture id -> the announced digest, hex (see PpcpClip::digestHex).
    std::vector<std::pair<QString, QString>> m_captureDigest;
    // capture id -> the announced completeness.  ⛔ Without this a partial clip
    // was filed as `complete`: PpcpClip's default, never overwritten (2 Sept).
    std::vector<std::pair<QString, ppcp_completeness>> m_captureCompleteness;
    // capture ids on a preview Stream, so 5.11j is checked on the way in.
    std::vector<QString> m_previewCaptures;

    struct Open {
        bool            active = false;
        PpcpClip        clip;
        std::uint64_t   declaredBytes = 0;
        // ⚠ Kept from `payload_begin` and CONVERTED AT `payload_end`, because
        // the conversion needs the Stream's profile and the Stream is named by
        // the announce -- which travels on the control connection while the
        // bytes travel on bulk, and the two are not ordered against each other.
        // ⛔ DEEP-COPIED.  `ppcp_achieved_frames` holds pointers into the
        // engine's decode arena, which is reset by the NEXT frame fed; a copy
        // of the struct is a copy of dangling pointers by the time `payload_end`
        // arrives.
        bool                      hasAchievedFrames = false;
        QString                   framesTb;
        std::vector<std::int64_t> framesNs;
        std::vector<std::int64_t> exposureNs;      // empty = absent
        std::vector<std::int64_t> isoValues;       // empty = absent
        std::vector<ppcp_matrix3> intrinsics;      // empty = absent
        ppcp_exposure_provenance  exposureProvenance = PPCP_EXP_PER_FRAME;
    };
    // Fills `streamId`, `shotId` and `preview` from what the announces said, or
    // leaves them empty where no announce for this Capture has been read yet.
    void resolveAnnounce(Open &open) const;
    // ⛔ ONE REASSEMBLY PER CAPTURE, NOT ONE PER INSTANCE.  `CORE` §2's whole
    // transport argument is that a clip on bulk must not head-of-line block
    // the preview channel, so their payloads INTERLEAVE on the wire by design.
    // A single slot here therefore lost every clip: measured 1 Sept 2026, the
    // swing's `payload_begin` landed on bulk and the next preview segment's
    // `payload_begin` replaced it "after 0 of 13126842 bytes" -- the 13 MB
    // then arrived chunk by chunk for a Capture nothing was collecting, and
    // the filer saw only the preview.  Keyed by capture id because that is
    // what every chunk and end carry (ENC 6b).
    std::vector<Open> m_openPayloads;
    std::int64_t      m_lastTableDumpMs = 0;
    // Three channels can each carry one payload at a time, so this is
    // generous; beyond it the oldest PREVIEW reassembly goes first, and a
    // clip is evicted only with a warning that names it.
    static constexpr std::size_t kMaxOpenPayloads = 32;
    Open *openPayload(const QString &captureId);
    void  closePayload(const QString &captureId);

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
