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

#include <vector>

// The drill registry — what `Condition::drills` points at.
//
// `drills` has been a real field with no content behind it anywhere: 0 of 50 shipped conditions
// carried one, which the developer guide names as "the one place the pack is structurally ready for
// something nobody has authored". The stated purpose of the whole rule set is to drive the "so
// what" conversation, and a fault with no answer to "what do I do about it" stops one sentence
// short of being useful.
//
// Same shape as the screen registry and for the same reasons — see screen_pack.h, including why
// this is a flat set rather than a provider hierarchy.
//
// A drill is COACHING content and the honesty rules differ from a measurement's. It states what the
// golfer does and what it is trying to change; it does not claim an effect size, because nothing
// here has measured one. `targets` is written as intent ("to feel the trail hip accept load"), never
// as a promise.

namespace pinpoint::analysis {

struct Drill {
    QString     id;            // `drill.*`, matching Condition::drills
    QString     label;
    QString     instruction;   // what the golfer actually does
    QString     targets;       // what it is trying to change, as INTENT rather than a claim
    QStringList equipment;     // empty when none is needed — most of the useful ones need none
    QString     note;          // when it is the wrong drill, or what it will not fix
};

struct DrillSet {
    QString            id;
    QString            version;
    int                schemaVersion = 1;
    QString            sourceLabel;
    bool               readOnly = false;
    std::vector<Drill> drills;

    const Drill *drill(const QString &id) const;
};

// ERRORS: duplicateId · drillIdNamespace (outside `drill.`, so nothing can point at it)
// WARNINGS: drillNoInstruction (nobody could do it) · drillNoTarget (nobody could say why)
ValidationReport validateDrillSet(const DrillSet &set);

struct DrillLoadResult {
    DrillSet         set;
    ValidationReport report;
    bool             parsed = false;
    bool             loaded = false;
};

DrillLoadResult loadDrillSet(const QByteArray &json, const QString &sourceLabel);
QByteArray      saveDrillSet(const DrillSet &set);

const DrillSet &sharedDrillSet();
void            resetSharedDrillSet();

} // namespace pinpoint::analysis
