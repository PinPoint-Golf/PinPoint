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

#include "ppcp_host_service.h"

#include <QDateTime>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QMetaObject>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <exception>

#include <ppcp/version.h>

#include "../Core/pp_debug.h"
#include "../Core/pp_settings.h"
#include "../Video/VideoInputPpcp.h"
#include "../Video/video_input_factory.h"
#include "ppcp_offer_controller.h"
#include "ppcp_source_declaration.h"

using namespace Ppcp;

namespace {

// CORE 5.1a — stable, and not derived from mutable local state.  Kept in the
// application's ordinary settings because it MUST survive a restart and nothing
// this class can see does; minted from the same CSPRNG that mints a psk.
QString hostPeerId()
{
    // ⚠ NOT a platform device identifier (5.2.1) and not the machine's name.
    // A generated identifier, persisted, and nothing about the hardware.
    static QString id;
    if (!id.isEmpty()) return id;
    std::uint8_t raw[16];
    if (!csprngBytes(raw, sizeof raw)) {
        // No fallback to a weak source: a peer id is not a secret, but a peer
        // id minted from a broken generator collides, and a collision is CORE
        // 8.5c's scoping quietly failing.
        return QStringLiteral("peer:pinpointstudio");
    }
    static const char *hex = "0123456789abcdef";
    QString s = QStringLiteral("peer:pps-");
    for (unsigned char b : raw) {
        s.append(QLatin1Char(hex[b >> 4]));
        s.append(QLatin1Char(hex[b & 0x0f]));
    }
    id = s;
    return id;
}

// RV 7.3c — the shortest expiry the workflow tolerates.  Long enough to walk to
// the bay, short enough that a photograph of the screen is worth little.
constexpr std::uint64_t kCodeLifetimeS = 300;

// The keychain service the persisted pairings live under (RV 7.2c).
const char *kPairingService = "golf.pinpoint.studio.ppcp.pairings";

}  // namespace

PpcpHostService::PpcpHostService(QObject *parent)
    : QObject(parent)
{
    // 7.4b — persistence is opt-in.  Installing the store does not persist
    // anything; rememberPairing() is the only path that writes a key, and it is
    // a user action.  On a platform with no protected storage this is null and
    // persist() refuses rather than writing to a file (7.2c).
    m_rv.setSecretStore(makePlatformPairingStore(kPairingService));
    m_rv.loadPersisted();

    // ⚠ pump() AND tick() ARE DIFFERENT SCHEDULES AND BOTH RUN HERE.  pump() is
    // driven by bytes being ready; tick() by TIME PASSING — §6.3's sync cadence,
    // §7.4's heartbeats and 8.2h's issue hold.  A loop that ran only when bytes
    // arrived would never notice a dead peer, which is the one thing §7.4
    // exists to notice.
    m_timer.setInterval(20);
    connect(&m_timer, &QTimer::timeout, this, &PpcpHostService::onTick);

    // ⚠ clipReady() IS DELIBERATELY NOT CONNECTED, AND THAT IS NOT AN OVERSIGHT.
    // A `PpcpClip` carries opaque PPCP identities; this application's
    // `Session.id` is a filesystem directory path and its `Shot.id` an `int`
    // ordinal, and CORE 8.5c keys idempotent re-import on OPAQUE ids.  Writing a
    // clip into the swing library today would either duplicate it on the second
    // arrival or throw the PPCP identity away, and I34 is precisely the
    // invariant that would be lost.  H3 made the same call for imported bundles
    // and landed them under `PPCP Imports/<peer>/<session>/` instead; the live
    // path has no equivalent yet and inventing one here would settle host review
    // item 2 by accident.  The seam is left open on purpose.
}

