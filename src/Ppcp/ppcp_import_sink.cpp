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

#include "ppcp_import_sink.h"

#include <cstdio>
#include <filesystem>

namespace Ppcp {
namespace {

std::string idStr(const ppcp_id &id)
{
    return std::string(id.v, id.len);
}

std::string hex(const ppcp_digest &d)
{
    if (!d.present) return {};
    static const char *k = "0123456789abcdef";
    std::string out;
    out.reserve(PPCP_SHA256_BYTES * 2);
    for (unsigned i = 0; i < PPCP_SHA256_BYTES; ++i) {
        out.push_back(k[d.value[i] >> 4]);
        out.push_back(k[d.value[i] & 0x0F]);
    }
    return out;
}

Completeness completenessOf(ppcp_completeness c)
{
    switch (c) {
    case PPCP_COMPLETE: return Completeness::Complete;
    case PPCP_ABSENT:   return Completeness::Absent;
    default:            return Completeness::Partial;
    }
}

// An id may legally contain characters a filesystem will not take: CORE 5.1a
// makes an Id opaque, and "opaque" includes `/`.  So the FILE name is sanitised
// and the LEDGER keeps the real id — never the other way round, because the
// sanitised form is lossy and I34 keys on the real one.
std::string sanitise(const std::string &id)
{
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out.push_back(ok ? c : '-');
    }
    if (out.empty()) out = "capture";
    return out;
}

// ── THE GAP THIS FILE REPORTED IS CLOSED: `ENC` 6g, erratum E7 ─────────────
//
// It used to say here that `payload_begin` carried `bytes`, a `digest`, a
// `chunk_bytes` and nothing that said what the bytes ARE — that `format.codec`
// is a CODEC ("hevc") three hops away through Source → CaptureProfile → Stream
// → Capture, and that a receiver writing a clip to disk therefore had to guess
// its extension from the Stream kind.  That was reported and E7 took it:
// `payload_begin.container` is an IANA media type, REQUIRED wherever the bytes
// are container-framed, and 6h forbids inferring one from `format.codec`, from
// `Stream.kind`, or by sniffing.
//
// So the declared container is used where there is one.  The Stream-kind table
// survives only for the case 6g leaves open — raw samples the profile describes
// in full, where there IS no container — and it is now a fallback with a
// clause behind it rather than a guess.
const char *extensionForContainer(const std::string &mediaType)
{
    if (mediaType == "video/mp4")         return ".mp4";
    if (mediaType == "video/quicktime")   return ".mov";
    if (mediaType == "video/x-matroska")  return ".mkv";
    if (mediaType == "audio/mp4")         return ".m4a";
    if (mediaType == "audio/wav" || mediaType == "audio/x-wav" ||
        mediaType == "audio/vnd.wave")    return ".wav";
    if (mediaType == "audio/aac")         return ".aac";
    if (mediaType == "application/octet-stream") return ".bin";
    return nullptr;   // known to be container-framed, unknown to this table
}

// ⚠ 6h — A FALLBACK, NOT AN INFERENCE, AND THE DIFFERENCE MATTERS.  Naming a
// file is not deciding what it contains: nothing downstream reads the extension
// to choose a demuxer.  Where the peer declared a container this host does not
// recognise, the subtype is used verbatim rather than mapped to something it
// might not be.
const char *extensionForStreamKind(const std::string &streamKind)
{
    if (streamKind == PPCP_STREAM_KIND_VIDEO)   return ".mp4";
    if (streamKind == PPCP_STREAM_KIND_PREVIEW) return ".mp4";
    if (streamKind == PPCP_STREAM_KIND_AUDIO)   return ".m4a";
    return ".bin";
}

}  // namespace

std::string PpcpImportSink::extensionForPayload(const std::string &container,
                                                const std::string &streamKind)
{
    if (container.empty()) return extensionForStreamKind(streamKind);
    if (const char *known = extensionForContainer(container)) return known;
    const std::size_t slash = container.find('/');
    const std::string sub = (slash == std::string::npos) ? container
                                                         : container.substr(slash + 1);
    return sub.empty() ? std::string(".bin") : "." + sanitise(sub);
}

