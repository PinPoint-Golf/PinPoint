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

#include "VideoInputPpcp.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <QCryptographicHash>
#include <QDateTime>
#include <QImage>
#include <QSize>
#include <QVideoFrame>

#include <ppcp/common.h>   // ppcp_result_str()
#include <ppcp/transfer.h>

#include "../Ppcp/ppcp_source_declaration.h"   // kHostTimebaseId, hostNowNs()

namespace {

QString idStr(const ppcp_id &id)
{
    return QString::fromUtf8(id.v);
}

// CORE 5.1 caps every Id at 64 bytes (PPCP_ID_MAX) — refused, not truncated,
// by ppcp_id_set() below that. A real peer id carries a "peer:" prefix around
// a UUID (~41 bytes) and a real source id can run "src:camera:ultra_wide"
// (~21 bytes); "st:" + peerId + ":" + sourceId + ":video" adds up to 72 on
// exactly that pairing — over the limit — where every existing test's
// hand-picked short ids ("host-1"/"cam-0") never came close. This is the
// first thing in this class to actually run against a real phone's real-
// length ids, which is why it was never caught. A short, deterministic hash
// keeps the stream id well inside 64 bytes regardless of how long a real
// peer/source id turns out to be, while staying IDENTICAL every time for the
// same (peerId, sourceId) pair — load-bearing for reclaimStream()'s dedup
// and for a repeated start() naming the same Stream it closed last time.
QString shortStreamKey(const QString &peerId, const QString &sourceId)
{
    const QByteArray h = QCryptographicHash::hash(
        (peerId + QLatin1Char('\x1f') + sourceId).toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(h.toHex().left(16));
}

} // namespace

QString VideoInputPpcp::streamIdFor(const QString &peerId, const QString &sourceId,
                                     const QString &kind)
{
    return QStringLiteral("st:%1:%2").arg(shortStreamKey(peerId, sourceId), kind);
}

namespace {

// CORE 5.7 `rate` is millihertz — "150 fps is 150000".  Everything in
// CameraCapabilities is fps, so the conversion happens once, here.
double fpsOf(std::int64_t mhz) { return static_cast<double>(mhz) / 1000.0; }

// 5.11m — a preview profile declares `intrinsics: none`, because decimation and
// downscaling change the intrinsic matrix.  That makes `intrinsics` the one
// field that tells a consumer which profiles it may NOT select for capture
// (5.11l), and it is the only test used here.  Nothing branches on a profile
// id, a name or a product string.
bool isPreviewProfile(const ppcp_capture_profile &p)
{
    return p.has_intrinsics && p.intrinsics == PPCP_INTR_NONE;
}

PixelEncoding encodingFor(const QString &codec, const QString &pixelFormat)
{
    const QString c = codec.toLower();
    const QString f = pixelFormat.toLower();
    if (c == "hevc" || c == "h265")                 return PixelEncoding::H265;
    if (c == "h264" || c == "avc" || c == "avc1")   return PixelEncoding::H264;
    if (c == "mjpeg" || c == "jpeg")                return PixelEncoding::MJPEG;
    if (f == "nv12")                                return PixelEncoding::YUV420_NV12;
    if (f == "i420" || f == "yuv420p")              return PixelEncoding::YUV420_I420;
    if (f == "yuyv" || f == "yuy2")                 return PixelEncoding::YUV422_YUYV;
    if (f == "uyvy")                                return PixelEncoding::YUV422_UYVY;
    if (f == "bgra" || f == "bgra8" || f == "bgra8888") return PixelEncoding::RGBA8;
    if (f == "rgba" || f == "rgba8" || f == "rgba8888") return PixelEncoding::RGBA8;
    if (f == "rgb8" || f == "rgb24")                return PixelEncoding::RGB8;
    if (f == "bgr8" || f == "bgr24")                return PixelEncoding::BGR8;
    if (f == "mono8" || f == "gray8" || f == "y8")  return PixelEncoding::Mono8;
    return PixelEncoding::Unknown;
}

int bitsFor(PixelEncoding e)
{
    switch (e) {
    case PixelEncoding::Mono8:        return 8;
    case PixelEncoding::RGB8:
    case PixelEncoding::BGR8:         return 24;
    case PixelEncoding::RGBA8:        return 32;
    case PixelEncoding::YUV422_YUYV:
    case PixelEncoding::YUV422_UYVY:  return 16;
    case PixelEncoding::YUV420_NV12:
    case PixelEncoding::YUV420_I420:  return 12;
    default:                          return 0;
    }
}

// Bytes per pixel for the PACKED formats only.  A planar format (NV12,
// YUV420P) has no single answer and is not copied plane-blind, so it returns 0
// and the caller takes the encoded-still path instead of shearing an image.
int packedBytesPerPixel(QVideoFrameFormat::PixelFormat f)
{
    switch (f) {
    case QVideoFrameFormat::Format_BGRA8888:
    case QVideoFrameFormat::Format_RGBA8888: return 4;
    case QVideoFrameFormat::Format_YUYV:
    case QVideoFrameFormat::Format_UYVY:     return 2;
    case QVideoFrameFormat::Format_Y8:       return 1;
    default:                                 return 0;
    }
}

QVideoFrameFormat::PixelFormat qtFormatFor(const QString &pixelFormat)
{
    const QString f = pixelFormat.toLower();
    if (f == "bgra" || f == "bgra8" || f == "bgra8888") return QVideoFrameFormat::Format_BGRA8888;
    if (f == "rgba" || f == "rgba8" || f == "rgba8888") return QVideoFrameFormat::Format_RGBA8888;
    if (f == "nv12")                                    return QVideoFrameFormat::Format_NV12;
    if (f == "i420" || f == "yuv420p")                  return QVideoFrameFormat::Format_YUV420P;
    if (f == "yuyv" || f == "yuy2")                     return QVideoFrameFormat::Format_YUYV;
    if (f == "uyvy")                                    return QVideoFrameFormat::Format_UYVY;
    if (f == "mono8" || f == "gray8" || f == "y8")      return QVideoFrameFormat::Format_Y8;
    return QVideoFrameFormat::Format_Invalid;
}

}  // namespace

// ---------------------------------------------------------------------------

namespace {

// The live instances, so 6.1f's re-feed can reach cameras this file did not
// open.  A plain list under a mutex: there are a handful of them, they are
// created and destroyed on the GUI thread, and the push comes from whichever
// thread pumps the link.
std::mutex &ppcpLiveMutex()
{
    static std::mutex m;
    return m;
}
std::vector<VideoInputPpcp *> &ppcpLive()
{
    static std::vector<VideoInputPpcp *> v;
    return v;
}

}  // namespace

VideoInputPpcp::VideoInputPpcp(QObject *parent)
    : VideoInputBase(parent)
{
    qRegisterMetaType<PpcpClip>("PpcpClip");
    std::lock_guard<std::mutex> g(ppcpLiveMutex());
    ppcpLive().push_back(this);
}

QString VideoInputPpcp::timebaseId() const
{
    const ppcp_source *s = source();
    return s ? idStr(s->timebase_id) : QString();
}

int VideoInputPpcp::applyTimebaseOffsets(const QString &peerId,
                                         const TimebaseOffsetLookup &lookup)
{
    std::vector<VideoInputPpcp *> targets;
    {
        std::lock_guard<std::mutex> g(ppcpLiveMutex());
        for (VideoInputPpcp *v : ppcpLive())
            if (v->m_peerId == peerId) targets.push_back(v);
    }
    int mapped = 0;
    for (VideoInputPpcp *v : targets) {
        qint64 off = 0;
        const QString tb = v->timebaseId();
        if (!tb.isEmpty() && lookup && lookup(tb, &off)) {
            v->setTimebaseOffsetNs(off);
            mapped++;
        } else {
            v->clearTimebaseMapping();
        }
    }
    return mapped;
}

int VideoInputPpcp::clearTimebaseMappings(const QString &peerId)
{
    std::vector<VideoInputPpcp *> targets;
    {
        std::lock_guard<std::mutex> g(ppcpLiveMutex());
        for (VideoInputPpcp *v : ppcpLive())
            if (v->m_peerId == peerId) targets.push_back(v);
    }
    for (VideoInputPpcp *v : targets) v->clearTimebaseMapping();
    return static_cast<int>(targets.size());
}

void VideoInputPpcp::reclaimStream(const QString &peerId, const QString &sourceId,
                                   ppcp_peer *peer, VideoInputPpcp *keep)
{
    if (peerId.isEmpty() || sourceId.isEmpty() || !peer) return;
    std::vector<VideoInputPpcp *> stale;
    {
        std::lock_guard<std::mutex> g(ppcpLiveMutex());
        for (VideoInputPpcp *v : ppcpLive())
            if (v != keep && v->m_peerId == peerId && v->m_sourceId == sourceId
                && v->m_peer == peer)
                stale.push_back(v);
    }
    // stop() outside the lock: it closes Streams (queues wire messages) and
    // may emit Qt signals — neither belongs inside a mutex guarding a plain
    // registry list. A stale instance with nothing open (m_captureStreamId
    // empty) simply no-ops here, same as any other stop() call.
    for (VideoInputPpcp *v : stale) v->stop();
}

int VideoInputPpcp::liveInstanceCount(const QString &peerId)
{
    std::lock_guard<std::mutex> g(ppcpLiveMutex());
    int n = 0;
    for (VideoInputPpcp *v : ppcpLive())
        if (peerId.isEmpty() || v->m_peerId == peerId) n++;
    return n;
}

VideoInputPpcp::~VideoInputPpcp()
{
    {
        std::lock_guard<std::mutex> g(ppcpLiveMutex());
        auto &v = ppcpLive();
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }
    // No stop() here: stop() queues `stream_close` frames on an engine this
    // class does not own and whose lifetime it cannot see.  The embedding
    // closes the Streams while the link is still up, or the link's own close
    // does it (5.11a1's other half).
}

QString VideoInputPpcp::deviceIdFor(const QString &peerId, const QString &sourceId)
{
    return QStringLiteral("ppcp:") + peerId + QLatin1Char('/') + sourceId;
}

bool VideoInputPpcp::parseDeviceId(const QString &deviceId, QString *peerId, QString *sourceId)
{
    if (!deviceId.startsWith(QLatin1String("ppcp:"))) return false;
    const QString rest = deviceId.mid(5);
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash == rest.size() - 1) return false;
    if (peerId)   *peerId   = rest.left(slash);
    if (sourceId) *sourceId = rest.mid(slash + 1);
    return true;
}

