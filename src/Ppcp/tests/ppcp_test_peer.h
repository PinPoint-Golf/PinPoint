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

// Two real `ppcp_peer`s in one process, with the bytes moved between them a
// frame at a time.  The H4 idiom, factored out because H5 and H7 need it too.
//
// ⚠ NOTHING HERE IS A MOCK.  The device end is a real engine configured as a
// capture peer; the host end is the one `makeHostEngine()` builds, which is the
// engine that ships.  A test against a differently-configured peer would be
// evidence about a peer that does not exist.
//
// ⚠ AND THE BYTES GO ONE FRAME AT A TIME.  peer.h: an event's `msg` is "valid
// until PPCP_PEER_EVENT_QUEUE further events have been queued", the queue is
// four deep, and a payload chunk's `data` points into the buffer the caller
// fed.  A test that handed a whole conversation over in one feed would be
// reading four-events-ago's bytes and would pass or fail on ring timing.
//
// ⚠ tools/ppcp-sim (libppcp L13) DOES NOT EXIST YET.  When it does, the rows
// this harness supports move from a same-process pair to a real independent
// implementation, which is what CONF §5's interoperability pairings ask for.
// Until then two engines in one process is the strongest available evidence and
// the claim file says so.

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ppcp/frame.h>
#include <ppcp/model.h>
#include <ppcp/peer.h>
#include <ppcp/version.h>

namespace pptest {

inline std::string idStr(const ppcp_id &id) { return std::string(id.v, id.len); }

// A capture peer, built the way a phone really is: role `capture`, an
// AVFoundation-shaped camera on its own clock, a microphone Source beside it so
// CT-I8's "two nominators of the same basis" is reachable, and Mint declared
// because 8.3d makes issuing a Shot the Mint profile's.
struct DevicePeer {
    std::vector<std::uint8_t>         storage;
    ppcp_peer                        *p = nullptr;
    std::vector<ppcp_capture_profile> profiles;
    std::vector<ppcp_source>          sources;
    std::vector<ppcp_timebase>        timebases;
    std::vector<ppcp_id>              declared;
    // CORE 5.19c / erratum E66 — OPT-IN, so every suite that already uses this
    // harness declares exactly what it declared before.  A phone owning no
    // Actuators omits the key from `declare` entirely and that is a complete
    // declaration, which is precisely what the default here is.
    std::vector<ppcp_actuator>        actuators;
    bool                              withActuator = false;
    // ⭐ 12.1c's CLAMP CASE, AND THE ONLY SHAPE IT IS EXPRESSIBLE IN.  An
    // `on_off` switch has nothing between on and off, so a device that
    // "achieved something other than what was asked" cannot be told apart from
    // one that echoed.  A `level` Actuator can, and since libppcp L30 the
    // engine no longer answers for us — the device below does — so the case
    // C2 reported as unreachable is reachable here and nowhere else.
    bool                              withLevelActuator = false;
    ppcp_peer_desc                    desc{};
    std::string                       peerId = "dev-1";
    std::string                       tb     = "tb:dev";
    std::int64_t                      clockNs = 0;

    ~DevicePeer() { if (p) ppcp_peer_free(p); }

    // ⚠ WITHOUT THIS EVERY `heartbeat` IS ANSWERED `error` /
    // `profile_not_supported` WITH "no health source", and liveness never
    // starts.  `ppcp_peer_config.health_report` reads as optional in peer.h —
    // "what `heartbeat_ack` carries" — and it is in fact a PRECONDITION for
    // 7.4a working at all.  Recorded as F-H5-3; the harness supplies one so the
    // other rows are about what they say they are about.
    static ppcp_result healthReport(void *ctx, ppcp_health *out)
    {
        DevicePeer *self = static_cast<DevicePeer *>(ctx);
        if (!self || !out) return PPCP_ERR_INVALID;
        *out = ppcp_health{};
        out->thermal = PPCP_THERMAL_NOMINAL;
        out->storage_free_bytes = 8ull * 1024 * 1024 * 1024;
        out->has_battery_pct = true;
        out->battery_pct = self->batteryPct;
        out->has_charging = true;
        out->charging = false;
        return PPCP_OK;
    }

    std::uint32_t batteryPct = 87;

