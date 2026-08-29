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

// The wired PPCP path, end to end: a phone attaches on a cable, this reads the
// presence record it serves over the usbmux tunnel, resolves the pairing under
// RV 5.3b BEFORE dialling, dials, and hands the finished link to
// PpcpHostService.  Design `docs/design/wired_transport_design.md` §5.2, §5.3
// and §6; Phase 1 contracts C2, C3, C6 of
// `docs/implementation/wired_transport_impl_plan.md`.
//
// ⚠ IT IS A SEPARATE TRANSLATION UNIT ON PURPOSE.  `ppcp_host_service.cpp` is
// already ~2900 lines and owns the listen/accept/code/discovery/guided surface;
// the wired path is a second, opposite-direction rendezvous with its own thread
// and its own failure vocabulary, and folding it in would have made the one
// file that composes everything the one file nobody can read.
//
// ── DIRECTION, WHICH IS THE WHOLE REASON THIS EXISTS ───────────────────────
//
// On WiFi the PHONE dials and this host listens (RV 5.2g/3.5e).  usbmux is
// host→device only (design §3), so on the cable RV 2d inverts and THIS HOST IS
// THE INITIATOR.  Two consequences run through everything below:
//
//   1. ⛔ `link->pairingId()` is EMPTY on a link we dialled — it is the
//      LISTENER that resolves an identity, and here we are the client.  So the
//      pairing resolved from the presence record is carried out of here by
//      hand and passed to `adoptLink()` (contract C2).  Without it the link
//      would be live, carry video, and be invisible to `phoneByPairing()`,
//      `notePeerName()` and `m_pairedThisRun` — the home screen would say
//      connected while Settings→Phones said disconnected.  That is the same
//      class as the empty-Phones-list bug of 26 Aug.
//   2. The engine is built with `listener = false` and this host sends
//      `hello` (contract C6).
//
// ── THREADS ────────────────────────────────────────────────────────────────
//
// ✅ The WATCH needs no thread.  usbmux `Listen` is a long-lived readable fd,
//    which is exactly the shape the DNS-SD browser already has, so it gets a
//    `QSocketNotifier` on the GUI thread (`ppcp_host_service.cpp:1187`).
//    ⛔ Teardown order is load-bearing: the notifier is destroyed BEFORE the
//    Watch is stopped, because "a QSocketNotifier left on a closed socket" is a
//    recorded trap in this codebase (`ppcp_host_service.cpp:1205`).
//
// ⛔ The DIAL gets its own thread, and it must NEVER be the accept thread.
//    `ppcp_host_service.cpp:242-296` polls `acceptChannelFor()` per half-built
//    link and then blocks 250 ms in `accept()`; a wired dial is a usbmux
//    `Connect` plus a presence read plus two concurrent TLS handshakes and can
//    block for hundreds of milliseconds — long enough to starve WiFi accepts
//    and stall preview-channel collection (design §6.3).  The finished link
//    crosses back with the identical `QMetaObject::invokeMethod(...,
//    Qt::QueuedConnection)` hand-off the accept thread uses, and under the same
//    rule: only self-contained objects cross, and the `PeerConnection` is
//    ADOPTED on the GUI thread.
//
// ── ABSENCE IS NEVER AN ERROR (design §6.2, RV 3.6a) ───────────────────────
//
// No usbmux provider, nothing plugged in, a charge-only cable, the capture app
// backgrounded, a phone this host has never paired with — none of those is an
// error state and none gets a banner.  Each produces at most one `ppWarn()`
// line, specific enough to diagnose, per the one-log rule.  Silent in the UI
// and specific in the log are not in tension; they are the requirement.

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

#include <QObject>
#include <QTimer>
#include <QSocketNotifier>
#include <QString>

#include "ppcp_transport.h"
#include "ppcp_usbmux.h"