void VideoInputPpcp::attach(ppcp_peer *peer, const QString &sessionId)
{
    m_peer = peer;
    m_sessionId = sessionId;
}

void VideoInputPpcp::detach()
{
    // ⚠ REMEMBERED, BECAUSE A PHONE COMING BACK IS THE ORDINARY CASE AND NOT AN
    // EXCEPTION.  A link drops and returns constantly on a range, and until
    // this flag existed a preview that had been running simply stayed dead: the
    // only call to `ppcpAttachIfNeeded()` is in `CameraInstance::startPreview()`,
    // which returns early when it is already previewing, so nothing re-attached
    // and the operator had to close and reopen the panel to get a picture back.
    m_resumeOnReattach = (m_state == State::Active || m_state == State::Suspended);
    m_peer = nullptr;
    m_state = State::Stopped;
    emit stateChanged(m_state);
}

// The counterpart declared again — a reconnection, or a second declaration
// (MSG 3.3a) — so every instance this file detached gets its Streams back.
// Mirrors detachAll(), and is deliberately keyed the same way: on the peer id,
// which is what a camera row is named by and what survives the link that
// carried it.
//
// ⚠ ONLY WHAT WAS RUNNING.  An instance the operator had not started is left
// alone: a phone reconnecting must not turn previews on by itself.
int VideoInputPpcp::reattachAll(const QString &peerId, ppcp_peer *peer,
                                const QString &sessionId)
{
    std::vector<VideoInputPpcp *> targets;
    {
        std::lock_guard<std::mutex> g(ppcpLiveMutex());
        for (VideoInputPpcp *v : ppcpLive())
            if (v->m_peerId == peerId && !v->m_peer && v->m_resumeOnReattach)
                targets.push_back(v);
    }
    int resumed = 0;
    for (VideoInputPpcp *v : targets) {
        v->attach(peer, sessionId);
        v->m_resumeOnReattach = false;
        // Empty device id: the peer and source are already parsed and must not
        // be re-derived.  start() re-opens both Streams against the new peer,
        // which is required — the old ids named Streams on a link that is gone.
        if (v->start(QString())) ++resumed;
    }
    return resumed;
}

void VideoInputPpcp::setTimebaseOffsetNs(qint64 offsetNs)
{
    m_offsetNs = offsetNs;
    m_hasOffset = true;
}

void VideoInputPpcp::clearTimebaseMapping()
{
    m_offsetNs = 0;
    m_hasOffset = false;
}

// ── The declaration, re-resolved every time ────────────────────────────────

const ppcp_peer_desc *VideoInputPpcp::counterpart() const
{
    return m_peer ? ppcp_peer_counterpart(m_peer) : nullptr;
}

const ppcp_source *VideoInputPpcp::source() const
{
    const ppcp_peer_desc *p = counterpart();
    if (!p || m_sourceId.isEmpty()) return nullptr;
    const QByteArray want = m_sourceId.toUtf8();
    for (std::size_t i = 0; i < p->source_count; ++i) {
        if (std::strcmp(p->sources[i].id.v, want.constData()) == 0)
            return &p->sources[i];
    }
    return nullptr;
}

const ppcp_capture_profile *VideoInputPpcp::profileById(const char *id) const
{
    const ppcp_source *s = source();
    if (!s || !id) return nullptr;
    for (std::size_t i = 0; i < s->profile_count; ++i)
        if (std::strcmp(s->profiles[i].id.v, id) == 0) return &s->profiles[i];
    return nullptr;
}

const ppcp_capture_profile *VideoInputPpcp::profileForStream(const QString &streamId) const
{
    if (!m_peer || streamId.isEmpty()) return nullptr;
    const ppcp_stream *st = ppcp_peer_stream_find(m_peer, streamId.toUtf8().constData());
    if (!st) return nullptr;
    return profileById(st->profile_id.v);
}