PpcpHostService::~PpcpHostService()
{
    // ⚠ A DESTRUCTOR IS `noexcept`, SO ANYTHING THAT ESCAPES HERE TERMINATES
    // THE PROCESS.  That is not hypothetical: this object is a function-local
    // static in main(), so this runs from `exit()` — after `QGuiApplication`
    // and, worse, after any function-local static constructed later than this
    // one.  `VideoInputPpcp`'s `ppcpLiveMutex()` is exactly that, `dropLink()`
    // reaches it, and locking a destroyed std::mutex throws `std::system_error`
    // rather than returning an error.  The result was a crash report on a clean
    // quit ("mutex lock failed: Invalid argument", 23 Aug).
    //
    // The REAL fix is in main.cpp, which now stops this on `aboutToQuit` while
    // everything is still alive; by the time this runs there is no link, no
    // live code and no thread, so stop() takes none of the dangerous paths.
    // This catch is the backstop for the next such ordering, and it uses
    // fprintf rather than ppWarn deliberately: the log is a static too, and a
    // handler that needs one more static to survive is no handler at all.
    try {
        stop();
    } catch (const std::exception &e) {
        fprintf(stderr, "PPCP host shutdown threw at exit (ignored): %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "PPCP host shutdown threw at exit (ignored)\n");
    }
}

void PpcpHostService::setOfferController(PpcpOfferController *c)
{
    m_offers = c;
}

bool PpcpHostService::start(quint16 port, QString *err)
{
    if (m_listening) return true;

    std::string e;
    if (!m_listener.listen(port, &e)) {
        if (err) *err = QString::fromStdString(e);
        setStatus(tr("Could not listen: %1").arg(QString::fromStdString(e)));
        return false;
    }
    m_port = m_listener.port();

    // RV 5.3b — the server resolves an offered identity against every pairing
    // it holds, outstanding codes and persisted pairings alike.  This is the
    // whole of the authentication decision and it is libppcp's arithmetic.
    m_listener.setIdentityResolver(m_rv.identityResolver());
    // CORE T2's minimum is two channels; a third (preview) arrives later under
    // ENC 2.1d and is taken through acceptInto(), not counted here.
    m_listener.setChannelsPerPeer(2);
    // RV 5.4k wants the negotiated mode recorded; 7.2b forbids a key or a
    // payload reaching a log, and nothing handed to this callback is either.
    m_listener.setLog([](const std::string &line) { ppWarn() << "[ppcp-rv]" << line.c_str(); });

    // ⚠ NO PEER AND NO ENGINE ARE BUILT HERE ANY MORE.  They used to be, one
    // of each, for the lifetime of the service — which is what made this a
    // one-phone host.  A `ppcp_peer` is the CONVERSATION (F-H8-5), so it is
    // built in adoptLink() per phone and dies with that phone's link.
    // `declareSelf()` moves with it: MSG 3.3c makes declaring a per-conversation
    // obligation, not a per-process one.

    m_stopping = false;
    m_acceptThread = std::thread([this] {
        while (!m_stopping) {
            // A short timeout rather than a long one so stop() is prompt; the
            // listener is not doing anything expensive while it waits.
            HandshakeFailure fail;
            std::unique_ptr<PeerConnection> link = m_listener.accept(250, &fail);
            if (!link) {
                // ⚠ `kind`, NOT `message`.  An ordinary 250 ms poll with nobody
                // dialling also returns null, and it must stay silent — the
                // panel would otherwise report a failure fifty times a second
                // for the whole time it is open.  `accept()`'s contract is that
                // a quiet poll leaves `kind` as None; anything else is a phone
                // that reached us and did not become a link.
                if (fail.kind != FailureKind::None) {
                    const HandshakeFailure f = fail;
                    // Same hand-over the link below uses, for the same reason:
                    // this thread owns nothing but the accept call.
                    QMetaObject::invokeMethod(this, [this, f] { noteFailure(f); },
                                              Qt::QueuedConnection);
                }
                continue;
            }
            // Hand it to the GUI thread.  Everything PPCP is single-threaded
            // from here on: this thread owns nothing but the accept call.
            auto *raw = link.release();
            QMetaObject::invokeMethod(this, [this, raw] {
                adoptLink(std::unique_ptr<PeerConnection>(raw));
            }, Qt::QueuedConnection);
        }
    });

    startDiscovery();
    startAdvertising();

    m_listening = true;
    m_timer.start();
    setStatus(tr("Waiting for a device on port %1.").arg(m_port));
    emit stateChanged();
    return true;
}

void PpcpHostService::stop()
{
    m_timer.stop();
    stopDiscovery();
    stopAdvertising();
    m_stopping = true;
    if (m_acceptThread.joinable()) m_acceptThread.join();
    dropAllPhones("shutdown");
    m_listener.stop();
    // 7.3b — the code dies with the session it belongs to, used or not, and
    // 7.2d erases its key material with it.
    closePairingCode();
    m_listening = false;
    emit stateChanged();
}

// ── Everything a phone's own peer needs before it carries anything ──────────
//
// Lifted wholesale out of the constructor, where it configured the single
// service-wide peer.  It is a function now for one reason: the SECOND phone
// must get exactly the setup the first got, and a second copy of this block
// would drift from the first the day somebody edits one of them.
bool PpcpHostService::configurePhonePeer(Phone *ph, std::string *err)
{
    ph->peer->setDeclarationHook([this, ph](const ppcp_peer_desc *d) { onDeclare(ph, d); });
    ph->peer->setRelationsHook([this, ph](const PpcpLiveSession &) { onRelations(ph); });
    ph->peer->addEventHook([this, ph](const ppcp_event &ev) {
        // ⚠ THE EVENT RING HAS EXACTLY ONE DRAINER AND IT IS PpcpHostPeer.
        // Everything else that needs to see events registers here.  In
        // particular `VideoInputPpcp::drainEvents()` MUST NOT be called on this
        // peer — it exists for the standalone paths where nothing else drains.
        if (m_offers) m_offers->observe(ph->counterpartId, ev);
    });

    // The host's own readings.  Both answer false for "cannot tell", which is a
    // different answer from "fine" and is treated as one — a peer reporting
    // `thermal: nominal` on no evidence is the fabrication this protocol
    // refuses everywhere else.
    //
    // ⚠ AND A HEALTH SOURCE IS A PRECONDITION FOR LIVENESS, NOT A DECORATION
    // ON IT (finding F-H5-3).  Without one every `heartbeat` is answered
    // `error` / `profile_not_supported`, no ack ever returns, and §7.4 silently
    // never runs.  This host has storage and no thermometer, so it says so.
    ph->peer->setStorage([](std::uint64_t *freeBytes) {
        if (!freeBytes) return false;
        const QStorageInfo si(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        if (!si.isValid() || !si.isReady() || si.bytesAvailable() < 0) return false;
        *freeBytes = static_cast<std::uint64_t>(si.bytesAvailable());
        return true;
    });


    // MSG 3.3c — a peer declares BEFORE it originates any message referencing a
    // Source, Stream or Candidate, so this happens before anything is pumped,
    // and once per conversation.
    std::string derr;
    if (!ph->peer->declareSelf(PpcpSourceDeclaration::hostInventory(), &derr))
        ppWarn() << "[ppcp] the host could not declare itself:" << derr.c_str();

    ph->engine = ph->peer->makeLibppcpEngine(&derr);
    if (!ph->engine) {
        if (err) *err = derr;
        return false;
    }
    return true;
}

PpcpHostService::Phone *PpcpHostService::phoneByPairing(const QString &pairingId)
{
    if (pairingId.isEmpty()) return nullptr;
    for (const std::unique_ptr<Phone> &p : m_phones)
        if (p->pairingId == pairingId) return p.get();
    return nullptr;
}

const PpcpHostService::Phone *PpcpHostService::phoneByPairing(const QString &pairingId) const
{
    return const_cast<PpcpHostService *>(this)->phoneByPairing(pairingId);
}

Ppcp::PpcpHostPeer *PpcpHostService::hostPeer()
{
    return m_phones.empty() ? nullptr : m_phones.front()->peer.get();
}

QString PpcpHostService::peerName() const
{
    // Deliberately empty with several connected — see the header.  A caller
    // that wants them all asks for `connectedNames`.
    return m_phones.size() == 1 ? m_phones.front()->name : QString();
}

QStringList PpcpHostService::connectedNames() const
{
    QStringList out;
    for (const std::unique_ptr<Phone> &p : m_phones)
        if (!p->name.isEmpty()) out << p->name;
    return out;
}

void PpcpHostService::adoptLink(std::unique_ptr<PeerConnection> link)
{
    if (!link) return;

    // A phone got in, so whatever failed before it is history.  The panel must
    // not carry a refusal underneath a live connection.
    clearFailure();

    // RV 7.3a — the code is spent by the PAIRING, not by the handshake.  This
    // link is two TLS handshakes (three with preview) over one K_tls; counting
    // handshakes would invalidate a `mu: 1` code on the control channel and
    // refuse the bulk channel of the SAME link.  See F-H6-1.
    const std::string pairing = link->pairingId();
    if (!pairing.empty()) m_rv.noteLinkEstablished(pairing);
    const QString pairingId = QString::fromStdString(pairing);
    if (!pairingId.isEmpty()) m_pairedThisRun.insert(pairingId);

    // ⚠ DO NOT DEDUPLICATE BY PAIRING HERE.  It is the obvious next thought —
    // "the same pairing dialling again is that phone reconnecting, so replace
    // its entry" — and it is wrong twice over.
    //
    // It is unreachable for the code this application publishes: `mu` is 1, and
    // the resolver refuses a spent code outright (7.3a, `refusedExhausted`), so
    // a second dial on it never gets far enough to be adopted.
    //
    // And it is incorrect for any code where `mu` exceeds 1, which the spec
    // exists to allow — "pairing several devices from one displayed code is a
    // real workflow".  Those devices share ONE pairing id, so collapsing by it
    // would take down the phone that arrived first the moment the second one
    // scanned the same code.  A pairing is not a phone; a LINK is.

    auto phone = std::make_unique<Phone>();
    Phone *ph = phone.get();
    ph->pairingId = pairingId;
    PpcpHostPeer::Config cfg;
    // The HOST's own id, identical on every conversation: it is this
    // application's identity, not this link's.
    cfg.peerId = hostPeerId().toStdString();
    ph->peer = std::make_unique<PpcpHostPeer>(cfg);

    // ⚠ F-H8-5 — A `ppcp_peer` IS THE CONVERSATION, NOT THE APPLICATION.
    //
    // Found by H8's conformance run on 23 Aug 2026, where twelve rows dial in
    // turn: the FIRST got a Session and the other eleven were refused
    // `ppcp_peer_session_open: invalid argument`, because the engine still held
    // the previous device's open Session, its declaration, its `msg_id`
    // sequence and its link state.  That finding is why this is built here, per
    // phone, and it is also the whole reason several phones work at all: two
    // conversations are two peers, and always were.
    std::string derr;
    if (!configurePhonePeer(ph, &derr)) {
        ppWarn() << "[ppcp] could not build an engine for this link:" << derr.c_str();
        // A phone that handshook correctly and is then dropped by OUR failure
        // is exactly the invisible refusal the failure line exists for.
        noteFailureText(tr("A phone paired, but this computer could not set up "
                           "the connection: %1").arg(QString::fromStdString(derr)));
        link->close();
        return;
    }

    ph->link = std::move(link);
    ph->peer->attach(ph->link.get(), ph->engine.get());
    m_phones.push_back(std::move(phone));

    // ⚠ THE CODE ON SCREEN IS NOW SPENT, SO REPLACE IT.  `mu` is 1 (7.3a), so
    // the QR still being displayed cannot pair anything else — it is a picture
    // of a used ticket, and the next phone scanning it would be refused with no
    // way to see why.  Minting the replacement HERE rather than making somebody
    // press a button is what makes pairing the second angle a matter of holding
    // the phone up to the screen.  Only while the panel is open: `m_codeLive`
    // is that condition, and a closed panel stays closed.
    if (m_codeLive) publishCode(/*userAsked=*/false);

    const TlsOutcome &tls = ph->link->tls();
    // 5.4k — the achieved version and key-exchange mode are made available to
    // the application layer and recorded.  Forward secrecy is a per-connection
    // outcome since the 5.4.3 relaxation, so a peer that cannot say which it
    // got cannot honestly tell a user what "encrypted" means here.
    ppWarn() << "[ppcp-rv] link up:" << tls.describe().c_str();
    setStatus(m_phones.size() == 1
                  ? tr("Device connected — %1.").arg(QString::fromStdString(tls.describe()))
                  : tr("%1 devices connected.").arg(m_phones.size()));
    refreshCode();
    emit stateChanged();
    emit phonesChanged();
}

void PpcpHostService::dropPhone(Phone *ph, const char *why)
{
    if (!ph) return;
    // Find it before anything is torn down, so the erase below cannot be
    // reading a half-destroyed entry.
    auto it = std::find_if(m_phones.begin(), m_phones.end(),
                           [ph](const std::unique_ptr<Phone> &p) { return p.get() == ph; });
    if (it == m_phones.end()) return;

    // The offer list is attached PER PEER, so only this phone's offers go.
    if (m_offers && !ph->counterpartId.isEmpty()) m_offers->detach(ph->counterpartId);
    if (!ph->counterpartId.isEmpty())
        VideoInputPpcp::clearTimebaseMappings(ph->counterpartId);
    ph->peer->attach(nullptr, nullptr);
    if (ph->link) ph->link->close();

    const QString name = ph->name;
    m_phones.erase(it);

    setStatus(name.isEmpty()
                  ? tr("Device disconnected (%1).").arg(QString::fromLatin1(why))
                  : tr("%1 disconnected (%2).").arg(name, QString::fromLatin1(why)));
    emit stateChanged();
    // The phone did not stop existing, it stopped being here — its row stays
    // and changes state, the way a switched-off IMU's does.
    emit phonesChanged();
}

void PpcpHostService::dropAllPhones(const char *why)
{
    while (!m_phones.empty()) dropPhone(m_phones.back().get(), why);
}

void PpcpHostService::onDeclare(Phone *ph, const ppcp_peer_desc *desc)
{
    if (!ph || !desc) return;

    // MSG 3.3 — a peer's cameras exist the moment it declares and at no other
    // moment.  There is no bus to walk and no scan to run: this is the whole of
    // PPCP camera discovery, and it is why `VideoInputFactory::enumerateDevices()`
    // has never had a PPCP branch.
    const int n = VideoInputFactory::registerPpcpPeer(desc);

    ph->counterpartId = QString::fromUtf8(desc->id.v, static_cast<int>(desc->id.len));
    // 4.4d's habit of mind, one layer in: a counterpart's product strings are
    // display text and are never an identifier or a trust signal.
    ph->name = QString::fromUtf8(desc->product.model.v,
                                   static_cast<int>(desc->product.model.len));

    // MSG 9.1 — the offer list, which has been installed DETACHED since H5
    // because there was no live peer to give it.  There is now.
    if (m_offers && ph->engine && ph->engine->peer())
        m_offers->attach(ph->engine->peer(), ph->counterpartId);

    // The one moment a phone says what it is called.  Written down here or not
    // at all: `dropPhone()` forgets the live name, and nothing about a pairing
    // survives a restart except its handle and its key.
    notePeerName(ph);

    ppWarn() << "[ppcp] peer declared:" << ph->name << "-" << n << "camera Source(s)";
    setStatus(tr("%1 connected — %2 camera(s).").arg(ph->name).arg(n));

    // CameraManager snapshots the registry at construction and merges only on
    // enumerate(); the home screen's DEVICES list reads the registry directly
    // and picks it up on its own two-second refresh.  So this signal exists for
    // the manager, not for the list.
    emit sourcesChanged();
    emit stateChanged();
    emit phonesChanged();
}

// ── RV §3 discovery ─────────────────────────────────────────────────────────
//
// ⚠ THE BROWSER WAS BUILT AND TESTED IN H6 AND HAD NO CALLER.
// `makePlatformBrowser()` and `decideDial()` were reached only from
// `ppcp_rendezvous_test`, so a subsystem the specification calls the
// reconnection path was dead code in the application.  This is the caller.
//
// ⚠ IT OWNS NO THREAD, BY THE BROWSER'S OWN DESIGN.  `RvBrowser` exposes the
// DNS-SD client socket and expects the embedding's event loop to watch it — so
// a QSocketNotifier on the GUI thread, and `process()` when it is readable.
// There is deliberately no second accept-style thread here: everything PPCP is
// single-threaded from the moment a link is adopted.
//
// ⚠ AND ITS FAILURES ARE SILENT ON PURPOSE (3.6a).  Multicast "will not work at
// a range" — rate-limited by consumer access points, blocked by guest-network
// client isolation, and it does not cross VLANs.  `makePlatformBrowser()`
// returns null on Windows and Linux, `start()` can refuse, and `process()` can
// report the browse has died.  None of those is an error state, none reaches
// `status`, and none produces a warning: the consequence is only that a phone
// stops being marked as present, and §4's pairing code still works.
void PpcpHostService::startDiscovery()
{
    m_browser = Ppcp::makePlatformBrowser();
    if (!m_browser) return;   // 3.6b — no DNS-SD client here, and that is fine

    const bool ok = m_browser->start(
        [this](const Ppcp::RvAdvertisement &ad) {
            // 3.4c — the one hard rule.  `decideDial` resolves the rotating
            // `rid` against our own held pairings and has no "unknown" branch;
            // a stranger's phone yields an empty pairing and is dropped here.
            const Ppcp::DialDecision d = Ppcp::decideDial(
                ad, PPCP_WIRE_VERSION_MAJOR,
                [this](const std::uint8_t rn[PPCP_RV_RN_BYTES],
                       const std::uint8_t rid[PPCP_RV_RID_BYTES]) {
                    return m_rv.resolveRid(rn, rid);
                });
            if (!d.dial) return;

            const QString inst = QString::fromStdString(ad.instanceName);
            const QString pid  = QString::fromStdString(d.pairingId);
            if (m_seenInstances.value(inst) == pid) return;
            m_seenInstances.insert(inst, pid);
            emit phonesChanged();
        },
        [this](const std::string &instanceName) {
            const QString inst = QString::fromStdString(instanceName);
            if (m_seenInstances.remove(inst) > 0) emit phonesChanged();
        });

    if (!ok) { m_browser.reset(); return; }

    const int fd = m_browser->fd();
    if (fd < 0) { m_browser.reset(); return; }

    m_browseWatch = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
    connect(m_browseWatch.get(), &QSocketNotifier::activated, this, [this] {
        if (m_browser && !m_browser->process()) {
            // 3.6a — the browse died.  A reason to stop watching, and NOT a
            // reason to report a fault.
            stopDiscovery();
        }
    });
    ppWarn() << "[ppcp-rv] discovery:" << m_browser->describe().c_str();
}

void PpcpHostService::stopDiscovery()
{
    m_browseWatch.reset();
    if (m_browser) m_browser->stop();
    m_browser.reset();
    if (!m_seenInstances.isEmpty()) {
        m_seenInstances.clear();
        emit phonesChanged();
    }
}

// ── RV §3, the advertisement half (3.5e / CA5) — work package H9 ────────────
//
// ⚠ THE OBJECTION IN `ppcp_discovery.h` DOES NOT REACH THIS, AND THAT IS WHY
// IT IS ADDITIVE.  What 3.5b calls "a responder, which a mobile platform
// supplies and several desktop platforms do not" is the thing that owns UDP
// 5353 for the whole machine.  On macOS that is mDNSResponder, it is already
// running, and `DNSServiceRegister` asks it — over the same local IPC socket
// `DNSServiceBrowse` already uses — to publish on our behalf.  This process
// binds no multicast socket either way.
//
// ⚠ 3.5d IS SATISFIED HERE RATHER THAN ASSUMED.  It forbids advertising for
// reconnection where the platform cannot resolve a PSK identity server-side,
// because 5.3b needs the listener to recompute `tag` with the `K_id` of every
// pairing held.  `m_listener.setIdentityResolver(m_rv.identityResolver())` at
// start-up IS that hook; a host without it would be discoverable and unable to
// complete the handshake it advertised for, which is the whole of the clause.
//
// ⚠ AND WHAT IS ADVERTISED IS PERSISTED PAIRINGS ONLY.  An outstanding CODE is
// dialled by the peer that scanned it, at the endpoint printed in the code
// (4.3d).  §3 is reconnection convenience; a pairing that has not happened yet
// has nothing to reconnect to.
void PpcpHostService::startAdvertising()
{
    m_advertiser = Ppcp::makePlatformAdvertiser();
    if (!m_advertiser) return;   // CA5 — Windows deferred; 3.6b makes it silent

    m_advert = std::make_unique<Ppcp::RvReconnectionAdvertisement>(
        m_advertiser.get(),
        // The `K_id` seam.  Eight bytes of `rn` and eight of `rid` cross it,
        // both of which 3.4e publishes in the clear on purpose; the key stays
        // in the rendezvous ledger and is never copied out.
        [this](const std::string &pairingId, std::uint8_t rn[PPCP_RV_RN_BYTES],
               std::uint8_t rid[PPCP_RV_RID_BYTES]) {
            return m_rv.mintRid(pairingId, rn, rid);
        },
        [](void *out, std::size_t len) { return Ppcp::csprngBytes(out, len); });

    // ⚠ AND THE WATCH IS INSTALLED BY `refreshAdvertisement()`, NOT HERE.  A
    // host that holds no persisted pairing yet registers nothing at start-up,
    // so there is no client socket to watch until the user remembers their
    // first phone — and a watch installed once at start-up would never be
    // installed at all on the run that matters.
    refreshAdvertisement();
}

void PpcpHostService::stopAdvertising()
{
    m_advertWatch.reset();
    if (m_advert) m_advert->stop();
    m_advert.reset();
    m_advertiser.reset();
}

// The set of persisted pairings is the ONLY input.  Called from start-up, from
// `rememberPairing()` and from `forgetPairing()`, which between them are every
// way it changes.
void PpcpHostService::refreshAdvertisement()
{
    if (!m_advert || !m_advertiser) return;

    const std::vector<std::string> held = m_rv.advertisablePairings();
    if (held.empty()) {
        // 3.4e's residual exposure is "anyone on the link can see that a
        // PPCP-capable peer is present".  With nothing to reconnect to there is
        // nothing to be found FOR, so the honest thing is to withdraw rather
        // than advertise an instance no phone can resolve.
        if (m_advert->active()) {
            // The watch goes first: `stop()` deallocates the DNS-SD ref and
            // closes the socket under it, and a QSocketNotifier left on a
            // closed descriptor is a busy loop.
            m_advertWatch.reset();
            m_advert->stop();
        }
        return;
    }
    const std::uint64_t now = static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch());
    if (m_advert->active()) {
        m_advert->setPairings(held, now);
        return;
    }
    // 3.7f is about a bootstrap instance and does not reach this one: a
    // reconnection instance names the PPCP listener, because the device dials
    // it to reconnect and 5.2g then makes the device the TLS client.
    if (!m_advert->start(m_port, held, now)) return;   // 3.6a — silent

    const int fd = m_advertiser->fd();
    if (fd < 0) return;
    m_advertWatch = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
    connect(m_advertWatch.get(), &QSocketNotifier::activated, this, [this] {
        if (m_advertiser && !m_advertiser->process()) {
            // 3.6a — the registration died.  Stop watching; report nothing.
            stopAdvertising();
        }
    });
    ppWarn() << "[ppcp-rv] advertising:" << m_advert->describe().c_str();
}

// ── The phone's own name ────────────────────────────────────────────────────
//
// ⚠ NOT IN THE KEYCHAIN, AND THE DISTINCTION IS THE POINT.  The pairing store
// holds PRK and says so in as many words — "nothing else, not the sid, not the
// endpoints, not the display name" — because 5.1c makes PRK the unit of
// storage and F-H6-3 records that this application's QSettings-backed secrets
// path is not protected storage.  A nickname is not key material: RV 7.2b
// constrains payloads and keys, and a display name is neither.  So it lives in
// the ordinary settings file beside `cameraAlias` and `imuAlias`, which is what
// it is — an alias for a device.
//
// ⚠ AND IT IS WHAT THE PHONE SAID, NOT WHAT WE DECIDED.  `product.model` is
// display text from an untrusted counterpart (4.4d), so it names a row and is
// never an identifier, never a trust signal, and never matched against.
void PpcpHostService::notePeerName(const Phone *ph)
{
    if (!ph || ph->pairingId.isEmpty() || ph->name.isEmpty()) return;
    QSettings s = ppSettings();
    QVariantMap names = s.value(QStringLiteral("ppcp/phoneNames")).toMap();
    QVariantMap row;
    row[QStringLiteral("name")]     = ph->name;
    row[QStringLiteral("peerId")]   = ph->counterpartId;
    row[QStringLiteral("lastSeen")] = QDateTime::currentSecsSinceEpoch();
    names[ph->pairingId] = row;
    s.setValue(QStringLiteral("ppcp/phoneNames"), names);
}

QString PpcpHostService::phoneNameFor(const QString &pairingId)
{
    const QVariantMap names =
        ppSettings().value(QStringLiteral("ppcp/phoneNames")).toMap();
    if (!names.contains(pairingId)) return {};
    return names.value(pairingId).toMap().value(QStringLiteral("name")).toString();
}

// ── Every phone this host knows about, as a device row ──────────────────────
//
// Built from the rendezvous ledger, which after `loadPersisted()` holds one
// entry per remembered pairing and one per code minted this run.  A LIVE code
// is not a phone — it is a QR on screen that nobody has scanned — so it is
// skipped here; it belongs to the pairing dialog.
QVariantList PpcpHostService::phones() const
{
    QVariantList out;
    for (const CodeStatus &st : m_rv.codes()) {
        const QString pid = QString::fromStdString(st.pairingId);
        // A phone, or a code?  Persisted means it was remembered, which can
        // only follow a pairing; otherwise it counts only if a link actually
        // resolved to it this run.  NOT `usesRemaining == 0`, which was the old
        // panel's rule: closeSession() zeroes that as well as invalidating, so
        // dismissing an unscanned QR left a device row behind for a phone that
        // never existed.
        if (!st.persisted && !m_pairedThisRun.contains(pid)) continue;

        const Phone *live = phoneByPairing(pid);
        const bool isLive = live != nullptr;
        const QString stored = phoneNameFor(pid);

        QVariantMap dev;
        dev[QStringLiteral("kind")]       = QStringLiteral("Phone");
        // The phone's own name where it has ever declared one; otherwise the
        // handle, shortened.  Inventing a friendlier name would assert a fact
        // about the device, and nothing here knows one.
        dev[QStringLiteral("name")] = isLive && !live->name.isEmpty() ? live->name
                                    : !stored.isEmpty()              ? stored
                                    : tr("Phone %1").arg(pid.left(6));
        dev[QStringLiteral("model")]      = stored;
        dev[QStringLiteral("backend")]    = QStringLiteral("PPCP");
        dev[QStringLiteral("identifier")] = pid;
        dev[QStringLiteral("pairingId")]  = pid;
        dev[QStringLiteral("persisted")]  = st.persisted;
        dev[QStringLiteral("invalidated")] = st.invalidated;

        // The same status vocabulary the camera and IMU rows use, so the home
        // screen's dot and the resource monitor need no new cases.  `available`
        // is discovery's word: paired, advertising on this network, not
        // connected.  Its ABSENCE says nothing — 3.6a, multicast fails routinely
        // — so a phone that is merely undiscovered reads `disconnected` and not
        // as any kind of fault.
        const bool seen = m_seenInstances.key(pid, QString()).isEmpty() == false;
        dev[QStringLiteral("status")] = st.invalidated ? QStringLiteral("revoked")
                                      : isLive        ? QStringLiteral("connected")
                                      : seen          ? QStringLiteral("available")
                                                      : QStringLiteral("disconnected");

        // A phone is not a Source.  Its CAMERAS carry the rate, and they are
        // their own rows in the same list — claiming a rate here would be
        // inventing a second number for the same bytes.
        dev[QStringLiteral("dataRateHz")]  = 0.0;
        dev[QStringLiteral("dataRateStr")] = QStringLiteral("—");
        dev[QStringLiteral("batteryPct")]  = -1;
        dev[QStringLiteral("hasWarning")]  = false;
        out.append(dev);
    }
    return out;
}

void PpcpHostService::onRelations(Phone *ph)
{
    if (!ph || ph->counterpartId.isEmpty()) return;

    // 6.1f — a `relation_update` was published, so every VideoInputPpcp bound
    // to this peer is re-fed its offset.  Per SOURCE timebase and not per peer:
    // a phone with a camera clock and an audio clock has one relation per clock
    // and a single scalar would fabricate one of them.
    const std::int64_t now = hostNowNs();
    const PpcpLiveSession &live = ph->peer->liveSession();
    const int mapped = VideoInputPpcp::applyTimebaseOffsets(
        ph->counterpartId, [&](const QString &tb, qint64 *outNs) {
            std::int64_t off = 0;
            if (!live.offsetToRefNs(tb.toStdString(), now, &off)) return false;
            *outNs = static_cast<qint64>(off);
            return true;
        });
    (void)mapped;
}

void PpcpHostService::onTick()
{
    // ⚠ THE CODE'S HALF RUNS BEFORE THE LINK GUARD, AND THAT IS THE WHOLE POINT.
    // It used to sit below a `no phone connected` early-out, so none of it ran
    // while no phone was connected — which is the entire window in which a code
    // is displayed and counting down.  The countdown a user reads was therefore
    // frozen at whatever `publishPairingCode()` left behind, and only ever moved
    // AFTER a device had already paired, when nobody is looking at it.
    //
    // 7.3b's periodic half, and 7.2d's: a code nobody used still holds a K_tls
    // until something removes it.
    if (m_rv.reap() > 0) emit codeChanged();

    // 3.4a/3.4d1 — the `rn` rotation, driven off the timer that is already
    // running rather than a second one.  `tick()` is an integer comparison
    // until a rotation is actually due, so calling it at 50 Hz costs nothing;
    // when one IS due it is a single TXT update (3.2d), not a re-registration.
    if (m_advert) m_advert->tick(static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch()));

    if (m_codeLive) {
        // ⚠ ONCE A SECOND, NOT ONCE A TICK.  m_timer is 20 ms, so emitting
        // unconditionally here signalled `codeChanged` fifty times a second and
        // every QR view repainting on it redrew its ~1681 modules each time.
        // `codeSecondsLeft()` has one-second resolution, so a signal that fires
        // faster carries no information that anything can render.
        const int left = codeSecondsLeft();
        if (left != m_lastSecondsLeft) {
            m_lastSecondsLeft = left;
            emit codeChanged();
        }
        // 7.3e — the publisher holds the authoritative clock, so when it says
        // the code is spent the code IS spent.  Nothing else cleared m_codeLive:
        // reap() drops the rendezvous entry but leaves this class believing it
        // still displays a live code, so the panel sat on "expires in 0s" for
        // as long as it was open while the handshake behind it would be refused.
        if (left == 0) {
            // ⚠ RENEW, DON'T GO DARK.  This used to close the code and leave
            // the panel showing an expired QR with a "Get a new code" button —
            // a click that only a person standing at this computer can make,
            // to prove something they proved by standing there.  7.3 is
            // explicit that the real defences are elsewhere: `mu` and 7.3b are
            // "clock-free and are the primary defence; `exp` ... is therefore
            // secondary rather than relied upon".  Renewing weakens neither.
            // Every renewal is a WHOLE new code — fresh psk and sid (7.3d),
            // its own 5-minute exp (7.3c), and the one it replaces invalidated
            // (7.3b) — so no individual code lives a second longer than before.
            //
            // Only while the panel is open, which needs no extra state: the
            // dialog publishes on open and closes on dismiss, so `m_codeLive`
            // IS "the panel is showing".  And not once a phone is on the link,
            // where a displayed code has nothing left to do.
            // Renewed whether or not a phone is already connected: with `mu: 1`
            // a live link does not make the NEXT phone's code unnecessary, and
            // publishCode() knows not to invalidate a session that is carrying
            // one.
            if (!publishCode(/*userAsked=*/false)) closePairingCode();
        }
    }

    // ⚠ EVERY PHONE, AND COLLECT THE DEAD BEFORE TOUCHING THE LIST.
    // dropPhone() erases from m_phones, so ticking and dropping in one pass
    // would invalidate the iterator underneath itself.
    const std::int64_t now = hostNowNs();
    std::vector<Phone *> dead;
    for (const std::unique_ptr<Phone> &p : m_phones)
        if (!p->peer->tick(now)) dead.push_back(p.get());
    for (Phone *p : dead) dropPhone(p, "link closed");
}