    static ppcp_result clockNow(void *ctx, const char *timebase_id, std::int64_t *out)
    {
        DevicePeer *self = static_cast<DevicePeer *>(ctx);
        if (!self || !out) return PPCP_ERR_INVALID;
        // I1 one layer above the wire: a clock answers for the timebase it IS
        // and for no other.  A device clock asked for `tb:host` refuses.
        if (!timebase_id || self->tb != timebase_id) return PPCP_ERR_NOT_FOUND;
        *out = self->clockNs;
        return PPCP_OK;
    }

    void build(bool withMic = true)
    {
        storage.assign(ppcp_peer_sizeof(), 0);

        ppcp_timebase t{};
        ASSERT_EQ(ppcp_timebase_make(&t, tb.c_str(), tb.size(), PPCP_TB_MONOTONIC, false, 1),
                  PPCP_OK);
        timebases.push_back(t);

        {
            ppcp_timing tm{};
            ASSERT_EQ(ppcp_timing_make_nominal_frame_start(&tm, 120000, PPCP_PROV_ASSUMED),
                      PPCP_OK);
            ppcp_geometry g{};
            ASSERT_EQ(ppcp_geometry_make_rolling_shutter(&g, 8000000, PPCP_PROV_ASSUMED,
                                                         PPCP_ROLL_TOP_TO_BOTTOM, 1080),
                      PPCP_OK);
            ppcp_capture_profile cp{};
            ASSERT_EQ(ppcp_capture_profile_make(&cp, "p-cap", &tm), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_camera(&cp, &g, PPCP_INTR_PER_FRAME), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_format(&cp, "hevc", 1920, 1080, "nv12"), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_rate(&cp, 240000, 240000, 240000), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_optical(&cp, 100000, 20000000, 100, 3200),
                      PPCP_OK);
            profiles.push_back(cp);
        }
        if (withMic) {
            // 6.1d — a Source with no `format` has `convention: mid` fixed, and
            // its canonical instant is the raw instant.  A microphone is the
            // case the clause is written about.
            ppcp_timing tm{};
            ASSERT_EQ(ppcp_timing_make(&tm, PPCP_CONV_MID), PPCP_OK);
            ppcp_capture_profile cp{};
            ASSERT_EQ(ppcp_capture_profile_make(&cp, "p-mic", &tm), PPCP_OK);
            profiles.push_back(cp);
        }

        ppcp_source cam{};
        ASSERT_EQ(ppcp_source_make(&cam, "src-cam", peerId.c_str(), "camera", tb.c_str(), true,
                                   &profiles[0], 1), PPCP_OK);
        ASSERT_EQ(ppcp_source_set_label(&cam, "down-the-line"), PPCP_OK);
        sources.push_back(cam);
        if (withMic) {
            ppcp_source mic{};
            ASSERT_EQ(ppcp_source_make(&mic, "src-mic", peerId.c_str(), "microphone",
                                       tb.c_str(), true, &profiles[1], 1), PPCP_OK);
            sources.push_back(mic);
        }

        for (const char *n : { "core", "capture", "detect", "mint", "live", "offline",
                               "markup" }) {
            ppcp_id id{};
            ASSERT_EQ(ppcp_id_set_z(&id, n), PPCP_OK);
            declared.push_back(id);
        }
        if (withActuator || withLevelActuator) {
            // CORE §2.2.3 — a peer that OWNS one declares Actuate.
            ppcp_id id{};
            ASSERT_EQ(ppcp_id_set_z(&id, PPCP_PROFILE_ACTUATE), PPCP_OK);
            declared.push_back(id);
        }
        ASSERT_EQ(ppcp_peer_desc_make(&desc, peerId.c_str(), PPCP_ROLE_CAPTURE,
                                      ppcp_wire_version(), declared.data(), declared.size(),
                                      timebases.data(), timebases.size()), PPCP_OK);
        ASSERT_EQ(ppcp_peer_desc_set_sources(&desc, sources.data(), sources.size()), PPCP_OK);
        if (withActuator || withLevelActuator) {
            // CB1 — the phone's torch is `control: on_off`, and 5.19b keeps
            // Actuator kinds disjoint from Source kinds: this is NOT a flag on
            // `src-cam`, it is its own entity beside it.
            ppcp_actuator torch{};
            ASSERT_EQ(ppcp_actuator_make(&torch, "act:torch", peerId.c_str(),
                                         PPCP_ACTUATOR_KIND_TORCH,
                                         PPCP_ACTUATOR_CONTROL_ON_OFF), PPCP_OK);
            ASSERT_EQ(ppcp_actuator_set_label(&torch, "rear torch"), PPCP_OK);
            if (withActuator) actuators.push_back(torch);
            if (withLevelActuator) {
                // A dimmable lamp beside it.  `control: level` is what makes a
                // CLAMP sayable: 12.1c's `state` carries the ACHIEVED value,
                // and a driver that quantises 0.9 to 0.6 is reporting a
                // different number from the one it was handed.
                ppcp_actuator lamp{};
                ASSERT_EQ(ppcp_actuator_make(&lamp, "act:lamp", peerId.c_str(),
                                             PPCP_ACTUATOR_KIND_TORCH,
                                             PPCP_ACTUATOR_CONTROL_LEVEL), PPCP_OK);
                ASSERT_EQ(ppcp_actuator_set_label(&lamp, "dimmable lamp"), PPCP_OK);
                actuators.push_back(lamp);
            }
            ASSERT_EQ(ppcp_peer_desc_set_actuators(&desc, actuators.data(), actuators.size()),
                      PPCP_OK);
        }
        ASSERT_EQ(ppcp_peer_desc_set_product(&desc, "PinPoint", "Capture", "1.0"), PPCP_OK);

        std::vector<const char *> names;
        for (const ppcp_id &id : declared) names.push_back(id.v);
        ppcp_peer_config pc{};
        pc.role          = PPCP_ROLE_CAPTURE;
        pc.peer_id       = peerId.c_str();
        pc.profiles      = names.data();
        pc.profile_count = names.size();
        pc.listener      = true;   // ENC 2.1a — no `link_bind` is minted here
        pc.clock         = ppcp_clock{ &DevicePeer::clockNow, this };
        // 6.1b — the clock this peer stamps `t2`/`t3` on when it ANSWERS a
        // probe.  Without it a `sync_probe` is answered
        // `error`/`profile_not_supported` rather than with a made-up instant.
        pc.sync_timebase = tb.c_str();
        pc.health_report = &DevicePeer::healthReport;
        pc.ctx = this;
        ASSERT_EQ(ppcp_peer_new(storage.data(), storage.size(), &pc, &p), PPCP_OK);
    }
};

// Moves every whole frame waiting on `ch` from one engine to the other, calling
// `after` once per frame so a consumer sees the message while its bytes are
// still the ones it was handed.
// Drains every event the peer queued, handing each to `fn`.
inline void drainEvents(ppcp_peer *p, const std::function<void(const ppcp_event &)> &fn)
{
    ppcp_event ev{};
    while (ppcp_peer_next_event(p, &ev) == PPCP_OK) fn(ev);
}

using EventSink = std::function<void(const ppcp_event &)>;

// Moves everything `from` has queued on `ch` into `to`, one frame at a time.
//
// ⚠ `sink` IS NOT OPTIONAL DECORATION SINCE libppcp 27b40c4 (F-L13-1).
// `ppcp_peer_feed()` now REFUSES to start a frame it cannot report — the event
// ring is PPCP_PEER_EVENT_QUEUE deep with two slots of headroom — so a peer
// that is fed several frames without being drained stops consuming after the
// second event.  That is the correct behaviour and it is the fix this
// repository asked for; what it means for a harness is that "pipe everything,
// then look at the events" is no longer a thing that works.  Draining between
// frames is what PpcpHostPeer::pump() does in production, and `sink` is how a
// test does the same.
//
// With no sink there is nowhere for the events to go, so a stalled feed STOPS
// rather than skipping the frame.  Skipping is what the old harness did by
// ignoring `took`, and it is how three assertions went quietly untested.
//
// ⚠ A STALLED pipe() DISCARDS THE BYTES IT HAD ALREADY DRAINED.
// ppcp_peer_drain() dequeues in bulk, so once a frame in the middle of a read
// cannot be fed there is nowhere to put the tail.  That is acceptable in a
// harness and would not be in the application — PpcpHostPeer::pump() keeps the
// tail in m_tails for exactly this reason — but it means a bare pipe() before a
// pipe(sink) THROWS AWAY the very frame the second call was going to look for.
// Pass the sink to the call that moves the interesting frame, and do not pipe
// twice.
inline void pipe(ppcp_peer *from, ppcp_peer *to, std::uint8_t ch,
                 const std::function<void()> &after = {},
                 const EventSink &sink = {})
{
    std::vector<std::uint8_t> buf(1u << 20);
    for (;;) {
        std::size_t len = 0;
        if (ppcp_peer_drain(from, ch, buf.data(), buf.size(), &len) != PPCP_OK || len == 0) break;
        std::size_t off = 0;
        while (off < len) {
            ppcp_frame_header h{};
            const std::uint8_t *payload = nullptr;
            std::size_t consumed = 0;
            if (ppcp_frame_read(buf.data() + off, len - off, &h, &payload, &consumed) != PPCP_OK)
                break;
            std::size_t took = 0;
            ppcp_peer_feed(to, ch, buf.data() + off, consumed, &took);
            if (took == 0) {
                if (!sink) return;
                drainEvents(to, sink);
                ppcp_peer_feed(to, ch, buf.data() + off, consumed, &took);
                if (took == 0) return;
            }
            if (after) after();
            if (sink) drainEvents(to, sink);
            off += consumed;
        }
    }
    if (sink) drainEvents(to, sink);
}

// ── ⭐ MSG 12.1c — THE DEVICE OWES THE ANSWER, SINCE libppcp L30 ────────────
//
// The engine used to write the `applied` ack itself, ECHOING the request.  That
// was the defect PinPointStudio's C2 report and libppcp package L30 both
// landed on: 12.1c says `state` is what the Actuator is ACTUALLY doing, and a
// sans-I/O library owns no hardware, so an ack it wrote could only ever be the
// echo the clause forbids.  A well-formed, declared, host-originated command is
// now raised as PPCP_EVENT_ACTUATOR_COMMAND with `status == PPCP_OK` meaning
// "you owe an answer", and this is the device end of that obligation.
//
// ⚠ `status != PPCP_OK` MEANS THE ENGINE ALREADY ANSWERED — 12.1d
// (`not_declared`), I39 (`malformed`), 12a (a non-host sender) — and answering
// again would put two responses on the wire for one Request.
//
// `apply` IS THE SIMULATED DRIVER.  Returning true acks `applied` with whatever
// it wrote into `achieved` — which is seeded with the request, so a driver that
// does nothing models a device that did exactly what it was told.  Returning
// false refuses (12.1b) with `reason`.  Absent, every command is applied
// exactly: an honest torch, not an engine echo.
using ActuatorApply = std::function<bool(const std::string &actuatorId,
                                         const ppcp_actuator_setting &requested,
                                         ppcp_actuator_setting *achieved,
                                         std::string *refuseReason)>;

inline void answerActuatorCommand(ppcp_peer *p, const ppcp_event &ev,
                                  const ActuatorApply &apply)
{
    if (!p || ev.kind != PPCP_EVENT_ACTUATOR_COMMAND || !ev.msg) return;
    if (ev.status != PPCP_OK) return;   // the engine has already answered it
    const ppcp_body_actuator_command &c = ev.msg->body.actuator_command;
    const std::string id = idStr(c.actuator_id);
    ppcp_actuator_setting achieved = c.setting;
    std::string reason;
    const bool ok = apply ? apply(id, c.setting, &achieved, &reason) : true;
    // MSG 1c — one of the two, never silence.  Asserted rather than ignored:
    // a refused answer would leave the host pending forever and the test
    // hanging on an ack that is never coming, which is a confusing way to fail.
    if (ok)
        EXPECT_EQ(ppcp_peer_actuator_command_applied(p, id.c_str(), &achieved,
                                                     ev.msg->env.msg_id), PPCP_OK);
    else
        EXPECT_EQ(ppcp_peer_actuator_command_refused(p, id.c_str(), reason.c_str(),
                                                     ev.msg->env.msg_id), PPCP_OK);
}

}  // namespace pptest
