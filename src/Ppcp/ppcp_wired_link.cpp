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

#include "ppcp_wired_link.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <QMetaObject>

#include <ppcp/cbor.h>
#include <ppcp/frame.h>
#include <ppcp/version.h>

#include "../Core/pp_debug.h"
#include "ppcp_bootstrap.h"    // sanitiseLabel() — RV 4.4d
#include "ppcp_discovery.h"    // pvAcceptsMajor() — RV 3.3a

#ifdef _WIN32
#  include <winsock2.h>
#  define ppw_close_socket closesocket
#  define ppw_poll WSAPoll
#else
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define ppw_close_socket ::close
#  define ppw_poll ::poll
#endif

namespace Ppcp {

namespace {

// The same clock `ppcp_transport.cpp` and `ppcp_usbmux.cpp` use — steady_clock
// milliseconds since its own epoch — which is what makes contract C1's
// `deadline` parameter comparable here at all.  ⚠ If any of the three ever
// changes its base, `DialFn`'s deadline stops meaning anything.
double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void fail(std::string *why, const char *what)
{
    if (why) *why = what;
}

}  // namespace

// ── Contract C3, the reader ────────────────────────────────────────────────
//
// ⚠ EVERY REFUSAL HERE IS ORDINARY.  A record from a device running a newer
// capture app, a collision on the presence port by some other process, a
// truncated read — all of them mean "treat this device as not wired", which is
// the state a phone on a charge-only cable is in anyway.  Nothing here is a
// user-visible error and nothing here is a security decision: the security
// decision is the resolve below it and the TLS handshake after that.
bool parseWiredPresence(const unsigned char *data, std::size_t len,
                        WiredPresence *out, std::string *why)
{
    if (!out) return false;
    *out = WiredPresence{};
    if (why) why->clear();

    if (data == nullptr || len == 0) { fail(why, "empty record"); return false; }
    // ⛔ The cap is checked here as well as at read time, because a caller that
    // got its bytes some other way must meet the same rule.
    if (len > kWiredPresenceMaxBytes) { fail(why, "record over 4096 bytes"); return false; }

    const ppcp_cbor_limits lim = ppcp_cbor_limits_for_channel(PPCP_CHANNEL_CONTROL);

    // One validating pass first, exactly as the frame decoder does: it is what
    // rejects duplicate keys (ENC 4d), integer keys (4a), `null` (4c), tags and
    // indefinite lengths, so the field reads below can stay simple.
    std::size_t consumed = 0;
    if (ppcp_cbor_validate(data, len, lim, &consumed) != PPCP_OK) {
        fail(why, "not well-formed CBOR");
        return false;
    }
    // Trailing bytes are a different fault from a bad record and are named as
    // one: the device writes ONE item and closes.
    if (consumed != len) { fail(why, "trailing bytes after the record"); return false; }

    ppcp_cbor_reader r;
    ppcp_cbor_reader_init(&r, data, len, lim);
    ppcp_cbor_item it;
    if (ppcp_cbor_read(&r, &it) != PPCP_OK || it.type != PPCP_CBOR_MAP) {
        fail(why, "record is not a map");
        return false;
    }

    bool sawPeers = false;
    const std::uint32_t fields = it.count;
    for (std::uint32_t f = 0; f < fields; ++f) {
        const char *k = nullptr;
        std::size_t klen = 0;
        if (ppcp_cbor_read_key(&r, &k, &klen) != PPCP_OK) {
            fail(why, "malformed key");
            return false;
        }

        if (ppcp_cbor_key_is(k, klen, "pv")) {
            ppcp_cbor_item v;
            if (ppcp_cbor_read(&r, &v) != PPCP_OK || v.type != PPCP_CBOR_TEXT) {
                fail(why, "pv is not text");
                return false;
            }
            out->pv.assign(reinterpret_cast<const char *>(v.bytes), v.len);
        } else if (ppcp_cbor_key_is(k, klen, "role")) {
            ppcp_cbor_item v;
            if (ppcp_cbor_read(&r, &v) != PPCP_OK || v.type != PPCP_CBOR_TEXT) {
                fail(why, "role is not text");
                return false;
            }
            out->role.assign(reinterpret_cast<const char *>(v.bytes), v.len);
        } else if (ppcp_cbor_key_is(k, klen, "dl")) {
            ppcp_cbor_item v;
            if (ppcp_cbor_read(&r, &v) != PPCP_OK || v.type != PPCP_CBOR_TEXT) {
                fail(why, "dl is not text");
                return false;
            }
            // ⛔ RV 4.4d — sanitised HERE, at the boundary, so nothing
            // downstream can hold the raw string by accident.  It is display
            // text and never a key.
            out->displayLabel =
                sanitiseLabel(std::string(reinterpret_cast<const char *>(v.bytes), v.len));
        } else if (ppcp_cbor_key_is(k, klen, "peers")) {
            ppcp_cbor_item arr;
            if (ppcp_cbor_read(&r, &arr) != PPCP_OK || arr.type != PPCP_CBOR_ARRAY) {
                fail(why, "peers is not an array");
                return false;
            }
            sawPeers = true;
            if (arr.count > kWiredPresenceMaxPeers) {
                // ⚠ Refused BEFORE reading the entries, not after: the cap is
                // there so a hostile record cannot make us do work.
                fail(why, "peers has more than 16 entries");
                return false;
            }
            for (std::uint32_t i = 0; i < arr.count; ++i) {
                ppcp_cbor_item ent;
                if (ppcp_cbor_read(&r, &ent) != PPCP_OK || ent.type != PPCP_CBOR_MAP) {
                    fail(why, "a peers entry is not a map");
                    return false;
                }
                WiredPresence::Listener L;
                bool sawPort = false, sawIdentity = false;
                const std::uint32_t entFields = ent.count;
                for (std::uint32_t e = 0; e < entFields; ++e) {
                    const char *ek = nullptr;
                    std::size_t eklen = 0;
                    if (ppcp_cbor_read_key(&r, &ek, &eklen) != PPCP_OK) {
                        fail(why, "malformed key in a peers entry");
                        return false;
                    }
                    if (ppcp_cbor_key_is(ek, eklen, "port")) {
                        ppcp_cbor_item v;
                        if (ppcp_cbor_read(&r, &v) != PPCP_OK || v.type != PPCP_CBOR_UINT) {
                            fail(why, "port is not an unsigned integer");
                            return false;
                        }
                        if (v.i < 1 || v.i > 65535) {
                            fail(why, "port is out of range");
                            return false;
                        }
                        L.port = static_cast<std::uint16_t>(v.i);
                        sawPort = true;
                    } else if (ppcp_cbor_key_is(ek, eklen, "psk_identity")) {
                        ppcp_cbor_item v;
                        if (ppcp_cbor_read(&r, &v) != PPCP_OK || v.type != PPCP_CBOR_BYTES) {
                            fail(why, "psk_identity is not a byte string");
                            return false;
                        }
                        // ⛔ EXACTLY 17.  RV 5.3f forbids transcoding or
                        // truncating an identity, so a wrong length is a
                        // refusal and never something to pad or cut.
                        if (v.len != kWiredPresenceIdentityBytes) {
                            fail(why, "psk_identity is not 17 bytes");
                            return false;
                        }
                        L.identity.assign(v.bytes, v.bytes + v.len);
                        sawIdentity = true;
                    } else if (ppcp_cbor_skip(&r) != PPCP_OK) {
                        // I13 — an unknown key's value is skipped at any depth.
                        fail(why, "malformed value in a peers entry");
                        return false;
                    }
                }
                if (!sawPort || !sawIdentity) {
                    fail(why, "a peers entry is missing port or psk_identity");
                    return false;
                }
                out->peers.push_back(std::move(L));
            }
        } else if (ppcp_cbor_skip(&r) != PPCP_OK) {
            // Forward compatibility: an unknown top-level key is skipped, which
            // is what lets the device add a field without this host refusing
            // every record it sends.
            fail(why, "malformed value");
            return false;
        }
    }

    // RV 3.3a — MAJOR, and only MAJOR.  A "1.7" record from a newer capture app
    // is acceptable; a "2.0" one is not.
    if (out->pv.empty() || !pvAcceptsMajor(out->pv, PPCP_WIRE_VERSION_MAJOR)) {
        fail(why, "pv major is not 1");
        return false;
    }
    if (!sawPeers)          { fail(why, "peers is absent"); return false; }
    if (out->peers.empty()) { fail(why, "peers is empty"); return false; }
    return true;
}

bool resolveFirstWiredPeer(const WiredPresence &record, const IdentityResolver &resolve,
                           std::size_t *whichPeer, ResolvedPairing *out)
{
    if (!resolve || !out) return false;
    for (std::size_t i = 0; i < record.peers.size(); ++i) {
        const WiredPresence::Listener &L = record.peers[i];
        if (L.identity.size() != kWiredPresenceIdentityBytes) continue;
        ResolvedPairing rp;
        if (!resolve(L.identity.data(), L.identity.size(), rp)) continue;
        if (rp.pairingId.empty()) continue;   // resolved to nothing usable
        *out = rp;
        if (whichPeer) *whichPeer = i;
        return true;
    }
    // ⛔ RV 3.4c — a phone this host is not paired with.  Not an error, not a
    // banner, and the caller says nothing to the device.
    return false;
}

// ── The orchestrator ───────────────────────────────────────────────────────

bool PpcpWiredLink::enabled()
{
    // Default ON.  `PINPOINT_PPCP_WIRED=0` is the escape hatch and the ONLY
    // value that turns it off; anything else (unset, "1", nonsense) leaves the
    // cable enabled, because a mistyped variable must not silently disable a
    // transport an operator is relying on.
    const char *v = std::getenv("PINPOINT_PPCP_WIRED");
    return !(v != nullptr && v[0] == '0' && v[1] == '\0');
}

PpcpWiredLink::PpcpWiredLink(QObject *parent) : QObject(parent) {}

PpcpWiredLink::~PpcpWiredLink() { stop(); }

void PpcpWiredLink::start(Usbmux::Provider provider)
{
    if (m_running) return;

    // ⚠ THE GATE, AND IT IS TEMPORARY — see the header.  §6.1's advertisement
    // arbitration is Phase 2; until it exists a phone that is both plugged in
    // and on WiFi would connect twice.
    if (!enabled()) return;

    m_provider = std::move(provider);

    // ⛔ A MISSING DAEMON MUST NOT KILL THE WIRED PATH FOR THE LIFE OF THE
    // PROCESS, AND IT USED TO.  This returned early when `m_watch.start()`
    // failed — before `m_running`, before the worker and before the retry timer
    // — so a Studio started while `usbmuxd` was down never looked again.
    //
    // ⚠ On Linux that is the ORDINARY sequence rather than an edge: the
    // open-source daemon exits when the last device detaches, so "start the app,
    // then plug the phone in" found no daemon and silently had no cable for ever
    // (measured 29 Aug 2026 — daemon down 19:59, app up 20:02, daemon back
    // 20:06, and the wired path never noticed).  Apple's usbmuxd runs
    // permanently, which is why macOS did not show this.
    //
    // The retry that already exists for DEVICES now has a sibling for the
    // DAEMON: everything below starts regardless, and onRetryTick() re-opens
    // the watch until it succeeds.
    m_running = true;
    {
        std::lock_guard<std::mutex> g(m_jobMutex);
        m_stopping = false;
    }
    m_worker = std::thread([this] { workerLoop(); });

    // ⛔ THE RETRY IS WHAT MAKES THE WIRED PATH WORK AT ALL IN THE FIELD — see
    // kWiredRetryFirstSecs.  An attach-only trigger misses the ordinary case of
    // a phone already on the cable when the app is opened.
    m_retryTimer.setInterval(1000);
    m_retryTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_retryTimer, &QTimer::timeout, this, &PpcpWiredLink::onRetryTick);
    m_retryTimer.start();

