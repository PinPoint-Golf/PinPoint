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

// THE OWNER.  Work package H-compose.
//
// Until this file existed, `docs/ppcp-conformance.md` §7.2 read: "Nothing in
// this application constructs a `PpcpHostPeer`.  H1's transport and H2's peer
// are built and tested; no screen, service or controller starts one."  Three
// joins were named in the code and had no caller, all for the same reason —
// there was nowhere for the link to live.  This is that place, and it is a
// separate translation unit rather than lines in `main.cpp` for a reason that
// cost a day on 22 Aug: `main.cpp` reaches whisper, ONNX Runtime, OpenCV and
// Sparkle, so nothing in `ppcp-tests` can compile it, and a break there is
// invisible until Mark builds the application.  Everything here is covered by
// the `ppcp_app_tu_syntax` row; main.cpp's share is four lines.
//
// WHAT IT OWNS, and why one object owns all of it:
//
//   - the `Listener` (H1) and the accepted link.  RV 5.2g: this host displays
//     the code and therefore LISTENS; the peer that scans dials it.
//   - the `PpcpRendezvous` (H6) whose resolver the listener authenticates
//     against, and which the accepted link's `pairingId` reports back to.
//   - the `PpcpHostPeer` (H2) and the libppcp engine behind it, which in turn
//     own the live Session, the arbitration bridge and the annotation store.
//
// They are one object because they share one lifetime and one failure: a
// pairing that is revoked kills a link, a link that drops leaves a Session that
// 7.5a can reconnect on the same K_tls, and every one of those is a transition
// somebody has to see the whole of.
//
// ⚠ IT OWNS ONE THREAD AND SAYS SO.  `Listener::accept()` blocks, and PPCP's
// pump must run whether or not bytes are arriving (`tick()` is driven by TIME
// PASSING — a link with no traffic still has heartbeats due).  So there is an
// accept thread whose only job is to block on `accept()` and hand the link
// over, and a `QTimer` on the GUI thread that pumps and ticks.  libppcp itself
// remains sans-I/O throughout: ground rule 7 says the embedding supplies the
// thread, and this is the embedding.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

#include <QDateTime>
#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QSocketNotifier>
#include <QTimer>

#include "ppcp_bootstrap.h"
#include <QVariantList>
#include <QVector>

#include "ppcp_discovery.h"
#include "ppcp_engine.h"
#include "ppcp_host_peer.h"
#include "ppcp_import_ledger.h"
#include "ppcp_import_sink.h"
#include "ppcp_qr.h"
#include "ppcp_rendezvous.h"
#include "ppcp_transport.h"

class PpcpOfferController;
namespace Ppcp { class PpcpWiredLink; }
// The preview consumer each connected phone's camera Sources get (see Phone).
class VideoInputPpcp;

class PpcpHostService : public QObject
{
    Q_OBJECT