const ppcp_capture_profile *VideoInputPpcp::bestCaptureProfile() const
{
    const ppcp_source *s = source();
    if (!s) return nullptr;

    // ── "Config from PPS", and this is the whole of it ─────────────────────
    //
    // A preferred profile, chosen by the operator and persisted, wins over the
    // automatic pick.  This is the one thing PPCP's config question turned out
    // to mean: WHICH declared CaptureProfile a phone camera uses.  The protocol
    // has carried it all along — `stream_open` names any declared `profile_id`
    // — and every profile a peer declares is already parsed into
    // CameraCapabilities here, so nothing but the choosing was missing.
    //
    // ⚠ A PREFERENCE THAT NO LONGER MATCHES A DECLARED PROFILE IS IGNORED, NOT
    // AN ERROR.  A phone may declare a different set after an OS update, and
    // I26's discipline applies to profiles as much as to Sources: naming one
    // the peer did not declare is refused at the wire, so falling back to the
    // automatic pick is the only honest response.
    if (!m_preferredProfileId.isEmpty()) {
        const QByteArray want = m_preferredProfileId.toUtf8();
        for (std::size_t i = 0; i < s->profile_count; ++i) {
            const ppcp_capture_profile &p = s->profiles[i];
            if (isPreviewProfile(p)) continue;
            if (idStr(p.id) == m_preferredProfileId) return &p;
        }
        // Fell through to the automatic pick.  ⚠ NOT LOGGED HERE: this class
        // reports through `errorOccurred` and counters and holds no logger, and
        // a fallback is not an error.  `CameraInstance::applyStoredPpcpProfile()`
        // checks the stored id against the declaration and says so there, where
        // the operator's choice is what is being restored.
    }

    const ppcp_capture_profile *best = nullptr;
    for (std::size_t i = 0; i < s->profile_count; ++i) {
        const ppcp_capture_profile &p = s->profiles[i];
        // 5.11l — a preview profile is never selectable for capture.
        if (isPreviewProfile(p)) continue;
        if (!best) { best = &p; continue; }
        const std::int64_t a = p.rate.present ? p.rate.nominal_mhz : 0;
        const std::int64_t b = best->rate.present ? best->rate.nominal_mhz : 0;
        if (a > b) best = &p;
    }
    return best;
}

QStringList VideoInputPpcp::declaredCaptureProfiles(double *outRatesHz, int maxRates) const
{
    QStringList ids;
    const ppcp_source *s = source();
    if (!s) return ids;
    int n = 0;
    for (std::size_t i = 0; i < s->profile_count; ++i) {
        const ppcp_capture_profile &p = s->profiles[i];
        if (isPreviewProfile(p)) continue;
        ids.append(idStr(p.id));
        if (outRatesHz && n < maxRates)
            outRatesHz[n] = p.rate.present ? static_cast<double>(p.rate.nominal_mhz) / 1000.0
                                           : 0.0;
        ++n;
    }
    return ids;
}

QString VideoInputPpcp::profileForRate(double fps) const
{
    // The nearest declared rate at or below what was asked, so "120" on a phone
    // that offers 60 and 240 selects 60 rather than silently giving four times
    // the data the operator chose.  Exact matches win outright.
    const ppcp_source *s = source();
    if (!s || fps <= 0.0) return {};
    const ppcp_capture_profile *best = nullptr;
    double bestHz = 0.0;
    for (std::size_t i = 0; i < s->profile_count; ++i) {
        const ppcp_capture_profile &p = s->profiles[i];
        if (isPreviewProfile(p) || !p.rate.present) continue;
        const double hz = static_cast<double>(p.rate.nominal_mhz) / 1000.0;
        if (hz <= fps + 0.5 && hz > bestHz) { bestHz = hz; best = &p; }
    }
    // Nothing at or below: take the slowest declared rather than refusing, so a
    // request for 30 on a phone whose floor is 60 still selects something.
    if (!best) {
        for (std::size_t i = 0; i < s->profile_count; ++i) {
            const ppcp_capture_profile &p = s->profiles[i];
            if (isPreviewProfile(p) || !p.rate.present) continue;
            const double hz = static_cast<double>(p.rate.nominal_mhz) / 1000.0;
            if (!best || hz < bestHz) { bestHz = hz; best = &p; }
        }
    }
    return best ? idStr(best->id) : QString();
}

void VideoInputPpcp::setPreferredCaptureProfile(const QString &profileId)
{
    if (m_preferredProfileId == profileId) return;
    m_preferredProfileId = profileId;
    // ⚠ APPLIED AT THE NEXT stream_open, NOT TO THE ONE THAT IS RUNNING.  A
    // Stream's identity is fixed for its life (5.1a) and its profile with it;
    // changing what a live Stream is would be a different Stream wearing the
    // same id.  The caller restarts the input if it wants the change now.
}

const ppcp_capture_profile *VideoInputPpcp::bestPreviewProfile() const
{
    const ppcp_source *s = source();
    if (!s) return nullptr;
    for (std::size_t i = 0; i < s->profile_count; ++i)
        if (isPreviewProfile(s->profiles[i])) return &s->profiles[i];
    return nullptr;
}

// ── Capabilities, out of the declaration and out of nothing else ───────────