    // ✅ No thread for the watch: usbmux `Listen` is a long-lived readable fd,
    // the same shape the DNS-SD browser has at ppcp_host_service.cpp:1187.
    tryOpenWatch();
}

// Drops the watch and everything scoped to it, and NOTHING ELSE.  The worker,
// the job queue and the retry timer all stay up, because the daemon coming back
// is an ordinary event on Linux rather than the end of the session.
void PpcpWiredLink::closeWatch()
{
    // ⛔ Notifier before the fd, exactly as stop() does: a QSocketNotifier left
    // on a closed socket fires forever on some platforms and asserts on others.
    m_notifier.reset();
    m_watch.stop();
    m_watchUp = false;

    // ⛔ THE DIALS MUST STOP WITH THE SUBSCRIPTION, AND LEAVING THEM RUNNING WAS
    // A DEFECT.  The first version of closeWatch() dropped only the notifier and
    // the socket, on the reasoning that the daemon returning is an ordinary event
    // and the object should stay alive for it.  That is right about the object
    // and wrong about the work in flight: `stop()` used to set m_stopping, clear
    // the queue, join the worker and clear m_dialling, and none of that happened
    // here.
    //
    // ⚠ So a dial that began under the OLD daemon could finish under the NEW one,
    // carrying a DeviceID from an attachment that no longer exists — this plan's
    // own finding is that a DeviceID must never outlive its attachment — and hand
    // back a link over a tunnel that is already dead.  `link up` followed
    // immediately by `link closed`, with no channel and no declare, which is the
    // signature observed 30 Aug 2026 on every re-armed subscription while the
    // one link that never went through this path stayed up for ten minutes.
    //
    // The worker is joined and restarted rather than left running: it owns
    // nothing but the dial, so this costs a thread create per cable cycle and
    // buys the guarantee that no work outlives the daemon it was issued against.
    {
        std::lock_guard<std::mutex> g(m_jobMutex);
        m_stopping = true;
        m_jobs.clear();
    }
    m_jobCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    m_dialling.clear();
    {
        std::lock_guard<std::mutex> g(m_jobMutex);
        m_stopping = false;
    }
    if (m_running) m_worker = std::thread([this] { workerLoop(); });
    // Say it again next time it is missing — a new absence is a new state, and
    // this is what keeps the one-line-per-state-change discipline honest across
    // an unplug/replug cycle rather than going silent after the first one.
    m_watchAnnouncedDown = false;
    m_watchDueInSecs     = kWiredWatchRetrySecs;
    // Attachment-scoped state dies with the watch: every DeviceID it holds was
    // issued by a daemon that is gone, and 29 Aug's finding was that a DeviceID
    // may not outlive its attachment.
    m_retry.clear();
    m_attached.clear();
}

