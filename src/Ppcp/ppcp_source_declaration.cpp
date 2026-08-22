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

#include "ppcp_source_declaration.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace Ppcp {
namespace {

// ── How many profiles one Source declares ───────────────────────────────────
//
// An industrial camera with a free ROI has an unbounded profile set: any width
// by any height by any pixel format.  A declaration is a snapshot that has to
// fit in a control frame (ENC §8: 1 MiB), so the host declares the modes it
// would actually open rather than the Cartesian product of what the sensor can
// be talked into.
//
// ⚠ THIS IS A DECLARATION-SIZE POLICY, NOT A PROTOCOL THRESHOLD.  I14 forbids a
// frame-rate, resolution, quality or confidence threshold in the protocol
// layer, and this is none of those: it is a bound on how much of a truthful
// declaration fits, decided by this application, changeable without any peer
// noticing.  The 120 fps ingest floor — a real threshold — lives in H2's
// ingest-policy callback and nowhere near here.
constexpr std::size_t kMaxProfilesPerSource = 16;

// CORE 5.7 `format` — { codec, width, height, pixel_format }.  `codec` is what
// the bytes are; `pixel_format` is how the samples are laid out inside them.
// Both are open-registry strings, so an encoding this table does not know
// round-trips under its native key rather than being dropped (10.3a, I13).
const char *codecFor(PixelEncoding e)
{
    switch (e) {
    case PixelEncoding::MJPEG: return "mjpeg";
    case PixelEncoding::H264:  return "h264";
    case PixelEncoding::H265:  return "h265";
    default:                   return "raw";
    }
}

std::string pixelFormatKey(const PixelFormat &pf)
{
    if (!pf.nativeKey.isEmpty()) return pf.nativeKey.toStdString();
    return "unknown";
}

// CORE 5.1a — an Id MUST NOT be derived from mutable local state.  A serial
// number is the hardware's; a device node path or a port-order index is not, so
// the serial wins wherever a backend could recover one and the enumerator's
// device id is the fallback.  Truncated to the 64 bytes of PPCP_ID_MAX from the
// FRONT, because the distinguishing end of a serial or a UUID is the tail.
std::string sourceIdFor(const PpcpSourceDeclaration::Camera &cam)
{
    std::string base = "cam:";
    if (!cam.caps.serialNumber.isEmpty()) base += cam.caps.serialNumber.toStdString();
    else if (!cam.id.empty())             base += cam.id;
    else                                  base += cam.caps.modelName.toStdString();

    if (base.size() > PPCP_ID_MAX) base = "cam:" + base.substr(base.size() - (PPCP_ID_MAX - 4));
    return base;
}

std::string clampId(std::string s, const char *fallback)
{
    if (s.empty()) return fallback;
    if (s.size() > PPCP_ID_MAX) s = s.substr(s.size() - PPCP_ID_MAX);
    return s;
}

// ── The timing a host camera declares, per backend ──────────────────────────
//
// ⚠ READ CT-S3 ASSERTION 3 BEFORE CHANGING THIS.  "No code path infers a
// convention, geometry or readout time from `Peer.product`, from `role`, or
// from a platform identifier."  That assertion is about a CONSUMER: a peer that
// works out what a COUNTERPART's timestamps mean from anything other than what
// the counterpart declared.  A peer describing its OWN hardware is the one
// place the mapping is legitimate, and refusing to do it here is how a host
// ends up with nothing on the wire and I19 passing by accident.
//
// The values themselves come from CORE §5.6.1's own table and plan decision
// A12, not from measurement — no model in this programme has been through a
// timecode rig — which is why every provenance below is `assumed`.
bool timingFor(VideoInputFactory::Backend backend, ppcp_timing *out)
{
    switch (backend) {
    case VideoInputFactory::Backend::Spinnaker:
    case VideoInputFactory::Backend::Aravis:
        // CORE 5.6.1: a host Source declares `timing.convention: start` (FLIR).
        // GenICam machine-vision cameras timestamp the start of exposure.
        // I22 makes the offset unrepresentable here — it exists if and only if
        // the convention is `nominal_frame_start` — so there is no unmeasured
        // constant on this path to give a provenance to.
        return ppcp_timing_make(out, PPCP_CONV_START) == PPCP_OK;

    case VideoInputFactory::Backend::AppleAVFoundation:
    case VideoInputFactory::Backend::QtMultimedia:
    case VideoInputFactory::Backend::Auto:
    default:
        // "`nominal_frame_start` is what EVERY AVFoundation source declares"
        // (CORE 5.7), and Qt Multimedia sits on AVFoundation on macOS and on
        // V4L2/Media Foundation elsewhere, none of which states where exposure
        // begins relative to the frame's nominal start.  A12: the offset is
        // declared explicitly, as zero, and `assumed` — 5.7b, because a
        // declared zero is a checkable claim and an omitted field is not, while
        // a declared zero with NO provenance is indistinguishable from a
        // measured one.
        return ppcp_timing_make_nominal_frame_start(out, /*offset_ns=*/0,
                                                    PPCP_PROV_ASSUMED) == PPCP_OK;
    }
}

bool geometryFor(VideoInputFactory::Backend backend, uint32_t rows, ppcp_geometry *out)
{
    switch (backend) {
    case VideoInputFactory::Backend::Spinnaker:
    case VideoInputFactory::Backend::Aravis:
        // CORE 5.6.1 again: a host Source declares `geometry: global`.  The
        // machine-vision cameras this product ships with are global shutter,
        // and a global geometry has no readout time, so I31 has nothing to
        // attach a provenance to.
        return ppcp_geometry_make_global(out) == PPCP_OK;

    default:
        // Rolling shutter, readout unmeasured.  A12 names this case
        // explicitly: `readout_ns` on every AVFoundation profile is `assumed`
        // until the rig exists.  Zero is the honest placeholder and it is
        // declared, not omitted, so a consumer can see that the claim was made
        // and that nobody stands behind it (5.7e).
        //
        // ⚠ A zero readout is NOT a claim of global shutter.  The geometry says
        // `rolling_shutter` and the provenance says `assumed`; a consumer that
        // treats an assumed zero as a measured zero has ignored the field that
        // exists to stop it.
        return ppcp_geometry_make_rolling_shutter(out, /*readout_ns=*/0,
                                                  PPCP_PROV_ASSUMED,
                                                  PPCP_ROLL_TOP_TO_BOTTOM,
                                                  rows) == PPCP_OK;
    }
}

// The resolutions this host would actually open on a camera.
std::vector<Resolution> resolutionsOf(const CameraCapabilities &caps)
{
    std::vector<Resolution> out;
    if (caps.resolution.kind == CapabilityKind::Discrete) {
        for (const Resolution &r : caps.resolution.presets)
            if (r.width > 0 && r.height > 0) out.push_back(r);
    }
    // A free-ROI camera (GenICam width/height ranges) has no preset list, and
    // enumerating the range would be a declaration of modes nobody asked for.
    // The current region is what the camera is set to and what a Stream would
    // get, so that is what is declared.
    if (out.empty() && caps.resolution.defaultResolution.width > 0)
        out.push_back(caps.resolution.defaultResolution);
    return out;
}

std::vector<PixelFormat> formatsOf(const CameraCapabilities &caps)
{
    std::vector<PixelFormat> out;
    for (const PixelFormat &pf : caps.pixelFormat.supported)
        if (!pf.nativeKey.isEmpty()) out.push_back(pf);
    if (out.empty() && !caps.pixelFormat.defaultFormat.nativeKey.isEmpty())
        out.push_back(caps.pixelFormat.defaultFormat);
    return out;
}

// CORE 5.7 `rate` is millihertz, "so 150 fps is 150000.  Avoids a float on the
// wire for a value used in scheduling."
int64_t mhzOf(double fps)
{
    if (fps <= 0.0) return 0;
    return static_cast<int64_t>(fps * 1000.0 + 0.5);
}

}  // namespace