CameraCapabilities VideoInputPpcp::capabilitiesFor(const ppcp_peer_desc *peer,
                                                   const ppcp_source *src)
{
    CameraCapabilities caps;
    if (!peer || !src) return caps;

    // ⚠ CT-S3 ASSERTION 3.  `product` is used for a LABEL and never to infer
    // behaviour: no convention, geometry or readout time below is read from it,
    // from the peer's role, or from any platform identifier.  Every timing
    // field comes from the CaptureProfile the peer declared (I19).
    if (peer->product.present) {
        caps.vendorName = idStr(peer->product.vendor);
        caps.modelName  = idStr(peer->product.model);
        caps.firmwareVersion = idStr(peer->product.version);
    }
    if (src->has_label) caps.modelName = idStr(src->label);
    caps.driverVersion = QStringLiteral("PPCP ") + idStr(peer->protocol_version);
    caps.serialNumber  = idStr(src->peer_id);
    // Not a bus this host can see: the Source is on the far side of a link.
    caps.connectionInterface = CameraCapabilities::Interface::Virtual;
    caps.isVirtual = true;
    caps.queriedAt = QDateTime::currentDateTime();

    caps.extensions.insert(QStringLiteral("ppcp.peer_id"),     idStr(src->peer_id));
    caps.extensions.insert(QStringLiteral("ppcp.source_id"),   idStr(src->id));
    caps.extensions.insert(QStringLiteral("ppcp.source_kind"), idStr(src->kind));
    // 5.6a / I19 — the Source's Timebase.  It is here because every instant
    // this Source produces is in it, and a consumer that lost it would have
    // bare numbers (I1).
    caps.extensions.insert(QStringLiteral("ppcp.timebase_id"), idStr(src->timebase_id));
    caps.extensions.insert(QStringLiteral("ppcp.physical"),    src->physical);
    if (src->viewpoint.present)
        caps.extensions.insert(QStringLiteral("ppcp.viewpoint"), idStr(src->viewpoint.label));

    bool haveRate = false, haveExposure = false, haveIso = false;
    double rateMin = 0, rateMax = 0, rateDefault = 0;
    double expMin = 0, expMax = 0;
    std::int64_t isoMin = 0, isoMax = 0;
    const ppcp_capture_profile *bestCapture = nullptr;

    for (std::size_t i = 0; i < src->profile_count; ++i) {
        const ppcp_capture_profile &p = src->profiles[i];
        const QString pid = idStr(p.id);
        const bool preview = isPreviewProfile(p);

        // Every profile's timing IS carried out, preview included — a consumer
        // that could not see the convention would be back to hardcoding it,
        // which is precisely what I19 exists to stop.
        const QString key = QStringLiteral("ppcp.profile.") + pid;
        static const char *conv[] = { "mid", "start", "end", "nominal_frame_start" };
        caps.extensions.insert(key + QStringLiteral(".timing.convention"),
                               QString::fromLatin1(conv[p.timing.convention]));
        if (p.timing.has_offset) {
            caps.extensions.insert(key + QStringLiteral(".timing.offset_ns"),
                                   static_cast<qlonglong>(p.timing.frame_start_to_exposure_offset_ns));
            static const char *prov[] = { "assumed", "vendor", "measured" };
            caps.extensions.insert(key + QStringLiteral(".timing.offset_provenance"),
                                   QString::fromLatin1(prov[p.timing.offset_provenance]));
        }
        if (p.has_geometry) {
            caps.extensions.insert(key + QStringLiteral(".geometry"),
                                   p.geometry.kind == PPCP_GEOM_ROLLING_SHUTTER
                                       ? QStringLiteral("rolling_shutter")
                                       : QStringLiteral("global"));
            if (p.geometry.kind == PPCP_GEOM_ROLLING_SHUTTER) {
                caps.extensions.insert(key + QStringLiteral(".geometry.readout_ns"),
                                       static_cast<qlonglong>(p.geometry.readout_ns));
                caps.extensions.insert(key + QStringLiteral(".geometry.rows"),
                                       static_cast<qlonglong>(p.geometry.rows));
            }
        }
        caps.extensions.insert(key + QStringLiteral(".preview"), preview);
        // I28 — absence means NOT MEASURED, and it is reported as absence.
        caps.extensions.insert(key + QStringLiteral(".measured"), p.has_measured);

        if (preview) continue;   // 5.11l: not a mode a consumer may select

        if (p.format.present) {
            Resolution r{ static_cast<int>(p.format.width), static_cast<int>(p.format.height) };
            if (r.width > 0 && r.height > 0) {
                bool seen = false;
                for (const Resolution &e : caps.resolution.presets)
                    if (e.width == r.width && e.height == r.height) { seen = true; break; }
                if (!seen) caps.resolution.presets.append(r);
            }
            PixelFormat pf;
            pf.nativeKey = idStr(p.format.codec);
            if (ppcp_id_is_set(&p.format.pixel_format))
                pf.nativeKey += QLatin1Char('/') + idStr(p.format.pixel_format);
            pf.encoding = encodingFor(idStr(p.format.codec), idStr(p.format.pixel_format));
            pf.bitsPerPixel = bitsFor(pf.encoding);
            bool seen = false;
            for (const PixelFormat &e : caps.pixelFormat.supported)
                if (e.nativeKey == pf.nativeKey) { seen = true; break; }
            if (!seen) caps.pixelFormat.supported.append(pf);
        }

        if (p.rate.present) {
            const double nominal = fpsOf(p.rate.nominal_mhz);
            const double lo = fpsOf(p.rate.min_mhz ? p.rate.min_mhz : p.rate.nominal_mhz);
            const double hi = fpsOf(p.rate.max_mhz ? p.rate.max_mhz : p.rate.nominal_mhz);
            if (!haveRate) { rateMin = lo; rateMax = hi; haveRate = true; }
            rateMin = std::min(rateMin, lo);
            rateMax = std::max(rateMax, hi);
            if (!bestCapture || nominal > rateDefault) rateDefault = nominal;
        }
        if (p.optical.present) {
            const double lo = static_cast<double>(p.optical.exposure_min_ns) / 1000.0;
            const double hi = static_cast<double>(p.optical.exposure_max_ns) / 1000.0;
            if (!haveExposure) { expMin = lo; expMax = hi; haveExposure = true; }
            expMin = std::min(expMin, lo);
            expMax = std::max(expMax, hi);
            if (p.optical.iso_min || p.optical.iso_max) {
                if (!haveIso) { isoMin = p.optical.iso_min; isoMax = p.optical.iso_max; haveIso = true; }
                isoMin = std::min<std::int64_t>(isoMin, p.optical.iso_min);
                isoMax = std::max<std::int64_t>(isoMax, p.optical.iso_max);
            }
        }
        if (!bestCapture || (p.rate.present && bestCapture->rate.present
                             && p.rate.nominal_mhz > bestCapture->rate.nominal_mhz))
            bestCapture = &p;
    }

    if (!caps.resolution.presets.isEmpty()) {
        caps.resolution.kind = CapabilityKind::Discrete;
        // Not writable: a consumer does not set a width on a peer's camera, it
        // opens a Stream naming a declared profile (MSG §6).
        caps.resolution.writable = false;
        if (bestCapture && bestCapture->format.present)
            caps.resolution.defaultResolution = {
                static_cast<int>(bestCapture->format.width),
                static_cast<int>(bestCapture->format.height) };
        else
            caps.resolution.defaultResolution = caps.resolution.presets.first();
    }
    if (!caps.pixelFormat.supported.isEmpty()) {
        caps.pixelFormat.kind = CapabilityKind::Discrete;
        caps.pixelFormat.writable = false;
        caps.pixelFormat.defaultFormat = caps.pixelFormat.supported.first();
    }
    if (haveRate) {
        caps.frameRate.kind = CapabilityKind::Range;
        caps.frameRate.readable = true;
        caps.frameRate.writable = false;
        caps.frameRate.range = { rateMin, rateMax, 0.0, rateDefault };
    }
    if (haveExposure) {
        caps.exposureTime.kind = CapabilityKind::Range;
        caps.exposureTime.readable = true;
        caps.exposureTime.writable = false;
        caps.exposureTime.range = { expMin, expMax, 0.0, 0.0 };
    }
    if (haveIso) {
        caps.extensions.insert(QStringLiteral("ppcp.iso_min"), static_cast<qlonglong>(isoMin));
        caps.extensions.insert(QStringLiteral("ppcp.iso_max"), static_cast<qlonglong>(isoMax));
    }

    // Every Capture carries `frames` (I2/5.8e), so the device timestamps in
    // hardware terms as far as a consumer is concerned.  It is NOT a trigger
    // input: nothing here can pulse a wire on the far side of a socket.
    caps.trigger.supported = false;
    caps.trigger.hasTimestamping = true;
    caps.chunkData.timestamp = true;
    caps.chunkData.exposureTime = true;   // 5.8d — present on any camera Capture

    return caps;
}

CameraCapabilities VideoInputPpcp::queryCapabilities() const
{
    return capabilitiesFor(counterpart(), source());
}

// ── Transport control ──────────────────────────────────────────────────────

void VideoInputPpcp::prepareDevice(const QString &deviceId)
{
    QString peerId, sourceId;
    if (parseDeviceId(deviceId, &peerId, &sourceId)) {
        m_peerId = peerId;
        m_sourceId = sourceId;
    }
}

bool VideoInputPpcp::openStream(const QString &streamId, const char *kind,
                                const char *profileId, ppcp_continuity continuity)
{
    m_lastStreamOpenError.clear();
    const ppcp_source *s = source();
    if (!m_peer || !s || !profileId) {
        m_lastStreamOpenError = QStringLiteral("no peer/source/profile");
        return false;
    }

    // 5.11 — `opened_at` is on the Stream's Timebase, which is the SOURCE's.
    // A consumer opening a Stream does not know the far side's clock, so it
    // proposes the origin and takes the owner's answer from `stream_open_ack`
    // (MSG 6.1b).  It is emphatically not this host's clock wearing the
    // device's timebase id, which is the I1 defect one layer up.
    ppcp_instant openedAt{};
    ppcp_result rc = ppcp_instant_make_z(&openedAt, s->timebase_id.v, 0);
    if (rc != PPCP_OK) {
        m_lastStreamOpenError = QStringLiteral("opened_at: %1").arg(ppcp_result_str(rc));
        return false;
    }

    ppcp_stream st{};
    rc = ppcp_stream_make(&st, streamId.toUtf8().constData(),
                          m_sessionId.toUtf8().constData(),
                          s->id.v, kind, profileId, s->timebase_id.v,
                          continuity, &openedAt);
    if (rc != PPCP_OK) {
        m_lastStreamOpenError = QStringLiteral("stream_make: %1").arg(ppcp_result_str(rc));
        return false;
    }
    rc = ppcp_peer_stream_open(m_peer, &st);
    if (rc != PPCP_OK) {
        m_lastStreamOpenError = QStringLiteral("stream_open: %1").arg(ppcp_result_str(rc));
        return false;
    }
    return true;
}

