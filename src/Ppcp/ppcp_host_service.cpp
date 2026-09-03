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

#include <QCryptographicHash>
#include <algorithm>

#include <QDateTime>
#include <QSysInfo>
#include <cmath>
#include <QDir>
#include <QSocketNotifier>

// 11.6f — the erasure below is not `memset`, which the optimiser is entitled to
// elide on a dying object.  OpenSSL is already this application's crypto.
#include <openssl/crypto.h>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QMetaObject>
#include <QUuid>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include <ppcp/version.h>

#include "../Core/pp_debug.h"
#include "../Core/pp_settings.h"
#include "../Video/VideoInputPpcp.h"
#include "../Video/video_input_factory.h"
#include "ppcp_offer_controller.h"
#include "ppcp_source_declaration.h"
#include "ppcp_wired_link.h"

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

// ── The name a phone shows for this computer ────────────────────────────────
//
// ⛔ NOT in the mDNS TXT record, and that is a security decision rather than an
// oversight.  §3's advertisement is cleartext on the LAN and deliberately
// minimal — `txtvers`, `pv`, `role`, `rn`, `rid` — and `rid` ROTATES precisely so
// a passive radio observer cannot link one venue's sightings to another's
// (3.4d/3.4e).  A stable human name beside it would undo that in one field: it is
// exactly the tracking beacon the rotation exists to prevent.
//
// So the name travels on the two paths that are already private or already
// consented to: the PAIRING CODE, which the operator is showing on purpose
// (4.3/4.4d), and `declare`'s `product`, which is inside the encrypted link
// after both ends have authenticated.
QString studioNameDefault()
{
    // The machine's own name, which is what a person already calls it. Falls
    // back only if the platform will not say.
    QString host = QSysInfo::machineHostName().trimmed();
    // ⚠ mDNS hands back "Marks-Mac-mini.local"; the suffix is machinery and not
    // part of what anybody calls the machine. Stripped so the default is
    // presentable enough that most people never edit it.
    if (host.endsWith(QStringLiteral(".local"), Qt::CaseInsensitive))
        host.chop(6);
    return host.isEmpty() ? QStringLiteral("PinPointStudio") : host;
}

// RV 7.3c — the shortest expiry the workflow tolerates.  Long enough to walk to
// the bay, short enough that a photograph of the screen is worth little.
constexpr std::uint64_t kCodeLifetimeS = 300;

// The keychain service the persisted pairings live under (RV 7.2c).
const char *kPairingService = "golf.pinpoint.studio.ppcp.pairings";

// The worst (largest) 6.1f sigma across every related timebase ONE phone's
// live session currently holds, in milliseconds. -1 when it holds none yet.
// Shared by the per-phone Settings -> Phones row and the
// phoneWorstSyncSigmaMs() aggregate, so the domain-correct evaluation point —
// `observedAtNs()`, never `hostNowNs()` (see that method's comment) — exists
// in exactly one place.
double worstSyncSigmaMsFor(const PpcpLiveSession &live)
{
    double worst = -1.0;
    for (const std::string &tb : live.relatedTimebases()) {
        std::int64_t atNs = 0;
        if (!live.observedAtNs(tb, &atNs)) continue;
        std::int64_t off = 0;
        double sigmaNs = 0.0;
        if (!live.offsetToRefNs(tb, atNs, &off, &sigmaNs)) continue;
        const double ms = sigmaNs / 1.0e6;
        if (ms > worst) worst = ms;
    }
    return worst;
}

// The estimator behind ONE source timebase, for the convergence trace.
//
// ⚠ NOT `ppcp_peer_sync_estimator_at(peer, 0)`, which is what the trace used to
// ask for while iterating relatedTimebases(): index 0 is whichever estimator was
// registered first, so on a phone declaring a camera clock AND an audio clock the
// trace attributed one estimator's sample count to every timebase it printed.
// I21 gives a multi-clock peer one estimator per timebase and there is no
// composition between them, so the lookup has to be by name.
//
// Matched on the REMOTE id because relatedTimebases() yields source timebases —
// `offsetToRefNs(sourceTimebase, ...)` names it in the parameter.
const ppcp_sync_estimator *estimatorForSource(ppcp_peer *peer, const std::string &sourceTb)
{
    if (!peer) return nullptr;
    const std::size_t n = ppcp_peer_sync_count(peer);
    for (std::size_t i = 0; i < n; ++i) {
        const ppcp_sync_estimator *e = ppcp_peer_sync_estimator_at(peer, i);
        if (!e) continue;
        const ppcp_id *remote = ppcp_sync_estimator_remote_tb(e);
        if (!remote) continue;
        if (sourceTb.size() == remote->len &&
            std::memcmp(sourceTb.data(), remote->v, remote->len) == 0)
            return e;
    }
    return nullptr;
}

}  // namespace

PpcpHostService::PpcpHostService(QObject *parent)
    : QObject(parent)
{
    // Installing the store does not itself persist anything — a completed
    // pairing writes its own key, inside PpcpRendezvous, the moment it exists
    // (erratum E57: 7.4b is now a SHOULD, and this application remembers by
    // default rather than asking first).  On a platform with no store this
    // stays null and every persistence attempt refuses rather than writing to
    // a file (7.2c) — not reachable in practice since erratum E56.
    m_rv.setSecretStore(makePlatformPairingStore(kPairingService));
    m_rv.loadPersisted();

    // ⚠ pump() AND tick() ARE DIFFERENT SCHEDULES.  tick() is TIME PASSING —
    // §6.3's sync cadence, §7.4's heartbeats and 8.2h's issue hold — and runs
    // here, on this timer, because a loop that ran only when bytes arrived would
    // never notice a dead peer, which is the one thing §7.4 exists to notice.
    //
    // ⛔ pump() IS DRIVEN BY BYTES BEING READY, AND UNTIL 29 AUG 2026 IT WAS NOT.
    // This comment claimed the split above while both halves ran off this one
    // 20 ms timer, so a reply waited up to a tick to be read — and `libppcp`
    // stamps sync `t4` inside ppcp_peer_feed() (`ppcp_peer.c:2675`), which pump()
    // is what calls.  Measured on hardware: `min_rtt` ~18 ms on a link whose real
    // round trip is a few, i.e. the poll rather than the network.  Phone::reads
    // now carries a QSocketNotifier per channel (see watchChannels()); tick()
    // still pumps as well, which costs nothing and covers the case where TLS has
    // buffered plaintext that the socket has no readability left to announce.
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

    // ── The import ledger (MSG §9.1, I34) ──────────────────────────────────
    //
    // Loaded once, here, and shared by every phone: what this host already
    // holds is a property of the host, not of the link it came in on.  The root
    // is the same one the file-import path uses, read out of the shared INI
    // rather than through AppSettings, because nothing in src/Ppcp may depend
    // on src/Gui.
    //
    // ⭐ IT IS NO LONGER UNDER `PPCP Imports/`.  That was right while bundles
    // were the only thing it recorded and wrong the moment a live capture lands
    // in the swing library instead: the ledger is the link between an opaque
    // PPCP identity and a derived swing identity, and it belongs above both
    // landing sites rather than inside one of them.  `<library>/ppcp-ledger.json`
    // now covers both, with `localPath` pointing wherever the bytes went.
    {
        QString root = ppSettings().value(QStringLiteral("General/athleteLibraryPath"))
                           .toString();
        if (root.isEmpty())
            root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(root);
        m_importLedger.load(
            QDir(root).filePath(QStringLiteral("ppcp-ledger.json")).toStdString());

        // The migration, run on every launch because it is idempotent: admit()
        // refuses to rewrite a held record, so a second fold adds nothing.  The
        // old file is left where it is.
        const QString legacy = QDir(root).filePath(
            QStringLiteral("PPCP Imports/ppcp-import.json"));
        const std::size_t folded = m_importLedger.foldIn(legacy.toStdString());
        if (folded > 0) {
            ppWarn() << "[ppcp] folded" << folded
                     << "capture(s) in from the legacy ledger at" << legacy
                     << "— the old file is left in place";
            m_importLedger.save();
        }

        ppWarn() << "[ppcp] ledger:" << m_importLedger.captureCount()
                 << "capture(s)," << m_importLedger.sessionCount() << "session(s),"
                 << m_importLedger.pendingCommitCount() << "commit(s) owed —"
                 << QString::fromStdString(m_importLedger.path());
    }
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
    // MSG 9.1a — `session_accept.have_digests` is what this host ALREADY HOLDS,
    // and a device does not replay the payload for one of those.  Until this
    // line the controller had no ledger, so the list was always empty and every
    // accepted offer re-sent every byte we already had.  `setLedger()` had no
    // production caller at all.
    if (m_offers) m_offers->setLedger(&m_importLedger);
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
    // ENC 2.1d and is collected by the accept thread's `acceptChannelFor()`
    // poll, not counted here.
    //
    // ⚠ THIS COMMENT USED TO SAY "taken through acceptInto()", AND NOTHING TOOK
    // IT.  `acceptInto()` had no caller outside the conformance harness, so a
    // phone opening a preview channel bound it and then watched it expire: the
    // link_id leaves the listener's table when its first two channels complete,
    // so the third arrived as a new half-built link with no control channel and
    // `expireLinks()` tore it down after `bindTimeoutMs`.  Silent at both ends.
    // Raised by the PinPointCapture team asking whether we accept a third bind
    // before they dialled one — which is the cheapest way this was ever going
    // to be found.
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
            // ENC 2.1d — before the plain accept, give any live link that is
            // still short of its third channel a chance to collect one.  A
            // short poll each: this is the same listener, and a long wait here
            // would starve the accept below.
            //
            // ⚠ THE CHANNEL CROSSES THREADS, THE LINK NEVER DOES.  A
            // TransportChannel is self-contained; the PeerConnection it will
            // join belongs to the GUI thread and is adopted there.
            {
                std::vector<Ppcp::LinkId> want;
                {
                    std::lock_guard<std::mutex> lk(m_wantChannelMutex);
                    want = m_wantChannel;
                }
                for (const Ppcp::LinkId &id : want) {
                    std::unique_ptr<TransportChannel> ch =
                        m_listener.acceptChannelFor(id, 1, nullptr);
                    if (!ch) continue;
                    auto *rawCh = ch.release();
                    const Ppcp::LinkId copy = id;
                    QMetaObject::invokeMethod(this, [this, copy, rawCh] {
                        adoptChannel(copy, rawCh);
                    }, Qt::QueuedConnection);
                }
            }

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
    startWired();

    m_listening = true;
    m_timer.start();
    setStatus(tr("Waiting for a device on port %1.").arg(m_port));
    emit stateChanged();
    return true;
}