namespace {
// One name for the same thing, so the call sites below read unchanged.
std::string extensionFrom(const std::string &container, const std::string &streamKind)
{
    return PpcpImportSink::extensionForPayload(container, streamKind);
}
}  // namespace

PpcpImportSink::PpcpImportSink(PpcpImportLedger &ledger, ppcp_peer *peer, Config cfg)
    : m_ledger(ledger), m_peer(peer), m_cfg(std::move(cfg))
{
    // I34 — what this host already holds, in the library's own index, so the
    // rule that decides "already imported" is libppcp's for both applications.
    m_ledger.seedIndex(&m_index);
}

PpcpImportSink::~PpcpImportSink()
{
    if (m_open.file) std::fclose(static_cast<std::FILE *>(m_open.file));
}

std::string PpcpImportSink::sessionDir()
{
    if (!m_stats.sessionDir.empty()) return m_stats.sessionDir;
    if (m_cfg.importRoot.empty() || m_stats.sessionId.empty()) return {};

    std::filesystem::path p(m_cfg.importRoot);
    p /= sanitise(m_stats.ownerPeerId.empty() ? "unknown-peer" : m_stats.ownerPeerId);
    p /= sanitise(m_stats.sessionId);
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    if (ec) return {};
    m_stats.sessionDir = p.string();
    return m_stats.sessionDir;
}

std::string PpcpImportSink::clipPath(const std::string &captureId,
                                    const std::string &container) const
{
    auto it = m_captureStream.find(captureId);
    std::string kind;
    if (it != m_captureStream.end()) {
        auto k = m_streamKind.find(it->second);
        if (k != m_streamKind.end()) kind = k->second;
    }
    std::filesystem::path p(m_stats.sessionDir);
    p /= sanitise(captureId) + extensionFrom(container, kind);
    return p.string();
}

void PpcpImportSink::onCapture(const ppcp_msg *m)
{
    const ppcp_capture &c = m->body.capture_announce.capture;
    ++m_stats.captures;

    // The Session an announcement belongs to travels in the ENVELOPE (MSG §5),
    // not in the body — so a bundle whose frames omit it falls back to the
    // Session this walk opened, which is the only one a bundle may contain.
    std::string sessionId = m->env.has_session_id ? idStr(m->env.session_id)
                                                  : m_stats.sessionId;
    if (sessionId.empty()) sessionId = m_stats.sessionId;

    CaptureKey key{ m_stats.ownerPeerId, sessionId, idStr(c.id) };
    if (!key.valid()) {
        // No `declare` has been met, so there is no MINTING peer and I34's
        // identity has no second scope. Recorded, not guessed: keying it on the
        // session alone would collide the moment two devices used the same
        // Capture ids, which is exactly what an opaque id permits.
        ++m_stats.capturesUnattributable;
        return;
    }

    m_captureStream[key.captureId] = idStr(c.stream_id);

    // I34 — the decision, made by the library's index and not by this file.
    ppcp_capture_key lk{};
    bool isNew = false;
    if (ppcp_id_set_z(&lk.session_id, key.sessionId.c_str()) != PPCP_OK
        || ppcp_id_set_z(&lk.peer_id, key.peerId.c_str()) != PPCP_OK
        || ppcp_id_set_z(&lk.capture_id, key.captureId.c_str()) != PPCP_OK)
        return;
    if (ppcp_capture_index_observe(&m_index, &lk, &isNew) != PPCP_OK) return;
    if (!isNew) {
        ++m_stats.capturesAlreadyHeld;
        return;   // "a no-op, never a duplicate"
    }

    PpcpImportLedger::CaptureRecord rec;
    rec.key = key;
    rec.digestHex = hex(c.digest);      // empty for `absent`, and for `pending`
    rec.completeness = completenessOf(c.completeness);
    switch (m_ledger.admit(rec)) {
    case PpcpImportLedger::Admission::Recorded:       ++m_stats.capturesNew; break;
    case PpcpImportLedger::Admission::AlreadyHeld:    ++m_stats.capturesAlreadyHeld; break;
    case PpcpImportLedger::Admission::DigestConflict: ++m_stats.digestConflicts; break;
    }
}