    // ── What the "Pair a device" panel binds to ────────────────────────────
    // In-screen, in the DEVICES area of the home screen.  No menu and no native
    // dialog: every control this application has is on the screen where its
    // result appears.
    Q_PROPERTY(bool listening READ listening NOTIFY stateChanged)
    Q_PROPERTY(quint16 port READ port NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // ⚠ "ANY PHONE", NOT "THE PHONE".  Two camera angles is the core case —
    // down-the-line and face-on are two phones — so these are aggregates over
    // however many are on the link, and `connectedCount` is what a caller needs
    // when the difference matters.  `peerName` is the ONE name only while there
    // is one; with several it is empty, because there is no honest single
    // answer and inventing one is how a panel ends up lying about which phone
    // it is talking to.
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(int connectedCount READ connectedCount NOTIFY stateChanged)
    Q_PROPERTY(QString peerName READ peerName NOTIFY stateChanged)
    // Read by Settings; written by setStudioName().
    Q_PROPERTY(QString studioName READ studioName NOTIFY studioNameChanged)
    Q_PROPERTY(QStringList connectedNames READ connectedNames NOTIFY stateChanged)

    // ── A phone that arrived and did not become a link ──────────────────────
    // Until these existed the pairing panel could only say "still waiting",
    // whether nothing had happened or a phone had reached us and been refused
    // ten seconds ago.  Every one of those outcomes was already on the app log
    // and none of it was on the screen.
    //
    // ⚠ WHAT MAY BE SAID, AND WHAT MAY NOT.  RV 7.7c makes rejection uniform
    // "in what it RETURNS or in how long it TAKES" — it binds what the
    // COUNTERPART observes, and 7.7b likewise forbids disclosure "to an
    // unauthenticated counterpart".  Neither reaches what this host puts on its
    // own screen, and RV 4.2b ("a pairing code that fails with 'could not pair'
    // tells a user nothing they can act on") and §8 ("detect the symptom and
    // explain it") ask for exactly this.  So a bind refusal, a timeout and a
    // 5.2b policy refusal are named.  The AUTHENTICATION failure is not, and
    // cannot be: `Ppcp::HandshakeFailure` carries one uniform message for an
    // unresolvable identity and a wrong key alike, and nothing here unpicks it.
    Q_PROPERTY(QString lastFailureText READ lastFailureText NOTIFY failureChanged)
    // Rises on every failure, including a repeat of the same one.  The text
    // alone cannot drive a QML binding: a phone that fails twice for the same
    // reason produces the same string, and the panel would not re-evaluate.
    Q_PROPERTY(int failureCount READ failureCount NOTIFY failureChanged)

    // The pairing code, as a QR.  `qrRows` is one string of '0'/'1' per row,
    // which a Canvas draws directly.
    //
    // ⚠ THE URI ITSELF IS NOT A PROPERTY AND MUST NOT BECOME ONE.  RV 4.4c and
    // 7.2b forbid a payload reaching a log, a crash report or a diagnostic
    // export, and a QML property is one `console.warn` away from all three.
    // The code exists to be photographed off the screen; the modules are the
    // only form of it this class publishes.
    Q_PROPERTY(QVariantList qrRows READ qrRows NOTIFY codeChanged)
    Q_PROPERTY(int qrSize READ qrSize NOTIFY codeChanged)
    Q_PROPERTY(bool codeLive READ codeLive NOTIFY codeChanged)
    Q_PROPERTY(int codeSecondsLeft READ codeSecondsLeft NOTIFY codeChanged)
    Q_PROPERTY(QStringList codeEndpoints READ codeEndpoints NOTIFY codeChanged)
    Q_PROPERTY(QVariantList outstandingCodes READ outstandingCodes NOTIFY codeChanged)

    // ── Every phone this host knows about, as a device ──────────────────────
    // A paired phone IS a device and belongs in the ordinary enumeration beside
    // the cameras and the IMUs, not in a bespoke table of pairing handles.  The
    // rows use the same key vocabulary `ResourceMonitorController` builds for a
    // Camera and an IMU, so the DEVICES list, the resource monitor and the
    // Settings panel all read one shape.
    //
    // ⚠ AND A PHONE THAT IS NOT HERE IS STILL LISTED, which is the IMU rule
    // ("Devices appear regardless of connection state") and not the camera one.
    // A remembered pairing is a standing ability to reconnect; it is a device
    // that is switched off, not a device that does not exist.
    Q_PROPERTY(QVariantList phones READ phones NOTIFY phonesChanged)

    // ── Aggregates over CONNECTED phones, for the session toolbar ───────────
    // The same shape `ImuManager::lowBatteryPercent` has: -1 means "no
    // connected phone has reported a level yet", not "0%".
    // ⛔ THEY NOTIFY OFF phoneHealthChanged(), NOT phonesChanged().  Every one
    // of these three moves when a READING arrives — a `heartbeat_ack` or a
    // `relation_update` — and those deliberately do not emit the structural
    // signal, because it rebuilds Settings -> Phones (see phoneHealthChanged
    // below).  `onDeclare()` and `dropPhone()` emit phoneHealthChanged() as
    // well as phonesChanged() so that the set these aggregate over changing
    // moves them too.
    Q_PROPERTY(int phoneLowestBatteryPct READ phoneLowestBatteryPct NOTIFY phoneHealthChanged)
    // "" when no connected phone has a reading yet; otherwise the most severe
    // `ThermalLevel` any connected phone has reported ("nominal".."critical").
    Q_PROPERTY(QString phoneWorstThermal READ phoneWorstThermal NOTIFY phoneHealthChanged)
    // 6.1f's `outSigmaNs`, worst-case across every related timebase on every
    // connected phone, in milliseconds. -1 while no relation has an estimate
    // yet (fresh connection, still in the ~2-minute burst-then-maintenance
    // convergence window) — never 0, which would claim a perfect sync nobody
    // has measured.
    Q_PROPERTY(double phoneWorstSyncSigmaMs READ phoneWorstSyncSigmaMs NOTIFY phoneHealthChanged)

    // ── RV-6 guided pairing, the INITIATOR half (H10) ───────────────────────
    //
    // A first pairing with no code carried between two screens: this host
    // browses for open bootstrap windows (3.3f, §3.7), the user picks ONE, and
    // both ends show six digits a person compares.  The engine and the traps
    // live in ppcp_bootstrap.h; this is the surface QML binds to.
    //
    // ⛔ TRAP 3 (11.3d1) IS WHY `guidedWindows` CARRIES NO DIGITS.  Discovered
    // windows are candidates; digits exist only for the ONE attempt that is
    // running.  A list of windows each showing a number would give an attacker
    // advertising N windows N blind draws against one confirmation, with the
    // operator finding the collision for them.  There is no property here that
    // could hold a second set of digits, and `beginGuidedPairing()` refuses
    // while an attempt is live.
    Q_PROPERTY(bool guidedAvailable READ guidedAvailable NOTIFY guidedChanged)
    Q_PROPERTY(QVariantList guidedWindows READ guidedWindows NOTIFY guidedChanged)
    Q_PROPERTY(bool guidedActive READ guidedActive NOTIFY guidedChanged)
    // idle | dialling | exchanging | comparing | confirming | paired | failed
    Q_PROPERTY(QString guidedPhase READ guidedPhase NOTIFY guidedChanged)

    // ⛔ 11.7a/11.7e/11.7f — "435 948", and EMPTY outside the one window in
    // which the digits exist.  Nothing before 11.5d has completed (there is
    // nothing to compare, and a progressive display would leak the value to
    // whichever side an attacker reached first) and nothing after the attempt
    // ends (they are a function of two ephemeral keys and are meaningless
    // outside the attempt that produced them).
    Q_PROPERTY(QString guidedDigits READ guidedDigits NOTIFY guidedChanged)

    // ⛔ 11.9c — `guidedMayRetry` IS FALSE FOR A MISMATCH OR A MAC FAILURE AND
    // THE DIALOGUE MUST SHOW NO RETRY AFFORDANCE AT ALL WHEN IT IS.  Not a
    // greyed-out one, not one behind a confirmation — none.  A mismatch is the
    // one signal this path produces that an attack is under way, and a dialogue
    // whose reflex is *try again* converts a one-shot bound into an unbounded
    // one by way of the operator's muscle memory.
    Q_PROPERTY(QString guidedMessage READ guidedMessage NOTIFY guidedChanged)
    Q_PROPERTY(bool guidedMayRetry READ guidedMayRetry NOTIFY guidedChanged)
    // 11.9d1 — the pairing code is offered on the FIRST `unsupported_version`
    // abort and on the second of anything else.
    Q_PROPERTY(bool guidedOfferCode READ guidedOfferCode NOTIFY guidedChanged)

public:
    explicit PpcpHostService(QObject *parent = nullptr);
    ~PpcpHostService() override;

    // The offer list of H5, which `main.cpp` has been installing DETACHED since
    // that session because there was no live peer to attach it to.  Hand it
    // over before start(); it is attached when a link arrives and detached when
    // one goes.
    void setOfferController(PpcpOfferController *c);

    // Opens the listener and builds this host's own `declare` (MSG 3.3c: a peer
    // declares before it originates any message referencing a Source).  Port 0
    // takes an ephemeral one, which is what a code carries anyway.
    bool start(quint16 port, QString *err = nullptr);
    void stop();

    bool    listening() const { return m_listening; }
    quint16 port() const { return m_port; }
    QString status() const { return m_status; }
    bool    connected() const { return !m_phones.empty(); }
    int     connectedCount() const { return static_cast<int>(m_phones.size()); }
    QString peerName() const;
    QStringList connectedNames() const;
    // The per-heartbeat readings for one pairing, so a panel can bind them
    // WITHOUT re-reading `phones()` and rebuilding its rows.  Same values and
    // same "no reading" sentinels (-1 / empty) that `phones()` publishes.
    Q_INVOKABLE QVariantMap phoneHealth(const QString &pairingId) const;

    QString lastFailureText() const { return m_lastFailureText; }
    int     failureCount() const { return m_failureCount; }

    QVariantList qrRows() const { return m_qrRows; }
    int          qrSize() const { return m_qr.size(); }
    bool         codeLive() const { return m_codeLive; }
    int          codeSecondsLeft() const;
    QStringList  codeEndpoints() const { return m_codeEndpoints; }
    QVariantList outstandingCodes() const;
    QVariantList phones() const;
    int          phoneLowestBatteryPct() const;
    QString      phoneWorstThermal() const;
    double       phoneWorstSyncSigmaMs() const;

    // RV 7.3c leaves the exact expiry to the publisher and this host chose 300
    // seconds.  Settable ONLY so `ppcp_host_service_test` can watch a code run
    // out without a five-minute test; nothing in the application calls it, and a
    // value <= 0 restores the default rather than minting a code that is already
    // expired.
    void setCodeLifetimeSecondsForTest(int seconds);

    // The failure path, without a phone.  ⚠ Settable ONLY for the tests, like
    // the lifetime above, and for a sharper reason: `ppcp_host_service_test`
    // links `ppcp_host_service_stubs.cpp`, whose whole point is that this suite
    // never accepts a link — "if a future test in this file did accept a link,
    // it would be asserting against stubs".  So the only honest way to reach
    // `noteFailure()` from that suite is to call it.  Nothing in the
    // application does; the accept thread is the sole real caller.
    void noteHandshakeFailureForTest(const Ppcp::HandshakeFailure &f);

    // ── Contract C2's two arms, without a cable ────────────────────────────
    //
    // ⚠ FOR THE TESTS ONLY, and it exists because the wired path's hardware is
    // the one thing a suite cannot have.  The stub usbmuxd's tunnel is a byte
    // echo, not a PPCP listener, so an end-to-end wired dial is not reachable
    // from `ppcp_host_service_test`; what IS reachable — and what the whole of
    // C2 turns on — is adoptLink() with and without a resolved pairing.  This
    // is the only door to the second of those, since nothing else in the
    // application dials.
    void adoptLinkForTest(std::unique_ptr<Ppcp::PeerConnection> link,
                          const QString &resolvedPairingId);

    // ⚠ TEST SEAM for §6.1's duplicate-link backstop, and it exists because the
    // backstop is unreachable from the suite otherwise: `counterpartId` is only
    // known at `onDeclare()`, which needs a `declare` off a real wire.  Drives
    // the same function the engine drives, with a synthetic counterpart id.
    // Returns false if `index` names no phone.
    // `actuatorKind` is CR-02's addition: non-empty declares ONE Actuator of
    // that kind, so the suite can exercise the torch path without a counterpart
    // it is never going to have.  Empty — the default — declares none, which
    // 5.19c makes a complete declaration and not a degenerate one.
    // `actuatorControl` is `on_off` (CB1) unless a caller asks for `level`,
    // which is the only shape 12.1c's ACHIEVED-differs-from-REQUESTED case is
    // expressible in — see the H18 note on `liveSessionForTest()`.
    bool declareForTest(std::size_t index, const QString &counterpartId,
                        const QString &actuatorKind = QString(),
                        const QString &actuatorControl = QString());

    // ⚠ TEST SEAM, H18.  This suite links `ppcp_host_service_stubs.cpp` and
    // never accepts a link from a real engine, so no `actuator_command_ack`
    // can ever ARRIVE here — and 12.1c's achieved value is carried by nothing
    // else.  The wire round trip (two engines, the device answering with a
    // CLAMPED level) is asserted in `ppcp_live_session_test`; what this seam
    // buys is the other half — that whatever the reading holds is what
    // `actuatorRowsFor()` publishes to QML, unaltered.  Null where `index`
    // names no phone or the phone holds no peer.
    Ppcp::PpcpLiveSession *liveSessionForTest(std::size_t index);

    // RV §4 — publish a code.  Fresh psk and sid per code (7.3d), every
    // reachable address in `ep` (4.3d), `mu: 1` (7.3a) and a short `exp`
    // (7.3c).  Displaces whatever code was showing, which 7.3b then invalidates.
    Q_INVOKABLE bool publishPairingCode() { return publishCode(/*userAsked=*/true); }
    // 7.3b — invalidate the displayed code, used or not.
    Q_INVOKABLE void closePairingCode();
    // 7.4b — a completed pairing is remembered automatically (erratum E57,
    // 25 August 2026, downgraded 7.4b's opt-in clause to a SHOULD) and stays
    // visible and individually revocable, which this is the revoking half of.
    Q_INVOKABLE void forgetPairing(const QString &pairingId);

    // A user-given label, independent of what the phone calls itself in MSG
    // 3.3's `declare` (untrusted, 4.4d) — the same relationship `cameraAlias`
    // and `imuAlias` have to a device's own reported name.  Empty clears it,
    // falling back to the declared name.  Stored beside `ppcp/phoneNames`
    // rather than through `AppSettings`: `Ppcp` has no dependency on `Gui/app`
    // and this keeps it that way.
    Q_INVOKABLE void setPhoneAlias(const QString &pairingId, const QString &alias);

    // ── RV-6 guided pairing (H10) ───────────────────────────────────────────
    bool         guidedAvailable() const;
    QVariantList guidedWindows() const;
    bool         guidedActive() const;
    QString      guidedPhase() const;
    QString      guidedDigits() const;
    QString      guidedMessage() const;
    bool         guidedMayRetry() const;
    bool         guidedOfferCode() const;

    // ⛔ THE ONE DOOR (11.3d1, trap 3).  Takes ONE instance name — the one the
    // user selected BEFORE any attempt began — and refuses while an attempt is
    // live.  There is no overload taking a list and adding one would be the
    // trap: `dl` exists so a browser seeing four windows can tell them apart,
    // and the obvious host interface is a list, so dialling the list is the
    // obvious next step and it is the thing 11.3d1 forbids.
    Q_INVOKABLE bool beginGuidedPairing(const QString &instanceName);

    // ⛔ 11.7c AND TRAP 8 — CALL ONLY FROM A CONTROL A PERSON TOUCHED.  "A peer
    // MUST NOT treat the arrival of the counterpart's `bs_confirm` as standing
    // in for its own user's."  Comparing the digits in software, or accepting
    // the counterpart's assertion that they matched, removes the entire
    // security of this path while leaving every byte on the wire unchanged —
    // and passes every static test in the document.  The comparison has value
    // only because it crosses a channel the attacker is not on, and the only
    // such channel is a person looking at two screens.
    Q_INVOKABLE void confirmGuidedDigitsMatch();

    // The user says the numbers do NOT match.  11.9a ends the attempt; 11.9c
    // then governs what they are told and `guidedMayRetry` is false.
    Q_INVOKABLE void rejectGuidedDigits();

    // The user walked away, or closed the dialogue mid-exchange.
    Q_INVOKABLE void cancelGuidedPairing();

    // 11.9b — dismissing a result is the "further explicit user action" that
    // has to happen before another attempt may begin.  Nothing retries on its
    // own and nothing reopens without this.
    Q_INVOKABLE void dismissGuidedResult();

    // ── The harness tap (RT-20c) ────────────────────────────────────────────
    //
    // RT-20c drives BOTH applications either side of the relay and cannot have
    // a person in the room for the protocol assertions.  This names THE CONTROL
    // a person would operate — the same vocabulary PinPointCapture's
    // `UserAction` uses — and dispatches to exactly the four entries above, so
    // the harness path and the real path are one path and cannot diverge.
    //
    // ⛔⛔ IT SUPPLIES THE TAP AND NEVER THE COMPARISON (11.1d, TRAP 8).
    // Nothing here reads the six digits, nothing receives the counterpart's,
    // and nothing decides whether they match.  `"match"` means *a person
    // pressed the button labelled "yes, they match"*, and the harness was told
    // to send it — it is not a conclusion drawn from any value.  A peer that
    // compared the digits in software, or accepted the counterpart's assertion
    // that they matched, "removes the entire security of the path while leaving
    // every byte on the wire unchanged" and passes every static test in the
    // document.
    //
    // ⛔ COMPILED ONLY UNDER PP_PPCP_RV6_HARNESS, WHICH A SHIPPING BUILD
    // REFUSES TO CONFIGURE.  Same gate and same refusal as the plaintext
    // conformance listener, for a sharper reason: this one presses "yes".
    //
    // `control` is one of: "match", "different", "cancel", "dismiss".
    // Returns false for an unknown control or when there is nothing to act on.
    Q_INVOKABLE bool guidedUserAction(const QString &control);

    // 3.7h — offer a window at an endpoint learned out of band, so a harness
    // can reach the interposed relay of RT-20c without a fake mDNS responder.
    // It becomes one more CANDIDATE; nothing dials it and 11.3d1 is unchanged.
    //
    // ⚠ HARNESS-GATED FOR NOW BECAUSE OF THE UI DECISION, NOT THE PROTOCOL.
    // 3.7h makes an out-of-band endpoint conformant; whether this application
    // should offer a "type an address" field is a product question nobody has
    // asked, and quietly answering it here would be the wrong way to settle it.
    Q_INVOKABLE bool addGuidedEndpoint(const QString &host, int port,
                                       const QString &label);

    // RT-9 — everything this subsystem contributes to a diagnostic bundle, and
    // it is constructed from a struct that holds no secret rather than filtered
    // afterwards.  Q_INVOKABLE so the diagnostics screen can offer it.
    Q_INVOKABLE QString diagnosticExport() const;

    // ── What a phone calls this computer (RV 4.3 / 4.4d) ───────────────────
    //
    // ⛔ IT USED TO BE THE CONSTANT "PinPointStudio", WHICH MEANS EVERY STUDIO A
    // PHONE HAS EVER PAIRED WITH IS CALLED THE SAME THING.  Raised by Mark while
    // testing one phone against macOS, Linux and Windows: his Remembered Studios
    // list read "PinPointStudio" three times and there was no way to tell which
    // row was which machine, or which to forget.  A studio with several hosts is
    // rare; a DEVELOPER with several hosts is not, and neither is a range that
    // replaces a machine.
    //
    // ⚠ Defaults to the computer's own host name, so the common case needs no
    // configuration at all and three machines are three different names on day
    // one.  At most 64 bytes (4.3), and trimmed; an empty value falls back to the
    // default rather than publishing nothing.
    QString studioName() const;
    Q_INVOKABLE void setStudioName(const QString &name);
    // The value a fresh install would use — shown as placeholder text so the
    // field can be cleared back to it.
    Q_INVOKABLE static QString defaultStudioName();

    // ── What an automated run reads back to ASSERT ─────────────────────────
    //
    // The arbitration counters are otherwise printed once, when a phone
    // disconnects, and the import ledger's only surface is a file.  A test
    // driving a capture device against this host needs them WHILE the session
    // is live and as data rather than as a log line.  Machine-readable, no
    // secret and no payload — the same discipline `diagnosticExport()` follows.
    Q_INVOKABLE QVariantMap ppcpStats() const;

    // ⭐ THE CLIP CHAIN, PUBLISHED FROM WHERE IT IS COUNTED.
    //
    // `PpcpClipFiler` lives in src/Gui and this class must not reach into it —
    // so main.cpp, which owns both, pushes the numbers here for ppcpStats() to
    // report.  A rig then reads ONE object for the whole leg: asked, announced,
    // converted, filed.
    //
    // ⚠ Deliberately a push and not a pull: a Qt signal or a direct call would
    // put a src/Gui symbol into this translation unit, and ppcp_host_service_test
    // stubs the src/Video symbols precisely so a suite about the pairing clock
    // does not drag Aravis, Spinnaker and Bluetooth in behind it.
    void setClipChainStats(const QVariantMap &m) { m_clipChain = m; }

    // What this build's service discovery is, in the words RvBrowser::describe()
    // uses, or why there is none.  Part of the diagnostic export because 3.6a
    // gives discovery no error channel at all: when a remembered phone does not
    // reappear by itself, whether this host is even browsing is the first thing
    // anybody needs to know, and it is otherwise unanswerable.
    QString discoveryDescription() const;

    // The first connected phone's peer, for callers that predate several.
    // Null when nothing is connected.
    Ppcp::PpcpHostPeer *hostPeer();
    // A SPECIFIC phone's peer, by its declared `Peer.id` — `Phone::counterpartId`,
    // the same id embedded in a PPCP camera's device id
    // (`VideoInputPpcp::deviceIdFor()` — "ppcp:<peer_id>/<source_id>"), and NOT
    // `Phone::pairingId` (local pairing bookkeeping, unrelated to the wire
    // peer id).  Null when no phone with that id is connected.
    Ppcp::PpcpHostPeer *peerForId(const QString &counterpartId);
    Ppcp::PpcpRendezvous &rendezvous() { return m_rv; }

    // ── CORE §8.2 arbitration, as the shot pipeline needs to reach it ───────
    //
    // The bridge of the first connected phone whose arbiter actually started,
    // or null.  `ShotController` branches on this: while it is non-null the
    // host's local `ShotArbiter` is not consulted at all.
    //
    // ⛔ "FIRST", AND THAT IS A KNOWN LIMITATION, NOT A CHOICE.  A
    // `PpcpShotBridge` belongs to a `PpcpHostPeer`, one per LINK, and libppcp's
    // arbiter is bound to a single peer at construction — session id,
    // `timebase_ref`, relation set, `issued_by` and the channel the Shot leaves
    // on are all read back off that one peer.  So two phones are two arbiters
    // that cannot see each other's Candidates, and one strike yields two Shots
    // in two Sessions with no `shot_link` between them (8.2l).  A host-wide
    // arbiter wants to live HERE, fed by every peer, with one Session id across
    // phones and the issued Shot fanned out to the other links — none of which
    // libppcp provides today.  Until it does, this returns one bridge and the
    // second phone's arbitration is not consulted.  Recorded rather than
    // papered over, because the failure is otherwise silent.
    Ppcp::PpcpShotBridge *activeShotBridge();

    // ── CORE §8.4 — ask every connected phone for the clip around a shot ───
    //
    // ⭐ THE CALL THAT WAS NEVER MADE.  `PpcpShotBridge::requestCapture()` has
    // existed, correct and tested, with no caller anywhere in the tree — so a
    // phone holding the footage in its ring was never once asked for it, and
    // `bulk 0/0` on every link teardown was the whole finding.
    //
    // ⚠ FANNED OUT, NOT SENT THROUGH `activeShotBridge()`.  That answers the
    // FIRST phone's bridge, so a request issued through it could never name a
    // second phone's Streams.  8.4's request is the ORPHAN form precisely so a
    // host may ask a device that never nominated the shot; every connected
    // phone is asked, and each answers for itself.
    //
    // ⚠ ISSUED AT THE SHOT, NOT WHEN THE SWING FOLDER EXISTS.  Allocation is
    // 4-11 s later (post-roll plus the history gather), and every second of
    // waiting spends a ring that is finite.  The answer is filed against the
    // swing when the folder arrives; see PpcpClipFiler.
    //
    // `t0HostNs` is a reading of `Session.timebase_ref` — for this host, of the
    // same steady_clock `EventBuffer::nowMicros()` reads, times 1000.  The owner
    // inverts §6.1 into its own convention at its end; this host must not do it
    // for them (5.13c).
    //
    // Returns how many Streams were asked for, across all phones.
    int requestCaptureForShot(const QString &shotId, qint64 t0HostNs);

    // A stable, filesystem-safe alias for one phone camera, used as the
    // `streams[]` alias in swing.json and as the clip's filename stem.  The
    // declared label where there is one, plus a short digest of peer+source so
    // that two phones offering "iPhone 16 - Wide" stay distinguishable.
    static QString streamAliasFor(const QString &peerId, const QString &sourceId,
                                  const QString &label);

    // ── Where a clip actually surfaces ─────────────────────────────────────
    //
    // Every VideoInputPpcp this service owns, across every connected phone.
    //
    // ⚠ THE CONNECTION IS MADE OUTSIDE THIS CLASS, ON PURPOSE.  Binding
    // `clipReady` here would put `&VideoInputPpcp::clipReady` -- a moc-generated
    // symbol -- into this translation unit, and `ppcp_host_service_test` STUBS
    // the src/Video symbols rather than linking the real ones, precisely so a
    // suite about the pairing clock does not drag in Aravis, Spinnaker and
    // Bluetooth.  So this hands out the instances and the join is made where
    // the real class is already linked.
    //
    // ⚠ These are preview-ONLY in what they OPEN, and that does not make them
    // preview-only in what they SEE: dispatchEvent() hands every event for a
    // peer to every live instance bound to it, and ownership is resolved by
    // Source -- so a shot Capture for a camera they cover is assembled here and
    // emitted with preview=false.  When the phone is ALSO in use as a session
    // camera, that instance assembles the same clip too; the ledger's admit()
    // answers AlreadyHeld for the second and nothing is written twice, which is
    // what I34 is for.
    std::vector<VideoInputPpcp *> previewConsumers() const;

    // MSG 8.4a — flush what is owed to every connected owner, now.  The tick
    // does this anyway; this exists so a clip that has just been flushed to disk
    // is acknowledged immediately rather than up to a tick later, because
    // `capture_committed` is what lets the device evict under I38 and a phone
    // used all season depends on it.
    void flushOwedCommitsNow();

    // ── The one ledger, borrowed rather than copied ────────────────────────
    //
    // ⛔ THERE MUST BE EXACTLY ONE IN-MEMORY LEDGER OVER THE FILE.  This class
    // loads it at construction and flushes owed commits from it on every tick;
    // `PpcpImportController` used to build a SECOND one, function-local, over
    // the very same JSON.  Neither reloaded before saving, so an import during a
    // live session wrote its records and the next flush from here overwrote the
    // file wholesale — the import's receipts gone, and the same bundle
    // duplicated on re-import.  Harmless only while nothing live was recorded in
    // it; a data-loss bug the moment live clips land there.
    //
    // So the file-import path borrows this one.  ⚠ `PpcpHostService` is a
    // function-local static in main(), constructed AFTER the import controller
    // and therefore destroyed BEFORE it — main.cpp drops the borrowed reference
    // in the `aboutToQuit` handler that already exists for this exact hazard.
    Ppcp::PpcpImportLedger &ledger() { return m_importLedger; }

    // ── CORE 7.3a / MSG 5.2 — arming, from the host ────────────────────────
    //
    // ⚠ NOT PART OF THE MVP, AND DELIBERATELY SO.  A capture device arms itself
    // in the shipping product; this is a capability beside that, not a
    // replacement for it.
    //
    // Sent to EVERY connected phone with an empty stream list, which MSG 5.2
    // makes "every open capture Stream": this application's `armed` is a
    // property of the whole capture path, not of one camera.  Returns false if
    // any phone refused, having still asked the others — a half-armed bay is a
    // state somebody has to see, not one to hide by failing early.
    Q_INVOKABLE bool armAll();
    Q_INVOKABLE bool disarmAll();

    // ── MSG §12 / CR-02 — the torch, and anything else a phone declares ─────
    //
    // Modelled on armAll() above, and carrying the same discipline for the same
    // reason: a `true` here means the command reached this peer's QUEUE.  It is
    // NOT the torch coming on.  The control that reads it back — the Cameras
    // pill's popup — binds to `actuators` in phones()/phoneHealth(), whose
    // `state` is written by `actuator_command_ack` and `actuator_state` only.
    //
    // `pairingId` names the phone the way every other per-phone invokable here
    // does; `actuatorId` is the wire id the phone declared, which is why the
    // caller reads it out of that same `actuators` list rather than composing
    // one.  False where the phone is not connected, declares no such Actuator,
    // or the library refused the command (12.1d, I39, 12a).
    Q_INVOKABLE bool setPhoneActuator(const QString &pairingId, const QString &actuatorId,
                                      bool on);
    // ⚠ TEST SEAM, for the reason declareForTest() above is one: this suite
    // links `ppcp_host_service_stubs.cpp` and never accepts a link, so there is
    // no connected phone to command.  Drives the same per-phone call
    // setPhoneActuator() drives, by index.
    bool setActuatorForTest(std::size_t index, const QString &actuatorId, bool on);

    // The aggregate a screen reads: the LEAST ready of the connected phones, so
    // one phone that is blocked or still coming up cannot be hidden behind
    // another that is ready.  Empty string with nothing connected.
    //
    // ⚠ IT IS NOT `ppcp_peer_is_armed()`.  That answers "arm was queued" — see
    // `PpcpLiveSession::isArmed()` — and showing it to a person would be a green
    // light with no evidence behind it.
    Q_PROPERTY(QString armState READ armState NOTIFY armStateChanged)
    QString armState() const;

    // The id of the Source this host declared for its own microphone, or empty
    // where it declared none (a machine with no audio input — MSG 3.3d makes
    // that a normal declaration, not a broken one).  What `ShotController` must
    // name when it nominates an acoustic Candidate: I26 refuses any Source this
    // host does not own, and 8.1e forbids inventing one.
    QString hostMicrophoneSourceId() const;

signals:
    // The set of preview consumers changed -- a phone connected, reconnected or
    // went away.  Whoever listens for clips must (re)connect to them; see
    // previewConsumers().
    void previewConsumersChanged();

    // One Stream was asked for a clip around `shotId`.  Emitted per Stream, so a
    // consumer can record what is owed before any of it arrives.
    void captureAsked(const QString &shotId, const QString &peerId,
                      const QString &sourceId, const QString &streamId,
                      const QString &alias);

    void stateChanged();
    void studioNameChanged();
    void statusChanged();
    void codeChanged();
    void phonesChanged();
    // ⛔ SEPARATE FROM phonesChanged() ON PURPOSE, and the separation is the
    // whole point.  A `heartbeat_ack` moves a READING; it does not change which
    // phones exist.  Emitting the structural signal for it made Settings ->
    // Phones rebuild every delegate once per heartbeat, which destroyed the
    // alias field an operator was typing into — measured 30 Aug 2026, and it
    // made the field unusable for as long as any phone was linked.
    // ⚠ `relation_update` IS THE SAME KIND OF EVENT and took a year less to
    // find: `onRelations()` kept emitting the structural signal after the
    // heartbeat stopped, so the field was still unusable while linked (31 Aug
    // 2026).  Anything that only moves battery, thermal, transport or sigma
    // belongs on this signal; only a phone appearing, leaving, being renamed or
    // being forgotten is phonesChanged().
    void phoneHealthChanged();
    void guidedChanged();
    void failureChanged();

    // MSG 3.3 — a counterpart declared, so its cameras exist NOW and at no
    // other moment.  `main.cpp` connects this to `CameraManager::enumerate()`:
    // the registry has already been told (registerPpcpPeer below), and the
    // home screen's DEVICES list reads the registry directly, but CameraManager
    // snapshots at construction and needs asking.
    void sourcesChanged();

    // The set of live arbiters moved — a phone's arbiter started, or the phone
    // holding the one we handed out went away.  `main.cpp` re-reads
    // `activeShotBridge()` and `hostMicrophoneSourceId()` on this and hands
    // both to `ShotController`.
    //
    // ⚠ THE JOIN IS A SIGNAL AND NOT A STORED std::function ON PURPOSE.
    // `ShotController` is a stack object in main(); this service is a
    // function-local static and outlives it.  A callback capturing
    // `&shotController` would dangle between main() returning and exit() — the
    // exact shape of the 23 Aug crash the destructor note above describes.  A
    // QObject connection is severed when either end dies.
    void shotBridgeChanged();

    // CORE 8.2h — the arbiter issued a Shot.  `t0HostNs` is on `tb:host`, which
    // is this Session's `timebase_ref`, and is never revised (I7).  `shotId` is
    // carried for the log only: it is the wire's identity for an event the
    // swing library knows by an ordinal.
    void arbitratedShot(qint64 t0HostNs, const QString &shotId);

    // 5.2a — a phone answered `arm`, or this host asked.  Drives the aggregate
    // above and the per-phone row in Settings -> Phones.
    void armStateChanged();

private:
    // ── One of these per connected phone ────────────────────────────────────
    //
    // ⚠ WHY A PEER PER PHONE AND NOT ONE PEER WITH SEVERAL LINKS.  Finding
    // F-H8-5 settled it: a `ppcp_peer` IS THE CONVERSATION, not the
    // application.  One engine shared across links kept the previous device's
    // open Session, its declaration and its `msg_id` sequence, and every device
    // after the first was refused `ppcp_peer_session_open: invalid argument`.
    // `PpcpHostPeer` holds exactly that per-conversation state — the attached
    // link, the engine, `m_declaredOnLink`, the reassembly tails, the live
    // Session — so one phone is one of these, whole.
    //
    // Nothing below this class needed changing for it: the transport already
    // assembles concurrent links (ENC 2.1's `link_id` exists for that, and
    // `ppcp_link_bind_test` pins it), and `VideoInputPpcp` has always been
    // keyed by peer id.  The single-phone assumption was only ever here.
    struct Phone {
        // Declared in the order they must DIE in — reverse declaration order —
        // because `peer` holds raw pointers to the other two.
        std::unique_ptr<Ppcp::PpcpHostPeer>   peer;
        std::unique_ptr<Ppcp::PpcpEngine>     engine;
        std::unique_ptr<Ppcp::PeerConnection> link;

        // The pairing this phone arrived on.  Stable, local, and the join
        // between a live link and the row in Settings -> Phones.
        QString pairingId;

        // Which way this link came in.  ⚠ It is decided at adoption and never
        // changes: design §7.3 forbids migrating a link between transports,
        // because a clock offset fitted on one is WRONG on the other and the
        // sigma would not widen to admit it.  A transport change is a NEW link.
        bool wired = false;
        // Its declared `Peer.id`, which is what `VideoInputPpcp` keys cameras
        // and timebase relations by.
        QString counterpartId;

        // CORE 5.19c / erratum E66 — the Actuators this phone declared, as a
        // top-level sibling of `sources` in `declare` and NOT nested in the
        // `peer` head.  Copied out of the borrowed `ppcp_peer_desc` at
        // `onDeclare()` because that struct's strings live in the decode arena
        // and are gone by the time a panel asks.
        //
        // ⚠ EMPTY IS A LEGAL DECLARATION (5.19c), on exactly the terms an empty
        // `sources` is, and a peer owning none omits the key entirely.  So a
        // phone with no torch is not a phone that failed to declare one, and
        // the control is simply absent rather than shown disabled.
        struct DeclaredActuator {
            QString id;
            QString kind;      // open registry: torch, indicator_led, …
            QString control;   // open registry: on_off, level
            QString label;     // informational, may be empty
        };
        QVector<DeclaredActuator> actuators;
        // What it called itself in MSG 3.3.  Display text from an untrusted
        // counterpart (4.4d): it names a row and is never an identifier.
        QString name;

        // MSG 9.1 — where a Session this phone REPLAYS onto the live link
        // lands.  Declared after `peer` so it dies first: it holds the peer
        // pointer.  Null until the engine exists.
        std::unique_ptr<Ppcp::PpcpImportSink> importSink;

        // ⛔ **THE PREVIEW CONSUMERS, ONE PER CAMERA SOURCE, ALIVE FROM
        // `declare`.**  Owned here and QObject-parented to the service.
        //
        // ⚠ Without these nothing was listening.  We request preview at
        // `declare` (5.11.2 calls setup and framing its main use), but
        // `VideoInputPpcp::dispatchEvent()` broadcasts only to LIVE instances
        // and the sole thing that ever constructed one was the Settings crop
        // editor.  So a phone announced pictures at 10 fps into a host with
        // nowhere to put them — and its `stream_close` was dropped too, so we
        // could not even tell that preview had stopped.  Diagnosed on hardware
        // 27 Aug 2026 after an operator saw no preview at all.
        //
        // ⚠ Raw pointers, deleted explicitly in dropPhone() BEFORE the peer
        // they point at goes.  Deliberately not unique_ptr: `Phone` would then
        // need a complete `VideoInputPpcp` in this header, which is a Video
        // dependency the Ppcp layer does not otherwise carry.
        std::vector<VideoInputPpcp *> previews;

        // ⛔ **ONE PER CHANNEL, AND THEY MUST DIE BEFORE `link` DOES.**  A
        // QSocketNotifier outlives its fd badly — the same trap
        // `startAdvertising()` records for the DNS-SD socket — so dropPhone()
        // clears this vector explicitly before the link closes, and it is
        // declared AFTER `link` so even an accidental default destruction runs
        // in the right order (members die in reverse declaration order).
        //
        // ⚠ WHY THEY EXIST.  pump() reads bytes; tick() runs the schedules that
        // need a clock.  Until 29 Aug 2026 BOTH ran only from the 20 ms QTimer,
        // so a reply sat in the socket for up to a tick before anything read
        // it — and `libppcp` stamps `t4` inside ppcp_peer_feed()
        // (`ppcp_peer.c:2675`), which pump() is what calls.  The measured cost
        // was a `min_rtt` of ~18 ms on a link whose real round trip is a few:
        // the poll, not the network.  These notifiers make the comment in
        // start() true.
        std::vector<std::unique_ptr<QSocketNotifier>> reads;
        // Re-entrancy guard: pump() -> drainEvents() -> a preview consumer can
        // spin the event loop, and a nested pump on the same peer would feed
        // the engine from a buffer the outer call still holds.
        bool pumping = false;
    };


private slots:
    void onTick();
    // RV-6 (H10).  Private: nothing outside drives an attempt.
    void pumpGuided();

private:
    // ── Phase 1 contract C2 — the pairing may have to be TOLD to us ────────
    //
    // Empty = "read it off the link", which is every WiFi caller: the LISTENER
    // resolved the identity, so `link->pairingId()` has the answer.
    //
    // ⛔ NON-EMPTY MEANS THIS HOST DIALLED, and on that side `link->pairingId()`
    // has nothing to say — which is the whole reason this parameter exists.  A
    // dialling caller resolved the pairing under RV 5.3b BEFORE it dialled
    // (design §5.2) and hands the answer in.  Everything the rendezvous ledger
    // drives is keyed on the pairing id — `phoneByPairing()` (so the
    // Settings→Phones row's status), `notePeerName()`, `m_pairedThisRun` — so a
    // link adopted without one would be live, carry video, and be invisible to
    // all of them.  Same class as the empty-Phones-list bug of 26 Aug.
    //
    // ⚠ AND IT IS ALSO THE ONLY SIGNAL "WE DIALLED" HAS, deliberately: the
    // contract fixes this signature, and "a caller passed a resolved pairing"
    // and "a caller dialled" are the same statement today.  It decides three
    // things — whether RV 7.3a's single-use accounting runs, whether the engine
    // is built `listener = false`, and whether this host sends `hello` — so if a
    // future non-dialling caller ever passes a resolved pairing, all three go
    // wrong together and this comment is where to start.
    void adoptLink(std::unique_ptr<Ppcp::PeerConnection> link,
                   const QString &resolvedPairingId = {});
    // Closes ONE phone's link and forgets it.  `why` is for the status line.
    void dropPhone(Phone *ph, const char *why);
    void dropAllPhones(const char *why);
    // Everything a freshly constructed peer needs before it is attached: the
    // hooks, the health sources and this host's own declaration.  One place, so
    // the second phone cannot quietly get a different setup from the first.
    // `listener` is ENC 2.1a's "which end dialled", defaulting to true so every
    // WiFi caller is unchanged; the wired path passes false (contract C6).  It
    // is a parameter of this function because this is where the peer is built,
    // and one place is what stops the second phone quietly getting a different
    // setup from the first.
    bool configurePhonePeer(Phone *ph, std::string *err, bool listener = true);
    void onDeclare(Phone *ph, const ppcp_peer_desc *desc);
    void onRelations(Phone *ph);
    // Is any connected phone paired on this pairing id?
    Phone *phoneByPairing(const QString &pairingId);
    const Phone *phoneByPairing(const QString &pairingId) const;

    // CR-02 — one entry per DECLARED Actuator, merged with what the ack and
    // `actuator_state` have said about it.  Published on BOTH phones() and
    // phoneHealth() from this one function, for the reason `charging` and
    // `storageFreeBytes` are: two hand-written copies of the same reading drift,
    // and the panel that refreshes a torch must not have to re-read the whole
    // phone list to do it (trap 1).
    QVariantList actuatorRowsFor(const Phone *ph) const;
    // The one place a command is actually issued; both the Q_INVOKABLE and the
    // test seam funnel through it so neither can drift from the other.
    bool commandActuator(Phone *ph, const QString &actuatorId, bool on);
    void setStatus(const QString &s);
    // Records a failure and builds its user-facing sentence.  One place, so the
    // rule above about what may be named lives in one place too.
    void noteFailure(const Ppcp::HandshakeFailure &f);
    // The same record, for a refusal this class makes itself rather than one
    // the transport reported.
    void noteFailureText(const QString &text);
    void clearFailure();
    static QString describeFailure(const Ppcp::HandshakeFailure &f);
    void refreshCode();
    // The one implementation behind both the button and the automatic renewal
    // at expiry.  `userAsked` is what separates them, and it decides exactly one
    // thing: whether the last failure is cleared.  A person pressing "New code"
    // is starting again and wants a clean panel; the clock coming round is not
    // an answer to "why did my phone not connect", and wiping the message on
    // that tick would delete the only report of a refusal 30 seconds before it.
    bool publishCode(bool userAsked);
    // Stops displaying the current code.  `invalidateSession` is false only
    // when the code being dropped is the one a LIVE link paired on — see the
    // definition; closing that session would wipe the K_tls the link still
    // needs.
    void dropDisplayedCode(bool invalidateSession);
    bool displayedCodePairedAPhone() const;
    // Remembers the phone's own name against the pairing it arrived on.  See
    // the definition for why this is not in the keychain.
    void notePeerName(const Phone *ph);
    void startDiscovery();
    void stopDiscovery();
    // RV 3.5e / CA5 — the advertisement half.  `refreshAdvertisement()` is
    // called wherever the set of persisted pairings can have changed, which is
    // the only input it has.
    void startAdvertising();
    void stopAdvertising();
    void refreshAdvertisement();
    static QString phoneNameFor(const QString &pairingId);
    // The user-editable half of a phone's label, in `ppcp/phoneAlias` — a flat
    // pairingId -> alias map, same shape as `phoneNameFor`'s but never written
    // from a `declare`.  Empty when the user has not set one.
    static QString phoneAliasFor(const QString &pairingId);

    Ppcp::PpcpRendezvous                   m_rv;
    Ppcp::Listener                         m_listener;
    std::vector<std::unique_ptr<Phone>>    m_phones;
    PpcpOfferController                   *m_offers = nullptr;

    // ── MSG §9.1 — what this host has taken in, and what it owes for it ─────
    //
    // ONE ledger for every phone, because I34's identity is
    // (minting peer, session, capture) and is deliberately NOT scoped by which
    // link the bytes arrived on: the same Session offered by two phones, or by
    // one phone twice, must be recognised as already held.  The per-phone
    // `importSink` above is the walker; this is the memory.
    //
    // ⚠ AND UNTIL 27 AUG THIS WAS NOT HERE AT ALL, WHICH COST THE DEVICE ITS
    // STORAGE.  Accepting a `session_offer` made a phone replay its whole
    // bundle onto the link, and PinPointStudio wrote nothing, recorded nothing,
    // and returned no `capture_committed` — so under 5.14h no Capture could
    // ever reach `confirmed` and the phone could never evict anything it had
    // sent us.  `have_digests` was likewise always empty, so it re-sent bytes
    // we already had.  The whole machinery existed and had no caller.
    QVariantMap                            m_clipChain;
    Ppcp::PpcpImportLedger                 m_importLedger;
    // Last aggregate arm state seen by the tick, so a change driven by TIME —
    // the stall deadline — is noticed as well as one driven by a message.
    QString                                m_lastArmState;

    // 5.14h — every `capture_committed` this host owes, sent to whichever phone
    // is the OWNER, once it is here to receive it.  Called from the tick.
    void flushOwedCommits(Phone *ph);

    // ── ENC 2.1d — the third channel, and the thread it has to cross ────────
    //
    // ⚠ NOTHING IN THIS APPLICATION USED TO ACCEPT ONE, THOUGH A COMMENT IN
    // start() SAID IT DID.  `Listener::acceptInto()` had no caller outside the
    // conformance harness, so a phone opening a `preview` channel after the
    // session was established bound it, waited out `bindTimeoutMs`, and had it
    // torn down — the link_id had already been erased from the listener's table
    // when its first two channels completed, so the third arrived as a NEW
    // half-built link with no control channel and expired as one.  Silent at
    // both ends, and exactly the shape ENC 2.1d calls "the expected case".
    //
    // The accept thread owns nothing but the accept call and a live link
    // belongs to the GUI thread, so the channel is collected on one and adopted
    // on the other.  This is the list of link ids still short of a third
    // channel, written by the GUI thread and read by the accept thread.
    std::mutex                          m_wantChannelMutex;
    std::vector<Ppcp::LinkId>           m_wantChannel;
    void noteWantsChannel(const Ppcp::LinkId &id, bool wants);
    // Runs on the GUI thread: the accept thread found a stream binding `id`.
    void adoptChannel(const Ppcp::LinkId &id, Ppcp::TransportChannel *raw);

    // Arms one read notifier per open channel of `ph`'s link, so pump() runs
    // when bytes arrive rather than up to a tick later.  Idempotent: it rebuilds
    // the set, which is what a channel arriving under ENC 2.1d needs.
    void watchChannels(Phone *ph);

    QTimer  m_timer;
    bool    m_listening = false;
    quint16 m_port = 0;
    QString m_status;
    QString m_lastFailureText;
    int     m_failureCount = 0;
    // How many consecutive refusals before the resolver state is dumped, and
    // how often to repeat it thereafter.  See noteFailureText().
    static constexpr int kFailureDiagAfter = 3;
    static constexpr int kFailureDiagEvery = 20;
    // ⚠ WHICH PAIRINGS A PHONE ACTUALLY ARRIVED ON, and it cannot be inferred
    // from the ledger.  `closeSession()` sets `invalidated` and zeroes
    // `usesRemaining` — so a code the user simply dismissed is indistinguishable
    // from one a phone spent, and both look "used".  Listing on that basis put a
    // device row up for a QR nobody ever scanned.  A device row means "this host
    // has met this phone", and this is the only place that fact exists.
    QSet<QString> m_pairedThisRun;

    // ── RV §3 discovery, the browser half ───────────────────────────────────
    //
    // ⚠ CONVENIENCE, NOT PLUMBING.  RV §3 is "optional.  Reconnection
    // convenience only — a first pairing always uses §4", and 3.6a MUST NOT
    // treat discovery failure as an error state.  So none of this has an error
    // path: `makePlatformBrowser()` returns null off macOS, the browse can die
    // and the only consequence is that a phone stops being marked as here.
    //
    // ⚠ AND IT CAN ONLY EVER SEE PHONES WE ARE ALREADY PAIRED WITH.  3.4c
    // forbids connecting to an instance whose `rid` cannot be resolved, and
    // `decideDial()` has no branch that dials anyway; the resolver is our own
    // pairing ledger.  A stranger's phone advertising on the same network
    // resolves to nothing and never becomes a row.
    // ── The wired path (design §6; Phase 1) ────────────────────────────────
    //
    // ⚠ DECLARED AFTER `m_rv` ON PURPOSE.  It holds the resolver
    // `m_rv.identityResolver()` returns, and that closure captures the
    // rendezvous's `Impl *`.  Members die in reverse declaration order, so this
    // one — and the worker thread `stop()` joins in its destructor — go before
    // the object the closure points at.
    //
    // ⚠ ON BY DEFAULT; `PINPOINT_PPCP_WIRED=0` forces it off.  See
    // `PpcpWiredLink::enabled()` for what made defaulting on safe.
    std::unique_ptr<Ppcp::PpcpWiredLink> m_wired;
    void startWired();
    void stopWired();

    std::unique_ptr<Ppcp::RvBrowser>  m_browser;
    std::unique_ptr<QSocketNotifier>  m_browseWatch;
    // `LostFn` hands back only the instance name, so the map from that to the
    // pairing it resolved to has to be kept here.  The `rid` behind the name
    // rotates at least every 15 minutes, so an instance that goes and comes
    // back is a different name for the same phone — which is why the VALUE is
    // the stable local pairingId and the key is not.
    QHash<QString, QString> m_seenInstances;

    // ── RV §3 discovery, the ADVERTISEMENT half (3.5e, CA5) ─────────────────
    //
    // ⚠ WITHOUT THIS, §7.4's PERSISTENCE BUYS NOTHING.  Our counterpart is
    // barred from advertising by 3.5d — `Network.framework`'s listener has no
    // server-side PSK resolver, so a phone that advertised would be
    // discoverable and unable to complete the handshake it advertised for — and
    // 3.5e then makes the peer that CAN advertise do so.  If neither did, both
    // ends would hold valid key material with no path by which either finds the
    // other, and the user would see a protocol that remembers them and still
    // asks for a code every session.
    //
    // ⚠ IT ADVERTISES ONLY PERSISTED PAIRINGS, AND ONLY ONE AT A TIME (3.4d1).
    // One instance is what keeps the COUNT of held pairings unobservable, which
    // is the property 3.4e is about; the driver rotates which pairing is named.
    //
    // ⚠ AND IT IS AS SILENT AS THE BROWSER (3.6a).  `makePlatformAdvertiser()`
    // returns null off macOS, the responder can refuse, and the registration
    // can be renamed under us.  None of those reaches `status`.
    std::unique_ptr<Ppcp::RvAdvertiser>                 m_advertiser;
    std::unique_ptr<Ppcp::RvReconnectionAdvertisement>  m_advert;
    std::unique_ptr<QSocketNotifier>                    m_advertWatch;

    // ── RV-6 (H10) ─────────────────────────────────────────────────────────
    // Owns no socket of its own: `m_guided` is driven from the tick that is
    // already running, and its stream is the one it opens for the single
    // attempt 11.3d1 allows.
    Ppcp::GuidedPairing m_guided;
    QString             m_guidedLastPhase;
    QString             m_guidedLastDigits;
    // 11.5g — where a completed guided pairing lands.  It becomes an ORDINARY
    // pairing (11.1a): "from here the pairing is INDISTINGUISHABLE from one
    // established by a scanned code, so §5, §7.4 and §7.5 apply verbatim".
    QString             m_guidedPairingId;

    // The accept loop.  Its ONLY job is to block in accept() and hand the link
    // to the GUI thread; it touches no PPCP state of its own.
    std::thread       m_acceptThread;
    std::atomic<bool> m_stopping{false};

    // 0 = use kCodeLifetimeS.  See setCodeLifetimeSecondsForTest().
    int                 m_codeLifetimeS = 0;
    Ppcp::PublishedCode m_code;
    bool                m_codeLive = false;
    // The last whole second `onTick()` reported, so the 20 ms tick emits
    // `codeChanged` once a second rather than fifty times.  -1 means "no code
    // has been counted yet"; 0 is a real reading and cannot stand in for it.
    int                 m_lastSecondsLeft = -1;

    // 6.3 convergence trace, opt-in (PINPOINT_SYNC_TRACE=1).  Off by default:
    // it prints once a second per related timebase, which is noise in an
    // ordinary session and the only way to see a slow convergence in a real one.
    const bool          m_syncTrace = !qEnvironmentVariableIsEmpty("PINPOINT_SYNC_TRACE");
    std::int64_t        m_lastSyncTraceMs = 0;
    const std::int64_t  m_syncTraceStartMs = QDateTime::currentMSecsSinceEpoch();
    Ppcp::QrCode        m_qr;
    QVariantList        m_qrRows;
    QStringList         m_codeEndpoints;
};