void PpcpHostService::stop()
{
    m_timer.stop();
    stopWired();
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
bool PpcpHostService::configurePhonePeer(Phone *ph, std::string *err, bool listener)
{
    // ⚠ THE PEER IS BUILT HERE, NOT IN adoptLink(), SINCE 29 AUG 2026.  It moved
    // for one reason: `listener` (ENC 2.1a — which end dialled) is now a
    // parameter, and a flag that has to be set in one function and read in
    // another is a flag two callers will eventually disagree about.  `Config` is
    // fixed at construction, so the construction belongs where the flag arrives.
    PpcpHostPeer::Config pcfg;
    // The HOST's own id, identical on every conversation: it is this
    // application's identity, not this link's.
    pcfg.peerId = hostPeerId().toStdString();
    // ⛔ false ONLY on the wired path, and safe there ONLY because
    // `ppcp_peer_set_link_id()` is never called on this host — see
    // PpcpHostPeer::Config::listener.
    pcfg.listener = listener;
    ph->peer = std::make_unique<PpcpHostPeer>(pcfg);

    ph->peer->setDeclarationHook([this, ph](const ppcp_peer_desc *d) { onDeclare(ph, d); });
    ph->peer->setRelationsHook([this, ph](const PpcpLiveSession &) { onRelations(ph); });
    // 7.4b — every `heartbeat_ack` moves this phone's battery/thermal/storage
    // reading. `phones()` reads `peerHealth()` straight off the live session
    // rather than a copy kept here, so all this hook has to do is tell
    // Settings -> Phones (and the toolbar's aggregate) that it moved.
    // ⛔ phoneHealthChanged(), NOT phonesChanged().  A reading moving is not the
    // phone list changing, and conflating them rebuilt every row in Settings ->
    // Phones on every heartbeat — see the signal's declaration.
    ph->peer->liveSession().setHealthCallback(
        [this](const PpcpLiveSession::PeerHealth &) { emit phoneHealthChanged(); });
    // ── CR-02 — three more readings, on the same signal and for the same
    // reason.  ⛔ EVERY ONE OF THEM IS phoneHealthChanged(), NEVER
    // phonesChanged().  `device_status`, `buffer_status` and an actuator ack
    // are READINGS moving; they do not change which phones exist.  Trap 1 is
    // the mistake this codebase has already made once with the heartbeat and
    // once with `relation_update`, and it cost an operator the alias field they
    // were typing into.
    ph->peer->liveSession().setDeviceStatusCallback(
        [this, ph](const PpcpLiveSession::DeviceStatus &d) {
            // 5.5a is push-on-change, so this fires when something a person
            // would want to know about actually happened.
            if (!d.available)
                ppWarn() << "[ppcp]" << ph->name << "source" << d.sourceId.c_str()
                         << "unavailable —"
                         << (d.reason.empty() ? "no reason given" : d.reason.c_str());
            emit phoneHealthChanged();
        });
    ph->peer->liveSession().setBufferStatusCallback(
        [this](const PpcpLiveSession::BufferMargin &) { emit phoneHealthChanged(); });
    ph->peer->liveSession().setActuatorCallback(
        [this, ph](const PpcpLiveSession::ActuatorReading &r) {
            // 12.1b — a refusal is a word the DEVICE chose and this host renders
            // it verbatim rather than mapping it onto one it already knows.
            if (!r.refusedReason.empty())
                ppWarn() << "[ppcp]" << ph->name << "refused" << r.actuatorId.c_str()
                         << "—" << r.refusedReason.c_str();
            emit phoneHealthChanged();
        });
    // 5.2a — the device answered `arm`.  This is the half that turns "we sent a
    // message" into "the device says it is ready", and until it existed nothing
    // consumed `readiness` at all.
    ph->peer->liveSession().setReadinessCallback(
        [this, ph](const PpcpLiveSession::PeerReadiness &r) {
            if (!r.blockedReason.empty())
                ppWarn() << "[ppcp]" << ph->name << "cannot arm —"
                         << r.blockedReason.c_str();
            else if (!r.settled && r.hasEstimate)
                ppWarn() << "[ppcp]" << ph->name << "arming — ready in about"
                         << r.estimatedReadyMs << "ms";
            emit armStateChanged();
            emit phonesChanged();
            // ⭐ The torch for capture waits for exactly this: armed means the
            // camera is warm and the light can be commanded.
            reconcileTorch(ph, "phone armed");
        });
    // 8.2h — a Shot this host issued or adopted.  Handed on as a Qt signal so
    // that the shot pipeline's lifetime is Qt's problem and not ours; see the
    // note on `shotBridgeChanged` for why a stored callback into main()'s stack
    // would be a use-after-free waiting for `exit()`.
    ph->peer->shotBridge().setShotCallback([this](const ppcp_shot &s) {
        emit arbitratedShot(static_cast<qint64>(s.t0.ns),
                            QString::fromUtf8(s.id.v, static_cast<int>(s.id.len)));
    });

    ph->peer->addEventHook([this, ph](const ppcp_event &ev) {
        // ⚠ THE EVENT RING HAS EXACTLY ONE DRAINER AND IT IS PpcpHostPeer.
        // Everything else that needs to see events registers here.  In
        // particular `VideoInputPpcp::drainEvents()` MUST NOT be called on this
        // peer — it exists for the standalone paths where nothing else drains.
        if (m_offers) m_offers->observe(ph->counterpartId, ev);
        // MSG 9.1 — a Session this phone is REPLAYING onto the live link.
        //
        // ⚠ `ev.imported` IS THE WHOLE FILTER, AND IT IS LOAD-BEARING IN BOTH
        // DIRECTIONS.  True means the frame belongs to a stored Session being
        // replayed, which is this sink's business and must be kept away from
        // the live arbiter (E28, and `PpcpShotBridge::observe()` drops exactly
        // the same frames for the opposite reason).  False means the running
        // Session's own capture, which belongs to `VideoInputPpcp` and must NOT
        // be filed as an import of somebody's archive.
        if (ph->importSink && ev.imported) ph->importSink->observeEvent(ev);
        // Any VideoInputPpcp a caller has attached to this peer (Settings ->
        // Cameras' ROI preview, so far) — broadcast, not drained a second
        // time, for the same reason the line above isn't a second drain.
        VideoInputPpcp::dispatchEvent(ph->counterpartId, ev);
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
    PpcpSourceDeclaration::Inventory inv = PpcpSourceDeclaration::hostInventory();
    // The other half of the rename path — see Inventory::studioName. A phone
    // already paired learns the new name here rather than only from a code it
    // will never scan again.
    inv.studioName = studioName().toStdString();
    if (!ph->peer->declareSelf(inv, &derr))
        ppWarn() << "[ppcp] the host could not declare itself:" << derr.c_str();

    ph->engine = ph->peer->makeLibppcpEngine(&derr);
    if (!ph->engine) {
        if (err) *err = derr;
        return false;
    }

    // MSG 9.1's landing place, one per phone, sharing the host's single ledger.
    // Built here rather than lazily on the first offer, because the sink seeds
    // its I34 index from the ledger at construction and a Capture arriving
    // against an unseeded index would look new when we already hold it.
    {
        QString root = ppSettings().value(QStringLiteral("General/athleteLibraryPath"))
                           .toString();
        if (root.isEmpty())
            root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        Ppcp::PpcpImportSink::Config icfg;
        icfg.importRoot =
            QDir(root).filePath(QStringLiteral("PPCP Imports")).toStdString();
        ph->importSink = std::make_unique<Ppcp::PpcpImportSink>(
            m_importLedger, ph->engine->peer(), icfg);
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

Ppcp::PpcpHostPeer *PpcpHostService::peerForId(const QString &counterpartId)
{
    if (counterpartId.isEmpty()) return nullptr;
    for (const std::unique_ptr<Phone> &p : m_phones)
        if (p->counterpartId == counterpartId) return p->peer.get();
    return nullptr;
}

// ── CORE 5.14h / MSG 8.4a — the receiver says it holds the bytes ────────────
//
// ⚠ THIS IS THE HALF THAT HAD NEVER BEEN BUILT, AND ITS ABSENCE WAS THE DEVICE'S
// PROBLEM RATHER THAN OURS.  `PpcpImportSink` has always queued a commit when a
// payload landed durably, and nothing ever sent one.  8.4b forbids an owner
// setting `confirmed` on its own authority, so `capture_committed` is the ONLY
// route to it — and without it a phone can never release the storage for
// anything it has given us.  It shows up at the far end as a device that fills
// up and cannot explain why.
void PpcpHostService::flushOwedCommits(Phone *ph)
{
    if (!ph || !ph->engine || !ph->engine->peer() || ph->counterpartId.isEmpty()) return;

    // Owed to the MINTING peer (I34's second scope), which is not necessarily
    // whoever handed us the bytes: a phone may replay a Session another peer
    // recorded.  We can only pay a debt to the peer that is actually here, and
    // 5.14h is content for the rest to wait — a commit may go days later.
    const std::vector<Ppcp::PpcpImportLedger::PendingCommit> owed =
        m_importLedger.pendingCommits(ph->counterpartId.toStdString());
    if (owed.empty()) return;

    std::size_t sent = 0;
    for (const Ppcp::PpcpImportLedger::PendingCommit &pc : owed) {
        ppcp_digest d{};
        const bool haveDigest = Ppcp::digestFromHex(pc.digestHex, &d);
        const ppcp_result cr = ppcp_peer_capture_committed(ph->engine->peer(),
                                                           pc.key.captureId.c_str(),
                                                           haveDigest ? &d : nullptr);
        if (cr == PPCP_ERR_INVALID) {
            // ⛔ Not "queue full or link down": 8.4a's commit CARRIES the digest
            // and libppcp refuses one without it.  A ledger row that reached
            // here with no digest (the filer did not carry the announce's until
            // 1 Sept 2026) is a debt that can never be paid; retrying it every
            // 20 ms for the life of the link was measured that evening as a
            // log flood and nothing else.  Struck from the ledger, said once.
            ppWarn() << "[ppcp] capture_committed can never be sent for"
                     << pc.key.captureId.c_str() << "—" << ppcp_result_str(cr)
                     << (haveDigest ? "" : "(the ledger holds no digest for it)")
                     << "— struck from the owed list; the clip on disk is unaffected";
            m_importLedger.clearCommitted(pc.key);
            continue;
        }
        if (cr != PPCP_OK)
            break;   // queue full or link down — the debt stands, and we retry
        // ⚠ CLEARED ON QUEUE, WHICH IS EARLIER THAN THE LEDGER'S OWN COMMENT
        // ASKS FOR ("once the owner has had it").  There is no acknowledgement
        // for `capture_committed` in the protocol to wait on, so "the engine
        // accepted it for sending" is the strongest fact available here.  The
        // failure this leaves open — a link dying between queue and flush — is
        // recoverable, because a device that never saw the commit simply keeps
        // the bytes and offers the Session again.
        m_importLedger.clearCommitted(pc.key);
        ++sent;
    }
    if (sent) {
        m_importLedger.save();
        ppWarn() << "[ppcp] capture_committed x" << sent << "->" << ph->name
                 << "(" << m_importLedger.pendingCommitCount() << "still owed)";
    }
}

// ── CORE 7.3a / MSG 5.2 — arming, from the host ─────────────────────────────

bool PpcpHostService::armAll()
{
    bool all = true;
    for (const std::unique_ptr<Phone> &p : m_phones) {
        if (!p->peer) continue;
        std::string err;
        // MSG 5.2's empty list — every open capture Stream.
        if (!p->peer->liveSession().arm({}, &err)) {
            all = false;
            ppWarn() << "[ppcp] arm refused by" << p->name << "-" << err.c_str();
        } else {
            p->hostArmed = true;
        }
    }
    // The device's answer is a `readiness` that has not arrived yet, so the
    // state now is `Arming` for everything asked, and the screen says so rather
    // than going green on the strength of a queued message.
    emit armStateChanged();
    emit phonesChanged();
    if (all) setStatus(tr("Arming %n device(s)…", "", static_cast<int>(m_phones.size())));
    return all;
}

bool PpcpHostService::disarmAll()
{
    bool all = true;
    for (const std::unique_ptr<Phone> &p : m_phones) {
        if (!p->peer) continue;
        std::string err;
        if (!p->peer->liveSession().disarm({}, &err)) {
            all = false;
            ppWarn() << "[ppcp] disarm refused by" << p->name << "-" << err.c_str();
        } else {
            p->hostArmed = false;
        }
    }
    emit armStateChanged();
    emit phonesChanged();
    setStatus(tr("Disarmed."));
    return all;
}

// ── The session screen's capture intent, on every phone ────────────────────

void PpcpHostService::setCaptureWanted(bool on)
{
    if (m_captureWanted == on) return;
    m_captureWanted = on;
    ppWarn() << "[ppcp] capture" << (on ? "started" : "stopped") << "on the host ->"
             << (on ? "arm" : "disarm") << static_cast<int>(m_phones.size()) << "phone(s)";
    // ⚠ FORCED, not reconciled: a golfer who ended the session on the phone
    // itself leaves `hostArmed` true with nothing armed — the phone cannot say
    // it stopped (no `readiness` fits, see AppModel.disarm) — so the next
    // Capture must ask again.  The phone's arm is idempotent, so asking an
    // armed phone costs one message.
    for (const std::unique_ptr<Phone> &p : m_phones) {
        reconcileArm(p.get(), on ? "capture started" : "capture stopped", /*force=*/true);
        // On the way UP this waits for the phone's readiness (see
        // reconcileTorch); on the way DOWN the light goes out now.
        reconcileTorch(p.get(), on ? "capture started" : "capture stopped");
    }
    emit captureWantedChanged();
    // As armAll(): the answer is a `readiness` that has not arrived, so the
    // aggregate reads `arming` now and the screen says so.
    emit armStateChanged();
    emit phonesChanged();
}

void PpcpHostService::reconcileArm(Phone *ph, const char *why, bool force)
{
    if (!ph || !ph->peer) return;
    if (!force && ph->hostArmed == m_captureWanted) return;
    // ⚠ Before `session_open` the library refuses `arm` (it is Live-profile
    // session control), so a phone still in its hello/declare exchange is left
    // for onDeclare(), which calls back in here once the Session is open.
    if (!ph->peer->liveSession().isOpen()) return;
    std::string err;
    const bool ok = m_captureWanted ? ph->peer->liveSession().arm({}, &err)
                                    : ph->peer->liveSession().disarm({}, &err);
    if (!ok) {
        ppWarn() << "[ppcp]" << (m_captureWanted ? "arm" : "disarm") << "refused by"
                 << ph->name << "-" << err.c_str() << "(" << why << ")";
        return;
    }
    ph->hostArmed = m_captureWanted;
    ppWarn() << "[ppcp]" << (m_captureWanted ? "arm ->" : "disarm ->") << ph->name
             << "(" << why << ")";
}

void PpcpHostService::reconcileTorch(Phone *ph, const char *why)
{
    if (!ph || !ph->peer) return;
    const Phone::DeclaredActuator *torch = nullptr;
    for (const Phone::DeclaredActuator &a : ph->actuators)
        if (a.kind == QStringLiteral("torch")) { torch = &a; break; }
    if (!torch) return;

    const bool desired = m_captureWanted && torchDuringCaptureFor(ph->pairingId);
    const PpcpLiveSession &ls = ph->peer->liveSession();
    const PpcpLiveSession::ActuatorReading *r = ls.actuatorReading(torch->id.toStdString());
    if (desired) {
        // ⚠ Not before the phone says it is armed: `arm` is what warms its
        // camera, and a torch commanded on a cold camera answers `no_actuator`.
        // The readiness callback calls back in here when that changes.
        if (ls.armState() != PpcpLiveSession::ArmState::Armed) return;
        if (r && r->commandPending) return;
        if (r && r->valid && r->hasOn && r->on) { ph->torchSentOn = 1; return; }
        if (commandActuator(ph, torch->id, true)) {
            ph->torchSentOn = 1;
            ppWarn() << "[ppcp] torch on ->" << ph->name << "(" << why << ")";
        }
    } else {
        if (ph->torchSentOn != 1) return;   // not ours: we never lit it
        if (commandActuator(ph, torch->id, false)) {
            ph->torchSentOn = 0;
            ppWarn() << "[ppcp] torch off ->" << ph->name << "(" << why << ")";
        }
    }
}

bool PpcpHostService::torchDuringCaptureFor(const QString &pairingId)
{
    return ppSettings().value(QStringLiteral("ppcp/torchDuringCapture")).toMap()
               .value(pairingId).toBool();
}

void PpcpHostService::setPhoneTorchDuringCapture(const QString &pairingId, bool on)
{
    if (pairingId.isEmpty()) return;
    QSettings s = ppSettings();
    QVariantMap prefs = s.value(QStringLiteral("ppcp/torchDuringCapture")).toMap();
    if (prefs.value(pairingId).toBool() == on && (on || !prefs.contains(pairingId))) {
        // Unchanged — but a phone that is armed and unlit still gets reconciled
        // below, so a tap on a pill that already reads "on" retries a refusal.
    } else if (on) {
        prefs[pairingId] = true;
        s.setValue(QStringLiteral("ppcp/torchDuringCapture"), prefs);
    } else {
        prefs.remove(pairingId);
        s.setValue(QStringLiteral("ppcp/torchDuringCapture"), prefs);
    }
    for (const std::unique_ptr<Phone> &p : m_phones)
        if (p->pairingId == pairingId) reconcileTorch(p.get(), "torch setting changed");
    emit phonesChanged();
    emit phoneHealthChanged();
}

// ── MSG §12 / CR-02 — the torch ─────────────────────────────────────────────
//
// ⛔ WHAT `true` MEANS HERE.  The same thing `armAll()` returning true means:
// the command reached this peer's queue.  It is NOT "the torch is on".  The
// answer is an `actuator_command_ack`, which arrives later and which
// `PpcpLiveSession::observe()` is the only writer of the state this function's
// caller reads back.  Nothing below writes a lit state.
bool PpcpHostService::setPhoneActuator(const QString &pairingId, const QString &actuatorId,
                                       bool on)
{
    Phone *ph = phoneByPairing(pairingId);
    if (!ph || !ph->peer) {
        ppWarn() << "[ppcp] actuator command for a phone that is not connected:" << pairingId;
        return false;
    }
    return commandActuator(ph, actuatorId, on);
}

bool PpcpHostService::setActuatorForTest(std::size_t index, const QString &actuatorId, bool on)
{
    if (index >= m_phones.size()) return false;
    return commandActuator(m_phones[index].get(), actuatorId, on);
}

bool PpcpHostService::commandActuator(Phone *ph, const QString &actuatorId, bool on)
{
    if (!ph || !ph->peer) return false;
    // 12.1d, one layer above the library's own check: an Actuator this phone
    // never declared is not ours to command, and 5.19c makes an empty list a
    // perfectly ordinary declaration rather than a fault.
    bool declared = false;
    for (const Phone::DeclaredActuator &a : ph->actuators)
        if (a.id == actuatorId) { declared = true; break; }
    if (!declared) {
        ppWarn() << "[ppcp]" << ph->name << "declared no actuator" << actuatorId;
        return false;
    }
    std::string err;
    if (!ph->peer->liveSession().setActuator(actuatorId.toStdString(), on, &err)) {
        ppWarn() << "[ppcp] actuator command refused for" << ph->name << "-" << err.c_str();
        // Still a reading change: `commandPending` did not move, so a control
        // that had gone busy on an earlier attempt must be told to re-read.
        emit phoneHealthChanged();
        return false;
    }
    // ⛔ phoneHealthChanged(), NOT phonesChanged() — trap 1.  Asking is a
    // reading moving (`commandPending` went true); the phone list is unchanged.
    emit phoneHealthChanged();
    return true;
}

// One row per DECLARED Actuator, with what we actually know about it.
//
// ⭐ THE ONLY WRITERS OF `state` ARE THE ACK AND `actuator_state`.  Read the
// three cases below against `PpcpLiveSession::ActuatorReading`: `state` comes
// from `valid` + `on`, both of which `setActuator()` deliberately leaves alone.
// `pending` is the click, and it is published as a SEPARATE field precisely so
// a control can show "asking" without showing "on".
QVariantList PpcpHostService::actuatorRowsFor(const Phone *ph) const
{
    QVariantList out;
    if (!ph || !ph->peer) return out;
    const PpcpLiveSession &ls = ph->peer->liveSession();
    for (const Phone::DeclaredActuator &a : ph->actuators) {
        QVariantMap m;
        m[QStringLiteral("id")]      = a.id;
        m[QStringLiteral("kind")]    = a.kind;
        m[QStringLiteral("control")] = a.control;
        m[QStringLiteral("label")]   = a.label;
        // The capture setting the pill binds to — a preference, not a reading.
        m[QStringLiteral("duringCapture")] = (a.kind == QStringLiteral("torch"))
                                             ? torchDuringCaptureFor(ph->pairingId) : false;

        const PpcpLiveSession::ActuatorReading *r =
            ls.actuatorReading(a.id.toStdString());
        // "unknown" is a READING, not a stub: 12.2 is push, so an Actuator
        // nobody has commanded and that has not moved has told us nothing, and
        // that is a different answer from "off".  A control must render it as
        // an indeterminate state rather than as a dark bulb.
        QString state = QStringLiteral("unknown");
        if (r && r->valid && r->hasOn) state = r->on ? QStringLiteral("on")
                                                     : QStringLiteral("off");
        m[QStringLiteral("state")]   = state;
        m[QStringLiteral("level")]   = (r && r->valid && r->hasLevel) ? r->level : -1.0;
        m[QStringLiteral("pending")] = r ? r->commandPending : false;
        // H18 — OUR conclusion that a command went unanswered past
        // `kActuatorStallNs`, kept apart from `refusedReason` (which is a word
        // the DEVICE chose).  MSG 1c makes answering a MUST, so this is a
        // nonconformant peer and not a busy one; `pending` has already gone
        // false so the control stops saying "asking", and `state` stays at
        // whatever we last actually knew.
        m[QStringLiteral("stalled")] = r ? r->commandStalled : false;
        // 12.1b's open registry, verbatim.  Empty where the last command was
        // applied or none has been sent.
        m[QStringLiteral("refusedReason")] =
            r ? QString::fromStdString(r->refusedReason) : QString();
        out.append(m);
    }
    return out;
}

QString PpcpHostService::armState() const
{
    // The LEAST ready wins, so a bay with one blocked phone does not read as
    // ready because the other one is.  Blocked outranks arming outranks armed.
    using AS = Ppcp::PpcpLiveSession::ArmState;
    bool any = false, anyBlocked = false, anyStalled = false, anyArming = false,
         anyDisarmed = false;
    for (const std::unique_ptr<Phone> &p : m_phones) {
        if (!p->peer) continue;
        any = true;
        switch (p->peer->liveSession().armState()) {
        case AS::Blocked:  anyBlocked = true;  break;
        case AS::Stalled:  anyStalled = true;  break;
        case AS::Arming:   anyArming = true;   break;
        case AS::Disarmed: anyDisarmed = true; break;
        case AS::Armed:    break;
        }
    }
    if (!any) return {};
    if (anyBlocked)  return QStringLiteral("blocked");
    if (anyStalled)  return QStringLiteral("stalled");
    if (anyArming)   return QStringLiteral("arming");
    if (anyDisarmed) return QStringLiteral("disarmed");
    return QStringLiteral("armed");
}

namespace {
// This file spells ppcp_id out inline everywhere else; one helper for the block
// below, which reaches for four of them.
QString qid(const ppcp_id &id) { return QString::fromUtf8(id.v, static_cast<int>(id.len)); }
}  // namespace

QString PpcpHostService::streamAliasFor(const QString &peerId, const QString &sourceId,
                                       const QString &label)
{
    // The alias becomes a `streams[]` key AND a filename stem, so it has to be
    // filesystem-safe, stable for the life of the pairing, and unique within one
    // swing -- two phones may both declare "iPhone 16 - Wide".
    QString stem = label.trimmed();
    if (stem.isEmpty()) stem = sourceId;
    QString safe;
    safe.reserve(stem.size());
    for (const QChar c : stem) {
        if (c.isLetterOrNumber())      safe.append(c.toLower());
        else if (!safe.endsWith(QLatin1Char('-'))) safe.append(QLatin1Char('-'));
    }
    while (safe.endsWith(QLatin1Char('-'))) safe.chop(1);
    while (safe.startsWith(QLatin1Char('-'))) safe.remove(0, 1);
    if (safe.isEmpty()) safe = QStringLiteral("phone");

    // Short digest of the identity, not of the label: two lenses on one phone
    // differ by Source, and two phones differ by Peer.
    const QByteArray h = QCryptographicHash::hash(
        (peerId + QLatin1Char('\x1f') + sourceId).toUtf8(), QCryptographicHash::Sha1);
    return safe + QLatin1Char('-') + QString::fromLatin1(h.toHex().left(8));
}

int PpcpHostService::requestCaptureForShot(const QString &shotId, qint64 t0HostNs)
{
    if (shotId.isEmpty()) return 0;

    // The interval asked for around `t0`.  A golf swing needs the backswing, so
    // the pre-roll is the long side; the host's own window is 4 s wide
    // (kWindowDuration) and this sits comfortably inside it.
    // ⚠ OVERRIDABLE, BECAUSE THE RIGHT WINDOW IS AN OPEN QUESTION AND WE ARE
    // MEASURING IT.  Design §9.4: asking for more than the device retains is
    // answered `outside_buffer` for the WHOLE clip, not a partial — so a window
    // chosen blind can look exactly like a device with no footage.
    //
    // Observed 1 Sept: pre 2000 ms was refused `outside_buffer`, and 491 ms
    // later the same ring produced a 12.3 MB clip for the phone's own shot. The
    // footage was there; the interval we named was not.
    //
    // ⛔ And the clamp below cannot help yet: this phone sends NO `buffer_status`
    // at all (zero in a full run), so there is no declared retention target to
    // clamp against and nothing tells us the ring's depth.
    static const qint64 kPreNs = qEnvironmentVariableIntValue("PINPOINT_PPCP_PRE_MS") > 0
        ? qint64(qEnvironmentVariableIntValue("PINPOINT_PPCP_PRE_MS")) * 1000 * 1000
        : 2000LL * 1000 * 1000;                        // 2.0 s before t0
    static const qint64 kPostNs = qEnvironmentVariableIntValue("PINPOINT_PPCP_POST_MS") > 0
        ? qint64(qEnvironmentVariableIntValue("PINPOINT_PPCP_POST_MS")) * 1000 * 1000
        : 1000LL * 1000 * 1000;                        // 1.0 s after t0

    int asked = 0;
    for (const std::unique_ptr<Phone> &ph : m_phones) {
        if (!ph || !ph->peer || !ph->engine) continue;
        ppcp_peer *peer = ph->engine->peer();
        if (!peer) continue;

        // ⚠ ONLY STREAMS THAT ARE ACTUALLY OPEN, AND ASKED OF THE LIBRARY RATHER
        // THAN RECONSTRUCTED.  A hand-built id would have to reproduce
        // `VideoInputPpcp::streamIdFor()`'s hash, and a stale comment two files
        // away still shows the pre-hash form -- so this reads the peer's own
        // Stream table instead.  5.21c confines a ring buffer, and therefore a
        // clip, to a `shot_windowed` Stream; a preview Stream retains nothing.
        std::vector<std::string> ids;
        std::vector<QString>     sourceIds;
        const std::size_t n = ppcp_peer_stream_count(peer);
        for (std::size_t i = 0; i < n; ++i) {
            const ppcp_stream *st = ppcp_peer_stream_at(peer, i);
            if (!st) continue;
            if (st->continuity != PPCP_SHOT_WINDOWED) continue;
            if (st->has_closed_at) continue;
            ids.push_back(qid(st->id).toStdString());
            sourceIds.push_back(qid(st->source_id));
        }
        if (ids.empty()) {
            // Not a fault: the phone is connected and previewing, but no capture
            // Stream is open, so there is nothing retaining an interval to ask
            // for.  Said once per shot, because silence here reads as success.
            // ⚠ WITH THE TABLE, because "none open" has meant "the phone never
            // opened them" and "they were opened and closed again" on the same
            // evening (1 Sept, cable run), and only the rows tell those apart.
            QString table;
            for (std::size_t i = 0; i < n; ++i) {
                const ppcp_stream *st = ppcp_peer_stream_at(peer, i);
                if (!st) continue;
                table += QStringLiteral(" %1[%2,%3%4]")
                             .arg(qid(st->id), qid(st->kind),
                                  st->continuity == PPCP_SHOT_WINDOWED
                                      ? QStringLiteral("shot_windowed")
                                      : QStringLiteral("continuous"),
                                  st->has_closed_at ? QStringLiteral(",closed") : QString());
            }
            ppWarn() << "[ppcp] no shot_windowed Stream open on" << ph->name
                     << "— no clip asked for shot" << shotId << "— the peer's" << n
                     << "Stream(s):" << (table.isEmpty() ? QStringLiteral("(none)") : table);
            continue;
        }

        // 5.21 / E60 — clamp the pre-roll to what the device says it AIMS to
        // keep, where it has said.  `retention_target_ns` is a DURATION, so this
        // needs no timebase conversion and cannot get a sign wrong.
        // ⚠ `retained_from` is an Instant in the DEVICE's timebase and is
        // deliberately NOT used here: converting it means evaluating a relation,
        // and a relation evaluated outside its own domain fabricated a ~460 ms
        // sigma against a real phone in August.  Asking for slightly too much is
        // answered by `absent`, which is a first-class answer (I10); asking for
        // the wrong interval is not.
        qint64 preNs = kPreNs;
        for (const Ppcp::PpcpLiveSession::BufferMargin &b : ph->peer->liveSession().bufferMargins()) {
            if (!b.hasRetentionTarget || b.retentionTargetNs <= 0) continue;
            if (std::find(ids.begin(), ids.end(), b.streamId) == ids.end()) continue;
            preNs = std::min(preNs, b.retentionTargetNs);
        }

        std::string err;
        if (!ph->peer->shotBridge().requestCapture(shotId.toStdString(), t0HostNs,
                                                   ids, preNs, kPostNs, &err)) {
            ppWarn() << "[ppcp] capture_request FAILED for" << ph->name
                     << "shot" << shotId << "—" << QString::fromStdString(err);
            continue;
        }

        const ppcp_peer_desc *desc = ppcp_peer_counterpart(peer);
        for (std::size_t i = 0; i < ids.size(); ++i) {
            QString label;
            if (desc) {
                for (std::size_t j = 0; j < desc->source_count; ++j) {
                    if (qid(desc->sources[j].id) != sourceIds[i]) continue;
                    if (desc->sources[j].has_label) label = qid(desc->sources[j].label);
                    break;
                }
            }
            emit captureAsked(shotId, ph->counterpartId, sourceIds[i],
                              QString::fromStdString(ids[i]),
                              streamAliasFor(ph->counterpartId, sourceIds[i], label));
            ++asked;
        }
        ppInfo() << "[ppcp] capture_request →" << ph->name << "for" << ids.size()
                 << "Stream(s), shot" << shotId << "pre_ms" << (preNs / 1000000)
                 << "post_ms" << (kPostNs / 1000000);
    }
    return asked;
}

void PpcpHostService::flushOwedCommitsNow()
{
    for (const std::unique_ptr<Phone> &p : m_phones)
        if (p && p->engine && p->peer) flushOwedCommits(p.get());
}

std::vector<VideoInputPpcp *> PpcpHostService::previewConsumers() const
{
    std::vector<VideoInputPpcp *> all;
    for (const std::unique_ptr<Phone> &p : m_phones)
        if (p) all.insert(all.end(), p->previews.begin(), p->previews.end());
    return all;
}

Ppcp::PpcpShotBridge *PpcpHostService::activeShotBridge()
{
    // First one with a running arbiter — see the header for why "first" and not
    // "the" is a limitation this code cannot fix on its own.
    for (const std::unique_ptr<Phone> &p : m_phones) {
        if (p->peer && p->peer->shotBridge().active()) return &p->peer->shotBridge();
    }
    return nullptr;
}

QString PpcpHostService::hostMicrophoneSourceId() const
{
    // Every phone holds its own copy of this host's declaration (MSG 3.3c makes
    // declaring a per-conversation obligation), so any connected phone answers.
    // They can differ — two phones that connected either side of a microphone
    // being plugged in hold different declarations — and the FIRST is the right
    // answer because it is the one whose bridge `activeShotBridge()` returns.
    for (const std::unique_ptr<Phone> &p : m_phones) {
        if (!p->peer) continue;
        const std::string id = p->peer->declaration().sourceIdOfKind("microphone");
        if (!id.empty()) return QString::fromStdString(id);
    }
    return {};
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

void PpcpHostService::adoptLink(std::unique_ptr<PeerConnection> link,
                                const QString &resolvedPairingId)
{
    if (!link) return;

    // A phone got in, so whatever failed before it is history.  The panel must
    // not carry a refusal underneath a live connection.
    clearFailure();

    // ── Contract C2, rule 1 — where the pairing comes from ─────────────────
    //
    // ⛔ `link->pairingId()` IS EMPTY WHENEVER THIS HOST DIALLED.  It is read
    // off the control channel's resolution, and only a LISTENER resolves.  On
    // the cable RV 2d inverts and we are the client, so the pairing is the one
    // we resolved from the presence record before dialling (design §5.2) and it
    // arrives as a parameter.
    const bool weDialled = !resolvedPairingId.isEmpty();
    const QString pairingId =
        weDialled ? resolvedPairingId : QString::fromStdString(link->pairingId());

    // ── Contract C2, rule 2 — and this is the one that is easy to get wrong ─
    //
    // RV 7.3a — the code is spent by the PAIRING, not by the handshake.  This
    // link is two TLS handshakes (three with preview) over one K_tls; counting
    // handshakes would invalidate a `mu: 1` code on the control channel and
    // refuse the bulk channel of the SAME link.  See F-H6-1.
    //
    // ⛔ AND IT RUNS ONLY WHEN WE DID NOT DIAL.  7.3a's single-use accounting is
    // about spending a PAIRING CODE.  A wired reconnection resolves against a
    // pairing this host already holds and persisted; it spends nothing, and
    // decrementing here would burn a code a phone never used.
    if (!weDialled && !pairingId.isEmpty())
        m_rv.noteLinkEstablished(pairingId.toStdString());

    // ── Contract C2, rule 3 — identical on both paths, on purpose ──────────
    //
    // The Settings→Phones row must look the same whichever way the link was
    // made, so `m_pairedThisRun` and the advertised set are updated either way.
    if (!pairingId.isEmpty()) m_pairedThisRun.insert(pairingId);
    // E57 — noteLinkEstablished() just remembered this pairing automatically
    // (a `mu: 1` code, which is the only kind this host publishes), so the
    // advertised set (3.5e) may have grown right here rather than only when a
    // user pressed "Remember".
    refreshAdvertisement();

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
    ph->wired     = weDialled;

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
    // ⛔ Contract C6 — the engine is a LISTENER on every path but the cable.
    if (!configurePhonePeer(ph, &derr, /*listener=*/!weDialled)) {
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

    // ── Contract C6, step 4 — the wired path says hello ────────────────────
    //
    // RV 2d inverts on the cable: the host dialled, so the host greets.  On the
    // WiFi path the phone dialled and sent `hello`, and this host answers it —
    // calling it here too would be a second greeting on a conversation that has
    // already started, which is why this is gated and not unconditional.
    //
    // ⛔ AND NOTHING CALLS `ppcp_peer_set_link_id()`.  `ppcp_peer_hello()`
    // auto-emits `link_bind` at `ppcp_peer.c:929` when `!listener &&
    // has_link_id`, and `Connector::connect()` has ALREADY written `link_bind`
    // on every stream of this link.  `listener = false` is safe only because
    // `has_link_id` stays false; set both and this host sends two bindings on
    // channel 0.
    //
    // ✅ Nothing downstream of this changes.  A responder raises HELLO *and*
    // CONNECTED (`ppcp_peer.c:2867`), so the declare-on-CONNECTED at
    // `ppcp_host_peer.cpp:204` fires identically whichever end dialled.
    if (weDialled && ph->engine && ph->engine->peer()) {
        const ppcp_result hr = ppcp_peer_hello(ph->engine->peer());
        if (hr != PPCP_OK)
            ppWarn() << "[ppcp-usb] hello was refused on the wired link (rc" << int(hr) << ")";
    }

    // ENC 2.1d — this link has its two required channels; a `preview` one may
    // follow at any later point in the session, and until this call nothing in
    // the application was listening for it.
    //
    // ⚠ UNLESS IT IS ALREADY HERE, which is the cable.  There the host is the
    // dialler, so `PpcpWiredLink` opened the third channel itself before handing
    // the link over (the phone cannot dial us over usbmux).  Asking the accept
    // thread to watch for one as well would leave it polling `acceptChannelFor()`
    // for a stream that can never arrive, for the life of the link.
    if (!ph->link->channel(Ppcp::Channel::Preview))
        noteWantsChannel(ph->link->linkId(), true);
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
    // ⚠ THE PAIRING IS NAMED HERE, AND ITS ABSENCE IS NAMED LOUDLY.  A link is
    // adopted on the strength of its handshake; the pairing id is what joins it
    // to the rendezvous ledger, and EVERYTHING that ledger drives is keyed on
    // it — `phoneByPairing()` (so the Settings -> Phones row's status),
    // `noteLinkEstablished()` (so remembering, 7.4b), `notePeerName()` (so the
    // device's name) and `m_pairedThisRun`.  A link adopted without one is
    // live, carries video, and is invisible to every one of those: the home
    // screen says connected while Settings -> Phones says disconnected, which
    // is precisely the report this line exists to stop being a mystery.
    ppWarn() << "[ppcp-rv] link up:" << tls.describe().c_str()
             // Design §6.1 — "surface which path a phone is on ... an operator
             // who cannot see that the cable did nothing cannot act on it".
             // The log half of that; the Settings→Phones half is Phase 2.
             << (weDialled ? "transport=usb" : "transport=wifi")
             << (pairingId.isEmpty()
                     ? QStringLiteral("pairing=NONE — not joined to the ledger, so no row "
                                      "will show it connected and nothing will be remembered")
                     : QStringLiteral("pairing=%1").arg(pairingId));
    setStatus(m_phones.size() == 1
                  ? tr("Device connected — %1.").arg(QString::fromStdString(tls.describe()))
                  : tr("%1 devices connected.").arg(m_phones.size()));
    refreshCode();
    watchChannels(ph);
    emit stateChanged();
    emit phonesChanged();
}

void PpcpHostService::adoptLinkForTest(std::unique_ptr<PeerConnection> link,
                                      const QString &resolvedPairingId)
{
    adoptLink(std::move(link), resolvedPairingId);
}

Ppcp::PpcpLiveSession *PpcpHostService::liveSessionForTest(std::size_t index)
{
    if (index >= m_phones.size() || !m_phones[index]->peer) return nullptr;
    return &m_phones[index]->peer->liveSession();
}

bool PpcpHostService::declareForTest(std::size_t index, const QString &counterpartId,
                                    const QString &actuatorKind,
                                    const QString &actuatorControl)
{
    if (index >= m_phones.size()) return false;
    ppcp_peer_desc desc{};
    const QByteArray id = counterpartId.toUtf8();
    if (ppcp_id_set(&desc.id, id.constData(), static_cast<std::size_t>(id.size())) != PPCP_OK)
        return false;
    ppcp_id_set_z(&desc.product.model, "test phone");
    // CORE 5.19c / erratum E66 — a top-level sibling of `sources`, and omitted
    // entirely by a peer owning none.  ⚠ `act` must outlive the onDeclare()
    // call below: `ppcp_peer_desc` borrows, and the copy out is what makes that
    // safe for the caller.
    ppcp_actuator act{};
    if (!actuatorKind.isEmpty()) {
        const QByteArray kind = actuatorKind.toUtf8();
        const QByteArray control = actuatorControl.isEmpty()
                                       ? QByteArray(PPCP_ACTUATOR_CONTROL_ON_OFF)
                                       : actuatorControl.toUtf8();
        if (ppcp_actuator_make(&act, "act:test", id.constData(), kind.constData(),
                               control.constData()) != PPCP_OK)
            return false;
        if (ppcp_peer_desc_set_actuators(&desc, &act, 1) != PPCP_OK) return false;
    }
    onDeclare(m_phones[index].get(), &desc);
    return true;
}

void PpcpHostService::dropPhone(Phone *ph, const char *why)
{
    if (!ph) return;
    // ⭐ SAY WHY, not just that.  `why` is this call site's word for it and is
    // the same string whether the peer left politely or the cable was pulled;
    // the transport's own verdict is the half that actually diagnoses anything,
    // so it is read here while the peer still exists to be asked.
    if (ph->peer) {
        const std::string cause = ph->peer->stats().closeCause;
        if (!cause.empty()) {
            const auto &st = ph->peer->stats();
            // ⭐ The per-channel traffic beside the cause.  A channel that died
            // having moved nothing is a very different fault from one that died
            // mid-transfer, and "link closed" said neither.
            ppWarn() << "[ppcp] link ended for" << ph->name << "—"
                     << QString::fromStdString(cause)
                     << QStringLiteral("| bytes in/out — control %1/%2  bulk %3/%4  preview %5/%6")
                            .arg(st.bytesInCh[0]).arg(st.bytesOutCh[0])
                            .arg(st.bytesInCh[1]).arg(st.bytesOutCh[1])
                            .arg(st.bytesInCh[2]).arg(st.bytesOutCh[2]);
        }
    }
    // Find it before anything is torn down, so the erase below cannot be
    // reading a half-destroyed entry.
    auto it = std::find_if(m_phones.begin(), m_phones.end(),
                           [ph](const std::unique_ptr<Phone> &p) { return p.get() == ph; });
    if (it == m_phones.end()) return;

    // The offer list is attached PER PEER, so only this phone's offers go.
    if (m_offers && !ph->counterpartId.isEmpty()) m_offers->detach(ph->counterpartId);

    // ⛔ **OUR OWN PREVIEW CONSUMERS GO FIRST, WHILE THE PEER IS STILL ALIVE.**
    // stop() closes the preview Stream, which is a wire message and needs a
    // live peer to carry it (5.1a1: say why).  Deleted rather than detached
    // because unlike a Settings tile — which outlives the phone and is
    // re-attached by reattachAll() on reconnect — these are owned here and a
    // fresh `declare` builds a fresh set.
    // ⛔ **THE NOTIFIERS GO BEFORE ANYTHING CAN CLOSE AN fd.**  Same trap as the
    // DNS-SD socket in startAdvertising(): a QSocketNotifier left watching a
    // closed descriptor fires forever on some platforms and asserts on others.
    ph->reads.clear();

    VideoInputPpcp::stopPreviewConsumers(&ph->previews);

    if (!ph->counterpartId.isEmpty()) {
        VideoInputPpcp::clearTimebaseMappings(ph->counterpartId);
        // Every VideoInputPpcp bound to this peer stops pointing at it BEFORE
        // the peer itself goes — see the header note on the same call in
        // registerDevice()'s absence: there is no "unregister" for a camera
        // row, so a preview instance can outlive the phone that owns it, and
        // must not be left holding a pointer this function is about to
        // invalidate.
        VideoInputPpcp::detachAll(ph->counterpartId);
    }
    // MSG 9.1 — whatever this phone replayed is closed out and the ledger put
    // on disk, BEFORE the peer the sink points at goes.  A replay cut short by
    // a dying link leaves its Session open in the ledger rather than being
    // recorded as truncated, which is the honest reading: we did not observe an
    // end, and ENC 7d only ever permits a downgrade on evidence.
    if (ph->importSink) {
        const Ppcp::PpcpImportSink::Stats &is = ph->importSink->stats();
        if (is.captures || is.clipsWritten)
            ppWarn() << "[ppcp] import from" << ph->name << "— captures" << is.captures
                     << "new" << is.capturesNew << "already held" << is.capturesAlreadyHeld
                     << "clips" << is.clipsWritten << "bytes" << is.clipBytes
                     << "commits queued" << is.commitsQueued;
        ph->importSink->finishLive();
        ph->importSink.reset();
        m_importLedger.save();
    }

    // The arbiter goes before the Session it arbitrates, and its numbers are
    // read BEFORE it goes — stop() frees the arbiter, and `attach(nullptr, ...)`
    // below stops it a second time.  A Session that saw nothing arbitrated is a
    // different fact from one that was never listening, and this is the only
    // place either can still be told.
    {
        const Ppcp::PpcpShotBridge::Stats &st = ph->peer->shotBridge().stats();
        if (st.nominated || st.observedForeign || st.issued || st.unarbitrated)
            ppWarn() << "[ppcp] arbitration for" << ph->name
                     << "— nominated" << st.nominated
                     << "observed" << st.observedForeign
                     << "issued" << st.issued
                     << "adopted" << st.adopted
                     << "excluded" << st.excluded
                     << "unarbitrated" << st.unarbitrated;
        ph->peer->shotBridge().stop();
    }
    // ⚠ THE SHOT PIPELINE LETS GO HERE — BEFORE THE ERASE, AND THAT ORDERING IS
    // THE WHOLE POINT.  This emit used to sit at the end of the function, after
    // `m_phones.erase()` had destroyed the Phone, its PpcpHostPeer and the
    // bridge inside it.  `ShotController` holds that bridge as a RAW POINTER, so
    // by the time the slot ran to replace it, the pointer it still held was
    // dangling — and a slot that so much as READ the outgoing bridge crashed on
    // freed memory.  It did, on a range, on 27 Aug.
    //
    // Emitted here instead, the handover happens while every object involved is
    // still alive: `stop()` above has already cleared this bridge's arbiter, so
    // `activeShotBridge()` skips it and answers with another phone's bridge or
    // null.  Nobody is ever handed a pointer to something that has gone.
    emit shotBridgeChanged();
    // MSG 4.4 — the Session closes with the link, whether or not it was ever
    // used for anything.  Best-effort: a peer already gone cannot be told, and
    // that is not a reason to skip the local half of closing it.
    {
        std::string cerr;
        ph->peer->liveSession().close("disconnected", &cerr);
    }
    ph->peer->attach(nullptr, nullptr);
    // Stop the accept thread polling for a third channel on a link that is
    // about to stop existing.
    if (ph->link) noteWantsChannel(ph->link->linkId(), false);
    if (ph->link) ph->link->close();

    const QString name = ph->name;
    const QString droppedPairing = ph->pairingId;
    m_phones.erase(it);

    // The other half of "link up": a reconnect is a DROP and an adopt, and
    // without both in the log a phone that reconnects twice reads as a phone
    // that connected three times — which is how a stale link left in `m_phones`
    // would hide.  `adoptLink()` deliberately does not deduplicate by pairing
    // (a pairing is not a phone; a LINK is), so the count is worth stating.
    ppWarn() << "[ppcp-rv] link down:" << why
             << (droppedPairing.isEmpty() ? QStringLiteral("pairing=NONE")
                                          : QStringLiteral("pairing=%1").arg(droppedPairing))
             << "-" << m_phones.size() << "link(s) still up";

    setStatus(name.isEmpty()
                  ? tr("Device disconnected (%1).").arg(QString::fromLatin1(why))
                  : tr("%1 disconnected (%2).").arg(name, QString::fromLatin1(why)));
    emit stateChanged();
    // The phone did not stop existing, it stopped being here — its row stays
    // and changes state, the way a switched-off IMU's does.
    emit phonesChanged();
    // The toolbar's battery/thermal/sigma aggregates are over CONNECTED phones
    // and notify off phoneHealthChanged(), so a phone leaving has to move them
    // too — otherwise the pill keeps showing a departed phone's last reading.
    emit phoneHealthChanged();

    // ⛔ A LINK ENDING RE-ARMS THE CABLE, and without this the wired path is a
    // one-shot per attachment.  A phone still plugged in whose link just died —
    // the app was backgrounded, WiFi dropped it, the operator restarted it — is
    // exactly the phone that should be re-probed, and it produces no usbmux
    // `Attached` event to trigger one.  §6.1 rule 1 still guards the dial, so a
    // phone that already has another live link is not double-connected.
    if (m_wired) m_wired->retryNow();
}

void PpcpHostService::noteWantsChannel(const Ppcp::LinkId &id, bool wants)
{
    std::lock_guard<std::mutex> lk(m_wantChannelMutex);
    auto it = std::find(m_wantChannel.begin(), m_wantChannel.end(), id);
    if (wants) {
        if (it == m_wantChannel.end()) m_wantChannel.push_back(id);
    } else if (it != m_wantChannel.end()) {
        m_wantChannel.erase(it);
    }
}

// ⛔ ONE READ NOTIFIER PER CHANNEL — see Phone::reads.  Rebuilt wholesale rather
// than appended to, because ENC 2.1d lets a channel arrive at any later point and
// a set that is only ever added to has no answer for a channel that closed.
//
// ⚠ `pump()`, NOT `tick()`.  tick() is the schedule half — 6.3's sync cadence,
// 7.4's heartbeats, 8.2h's issue hold — and it is driven by TIME PASSING, so it
// must keep running on the timer whether or not bytes arrive.  Calling it from a
// readable socket would run those schedules at the rate of the traffic.
void PpcpHostService::watchChannels(Phone *ph)
{
    if (!ph || !ph->link || !ph->peer) return;
    ph->reads.clear();
    for (const Channel c : ph->link->channels()) {
        TransportChannel *tc = ph->link->channel(c);
        if (!tc || !tc->isOpen()) continue;
        const int fd = tc->fd();
        if (fd < 0) continue;
        auto n = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
        connect(n.get(), &QSocketNotifier::activated, this, [this, ph] {
            // Re-entrancy: a preview consumer downstream of drainEvents() can
            // spin the event loop, and a nested pump would feed the engine from
            // a buffer the outer call still owns.
            if (ph->pumping) return;
            ph->pumping = true;
            const bool alive = ph->peer->pump();
            ph->pumping = false;
            // ⚠ Drop on the same rule tick() uses.  A dead link discovered here
            // must not be left for the timer: its notifiers are still armed on
            // a closed fd, which is the trap dropPhone() exists to avoid.
            if (!alive) dropPhone(ph, "link closed");
        });
        ph->reads.push_back(std::move(n));
    }
}

void PpcpHostService::adoptChannel(const Ppcp::LinkId &id, Ppcp::TransportChannel *raw)
{
    std::unique_ptr<TransportChannel> ch(raw);
    // The phone may have gone between the accept thread collecting this and the
    // GUI thread getting here.  Dropping the channel is then correct and is not
    // an error: the link it wanted to join no longer exists.
    for (const std::unique_ptr<Phone> &p : m_phones) {
        if (!p->link || p->link->linkId() != id) continue;
        if (p->link->adopt(std::move(ch))) {
            ppWarn() << "[ppcp] ENC 2.1d — a third channel joined" << p->name
                     << "(" << static_cast<int>(p->link->channels().size()) << "channels)";
            // One extra channel is all 2.1d's preview case wants; asking for
            // more would keep a poll running on the accept thread for ever.
            noteWantsChannel(id, false);
            watchChannels(p.get());   // the new channel needs its own notifier
        }
        return;
    }
}

void PpcpHostService::dropAllPhones(const char *why)
{
    while (!m_phones.empty()) dropPhone(m_phones.back().get(), why);
}

void PpcpHostService::onDeclare(Phone *ph, const ppcp_peer_desc *desc)
{
    if (!ph || !desc) return;

    // ── §6.1's BACKSTOP — one phone, one link, whatever route it took ───────
    //
    // ⛔ THIS RUNS BEFORE `registerPpcpPeer()` AND BEFORE `counterpartId` IS
    // ASSIGNED, AND BOTH ORDERINGS ARE LOAD-BEARING.  Everything downstream of
    // a declaration is keyed on the COUNTERPART, not on the Phone — the camera
    // registry, the timebase mappings, the offer list — and a duplicate link
    // shares its counterpart id with the incumbent by definition.  Register the
    // newcomer's cameras first and `dropPhone()` would then call
    // `detachAll(counterpartId)` and tear out the INCUMBENT's cameras while
    // closing the duplicate: the backstop would cause the outage it exists to
    // prevent.  Leaving `ph->counterpartId` empty is what makes the close inert
    // — every counterpart-keyed teardown in dropPhone() is guarded on it.
    //
    // The wired takeover handles the ordinary collision: the cable dials, the
    // WiFi link is dropped, one link remains.  It does not cover the PHONE
    // originating a second link anyway — from a scanned pairing code, or an
    // endpoint carried in one (`RV` 4.3d).  Its own reconnect will not do that
    // (it dials only when no link is up), so this takes a deliberate human
    // action, which is exactly what someone troubleshooting a connection does.
    //
    // ⛔ The cost of missing it is silent wrong data rather than a visible
    // error: two `Phone` rows, two sets of preview consumers, and ONE PHONE'S
    // CANDIDATES ENTERING THE ARBITER TWICE.  `adoptLink()` deliberately does
    // not deduplicate by pairing — with `mu > 1` several devices legitimately
    // share one — so nothing upstream of here can catch it.
    //
    // ⛔ KEYED ON THE COUNTERPART, NEVER ON THE PAIRING.  `Peer.id` is the phone
    // (`CORE` 5.1a, stable for the entity's lifetime); a pairing is not a phone.
    //
    // ⚠ IT CANNOT BE DONE EARLIER THAN THIS FUNCTION.  The counterpart id is
    // only known here, from `desc->id` — after both TLS handshakes and the
    // `hello` exchange.  Two wasted handshakes is the price of a backstop.
    //
    // ⚠ KEEP THE INCUMBENT, ALWAYS, whichever transport it is on: it holds the
    // sync history, and a rule with no comparison in it cannot oscillate.
    // ✅ It does not fight the wired takeover, which drops the WiFi phone BEFORE
    // adopting the cable link — by the time the newcomer declares there is no
    // incumbent left to keep.
    const QString incomingId =
        QString::fromUtf8(desc->id.v, static_cast<int>(desc->id.len));
    for (const std::unique_ptr<Phone> &other : m_phones) {
        if (other.get() == ph) continue;
        if (other->counterpartId.isEmpty() || other->counterpartId != incomingId) continue;

        ppWarn() << "[ppcp] this phone already has a link — keeping the one it has "
                    "and closing the newcomer. One phone, one link (design §6.1)";
        // ⚠ Deferred: we are inside this peer's own event drain
        // (drainEvents() -> onDeclare), and destroying the Phone that owns the
        // engine being drained would pull the ground from under the caller.
        Phone *doomed = ph;
        QMetaObject::invokeMethod(
            this, [this, doomed] {
                for (const std::unique_ptr<Phone> &p : m_phones)
                    if (p.get() == doomed) { dropPhone(doomed, "duplicate link"); return; }
            },
            Qt::QueuedConnection);
        return;   // ⛔ nothing below may run for a link we are closing
    }

    // MSG 3.3 — a peer's cameras exist the moment it declares and at no other
    // moment.  There is no bus to walk and no scan to run: this is the whole of
    // PPCP camera discovery, and it is why `VideoInputFactory::enumerateDevices()`
    // has never had a PPCP branch.
    const int n = VideoInputFactory::registerPpcpPeer(desc);

    ph->counterpartId = incomingId;

    // CORE 5.19c / erratum E66 — `actuators` travels as a TOP-LEVEL key of
    // `declare`, a sibling of `sources`, and libppcp has already parsed it into
    // `desc->actuators`.  Copied out here because `desc` is borrowed: its
    // strings point into the decode arena and are gone the moment this returns.
    //
    // ⚠ A FRESH DECLARATION REPLACES THE LIST WHOLESALE.  A phone that
    // re-declares with no torch has no torch, and keeping the previous entry
    // would leave a control on screen for something that is no longer there.
    ph->actuators.clear();
    for (std::size_t i = 0; i < desc->actuator_count; ++i) {
        const ppcp_actuator &a = desc->actuators[i];
        Phone::DeclaredActuator da;
        da.id      = QString::fromUtf8(a.id.v, static_cast<int>(a.id.len));
        da.kind    = QString::fromUtf8(a.kind.v, static_cast<int>(a.kind.len));
        da.control = QString::fromUtf8(a.control.v, static_cast<int>(a.control.len));
        da.label   = a.has_label
                     ? QString::fromUtf8(a.label.v, static_cast<int>(a.label.len))
                     : QString();
        ph->actuators.push_back(da);
    }
    if (!ph->actuators.isEmpty())
        ppWarn() << "[ppcp]" << incomingId << "declared" << ph->actuators.size()
                 << "actuator(s)";

    // 4.4d's habit of mind, one layer in: a counterpart's product strings are
    // display text and are never an identifier or a trust signal.
    ph->name = QString::fromUtf8(desc->product.model.v,
                                   static_cast<int>(desc->product.model.len));

    // MSG 4.1 — the live Session, opened the moment a phone declares rather
    // than deferred to a later "start recording" action: it is a property of
    // the WHOLE conversation (sync, liveness, and — once a caller attaches a
    // VideoInputPpcp to it — a preview), not of a capture in particular.
    // `PpcpHostPeer::attach()` has already bound `m_live` to this peer, and
    // its pump (`tick()`) and event feed (`drainEvents()` -> `m_live.observe()`)
    // already run unconditionally for every connected phone — this is only
    // the one call that turns that machinery on.
    //
    // ⚠ STILL NOT ARMED.  arm() is a property of the capture path and remains
    // uncalled — a live preview tile is independent of arming (5.11.2's preview
    // Stream is unconditional and "always continuous"), and the device arms
    // itself in the shipping product.  A refusal here is logged and the
    // connection proceeds — a phone with no Session simply has no preview, the
    // same as before this existed.
    {
        Ppcp::PpcpLiveSession::Config cfg;
        cfg.sessionId = ("sess:" + QUuid::createUuid().toString(QUuid::WithoutBraces))
                             .toStdString();
        // 8.2c — DECLARED, and named here rather than left to the default so
        // that the one number the host's own corroboration rule is stated in
        // lives in one place.  It is CORE §5.10's proposal and not a
        // measurement; CORE B8 says the same, and B8 also says the floor must
        // eventually be measured per nominator class.
        cfg.coincidenceWindowNs = PPCP_DEFAULT_COINCIDENCE_WINDOW_NS;
        std::string serr;
        if (!ph->peer->liveSession().open(cfg, &serr))
            ppWarn() << "[ppcp] live session open refused for" << ph->name << "-" << serr.c_str();
    }

    // ── CORE §8.2 — the arbiter, and it starts HERE for a reason ────────────
    //
    // ⚠ AFTER open(), NEVER BEFORE.  `ppcp_arbiter_new()` will happily build an
    // arbiter for a peer with no Session, and `active()` then answers true
    // while every entry point inside it bails on a null `timebase_ref` and a
    // null `session_id`.  `observe()` counts only on PPCP_OK, so the whole
    // failure reads as zero candidates, zero groups, zero retained and zero
    // errors — indistinguishable from a quiet range.  The conformance harness
    // opens the Session first for exactly this reason.
    //
    // 8.3e — the library has no random source (ground rule 8), so the embedding
    // mints Shot and Candidate ids.  Returning false would refuse rather than
    // issue a Shot with a made-up id; QUuid cannot fail, so this never does.
    {
        Ppcp::PpcpShotBridge::Config bc;
        std::string aerr;
        const bool ok = ph->peer->shotBridge().start(
            bc,
            [](std::string *out) {
                *out = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                return true;
            },
            &aerr);
        if (!ok) {
            // I20 — libppcp refuses an arbiter for a peer that is not
            // `role: host` or does not declare Arbitrate.  Neither is reachable
            // for this host's own engine, so a refusal here means something
            // structural changed; say so rather than falling back to something
            // that is not arbitration.
            ppWarn() << "[ppcp] the arbiter would not start for" << ph->name << "-"
                     << aerr.c_str();
        } else {
            ppWarn() << "[ppcp] arbitration active for" << ph->name;
        }
        emit shotBridgeChanged();
    }

    // ── CORE 5.11.2 — preview from the moment the link is up ───────────────
    //
    // ⚠ NOT WHEN A TILE OPENS, WHICH IS WHERE IT USED TO START.  A preview is
    // how an operator confirms the camera works and frames the shot BEFORE
    // anything is captured; one that appears only once capture has begun cannot
    // do the job it exists for.  5.11.2 says "opening one is an ordinary
    // `stream_open` from the consumer that wants it" and names preview-alone
    // "during setup and framing" as its MAIN USE — so the request goes out here,
    // beside `session_open`, and the phone produces from then on (5.11c/I36).
    //
    // ⛔ AND THE CONSUMER MAY OWN THIS ONE.  ENC 7a/7b makes a device originate
    // its own capture Streams so its bundle is self-describing; preview never
    // reaches a bundle at all (5.11j), so that argument does not apply and the
    // Stream belongs with whoever wants to look.
    //
    // A peer that declares no preview profile is declining preview, which 5.11.2
    // makes conformant — nothing is asked for and nothing is wrong.
    {
        const QString sessionId =
            QString::fromStdString(ph->peer->liveSession().config().sessionId);
        ppcp_peer *const peer = ph->peer->liveSession().peer();

        const int asked = VideoInputPpcp::openPreviewStreams(
            peer, ph->counterpartId, sessionId, desc);
        ppWarn() << "[ppcp] preview requested for" << asked << "camera Source(s) on" << ph->name;

        // ⛔ **AND SOMETHING HAS TO BE LISTENING WHEN THE PICTURES ARRIVE.**
        // Asking is half of it.  `VideoInputPpcp::dispatchEvent()` hands an
        // inbound event to every LIVE instance for the peer, and until 27 Aug
        // 2026 the only code that ever constructed one was the Settings crop
        // editor — so between connect and an operator opening that panel, every
        // preview Capture was dropped before any counter moved, and so was the
        // `stream_close` that would have told us preview had ended.
        //
        // One consumer per camera Source, alive for the connection.  A tile the
        // operator opens later is a SECOND consumer of the same Stream and both
        // are fed: onCaptureAnnounce() resolves by `source_id`, not by which
        // Streams an instance opened, and reclaimStream() leaves preview-only
        // instances alone (they own no capture Stream to collide on).
        //
        // ⚠ Empty on a first connect only.  A reconnect keeps whatever survived
        // — reattachAll() below re-attaches and restarts it — so this must not
        // build a second set on top.
        if (ph->previews.empty()) {
            const int live = VideoInputPpcp::startPreviewConsumers(
                this, peer, ph->counterpartId, sessionId, desc, &ph->previews);
            // ⭐ AND SOMETHING HAS TO BE LISTENING WHEN THE CLIP ARRIVES --
            // the same lesson as the preview consumers themselves, learned
            // twice.  The join is made outside this class (see
            // previewConsumers()); this only says the set has changed.
            if (live > 0) emit previewConsumersChanged();
            if (live > 0)
                ppWarn() << "[ppcp] preview consumer live for" << live
                         << "camera Source(s) on" << ph->name;
        }
    }

    // ⚠ A RECONNECTION IS A DECLARE, AND A PREVIEW THAT WAS RUNNING MUST COME
    // BACK BY ITSELF.  `dropPhone()` detaches every VideoInputPpcp bound to the
    // peer; nothing re-attached them, because the only call to
    // `CameraInstance::ppcpAttachIfNeeded()` is inside `startPreview()`, which
    // returns early when it is already previewing.  So an operator with the ROI
    // editor open watched the tile die on a link drop and stay dead — and a
    // phone dropping and returning is the ordinary case on a range, not the
    // exception.  A no-op on a first connect, when nothing has been detached.
    {
        const int back = VideoInputPpcp::reattachAll(
            ph->counterpartId, ph->peer->liveSession().peer(),
            QString::fromStdString(ph->peer->liveSession().config().sessionId));
        if (back > 0)
            ppWarn() << "[ppcp]" << back << "preview(s) resumed after" << ph->name
                     << "reconnected";
    }

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

    // ⭐ A phone that arrives (or comes back) while the session screen is
    // already capturing is armed now, on the Session opened above — the golfer
    // is not going to walk over and tap it.  A no-op while capture is off.
    reconcileArm(ph, "declared while capture is live");
    reconcileTorch(ph, "declared while capture is live");

    // CameraManager snapshots the registry at construction and merges only on
    // enumerate(); the home screen's DEVICES list reads the registry directly
    // and picks it up on its own two-second refresh.  So this signal exists for
    // the manager, not for the list.
    emit sourcesChanged();
    emit stateChanged();
    emit phonesChanged();
    // As in dropPhone(): the connected set just grew, and the aggregates over
    // it notify off phoneHealthChanged().
    emit phoneHealthChanged();
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
            // ── RV-6 (H10) — an OPEN BOOTSTRAP WINDOW is a different record ──
            // 3.3f tells the two forms apart by the presence of `bs`, and 3.3g
            // makes an instance carrying both `bs` and `rid` malformed and
            // ignored.  `noteAdvertisement` classifies and keeps only windows;
            // ⛔ it DIALS NOTHING (11.3d1, trap 3) — the user selects one, and
            // `beginGuidedPairing()` is the only door.
            if (m_guided.noteAdvertisement(ad, PPCP_WIRE_VERSION_MAJOR))
                emit guidedChanged();

            if (!d.dial) return;

            const QString inst = QString::fromStdString(ad.instanceName);
            const QString pid  = QString::fromStdString(d.pairingId);
            if (m_seenInstances.value(inst) == pid) return;
            m_seenInstances.insert(inst, pid);
            emit phonesChanged();
        },
        [this](const std::string &instanceName) {
            const QString inst = QString::fromStdString(instanceName);
            // 3.7b/3.7d — a bootstrap instance exists only while its window is
            // open, so the record going away IS the window closing.
            if (m_guided.dropInstance(instanceName)) emit guidedChanged();
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

// ── The wired path (design §6), behind a temporary gate ────────────────────
//
// ⚠ EVERYTHING IT DOES IS SILENT WHEN THERE IS NOTHING TO DO.  No usbmux
// provider, nothing plugged in, a charge-only cable, a phone this host has
// never paired with — RV 3.6a and design §6.2 make every one of those an
// ordinary state, so none of it reaches `status`, `noteFailure()` or a banner.
// One `ppWarn()` line each, for somebody who went looking.
//
// ✅ WIRED IS ON BY DEFAULT.  `PINPOINT_PPCP_WIRED=0` forces it off and is an
// escape hatch rather than a feature flag.  Two mechanisms make that safe, and
// neither is the advertisement suppression §6.1 first proposed: the cable takes
// over from an idle WiFi link instead of racing it, and `onDeclare()` closes a
// duplicate link keyed on the counterpart — without which one phone could hold
// two links and enter the arbiter twice.  The old gate kept Phase 1's M1
// measurable — the cable on and the radio quiet — and it has done that job.
void PpcpHostService::startWired()
{
    if (m_wired) return;
    if (!Ppcp::PpcpWiredLink::enabled()) return;

    // ⚠ NOT QObject-parented: this unique_ptr is the ownership, and a QObject
    // parent as well would be a second one.  It is constructed on the GUI
    // thread, which is the affinity its queued hand-offs need.
    m_wired = std::make_unique<Ppcp::PpcpWiredLink>();
    // RV 5.3b, run client-side and BEFORE the dial (design §5.2).  The host's
    // own resolver, unchanged — a second one would be a second place for expiry
    // (7.3e), exhaustion (7.3a) and invalidation (7.3b) to be got wrong.
    m_wired->setIdentityResolver(m_rv.identityResolver());
    // §6.1 rule 1, widened so the cable can take over from WiFi — see
    // PpcpWiredLink::PeerLinkState for why a takeover and not a refusal.
    using PLS = Ppcp::PpcpWiredLink::PeerLinkState;
    m_wired->setPeerLinkState([this](const QString &pairingId) {
        const Phone *ph = phoneByPairing(pairingId);
        if (!ph)        return PLS::None;
        if (ph->wired)  return PLS::Wired;

        // ⛔ "BUSY" IS WHAT PROTECTS A RUNNING SESSION, and it is deliberately
        // generous.  Taking over costs a measured 35 s before the new link's
        // sigma falls under the 5 ms arbitration gate, so anything that has
        // begun doing work keeps the link it has.  Nothing observed yet means
        // nothing to lose: the phone connected moments ago and the operator is
        // still setting up.
        const Ppcp::PpcpShotBridge::Stats &st = ph->peer->shotBridge().stats();
        const bool sawShots = st.nominated || st.observedForeign || st.issued
                              || st.adopted || st.excluded || st.unarbitrated;
        const bool importing =
            ph->importSink && (ph->importSink->stats().captures
                               || ph->importSink->stats().clipsWritten);
        return (sawShots || importing) ? PLS::WifiBusy : PLS::WifiIdle;
    });
    // The WiFi link goes only once the cable is up — make before break, so there
    // is never a moment with no link at all.
    m_wired->setDropForTakeover([this](const QString &pairingId) {
        if (Phone *ph = phoneByPairing(pairingId))
            dropPhone(ph, "replaced by the cable");
    });
    // ⛔ Contract C2 — the pairing the wired path resolved travels with the link.
    m_wired->setAdoptHandler(
        [this](std::unique_ptr<Ppcp::PeerConnection> link, const QString &pairingId) {
            adoptLink(std::move(link), pairingId);
        });
    m_wired->start();
}

void PpcpHostService::stopWired()
{
    // ⛔ Before the listener and before the accept thread join: its own stop()
    // drops the QSocketNotifier before closing the watch fd and joins its dial
    // thread, so nothing of it is still running when the phones are dropped.
    if (m_wired) m_wired->stop();
    m_wired.reset();
}

void PpcpHostService::stopDiscovery()
{
    m_browseWatch.reset();
    if (m_browser) m_browser->stop();
    m_browser.reset();
    // The windows we knew about are unobservable now.  An attempt already
    // running keeps its own socket — 11.3b's stream is a connection of its own
    // and does not depend on discovery — so it is NOT cancelled here.
    m_guided.clearCandidates();
    emit guidedChanged();
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
// `adoptLink()` and `pumpGuided()` (a pairing completing is now the moment it
// is remembered, erratum E57) and from `forgetPairing()` — between them every
// way the set changes.
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

// ── The user's own name for it ──────────────────────────────────────────────
//
// A flat pairingId -> alias map, deliberately shaped like `cameraAlias` and
// `imuAlias` rather than nested like `ppcp/phoneNames` above: those two are
// both "the settings panel's editable label for a device", and `phoneNames`
// is not that — it is what the phone SAID, refreshed on every `declare` and
// never typed by a person. Kept as a separate key rather than folded into
// that row so the two can be cleared independently if that's ever wanted;
// today `forgetPairing()` drops both together, since a `pairingId` is a fresh
// CSPRNG handle every time (7.3d) and an alias keyed by a dead one could never
// be found again regardless.
QString PpcpHostService::phoneAliasFor(const QString &pairingId)
{
    return ppSettings().value(QStringLiteral("ppcp/phoneAlias")).toMap()
        .value(pairingId).toString();
}

QString PpcpHostService::defaultStudioName() { return studioNameDefault(); }

QString PpcpHostService::studioName() const
{
    const QString stored =
        ppSettings().value(QStringLiteral("ppcp/studioName")).toString().trimmed();
    return stored.isEmpty() ? studioNameDefault() : stored;
}

void PpcpHostService::setStudioName(const QString &name)
{
    QSettings s = ppSettings();
    // 4.3 — at most 64 bytes on the wire.  Truncated by BYTES and not by
    // characters, because the limit is the wire's and a multi-byte name cut
    // mid-sequence would be invalid UTF-8 rather than merely short.
    QByteArray utf8 = name.trimmed().toUtf8();
    while (utf8.size() > 64) {
        utf8.chop(1);
        // Step back off a continuation byte so the result stays well-formed.
        while (!utf8.isEmpty() && (static_cast<unsigned char>(utf8.back()) & 0xC0) == 0x80)
            utf8.chop(1);
    }
    const QString trimmed = QString::fromUtf8(utf8);
    if (trimmed == studioName()) return;

    // ⚠ Empty CLEARS the override rather than publishing an empty name — the
    // field then shows the machine name again, which is what a user emptying a
    // box expects and is also the only value that is never wrong.
    if (trimmed.isEmpty()) s.remove(QStringLiteral("ppcp/studioName"));
    else                   s.setValue(QStringLiteral("ppcp/studioName"), trimmed);

    // ⚠ The name reaches a phone in two ways and BOTH need this.  A code
    // published from now on carries it; an already-paired phone learns it from
    // the next `declare`, which is why refreshCode() alone is not enough.
    refreshCode();
    emit studioNameChanged();
    emit stateChanged();
}

void PpcpHostService::setPhoneAlias(const QString &pairingId, const QString &alias)
{
    if (pairingId.isEmpty()) return;
    QSettings s = ppSettings();
    QVariantMap aliases = s.value(QStringLiteral("ppcp/phoneAlias")).toMap();
    const QString trimmed = alias.trimmed();
    const QString current = aliases.value(pairingId).toString();
    const bool changed = trimmed.isEmpty() ? aliases.contains(pairingId) : (current != trimmed);
    if (!changed) return;

    if (trimmed.isEmpty()) aliases.remove(pairingId);
    else                   aliases[pairingId] = trimmed;
    s.setValue(QStringLiteral("ppcp/phoneAlias"), aliases);
    emit phonesChanged();
}

// ── Every phone this host knows about, as a device row ──────────────────────
//
// Built from the rendezvous ledger, which after `loadPersisted()` holds one
// entry per remembered pairing and one per code minted this run.  A LIVE code
// is not a phone — it is a QR on screen that nobody has scanned — so it is
// skipped here; it belongs to the pairing dialog.
QVariantMap PpcpHostService::ppcpStats() const
{
    QVariantMap m;
    m[QStringLiteral("listening")]  = m_listening;
    m[QStringLiteral("port")]       = m_port;
    m[QStringLiteral("phones")]     = static_cast<int>(m_phones.size());
    m[QStringLiteral("armState")]   = armState();

    // The clip chain, as main.cpp last pushed it. Zeroes where nothing has run
    // yet, which is a reading and not an absence.
    const QVariantMap chain = m_clipChainProvider ? m_clipChainProvider() : m_clipChain;
    for (auto it = chain.cbegin(); it != chain.cend(); ++it)
        m[it.key()] = it.value();
    if (!m.contains(QStringLiteral("clipsFiled"))) {
        m[QStringLiteral("captureRequests")] = 0;
        m[QStringLiteral("clipsAnnounced")]  = 0;
        m[QStringLiteral("clipsConverted")]  = 0;
        m[QStringLiteral("clipsFiled")]      = 0;
    }

    // Summed across phones: a test asserting "the shot crossed" does not care
    // which link carried it, and with one phone the sum IS that phone's.
    int nominated = 0, observed = 0, issued = 0, adopted = 0, excluded = 0;
    int unarbitrated = 0, uncorroborated = 0, reconsidered = 0, refused = 0;
    QVariantList perPhone;
    for (const std::unique_ptr<Phone> &p : m_phones) {
        if (!p->peer) continue;
        const Ppcp::PpcpShotBridge::Stats &b = p->peer->shotBridge().stats();
        nominated      += static_cast<int>(b.nominated);
        observed       += static_cast<int>(b.observedForeign);
        issued         += static_cast<int>(b.issued);
        adopted        += static_cast<int>(b.adopted);
        excluded       += static_cast<int>(b.excluded);
        unarbitrated   += static_cast<int>(b.unarbitrated);
        uncorroborated += static_cast<int>(b.uncorroborated);
        reconsidered   += static_cast<int>(b.reconsidered);
        refused        += static_cast<int>(b.nominationsRefused);

        QVariantMap one;
        one[QStringLiteral("name")]        = p->name;
        one[QStringLiteral("peerId")]      = p->counterpartId;
        // The handle setPhoneActuator() takes, so a probe can command a torch
        // on the phone it is reading, and this phone's own arm state — the
        // aggregate `armState` above is the least-ready of all of them.
        one[QStringLiteral("pairingId")]   = p->pairingId;
        one[QStringLiteral("hostArmed")]   = p->hostArmed;
        {
            using AS = Ppcp::PpcpLiveSession::ArmState;
            QString a;
            switch (p->peer->liveSession().armState()) {
            case AS::Blocked:  a = QStringLiteral("blocked");  break;
            case AS::Stalled:  a = QStringLiteral("stalled");  break;
            case AS::Arming:   a = QStringLiteral("arming");   break;
            case AS::Disarmed: a = QStringLiteral("disarmed"); break;
            case AS::Armed:    a = QStringLiteral("armed");    break;
            }
            one[QStringLiteral("armState")] = a;
        }
        one[QStringLiteral("arbiter")]     = p->peer->shotBridge().active();
        one[QStringLiteral("retained")]    =
            static_cast<int>(p->peer->shotBridge().retainedCount());
        one[QStringLiteral("groups")]      =
            static_cast<int>(p->peer->shotBridge().groupCount());
        one[QStringLiteral("channels")]    =
            p->link ? static_cast<int>(p->link->channels().size()) : 0;
        one[QStringLiteral("sessionOpen")] = p->peer->liveSession().isOpen();
        // ⭐ 6.1f's clock agreement, and an automated run CANNOT PROCEED WITHOUT
        // IT.  A session started before the relation converges arbitrates on a
        // sigma wider than the 5 ms gate, so shots are excluded and the run
        // measures the warm-up rather than the product.  A person waits for the
        // number on screen to settle; a rig has to be able to read it.
        // -1 while nothing has a relation yet — the same sentinel the phones
        // list uses, and not to be confused with zero.
        one[QStringLiteral("syncSigmaMs")] = worstSyncSigmaMsFor(p->peer->liveSession());

        // ── CR-02 §5.10h — how long this Session has been open ─────────────
        //
        // ⭐ THE HOST OPENED IT, SO THE HOST KNOWS.  `openedAtNs()` is the
        // reading `PpcpLiveSession::open()` took of `tb:host` at the moment it
        // sent `session_open`, set once and never revised; `hostNowNs()` is a
        // reading of the SAME clock now, so the subtraction stays inside one
        // timebase and needs no relation.  Before E61 a consumer asking this
        // had to fabricate a start time from the first message it happened to
        // see, which is exactly what 5.10h exists to prevent.
        //
        // ⚠ There is NO wire carrier for `opened_at` (plan §10 item 3), so
        // this is only computable BECAUSE we are the host.  -1 where no Session
        // is open, which is the same "no reading" sentinel every other
        // per-phone number here uses.
        const PpcpLiveSession &ls = p->peer->liveSession();
        one[QStringLiteral("sessionForMs")] =
            (ls.isOpen() && ls.openedAtNs() != 0)
            ? static_cast<qlonglong>((Ppcp::hostNowNs() - ls.openedAtNs()) / 1000000)
            : qlonglong(-1);

        // ── MSG 5.5 — per-Source availability ──────────────────────────────
        //
        // ⛔ KEYED ON `source_id` (CB5).  Published as its own list rather than
        // folded into `inputs` because a Source can report unavailable with no
        // VideoInputPpcp consumer attached at all — a camera taken by another
        // app before anybody opened a Stream on it is precisely the case
        // `in_use` exists for, and folding would drop it.
        QVariantList devStatus;
        for (const PpcpLiveSession::DeviceStatus &d : ls.deviceStatuses()) {
            QVariantMap e;
            e[QStringLiteral("sourceId")]  = QString::fromStdString(d.sourceId);
            e[QStringLiteral("available")] = d.available;
            e[QStringLiteral("reason")]    = QString::fromStdString(d.reason);
            e[QStringLiteral("sinceNs")]   = static_cast<qlonglong>(d.sinceNs);
            e[QStringLiteral("sinceTimebase")] = QString::fromStdString(d.sinceTimebase);
            devStatus.append(e);
        }
        one[QStringLiteral("deviceStatus")] = devStatus;

        // ── MSG 5.6 — the ring buffer's standing margin, per Stream ─────────
        //
        // ⚠ `retainedFromNs` IS IN THE DEVICE'S OWN TIMEBASE and is carried
        // with the `tb` it was stamped in.  It is NOT subtracted from a host
        // reading anywhere: that would mix two clocks with no shared epoch,
        // which is the exact defect `offsetToRefNs()`'s header records finding
        // live on 27 August.  A retained WINDOW would need the device's own
        // "now" and nothing on this wire carries one, so the panel shows the
        // target, the discard count and the last discard span instead.
        QVariantList bufStatus;
        for (const PpcpLiveSession::BufferMargin &b : ls.bufferMargins()) {
            QVariantMap e;
            e[QStringLiteral("streamId")]       = QString::fromStdString(b.streamId);
            e[QStringLiteral("retainedFromNs")] = static_cast<qlonglong>(b.retainedFromNs);
            e[QStringLiteral("retainedFromTimebase")] =
                QString::fromStdString(b.retainedFromTimebase);
            e[QStringLiteral("retentionTargetMs")] = b.hasRetentionTarget
                ? static_cast<qlonglong>(b.retentionTargetNs / 1000000) : qlonglong(-1);
            e[QStringLiteral("discardedSinceOpen")] =
                static_cast<qlonglong>(b.discardedSinceOpen);
            e[QStringLiteral("lastDiscardDurationMs")] = b.hasLastDiscard
                ? static_cast<qlonglong>(b.lastDiscardDurationNs / 1000000) : qlonglong(-1);
            e[QStringLiteral("lastDiscardSinceNs")] = b.hasLastDiscard
                ? static_cast<qlonglong>(b.lastDiscardSinceNs) : qlonglong(0);
            bufStatus.append(e);
        }
        one[QStringLiteral("bufferStatus")] = bufStatus;

        // Same builder the two phone-row readers use.
        one[QStringLiteral("actuators")] = actuatorRowsFor(p.get());

        // The camera side of the same link — where a preview actually stops.
        one[QStringLiteral("inputs")] = VideoInputPpcp::countersFor(p->counterpartId);
        perPhone.append(one);
    }
    m[QStringLiteral("nominated")]      = nominated;
    m[QStringLiteral("observedForeign")]= observed;
    m[QStringLiteral("issued")]         = issued;
    m[QStringLiteral("adopted")]        = adopted;
    m[QStringLiteral("excluded")]       = excluded;
    // ⚠ The one that says "a device nominated and we were not listening".
    m[QStringLiteral("unarbitrated")]   = unarbitrated;
    m[QStringLiteral("uncorroborated")] = uncorroborated;
    m[QStringLiteral("reconsidered")]   = reconsidered;
    m[QStringLiteral("nominationsRefused")] = refused;
    m[QStringLiteral("perPhone")]       = perPhone;

    // MSG 9.1 — what we have taken in and what we still owe for it.
    m[QStringLiteral("importCaptures")] = static_cast<int>(m_importLedger.captureCount());
    m[QStringLiteral("importSessions")] = static_cast<int>(m_importLedger.sessionCount());
    m[QStringLiteral("commitsOwed")]    = static_cast<int>(m_importLedger.pendingCommitCount());
    return m;
}

// The three readings a `heartbeat_ack` moves, for one pairing.
//
// ⛔ EXISTS SO A PANEL NEED NOT RE-READ phones() TO REFRESH A NUMBER.  Binding a
// row to the whole list means every heartbeat rebuilds every delegate, and a
// destroyed delegate takes the alias field's focus with it — which made
// Settings -> Phones untypeable while any phone was linked.  Same values and
// the same "no reading" sentinels phones() publishes (-1, empty string), read
// off the live session rather than a copy, so the two cannot drift.
QVariantMap PpcpHostService::phoneHealth(const QString &pairingId) const
{
    QVariantMap out;
    const Phone *live = phoneByPairing(pairingId);
    const bool   isLive = live != nullptr;

    static const PpcpLiveSession::PeerHealth kNoHealth{};
    const PpcpLiveSession::PeerHealth &health =
        isLive ? live->peer->liveSession().peerHealth() : kNoHealth;

    out[QStringLiteral("batteryPct")] = (health.valid && health.hasBatteryPct)
                                       ? static_cast<int>(health.batteryPct) : -1;
    out[QStringLiteral("thermal")] = health.valid
        ? QString::fromStdString(ppcp_thermal_level_str(health.thermal))
        : QString();
    // The other two readings the same `heartbeat_ack` carries, parsed since
    // H4 and displayed nowhere until CR-02's statistics tab (CB3).  Same "no
    // reading yet" sentinel as the battery: -1, never a plausible-looking 0.
    out[QStringLiteral("charging")] = (health.valid && health.hasCharging)
                                     ? (health.charging ? 1 : 0) : -1;
    out[QStringLiteral("storageFreeBytes")] = health.valid
        ? static_cast<qlonglong>(health.storageFreeBytes) : qlonglong(-1);
    out[QStringLiteral("syncSigmaMs")] = isLive
        ? worstSyncSigmaMsFor(live->peer->liveSession()) : -1.0;
    // CR-02 — published here AND in phones(), from the same builder, so the
    // torch control can refresh on `phoneHealthChanged()` without re-reading
    // the phone list and rebuilding every row (trap 1).  Empty for a phone that
    // is not connected: a remembered pairing has no declaration and therefore
    // no Actuators, which is not the same as a phone that declared none.
    out[QStringLiteral("actuators")] = actuatorRowsFor(live);
    return out;
}

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
        const QString alias  = phoneAliasFor(pid);

        // The phone's own name where it has ever declared one; otherwise the
        // handle, shortened.  Inventing a friendlier name would assert a fact
        // about the device, and nothing here knows one.  This is what Settings
        // -> Phones shows as the row's META line, and what `name` below falls
        // back to when there is no alias.
        const QString declaredName = isLive && !live->name.isEmpty() ? live->name
                                    : !stored.isEmpty()              ? stored
                                    : tr("Phone %1").arg(pid.left(6));

        QVariantMap dev;
        dev[QStringLiteral("kind")]       = QStringLiteral("Phone");
        // A user-set alias wins everywhere a phone's name is shown — the
        // DEVICES list, the resource monitor, this row's own header — the same
        // priority `cameraAlias`/`imuAlias` have over a device's own reported
        // name.  `alias` and `declaredName` are exposed separately so Settings
        // -> Phones can offer the alias as an editable field while still
        // showing what the phone itself declared underneath it.
        dev[QStringLiteral("alias")]        = alias;
        dev[QStringLiteral("declaredName")] = declaredName;
        dev[QStringLiteral("name")] = !alias.isEmpty() ? alias : declaredName;
        dev[QStringLiteral("model")]      = stored;
        dev[QStringLiteral("backend")]    = QStringLiteral("PPCP");
        dev[QStringLiteral("identifier")] = pid;
        dev[QStringLiteral("pairingId")]  = pid;
        dev[QStringLiteral("persisted")]  = st.persisted;
        dev[QStringLiteral("invalidated")] = st.invalidated;
        // Joins a phone's row to its cameras' rows in `cameraManager.cameraList`
        // — `VideoInputPpcp`'s `serialNumber` is this same peer id (see
        // ResourceMonitorController) — so Settings -> Phones can show how many
        // cameras this phone is currently contributing without inventing a
        // second count of the same Sources.  Empty while not connected: a
        // remembered-but-disconnected phone has no live cameras to count.
        dev[QStringLiteral("counterpartId")] = isLive ? live->counterpartId : QString();

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

        // 7.4b's `heartbeat_ack` battery/thermal, read straight off the live
        // session — there is nothing to poll, it is already the latest one
        // `PpcpLiveSession::observe()` parsed. `valid` is false until the
        // first ack arrives, which is a different answer from "0%"/"nominal"
        // and is kept distinct: -1 and an empty string, the same "no reading
        // yet" sentinel `imu_instance`'s `batteryPercent` uses.
        static const PpcpLiveSession::PeerHealth kNoHealth{};
        const PpcpLiveSession::PeerHealth &health =
            isLive ? live->peer->liveSession().peerHealth() : kNoHealth;
        dev[QStringLiteral("batteryPct")] = (health.valid && health.hasBatteryPct)
                                           ? static_cast<int>(health.batteryPct) : -1;
        dev[QStringLiteral("thermal")] = health.valid
            ? QString::fromStdString(ppcp_thermal_level_str(health.thermal))
            : QString();
        // ⚠ Published here AND in phoneHealth() above, from the same struct,
        // for the same reason battery and thermal are: a panel refreshing one
        // reading must not have to re-read this whole list.  -1 is "no ack
        // yet" for both — charging is tri-state (-1 unknown / 0 / 1) because
        // `has_charging` is optional on the wire.
        dev[QStringLiteral("charging")] = (health.valid && health.hasCharging)
                                         ? (health.charging ? 1 : 0) : -1;
        dev[QStringLiteral("storageFreeBytes")] = health.valid
            ? static_cast<qlonglong>(health.storageFreeBytes) : qlonglong(-1);

        // Design §6.1 — "surface which path a phone is on ... an operator who
        // cannot see that the cable did nothing cannot act on it."  ⚠ It is a
        // property of the LINK, so a remembered-but-absent phone has none: the
        // empty string is the same "no reading" sentinel as the battery's -1,
        // and it must not read as "on WiFi".
        dev[QStringLiteral("transport")] = isLive
            ? (live->wired ? QStringLiteral("cable") : QStringLiteral("wifi"))
            : QString();

        // 6.1f's clock agreement, THIS phone's own worst related-timebase
        // sigma — not the toolbar's cross-phone aggregate. -1 while nothing
        // has a relation yet, the same sentinel every other reading above uses.
        dev[QStringLiteral("syncSigmaMs")] = isLive
            ? worstSyncSigmaMsFor(live->peer->liveSession()) : -1.0;

        // 5.2a / 7.3c — this phone's own arm state, which is THE DEVICE'S
        // answer and not our sent flag.  Empty for a phone that is not here:
        // a remembered pairing has no arm state, the same way it has no
        // battery reading.
        using AS = Ppcp::PpcpLiveSession::ArmState;
        QString armStr;
        QString armBlocked;
        int     armReadyMs = -1;
        if (isLive) {
            const PpcpLiveSession &ls = live->peer->liveSession();
            switch (ls.armState()) {
            case AS::Disarmed: armStr = QStringLiteral("disarmed"); break;
            case AS::Arming:   armStr = QStringLiteral("arming");   break;
            case AS::Armed:    armStr = QStringLiteral("armed");    break;
            case AS::Blocked:  armStr = QStringLiteral("blocked");  break;
            case AS::Stalled:  armStr = QStringLiteral("stalled");  break;
            }
            armBlocked = QString::fromStdString(ls.blockedReason());
            const PpcpLiveSession::PeerReadiness &rd = ls.peerReadiness();
            if (rd.valid && rd.hasEstimate) armReadyMs = static_cast<int>(rd.estimatedReadyMs);
        }
        dev[QStringLiteral("armState")]        = armStr;
        dev[QStringLiteral("armBlockedReason")] = armBlocked;
        dev[QStringLiteral("armReadyMs")]      = armReadyMs;

        // CORE 5.19c / MSG §12 — what this phone declared it can be TOLD to do.
        // ⚠ An empty list is a complete answer (5.19c): a phone owning no
        // Actuators omits the key from `declare` entirely, and the Cameras
        // pill simply shows no control rather than a disabled one.  Same
        // builder as phoneHealth() so the two cannot drift.
        dev[QStringLiteral("actuators")] = actuatorRowsFor(live);

        dev[QStringLiteral("hasWarning")]  = false;
        out.append(dev);
    }
    return out;
}

int PpcpHostService::phoneLowestBatteryPct() const
{
    int lowest = -1;
    for (const std::unique_ptr<Phone> &p : m_phones) {
        const PpcpLiveSession::PeerHealth &health = p->peer->liveSession().peerHealth();
        if (!health.valid || !health.hasBatteryPct) continue;
        const int pct = static_cast<int>(health.batteryPct);
        if (lowest < 0 || pct < lowest) lowest = pct;
    }
    return lowest;
}

QString PpcpHostService::phoneWorstThermal() const
{
    bool have = false;
    ppcp_thermal_level worst = PPCP_THERMAL_NOMINAL;
    for (const std::unique_ptr<Phone> &p : m_phones) {
        const PpcpLiveSession::PeerHealth &health = p->peer->liveSession().peerHealth();
        if (!health.valid) continue;
        if (!have || health.thermal > worst) worst = health.thermal;
        have = true;
    }
    return have ? QString::fromStdString(ppcp_thermal_level_str(worst)) : QString();
}

double PpcpHostService::phoneWorstSyncSigmaMs() const
{
    // 6.1f's uncertainty, worst-case across every related timebase on every
    // connected phone — the same number PPC's own screen shows while its
    // clock relation is still converging. -1 means "no relation yet", not
    // "perfectly synced": `offsetToRefNs()` writes nothing until §6.3a has an
    // estimate, so a fresh connection reads -1 for the ~2 minutes it takes
    // the burst-then-maintenance cadence to settle, not 0ms.
    //
    // ⚠ EVALUATED AT `observedAtNs()`, NOT `hostNowNs()` — see the comment on
    // `PpcpLiveSession::observedAtNs()` for why: `hostNowNs()` is this HOST's
    // own since-boot clock, and mixing it into a relation stamped in the
    // PHONE's since-boot clock fabricates an elapsed interval libppcp then
    // grows the sigma by — a real ~17ms turned into ~460ms against a real
    // phone, found live 27 Aug.
    double worst = -1.0;
    for (const std::unique_ptr<Phone> &p : m_phones) {
        const double ms = worstSyncSigmaMsFor(p->peer->liveSession());
        if (ms > worst) worst = ms;
    }
    return worst;
}

void PpcpHostService::onRelations(Phone *ph)
{
    if (!ph || ph->counterpartId.isEmpty()) return;

    // 6.1f — a `relation_update` was published, so every VideoInputPpcp bound
    // to this peer is re-fed its offset.  Per SOURCE timebase and not per peer:
    // a phone with a camera clock and an audio clock has one relation per clock
    // and a single scalar would fabricate one of them.
    //
    // ⚠ EVALUATED AT `observedAtNs()`, NOT `hostNowNs()` — this used to pass
    // the host's own clock reading as if it were an instant in the PHONE's own
    // since-boot clock (the domain `offsetToRefNs()`'s `sourceTimebase` names).
    // The two counters share no epoch, so the offset this fed into
    // `VideoInputPpcp::setTimebaseOffsetNs()` was corrupted by a fabricated
    // elapsed interval the same way `phoneWorstSyncSigmaMs()`'s sigma was —
    // found live 27 Aug via the toolbar pill showing ~460ms against a real
    // ~17ms. `observedAtNs()` is always a genuine instant in the relation's
    // own domain, so this is correct rather than merely closer.
    const PpcpLiveSession &live = ph->peer->liveSession();
    const int mapped = VideoInputPpcp::applyTimebaseOffsets(
        ph->counterpartId, [&](const QString &tb, qint64 *outNs) {
            const std::string stb = tb.toStdString();
            std::int64_t atNs = 0;
            if (!live.observedAtNs(stb, &atNs)) return false;
            std::int64_t off = 0;
            if (!live.offsetToRefNs(stb, atNs, &off)) return false;
            *outNs = static_cast<qint64>(off);
            return true;
        });
    (void)mapped;

    // Every relation_update moves `phoneWorstSyncSigmaMs`, so the toolbar's
    // warning pill tracks convergence the same way it tracks battery/thermal —
    // off a signal rather than a poll.
    // ⛔ phoneHealthChanged(), NOT phonesChanged(), FOR THE SAME REASON THE
    // HEARTBEAT USES IT.  A clock relation converging is a READING moving; it
    // does not change which phones exist.  `relation_update` arrives every few
    // seconds for as long as a phone is linked, so the structural signal here
    // rebuilt every delegate in Settings -> Phones on that cadence and destroyed
    // the alias field an operator was typing into — the same defect the
    // heartbeat fix cured, arriving by a second door (reported 31 Aug 2026).
    emit phoneHealthChanged();
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

    // ── THE TICK'S OWN PUNCTUALITY ─────────────────────────────────────────
    // ⚠ Heartbeats are queued from this tick (7.4a) and a phone declares the
    // link LOST after three missed intervals — 3 s at the default.  A link
    // that dies with "no error reported" on the phone and "the peer shut down
    // cleanly" here (1 Sept 2026, 36 s into a hosted run) therefore has two
    // candidate causes that leave identical logs: the network held the beats
    // up, or THIS THREAD did.  One line settles which.  Measured against the
    // wall clock, not the tick count, because a tick that did not fire is
    // exactly the thing being looked for.
    {
        const std::int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastTickMs != 0 && nowMs - m_lastTickMs >= kTickStallWarnMs)
            ppWarn() << "[ppcp] host tick stalled" << (nowMs - m_lastTickMs)
                     << "ms — no heartbeat left this host meanwhile, and a phone"
                     << "declares the link lost after 3 missed intervals";
        m_lastTickMs = nowMs;
    }

    // ── 6.3 convergence trace (PINPOINT_SYNC_TRACE=1) ──────────────────────
    // ⚠ THE TWO TERMS OF THE SIGMA ARE MEASURED APART, because only their
    // RATIO says what a slow convergence is waiting for.  6.3's published
    // sigma is sqrt(offset_sigma^2 + (elapsed * skew_sigma)^2), so evaluating
    // the SAME relation at `observed_at` and again five seconds later
    // separates them without libppcp having to expose either: the first is the
    // offset term alone, and the second gives the skew term by subtraction.
    // The offset term is floored by half the minimum RTT and cannot be tuned
    // away; the skew term collapses as the retained window comes to span real
    // time.  Which of the two is holding the estimate above the shot bridge's
    // 5 ms gate decides whether the answer is a different schedule or a
    // different method, and guessing between those is how two minutes stayed
    // unexplained.
    if (m_syncTrace) {
        const std::int64_t nowMono = QDateTime::currentMSecsSinceEpoch();
        if (nowMono - m_lastSyncTraceMs >= 1000) {
            m_lastSyncTraceMs = nowMono;
            for (const std::unique_ptr<Phone> &p : m_phones) {
                const PpcpLiveSession &live = p->peer->liveSession();
                for (const std::string &tb : live.relatedTimebases()) {
                    std::int64_t atNs = 0;
                    if (!live.observedAtNs(tb, &atNs)) continue;
                    std::int64_t off = 0;
                    double s0 = 0.0, s5 = 0.0;
                    if (!live.offsetToRefNs(tb, atNs, &off, &s0)) continue;
                    if (!live.offsetToRefNs(tb, atNs + 5'000'000'000LL, &off, &s5)) continue;
                    // s5^2 = s0^2 + (5 s * skew)^2, so the skew term falls out.
                    const double drift5 = (s5 > s0) ? std::sqrt(s5 * s5 - s0 * s0) : 0.0;
                    const double skewPpm = drift5 / 5.0e9 * 1.0e6;
                    // Exchanges actually FOLDED IN (against probes sent), and the
                    // minimum RTT the fit retained.  The gap between probes and
                    // observed is one diagnosis — a probe nobody answers costs the
                    // same as one that lands and teaches nothing.
                    //
                    // ⚠ `min_rtt` IS THE ONE THAT SAYS WHETHER A SCHEDULE COULD
                    // EVER HELP.  6.3f's estimator floors `offset_sigma` at half
                    // of it (`ppcp_sync.c`: `asym = min_rtt_ns * 0.5`, combined in
                    // quadrature with the fit's residual), so an offset sigma
                    // sitting on that floor is a TRANSPORT result and no cadence
                    // reaches it — which is what seven scheduling experiments
                    // established the expensive way.  Printing the floor beside
                    // the value is what tells the two apart at a glance.
                    const ppcp_sync_estimator *est = estimatorForSource(live.peer(), tb);
                    const double minRttMs =
                        est ? ppcp_sync_estimator_min_rtt_ns(est) / 1.0e6 : 0.0;
                    // E67 — `offset_sigma` above is what a conversion RESOLVES to
                    // (the declared relation or the inverse of ours, whichever is
                    // tighter).  This is what the phone DECLARED for the same
                    // direction, so a run can time both estimators.
                    double declaredMs = -1.0;
                    if (const ppcp_relation_set *rs = live.relations()) {
                        ppcp_id from{}, to{};
                        if (ppcp_id_set(&from, tb.c_str(), tb.size()) == PPCP_OK
                            && ppcp_id_set(&to, live.config().timebaseRef.c_str(),
                                           live.config().timebaseRef.size()) == PPCP_OK) {
                            if (const ppcp_timebase_relation *r = ppcp_relations_find(rs, &from, &to))
                                declaredMs = r->offset_sigma_ns / 1.0e6;
                        }
                    }
                    // ⚠ THE RELATION SET IS NOT NECESSARILY OURS.  Both peers call
                    // ppcp_peer_publish_relations(), and libppcp puts a published
                    // relation into the SAME p->relations that an arriving
                    // `relation_update` writes to (ppcp_peer.c:1978 and :2835).  So
                    // `offset_sigma` above is whichever peer wrote that key last,
                    // and it can be the phone's estimate of us rather than ours of
                    // it.  Printing OUR estimator's own sigma beside it is the only
                    // way to tell — and if the two disagree, the number the shot
                    // bridge gates on was never this host's to improve.
                    double ownSigmaMs = 0.0;
                    std::string ownDir;
                    if (est) {
                        ppcp_timebase_relation own{};
                        if (ppcp_sync_estimator_relation(est, &own) == PPCP_OK) {
                            ownSigmaMs = own.offset_sigma_ns / 1.0e6;
                            ownDir = std::string(own.from.v, own.from.len) + "->" +
                                     std::string(own.to.v, own.to.len);
                        }
                    }
                    ppWarn()
                        << "[ppcp-sync] t+" << (nowMono - m_syncTraceStartMs) / 1000 << "s "
                        << "probes=" << live.stats().probesQueued
                        << "observed="
                        << (est ? (qulonglong)ppcp_sync_estimator_count(est) : 0)
                        << tb.c_str()
                        << " min_rtt=" << minRttMs << "ms"
                        << " (floor=" << minRttMs / 2.0 << "ms)"
                        << " offset_sigma=" << s0 / 1.0e6 << "ms"
                        << " own_sigma=" << ownSigmaMs << "ms"
                        << " declared_sigma=" << declaredMs << "ms"
                        << " own_dir=" << ownDir.c_str()
                        << " skew_sigma=" << skewPpm << "ppm"
                        << " sigma@5s=" << s5 / 1.0e6 << "ms"
                        << (s5 < 5.0e6 ? "  <-- UNDER THE 5ms GATE" : "");
                }
            }
        }
    }

    // RV-6 (H10) — drive the single attempt 11.3d1 allows, off the timer that
    // is already running.  `poll()` applies 11.3e's 30- and 60-second timers,
    // which are the embedding's obligation because libppcp owns no clock.
    pumpGuided();

    // 3.4a/3.4d1 — the `rn` rotation, driven off the timer that is already
    // running rather than a second one.  `tick()` is an integer comparison
    // until a rotation is actually due, so calling it at 50 Hz costs nothing;
    // when one IS due it is a single TXT update (3.2d), not a re-registration.
    if (m_advert) m_advert->tick(static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch()));
    // The browser's own periodic hook (native Windows engine only — see
    // RvBrowser::tick()'s header comment; every other backend's default
    // no-op costs the same integer comparison and nothing else).
    if (m_browser) m_browser->tick(static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch()));

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
    // 5.14h — pay what we owe, to whichever phone is here to receive it.  After
    // the tick, so a commit queued by a payload that landed during this very
    // tick goes out on the next one rather than waiting for another event; and
    // only for phones that survived it.
    for (const std::unique_ptr<Phone> &p : m_phones)
        if (std::find(dead.begin(), dead.end(), p.get()) == dead.end())
            flushOwedCommits(p.get());
    // The arm state can move with nothing arriving — the stall deadline above is
    // a conclusion drawn from time passing — so the transition is noticed here
    // rather than only on a `readiness`.
    {
        const QString now = armState();
        if (now != m_lastArmState) {
            m_lastArmState = now;
            emit armStateChanged();
            emit phonesChanged();
        }
    }
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
    cfg.displayName = studioName().toStdString();

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

void PpcpHostService::forgetPairing(const QString &pairingId)
{
    // 7.4d — revocation is honoured immediately by this side, which means the
    // next handshake resolves nothing and fails like any stranger (7.7c).  A
    // remembered pairing's only way to stop being remembered, now that E57
    // has made 7.4b a SHOULD and this host remembers a completed pairing on
    // its own: there is no opt-in step left to simply not take.
    m_rv.revoke(pairingId.toStdString());
    // 7.4d is "honoured immediately by this side", and an advertisement still
    // naming the revoked pairing would be this side continuing to offer it.
    refreshAdvertisement();
    // The nickname goes with the key — both halves of it.  7.4d is about the
    // pairing, but a name (what the phone said) or an alias (what the user
    // called it) left behind is a record that this host has met that phone,
    // and "forget" must not leave one; neither could be found again anyway,
    // since the next pairing with this device draws a fresh pairingId (7.3d).
    {
        QSettings st = ppSettings();
        QVariantMap names = st.value(QStringLiteral("ppcp/phoneNames")).toMap();
        if (names.remove(pairingId) > 0)
            st.setValue(QStringLiteral("ppcp/phoneNames"), names);
        QVariantMap aliases = st.value(QStringLiteral("ppcp/phoneAlias")).toMap();
        if (aliases.remove(pairingId) > 0)
            st.setValue(QStringLiteral("ppcp/phoneAlias"), aliases);
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
    //
    // The wired line joins them for the same reason 5.4k puts the negotiated
    // TLS mode here: a counted state, no UDID, no identity, no key (RV 7.2b).
    return QString::fromStdString(m_rv.diagnosticExport())
         + QStringLiteral("\ndiscovery: %1\ndiscovered-pairings: %2\nadvertisement: %3\n%4\n")
               .arg(discoveryDescription())
               .arg(m_seenInstances.size())
               .arg(m_advert ? QString::fromStdString(m_advert->describe())
                             : tr("no service advertisement on this platform"))
               .arg(m_wired ? m_wired->describe()
                            : QStringLiteral("wired: off (PINPOINT_PPCP_WIRED=0)"));
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

    // ⛔ THE ONE PLACE THAT CAN SAY *WHY* AUTHENTICATION KEEPS FAILING, and it
    // had no caller in the application at all — only tests.  RV 5.3c/7.7c make
    // the handshake message deliberately UNIFORM, because naming the cause on
    // the wire is the distinguisher 7.7c forbids; that is a property of what we
    // tell the PEER, and says nothing about what the operator's own log may
    // know.  Without this, repeated refusals are indistinguishable from each
    // other and the only recourse is to guess — which cost most of 30 Aug 2026.
    //
    // ⚠ Threshold, not every failure: a single refusal is ordinary (a stale
    // code, a phone that walked away mid-handshake) and must stay quiet.  A RUN
    // of them is a state worth dumping, and it is dumped again every
    // kFailureDiagEvery so a long run shows whether the state is changing —
    // roughly one line per five minutes at the observed two-per-30s rate.
    if (m_failureCount == kFailureDiagAfter
     || (m_failureCount > kFailureDiagAfter
         && (m_failureCount - kFailureDiagAfter) % kFailureDiagEvery == 0)) {
        ppWarn() << "[ppcp-rv]" << m_failureCount
                 << "handshake refusals — resolver state follows\n"
                 << m_rv.diagnosticExport().c_str();
    }
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

// ══ RV-6 — guided pairing, the INITIATOR half (H10) ═══════════════════════
//
// ⛔ THE THREE TRAPS OF THIS SECTION, AND WHY EACH IS INVISIBLE FROM OUTSIDE.
//
//   TRAP 3 (11.3d1) — `guidedWindows` carries no digits and nothing dials a
//     candidate.  `beginGuidedPairing()` takes ONE name and refuses while an
//     attempt is live, so there is never a moment at which two sets of digits
//     exist.  An attacker advertising N windows would otherwise get N blind
//     draws against one confirmation, WITH THE OPERATOR FINDING THE COLLISION
//     FOR THEM: shown a list of numbers one of which matches the phone in their
//     hand, an operator taps the match and reads it as success.
//
//   TRAP 7 (11.6b) — lives in ppcp_bootstrap.cpp, where a failed key agreement
//     becomes `invalid_key` and never a transport error and never a retry.
//     What surfaces here is `guidedMayRetry == false` and a message that says
//     it is not a network fault.
//
//   TRAP 8 (11.1d) — there is NO property here carrying the counterpart's
//     digits and no function that could take one.  `confirmGuidedDigitsMatch()`
//     is reachable only from a control a person touched.  A peer that compared
//     the digits itself would pass every static test in the document and
//     authenticate nothing.

bool PpcpHostService::guidedAvailable() const
{
    // 3.6b — silent absence.  No DNS-SD client, no windows to find, and the
    // §4 pairing code path is unaffected and is what the user sees instead.
    return m_browser != nullptr;
}

QVariantList PpcpHostService::guidedWindows() const
{
    QVariantList out;
    for (const auto &c : m_guided.candidates()) {
        QVariantMap m;
        m.insert("instanceName", QString::fromStdString(c.instanceName));
        // ⛔ 4.4d, reached through 3.3g — UNTRUSTED DISPLAY TEXT.  Already
        // escaped and truncated by `sanitiseLabel()`.  It is shown before
        // anything has been authenticated, so it is whatever a stranger put on
        // the wire, and it is NEVER an identifier, a trust signal or a storage
        // key: `instanceName` above is what selection is keyed on.
        m.insert("label", QString::fromStdString(c.label));
        m.insert("hasLabel", c.hasLabel);
        m.insert("role", QString::fromStdString(c.role));
        // ⚠ NO `rid`, NO `rn`, NO PAIRING HANDLE, and there is nothing to put
        // there: 3.3g says a bootstrap instance carries none, because it names
        // no pairing.
        out.append(m);
    }
    return out;
}

bool PpcpHostService::guidedActive() const { return m_guided.attemptInProgress(); }

QString PpcpHostService::guidedPhase() const
{
    const Ppcp::GuidedAttempt *a = m_guided.attempt();
    if (a == nullptr) return QStringLiteral("idle");
    switch (a->phase()) {
    case Ppcp::GuidedPhase::Idle:       return QStringLiteral("idle");
    case Ppcp::GuidedPhase::Dialling:   return QStringLiteral("dialling");
    case Ppcp::GuidedPhase::Exchanging: return QStringLiteral("exchanging");
    case Ppcp::GuidedPhase::Comparing:  return QStringLiteral("comparing");
    case Ppcp::GuidedPhase::Confirming: return QStringLiteral("confirming");
    case Ppcp::GuidedPhase::Paired:     return QStringLiteral("paired");
    case Ppcp::GuidedPhase::Failed:     return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

QString PpcpHostService::guidedDigits() const
{
    // ⛔ 11.7e and 11.7f BOTH, and the emptiness is the enforcement.  Nothing
    // before 11.5d has completed — there is nothing to compare, and a
    // progressive display would leak the value to whichever side an attacker
    // reached first — and nothing after the attempt ends, because the digits
    // are a function of two ephemeral keys and are meaningless outside it.
    // `GuidedAttempt::sasDigits()` returns "" outside that window and the
    // engine has wiped the value.
    const Ppcp::GuidedAttempt *a = m_guided.attempt();
    if (a == nullptr) return QString();
    return QString::fromStdString(a->sasDigits());
}

QString PpcpHostService::guidedMessage() const
{
    const Ppcp::GuidedAttempt *a = m_guided.attempt();
    if (a == nullptr) return QString();
    if (a->phase() == Ppcp::GuidedPhase::Paired)
        return tr("Paired. That device can now connect on its own.");
    if (a->phase() != Ppcp::GuidedPhase::Failed) return QString();
    return QString::fromStdString(a->advice().message);
}

bool PpcpHostService::guidedMayRetry() const
{
    // ⛔ 11.9c — FALSE MEANS THE DIALOGUE SHOWS NO RETRY AFFORDANCE AT ALL.
    // Not a greyed-out one, not one behind a confirmation — none.  A mismatch
    // or a MAC failure means either an implementation is wrong or someone is on
    // the link, and a dialogue whose reflex is *try again* converts a one-shot
    // bound into an unbounded one by way of the operator's muscle memory.
    const Ppcp::GuidedAttempt *a = m_guided.attempt();
    if (a == nullptr || a->phase() != Ppcp::GuidedPhase::Failed) return false;
    return a->advice().mayOfferRetry;
}

bool PpcpHostService::guidedOfferCode() const
{
    // 11.9d1 on the first `unsupported_version`, 11.9d on the second of
    // anything else.  §4's code is REQUIRED of every implementation (2a), does
    // not depend on multicast, and is the answer to both plausible causes.
    if (m_guided.shouldOfferPairingCode()) return true;
    const Ppcp::GuidedAttempt *a = m_guided.attempt();
    return a != nullptr && a->phase() == Ppcp::GuidedPhase::Failed &&
           a->advice().offerPairingCode;
}

bool PpcpHostService::beginGuidedPairing(const QString &instanceName)
{
    // ⛔ TRAP 3'S ONE DOOR.  One name, and a refusal while an attempt is live.
    std::string why;
    const bool ok = m_guided.begin(instanceName.toStdString(), &why);
    if (!ok && !why.empty())
        ppWarn() << "[ppcp-rv6] not started:" << why.c_str();
    emit guidedChanged();
    return ok;
}

void PpcpHostService::confirmGuidedDigitsMatch()
{
    // ⛔ 11.7c / TRAP 8 — THIS IS THE AFFIRMATIVE ACT OF A PERSON AT THIS END,
    // and there is no other caller.  "A single affirmation at one end does not
    // establish a pairing at the other, and a peer MUST NOT treat the arrival
    // of the counterpart's `bs_confirm` as standing in for its own user's."
    Ppcp::GuidedAttempt *a = m_guided.attempt();
    if (a == nullptr) return;
    a->affirm();
    pumpGuided();
    emit guidedChanged();
}

void PpcpHostService::rejectGuidedDigits()
{
    Ppcp::GuidedAttempt *a = m_guided.attempt();
    if (a == nullptr) return;
    a->decline();
    emit guidedChanged();
}

void PpcpHostService::cancelGuidedPairing()
{
    Ppcp::GuidedAttempt *a = m_guided.attempt();
    if (a == nullptr) return;
    // 11.9a — any abort ends the attempt and leaves no pairing at either peer.
    // A user walking away is a timeout rather than a mismatch, so 11.9c lets it
    // read as the ordinary failure it is.
    a->abort(PPCP_BS_RC_TIMEOUT);
    emit guidedChanged();
}

void PpcpHostService::dismissGuidedResult()
{
    // 11.9b — "MUST NOT reopen the window without a further explicit user
    // action."  This IS that action's first half: until the last result has
    // been dismissed, `begin()` refuses.
    m_guided.endAttempt();
    m_guidedLastPhase.clear();
    m_guidedLastDigits.clear();
    emit guidedChanged();
}

void PpcpHostService::pumpGuided()
{
    Ppcp::GuidedAttempt *a = m_guided.attempt();
    if (a == nullptr) return;
    if (!a->terminal()) a->poll();

    // 11.5g — the pairing exists only now, and 11.1a makes it an ORDINARY one:
    // "from here the pairing is INDISTINGUISHABLE from one established by a
    // scanned code, so §5, §7.4 and §7.5 apply verbatim."  So it goes into the
    // same ledger the code path fills, and the Phones panel, the resolver and
    // the reconnection advertiser cannot tell which produced it.
    if (a->phase() == Ppcp::GuidedPhase::Paired && m_guidedPairingId.isEmpty()) {
        ppcp_bs_pairing p{};
        if (a->takePairing(&p)) {
            std::string id, err;
            // ⛔ 4.4d — the label is UNTRUSTED and is carried as a label only.
            const std::string label = a->candidate().label;
            if (m_rv.adoptGuidedPairing(p.sid, p.keys, label, &id, &err)) {
                m_guidedPairingId = QString::fromStdString(id);
                ppWarn() << "[ppcp-rv6] guided pairing established";
                // E57 — adoptGuidedPairing() remembered this pairing
                // automatically, so it is worth advertising already (3.5e).
                refreshAdvertisement();
                emit phonesChanged();
            } else {
                ppWarn() << "[ppcp-rv6] the pairing could not be recorded:"
                         << err.c_str();
            }
            // 11.6f / 11.10c — whatever happened above, this copy goes.  A peer
            // computes the whole chain the moment it holds `Z`, so a failure
            // here would otherwise leave a `PRK` for a pairing nothing knows
            // about.  Computing is not holding.
            OPENSSL_cleanse(&p, sizeof p);
        }
    }

    const QString phase = guidedPhase();
    const QString digits = guidedDigits();
    if (phase != m_guidedLastPhase || digits != m_guidedLastDigits) {
        m_guidedLastPhase = phase;
        m_guidedLastDigits = digits;
        emit guidedChanged();
    }
}

// ── The harness tap (RT-20c) ───────────────────────────────────────────────
//
// ⛔⛔ THE TAP, NEVER THE COMPARISON.  See the header.  Read the four branches
// below and note what is absent from every one of them: no digits are read, no
// counterpart value arrives, and nothing is compared.  `control` names a
// button; the caller decided which to press before it called.
bool PpcpHostService::guidedUserAction(const QString &control)
{
#if defined(PP_PPCP_RV6_HARNESS)
    // ⚠ THE SAME FOUR ENTRIES THE REAL CONTROLS CALL, AND DELIBERATELY NOT A
    // PARALLEL PATH.  A harness that reached past these into the engine would
    // be testing something the application does not do, and 11.7c's obligation
    // — that the affirmation is this device's own user's — would then hold on a
    // path no test ever exercised.
    if (control == QLatin1String("match")) {
        if (m_guided.attempt() == nullptr) return false;
        confirmGuidedDigitsMatch();
        return true;
    }
    if (control == QLatin1String("different")) {
        if (m_guided.attempt() == nullptr) return false;
        rejectGuidedDigits();
        return true;
    }
    if (control == QLatin1String("cancel")) {
        if (!m_guided.attemptInProgress()) return false;
        cancelGuidedPairing();
        return true;
    }
    if (control == QLatin1String("dismiss")) {
        dismissGuidedResult();
        return true;
    }
    return false;
#else
    // ⛔ THE SHIPPING SHAPE.  Not merely inert — it says so, because a silent
    // false would let a harness "pass" against a build that never pressed
    // anything, and a green RT-20c row that asserted nothing is worse than a
    // red one.
    (void)control;
    ppWarn() << "[ppcp-rv6] guidedUserAction refused: this build has no harness "
                "tap (PP_PPCP_RV6_HARNESS is off, and a shipping build refuses "
                "to configure with it on).";
    return false;
#endif
}

bool PpcpHostService::addGuidedEndpoint(const QString &host, int port,
                                        const QString &label)
{
#if defined(PP_PPCP_RV6_HARNESS)
    // 3.7h — "§11 constrains the handshake and not how the endpoint was
    // learned."  Everything downstream of this is the ordinary path: the same
    // five frames, the same ordering, the same digits, the same affirmation,
    // and the same refusal of a second concurrent attempt.
    if (port <= 0 || port > 65535) return false;
    const QString name = QStringLiteral("oob:%1:%2").arg(host).arg(port);
    const bool ok = m_guided.addEndpoint(name.toStdString(), host.toStdString(),
                                         static_cast<std::uint16_t>(port),
                                         label.toStdString());
    if (ok) emit guidedChanged();
    return ok;
#else
    (void)host; (void)port; (void)label;
    ppWarn() << "[ppcp-rv6] addGuidedEndpoint refused: this build has no "
                "harness tap (PP_PPCP_RV6_HARNESS is off).";
    return false;
#endif
}