void PpcpImportSink::onPayloadBegin(const ppcp_msg *m)
{
    if (m_open.file) {
        std::fclose(static_cast<std::FILE *>(m_open.file));
        m_open = OpenPayload{};
    }
    if (!m_cfg.writeClips) return;
    if (sessionDir().empty()) return;

    m_open.captureId = idStr(m->body.payload_begin.capture_id);
    m_open.declaredBytes = m->body.payload_begin.bytes;
    // ENC 6g / MSG 8.3h (E7) — the container the SENDER declared, carried
    // through to where the bytes land.  Absent means raw samples the Stream's
    // profile describes in full, which is the only case 6g leaves open.
    m_open.container = m->body.payload_begin.has_container
                           ? idStr(m->body.payload_begin.container) : std::string();
    if (!m_open.container.empty()) ++m_stats.payloadsWithContainer;
    m_open.path = clipPath(m_open.captureId, m_open.container);
    m_open.file = std::fopen(m_open.path.c_str(), "wb");
}

void PpcpImportSink::onPayloadChunk(const ppcp_msg *m)
{
    const ppcp_body_payload_chunk &b = m->body.payload_chunk;
    if (!m_open.file || idStr(b.capture_id) != m_open.captureId) return;
    if (b.data == nullptr || b.data_len == 0) return;
    // ENC 6b — `offset` is index x chunk_bytes and is carried on every chunk, so
    // a chunk that arrives out of order is placed rather than appended.  In a
    // bundle they are in order; seeking anyway is what makes the same code the
    // socket path's.
    std::FILE *f = static_cast<std::FILE *>(m_open.file);
    if (std::fseek(f, static_cast<long>(b.offset), SEEK_SET) != 0) return;
    const std::size_t wrote = std::fwrite(b.data, 1, b.data_len, f);
    m_open.written += wrote;
    m_stats.clipBytes += wrote;
}

void PpcpImportSink::onPayloadEnd(const ppcp_msg *m)
{
    if (!m_open.file) return;
    const std::string capId = idStr(m->body.payload_end.capture_id);
    std::FILE *f = static_cast<std::FILE *>(m_open.file);
    std::fflush(f);
    std::fclose(f);
    m_open.file = nullptr;

    if (capId != m_open.captureId) { m_open = OpenPayload{}; return; }
    ++m_stats.clipsWritten;

    const CaptureKey key{ m_stats.ownerPeerId, m_stats.sessionId, capId };
    m_ledger.setLocalPath(key, m_open.path, hex(m->body.payload_end.digest));

    // CORE 5.14h — the payload is on disk and flushed, which is what "durably
    // commits" means (MSG 8.4a).  8.4b makes this the ONLY route to `confirmed`
    // on the offline path, and 5.14h1 says a closed Session is no reason to
    // withhold it: without this the owning device can never evict the clip and
    // its storage fills across a season.
    m_ledger.queueCommitted(key, hex(m->body.payload_end.digest));
    ++m_stats.commitsQueued;

    m_open = OpenPayload{};
}

void PpcpImportSink::drainEvents()
{
    if (!m_peer) return;
    ppcp_event ev{};
    while (ppcp_peer_next_event(m_peer, &ev) == PPCP_OK)
        observeEvent(ev);
}