// ── The host clock, declared as it actually behaves ─────────────────────────

ppcp_timebase hostTimebase()
{
    ppcp_timebase tb{};

    // std::chrono::steady_clock's tick, as the standard library reports it.
    // Read rather than assumed: a platform whose steady_clock is
    // microsecond-ticked must not declare nanosecond resolution, because a
    // consumer sizes its uncertainty from this field.
    using P = std::chrono::steady_clock::period;
    const int64_t resolution_ns =
        std::max<int64_t>(1, static_cast<int64_t>(1000000000LL * P::num / P::den));

    // ⚠ WHY `monotonic` ON EVERY PLATFORM, INCLUDING THE ONES WHERE THE COUNTER
    // MIGHT SURVIVE A SLEEP.  CORE 5.3: `monotonic` halts across device sleep,
    // `continuous` does not.  macOS's steady_clock is CLOCK_UPTIME_RAW and
    // Linux's is CLOCK_MONOTONIC — both halt, so both are `monotonic` with no
    // ambiguity.  Windows' QueryPerformanceCounter is documented differently
    // across hardware generations, and we have not measured it.
    //
    // The two errors are not symmetric.  Declaring `monotonic` where the clock
    // is really continuous costs a consumer nothing: it allows for a gap that
    // never comes.  Declaring `continuous` where the clock really halts makes a
    // consumer trust an elapsed time across a sleep that did not elapse.  So
    // the unmeasured platform declares the claim that cannot mislead, and this
    // comment is the record of that being a decision rather than an oversight.
    ppcp_timebase_make(&tb, kHostTimebaseId, std::strlen(kHostTimebaseId),
                       PPCP_TB_MONOTONIC, /*epoch_stable=*/false, resolution_ns);
    return tb;
}

