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

// PPCP-RV §3 service discovery — THE BROWSER HALF, AND ONLY THE BROWSER HALF.
//
// RV 3.5b: "a capture peer advertises and a host browses.  Browsing needs only
// the querier role; advertising needs a responder, which a mobile platform
// supplies and several desktop platforms do not."  This host therefore
// registers NOTHING, answers NOTHING, and never binds UDP 5353 — it asks the
// platform's existing responder (mDNSResponder on macOS) to browse on its
// behalf over a local IPC socket.  There is no QUdpSocket anywhere in this
// file and there must never be one: binding 5353 in this process would
// conflict with the responder that already owns it.
//
// ⚠ AND THE WHOLE THING IS A CONVENIENCE.  RV §3 is "optional.  Reconnection
// convenience only — a first pairing always uses §4", and 3.6a MUST NOT treat
// discovery failure as an error state: "multicast is rate-limited or dropped by
// many consumer access points, blocked by client isolation on guest networks,
// and does not cross VLAN boundaries.  IT WILL NOT WORK AT A RANGE."  So this
// class has no error signal, only results, and its absence is silent (3.6b).
//
// ⚠ 3.4c IS THE ONE HARD RULE HERE: a browsing peer MUST NOT connect to an
// instance whose `rid` it cannot resolve.  `decideDial()` below is the only
// function that answers the question, it takes the resolver as its argument,
// and it has no "unknown" branch that connects anyway.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ppcp/rv.h>

namespace Ppcp {

// One discovered `_ppcp._tcp` instance, as far as it has been resolved.
struct RvAdvertisement {
    std::string   instanceName;    // 3.2a — "PPCP-" + 8 uppercase hex
    std::string   host;            // resolved target, or an address literal
    std::uint16_t port = 0;

    // 3.3a's TXT keys.  Anything else in the record is IGNORED and is not an
    // error — the same rule as 4.2c, one layer out.
    std::string   txtvers;
    std::string   pv;              // e.g. "1.0" or "1.0-1.2"
    std::string   role;            // host | capture | observer
    bool          hasRn = false;
    std::uint8_t  rn[PPCP_RV_RN_BYTES]{};
    bool          hasRid = false;
    std::uint8_t  rid[PPCP_RV_RID_BYTES]{};
};

// DNS-SD TXT is a sequence of length-prefixed "key=value" strings.  Returns
// false only when the buffer is malformed as a TXT record; an unknown key, a
// missing key or a value of the wrong length leaves the corresponding field
// unset and is NOT a parse failure (3.3a: "a receiver ignores any key it does
// not recognise").
bool parseTxtRecord(const std::uint8_t *txt, std::size_t len, RvAdvertisement *out);

// 3.3a — "a browser filters on MAJOR before connecting".  `pv` is either a
// single version or an inclusive range `lo-hi`; this answers whether `major`
// falls inside it.  An unparseable or absent `pv` is NOT acceptable: it is
// exactly the case where connecting would waste a dial on a peer that cannot
// speak to us.
bool pvAcceptsMajor(const std::string &pv, int major);

// 3.2a — the instance name a given `rid` must produce.  A name that disagrees
// with the TXT record's own `rid` is a malformed advertisement, and saying so
// is free.
bool instanceNameMatchesRid(const std::string &instanceName,
                            const std::uint8_t rid[PPCP_RV_RID_BYTES]);

// The decision of 3.4b/3.4c.  `resolve` is PpcpRendezvous::resolveRid — it
// returns the pairing handle on a match and an empty string otherwise.
struct DialDecision {
    bool        dial = false;
    std::string pairingId;      // empty unless `dial`
    // Diagnostic only.  RV 7.2b is satisfied because none of these name a key,
    // a secret or a payload; `rid` is published in the clear by design (3.4e).
    const char *why = "";
};
using RidResolver =
    std::function<std::string(const std::uint8_t rn[PPCP_RV_RN_BYTES],
                              const std::uint8_t rid[PPCP_RV_RID_BYTES])>;

DialDecision decideDial(const RvAdvertisement &ad, int wireMajor, const RidResolver &resolve);

// ── The platform browser ────────────────────────────────────────────────────
// Wraps the system DNS-SD client.  Owns no thread: it exposes the client
// socket so a Qt event loop (or a headless harness) can watch it and call
// process() when it is readable.  Returns null where the platform has no
// DNS-SD client, and that is not an error either (3.6a).
class RvBrowser {
public:
    virtual ~RvBrowser() = default;

    // Called for each instance that resolves far enough to carry rn and rid.
    // Called on whichever thread invoked process().
    using FoundFn = std::function<void(const RvAdvertisement &)>;
    using LostFn = std::function<void(const std::string &instanceName)>;

    virtual bool start(FoundFn onFound, LostFn onLost) = 0;
    virtual void stop() = 0;
    // The client socket to watch for readability, or -1.
    virtual int  fd() const = 0;
    // Handle whatever is ready.  Returns false once the browse has died, which
    // 3.6a makes a reason to stop watching and NOT a reason to report a fault.
    virtual bool process() = 0;
    // Whether this build has a browser at all, for the diagnostic export.
    virtual std::string describe() const = 0;
};

std::unique_ptr<RvBrowser> makePlatformBrowser();

}  // namespace Ppcp
