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
#include <string>
#include <vector>
#include <thread>

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QSocketNotifier>
#include <QTimer>
#include <QVariantList>

#include "ppcp_discovery.h"
#include "ppcp_engine.h"
#include "ppcp_host_peer.h"
#include "ppcp_qr.h"
#include "ppcp_rendezvous.h"
#include "ppcp_transport.h"

class PpcpOfferController;

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
    QString lastFailureText() const { return m_lastFailureText; }
    int     failureCount() const { return m_failureCount; }

    QVariantList qrRows() const { return m_qrRows; }
    int          qrSize() const { return m_qr.size(); }
    bool         codeLive() const { return m_codeLive; }
    int          codeSecondsLeft() const;
    QStringList  codeEndpoints() const { return m_codeEndpoints; }
    QVariantList outstandingCodes() const;
    QVariantList phones() const;

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

    // RV §4 — publish a code.  Fresh psk and sid per code (7.3d), every
    // reachable address in `ep` (4.3d), `mu: 1` (7.3a) and a short `exp`
    // (7.3c).  Displaces whatever code was showing, which 7.3b then invalidates.
    Q_INVOKABLE bool publishPairingCode() { return publishCode(/*userAsked=*/true); }
    // 7.3b — invalidate the displayed code, used or not.
    Q_INVOKABLE void closePairingCode();
    // 7.4b — persistence is opt-in, visible and individually revocable, so all
    // three are user actions and none of them happens on its own.
    Q_INVOKABLE bool rememberPairing(const QString &pairingId);
    Q_INVOKABLE void forgetPairing(const QString &pairingId);

    // RT-9 — everything this subsystem contributes to a diagnostic bundle, and
    // it is constructed from a struct that holds no secret rather than filtered
    // afterwards.  Q_INVOKABLE so the diagnostics screen can offer it.
    Q_INVOKABLE QString diagnosticExport() const;

    // What this build's service discovery is, in the words RvBrowser::describe()
    // uses, or why there is none.  Part of the diagnostic export because 3.6a
    // gives discovery no error channel at all: when a remembered phone does not
    // reappear by itself, whether this host is even browsing is the first thing
    // anybody needs to know, and it is otherwise unanswerable.
    QString discoveryDescription() const;

    // The first connected phone's peer, for callers that predate several.
    // Null when nothing is connected.
    Ppcp::PpcpHostPeer *hostPeer();
    Ppcp::PpcpRendezvous &rendezvous() { return m_rv; }

signals:
    void stateChanged();
    void statusChanged();
    void codeChanged();
    void phonesChanged();
    void failureChanged();

    // MSG 3.3 — a counterpart declared, so its cameras exist NOW and at no
    // other moment.  `main.cpp` connects this to `CameraManager::enumerate()`:
    // the registry has already been told (registerPpcpPeer below), and the
    // home screen's DEVICES list reads the registry directly, but CameraManager
    // snapshots at construction and needs asking.
    void sourcesChanged();

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
        // Its declared `Peer.id`, which is what `VideoInputPpcp` keys cameras
        // and timebase relations by.
        QString counterpartId;
        // What it called itself in MSG 3.3.  Display text from an untrusted
        // counterpart (4.4d): it names a row and is never an identifier.
        QString name;
    };


private slots:
    void onTick();

private:
    void adoptLink(std::unique_ptr<Ppcp::PeerConnection> link);
    // Closes ONE phone's link and forgets it.  `why` is for the status line.
    void dropPhone(Phone *ph, const char *why);
    void dropAllPhones(const char *why);
    // Everything a freshly constructed peer needs before it is attached: the
    // hooks, the health sources and this host's own declaration.  One place, so
    // the second phone cannot quietly get a different setup from the first.
    bool configurePhonePeer(Phone *ph, std::string *err);
    void onDeclare(Phone *ph, const ppcp_peer_desc *desc);
    void onRelations(Phone *ph);
    // Is any connected phone paired on this pairing id?
    Phone *phoneByPairing(const QString &pairingId);
    const Phone *phoneByPairing(const QString &pairingId) const;
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

    Ppcp::PpcpRendezvous                   m_rv;
    Ppcp::Listener                         m_listener;
    std::vector<std::unique_ptr<Phone>>    m_phones;
    PpcpOfferController                   *m_offers = nullptr;

    QTimer  m_timer;
    bool    m_listening = false;
    quint16 m_port = 0;
    QString m_status;
    QString m_lastFailureText;
    int     m_failureCount = 0;
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
    Ppcp::QrCode        m_qr;
    QVariantList        m_qrRows;
    QStringList         m_codeEndpoints;
};