void VideoInputPpcp::closeStream(const QString &streamId, const char *reason)
{
    if (!m_peer || streamId.isEmpty()) return;

    // 5.11a1 — either peer may close, and the consumer's reason is that it no
    // longer wants the data.
    //
    // ⚠ `closed_at` IS THE AWKWARD PART, AND IT IS A SPECIFICATION QUESTION,
    // NOT A CODING ONE.  CORE 5.11 gives a Stream `closed_at` cardinality
    // `0..1`; MSG §11's `stream_close` body writes `{ stream_id, closed_at:
    // Instant, reason: Kind }` with no optionality, and libppcp enforces the
    // MSG reading — ppcp_peer_stream_close() refuses a NULL.  So a CONSUMER
    // exercising 5.11a1 must state an instant, and the only clock it can read
    // is its own: the Stream's `timebase_id` is the far side's, and putting
    // this host's number under that id would be I1's defect written down.
    //
    // So the instant carries `tb:host` — honestly labelled, this host's own
    // monotonic clock, meaning "I stopped wanting this at this moment".  A
    // receiver that needs it on the Stream's timebase converts through a
    // TimebaseRelation, which is what relations are for.  Reported as F-H4-2.
    ppcp_instant closedAt{};
    if (ppcp_instant_make_z(&closedAt, Ppcp::kHostTimebaseId, Ppcp::hostNowNs()) != PPCP_OK)
        return;
    (void)ppcp_peer_stream_close(m_peer, streamId.toUtf8().constData(), &closedAt, reason);
}

bool VideoInputPpcp::start(const QString &deviceId)
{
    if (!deviceId.isEmpty()) prepareDevice(deviceId);

    if (!m_peer) {
        emit errorOccurred(QStringLiteral("PPCP camera: no peer attached"));
        m_state = State::Error;
        emit stateChanged(m_state);
        return false;
    }
    if (m_sessionId.isEmpty()) {
        emit errorOccurred(QStringLiteral("PPCP camera: no session open"));
        m_state = State::Error;
        emit stateChanged(m_state);
        return false;
    }
    const ppcp_source *s = source();
    if (!s) {
        emit errorOccurred(QStringLiteral("PPCP camera: peer declares no Source '%1'").arg(m_sourceId));
        m_state = State::Error;
        emit stateChanged(m_state);
        return false;
    }
    if (!ppcp_source_kind_is_camera(s)) {
        emit errorOccurred(QStringLiteral("PPCP Source '%1' is kind '%2', not a camera")
                               .arg(m_sourceId, idStr(s->kind)));
        m_state = State::Error;
        emit stateChanged(m_state);
        return false;
    }

    const ppcp_capture_profile *cap = bestCaptureProfile();
    if (!cap) {
        emit errorOccurred(QStringLiteral("PPCP Source '%1' declares no capture profile").arg(m_sourceId));
        m_state = State::Error;
        emit stateChanged(m_state);
        return false;
    }

    // ⚠ A STALE SIBLING FOR THE SAME SOURCE, ON THE SAME LIVE PEER, IS STOPPED
    // FIRST RATHER THAN RACED WITH. Stream ids are deterministic per
    // (peerId, sourceId) — "st:<peer>:<src>:video" — on purpose (5.1a: a
    // Stream's identity is fixed), which means two live VideoInputPpcp
    // instances bound to the SAME ppcp_peer for the same camera Source
    // collide on peer_stream_add() rather than each getting their own. That
    // is reachable in practice: a Settings -> Cameras Repeater rebuild (e.g.
    // the cameraListChanged() a PPCP declare fires the moment its Sources
    // are registered) can create a fresh preview instance for a row before
    // the OLD delegate's Component.onDestruction has synchronously torn its
    // own instance down. There is only one honest answer for "what is this
    // camera's preview showing right now": whichever start() is running NOW
    // — so a stale sibling is stopped first rather than left to lose the
    // race and report a confusing "stream_open refused".  Matching on `m_peer`
    // too (not just the id strings) matters: two DIFFERENT live peers can
    // coincidentally share a peerId/sourceId — any test harness that
    // hardcodes both ends' ids is exactly this — and those are not the race
    // this exists to resolve.
    reclaimStream(m_peerId, m_sourceId, m_peer, this);

    // The capture Stream.  `shot_windowed`: a host that arbitrates asks for
    // clips around a t0, and 5.14d then forbids `{stream: true}` on it, which
    // is the shape we want — a clip anchors to a Shot or a Candidate.
    m_captureStreamId = streamIdFor(m_peerId, m_sourceId, QStringLiteral("video"));
    if (!openStream(m_captureStreamId, PPCP_STREAM_KIND_VIDEO, cap->id.v, PPCP_SHOT_WINDOWED)) {
        m_captureStreamId.clear();
        emit errorOccurred(QStringLiteral("PPCP camera: stream_open refused for '%1' (%2)")
                               .arg(m_sourceId, m_lastStreamOpenError));
        m_state = State::Error;
        emit stateChanged(m_state);
        return false;
    }

    if (cap->format.present) {
        m_frameWidth  = static_cast<int>(cap->format.width);
        m_frameHeight = static_cast<int>(cap->format.height);
        m_previewPixelFormat = idStr(cap->format.pixel_format);
    }

    // 5.11.2 — the live tile, WHERE THE DEVICE OFFERS ONE.  A peer that
    // declares no preview profile is not broken and is not asked twice: "a peer
    // that does not offer a suitable profile simply refuses, and nothing else
    // changes" (5.11.2).  Always `continuous` (5.11), and its payload wants a
    // bulk channel of its own (5.11h) — the transport's channel 2, which
    // PeerConnection already carries as an optional third connection.
    if (const ppcp_capture_profile *pv = bestPreviewProfile()) {
        m_previewStreamId = streamIdFor(m_peerId, m_sourceId, QStringLiteral("preview"));
        if (!openStream(m_previewStreamId, PPCP_STREAM_KIND_PREVIEW, pv->id.v, PPCP_CONTINUOUS))
            m_previewStreamId.clear();
        else if (pv->format.present) {
            m_frameWidth  = static_cast<int>(pv->format.width);
            m_frameHeight = static_cast<int>(pv->format.height);
            m_previewPixelFormat = idStr(pv->format.pixel_format);
        }
    }

    m_state = State::Active;
    emit stateChanged(m_state);
    return true;
}

void VideoInputPpcp::stop()
{
    closeStream(m_previewStreamId, "not_needed");
    closeStream(m_captureStreamId, "not_needed");
    m_previewStreamId.clear();
    m_captureStreamId.clear();
    m_open = Open{};
    m_state = State::Stopped;
    emit stateChanged(m_state);
}

void VideoInputPpcp::suspend()
{
    // 5.11i — preview degrades first, and a consumer that does not want to look
    // at it right now says so rather than letting it compete for bulk capacity.
    // The capture Stream stays open: suspending a live tile must not disarm a
    // camera that is waiting for a shot.
    if (!m_previewStreamId.isEmpty()) {
        closeStream(m_previewStreamId, "not_needed");
        m_previewStreamId.clear();
    }
    m_state = State::Suspended;
    emit stateChanged(m_state);
}

