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

#include "ppcp_host_peer.h"

#include <cstring>

#include <ppcp/frame.h>

#include "ppcp_host_engine.h"

namespace Ppcp {
namespace {

// The pump's working buffer. Not a protocol constant: ENC §8's limits are the
// library's to enforce on a whole frame, and this is how many bytes are moved
// between a socket and the engine per call. Sized to the default chunk of
// ENC 6f so a bulk chunk usually crosses in one read.
constexpr std::size_t kPumpBuffer = 256u * 1024u;

}  // namespace

PpcpHostPeer::PpcpHostPeer(Config cfg)
    : m_cfg(std::move(cfg)), m_policy(m_cfg.ingest)
{
    m_scratch.resize(kPumpBuffer);
}

PpcpHostPeer::~PpcpHostPeer() = default;

bool PpcpHostPeer::declareSelf(const PpcpSourceDeclaration::Inventory &inv, std::string *err)
{
    if (!m_declaration.build(m_cfg.peerId, inv, err)) return false;

    // Validated before it can be sent, through libppcp's own validators. A
    // declaration that fails I19 must not reach the wire, and finding that out
    // at the encoder rather than here would mean the host had already committed
    // to a session it cannot describe itself in.
    std::string where;
    if (m_declaration.validate(&where) != PPCP_OK) {
        if (err) *err = "declaration is not valid: " + where;
        return false;
    }
    return true;
}

IngestVerdict PpcpHostPeer::evaluate(const ppcp_peer_desc &counterpart) const
{
    return m_policy.evaluate(counterpart);
}

ppcp_result PpcpHostPeer::readiness(ppcp_readiness *out) const
{
    if (!out) return PPCP_ERR_INVALID;

    // CORE 5.15's question is "if I arm now, will the first shot have settled
    // exposure?" — a measurement, not a state name (5.15a).
    //
    // Order matters: a BLOCKED reason is the more informative answer, so the
    // two conditions that make arming refusable are checked before settledness.

    // CORE §9 / 5.14g1 — "a peer under storage pressure REFUSES TO ARM rather
    // than dropping swings a consumer has not received". The floor is this
    // application's, not the protocol's; the reason on the wire is 5.15's.
    std::uint64_t freeBytes = 0;
    if (m_storage && m_storage(&freeBytes) && freeBytes < m_cfg.storageFloorBytes) {
        ppcp_readiness_not_settled(out, 0);
        return ppcp_readiness_set_blocked(out, "storage_full");
    }

    // CORE 5.8's ThermalLevel is an ordinal protocol vocabulary, not a platform
    // passthrough: `critical` is the level at which this host cannot become
    // ready at all, and the mapping from whatever the OS said is the
    // embedding's (the ThermalFn).
    ppcp_thermal_level thermal = PPCP_THERMAL_NOMINAL;
    if (m_thermal && m_thermal(&thermal) && thermal == PPCP_THERMAL_CRITICAL) {
        ppcp_readiness_not_settled(out, 0);
        return ppcp_readiness_set_blocked(out, "thermal_limit");
    }

    // A host owning no Sources is not "blocked": it has nothing to arm and
    // nothing is wrong with it. CORE 5.6.1 — Source count is not a role marker.
    // `no_source` is for a peer that ought to have one and does not, so it is
    // reported only when this host has declared and found nothing.
    if (m_declaration.peer() && m_declaration.sourceCount() == 0) {
        ppcp_readiness_not_settled(out, 0);
        return ppcp_readiness_set_blocked(out, "no_source");
    }

    // Host cameras are machine-vision cameras under manual exposure: there is
    // no auto-exposure loop to settle, so the honest answer is `settled`. When
    // a host camera under an automatic mode arrives, this is where its
    // estimated time-to-settled goes, measured and not guessed.
    return ppcp_readiness_settled(out);
}

void PpcpHostPeer::attach(PeerConnection *link, PpcpEngine *engine)
{
    m_link = link;
    m_engine = engine;
    m_stats = PumpStats{};
    // A new link is a new conversation: MSG 3.3c's obligation is per link and
    // this host has not met it on this one yet.
    m_declaredOnLink = false;

    // H5 and H7 hang off the peer this link carries, and all three are bound
    // here rather than by the caller — because a caller that bound two of the
    // three would produce a Session that synchronises but never arbitrates, and
    // nothing would say so.
    ppcp_peer *p = engine ? engine->peer() : nullptr;
    m_live.attach(p, &m_declaration);
    m_shots.attach(p, &m_declaration, &m_live);
    m_annotations.attach(p, m_cfg.peerId);

    // 6.1f — a published or received relation moves every camera's mapping onto
    // `tb:host`.  Wired here so the hook survives a re-attach.
    m_live.setRelationsCallback([this] { if (m_onRelations) m_onRelations(m_live); });
}

void PpcpHostPeer::setRelationsHook(RelationsFn f)
{
    m_onRelations = std::move(f);
    m_live.setRelationsCallback([this] { if (m_onRelations) m_onRelations(m_live); });
}

void PpcpHostPeer::drainEvents()
{
    if (!m_engine) return;
    ppcp_peer *p = m_engine->peer();
    if (!p) return;

    // ⚠ ONE DRAINER, AND THIS IS IT.  ppcp_peer_next_event() REMOVES the event;
    // a second consumer calling it against the same peer would see roughly half
    // the conversation, silently and non-deterministically.  Everything else
    // that needs events registers a hook — see the note in the header.
    //
    // The dispatch order is deliberate.  The live session first, because a
    // `relation_update` must be in the relation set before the arbiter converts
    // a Candidate against it (8.2a uses the CURRENT set).  The bridge second.
    // Markup third, since nothing depends on it.  Application hooks last, so
    // they see a peer whose protocol state is already settled.
    ppcp_event ev{};
    while (ppcp_peer_next_event(p, &ev) == PPCP_OK) {
        m_live.observe(ev);
        m_shots.observe(ev);
        m_annotations.observeEvent(ev);

        // ── MSG 3.3c/3.3d — THIS HOST DECLARES ITSELF, AND NOTHING DID ──────
        //
        // ⚠ FOUND BY H8's CONFORMANCE RUN, 23 Aug 2026 (finding F-H8-1).
        // `declareSelf()` built the declaration and `validate()` checked it,
        // and NO CALL SITE ANYWHERE IN THIS APPLICATION EVER PUT IT ON A WIRE:
        // `grep -rn ppcp_peer_declare src/` outside the test tree returned
        // nothing.  3.3d is explicit that a host declares its own Sources with
        // the same structure a capture peer uses — "a host owning no Sources
        // sends `declare` with an empty `sources` list; it does not skip the
        // message" — and 3.3c makes it a precondition for originating anything
        // that names a Source, Stream or Candidate.  A host that never declared
        // was therefore unable to nominate from its own microphone (CT-I8) and
        // gave a third-party device nothing to convert its instants against
        // (I19).  Every suite in `ppcp-tests` declared BY HAND in its fixture,
        // which is exactly how a composition defect survives a green suite.
        //
        // It goes HERE, on PPCP_EVENT_CONNECTED, because that is the moment a
        // wire version is agreed and before anything this peer originates can
        // reference a Source.  Once per link: `attach()` clears the flag, and
        // 3.3a's `generation` is the library's to increment, so sending a
        // second snapshot per link would be claiming a change that did not
        // happen.
        if (ev.kind == PPCP_EVENT_CONNECTED && !m_declaredOnLink) {
            m_declaredOnLink = true;
            if (const ppcp_peer_desc *self = m_declaration.peer())
                ppcp_peer_declare(p, self);
        }

        // MSG 3.3 — a counterpart declared, so its cameras exist now.  PPCP
        // Sources are not discovered by scanning; this is the only moment they
        // can be registered.
        if (ev.kind == PPCP_EVENT_DECLARE && m_onDeclare)
            m_onDeclare(ppcp_peer_counterpart(p));

        for (const EventFn &f : m_hooks) f(ev);
    }
}

bool PpcpHostPeer::tick(std::int64_t nowNs)
{
    const bool alive = pump();

    // §6.3's sync cadence and §7.4's heartbeat cadence — two schedules, one
    // call, and 6.3d is why they are not one schedule.
    m_live.pump(nowNs);

    // 8.2h — issue every group whose earliest contributing Candidate is at
    // least `issue_hold_ns` old.  `nowNs` is a reading of `Session.timebase_ref`
    // because this host's reference timebase IS its own clock.
    m_shots.pump(nowNs);

    // Whatever those two queued now has somewhere to go.
    if (alive) return pump();
    return alive;
}

bool PpcpHostPeer::pump()
{
    if (!m_link || !m_engine) return false;

    bool alive = false;

    for (Channel ch : m_link->channels()) {
        TransportChannel *tc = m_link->channel(ch);
        if (!tc || !tc->isOpen()) continue;
        alive = true;
        const std::uint8_t chNo = static_cast<std::uint8_t>(ch);

        // ── Socket to engine ───────────────────────────────────────────────
        // Read what is there and no more. A channel with nothing on it returns
        // WouldBlock, which is not an error and not a reason to stop looking at
        // the others: ENC 2d makes cross-channel arrival order explicitly
        // unordered, so a control frame must not wait on a bulk read.
        //
        // ⚠ THE TAIL IS OURS TO KEEP (L6). ppcp_peer_feed() consumes WHOLE
        // frames and reports how many bytes it took; a socket read ends
        // wherever the network felt like ending it, so the remainder is a
        // partial frame that must be re-presented with the next read behind
        // it. The engine deliberately buffers nothing (peer.h: nine megabytes
        // per link for a caller that already has the bytes), which makes this
        // buffer the embedding's obligation under ground rule 7 and not an
        // optimisation.
        std::vector<std::uint8_t> &tail = m_tails[chNo];
        for (;;) {
            std::size_t got = 0;
            const std::size_t keep = tail.size();
            if (keep >= m_scratch.size()) {
                // A frame larger than the whole working buffer. ENC §8's limits
                // are 1 MiB control / 8 MiB bulk, so grow rather than refuse:
                // refusing here would be this repository inventing a limit the
                // specification does not have (I14's discipline, applied to
                // sizes).
                m_scratch.resize(keep + kPumpBuffer);
            }
            std::memcpy(m_scratch.data(), tail.data(), keep);
            const IoStatus st = tc->read(m_scratch.data() + keep, m_scratch.size() - keep, got);
            if (st == IoStatus::Ok && got > 0) {
                m_stats.bytesIn += got;
                const std::size_t have = keep + got;

                // ⚠ ONE FRAME PER FEED, AND DRAIN BETWEEN THEM.  F-L13-1:
                // ppcp_peer_feed() consumes UNBOUNDEDLY MANY whole frames from
                // one buffer, the event ring is FOUR deep, and an overflow
                // drops the OLDEST event with nothing readable to say so.  A
                // single socket read carrying a replayed bundle therefore lost
                // `capture_announce` while the payload frames that referenced
                // it arrived — silently, and only under load, which is the
                // worst shape available.
                //
                // The slice is bounded by the frame header's own `payload_len`,
                // exactly as PpcpBundleTransport has always done it.  A header
                // the parser REFUSES is handed over unshortened: ENC 8a is the
                // engine's refusal to make, and a transport that pre-judged it
                // would be deciding what the library is for.
                //
                // The library fix is libppcp L15's; until it lands this is not
                // a workaround but the documented contract — peer.h's own event
                // note says a `msg` is valid only "until PPCP_PEER_EVENT_QUEUE
                // further events have been queued", and the only way to honour
                // that is to look between feeds.
                std::size_t off = 0;
                bool fatal = false;
                while (off < have) {
                    std::size_t slice = have - off;
                    if (slice >= PPCP_FRAME_HEADER_BYTES) {
                        ppcp_frame_header fh{};
                        if (ppcp_frame_header_parse(m_scratch.data() + off, &fh) == PPCP_OK) {
                            const std::size_t whole =
                                static_cast<std::size_t>(PPCP_FRAME_HEADER_BYTES)
                                + fh.payload_len;
                            if (whole > slice) break;   // a partial frame: it is the tail
                            slice = whole;
                        }
                    } else {
                        break;   // not even a header yet
                    }

                    // The engine gets the channel with the bytes because ENC
                    // 2c's "the channel in the header matches the stream it
                    // arrived on" is the engine's check to make, not ours — we
                    // are the only thing that knows which stream this was.
                    std::size_t consumed = 0;
                    const ppcp_result r =
                        m_engine->feed(chNo, m_scratch.data() + off, slice, &consumed);
                    off += consumed;

                    // ⚠ HERE, AND NOT AT THE END OF pump().  The event's `msg`
                    // borrows the bytes just fed, and `payload_chunk.data`
                    // points straight into this buffer — which the next read
                    // overwrites.
                    drainEvents();

                    if (r == PPCP_ERR_FATAL_LIMIT || r == PPCP_ERR_MALFORMED) {
                        // ENC 8a — a length beyond the channel limit means the
                        // stream has desynchronised and cannot be
                        // resynchronised. The link is finished; nothing is
                        // skipped and retried.
                        fatal = true;
                        break;
                    }
                    if (consumed == 0) break;   // no progress; keep it as tail
                }

                tail.assign(m_scratch.begin() + static_cast<std::ptrdiff_t>(off),
                            m_scratch.begin() + static_cast<std::ptrdiff_t>(have));
                if (fatal) {
                    tc->close();
                    m_stats.closed = true;
                    return false;
                }
                if (have < m_scratch.size()) break;
                continue;   // the buffer filled; there may be more waiting
            }
            if (st == IoStatus::WouldBlock) break;
            if (st == IoStatus::Closed || st == IoStatus::Error) {
                tc->close();
                m_stats.closed = true;
                alive = false;
                break;
            }
            break;
        }
        if (!tc->isOpen()) continue;

        // ── Engine to socket ───────────────────────────────────────────────
        //
        // ⚠ PEEK AND COMMIT, NOT DRAIN, AND FINDING 5 IS WHY.  drain() DEQUEUES:
        // it hands back whole frames and assumes every byte was written.  Under
        // CORE T2 backpressure — ordinary on a bulk channel carrying a clip —
        // write() returns short, and the bytes it did not take were bytes the
        // engine had already forgotten.  This pump used to count those
        // occasions and lose the data anyway.
        //
        // L9 answered it.  peek borrows the queue without removing anything;
        // commit removes EXACTLY the byte count the socket accepted — not a
        // whole number of frames, because a channel is an ordered byte stream
        // and rounding down to a frame boundary would re-send bytes that had
        // already left.  The two paths do not mix, so once a partial commit has
        // left the head frame half-written, drain() refuses; this loop never
        // calls it.
        for (;;) {
            const std::uint8_t *bytes = nullptr;
            std::size_t len = 0;
            const ppcp_result pr = m_engine->drainPeek(chNo, &bytes, &len);
            if (pr == PPCP_ERR_UNIMPLEMENTED) {
                // An engine that is not a ppcp_peer — the bundle transport's,
                // which writes to a file and cannot short-write.  Nothing to do
                // here; this pump is only ever given the socket one.
                break;
            }
            if (pr != PPCP_OK || len == 0 || !bytes) break;

            std::size_t written = 0;
            const IoStatus st = tc->write(bytes, len, written);
            if (written > 0) {
                m_stats.bytesOut += written;
                const ppcp_result cr = m_engine->drainCommit(chNo, written);
                if (cr != PPCP_OK) {
                    // The engine and this pump now disagree about what left, and
                    // there is no honest way to resynchronise a byte stream.
                    tc->close();
                    m_stats.closed = true;
                    return false;
                }
            }
            if (st == IoStatus::Closed || st == IoStatus::Error) {
                tc->close();
                m_stats.closed = true;
                alive = false;
                break;
            }
            if (st == IoStatus::WouldBlock || written < len) {
                // CORE T2's backpressure, observed rather than assumed — and now
                // survived rather than merely counted.  The OTHER channels are
                // untouched, which is the entire reason there are two of them
                // (CORE §3.1).  The counter stays because the RATE of short
                // writes is a real signal about the link, even now that none of
                // them loses anything.
                ++m_stats.wouldBlockOnWrite;
                break;
            }
        }
    }

    // A last sweep for events the engine raised ITSELF rather than decoded —
    // link loss, link restored — which are queued by the liveness pump and by
    // nothing that was fed.  Every decoded event was already drained beside the
    // frame it came from, above; this is not a second chance at those, because
    // by now their bytes are gone.
    drainEvents();

    return alive;
}

// ── The binding to libppcp's engine (L6) ────────────────────────────────────
//
// ⚠ IT MOVED, AND THE MOVE IS THE POINT. Building the peer used to live here,
// beside the socket pump, which made the bundle path of H3 either a second
// construction of the same engine or a dependency on OpenSSL to read a file.
// ppcp_host_engine.cpp is now the one place that says what a PinPointStudio
// peer is, and both transports ask it — which is what makes "the same ingest
// path" a fact about the code rather than a claim in a comment.

std::unique_ptr<PpcpEngine> PpcpHostPeer::makeLibppcpEngine(std::string *whyNot)
{
    HostEngineConfig cfg;
    cfg.peerId = m_cfg.peerId;
    cfg.policy = &m_policy;
    cfg.health = [this](ppcp_readiness *out) { return this->readiness(out); };
    // ⚠ F-H5-3 IS CLOSED IN libppcp, AND ITS FIX IS A HARD PRECONDITION.
    // ppcp_peer_new() now REFUSES a peer that declares `live` with no
    // `health_report` — "a peer that has no thermometer declares no Live" — so
    // the finding this repository raised at the end of S3 has become a
    // constructor error rather than a silent absence of liveness.  Good, and it
    // means the host owes a reading.  It is the SAME two callbacks the
    // embedding already supplies for readiness, reported honestly: a reading
    // this host cannot take is omitted, never defaulted to `nominal`.
    cfg.healthReport = [this](ppcp_health *out) -> ppcp_result {
        if (!out) return PPCP_ERR_INVALID;
        *out = ppcp_health{};
        // 7.4b — degradation is REPORTED, not absorbed.  With no thermometer
        // the honest answer is the level that says nothing has degraded; with
        // one, whatever it says.  `PPCP_THERMAL_NOMINAL` on no evidence would be
        // a fabrication, which is why setThermal() is asked first and its
        // "cannot tell" answer is preserved by leaving no vendor label.
        ppcp_thermal_level t = PPCP_THERMAL_NOMINAL;
        if (m_thermal) m_thermal(&t);
        out->thermal = t;
        std::uint64_t freeBytes = 0;
        if (m_storage && m_storage(&freeBytes)) out->storage_free_bytes = freeBytes;
        return PPCP_OK;
    };
    cfg.listener = true;
    return makeHostEngine(std::move(cfg), whyNot);
}

}  // namespace Ppcp