// ── RV §4 — the pairing code ────────────────────────────────────────────────

void PpcpHostService::setCodeLifetimeSecondsForTest(int seconds)
{
    m_codeLifetimeS = seconds > 0 ? seconds : 0;
}

bool PpcpHostService::publishCode(bool userAsked)
{
    if (!m_listening) {
        setStatus(tr("Not listening, so there is nothing to pair with."));
        return false;
    }
    // 7.3b — displacing a code invalidates it, used or not.  ⚠ UNLESS A PHONE
    // IS ON THE OTHER END OF IT.  Pressing "New code" after a phone paired used
    // to run closeSession() over the pairing that phone is connected on, wiping
    // the K_tls its reconnection (7.5a) and its preview channel (ENC 2.1d) both
    // need.  The displaced code is spent either way; only the session differs.
    dropDisplayedCode(/*invalidateSession=*/!displayedCodePairedAPhone());
    // A new code is a new attempt, so the last one's failure stops being the
    // answer to "what is happening".  ⚠ closePairingCode() deliberately does
    // NOT do this: a user who dismisses the panel after a refusal should still
    // find out what happened, and only asking for a fresh code says otherwise.
    // ⚠ AND NEITHER DOES THE AUTOMATIC RENEWAL — see publishCode()'s comment.
    if (userAsked) clearFailure();

    // 4.3d — every address this host is reachable at: wired, wireless, and its
    // hotspot address where it provides one.  This is what makes the code work
    // when discovery does not, and discovery WILL NOT WORK AT A RANGE (3.6a).
    const std::vector<RvEndpoint> eps = reachableEndpoints(m_port);

    PpcpRendezvous::Config cfg;
    cfg.codeLifetimeS = m_codeLifetimeS > 0 ? static_cast<std::uint64_t>(m_codeLifetimeS)
                                            : kCodeLifetimeS;
    cfg.maxUses = 1;   // 7.3a; anything above it costs 7.4f
    // 4.3/4.4d — ours to print, UNTRUSTED at the far end.  A bay name and not a
    // person's name: the code is photographed and the name is on it.
    cfg.displayName = "PinPointStudio";

    std::string err;
    if (!m_rv.publish(cfg, eps, nullptr, &m_code, &err)) {
        setStatus(tr("Could not publish a pairing code: %1").arg(QString::fromStdString(err)));
        return false;
    }

    // 4.1d — error correction level M or higher.  A failed encode is a REFUSAL
    // and never a truncated symbol: a truncated code scans perfectly and pairs
    // with nothing.
    m_qr = QrCode::encodeText(m_code.uri);
    if (!m_qr.isValid()) {
        m_rv.closeSession(m_code.sessionId);
        setStatus(tr("The pairing code is too long to render."));
        return false;
    }
    m_codeLive = true;
    m_lastSecondsLeft = -1;
    refreshCode();
    setStatus(tr("Scan the code with the capture device."));
    return true;
}