int64_t hostNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace {
ppcp_result hostClockNow(void * /*ctx*/, const char *timebase_id, int64_t *out_ns)
{
    // I1 one layer above the wire: the clock is asked for a NAMED timebase and
    // answers only for the one it is.  A host that answered for `tb:device`
    // would be inventing a reading of somebody else's clock.
    if (!timebase_id || std::strcmp(timebase_id, kHostTimebaseId) != 0)
        return PPCP_ERR_NOT_FOUND;
    if (!out_ns) return PPCP_ERR_INVALID;
    *out_ns = hostNowNs();
    return PPCP_OK;
}
}  // namespace

ppcp_clock hostClock()
{
    ppcp_clock c{};
    c.now = hostClockNow;
    c.ctx = nullptr;
    return c;
}

// ── The declaration ─────────────────────────────────────────────────────────

PpcpSourceDeclaration::PpcpSourceDeclaration() = default;
PpcpSourceDeclaration::~PpcpSourceDeclaration() = default;

const std::vector<std::string> &PpcpSourceDeclaration::studioProfiles()
{
    // Plan §2 / CORE §2.2.3.  NOT Mint: PinPointStudio arbitrates, it does not
    // mint, and CONF §1d makes the negative half of that a conformance
    // obligation — a peer that mints without declaring Mint fails.
    static const std::vector<std::string> p = {
        "core", "capture", "detect", "arbitrate", "live", "offline", "markup"
    };
    return p;
}

const char *PpcpSourceDeclaration::intern(const std::string &s)
{
    m_strings.push_back(s);
    return m_strings.back().c_str();
}

const ppcp_peer_desc *PpcpSourceDeclaration::peer() const
{
    return m_built ? &m_peer : nullptr;
}