// Opens the device watch and arms its notifier.  Absence of a daemon is an
// ordinary outcome (design §6.2, RV 3.6a) and returns false without a fuss.
bool PpcpWiredLink::tryOpenWatch()
{
    if (m_watchUp) return true;

    const Usbmux::Result r = m_watch.start(m_provider, 2000);
    if (!r.ok()) {
        // ⚠ ONE LINE, AND ONLY WHEN THE OUTCOME CHANGES.  A phone left on
        // charge overnight would otherwise print this every few seconds for the
        // life of the session — the same discipline the device retry keeps.
        if (!m_watchAnnouncedDown) {
            m_watchAnnouncedDown = true;
            ppWarn() << "[ppcp-usb] wired path unavailable —" << r.message().c_str();
        }
        return false;
    }

    const pp_socket_t fd = m_watch.fd();
    if (fd == kInvalidSocket) {
        m_watch.stop();
        if (!m_watchAnnouncedDown) {
            m_watchAnnouncedDown = true;
            ppWarn() << "[ppcp-usb] wired path unavailable — the device watch gave no descriptor";
        }
        return false;
    }

    // ✅ No thread for the watch: usbmux `Listen` is a long-lived readable fd,
    // the same shape the DNS-SD browser has at ppcp_host_service.cpp:1187.
    m_notifier = std::make_unique<QSocketNotifier>(static_cast<int>(fd), QSocketNotifier::Read);
    connect(m_notifier.get(), &QSocketNotifier::activated,
            this, &PpcpWiredLink::onWatchReadable);

    // One diagnostic sweep so the log can tell "nothing plugged in" from "a
    // phone is here and it is on WiFi" (design §6.2).  It BLOCKS, so it goes to
    // the worker like every other usbmux request.
    {
        std::lock_guard<std::mutex> g(m_jobMutex);
        m_jobs.push_back(Job{});   // deviceId 0 == the probe
    }
    m_jobCv.notify_one();

    m_watchUp = true;
    m_watchAnnouncedDown = false;
    ppWarn() << "[ppcp-usb] wired path armed —" << m_provider.describe().c_str();
    return true;
}