namespace Ppcp {

// ── Contract C3 — the `WiredPresence` record and its reader rules ──────────
//
// A CBOR definite-length map the device serves in the clear on
// `Usbmux::kWiredPresencePort`, over the tunnel, and then closes.
//
//   { "pv": "1.0", "role": "capture", "dl"?: tstr,
//     "peers": [ { "port": uint, "psk_identity": bstr(17) } ] }
//
// ⛔ THERE IS NO FRAMING.  The device writes the record and closes the
// connection; the host reads to EOF under a byte cap and a deadline.  A short
// read, a timeout or a cap breach is the same refusal as a parse failure, and a
// refusal is silence plus one log line — the device is simply treated as not
// wired (design §5.3, §6.2).
constexpr std::size_t kWiredPresenceMaxBytes  = 4096;
constexpr std::size_t kWiredPresenceMaxPeers  = 16;
// RV 5.3a — `0x01 || rn2(8) || tag(8)`.  Exactly 17, never "at least".
constexpr std::size_t kWiredPresenceIdentityBytes = 17;
constexpr int         kWiredPresenceReadMs    = 2000;

// ⛔ THE WIRED HANDSHAKE IS BOUNDED FAR TIGHTER THAN THE WIFI ONE, AND THAT IS A
// SHUTDOWN REQUIREMENT AS MUCH AS A TIMING ONE.  `Options::handshakeTimeoutMs`
// defaults to 10 s, which is a generous and correct budget for a phone dialling
// in over a contended radio.  On a cable it is not: min_rtt over usbmux is
// ~1 ms (design §1), so a TLS handshake still unfinished after this long is not
// slow, it is broken.
//
// The reason it is a CONSTANT here rather than left at the default is
// `stop()`: it joins the worker, and the worker can be sitting inside
// Connector::connect().  At the default, quitting the app mid-dial blocks the
// GUI thread for up to 10 s with no window and no explanation — the classic
// "it hung on exit" report.  With this, the worst case is one presence dial
// plus one read plus one handshake, and every one of the three is now checked
// against m_stopping before it is entered.
constexpr int         kWiredHandshakeMs       = 3000;

// ── The retry cadence, and why the wired path needs one at all ─────────────
//
// ⛔ FOUND ON HARDWARE 29 Aug 2026: without this the wired path fires ONLY on a
// usbmux `Attached` event, and the most common real sequence produces no such
// event.  A phone is left on the cable to charge, the host starts, ONE presence
// read is attempted, the capture app happens to be backgrounded, `Number=3`
// comes back — and nothing ever looks again.  Observed live: the phone's own
// WiFi dial then won the link a minute later when the app WAS foregrounded.
//
// ⚠ "Phone already plugged in, operator then opens the app" is the normal way
// this product is used, so an attach-only trigger means wired essentially never
// engages in the field.
//
// ✅ §6.1 rule 4 says *"presence unreadable → change nothing"*, which forbids
// disturbing the WiFi path on a failed read.  It does NOT say "never look
// again", and re-probing costs one usbmux Connect plus a few HMACs.
// ⛔ FLAT, NOT EXPONENTIAL, AND THAT IS A RACE THE CADENCE HAS TO BE ABLE TO
// WIN.  Measured 29 Aug 2026 with a 2→4→…→30 s backoff: the operator opened the
// capture app, **the phone dialled over WiFi 4 s later**, and the host's next
// wired probe was up to 30 s away — so WiFi took the link every time and the
// cable never got a look in.
//
// The thing being raced is the phone's own reconnect, which fires on app
// foreground and is quick.  A probe is one `connect()` on a local unix socket
// plus a refused plist round trip — a millisecond or two — so probing every 2 s
// for as long as a phone sits on a cable costs nothing worth measuring, and an
// exponential ramp buys nothing but a lost race.
//
// ⚠ It does NOT close the race, and §6.1's advertisement suppression is what
// actually resolves it (Phase 2).  See the Log's note on the chicken-and-egg in
// §6.1: the host may not suppress before presence is proven (rule 4 — plugging
// in to charge must never disturb WiFi), and it cannot prove presence before
// the app is up, by which time the phone has already dialled.
constexpr int         kWiredRetrySecs         = 2;

// ⛔ HOW OFTEN TO RE-OPEN A USBMUX DAEMON THAT WAS NOT THERE AT STARTUP.
// Slower than kWiredRetrySecs on purpose: that one is racing the phone's own
// WiFi dial and must win it, whereas this races nothing — the daemon either
// exists or it does not, and re-opening a socket that is not there costs a
// connect() and an error every time.
//
// ⚠ ON LINUX THIS IS THE NORMAL CASE, NOT AN EDGE.  The open-source `usbmuxd`
// EXITS when the last device detaches, so a Studio started before a phone is
// plugged in finds no daemon at all — measured 29 Aug 2026.  Apple's runs
// permanently, which is why macOS almost never reaches this path.
constexpr int         kWiredWatchRetrySecs    = 5;

struct WiredPresence {
    // One entry per listener the device holds — contract C5 makes that one per
    // held pairing, each on its own ephemeral port.  ⚠ It is NOT a list of
    // hosts.
    struct Listener {
        std::uint16_t port = 0;
        PskIdentity   identity;   // the raw 17 octets, binary (RV 5.3f)
    };

