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

// The host's OWN declaration: `declare` as PinPointStudio sends it.  Work
// package H2, first half — it needs no peer engine, only the entity vocabulary.
//
// WHY A HOST DECLARES AT ALL, which is the part that looks like busywork until
// it is missing.  CORE §5.6.1: every field of a Source and its profiles is
// declared by BOTH sides.  "If only devices declared, a host would hardcode its
// own cameras' convention.  That works for one implementation and fails the
// open-protocol commitment immediately: a third-party host with different
// cameras cannot participate, because the conversion has been baked into an
// implementation instead of carried by the protocol."  CT-S3 exists because the
// reference host passes I19 BY ACCIDENT — its hardcoded conventions are right,
// so every test it runs against itself is green and nothing is on the wire.
//
// I19 (CORE 5.6a) in one line: every Source declares `timebase_id`, and every
// CaptureProfile it offers declares `timing`, `geometry` and `intrinsics`,
// REGARDLESS OF WHICH PEER OWNS THE SOURCE.  No convention is implied by peer
// role, product or platform.
//
// MSG 3.3d: a host owning no Sources sends `declare` with an EMPTY `sources`
// list.  It does not skip the message, and Source count is not a role marker.
//
// I31 / plan A12: every timing constant nobody has measured is `assumed`.  No
// model in this programme has been through a timecode rig, so nothing here is
// `measured` and nothing derives a `measured` from a claimed value (I28).
//
// ⚠ WHAT THIS CLASS DELIBERATELY DOES NOT DO.  It never reads a convention, a
// geometry or a readout time out of a COUNTERPART's declaration, a product
// string, a role or a platform identifier — CT-S3 assertion 3.  It maps this
// host's own hardware, which is the one place that mapping is legitimate, and
// the conversion of CORE §6.1 is applied from what arrived on the wire.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <ppcp/common.h>
#include <ppcp/model.h>
#include <ppcp/time.h>
#include <ppcp/timing.h>

#include "../Video/camera_capabilities.h"
#include "../Video/video_input_factory.h"

namespace Ppcp {

// The host's declared Timebase.  One id, used by every Source the host owns,
// because they are all sampled against the same host clock.
inline constexpr const char *kHostTimebaseId = "tb:host";

// CORE 5.3 — the host's monotonic clock, declared as it actually behaves.  See
// the .cpp for why this is `monotonic` on every platform and not `continuous`
// on the ones where the counter might survive a sleep.
ppcp_timebase hostTimebase();

// The value of that clock now, in nanoseconds.  This is the function H2's peer
// adapter hands to `ppcp_clock` (the library owns no clock — ground rule 7).
int64_t hostNowNs();

// The same thing as libppcp's injectable clock interface.  Answers only for
// `tb:host`; any other timebase id is PPCP_ERR_NOT_FOUND, because a clock that
// answered "what time is it" without being told which clock is I1's defect one
// layer above the wire.
ppcp_clock hostClock();

// Builds and owns one `declare`-worth of entities: the Peer, its Timebase, its
// Sources and their CaptureProfiles.  libppcp allocates nothing and its structs
// hold borrowed pointers, so the storage has to live somewhere; it lives here,
// and every pointer inside peer() points into this object.
class PpcpSourceDeclaration {
public:
    // ── What the host knows about its own capture hardware ─────────────────
    //
    // A plain snapshot rather than a live VideoInputBase, for two reasons: the
    // declaration must be buildable in a unit test with no camera attached
    // (which is most of CT-S3), and CameraCapabilities is already the shape the
    // three backends normalise onto.
    struct Camera {
        VideoInputFactory::Backend backend = VideoInputFactory::Backend::Auto;
        std::string id;        // DeviceEnumerator's device id
        std::string label;     // human-readable, informational (5.6 `label`)
        CameraCapabilities caps;
    };

    struct Microphone {
        std::string id;
        std::string label;
    };

    struct Inventory {
        std::vector<Camera> cameras;
        bool hasMicrophone = false;
        Microphone microphone;
    };

    // Reads the inventory from DeviceEnumerator.  The ONLY function here that
    // knows what a QString is, and the only one that touches global state —
    // everything else is a pure function of an Inventory, which is what makes
    // the CT-S3 evidence reproducible on a machine with no cameras.
    static Inventory hostInventory();

    PpcpSourceDeclaration();
    ~PpcpSourceDeclaration();
    PpcpSourceDeclaration(const PpcpSourceDeclaration &) = delete;
    PpcpSourceDeclaration &operator=(const PpcpSourceDeclaration &) = delete;

    // Builds the declaration.  An empty inventory produces a valid Peer with an
    // EMPTY sources list (MSG 3.3d), which is a normal outcome and not an error.
    bool build(const std::string &peerId, const Inventory &inv, std::string *err = nullptr);

    // Valid until the next build().  Null before the first one.
    const ppcp_peer_desc *peer() const;

    // Every profile, every Source and the Peer, through libppcp's own
    // validators — the ones that carry I19, I22, I27, I28 and I31.  `where`
    // names the entity that failed, so a red test says which.
    ppcp_result validate(std::string *where = nullptr) const;

    // The id of the first Source of a given `kind` ("microphone", "camera"),
    // or empty where this host declared none.  Empty is a NORMAL answer: a Mac
    // with no audio input declares no microphone Source at all (MSG 3.3d), and
    // a caller must treat that as "we have none" rather than as a failure.
    //
    // ⚠ MATCH ON `kind`, NEVER ON THE ID CONVENTION.  A caller that rebuilt the
    // id as `"mic:" + deviceId` would be wrong for a long device id: `clampId()`
    // keeps the LAST PPCP_ID_MAX bytes of the whole string, so a CoreAudio UID
    // over 60 bytes silently loses the `mic:` prefix.  The conformance harness
    // made exactly that mistake and was refused by I26, which is the check
    // doing its job.  This exists so nobody makes it a third time — the loop
    // was already written twice in the test tree before it was written here.
    std::string sourceIdOfKind(const char *kind) const;

    std::size_t sourceCount() const { return m_sources.size(); }
    std::size_t profileCount() const { return m_profiles.size(); }
    const std::vector<ppcp_source> &sources() const { return m_sources; }
    const std::vector<ppcp_capture_profile> &profiles() const { return m_profiles; }

    // The conformance profile set PinPointStudio claims (plan §2, CORE §2.2.3).
    static const std::vector<std::string> &studioProfiles();

private:
    // Stable backing for every string a ppcp_id was built from.  A deque and
    // not a vector: libppcp copies ids by value, but the Inventory's own
    // strings do not outlive the call, so anything built from them is copied
    // here first and never moved afterwards.
    std::deque<std::string> m_strings;
    const char *intern(const std::string &s);

    std::vector<ppcp_capture_profile> m_profiles;
    std::vector<ppcp_source>          m_sources;
    std::vector<ppcp_timebase>        m_timebases;
    std::vector<ppcp_id>              m_declaredProfiles;
    ppcp_peer_desc                    m_peer{};
    bool                              m_built = false;
};

}  // namespace Ppcp