// ⚠ THE LIVE PATH CANNOT DRAIN, AND THAT IS WHY THIS IS SEPARATE.  On a socket
// the event ring has exactly ONE drainer — `PpcpHostPeer` — and a second
// consumer calling `ppcp_peer_next_event()` would steal frames from the first.
// So the live embedding registers an event hook and hands each event here,
// exactly as `PpcpAnnotationStore::observeEvent()` is fed.  `drainEvents()`
// above remains the offline bundle path, where this class IS the only drainer.
void PpcpImportSink::observeEvent(const ppcp_event &ev)
{
    {
        const ppcp_msg *m = ev.msg;
        if (!m) return;
        switch (ev.kind) {
        case PPCP_EVENT_DECLARE:
            // The bundle's MINTING peer (I34's second scope).  The first
            // declaration wins: CORE 4.1b makes a bundle one Session recorded by
            // one peer, so a second `declare` is that peer's new snapshot
            // (MSG 3.3a) and not a different owner.
            if (m_stats.ownerPeerId.empty())
                m_stats.ownerPeerId = idStr(m->body.declare.peer.id);
            break;
        case PPCP_EVENT_SESSION_OPEN:
            m_stats.sessionId = idStr(m->body.session_open.session_id);
            break;
        case PPCP_EVENT_STREAM_OPEN:
            ++m_stats.streams;
            m_streamKind[idStr(m->body.stream_open.stream.id)] =
                idStr(m->body.stream_open.stream.kind);
            break;
        case PPCP_EVENT_CAPTURE:
            if (m->type == PPCP_MT_CAPTURE_ANNOUNCE) onCapture(m);
            break;
        case PPCP_EVENT_PAYLOAD:
            if (m->type == PPCP_MT_PAYLOAD_BEGIN)      onPayloadBegin(m);
            else if (m->type == PPCP_MT_PAYLOAD_CHUNK) onPayloadChunk(m);
            else if (m->type == PPCP_MT_PAYLOAD_END)   onPayloadEnd(m);
            break;
        case PPCP_EVENT_SESSION_STATE:
            // I10 — the owner's assertion, remembered rather than applied.
            // finish() writes it, because ENC 7d resolves the assertion against
            // the reader's truncation and that is not known until the walk ends
            // — and because the ledger has no Session record to close until
            // then either.
            if (m->body.session_state.state == PPCP_SESSION_CLOSED) m_sawClose = true;
            break;
        case PPCP_EVENT_SESSION_CLOSE:
            m_sawClose = true;
            break;
        case PPCP_EVENT_UNKNOWN:
            // MSG 1b / I13 — an unknown type is carried, not dropped, and it is
            // NOT a reason to refuse the bundle.  ENC 7f's forward compatibility
            // is exactly this arriving.
            ++m_stats.unknownEvents;
            break;
        default:
            break;
        }
    }
}

void PpcpImportSink::finish(const PpcpBundleTransport::Result &r)
{
    if (m_open.file) {
        // A payload the bundle stopped mid-transfer.  The bytes that arrived
        // are kept — ENC 7d makes truncation a fact about completeness, not a
        // reason to throw away what was read — but nothing is owed back for it:
        // 5.14h is about a payload DURABLY COMMITTED, and half a clip is not.
        std::fclose(static_cast<std::FILE *>(m_open.file));
        m_open = OpenPayload{};
    }
    if (m_stats.sessionId.empty()) return;

    m_ledger.noteSession(m_stats.ownerPeerId, m_stats.sessionId,
                         r.assertedCompleteness, completenessOf(r.asserted),
                         r.truncated, m_stats.sessionDir);

    // CORE 5.10 — the Session closed.  5.14h1 makes this NOT a reason to
    // withhold a `capture_committed` that is owed: the commit may go days after
    // the import, and releasing the owner's storage stays legitimate after a
    // Session closes.
    if (m_sawClose) m_ledger.closeSession(m_stats.ownerPeerId, m_stats.sessionId);
}

void PpcpImportSink::finishLive()
{
    if (m_open.file) {
        // Same rule as finish(): the bytes that arrived are kept, and nothing
        // is owed back for a payload that never completed (5.14h is about a
        // payload DURABLY committed, and half a clip is not).
        std::fclose(static_cast<std::FILE *>(m_open.file));
        m_open = OpenPayload{};
    }
    if (m_stats.sessionId.empty()) return;

    // `asserted = false`, `truncated = false`: a live replay makes no
    // bundle-level completeness claim, and a link that died mid-replay is not
    // an assertion of truncation — it is an absence of `session_close`, which
    // the line below then declines to act on.  Inventing `truncated` here would
    // be exactly the fabrication ENC 7d's one-directional rule exists to stop.
    m_ledger.noteSession(m_stats.ownerPeerId, m_stats.sessionId,
                         /*asserted=*/false, Completeness::Partial,
                         /*bundleTruncated=*/false, m_stats.sessionDir);
    if (m_sawClose) m_ledger.closeSession(m_stats.ownerPeerId, m_stats.sessionId);
}

}  // namespace Ppcp