bool PpcpSourceDeclaration::build(const std::string &peerId, const Inventory &inv,
                                  std::string *err)
{
    auto fail = [&](const char *why) {
        if (err) *err = why;
        m_built = false;
        return false;
    };

    m_strings.clear();
    m_profiles.clear();
    m_sources.clear();
    m_timebases.clear();
    m_declaredProfiles.clear();
    m_peer = ppcp_peer_desc{};
    m_built = false;

    if (peerId.empty() || peerId.size() > PPCP_ID_MAX) return fail("peer id is not a valid Id");

    // 3.3b: every `timebase_id` referenced by any Source appears in `timebases`.
    // The host has exactly one clock, so there is exactly one.
    m_timebases.push_back(hostTimebase());

    // ── Profiles first, in one contiguous block per Source ─────────────────
    // libppcp allocates nothing: a ppcp_source holds a BORROWED pointer into
    // this vector, so every profile must exist before any Source names them, or
    // a reallocation halfway through would leave the earlier Sources pointing
    // at freed storage.
    struct Span { std::size_t off, count; };
    std::vector<Span> camSpans;
    camSpans.reserve(inv.cameras.size());

    std::size_t reserve = 1;   // the microphone's
    for (const Camera &cam : inv.cameras) reserve += kMaxProfilesPerSource;
    m_profiles.reserve(reserve);

    for (const Camera &cam : inv.cameras) {
        const std::size_t off = m_profiles.size();

        ppcp_timing timing{};
        if (!timingFor(cam.backend, &timing)) return fail("timing could not be constructed");

        const std::vector<Resolution> resolutions = resolutionsOf(cam.caps);
        const std::vector<PixelFormat> formats = formatsOf(cam.caps);

        for (const Resolution &r : resolutions) {
            for (const PixelFormat &pf : formats) {
                if (m_profiles.size() - off >= kMaxProfilesPerSource) break;

                ppcp_geometry geometry{};
                if (!geometryFor(cam.backend, static_cast<uint32_t>(r.height), &geometry))
                    return fail("geometry could not be constructed");

                // A profile id unique within its Source, and readable, because
                // a `declare_ack` may reject one by name (MSG 3.4c).
                const std::string pid = std::to_string(r.width) + "x" + std::to_string(r.height)
                                        + "@" + pixelFormatKey(pf);

                ppcp_capture_profile p{};
                if (ppcp_capture_profile_make(&p, intern(clampId(pid, "p")), &timing) != PPCP_OK)
                    return fail("capture profile could not be constructed");

                // I19 — a camera profile carries geometry AND intrinsics.
                //
                // `fixed` and not `per_frame`: `per_frame` is a claim that the
                // pipeline attaches an intrinsic matrix to every frame, and no
                // backend this host uses delivers one.  Declaring the stronger
                // form because a phone can is the same error as reporting a
                // cold sample as sustained (the discipline of 5.8h, applied one
                // field over).
                if (ppcp_capture_profile_set_camera(&p, &geometry, PPCP_INTR_FIXED) != PPCP_OK)
                    return fail("camera profile fields could not be set");

                if (ppcp_capture_profile_set_format(&p, codecFor(pf.encoding),
                                                    static_cast<uint32_t>(r.width),
                                                    static_cast<uint32_t>(r.height),
                                                    intern(pixelFormatKey(pf))) != PPCP_OK)
                    return fail("format could not be set");

                if (cam.caps.frameRate.kind == CapabilityKind::Range) {
                    const CapabilityRange<double> &fr = cam.caps.frameRate.range;
                    const int64_t nominal = mhzOf(fr.defaultValue > 0 ? fr.defaultValue : fr.max);
                    if (nominal > 0)
                        ppcp_capture_profile_set_rate(&p, nominal, mhzOf(fr.min), mhzOf(fr.max));
                } else if (cam.caps.frameRate.kind == CapabilityKind::Fixed) {
                    const int64_t v = mhzOf(cam.caps.frameRate.fixedValue);
                    if (v > 0) ppcp_capture_profile_set_rate(&p, v, v, v);
                }

                // ⚠ `optical` IS DELIBERATELY OMITTED, and the reason is worth
                // recording.  CORE 5.7 makes it one field —
                // { exposure_min_ns, exposure_max_ns, iso_min, iso_max, … } —
                // with no way to declare the exposure half without the ISO
                // half.  A GenICam camera reports GAIN IN dB and has no ISO at
                // all, so declaring an ISO range for it would be inventing a
                // number, which is exactly what I28 and I31 exist to prevent.
                // The exposure range is therefore withheld rather than
                // half-invented.  Reported to the orchestrator: 5.7 `optical`
                // needs per-field optionality, or a gain field beside iso.

                // No `measured`.  I28: absence means NOT MEASURED, and a peer
                // MUST NOT synthesise a MeasuredCapability from claimed values,
                // from a device profile table, or from a previous model.  There
                // is no self-test on the host path yet; when there is, it sets
                // this and nothing else may.

                m_profiles.push_back(p);
            }
        }

        camSpans.push_back({ off, m_profiles.size() - off });
    }

    std::size_t micOff = 0, micCount = 0;
    if (inv.hasMicrophone) {
        micOff = m_profiles.size();

        // CORE 5.7 Timing `mid` — the sample's timestamp is the middle of what
        // it integrates.  A microphone has no frames, so `nominal_frame_start`
        // has nothing to be nominal about, and I22 makes the offset
        // unrepresentable outside that convention anyway.
        ppcp_timing timing{};
        if (ppcp_timing_make(&timing, PPCP_CONV_MID) != PPCP_OK)
            return fail("microphone timing could not be constructed");

        // No `format`: 5.7 gives it to "camera and other framed sources", and
        // audio is not framed.  No `geometry` and no `intrinsics` either —
        // 5.6a's "every profile declares timing, geometry and intrinsics" is
        // I19 read with 5.7's own cardinality column, where geometry and
        // intrinsics are "camera: 1" and a microphone has neither.
        ppcp_capture_profile p{};
        if (ppcp_capture_profile_make(&p, "audio", &timing) != PPCP_OK)
            return fail("microphone profile could not be constructed");
        m_profiles.push_back(p);
        micCount = 1;
    }

    // ── Then the Sources, now that the profile storage is final ────────────
    m_sources.reserve(inv.cameras.size() + (inv.hasMicrophone ? 1 : 0));

    for (std::size_t i = 0; i < inv.cameras.size(); ++i) {
        const Camera &cam = inv.cameras[i];
        if (camSpans[i].count == 0) continue;   // a camera that offers no mode is not a Source

        ppcp_source s{};
        if (ppcp_source_make(&s, intern(sourceIdFor(cam)), intern(peerId), "camera",
                             kHostTimebaseId,
                             // 5.6b: `physical: false` is for a VIRTUAL
                             // multi-lens device that switches sensors on scene
                             // and focus distance.  A machine-vision camera and
                             // a webcam are each one sensor behind one lens.
                             /*physical=*/true,
                             m_profiles.data() + camSpans[i].off,
                             camSpans[i].count) != PPCP_OK)
            return fail("camera Source could not be constructed");

        if (!cam.label.empty())
            ppcp_source_set_label(&s, intern(clampId(cam.label, "camera")));

        // No `viewpoint`.  5.6e: it declares HOW it was arrived at, and this
        // host neither asks the user where a camera is looking from nor
        // classifies it.  Absent is the honest answer; a `declared` viewpoint
        // nobody declared would be the invention 5.6e is written against.
        //
        // No `calibration` either, until the rig exists — 5.9's uncertainty is
        // mandatory, so there is no way to offer a calibration without one.

        m_sources.push_back(s);
    }

    if (micCount > 0) {
        ppcp_source s{};
        const std::string mid = clampId(inv.microphone.id.empty()
                                            ? std::string("mic")
                                            : "mic:" + inv.microphone.id,
                                        "mic");
        if (ppcp_source_make(&s, intern(mid), intern(peerId), "microphone", kHostTimebaseId,
                             /*physical=*/true, m_profiles.data() + micOff, micCount) != PPCP_OK)
            return fail("microphone Source could not be constructed");
        if (!inv.microphone.label.empty())
            ppcp_source_set_label(&s, intern(clampId(inv.microphone.label, "microphone")));
        m_sources.push_back(s);
    }

    // ── And the Peer ───────────────────────────────────────────────────────
    m_declaredProfiles.reserve(studioProfiles().size());
    for (const std::string &name : studioProfiles()) {
        ppcp_id id{};
        if (ppcp_id_set(&id, name.c_str(), name.size()) != PPCP_OK)
            return fail("declared profile name is not a valid Id");
        m_declaredProfiles.push_back(id);
    }

    if (ppcp_peer_desc_make(&m_peer, intern(peerId), PPCP_ROLE_HOST, ppcp_wire_version(),
                            m_declaredProfiles.data(), m_declaredProfiles.size(),
                            m_timebases.data(), m_timebases.size()) != PPCP_OK)
        return fail("peer declaration could not be constructed");

    // MSG 3.3d — a host owning no Sources sends `declare` with an EMPTY
    // `sources` list.  It does not skip the message, and this call is made even
    // when the vector is empty so that the empty list is a stated fact rather
    // than a field nobody set.
    if (ppcp_peer_desc_set_sources(&m_peer, m_sources.empty() ? nullptr : m_sources.data(),
                                   m_sources.size()) != PPCP_OK)
        return fail("sources could not be attached to the declaration");

    // 5.2c `product` is informational and, per I19, is never used to infer
    // behaviour — which is exactly why it is safe to send.
    ppcp_peer_desc_set_product(&m_peer, "PinPoint Golf", "PinPointStudio",
                               ppcp_library_version());

    // No `relations`.  CORE 5.4: a relation is measured, and the host has
    // exactly one timebase, so there is no pair to relate until a counterpart
    // declares one and the sync burst of §6.3 has run (L9).  An `unrelated`
    // relation to nothing is not a thing.

    m_built = true;
    return true;
}

ppcp_result PpcpSourceDeclaration::validate(std::string *where) const
{
    if (!m_built) {
        if (where) *where = "nothing built";
        return PPCP_ERR_INVALID;
    }

    for (const ppcp_timebase &tb : m_timebases) {
        const ppcp_result r = ppcp_timebase_validate(&tb);
        if (r != PPCP_OK) {
            if (where) *where = std::string("timebase ") + tb.id.v;
            return r;
        }
    }

    for (const ppcp_capture_profile &p : m_profiles) {
        const ppcp_result r = ppcp_capture_profile_validate(&p);
        if (r != PPCP_OK) {
            if (where) *where = std::string("profile ") + p.id.v;
            return r;
        }
    }

    for (const ppcp_source &s : m_sources) {
        const ppcp_result r = ppcp_source_validate(&s);
        if (r != PPCP_OK) {
            if (where) *where = std::string("source ") + s.id.v;
            return r;
        }
    }

    const ppcp_result r = ppcp_peer_desc_validate(&m_peer);
    if (r != PPCP_OK && where) *where = std::string("peer ") + m_peer.id.v;
    return r;
}

}  // namespace Ppcp
