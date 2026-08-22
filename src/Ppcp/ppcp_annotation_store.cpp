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

#include "ppcp_annotation_store.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <ppcp/cbor.h>
#include <ppcp/peer.h>

namespace fs = std::filesystem;

namespace Ppcp {
namespace {

std::string idStr(const ppcp_id &id) { return std::string(id.v, id.len); }

// A filename that cannot escape its directory.  Ids are opaque (5.1a) and may
// legitimately contain a '/' — `st:dev-1/src-1` is the shape this application
// already mints — so they are percent-escaped rather than trusted or rejected.
std::string safeName(const std::string &id)
{
    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                           || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (plain && !(c == '.' && out.empty())) { out.push_back(static_cast<char>(c)); continue; }
        out.push_back('%');
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0f]);
    }
    return out.empty() ? std::string("_") : out;
}

}  // namespace

PpcpAnnotationStore::PpcpAnnotationStore()
{
    m_storage.assign(ppcp_annotation_store_sizeof(), 0);
    ppcp_annotation_store *s = nullptr;
    if (ppcp_annotation_store_new(m_storage.data(), m_storage.size(), &s) == PPCP_OK) m_store = s;
}

PpcpAnnotationStore::~PpcpAnnotationStore() = default;

bool PpcpAnnotationStore::setRoot(const std::string &root, std::string *err)
{
    if (root.empty()) { m_root.clear(); return true; }
    std::error_code ec;
    fs::create_directories(fs::path(root), ec);
    if (ec) { if (err) *err = "could not create " + root + ": " + ec.message(); return false; }
    m_root = root;
    return true;
}

void PpcpAnnotationStore::attach(ppcp_peer *peer, const std::string &selfPeerId)
{
    m_peer = peer;
    m_selfPeerId = selfPeerId;
}

void PpcpAnnotationStore::detach() { m_peer = nullptr; }

std::vector<std::uint8_t> *PpcpAnnotationStore::bodySlot(const std::string &id)
{
    for (auto &e : m_bodies) if (e.first == id) return &e.second;
    m_bodies.emplace_back(id, std::vector<std::uint8_t>{});
    return &m_bodies.back().second;
}

bool PpcpAnnotationStore::observe(const ppcp_annotation &a, bool *outReplaced, std::string *err)
{
    if (!m_store) { if (err) *err = "annotation store was not constructed"; return false; }

    // The body bytes must outlive the caller's frame buffer: an `annotation`
    // arriving over a socket points `body` into the bytes the transport fed,
    // and those are re-presented on the next read.  Copy first, then hand the
    // library a pointer into our copy — the same discipline the import sink
    // uses for a payload chunk, and for the same reason.
    ppcp_annotation copy = a;
    std::vector<std::uint8_t> *slot = bodySlot(idStr(a.id));
    slot->assign(a.body, a.body + a.body_len);
    copy.body = slot->empty() ? nullptr : slot->data();
    copy.body_len = slot->size();

    bool replaced = false;
    // 5.18e — the total order is the LIBRARY'S, including the bytewise
    // `author_peer_id` tiebreak that stops two concurrent editors diverging.
    const ppcp_result r = ppcp_annotation_store_observe(m_store, &copy, &replaced);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_annotation_store_observe: ") + ppcp_result_str(r);
        return false;
    }
    ++m_stats.received;
    if (outReplaced) *outReplaced = replaced;
    if (!replaced) {
        // 9.0c — what arrived was superseded by what we hold, and is IGNORED.
        // Nothing is written: persisting it would put the losing revision on
        // disk and a restart would then load it back over the winner.
        ++m_stats.superseded;
        return true;
    }
    return persist(copy, err);
}

void PpcpAnnotationStore::observeEvent(const ppcp_event &ev)
{
    if (ev.kind != PPCP_EVENT_ANNOTATION || !ev.msg) return;
    (void)observe(ev.msg->body.annotation.annotation, nullptr, nullptr);
}