void VideoInputPpcp::resume()
{
    if (const ppcp_capture_profile *pv = bestPreviewProfile()) {
        m_previewStreamId = streamIdFor(m_peerId, m_sourceId, QStringLiteral("preview"));
        if (!openStream(m_previewStreamId, PPCP_STREAM_KIND_PREVIEW, pv->id.v, PPCP_CONTINUOUS))
            m_previewStreamId.clear();
    }
    m_state = State::Active;
    emit stateChanged(m_state);
}

bool VideoInputPpcp::isActive() const
{
    return m_state == State::Active;
}

QVideoFrameFormat VideoInputPpcp::frameFormat() const
{
    const QVideoFrameFormat::PixelFormat pf = qtFormatFor(m_previewPixelFormat);
    if (m_frameWidth <= 0 || m_frameHeight <= 0 || pf == QVideoFrameFormat::Format_Invalid)
        return {};
    return QVideoFrameFormat(QSize(m_frameWidth, m_frameHeight), pf);
}

qint64 VideoInputPpcp::lastFrameInstantUs() const
{
    // ⚠ THE REFUSAL IS THE FEATURE.  §6.1 is a conversion WITHIN a Source's
    // timebase; the mapping onto this host's clock is a TimebaseRelation and
    // nobody has measured one yet (H5 over L9).  Answering a plausible number
    // here would put a fabricated clock mapping into the EventBuffer, where it
    // is indistinguishable from a real one and looks exactly like drift.  0
    // means "I have no instant on your clock", and the consumer stamps arrival
    // — which is at least honestly wrong.
    if (!m_haveLastInstant || !m_hasOffset) return 0;
    const qint64 hostNs = m_lastInstantSourceNs + m_offsetNs;
    return hostNs / 1000;
}

bool VideoInputPpcp::rowInstantNs(int frame, quint32 row, qint64 *outNs) const
{
    if (!outNs || frame < 0 || frame >= m_lastCanonicalNs.size()) return false;
    if (!m_haveLastGeometry) return false;
    // 6.2c — the frame's canonical instant IS the first row's, so it is what
    // ppcp_row_instant() is given.  The arithmetic is L3's, for the same reason
    // the conversion above is: assembling it by hand is how it gets got wrong.
    std::int64_t out = 0;
    if (ppcp_row_instant(&m_lastGeometry, m_lastCanonicalNs[frame], row, &out) != PPCP_OK)
        return false;
    *outNs = static_cast<qint64>(out);
    return true;
}

// ── The event pump ─────────────────────────────────────────────────────────

void VideoInputPpcp::drainEvents()
{
    if (!m_peer) return;
    ppcp_event ev{};
    while (ppcp_peer_next_event(m_peer, &ev) == PPCP_OK) processEvent(ev);
}

// The dispatch a drained event gets, factored out of drainEvents() so a peer
// this class does NOT drain (one PpcpHostPeer is already pumping — see
// dispatchEvent() below) can still hand it an event one at a time, in the
// same shape drainEvents() always produced.
void VideoInputPpcp::processEvent(const ppcp_event &ev)
{
    const ppcp_msg *m = ev.msg;
    if (!m) return;
    switch (ev.kind) {
    case PPCP_EVENT_STREAM_OPEN_ACK:
        onStreamOpenAck(m);
        break;
    case PPCP_EVENT_STREAM_CLOSE:
        onStreamClose(m);
        break;
    case PPCP_EVENT_CAPTURE:
        if (m->type == PPCP_MT_CAPTURE_ANNOUNCE) onCaptureAnnounce(m);
        break;
    case PPCP_EVENT_PAYLOAD:
        switch (m->type) {
        case PPCP_MT_PAYLOAD_BEGIN: onPayloadBegin(m); break;
        case PPCP_MT_PAYLOAD_CHUNK: onPayloadChunk(m); break;
        case PPCP_MT_PAYLOAD_END:   onPayloadEnd(m);   break;
        case PPCP_MT_PAYLOAD_ABORT: onPayloadAbort(m); break;
        default: break;
        }
        break;
    default:
        // Every other event belongs to the session layer, which is not this
        // class's business (F7).  C1: they are parsed and carried whatever
        // this consumer does with them.
        break;
    }
}

// ── A peer PpcpHostPeer already pumps — the second-consumer path ───────────
//
// ⚠ THE EVENT RING HAS EXACTLY ONE DRAINER (ppcp_host_peer.h).  A
// `VideoInputPpcp` bound to a peer PpcpHostPeer is already pumping MUST NOT
// call drainEvents() on it — a second `ppcp_peer_next_event()` caller would
// each see roughly half the conversation.  `PpcpHostPeer::addEventHook()` is
// the one drainer's way of handing the SAME event to a second consumer, once,
// synchronously, while its bytes are still the ones the transport fed — so
// this is a broadcast to every live instance bound to that peer, exactly the
// way applyTimebaseOffsets()/clearTimebaseMappings() already broadcast a
// per-peer update to this same registry.  Returns how many instances saw it.
int VideoInputPpcp::dispatchEvent(const QString &peerId, const ppcp_event &ev)
{
    std::vector<VideoInputPpcp *> targets;
    {
        std::lock_guard<std::mutex> g(ppcpLiveMutex());
        for (VideoInputPpcp *v : ppcpLive())
            if (v->m_peerId == peerId) targets.push_back(v);
    }
    for (VideoInputPpcp *v : targets) v->processEvent(ev);
    return static_cast<int>(targets.size());
}

// The peer went away (7.4d-adjacent, at the transport's own disconnect
// rather than a pairing revocation) — every live instance bound to it must
// stop pointing at it before the ppcp_peer itself is destroyed, or the next
// start()/processEvent() call reads a dangling pointer.  Same filter as
// applyTimebaseOffsets()/clearTimebaseMappings(); the action is detach()
// rather than a re-feed.
int VideoInputPpcp::detachAll(const QString &peerId)
{
    std::vector<VideoInputPpcp *> targets;
    {
        std::lock_guard<std::mutex> g(ppcpLiveMutex());
        for (VideoInputPpcp *v : ppcpLive())
            if (v->m_peerId == peerId) targets.push_back(v);
    }
    for (VideoInputPpcp *v : targets) v->detach();
    return static_cast<int>(targets.size());
}

// ── MSG 5.1 / E18's clause 1c — the answer we asked for and never read ──────
//
// ⚠ `stream_open` IS A REQUEST, `any -> owner`, AND THE OWNER MAY REFUSE.  That
// is the message table's own direction, and it is what makes a host asking a
// phone for a particular CaptureProfile legal — the host requests, the device
// that owns the camera answers `opened` or `refused`.
//
// ⛔ AND WE HAD THE COMMENT WITHOUT THE CODE.  `openStream()` says, and has
// always said, "a consumer opening a Stream does not know the far side's clock,
// so it proposes the origin and takes the owner's answer from
// `stream_open_ack`".  Nothing took it.  `openStream()` returned true when the
// request was QUEUED and every caller read that as "the Stream is open" — the
// same "I sent it" mistaken for "it happened" that arming had, and now
// load-bearing: 5.11l REQUIRES an owner to refuse a consumer that selects a
// preview profile for capture, so a refusal is an expected answer and not an
// error path.  Without this, an operator's chosen capture format could be
// refused by the phone and the picker would go on showing their choice.
void VideoInputPpcp::onStreamOpenAck(const ppcp_msg *m)
{
    const ppcp_body_stream_open_ack &a = m->body.stream_open_ack;
    const QString streamId = idStr(a.stream_id);
    const bool isCapture = (streamId == m_captureStreamId && !streamId.isEmpty());
    const bool isPreview = (streamId == m_previewStreamId && !streamId.isEmpty());
    if (!isCapture && !isPreview) return;   // some other consumer's Stream

    if (a.verdict == PPCP_STREAM_REFUSED) {
        const QString why = a.has_reason ? idStr(a.reason) : QStringLiteral("no reason given");
        ++m_counters.streamsRefused;
        // Forgotten rather than remembered as open: the id must stop matching,
        // or `onCaptureAnnounce()` would accept Captures on a Stream the owner
        // told us it never opened.
        if (isCapture) m_captureStreamId.clear();
        else           m_previewStreamId.clear();
        m_lastStreamOpenError = why;
        const QString what = isPreview ? QStringLiteral("preview") : QStringLiteral("capture");
        emit errorOccurred(QStringLiteral("PPCP camera: the device refused the %1 stream (%2)")
                               .arg(what, why));
        return;
    }

    // 5.11 / MSG 6.1b — `opened_at` is the OWNER's reading on the Stream's own
    // timebase, which is why the request proposed an origin rather than this
    // host's clock wearing the device's timebase id.  Kept as the owner sent it
    // and never re-stamped here (I1).
    if (a.has_opened_at) {
        if (isCapture) m_captureOpenedAtNs = static_cast<qint64>(a.opened_at.ns);
        else           m_previewOpenedAtNs = static_cast<qint64>(a.opened_at.ns);
    }
    ++m_counters.streamsOpened;
}