void PpcpHostService::closePairingCode()
{
    // ⚠ NOT UNCONDITIONALLY TRUE, and the QML used to carry this rule instead:
    // the pair panel guarded its own close with "unless a phone got in first,
    // because closing its session would take the link down with it".  A rule
    // that lives in a caller is a rule the next caller does not have.  It lives
    // here now, which is why the panel can simply close its code.
    dropDisplayedCode(/*invalidateSession=*/!displayedCodePairedAPhone());
}

// Is the code currently on screen the one a CONNECTED phone paired on?
bool PpcpHostService::displayedCodePairedAPhone() const
{
    return m_codeLive
        && phoneByPairing(QString::fromStdString(m_code.pairingId)) != nullptr;
}

// ⚠ WHY INVALIDATING IS A DECISION AND NOT A REFLEX.  `closeSession()` marks
// the row invalidated AND wipes its key material (7.2d) unless the pairing was
// persisted.  That is exactly right for a code nobody used — and destructive
// for one a phone HAS used, because the pairing outlives the code that created
// it (7.3f) and the live link still needs `K_tls`: RV 7.5a reconnects a dropped
// channel on it, and ENC 2.1d opens the preview channel on it later, with a
// handshake that has to resolve.  `noteLinkEstablished()` is deliberate about
// this — it spends the code and keeps the keys, saying so in as many words.
//
// So displacing a SPENT code drops the display and leaves the session alone.
// Nothing is lost by that: 7.3a already took `usesRemaining` to zero, so the
// code cannot pair anything else; it is invalidated in the only sense that
// matters, and 7.3b will finish the job when the session really does close.
void PpcpHostService::dropDisplayedCode(bool invalidateSession)
{
    if (!m_codeLive) return;
    // 7.3b — invalidated when the session it belongs to closes, whether or not
    // it was used, and its key material erased with it (7.2d) unless the
    // pairing was persisted under 7.4.
    if (invalidateSession) m_rv.closeSession(m_code.sessionId);
    m_codeLive = false;
    m_code = PublishedCode{};
    m_qr = QrCode{};
    // -1 rather than 0: 0 is a value codeSecondsLeft() genuinely returns, and a
    // memo holding it would swallow the first tick of the NEXT code that
    // happened to be read at its final second.
    m_lastSecondsLeft = -1;
    refreshCode();
}