bool PpcpAnnotationStore::author(const std::string &annotationId, const std::string &sessionId,
                                 const std::string &shotId, const std::string &kind,
                                 const std::string &format,
                                 const std::vector<std::uint8_t> &body,
                                 const std::string &streamId, const ppcp_stream *stream,
                                 const std::string &timebaseRef, std::int64_t atNs,
                                 std::int64_t createdAtNs, std::uint64_t revision,
                                 std::string *err)
{
    if (m_selfPeerId.empty()) { if (err) *err = "no author peer id"; return false; }

    // 5.18j — the presence of `stream_id` FOLLOWS `kind`, and libppcp makes the
    // rule reachable rather than leaving it to a reader: `line` and `plane` are
    // view-specific and carry one; `text` and `nav_anchor` are not and do not.
    const ppcp_kind_view view = ppcp_annotation_kind_view(kind.c_str(), kind.size());
    const bool wantsStream = (view == PPCP_KIND_VIEW_SPECIFIC)
                             || (view == PPCP_KIND_UNREGISTERED && !streamId.empty());

    ppcp_instant at{};
    // 5.18g — where `stream_id` is present, `at` is in THAT STREAM'S timebase;
    // where it is absent, it is in `Session.timebase_ref`.  Choosing the wrong
    // one puts a host-clock number under a device's timebase id, which is I1's
    // defect written into the wire.
    const std::string tb = (wantsStream && stream) ? idStr(stream->timebase_id) : timebaseRef;
    if (ppcp_instant_make(&at, tb.c_str(), tb.size(), atNs) != PPCP_OK) {
        if (err) *err = "at is not a valid Instant on " + tb;
        return false;
    }
    ppcp_instant created{};
    if (ppcp_instant_make(&created, timebaseRef.c_str(), timebaseRef.size(), createdAtNs)
        != PPCP_OK) {
        if (err) *err = "created_at is not a valid Instant on " + timebaseRef;
        return false;
    }

    std::vector<std::uint8_t> *slot = bodySlot(annotationId);
    *slot = body;

    ppcp_annotation a{};
    ppcp_result r = ppcp_annotation_make(&a, annotationId.c_str(), sessionId.c_str(),
                                         shotId.c_str(), &at, m_selfPeerId.c_str(),
                                         PPCP_ANNOT_USER, kind.c_str(), format.c_str(),
                                         slot->empty() ? nullptr : slot->data(), slot->size(),
                                         &created, revision);
    if (r != PPCP_OK) {
        if (err) *err = std::string("ppcp_annotation_make: ") + ppcp_result_str(r);
        return false;
    }
    if (wantsStream) {
        if (streamId.empty()) {
            ++m_stats.placementRefused;
            if (err) *err = "a view-specific kind carries stream_id (5.18j): " + kind;
            return false;
        }
        r = ppcp_annotation_set_stream_id(&a, streamId.c_str());
        if (r != PPCP_OK) {
            if (err) *err = std::string("ppcp_annotation_set_stream_id: ") + ppcp_result_str(r);
            return false;
        }
    }

    ppcp_id ref{};
    if (ppcp_id_set(&ref, timebaseRef.c_str(), timebaseRef.size()) != PPCP_OK) {
        if (err) *err = "timebase_ref is not a valid Id";
        return false;
    }
    // Checked BEFORE origination, so a misplaced annotation is a refusal here
    // rather than something rendered in the wrong place at the far end.
    r = ppcp_annotation_validate_placement(&a, &ref, wantsStream ? stream : nullptr);
    if (r != PPCP_OK) {
        ++m_stats.placementRefused;
        if (err) *err = std::string("ppcp_annotation_validate_placement: ") + ppcp_result_str(r);
        return false;
    }

    bool replaced = false;
    if (ppcp_annotation_store_observe(m_store, &a, &replaced) != PPCP_OK) {
        if (err) *err = "the store refused the annotation";
        return false;
    }
    ++m_stats.authored;
    if (!persist(a, err)) return false;

    if (m_peer) {
        // MSG 9.0a — `annotation` travels either direction, and Markup confers
        // it.  C2 refuses a peer that has not declared Markup, which is the
        // negative half of CONF §1d; the refusal is reported, not swallowed.
        r = ppcp_peer_annotate(m_peer, &a);
        if (r != PPCP_OK) {
            if (err) *err = std::string("ppcp_peer_annotate: ") + ppcp_result_str(r);
            return false;
        }
        ++m_stats.sent;
    }
    return true;
}

bool PpcpAnnotationStore::markDeleted(const std::string &annotationId, std::uint64_t revision,
                                      std::int64_t createdAtNs, std::string *err)
{
    const ppcp_annotation *cur = find(annotationId);
    if (!cur) { if (err) *err = "no annotation " + annotationId; return false; }

    ppcp_annotation a = *cur;
    a.revision = revision;
    if (ppcp_instant_make(&a.created_at, a.created_at.tb.v, a.created_at.tb.len, createdAtNs)
        != PPCP_OK) {
        if (err) *err = "created_at is not a valid Instant";
        return false;
    }
    if (ppcp_annotation_set_deleted(&a, true) != PPCP_OK) {
        if (err) *err = "could not mark deleted";
        return false;
    }
    bool replaced = false;
    if (ppcp_annotation_store_observe(m_store, &a, &replaced) != PPCP_OK) {
        if (err) *err = "the store refused the deletion revision";
        return false;
    }
    if (!persist(a, err)) return false;
    if (m_peer) {
        const ppcp_result r = ppcp_peer_annotate(m_peer, &a);
        if (r != PPCP_OK) { if (err) *err = std::string("ppcp_peer_annotate: ") + ppcp_result_str(r); return false; }
        ++m_stats.sent;
    }
    return true;
}

std::string PpcpAnnotationStore::dirFor(const ppcp_annotation &a) const
{
    // 8.5c's scope on disk: Session, then Shot.  An annotation is anchored to a
    // Shot (5.18) and nothing else, which is why there is no per-Stream level
    // here even for a view-specific kind — the Stream is a field, not a scope.
    return (fs::path(m_root) / safeName(idStr(a.session_id)) / safeName(idStr(a.shot_id))
            / "annotations")
        .string();
}

