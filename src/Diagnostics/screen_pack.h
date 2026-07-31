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

#include "characteristic_pack.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

// The physical-screen registry — what `Condition::screenRef` points at.
//
// Until now `screenRef` was a namespace and nothing else: thirteen conditions named a `screen.*` id
// and no file in the tree said what any of them meant, how to run one, or what passing looked like.
// The explanation pass already takes screen ANSWERS (`explain()`'s knownScreenResults) and its
// highest-value output is "screen this — it would explain four of your findings", so the one thing
// standing between that and a coach was the protocol behind the id.
//
// PROVENANCE, and it is not a style note: a screen here is described from CLINICAL RANGE-OF-MOTION
// literature in generic terms — a seated trunk rotation, a knee-to-wall, a modified Thomas test.
// Branded screening systems are not named, cited or alluded to anywhere. The movements are common
// property of the physiotherapy literature; the packaging of them is somebody's product, and
// `core_pack_test` greps the raw bytes to keep it out.
//
// Deliberately NOT behind an abstract provider the way packs and norms are. That polymorphism
// exists so a COMMUNITY pack can arrive and be namespaced against core; there is no community
// story for screens, and inventing three provider classes for a flat reference list would be
// ceremony standing in for a requirement. The user layer is a merge, below. If a second origin ever
// turns up, it is one virtual away.

namespace pinpoint::analysis {

struct Screen {
    QString id;               // `screen.*`, matching Condition::screenRef
    QString label;
    QString bodyRegion;       // for grouping in the UI: "Hip", "Thoracic spine", "Shoulder", …
    QString protocol;         // how to run it, in one paragraph, genericly described
    QString passCriterion;    // what passing looks like, in words — always present

    // The numeric floor where the literature gives one. Optional because several screens are
    // qualitative ("achieves neutral extension") and inventing a number for those would be worse
    // than leaving the words to do the work.
    std::optional<double> passAtLeast;
    QString               unit;      // "°", "cm", "s" — required when passAtLeast is set

    QString citation;         // DOI or PMID. NEVER a commercial protocol or certification body.
    QString note;             // caveats: what the screen does not settle
};

struct ScreenSet {
    QString             id;
    QString             version;
    int                 schemaVersion = 1;
    QString             sourceLabel;
    bool                readOnly = false;
    std::vector<Screen> screens;

    const Screen *screen(const QString &id) const;
};

// ── Validation ──────────────────────────────────────────────────────────────
//
// ERRORS:
//   duplicateId          two screens share an id
//   screenIdNamespace    an id outside the `screen.` namespace — the join with Condition::screenRef
//                        is by exact string, so a stray id silently matches nothing
//   screenUnitMissing    a numeric pass floor with no unit; the number would be unreadable
// WARNINGS:
//   screenNoProtocol     a screen nobody could actually run
//   screenNoPass         no pass criterion, so the answer is unrecordable
ValidationReport validateScreenSet(const ScreenSet &set);

struct ScreenLoadResult {
    ScreenSet        set;
    ValidationReport report;
    bool             parsed = false;   // the JSON was read; may still have failed validation
    bool             loaded = false;   // parsed AND valid
};

ScreenLoadResult loadScreenSet(const QByteArray &json, const QString &sourceLabel);
QByteArray       saveScreenSet(const ScreenSet &set);

// The assembled registry: the shipped set, with the user's own layered over it by id.
//
// `PINPOINT_CORE_SCREENS` overrides the shipped path, for the same reason the pack and norm
// providers have their own override — the Qt resource exists only inside the app binary, so without
// it no standalone test or offline tool could reach shipped content.
const ScreenSet &sharedScreenSet();
void             resetSharedScreenSet();   // after writing a user set; call from the UI thread

// ── The two layers, separately ──────────────────────────────────────────────
//
// sharedScreenSet() is the MERGE, which is the right answer for everybody who only reads. An editor
// needs the layers apart: it must write back the user's own entries and never a flattened copy of
// the shipped set, and it has to be able to answer "does this ship?" — which the merge cannot.
//
// The same split the pack and norm registries already have (userPackPath / saveUserPack), arriving
// here for the same reason: the Diagnostic Model panel writes screens now.
const ScreenSet &coreScreenSet();          // shipped only, cached like the merge
ScreenSet        loadUserScreenSet();      // the user layer alone; empty when there is no file
QString          userScreenSetPath();
bool             saveUserScreenSet(const ScreenSet &set, QString *whyNot = nullptr);

} // namespace pinpoint::analysis