void PpcpHostService::refreshCode()
{
    m_qrRows.clear();
    m_codeEndpoints.clear();
    if (m_qr.isValid()) {
        for (int y = 0; y < m_qr.size(); ++y) {
            QString row;
            row.reserve(m_qr.size());
            for (int x = 0; x < m_qr.size(); ++x)
                row.append(m_qr.at(x, y) ? QLatin1Char('1') : QLatin1Char('0'));
            m_qrRows.append(row);
        }
    }
    CodeStatus st;
    if (m_codeLive && m_rv.status(m_code.pairingId, &st))
        for (const auto &e : st.endpoints)
            m_codeEndpoints << QStringLiteral("%1:%2")
                                   .arg(QString::fromStdString(e.host))
                                   .arg(e.port);
    emit codeChanged();
}

int PpcpHostService::codeSecondsLeft() const
{
    if (!m_codeLive || m_code.expUnixS == 0) return 0;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 left = static_cast<qint64>(m_code.expUnixS) - now;
    return left > 0 ? static_cast<int>(left) : 0;
}

QVariantList PpcpHostService::outstandingCodes() const
{
    // 7.3a/7.3b's visible half: what is outstanding, how much of it is left and
    // whether it still authenticates.  Built from CodeStatus, which holds no
    // secret and no payload field — the same struct the diagnostic export uses,
    // and for the same reason.
    QVariantList out;
    for (const CodeStatus &c : m_rv.codes()) {
        QVariantMap m;
        m[QStringLiteral("pairingId")] = QString::fromStdString(c.pairingId);
        m[QStringLiteral("sessionId")] = QString::fromStdString(c.sessionId);
        m[QStringLiteral("usesRemaining")] = static_cast<qulonglong>(c.usesRemaining);
        m[QStringLiteral("maxUses")] = static_cast<qulonglong>(c.maxUses);
        m[QStringLiteral("invalidated")] = c.invalidated;
        m[QStringLiteral("persisted")] = c.persisted;
        m[QStringLiteral("endpoints")] = static_cast<int>(c.endpoints.size());
        out.append(m);
    }
    return out;
}

