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
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(QString peerName READ peerName NOTIFY stateChanged)

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
    bool    connected() const { return m_link != nullptr; }
    QString peerName() const { return m_peerName; }

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

    // RV §4 — publish a code.  Fresh psk and sid per code (7.3d), every
    // reachable address in `ep` (4.3d), `mu: 1` (7.3a) and a short `exp`
    // (7.3c).  Displaces whatever code was showing, which 7.3b then invalidates.
    Q_INVOKABLE bool publishPairingCode();
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

    Ppcp::PpcpHostPeer &hostPeer() { return m_peer; }
    Ppcp::PpcpRendezvous &rendezvous() { return m_rv; }

signals:
    void stateChanged();
    void statusChanged();
    void codeChanged();
    void phonesChanged();

    // MSG 3.3 — a counterpart declared, so its cameras exist NOW and at no
    // other moment.  `main.cpp` connects this to `CameraManager::enumerate()`:
    // the registry has already been told (registerPpcpPeer below), and the
    // home screen's DEVICES list reads the registry directly, but CameraManager
    // snapshots at construction and needs asking.
    void sourcesChanged();

private slots:
    void onTick();

private:
    void adoptLink(std::unique_ptr<Ppcp::PeerConnection> link);
    void dropLink(const char *why);
    void onDeclare(const ppcp_peer_desc *desc);
    void onRelations();
    void setStatus(const QString &s);
    void refreshCode();
    // Remembers the phone's own name against the pairing it arrived on.  See
    // the definition for why this is not in the keychain.
    void notePeerName();
    void startDiscovery();
    void stopDiscovery();
    static QString phoneNameFor(const QString &pairingId);

    Ppcp::PpcpRendezvous                   m_rv;
    Ppcp::Listener                         m_listener;
    Ppcp::PpcpHostPeer                     m_peer;
    std::unique_ptr<Ppcp::PpcpEngine>      m_engine;
    std::unique_ptr<Ppcp::PeerConnection>  m_link;
    PpcpOfferController                   *m_offers = nullptr;

    QTimer  m_timer;
    bool    m_listening = false;
    quint16 m_port = 0;
    QString m_status;
    QString m_peerName;
    QString m_counterpartId;
    // The pairing the LIVE link resolved to.  It used to be read in adoptLink()
    // and dropped on the floor immediately after noteLinkEstablished(), which
    // left no way to say "this connected phone is that remembered pairing" —
    // and therefore no way to give a remembered pairing the name its phone sent.
    QString m_linkPairingId;
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
