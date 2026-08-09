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

// The one canonical golf-bag club vocabulary, driver → putter. SINGLE SOURCE OF
// TRUTH for every club picker in the app — the shot-carousel swing-edit popover
// (ShotListModel::clubOptions, persisted to swing.json review.club) and the Markup
// Lab metadata panel (MarkupController::clubOptions, persisted to truth.json
// meta.club). Both must offer the same labels so one physical swing never carries
// two differently-spelled club strings across its sidecars.
//
// Uppercase to match the carousel display font and the capture-time "DRIVER" stub,
// so an unedited shot's club maps to a picker row. Adding/renaming a club here
// changes it everywhere at once.

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace pinpoint {

inline const QStringList &clubVocabulary()
{
    static const QStringList kClubs = {
        QStringLiteral("DRIVER"),
        QStringLiteral("3 WOOD"),
        QStringLiteral("5 WOOD"),
        QStringLiteral("3 HYBRID"),
        QStringLiteral("4 HYBRID"),
        QStringLiteral("3 IRON"),
        QStringLiteral("4 IRON"),
        QStringLiteral("5 IRON"),
        QStringLiteral("6 IRON"),
        QStringLiteral("7 IRON"),
        QStringLiteral("8 IRON"),
        QStringLiteral("9 IRON"),
        QStringLiteral("PITCHING WEDGE"),
        QStringLiteral("GAP WEDGE"),
        QStringLiteral("SAND WEDGE"),
        QStringLiteral("LOB WEDGE"),
        QStringLiteral("PUTTER"),
    };
    return kClubs;
}

// The last-resort club for a document that declares none anywhere. It is a LIE, and a
// consequential one — context_tree resolves anything containing "DRIVER" to the driver
// node, so a stubbed swing is graded against driver corridors, while an EMPTY club would
// have resolved to the default context instead. It survives only because the carousel and
// the picker have always shown a club for every row. swingDocClub() below exists to make
// reaching this line rare: every document written since capture.club.name landed carries
// the club that was actually selected.
inline QString clubStub() { return QStringLiteral("DRIVER"); }

// WHICH CLUB A SWING WAS HIT WITH, from its swing.json root. THE one resolver — the
// session-picker summary (swing_doc.cpp) and the diagnostics phase grid
// (measure_sample.cpp) both call it, so a per-club filter can never bucket the same swing
// two ways.
//
// Precedence, most authoritative first:
//   1. review.club     — the user's own pick or correction, made in the swing-edit
//                        popover, and the only one of the three a human typed.
//   2. capture.club.name — the session's active club at the instant of the shot
//                        (Home CLUB chip → SessionController::activeClub → the export
//                        job). Written by the camera path, which has no review block
//                        until the shot is edited; without this step every unedited
//                        camera swing read back as the stub.
//   3. the stub        — nothing declared it. Pre-dates capture.club.name; unrecoverable.
inline QString swingDocClub(const QJsonObject &root)
{
    const QString review = root.value(QStringLiteral("review")).toObject()
                               .value(QStringLiteral("club")).toString().trimmed();
    if (!review.isEmpty())
        return review;

    const QString captured = root.value(QStringLiteral("capture")).toObject()
                                 .value(QStringLiteral("club")).toObject()
                                 .value(QStringLiteral("name")).toString().trimmed();
    if (!captured.isEmpty())
        return captured;

    return clubStub();
}

} // namespace pinpoint
