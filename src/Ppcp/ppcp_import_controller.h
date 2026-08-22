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

// "Import session…", and deliberately nothing more.
//
// The UI's whole job here is to pick a file. Everything after that is the file
// transport of H3, which is the same ingest path a socket uses (plan A10) —
// there is no import wizard, no format chooser and no second schema, because
// CORE §9 and ENC §7 say a bundle IS a recorded session and "a separate
// 'import' feature is how two ingest paths and a drifting schema come about".
//
// ⚠ A CONTEXT PROPERTY, NOT A QML_ELEMENT. libppcp is an optional dependency
// of this application (H0: a box with no sibling checkout and no fetch still
// builds), so registering this into the QML module would make the module fail
// to compile on exactly the machines the optionality exists for. main.cpp
// installs `ppcpImport` when the library is embedded and does not when it is
// not, and PpcpImportAction.qml checks before it calls.
//
// ⚠ WHERE THE IMPORTED SESSION LANDS, AND WHY IT IS NOT IN THE SWING LIBRARY
// YET. The clips and the ledger go under `<library>/PPCP Imports/<peer>/
// <session>/`, keyed on the PPCP ids. They are not written as `swing.json`
// beside the live captures because this application's `Session.id` is a
// filesystem directory path and its `Shot.id` an `int` ordinal, and CORE 8.5c
// keys idempotent re-import on OPAQUE ids — so the join needs host work item 2
// (Session and Shot identity) first. Doing it early would either duplicate a
// session on the second import or throw the PPCP identity away, and I34 is
// precisely the invariant that would be lost.

#include <QObject>
#include <QString>
#include <QUrl>
#include <memory>

namespace Ppcp { class PpcpEngine; }

class PpcpImportController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool ingestAvailable READ ingestAvailable CONSTANT)

public:
    explicit PpcpImportController(QObject *parent = nullptr);
    ~PpcpImportController() override;

    QString status() const { return m_status; }
    bool busy() const { return m_busy; }

    // Whether an engine can be built at all. The UI uses it to say what will
    // happen BEFORE the user picks a file, which is the difference between a
    // deferral and a disappointment.
    bool ingestAvailable() const;

    // The whole action. `file` is what the FileDialog handed back.
    Q_INVOKABLE bool importSession(const QUrl &file);

    // Where the ledger lives. Set from the athlete library root; defaults to
    // the app data location so an import works before a library is chosen.
    Q_INVOKABLE void setLibraryRoot(const QString &root);

signals:
    void statusChanged();
    void busyChanged();
    void importFinished(bool ok, const QString &message);

private:
    void setStatus(const QString &s);

    QString m_status;
    QString m_libraryRoot;
    // CORE 5.1a — this host's own stable Id. A placeholder until H4 mints and
    // persists one; it is never derived from mutable local state.
    QString m_peerId = QStringLiteral("peer:pinpointstudio");
    bool    m_busy = false;
};