// ── MSG 5.1d / CORE 5.11i — the OWNER closing a Stream we are reading ───────
//
// ⚠ 5.1d MAKES A CLOSE LEGAL IN EITHER DIRECTION, AND WE WERE ONLY EVER THE
// ONE DOING IT.  `PPCP_EVENT_STREAM_CLOSE` had no handler here at all, so a
// device closing a Stream left this consumer believing it was open — showing a
// frozen tile, or waiting for Captures that were never coming, and saying
// nothing.
//
// 5.11i names the case exactly, and it is the one a range session produces:
// "a peer under sustained thermal load closes a `preview` rather than keeping
// it nominally open and announcing absence for the rest of the session."  A hot
// phone mid-round is the expected path, not the unhappy one.
void VideoInputPpcp::onStreamClose(const ppcp_msg *m)
{
    const ppcp_body_stream_close &b = m->body.stream_close;
    const QString streamId = idStr(b.stream_id);
    const bool isCapture = (streamId == m_captureStreamId && !streamId.isEmpty());
    const bool isPreview = (streamId == m_previewStreamId && !streamId.isEmpty());
    if (!isCapture && !isPreview) return;

    // 5.1d's vocabulary is an open registry — `thermal_limit`, `storage_full`,
    // `not_needed`, `calibration_changed` and whatever a peer adds — so it is
    // carried as the owner worded it and never mapped onto one we knew (10.3a).
    const QString why = idStr(b.reason);
    if (isCapture) m_captureStreamId.clear();
    else           m_previewStreamId.clear();
    ++m_counters.streamsClosedByOwner;

    const QString what = isPreview ? QStringLiteral("preview") : QStringLiteral("capture");
    m_lastStreamOpenError = why;
    emit errorOccurred(QStringLiteral("PPCP camera: the device closed the %1 stream (%2)")
                           .arg(what, why.isEmpty() ? QStringLiteral("no reason given") : why));
}

void VideoInputPpcp::onCaptureAnnounce(const ppcp_msg *m)
{
    const ppcp_capture &c = m->body.capture_announce.capture;
    const QString capId = idStr(c.id);
    const QString streamId = idStr(c.stream_id);

    // ── WHOSE STREAM MAY CARRY A CAPTURE FOR THIS SOURCE ───────────────────
    //
    // ⚠ THIS USED TO FILTER ON "STREAMS I OPENED", AND THAT WAS THIS HOST
    // ASSUMING A RULE THE SPECIFICATION DOES NOT STATE.  MSG 5.1c is a MUST
    // about the ZERO-HOST case — "the capturing peer originates `stream_open`
    // for its own Streams" — and says nothing about who opens one when a host
    // is present.  5.11a1 settles who may CLOSE and not who may open.  So a
    // device opening its own capture Stream in a hosted Session is legal, and
    // this filter dropped every Capture announced on it with no error at either
    // end: the id was simply one we had never seen.
    //
    // The PinPointCapture team's argument for why the device must be the one to
    // open it is the decisive half, and it is about the FILE rather than the
    // wire: ENC 7a/7b make a bundle the owner's OUTBOUND frames, so a
    // host-originated `stream_open` can never appear in the device's own
    // bundle.  Under a host-opens rule that bundle carries Captures with no
    // Stream record behind them — no `profile_id`, no `timebase_id`, no
    // `continuity` — and a device announcing against OUR id instead just moves
    // the same hole into its own file.
    //
    // So resolution is by SOURCE, which is the identity both ends agree on.
    const ppcp_stream *st =
        m_peer ? ppcp_peer_stream_find(m_peer, streamId.toUtf8().constData()) : nullptr;
    const bool ours = !streamId.isEmpty()
                      && (streamId == m_captureStreamId || streamId == m_previewStreamId);
    if (!ours) {
        if (!st || idStr(st->source_id) != m_sourceId) return;
        ++m_counters.foreignStreamCaptures;
    }

    // 5.11m — a preview Stream is `kind: preview`.  For a Stream we opened the
    // id already says which it is; for one the device opened, the Stream's own
    // `kind` is the only honest answer and is read rather than guessed from a
    // profile or a name.
    const bool preview = ours ? (streamId == m_previewStreamId && !streamId.isEmpty())
                              : (st && idStr(st->kind) == QLatin1String("preview"));

    // ⚠ CT-I36a, HOST AS CONSUMER, AND IT HAS TO BE DONE HERE.
    // ppcp_capture_validate_in_stream() is the function that carries 5.11j —
    // "a consumer therefore never sees `transfer: pending` on a preview
    // Capture" — but the engine does NOT run it on receipt: peer_handle()'s
    // PPCP_MT_CAPTURE_ANNOUNCE arm calls ppcp_transfer_observe_announce(...,
    // false), hardcoding `is_preview` even though ppcp_peer_stream_find() could
    // resolve the Stream from the Capture's own `stream_id`.  So the refusal is
    // the embedding's to make, and this is where the host makes it.  Reported
    // to libppcp as finding F-H4-1.
    if (st) {
        if (ppcp_capture_validate_in_stream(&c, st) != PPCP_OK) {
            if (preview) ++m_counters.previewPendingRefused;
            return;
        }
    } else if (preview && c.transfer == PPCP_TRANSFER_PENDING
                       && c.completeness != PPCP_ABSENT) {
        ++m_counters.previewPendingRefused;
        return;
    }

    if (preview) {
        ++m_counters.previewCaptures;
        m_previewCaptures.push_back(capId);
        // I11 / 5.11c3 — a shed preview frame is an `absent` SEGMENT, never a
        // gap.  A peer that reports one as a gap is claiming a dropout it did
        // not have, and the conflation is counted rather than absorbed.
        if (c.gap_count > 0) ++m_counters.previewGapsSeen;
    }
    if (c.completeness == PPCP_ABSENT) {
        ++m_counters.absentSegments;
        // 5.14d — the interval IS the claim on an `absent` segment, and it
        // still accounts for its span (I36).  There is no payload to wait for,
        // so the Capture is complete as an answer the moment it is announced.
        PpcpClip clip;
        clip.captureId = capId;
        clip.streamId = streamId;
        clip.peerId = m_peerId;
        clip.sourceId = m_sourceId;
        clip.completeness = PPCP_ABSENT;
        clip.absentReason = c.has_absent_reason ? idStr(c.absent_reason) : QString();
        clip.preview = preview;
        if (!preview) { ++m_counters.clips; emit clipReady(clip); }
        return;
    }

    m_captureStream.emplace_back(capId, streamId);
}