void PpcpWiredLink::stop()
{
    // ⛔ THE NOTIFIER GOES FIRST, BEFORE ANYTHING CAN CLOSE THE fd.  Exactly
    // the discipline stopDiscovery() keeps at ppcp_host_service.cpp:1205: "a
    // QSocketNotifier left on a closed socket" fires for ever on some platforms
    // and asserts on others.
    m_notifier.reset();
    m_retryTimer.stop();
    m_retry.clear();

    {
        std::lock_guard<std::mutex> g(m_jobMutex);
        m_stopping = true;
        m_jobs.clear();
    }
    m_jobCv.notify_all();
    if (m_worker.joinable()) m_worker.join();

    m_watch.stop();
    m_dialling.clear();
    m_attached.clear();
    m_running = false;
    m_watchUp = false;
    m_watchDueInSecs = 0;
    m_watchAnnouncedDown = false;
}

QString PpcpWiredLink::describe() const
{
    if (!enabled()) return QStringLiteral("wired: off (PINPOINT_PPCP_WIRED=0)");
    if (!m_running) return QStringLiteral("wired: no usbmux provider");
    return QStringLiteral("wired: watching, %1 attached, %2 dialled, %3 adopted")
        .arg(m_attachedSeen)
        .arg(m_dialled)
        .arg(m_adopted);
}

void PpcpWiredLink::onWatchReadable()
{
    std::vector<Usbmux::Watch::Event> events;
    const bool alive = m_watch.poll(events);

    for (const Usbmux::Watch::Event &e : events) {
        if (e.kind == Usbmux::Watch::EventKind::Attached) handleAttached(e.device);
        else                                              handleDetached(e.deviceId);
    }

    if (!alive) {
        // 3.6a's habit of mind applied to the cable: the watch ending is a
        // reason to stop watching and NOT a fault to report.  ⛔ The notifier
        // must come off before the fd closes, which is what stop() does first.
        const Usbmux::Result why = m_watch.lastError();
        ppWarn() << "[ppcp-usb] device watch ended —" << why.message().c_str();
        // ⛔ closeWatch(), NOT stop() — AND THIS IS THE COMMON CASE ON LINUX.
        // The open-source `usbmuxd` exits when the last device detaches, so the
        // watch ends on every unplug.  stop() is SHUTDOWN: it clears m_running
        // and stops the retry timer, and since nothing calls startWired() a
        // second time (`ppcp_host_service.cpp` calls it once), that left the
        // wired path dead for the rest of the process — observed live 30 Aug
        // 2026, cable replugged and never re-armed.  Tearing down only the
        // watch leaves onRetryTick() free to re-open it, which is the same
        // recovery that already covers "no daemon when we started".
        closeWatch();
    }
}