    std::string pv;     // "1.0"; RV 3.3a — MAJOR is what is filtered on
    std::string role;   // "capture"
    // ⛔ UNTRUSTED (RV 4.4d).  Already run through `sanitiseLabel()` by the
    // reader, because a raw counterpart string reaching a log or a row is the
    // thing 4.4d exists to stop.  Never a key, never an identifier.
    std::string displayLabel;
    std::vector<Listener> peers;
};

// Contract C3's reader, verbatim: any key order, unknown keys ignored (forward
// compatibility, ENC I13), `pv` MAJOR must be 1, `peers` present, non-empty and
// at most 16, every `psk_identity` exactly 17 bytes, the whole record at most
// 4096 bytes.  Returns false with a short, non-secret reason in `why`.
bool parseWiredPresence(const unsigned char *data, std::size_t len,
                        WiredPresence *out, std::string *why);

// RV 5.3b, run client-side (design §5.2).  Offers each entry's identity to the
// host's own resolver — the SAME `PpcpRendezvous::identityResolver()` the
// listener authenticates with — and takes the FIRST that resolves.
//
// ⛔ No match is not a failure: it is a phone this host is not paired with, and
// RV 3.4c's rule is to stay silent.  Returns false and that is the whole of it.
bool resolveFirstWiredPeer(const WiredPresence &record, const IdentityResolver &resolve,
                           std::size_t *whichPeer, ResolvedPairing *out);

// ── The orchestrator ───────────────────────────────────────────────────────
class PpcpWiredLink : public QObject
{
    Q_OBJECT

public:
    // ⛔ WIRED IS ON BY DEFAULT AS OF 29 Aug 2026, and the env gate that used
    // to hold it back is gone.  `PINPOINT_PPCP_WIRED=0` still forces it OFF —
    // an escape hatch for a range that hits trouble, not a feature flag.
    //
    // ⚠ WHAT MADE IT SAFE TO DEFAULT ON was not the advertisement suppression
    // §6.1 originally proposed — that could never win the race (the host may
    // suppress only once presence is PROVEN, and proof arrives the same instant
    // the phone dials).  Two things replaced it: the cable TAKES OVER from an
    // idle WiFi link rather than racing it, and `onDeclare()` closes a duplicate
    // link keyed on the counterpart.  Without that second one a phone could hold
    // two links and enter the arbiter twice, which is silent wrong data.
    static bool enabled();

    explicit PpcpWiredLink(QObject *parent = nullptr);
    ~PpcpWiredLink() override;

    PpcpWiredLink(const PpcpWiredLink &) = delete;
    PpcpWiredLink &operator=(const PpcpWiredLink &) = delete;

    // Where a finished link goes.  Called on the GUI thread with the pairing
    // this host resolved BEFORE it dialled — contract C2's `resolvedPairingId`.
    using AdoptFn = std::function<void(std::unique_ptr<PeerConnection>, const QString &)>;
    void setAdoptHandler(AdoptFn f) { m_adopt = std::move(f); }

    // RV 5.3b's resolver, taken as it stands from `PpcpRendezvous`.
    //
    // ⚠ REUSED, NOT REIMPLEMENTED, AND THAT IS A DESIGN CONSTRAINT (§5.3): a
    // second, faster resolver would be a second place for expiry (7.3e),
    // exhaustion (7.3a) and invalidation (7.3b) to be got wrong.  It keeps its
    // no-early-exit constant-time comparison and its policy-after-resolve
    // ordering, neither of which matters on this side and both of which are
    // cheaper to keep than to reason about a second time.
    void setIdentityResolver(IdentityResolver r) { m_resolve = std::move(r); }