void VideoInputPpcp::onPayloadBegin(const ppcp_msg *m)
{
    const ppcp_body_payload_begin &b = m->body.payload_begin;
    const QString capId = idStr(b.capture_id);

    m_open = Open{};
    m_open.active = true;
    m_open.declaredBytes = b.bytes;
    m_open.clip.captureId = capId;
    m_open.clip.peerId = m_peerId;
    m_open.clip.sourceId = m_sourceId;
    for (const auto &e : m_captureStream)
        if (e.first == capId) { m_open.clip.streamId = e.second; break; }
    m_open.clip.preview = !m_open.clip.streamId.isEmpty()
                          && m_open.clip.streamId == m_previewStreamId;

    // ── CORE §6.1, and the whole reason this work package exists ───────────
    //
    // I30: `achieved_frames` rides `payload_begin` and nowhere else.  I17: the
    // conversion needs the CONVENTION, the PER-FRAME exposure, and — for
    // `nominal_frame_start` — the offset, and no two of the three are enough.
    // ppcp_achieved_frames_canonical_at() is L3's one-call form of exactly that
    // triple, which is why it is called rather than the arithmetic being
    // written out here: "the ways of getting the conversion wrong all involve
    // assembling the three inputs by hand".
    //
    // The `timing` comes from the CaptureProfile the STREAM named, resolved
    // through the counterpart's declaration.  Not from a product string, not
    // from a role, not from a platform (CT-S3 assertion 3), and not from this
    // host's own cameras' convention (5.6.1).
    if (!b.has_achieved_frames) {
        // 5.8d makes AchievedFrames mandatory on any camera Capture that has
        // frames; 5.8j exempts a preview Stream from the EXPOSURE requirement,
        // never from `frames`.  Absent altogether, there is nothing to convert.
        ++m_counters.unconvertible;
        return;
    }
    const ppcp_capture_profile *p = profileForStream(m_open.clip.streamId);
    if (!p) {
        // No profile, no `timing`, no conversion.  Recorded, never guessed: a
        // fallback convention here would be the hardcoding I19 exists to stop.
        ++m_counters.unconvertible;
        return;
    }

    const ppcp_achieved_frames &af = b.achieved_frames;
    m_open.clip.timebaseId = idStr(af.tb);
    m_open.clip.canonicalNs.reserve(static_cast<int>(af.frame_count));
    m_open.clip.exposureNs.reserve(static_cast<int>(af.frame_count));
    for (std::size_t i = 0; i < af.frame_count; ++i) {
        ppcp_instant out{};
        if (ppcp_achieved_frames_canonical_at(&af, &p->timing, i, &out) != PPCP_OK) {
            ++m_counters.unconvertible;
            m_open.clip.canonicalNs.clear();
            break;
        }
        m_open.clip.canonicalNs.append(static_cast<qint64>(out.ns));
        std::int64_t exp = 0;
        if (ppcp_achieved_frames_exposure_at(&af, i, &exp) == PPCP_OK)
            m_open.clip.exposureNs.append(static_cast<qint64>(exp));
    }
    if (!m_open.clip.canonicalNs.isEmpty()) {
        m_lastCanonicalNs = m_open.clip.canonicalNs;
        m_lastCanonicalTb = m_open.clip.timebaseId;
        m_lastInstantSourceNs = m_open.clip.canonicalNs.first();
        m_haveLastInstant = true;
        m_haveLastGeometry = p->has_geometry;
        if (p->has_geometry) m_lastGeometry = p->geometry;
    }
    m_open.clip.payload.reserve(static_cast<int>(b.bytes));
}

void VideoInputPpcp::onPayloadChunk(const ppcp_msg *m)
{
    const ppcp_body_payload_chunk &b = m->body.payload_chunk;
    if (!m_open.active || idStr(b.capture_id) != m_open.clip.captureId) return;
    if (!b.data || b.data_len == 0) return;
    // ENC 6b — `offset` is index x chunk_bytes and travels on every chunk, so a
    // chunk that arrives out of order is PLACED, not appended.  On one bulk
    // channel they are ascending (8.3a); placing anyway is what makes the same
    // code work when they are not.
    const qsizetype need = static_cast<qsizetype>(b.offset) + static_cast<qsizetype>(b.data_len);
    if (m_open.clip.payload.size() < need) m_open.clip.payload.resize(need);
    std::memcpy(m_open.clip.payload.data() + b.offset, b.data, b.data_len);
}

void VideoInputPpcp::onPayloadEnd(const ppcp_msg *m)
{
    if (!m_open.active) return;
    if (idStr(m->body.payload_end.capture_id) != m_open.clip.captureId) return;
    deliver();
}

void VideoInputPpcp::onPayloadAbort(const ppcp_msg *m)
{
    if (!m_open.active) return;
    if (idStr(m->body.payload_abort.capture_id) != m_open.clip.captureId) return;
    // 8.3c — `already_present` is not a failure, and neither reaches a frame.
    // The Capture stays announced; what it does NOT do is arrive as data.
    m_open = Open{};
}

void VideoInputPpcp::deliver()
{
    PpcpClip clip = m_open.clip;
    m_open = Open{};

    if (clip.preview) {
        ++m_counters.previewFrames;
        emitPreviewFrame(clip);
        return;
    }
    // ⚠ A CLIP IS NOT A LIVE FRAME.  It leaves with its canonical instants
    // attached; pushing it through videoFrameReady() would hand it to a
    // consumer that stamps EventBuffer::nowMicros() on arrival and throw the
    // conversion away.  See the header note and F7.
    ++m_counters.clips;
    emit clipReady(clip);
}

void VideoInputPpcp::emitPreviewFrame(const PpcpClip &clip)
{
    // ENC §6 declares no container for a payload (finding 3 in this repository's
    // conformance claim), so a consumer must decide what the bytes are from the
    // profile's `format`.  Two cases are handled and a third is refused rather
    // than guessed: raw at exactly the declared size, an encoded still QImage
    // can read, and anything else.
    const int w = m_frameWidth, h = m_frameHeight;
    const QVideoFrameFormat::PixelFormat pf = qtFormatFor(m_previewPixelFormat);

    const int bpp = packedBytesPerPixel(pf);
    if (w > 0 && h > 0 && pf != QVideoFrameFormat::Format_Invalid && bpp > 0
            && clip.payload.size() >= static_cast<qsizetype>(w) * h * bpp) {
        QVideoFrameFormat fmt(QSize(w, h), pf);
        QVideoFrame frame(fmt);
        if (frame.map(QVideoFrame::WriteOnly)) {
            // Row by row, because the mapped buffer's stride is Qt's business
            // and need not equal w * bpp — a flat memcpy of the payload would
            // shear the image on any backend that pads.
            const int dstStride = frame.bytesPerLine(0);
            const int srcStride = w * bpp;
            uchar *dst = frame.bits(0);
            if (dst && dstStride >= srcStride) {
                for (int y = 0; y < h; ++y)
                    std::memcpy(dst + static_cast<qsizetype>(y) * dstStride,
                                clip.payload.constData() + static_cast<qsizetype>(y) * srcStride,
                                static_cast<std::size_t>(srcStride));
                frame.unmap();
                emit videoFrameReady(frame);
                return;
            }
            frame.unmap();
        }
    }

    QImage img;
    if (img.loadFromData(clip.payload)) {
        emit videoFrameReady(QVideoFrame(img.convertToFormat(QImage::Format_RGBA8888)));
        return;
    }
    ++m_counters.decodeFailures;
}