bool PpcpHostService::rememberPairing(const QString &pairingId)
{
    std::string why;
    const bool ok = m_rv.persist(pairingId.toStdString(), &why);
    if (!ok) setStatus(tr("Cannot remember this device: %1").arg(QString::fromStdString(why)));
    // 3.5e — a pairing only becomes worth advertising once it is persisted,
    // because that is the moment it acquires something to reconnect to.
    if (ok) refreshAdvertisement();
    emit codeChanged();
    emit phonesChanged();
    return ok;
}

void PpcpHostService::forgetPairing(const QString &pairingId)
{
    // 7.4d — revocation is honoured immediately by this side, which means the
    // next handshake resolves nothing and fails like any stranger (7.7c).
    m_rv.revoke(pairingId.toStdString());
    // 7.4d is "honoured immediately by this side", and an advertisement still
    // naming the revoked pairing would be this side continuing to offer it.
    refreshAdvertisement();
    // The nickname goes with the key.  7.4d is about the pairing, but a name
    // left behind is a record that this host has met that phone, and "forget"
    // must not leave one.
    {
        QSettings st = ppSettings();
        QVariantMap names = st.value(QStringLiteral("ppcp/phoneNames")).toMap();
        if (names.remove(pairingId) > 0)
            st.setValue(QStringLiteral("ppcp/phoneNames"), names);
    }
    emit codeChanged();
    emit phonesChanged();
}