    // ── §6.1 rule 1, WIDENED so the cable can take over from WiFi ─────────
    //
    // The original rule was "a live link for that peer already exists → do
    // nothing, never open a second, on either transport", and it made the cable
    // unusable in the ordinary case: the phone dials over WiFi the instant the
    // app is foregrounded, wins the race, and the cable then declines to try.
    //
    // ⛔ THE RACE CANNOT BE WON BY SUPPRESSING FIRST.  Rule 2 allows the host to
    // withhold its advertisement only once wired is PROVEN, rule 4 forbids
    // withholding it on mere attachment ("plugging a phone in to charge must
    // never disturb WiFi"), and presence cannot be proven until the capture app
    // is up — which is the same instant the phone dials.  So the host is never
    // permitted to act before the phone has already acted.
    //
    // ✅ The answer is to stop racing and TAKE OVER instead: let WiFi connect,
    // dial the cable anyway, and drop the WiFi link once the cable is up.
    //
    // ⚠ This is NOT the migration §7.3 forbids.  Nothing carries a clock fit
    // across: the cable link is a fresh `ppcp_peer` with a fresh estimator, and
    // the WiFi link is destroyed rather than converted.  §7.3 names exactly this
    // as the honest form.
    //
    // ⛔ AND IT COSTS A MEASURED 35 SECONDS.  A new cable link's `offset_sigma`
    // starts at 8.4 ms and does not fall under the 5 ms arbitration gate until
    // t+35 s (measured 29 Aug 2026), so a shot crossing in that window cannot be
    // arbitrated at all.  That is free at the start of a session and unacceptable
    // in the middle of one, which is why `Busy` exists and is honoured.
    enum class PeerLinkState {
        None,        // nothing for this pairing — dial
        WifiIdle,    // on WiFi, nothing has happened yet — dial and take over
        WifiBusy,    // on WiFi and working — ⛔ leave it alone
        Wired        // already on the cable — nothing to do
    };
    void setPeerLinkState(std::function<PeerLinkState(const QString &)> f)
    {
        m_peerState = std::move(f);
    }

    // Drops the WiFi link for this pairing so the cable can replace it.  Called
    // on the GUI thread immediately before the new link is adopted.
    void setDropForTakeover(std::function<void(const QString &)> f)
    {
        m_dropForTakeover = std::move(f);
    }

    // Opens the usbmux `Listen` connection and arms the notifier.  Silent and
    // harmless when the gate is off, when there is no provider, or when nothing
    // is plugged in.  Idempotent.
    void start(Usbmux::Provider provider = Usbmux::Provider::platformDefault());
    void stop();

    // True once stop() has asked the worker to wind up.  ⚠ Checked BETWEEN the
    // phases of a dial, never inside one: a job that is already in a usbmux
    // Connect or a TLS handshake runs to its (now bounded) timeout, and the
    // point of this is only that a stopping worker does not START the next
    // phase.  Takes m_jobMutex, so it must not be called while holding it.
    bool stopping() const;

    bool active() const { return m_watch.active(); }

    // A phone's link ended, so any attached device may be dialable again.
    // Clears the "already linked" marks and re-arms the retry for every device
    // still on a cable.
    //
    // ⚠ It takes NO pairing id on purpose.  Re-arming everything costs one
    // presence read per attached phone and is guarded by §6.1 rule 1 before a
    // handshake is ever attempted; remembering which udid owned which pairing
    // would be the `udid -> pairingId` cache design §10 forbids, and it would go
    // stale the moment a phone were re-paired.
    void retryNow();

    // For the diagnostic export and the log: what the wired path is doing, with
    // no identity, key or UDID in it (RV 7.2b).
    QString describe() const;

private slots:
    void onWatchReadable();
    // One tick a second; enqueues whichever attached devices are due.
    void onRetryTick();
    // Opens the usbmux device watch and arms its notifier.  Returns false when
    // there is no daemon, which is an ordinary outcome and not an error.
    bool tryOpenWatch();

private:
    // ── The GUI thread half ────────────────────────────────────────────────
    void handleAttached(const Usbmux::Device &d);
    void handleDetached(Usbmux::DeviceId id);
    // Runs on the GUI thread; the worker posted it here.
    void onDialFinished(PeerConnection *raw, QString resolvedPairingId,
                        Usbmux::DeviceId deviceId);