void PpcpWiredLink::handleAttached(const Usbmux::Device &d)
{
    ++m_attachedSeen;

    // ⛔ §4.2's ONE filter, asked of the one place that owns it.  usbmux also
    // reports WiFi-paired devices as `ConnectionType == "Network"`, and treating
    // one as wired would label a WiFi link as wired and silently invalidate
    // every timing claim this work exists to make.
    if (!d.isWired()) {
        ppWarn() << "[ppcp-usb] a device is here but it is on WiFi (ConnectionType="
                 << d.connectionType.c_str() << ") — not a wired path";
        return;
    }
    if (d.serialNumber.empty()) {
        ppWarn() << "[ppcp-usb] an attachment arrived with no serial number — ignored";
        return;
    }

    // ⛔ Remembered by UDID.  The DeviceID is usbmuxd's handle on THIS
    // attachment and nothing more — measured moving from 306 to 308 for one
    // phone on one cable in a day — so it is carried alongside as the thing we
    // dial with, never as the thing we key on.
    m_attached.emplace_back(d.deviceId, d.serialNumber);

    // A NEW attachment is a clean slate: fresh backoff, and nothing logged yet.
    // ⚠ Erase any stale entry first — an unplug/replug must not inherit the old
    // attachment's DeviceID, which usbmuxd will have changed.
    m_retry.erase(std::remove_if(m_retry.begin(), m_retry.end(),
                                 [&](const Retry &r) { return r.udid == d.serialNumber; }),
                  m_retry.end());
    Retry fresh;
    fresh.udid     = d.serialNumber;
    fresh.deviceId = d.deviceId;
    m_retry.push_back(fresh);

    if (std::find(m_dialling.begin(), m_dialling.end(), d.serialNumber) != m_dialling.end())
        return;   // a dial is already in flight for this phone

    m_dialling.push_back(d.serialNumber);
    ++m_dialled;
    {
        std::lock_guard<std::mutex> g(m_jobMutex);
        if (m_stopping) return;
        m_jobs.push_back(Job{d.deviceId, d.serialNumber});
    }
    m_jobCv.notify_one();
}

PpcpWiredLink::Retry *PpcpWiredLink::retryFor(const std::string &udid)
{
    for (Retry &r : m_retry)
        if (r.udid == udid) return &r;
    return nullptr;
}

// ── The retry, and the rule it must not break ──────────────────────────────
//
// ⛔ §6.1 rule 4: "presence unreadable → change nothing.  Plugging a phone in to
// charge must never disturb WiFi."  Nothing here touches the advertisement, the
// WiFi path or any live link — a due device gets one more presence read, and a
// device that keeps refusing simply keeps being re-read at kWiredRetryMaxSecs.
//
// ⚠ THE LOG IS WHAT MAKES THAT AFFORDABLE.  A phone plugged in only to charge
// refuses for ever, and a line per attempt would be a line every 30 s for the
// life of the session — which is how a one-log-per-app rule turns into noise
// nobody reads.  So a line is printed when the OUTCOME CHANGES and not when an
// attempt is made.
void PpcpWiredLink::onRetryTick()
{
    if (!m_running) return;

    // ⛔ The daemon first: with no watch there are no devices to retry, and
    // this is the only thing that recovers a Studio started before usbmuxd.
    if (!m_watchUp) {
        if (--m_watchDueInSecs > 0) return;
        m_watchDueInSecs = kWiredWatchRetrySecs;
        if (!tryOpenWatch()) return;
    }

    for (Retry &r : m_retry) {
        if (r.linked) continue;   // this cable already carries a link
        if (std::find(m_dialling.begin(), m_dialling.end(), r.udid) != m_dialling.end())
            continue;             // one already in flight
        // ⚠ Cheap pre-dial gate, and it runs HERE because this is the GUI
        // thread — the worker may not touch the phone list.  It saves two TLS
        // handshakes every 2 s for a phone that is already on the cable or busy.
        if (!r.knownPairing.isEmpty() && m_peerState) {
            const PeerLinkState st = m_peerState(r.knownPairing);
            if (st == PeerLinkState::Wired || st == PeerLinkState::WifiBusy) continue;
        }
        if (--r.dueInSecs > 0) continue;

        // Flat — see kWiredRetrySecs.  A ramp loses the race against the
        // phone's own WiFi dial, which fires on app foreground.  The only
        // slow case is a phone that is not ours (kWiredNoMatchRetrySecs).
        r.dueInSecs = r.everySecs;

        m_dialling.push_back(r.udid);
        ++m_dialled;
        {
            std::lock_guard<std::mutex> g(m_jobMutex);
            if (m_stopping) return;
            m_jobs.push_back(Job{r.deviceId, r.udid});
        }
        m_jobCv.notify_one();
    }
}

void PpcpWiredLink::noteDialOutcome(const std::string &udid, DialOutcome o,
                                    const QString &why, const QString &pairing)
{
    m_dialling.erase(std::remove(m_dialling.begin(), m_dialling.end(), udid),
                     m_dialling.end());

    Retry *r = retryFor(udid);
    if (!r) return;   // detached while the dial was in flight
    if (!pairing.isEmpty()) r->knownPairing = pairing;

    if (o == DialOutcome::Adopted) {
        r->linked = true;
        return;
    }

    // ⛔ "NONE OF THIS PHONE'S PAIRINGS IS ONE OF OURS" IS NOT SETTLED — see
    // kWiredNoMatchRetrySecs.  It used to latch the retry off for the life of the
    // attachment; the capture app restarting rebuilds its presence record, and a
    // latched host never sees the new one.  Slow down, do not stop.
    r->everySecs = (o == DialOutcome::NoMatch) ? kWiredNoMatchRetrySecs
                                               : kWiredRetrySecs;

    // One line per state change, per the note above.
    if (!r->everLogged || r->lastLogged != o) {
        r->everLogged = true;
        r->lastLogged = o;
        ppWarn() << "[ppcp-usb]" << why.toUtf8().constData()
                 << "— retrying every" << r->everySecs
                 << "s while it stays plugged in";
    }
}

