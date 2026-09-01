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

#include "ppcp_import_controller.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <memory>

#include "ppcp_bundle_transport.h"
#include "ppcp_engine.h"
#include "ppcp_host_engine.h"
#include "ppcp_import_ledger.h"
#include "ppcp_import_sink.h"

PpcpImportController::PpcpImportController(QObject *parent) : QObject(parent) {}
PpcpImportController::~PpcpImportController() = default;

bool PpcpImportController::ingestAvailable() const
{
    return true;
}

void PpcpImportController::setLibraryRoot(const QString &root)
{
    m_libraryRoot = root;
}

void PpcpImportController::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

bool PpcpImportController::importSession(const QUrl &file)
{
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    if (path.isEmpty()) {
        setStatus(tr("No file chosen."));
        emit importFinished(false, m_status);
        return false;
    }

    m_busy = true;
    emit busyChanged();

    QString root = m_libraryRoot;
    if (root.isEmpty())
        root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString importRoot = QDir(root).filePath(QStringLiteral("PPCP Imports"));
    QDir().mkpath(importRoot);

    // The ledger is the record of what has already been taken in (I34). It is
    // loaded before the walk so a re-import can be a no-op rather than a
    // duplicate, and saved after so a crash between the two costs an import
    // rather than a library.
    //
    // ⛔ BORROWED WHERE THERE IS ONE TO BORROW.  Building a private ledger over
    // a file another object already has open is how an import's records used to
    // be lost: two in-memory copies, one file, and whichever saved last won.
    // `fallback` is used only when nothing shared was injected — a build with no
    // PPCP transport, or a test — where there is no second writer to race.
    Ppcp::PpcpImportLedger  fallback;
    Ppcp::PpcpImportLedger &ledger = m_sharedLedger ? *m_sharedLedger : fallback;
    if (!m_sharedLedger) {
        fallback.load(QDir(root).filePath(QStringLiteral("ppcp-ledger.json")).toStdString());
        fallback.foldIn(
            QDir(importRoot).filePath(QStringLiteral("ppcp-import.json")).toStdString());
    }

    // ⚠ THE SAME ENGINE A SOCKET GETS, FROM THE SAME FACTORY. Plan A10 and
    // CORE 9a: "a consumer gains a file transport, NOT an importer". If this
    // line built a different peer, or a lookalike, the claim would be false.
    std::string why;
    // Named members, not a positional brace list: H5 grew this struct and the
    // positional form silently shifted `listener` onto the `health` callback.
    Ppcp::HostEngineConfig cfg;
    cfg.peerId   = m_peerId.toStdString();
    cfg.policy   = nullptr;
    cfg.listener = true;
    std::unique_ptr<Ppcp::PpcpEngine> engine = Ppcp::makeHostEngine(std::move(cfg), &why);
    m_busy = false;
    emit busyChanged();

    if (!engine || !engine->peer()) {
        setStatus(tr("The PPCP engine could not be started: %1")
                      .arg(QString::fromStdString(why)));
        emit importFinished(false, m_status);
        return false;
    }

    Ppcp::PpcpImportSink::Config sinkCfg;
    sinkCfg.importRoot = importRoot.toStdString();
    Ppcp::PpcpImportSink sink(ledger, engine->peer(), sinkCfg);

    Ppcp::PpcpBundleTransport::Options opt;
    opt.sink = engine->peer();
    opt.index = sink.index();
    // Per frame, because the engine's event ring is four deep and a payload
    // chunk's bytes are only valid until the next frame is fed.
    opt.onFrame = [&sink](std::uint8_t) { sink.drainEvents(); };

    const Ppcp::PpcpBundleTransport::Result r =
        Ppcp::PpcpBundleTransport::streamFile(path.toStdString(), opt);
    sink.drainEvents();
    sink.finish(r);

    if (!r.ok) {
        setStatus(tr("%1 is not a session bundle: %2")
                      .arg(QFileInfo(path).fileName(), QString::fromStdString(r.error)));
        emit importFinished(false, m_status);
        return false;
    }

    ledger.save();

    const Ppcp::PpcpImportSink::Stats &st = sink.stats();
    QString msg = tr("%1: %2 frames, %3 captures (%4 new, %5 already held), %6 clips")
                      .arg(QFileInfo(path).fileName())
                      .arg(r.frames)
                      .arg(st.captures)
                      .arg(st.capturesNew)
                      .arg(st.capturesAlreadyHeld)
                      .arg(st.clipsWritten);

    // ENC 7d — said out loud, because a partial session is a different thing
    // from a complete one and a user who is not told will assume the second.
    // Three states, not two: `unknown` is a bundle that simply ended, and it is
    // not `complete` because completeness is asserted and never inferred (I10).
    switch (r.completeness) {
    case PPCP_COMPLETE: msg += tr(" — the owner asserts this session is complete"); break;
    case PPCP_PARTIAL:  msg += tr(" — this session is partial"); break;
    case PPCP_ABSENT:   msg += tr(" — the owner asserts this session's data is absent"); break;
    default:            msg += tr(" — completeness unknown: nothing asserted one"); break;
    }
    if (r.truncated) msg += tr(" (the file ends mid-frame)");
    if (!r.manifestOrdered)
        msg += tr(" — a payload frame preceded the manifest, which ENC 7c forbids");

    // CORE 5.14h — what is now owed to the device that recorded this.
    if (st.commitsQueued > 0)
        msg += tr(". %1 capture_committed owed to %2 on its next connection")
                   .arg(st.commitsQueued)
                   .arg(QString::fromStdString(st.ownerPeerId));

    setStatus(msg);
    emit importFinished(true, m_status);
    return true;
}