bool PpcpAnnotationStore::persist(const ppcp_annotation &a, std::string *err)
{
    if (m_root.empty()) return true;   // memory-only is a legal configuration

    std::error_code ec;
    const fs::path dir(dirFor(a));
    fs::create_directories(dir, ec);
    if (ec) { if (err) *err = "could not create " + dir.string() + ": " + ec.message(); return false; }

    const std::string base = safeName(idStr(a.id));

    // ⚠ THE BODY GOES TO ITS OWN FILE, RAW.  5.18a: lossless round-tripping is
    // the requirement and interpreting the format is explicitly not one.  A
    // JSON or base64 sidecar would round-trip too — but it would also invite
    // the next reader to parse the body, and an 8 KiB blob with an embedded NUL
    // is legal (5.18f).  Bytes in, bytes out, no encoder in the path.
    {
        std::ofstream f(dir / (base + ".body"), std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "could not write the annotation body"; return false; }
        if (a.body_len)
            f.write(reinterpret_cast<const char *>(a.body),
                    static_cast<std::streamsize>(a.body_len));
        if (!f) { if (err) *err = "the annotation body was not written whole"; return false; }
    }

    // The fields beside it, as CBOR through libppcp's own encoder — so the
    // persisted form is the WIRE form and a stored annotation and a received
    // one cannot drift apart.  There is no second schema here either.
    {
        std::vector<std::uint8_t> buf(PPCP_ANNOTATION_BODY_MAX + 4096);
        ppcp_cbor_writer w{};
        ppcp_cbor_writer_init(&w, buf.data(), buf.size());
        if (ppcp_annotation_encode(&w, &a) != PPCP_OK) {
            if (err) *err = "the annotation could not be encoded";
            return false;
        }
        std::size_t wrote = 0;
        if (ppcp_cbor_writer_finish(&w, &wrote) != PPCP_OK) {
            if (err) *err = "the annotation record was left incomplete";
            return false;
        }
        std::ofstream f(dir / (base + ".cbor"), std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "could not write the annotation record"; return false; }
        f.write(reinterpret_cast<const char *>(buf.data()),
                static_cast<std::streamsize>(wrote));
        if (!f) { if (err) *err = "the annotation record was not written whole"; return false; }
    }
    ++m_stats.persisted;
    return true;
}

bool PpcpAnnotationStore::loadFromRoot(std::string *err)
{
    if (m_root.empty()) return true;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(m_root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        if (it->path().extension() != ".cbor") continue;

        std::ifstream f(it->path(), std::ios::binary);
        if (!f) continue;
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
        ppcp_cbor_reader r{};
        ppcp_cbor_reader_init(&r, bytes.data(), bytes.size(),
                              ppcp_cbor_limits_for_channel(PPCP_CHANNEL_CONTROL));
        ppcp_annotation a{};
        if (ppcp_annotation_decode(&r, &a) != PPCP_OK) continue;

        // The body was written raw and beside it; the CBOR record carries it
        // too, but the raw file is the one 5.18a's round trip is asserted over.
        const fs::path bodyPath = it->path().parent_path()
                                  / (it->path().stem().string() + ".body");
        std::vector<std::uint8_t> body;
        std::ifstream bf(bodyPath, std::ios::binary);
        if (bf) body.assign((std::istreambuf_iterator<char>(bf)),
                            std::istreambuf_iterator<char>());
        std::vector<std::uint8_t> *slot = bodySlot(idStr(a.id));
        *slot = body;
        a.body = slot->empty() ? nullptr : slot->data();
        a.body_len = slot->size();

        bool replaced = false;
        (void)ppcp_annotation_store_observe(m_store, &a, &replaced);
    }
    if (ec && err) *err = ec.message();
    return true;
}

std::size_t PpcpAnnotationStore::count() const
{
    return m_store ? ppcp_annotation_store_count(m_store) : 0;
}

const ppcp_annotation *PpcpAnnotationStore::at(std::size_t index) const
{
    return m_store ? ppcp_annotation_store_at(m_store, index) : nullptr;
}

const ppcp_annotation *PpcpAnnotationStore::find(const std::string &id) const
{
    if (!m_store) return nullptr;
    ppcp_id key{};
    if (ppcp_id_set(&key, id.c_str(), id.size()) != PPCP_OK) return nullptr;
    return ppcp_annotation_store_find(m_store, &key);
}

std::vector<const ppcp_annotation *>
PpcpAnnotationStore::forShot(const std::string &sessionId, const std::string &shotId) const
{
    std::vector<const ppcp_annotation *> out;
    const std::size_t n = count();
    for (std::size_t i = 0; i < n; ++i) {
        const ppcp_annotation *a = at(i);
        if (!a) continue;
        if (idStr(a->session_id) != sessionId || idStr(a->shot_id) != shotId) continue;
        out.push_back(a);
    }
    return out;
}

}  // namespace Ppcp