void PpcpWiredLink::retryNow()
{
    // ⚠ THIS RUNS INSIDE A TAKEOVER TOO, and the interaction is worth stating.
    // dropPhone() calls it, and a takeover drops the WiFi phone — so the entry
    // for the phone we have JUST connected over the cable gets its `linked` flag
    // cleared here, moments before the cable link is adopted.  What stops it
    // being re-dialled a second later is the pre-dial state check in
    // onRetryTick(): by then the wired Phone exists, `knownPairing` was recorded
    // when the dial succeeded, and PeerLinkState answers `Wired`.
    //
    // ⛔ So the pre-dial check is not merely an optimisation — remove it and a
    // takeover re-dials itself in a loop.
    for (Retry &r : m_retry) {
        r.linked    = false;
        r.dueInSecs = 1;
        r.everySecs = kWiredRetrySecs;   // a link ending re-arms the fast cadence
        // ⚠ The outcome memory is deliberately NOT cleared: if the phone is
        // still refusing for the same reason, that is not a state change and
        // does not deserve a second identical line.
    }
}

void PpcpWiredLink::handleDetached(Usbmux::DeviceId id)
{
    // ⚠ A detach carries ONLY the DeviceID, so the udid has to come from what
    // we remembered at attach.
    for (auto it = m_attached.begin(); it != m_attached.end(); ++it) {
        if (it->first != id) continue;
        const std::string udid = it->second;
        m_attached.erase(it);
        m_dialling.erase(std::remove(m_dialling.begin(), m_dialling.end(), udid),
                         m_dialling.end());
        // The retry is attachment-scoped, exactly like m_attached: a phone that
        // is not plugged in is not re-probed, and a replug starts a clean ramp.
        m_retry.erase(std::remove_if(m_retry.begin(), m_retry.end(),
                                     [&](const Retry &r) { return r.udid == udid; }),
                      m_retry.end());
        // ⛔ The LINK is not touched here.  Never switch a live link's transport
        // and never migrate one (design §7.3): a link whose cable is pulled dies
        // the ordinary way, through the channel going readable-and-closed and
        // dropPhone().  Unplugging is not a reason for this class to reach into
        // the service's phone list.
        return;
    }
}

// ── The worker thread ──────────────────────────────────────────────────────
//
// ⛔ IT IS NOT THE ACCEPT THREAD, AND THAT IS THE POINT.  The accept thread
// polls acceptChannelFor() per half-built link and then blocks 250 ms in
// accept(); a usbmux Connect plus a presence read plus two concurrent TLS
// handshakes parked there would starve WiFi accepts and stall preview-channel
// collection (design §6.3).
void PpcpWiredLink::workerLoop()
{
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(m_jobMutex);
            m_jobCv.wait(lk, [this] { return m_stopping || !m_jobs.empty(); });
            if (m_stopping) return;
            j = m_jobs.front();
            m_jobs.pop_front();
        }
        runJob(j);
    }
}

bool PpcpWiredLink::stopping() const
{
    std::lock_guard<std::mutex> g(m_jobMutex);
    return m_stopping;
}