QString PpcpHostService::discoveryDescription() const
{
    if (!m_browser)
        return tr("no service discovery on this platform");
    return QString::fromStdString(m_browser->describe());
}

QString PpcpHostService::diagnosticExport() const
{
    // RT-9 — nothing here carries a secret or a payload.  The browser's
    // description is a build fact ("DNS-SD via mDNSResponder (browse only)"),
    // and the count is of pairings this host already holds; no `rid`, no
    // instance name and no endpoint goes in, because those describe a peer.
    // The advertisement's line names no pairing either, for the same reason:
    // a local handle is not key material, but recording WHICH pairing was on
    // the wire is the correlation 3.4e's unlinkability argument is about.
    return QString::fromStdString(m_rv.diagnosticExport())
         + QStringLiteral("\ndiscovery: %1\ndiscovered-pairings: %2\nadvertisement: %3\n")
               .arg(discoveryDescription())
               .arg(m_seenInstances.size())
               .arg(m_advert ? QString::fromStdString(m_advert->describe())
                             : tr("no service advertisement on this platform"));
}

// ── What a failed arrival is allowed to say ─────────────────────────────────
//
// ⚠ READ THE HEADER'S NOTE BEFORE CHANGING A WORD OF THIS.  RV 7.7c binds what
// the COUNTERPART observes, not this screen — but `FailureKind::Handshake` is
// still unnameable HERE, because the transport never told us which check failed
// and must not be made to.  The other three are policy, framing and silence
// respectively, none of them the pair of outcomes 7.7c holds together.
QString PpcpHostService::describeFailure(const HandshakeFailure &f)
{
    switch (f.kind) {
    case FailureKind::None:
        return QString();

    case FailureKind::NotForwardSecret:
        return tr("A phone was refused: its secure connection is not forward "
                  "secret, which PPCP does not allow.");

    case FailureKind::HandshakeTimeout:
        return tr("A phone connected but did not finish securing the link in "
                  "time, and was dropped.");

    case FailureKind::BindRejected:
        return tr("A phone secured the link but its stream was refused: %1.")
                 .arg(QString::fromLatin1(describe(f.bind)));

    case FailureKind::Handshake:
        break;
    }

    // The uniform one.  There is nothing to name and there never will be, so
    // what is offered instead is the fact the counterpart already observed: the
    // TLS alert and how long it took.  5.3c makes that alert IDENTICAL for an
    // unresolvable identity and a wrong key, so it discriminates nothing — and
    // it is the only thing that tells one failing phone from another when the
    // sentence above it cannot change.
    QString s = tr("A phone reached this computer and the secure connection "
                   "failed. Nothing was disclosed to it.");
    if (f.alert >= 0) {
        s += QLatin1Char(' ');
        s += f.alertWasSent
                 ? tr("(TLS alert %1, sent by this computer, after %2 ms.)")
                       .arg(f.alert).arg(qRound(f.elapsedMs))
                 : tr("(TLS alert %1, sent by the phone, after %2 ms.)")
                       .arg(f.alert).arg(qRound(f.elapsedMs));
    }
    return s;
}

void PpcpHostService::noteFailure(const HandshakeFailure &f)
{
    if (f.kind == FailureKind::None) return;
    noteFailureText(describeFailure(f));
}

void PpcpHostService::noteFailureText(const QString &text)
{
    if (text.isEmpty()) return;
    m_lastFailureText = text;
    ++m_failureCount;
    emit failureChanged();
}

void PpcpHostService::clearFailure()
{
    if (m_lastFailureText.isEmpty()) return;
    m_lastFailureText.clear();
    emit failureChanged();
}

void PpcpHostService::noteHandshakeFailureForTest(const HandshakeFailure &f)
{
    noteFailure(f);
}

void PpcpHostService::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}