    // What one dial attempt ended as.  ⚠ It travels back to the GUI thread
    // rather than being logged where it happens, because whether it is worth a
    // log line depends on the PREVIOUS attempt's outcome — state that lives
    // here and not on the worker.
    enum class DialOutcome {
        NoPresence,       // the tunnel was refused — Number=3, and it is ambiguous
        Unreadable,       // the tunnel opened and the record did not arrive
        Refused,          // the record arrived and failed contract C3
        NoMatch,          // parsed, and no held pairing resolves it (RV 3.4c)
        HandshakeFailed,  // resolved, and the PPCP dial did not complete
        Adopted           // a link
    };
    // `pairing` is empty until a dial got far enough to resolve one; once known
    // it is kept so the retry tick can skip a phone that is busy on WiFi without
    // paying for a presence read and two TLS handshakes to find out.
    void noteDialOutcome(const std::string &udid, DialOutcome o, const QString &why,
                         const QString &pairing = QString());

    // Per-attached-device retry state.  Attachment-scoped: created on Attached,
    // erased on Detached, exactly like m_attached.
    struct Retry {
        std::string      udid;
        Usbmux::DeviceId deviceId = 0;
        int  dueInSecs   = 0;                      // counts down on the tick
        bool linked      = false;                  // a link was adopted; stop
        QString knownPairing;                      // once resolved, remembered
        bool everLogged  = false;
        DialOutcome lastLogged = DialOutcome::NoPresence;
    };
    Retry *retryFor(const std::string &udid);

    // ── The worker thread half ─────────────────────────────────────────────
    struct Job {
        Usbmux::DeviceId deviceId = 0;
        std::string      udid;      // ⛔ the STABLE identity; deviceId is not
    };
    void workerLoop();
    void runJob(const Job &j);

    // Reads the presence record off a tunnel fd: to EOF, at most
    // kWiredPresenceMaxBytes, inside kWiredPresenceReadMs.  Closes `fd`.
    static bool readPresence(pp_socket_t fd, std::vector<unsigned char> *out,
                             std::string *why);

    Usbmux::Provider                 m_provider;
    Usbmux::Watch                    m_watch;
    std::unique_ptr<QSocketNotifier> m_notifier;

    AdoptFn                                m_adopt;
    IdentityResolver                       m_resolve;
    std::function<PeerLinkState(const QString &)> m_peerState;
    std::function<void(const QString &)>         m_dropForTakeover;

    // ⛔ Keyed by UDID, never by DeviceID.  MEASURED 29 Aug 2026: the same
    // iPhone on the same cable was DeviceID 306 in the morning and 308 in the
    // afternoon with an unchanged SerialNumber.  A DeviceID is usbmuxd's handle
    // on ONE attachment and is valid only between that attachment's `Attached`
    // and its `Detached`.
    //
    // ⚠ AND THERE IS DELIBERATELY NO `udid -> pairingId` CACHE (design §10).
    // The identity has to be re-read each time anyway — the device refreshes
    // `rn2` per listener session — resolution is one HMAC per held pairing, and
    // a cache would go stale the moment a phone were re-paired.  This set holds
    // ONLY "a dial is already in flight for this phone", which is in-attachment
    // state and nothing else.
    std::vector<std::string> m_dialling;      // GUI thread only

    // Detached is announced with ONLY a DeviceID (see Usbmux::Watch::Event), so
    // the udid seen at attach has to be remembered to name the phone that left.
    // Attachment-scoped by construction: an entry is added on Attached and
    // erased on Detached.
    std::vector<std::pair<Usbmux::DeviceId, std::string>> m_attached;
    std::vector<Retry>                                    m_retry;    // GUI thread only
    QTimer                                                m_retryTimer;

    std::thread                m_worker;
    mutable std::mutex         m_jobMutex;   // mutable: stopping() is const
    std::condition_variable    m_jobCv;
    std::deque<Job>            m_jobs;
    bool                       m_stopping = false;   // guarded by m_jobMutex

    // GUI thread only: whether start() got as far as arming the notifier.  It
    // is what a queued hand-off arriving after stop() checks before adopting a
    // link into a service that has torn down.
    bool                       m_running = false;
    // ⚠ `m_running` means "this object is live"; `m_watchUp` means "we have a
    // usbmux daemon".  They were one thing and that WAS THE BUG: a missing
    // daemon at startup left the whole object dead for the life of the process.
    bool                       m_watchUp = false;
    bool                       m_watchAnnouncedDown = false;
    int                        m_watchDueInSecs = 0;

    // Counters for describe() — a diagnosis, never a secret.
    int m_attachedSeen = 0;
    int m_dialled = 0;
    int m_adopted = 0;
};

}  // namespace Ppcp