void PpcpWiredLink::runJob(const Job &j)
{
    Usbmux::Client client(m_provider);

    // deviceId 0 is the start-up probe: one enumeration, one log line, no dial.
    if (j.deviceId == 0) {
        std::vector<Usbmux::Device> all;
        const Usbmux::Result r = client.listDevices(all, 2000);
        // ⚠ `!ok()` DOES NOT MEAN THE VECTOR IS EMPTY.  listDevices() answers
        // NoDevices and NoWiredDevices with ok() == false and fills `out` in
        // either case, precisely so "a phone is here but it is on WiFi" is
        // sayable.
        const std::size_t wired = Usbmux::wiredOnly(all).size();
        ppWarn() << "[ppcp-usb]" << all.size() << "device(s) attached," << wired
                 << "on a cable —" << r.message().c_str();
        return;
    }

    // Every exit below this point reports its outcome to the GUI thread — the
    // one place that knows whether it is worth a log line and when to try again.
    auto finish = [this, udid = j.udid](DialOutcome o, const QString &why) {
        QMetaObject::invokeMethod(
            this, [this, udid, o, why] { noteDialOutcome(udid, o, why); },
            Qt::QueuedConnection);
    };

    // ── 1. The presence record, over the tunnel ────────────────────────────
    //
    // ⚠ The first of two stop checks — one before each phase that can block,
    // which is this presence dial-and-read and the TLS connect below.  A quit
    // that arrives while this job is queued must not pay for a dial nobody is
    // waiting for.
    if (stopping()) return;
    Usbmux::Result diag;
    const pp_socket_t fd = client.dial(j.deviceId, Usbmux::kWiredPresencePort, &diag,
                                       kWiredPresenceReadMs);
    if (fd == kInvalidSocket) {
        // ⚠ `Number=3` IS AMBIGUOUS AND IS REPORTED AS SUCH.  A closed presence
        // port and a dial refused at the mux layer are the SAME wire event —
        // measured 29 Aug 2026 — so this host names both possibilities and
        // diagnoses neither.  ⛔ Do not print "trust not granted": M5 has not
        // run and the only device tested is a trusted one.
        finish(DialOutcome::NoPresence,
               QStringLiteral("no presence record — %1 (the capture app is not running "
                              "or not in the foreground, or this host is not trusted by "
                              "the device)")
                   .arg(QString::fromStdString(diag.message())));
        return;
    }

    std::vector<unsigned char> bytes;
    std::string why;
    if (!readPresence(fd, &bytes, &why)) {
        finish(DialOutcome::Unreadable,
               QStringLiteral("presence record unreadable — %1 — treating the device as "
                              "not wired").arg(QString::fromStdString(why)));
        return;
    }

    // ── 2. Parse it (contract C3) ──────────────────────────────────────────
    WiredPresence rec;
    if (!parseWiredPresence(bytes.data(), bytes.size(), &rec, &why)) {
        finish(DialOutcome::Refused,
               QStringLiteral("presence record refused — %1 — treating the device as "
                              "not wired").arg(QString::fromStdString(why)));
        return;
    }

    // ── 3. RV 5.3b, client-side, first match wins ──────────────────────────
    //
    // ⚠ THE RESOLVER IS THE HOST'S OWN, UNCHANGED.  It is already called off
    // the GUI thread — `Listener::setIdentityResolver()` runs it on the accept
    // thread — and `PpcpRendezvous` guards every entry of its state with one
    // mutex, so this is the second caller of a callback that was already
    // designed to be one.
    std::size_t which = 0;
    ResolvedPairing resolved;
    if (!resolveFirstWiredPeer(rec, m_resolve, &which, &resolved)) {
        // RV 3.4c — a phone this host is not paired with.  Stay silent to the
        // device; one line for somebody who went looking.
        finish(DialOutcome::NoMatch,
               QStringLiteral("a cabled device published %1 listener(s), none of which "
                              "resolves to a pairing this host holds")
                   .arg(int(rec.peers.size())));
        return;
    }

    const QString pairingId = QString::fromStdString(resolved.pairingId);

    // ── 4. Dial the PPCP listener behind that pairing ──────────────────────
    ConnectorConfig cfg;
    // `host`/`port` are unused when `dial` is set — the dialler already knows
    // where it is going (contract C1).
    cfg.kTls     = resolved.kTls;
    cfg.identity = rec.peers[which].identity;
    cfg.channels = {Channel::Control, Channel::Bulk};
    // See kWiredHandshakeMs — a cable that has not handshaken in 3 s is broken,
    // not busy, and this is also what bounds how long stop() can block.
    cfg.options.handshakeTimeoutMs = kWiredHandshakeMs;
    const Usbmux::DeviceId deviceId = j.deviceId;
    const std::uint16_t    port     = rec.peers[which].port;
    const Usbmux::Provider provider = m_provider;
    // ⛔ ONE FRESH TUNNEL PER CHANNEL.  `Connector::connect()` calls this once
    // per channel and requires a DISTINCT fd each time, so a Client is built
    // per call: a usbmux socket is CONSUMED by its Connect and is the tunnel
    // afterwards.
    //
    // ⛔ AND THE fd MUST STAY NON-BLOCKING.  A blocking fd handed back here does
    // not fail slowly, it HANGS FOREVER: the transport drives OpenSSL through
    // waitFor()/poll(), so `handshakeTimeoutMs` never fires on a descriptor
    // that never returns to it.  `Usbmux::Client::dial()` sets O_NONBLOCK
    // itself; nothing here undoes it or wraps it in something that would.
    cfg.dial = [provider, deviceId, port](double deadline) -> pp_socket_t {
        // The deadline is in the transport's own steady_clock milliseconds,
        // which is the same base nowMs() reads here.
        int remaining = static_cast<int>(deadline - nowMs());
        if (remaining < 1)    remaining = 1;
        if (remaining > 5000) remaining = 5000;
        Usbmux::Client c(provider);
        Usbmux::Result d;
        const pp_socket_t s = c.dial(deviceId, port, &d, remaining);
        if (s == kInvalidSocket)
            ppWarn() << "[ppcp-usb] tunnel refused —" << d.message().c_str();
        return s;
    };

    // The second stop check — the expensive one.  Two concurrent TLS handshakes
    // over two fresh tunnels is the longest thing this worker ever does, and
    // entering it during a shutdown is what made quitting slow.
    if (stopping()) return;
    HandshakeFailure hf;
    std::unique_ptr<PeerConnection> link = Connector::connect(cfg, &hf);
    if (!link) {
        QMetaObject::invokeMethod(
            this,
            [this, udid = j.udid, pairingId,
             m = QString::fromStdString(hf.message.empty() ? "no link" : hf.message)] {
                noteDialOutcome(udid, DialOutcome::HandshakeFailed,
                                QStringLiteral("wired handshake did not complete — %1").arg(m),
                                pairingId);
            },
            Qt::QueuedConnection);
        return;
    }

    // ── 5. Cross to the GUI thread ─────────────────────────────────────────
    //
    // ⚠ The identical hand-off the accept thread uses at
    // ppcp_host_service.cpp:303 and :328, under the same rule: the worker owns
    // nothing but the dial, and the PeerConnection is ADOPTED on the GUI thread.
    PeerConnection *raw = link.release();
    QMetaObject::invokeMethod(
        this, [this, raw, pairingId, deviceId, udid = j.udid] {
            noteDialOutcome(udid, DialOutcome::Adopted, QString(), pairingId);
            onDialFinished(raw, pairingId, deviceId);
        },
        Qt::QueuedConnection);
}

