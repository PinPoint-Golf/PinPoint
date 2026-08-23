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
    : QObject(parent), m_peer([] {
          PpcpHostPeer::Config c;
          c.peerId = hostPeerId().toStdString();
          return c;
      }())
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

    m_peer.setDeclarationHook([this](const ppcp_peer_desc *d) { onDeclare(d); });
    m_peer.setRelationsHook([this](const PpcpLiveSession &) { onRelations(); });
    m_peer.addEventHook([this](const ppcp_event &ev) {
        // ⚠ THE EVENT RING HAS EXACTLY ONE DRAINER AND IT IS PpcpHostPeer.
        // Everything else that needs to see events registers here.  In
        // particular `VideoInputPpcp::drainEvents()` MUST NOT be called on this
        // peer — it exists for the standalone paths where nothing else drains.
        if (m_offers) m_offers->observe(ev);
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
    m_peer.setStorage([](std::uint64_t *freeBytes) {
        if (!freeBytes) return false;
        const QStorageInfo si(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        if (!si.isValid() || !si.isReady() || si.bytesAvailable() < 0) return false;
        *freeBytes = static_cast<std::uint64_t>(si.bytesAvailable());
        return true;
    });

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
    stop();
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

    // MSG 3.3c — a peer declares BEFORE it originates any message referencing a
    // Source, Stream or Candidate, so this happens before anything is pumped.
    std::string derr;
    if (!m_peer.declareSelf(PpcpSourceDeclaration::hostInventory(), &derr))
        ppWarn() << "[ppcp] the host could not declare itself:" << derr.c_str();

    m_engine = m_peer.makeLibppcpEngine(&derr);
    if (!m_engine) {
        if (err) *err = QString::fromStdString(derr);
        setStatus(tr("Could not build the PPCP engine: %1").arg(QString::fromStdString(derr)));
        m_listener.stop();
        return false;
    }

    m_stopping = false;
    m_acceptThread = std::thread([this] {
        while (!m_stopping) {
            // A short timeout rather than a long one so stop() is prompt; the
            // listener is not doing anything expensive while it waits.
            HandshakeFailure fail;
            std::unique_ptr<PeerConnection> link = m_listener.accept(250, &fail);
            if (!link) continue;
            // Hand it to the GUI thread.  Everything PPCP is single-threaded
            // from here on: this thread owns nothing but the accept call.
            auto *raw = link.release();
            QMetaObject::invokeMethod(this, [this, raw] {
                adoptLink(std::unique_ptr<PeerConnection>(raw));
            }, Qt::QueuedConnection);
        }
    });

    startDiscovery();

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
    m_stopping = true;
    if (m_acceptThread.joinable()) m_acceptThread.join();
    dropLink("shutdown");
    m_listener.stop();
    // 7.3b — the code dies with the session it belongs to, used or not, and
    // 7.2d erases its key material with it.
    closePairingCode();
    m_engine.reset();
    m_listening = false;
    emit stateChanged();
}

void PpcpHostService::adoptLink(std::unique_ptr<PeerConnection> link)
{
    if (!link) return;
    if (m_link) {
        // One link at a time in this build.  A second dialler is refused at the
        // link rather than half-adopted: 7.3a already spent the code, and a
        // host that quietly replaced a live capture peer mid-session would lose
        // whatever it was carrying.
        link->close();
        return;
    }

    // RV 7.3a — the code is spent by the PAIRING, not by the handshake.  This
    // link is two TLS handshakes (three with preview) over one K_tls; counting
    // handshakes would invalidate a `mu: 1` code on the control channel and
    // refuse the bulk channel of the SAME link.  See F-H6-1.
    const std::string pairing = link->pairingId();
    if (!pairing.empty()) m_rv.noteLinkEstablished(pairing);
    // ...and KEEP it.  Everything that wants to say something about "the phone
    // on the other end of this link" — its name, its device row, whether the
    // remembered pairing in Settings is the one currently connected — needs the
    // join between the live link and the pairing, and until now the only reader
    // of link->pairingId() consumed it and let it go.
    m_linkPairingId = QString::fromStdString(pairing);
    if (!m_linkPairingId.isEmpty()) m_pairedThisRun.insert(m_linkPairingId);

    m_link = std::move(link);

    // ⚠ F-H8-5 — A `ppcp_peer` IS THE CONVERSATION, NOT THE APPLICATION, AND
    // THIS BUILT ONE ENGINE IN start() AND GAVE IT TO EVERY LINK.
    //
    // Found by H8's conformance run on 23 Aug 2026, where twelve rows dial in
    // turn: the FIRST got a Session and the other eleven were refused
    // `ppcp_peer_session_open: invalid argument`, because the engine still held
    // the previous device's open Session, its declaration, its `msg_id`
    // sequence and its link state.  Nothing had closed that Session — the link
    // died rather than saying goodbye, which is the ordinary way a link ends.
    // In the application the same defect reads as "the second device to pair
    // after a drop never gets a Session", and nothing in `ppcp-tests` could see
    // it because every suite there builds one engine for one link.
    //
    // 7.5a's resume is a DIFFERENT case and is not what this was: resume is the
    // same peer on the same K_tls, and it is opened deliberately, not inherited
    // by whoever dials next.
    std::string derr;
    m_engine = m_peer.makeLibppcpEngine(&derr);
    if (!m_engine) {
        ppWarn() << "[ppcp] could not build an engine for this link:" << derr.c_str();
        setStatus(tr("Could not build the PPCP engine: %1").arg(QString::fromStdString(derr)));
        m_link->close();
        m_link.reset();
        return;
    }
    m_peer.attach(m_link.get(), m_engine.get());

    const TlsOutcome &tls = m_link->tls();
    // 5.4k — the achieved version and key-exchange mode are made available to
    // the application layer and recorded.  Forward secrecy is a per-connection
    // outcome since the 5.4.3 relaxation, so a peer that cannot say which it
    // got cannot honestly tell a user what "encrypted" means here.
    ppWarn() << "[ppcp-rv] link up:" << tls.describe().c_str();
    setStatus(tr("Device connected — %1.").arg(QString::fromStdString(tls.describe())));
    refreshCode();
    emit stateChanged();
}

void PpcpHostService::dropLink(const char *why)
{
    if (!m_link) return;
    if (m_offers) m_offers->detach();
    if (!m_counterpartId.isEmpty()) VideoInputPpcp::clearTimebaseMappings(m_counterpartId);
    m_peer.attach(nullptr, nullptr);
    m_link->close();
    m_link.reset();
    m_counterpartId.clear();
    m_peerName.clear();
    setStatus(tr("Device disconnected (%1).").arg(QString::fromLatin1(why)));
    m_linkPairingId.clear();
    emit stateChanged();
    // The phone did not stop existing, it stopped being here — its row stays
    // and changes state, the way a switched-off IMU's does.
    emit phonesChanged();
}

void PpcpHostService::onDeclare(const ppcp_peer_desc *desc)
{
    if (!desc) return;

    // MSG 3.3 — a peer's cameras exist the moment it declares and at no other
    // moment.  There is no bus to walk and no scan to run: this is the whole of
    // PPCP camera discovery, and it is why `VideoInputFactory::enumerateDevices()`
    // has never had a PPCP branch.
    const int n = VideoInputFactory::registerPpcpPeer(desc);

    m_counterpartId = QString::fromUtf8(desc->id.v, static_cast<int>(desc->id.len));
    // 4.4d's habit of mind, one layer in: a counterpart's product strings are
    // display text and are never an identifier or a trust signal.
    m_peerName = QString::fromUtf8(desc->product.model.v,
                                   static_cast<int>(desc->product.model.len));

    // MSG 9.1 — the offer list, which has been installed DETACHED since H5
    // because there was no live peer to give it.  There is now.
    if (m_offers && m_engine && m_engine->peer())
        m_offers->attach(m_engine->peer(), m_counterpartId);

    // The one moment a phone says what it is called.  Written down here or not
    // at all: `dropLink()` clears m_peerName, and nothing about a pairing
    // survives a restart except its handle and its key.
    notePeerName();

    ppWarn() << "[ppcp] peer declared:" << m_peerName << "-" << n << "camera Source(s)";
    setStatus(tr("%1 connected — %2 camera(s).").arg(m_peerName).arg(n));

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
void PpcpHostService::notePeerName()
{
    if (m_linkPairingId.isEmpty() || m_peerName.isEmpty()) return;
    QSettings s = ppSettings();
    QVariantMap names = s.value(QStringLiteral("ppcp/phoneNames")).toMap();
    QVariantMap row;
    row[QStringLiteral("name")]     = m_peerName;
    row[QStringLiteral("peerId")]   = m_counterpartId;
    row[QStringLiteral("lastSeen")] = QDateTime::currentSecsSinceEpoch();
    names[m_linkPairingId] = row;
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

        const bool isLive = !m_linkPairingId.isEmpty() && pid == m_linkPairingId;
        const QString stored = phoneNameFor(pid);

        QVariantMap dev;
        dev[QStringLiteral("kind")]       = QStringLiteral("Phone");
        // The phone's own name where it has ever declared one; otherwise the
        // handle, shortened.  Inventing a friendlier name would assert a fact
        // about the device, and nothing here knows one.
        dev[QStringLiteral("name")] = isLive && !m_peerName.isEmpty() ? m_peerName
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

void PpcpHostService::onRelations()
{
    if (m_counterpartId.isEmpty()) return;

    // 6.1f — a `relation_update` was published, so every VideoInputPpcp bound
    // to this peer is re-fed its offset.  Per SOURCE timebase and not per peer:
    // a phone with a camera clock and an audio clock has one relation per clock
    // and a single scalar would fabricate one of them.
    const std::int64_t now = hostNowNs();
    const PpcpLiveSession &live = m_peer.liveSession();
    const int mapped = VideoInputPpcp::applyTimebaseOffsets(
        m_counterpartId, [&](const QString &tb, qint64 *outNs) {
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
    // It used to sit below `if (!m_link) return`, which meant none of it ran
    // while no phone was connected — which is the entire window in which a code
    // is displayed and counting down.  The countdown a user reads was therefore
    // frozen at whatever `publishPairingCode()` left behind, and only ever moved
    // AFTER a device had already paired, when nobody is looking at it.
    //
    // 7.3b's periodic half, and 7.2d's: a code nobody used still holds a K_tls
    // until something removes it.
    if (m_rv.reap() > 0) emit codeChanged();

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
        if (left == 0) closePairingCode();
    }

    if (!m_link) return;
    if (!m_peer.tick(hostNowNs())) dropLink("link closed");
}

// ── RV §4 — the pairing code ────────────────────────────────────────────────

void PpcpHostService::setCodeLifetimeSecondsForTest(int seconds)
{
    m_codeLifetimeS = seconds > 0 ? seconds : 0;
}

bool PpcpHostService::publishPairingCode()
{
    if (!m_listening) {
        setStatus(tr("Not listening, so there is nothing to pair with."));
        return false;
    }
    // 7.3b — displacing a code invalidates it, used or not.
    closePairingCode();

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
    if (!m_codeLive) return;
    // 7.3b — invalidated when the session it belongs to closes, whether or not
    // it was used, and its key material erased with it (7.2d) unless the
    // pairing was persisted under 7.4.
    m_rv.closeSession(m_code.sessionId);
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
    emit codeChanged();
    emit phonesChanged();
    return ok;
}

void PpcpHostService::forgetPairing(const QString &pairingId)
{
    // 7.4d — revocation is honoured immediately by this side, which means the
    // next handshake resolves nothing and fails like any stranger (7.7c).
    m_rv.revoke(pairingId.toStdString());
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
    return QString::fromStdString(m_rv.diagnosticExport())
         + QStringLiteral("\ndiscovery: %1\ndiscovered-pairings: %2\n")
               .arg(discoveryDescription())
               .arg(m_seenInstances.size());
}

void PpcpHostService::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}