void PpcpWiredLink::onDialFinished(PeerConnection *raw, QString resolvedPairingId,
                                   Usbmux::DeviceId)
{
    std::unique_ptr<PeerConnection> link(raw);
    if (!link) return;

    // stop() may have run between the worker releasing the link and this
    // arriving; a link with nowhere to go is closed rather than leaked.
    if (!m_running || !m_adopt) {
        link->close();
        return;
    }

    // §6.1 rule 1, re-checked here because the state can have moved while the
    // handshake ran.  See PeerLinkState in the header for why this is a takeover
    // rather than a refusal.
    const PeerLinkState st = m_peerState ? m_peerState(resolvedPairingId)
                                         : PeerLinkState::None;
    if (st == PeerLinkState::Wired || st == PeerLinkState::WifiBusy) {
        ppWarn() << "[ppcp-usb] a link for this pairing is already up and"
                 << (st == PeerLinkState::Wired ? "on the cable" : "in use")
                 << "— leaving it alone (design §6.1 rule 1)";
        link->close();
        return;
    }

    if (st == PeerLinkState::WifiIdle) {
        // ⛔ DROP THE WiFi LINK ONLY NOW — the cable is already up, so this is a
        // make-before-break and there is never a moment with no link at all.
        // ⚠ The new link re-converges from nothing: ~35 s before it can
        // arbitrate a shot.  That is why WifiBusy above is left alone.
        ppWarn() << "[ppcp-usb] the cable is up for this pairing — dropping the "
                    "WiFi link and taking over (re-syncs, about a minute)";
        if (m_dropForTakeover) m_dropForTakeover(resolvedPairingId);
    }

    ++m_adopted;
    // ⛔ CONTRACT C2 — the pairing travels as a PARAMETER.  link->pairingId() is
    // empty on a link we dialled, and everything the rendezvous ledger drives is
    // keyed on the pairing id.
    m_adopt(std::move(link), resolvedPairingId);
}

// ── The presence read: to EOF, capped, deadlined, no framing ───────────────
bool PpcpWiredLink::readPresence(pp_socket_t fd, std::vector<unsigned char> *out,
                                 std::string *why)
{
    if (!out) return false;
    out->clear();

    const double deadline = nowMs() + kWiredPresenceReadMs;
    for (;;) {
        const int remaining = static_cast<int>(deadline - nowMs());
        if (remaining <= 0) {
            ppw_close_socket(static_cast<int>(fd));
            fail(why, "timed out waiting for the record");
            return false;
        }

        pollfd p{};
        p.fd = static_cast<int>(fd);
        p.events = POLLIN;
        const int pr = ppw_poll(&p, 1, remaining);
        if (pr == 0) {
            ppw_close_socket(static_cast<int>(fd));
            fail(why, "timed out waiting for the record");
            return false;
        }
        if (pr < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            ppw_close_socket(static_cast<int>(fd));
            fail(why, "the tunnel failed while reading");
            return false;
        }

        unsigned char buf[1024];
        const auto n = ::recv(static_cast<int>(fd), reinterpret_cast<char *>(buf),
                              static_cast<int>(sizeof buf), 0);
        if (n == 0) break;   // EOF — the device wrote the record and closed
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
            ppw_close_socket(static_cast<int>(fd));
            fail(why, "the tunnel failed while reading");
            return false;
        }
        // ⛔ The cap is enforced as bytes ARRIVE, not after: a device that never
        // stops writing must not be able to make this host allocate.
        if (out->size() + static_cast<std::size_t>(n) > kWiredPresenceMaxBytes) {
            ppw_close_socket(static_cast<int>(fd));
            fail(why, "record exceeded 4096 bytes");
            return false;
        }
        out->insert(out->end(), buf, buf + n);
    }

    ppw_close_socket(static_cast<int>(fd));
    if (out->empty()) { fail(why, "the device closed without writing a record"); return false; }
    return true;
}

}  // namespace Ppcp
