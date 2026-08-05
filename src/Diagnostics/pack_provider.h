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

#include <memory>
#include <vector>

// The pack provider seam. Assembled ON DEMAND (makeCharacteristicPackProvider), mirroring
// makeMetricCatalogue() and makeReferenceBandProvider() — no self-registering statics, no startup
// registration, nothing that runs before main().

namespace pinpoint::analysis {

// Where a pack came from. This decides who may override whom, and the distinction is load-bearing:
//
//   Core        — shipped, read-only.
//   LocalUser   — this user's own library. Their deliberate edits, so they OVERRIDE core.
//   Community   — imported from someone else. Namespaced, and core always wins.
//
// The brief's rule is "core wins on collision", aimed at stopping a community pack silently
// redefining a shipped characteristic — a user reading the detail page would have no way to tell
// which definition they were looking at. That rationale does not apply to the user editing their
// own library: refusing their edit would make the editor useless for every shipped characteristic.
// So the protection is kept exactly where it was aimed, and no wider.
enum class PackOrigin { Core, LocalUser, Community };

class ICharacteristicPackProvider {
public:
    virtual ~ICharacteristicPackProvider() = default;

    // The assembled library. A provider that fails to load reports it through `report()` and
    // returns whatever it could read — the UI shows a broken community pack rather than an empty
    // library with no explanation.
    virtual const CharacteristicPack &pack() const = 0;

    // Load-time and structural findings. Warnings here ARE the health list.
    virtual const ValidationReport &report() const = 0;

    virtual QString label() const = 0;
    virtual PackOrigin origin() const = 0;
};

// Where the user's own pack lives. Exposed so the editor writes to the same file the provider
// reads, rather than each guessing the path.
QString userPackPath();

// Persist the user's pack. Returns false (with `whyNot` set) rather than throwing, so a failed save
// surfaces in the UI instead of silently losing an edit.
bool saveUserPack(const CharacteristicPack &pack, QString *whyNot = nullptr);

// The shipped core pack, read from the Qt resource. Read-only.
std::unique_ptr<ICharacteristicPackProvider> makeResourcePackProvider(
    const QString &resourcePath = QStringLiteral(":/diagnostics/core.json"));

// Every characteristic pack file in a directory (QStandardPaths::AppDataLocation/diagnostics by
// default), in LAYERING ORDER: `user.json` first if present, then the rest alphabetically. Every
// *.json is a pack EXCEPT the sibling registries that share the directory (`*.norms.json`,
// `screens.json`, `drills.json`, `references.json`, `contexts.json`).
//
// Exposed because the skip list and the ordering are ONE decision with two callers — the provider
// factory below, and any tool or test that needs to know what the app will read — and a second copy
// of either would eventually disagree with this one. The argument for both lives at the definition
// in file_pack_provider.cpp.
QStringList characteristicPackFilesIn(const QString &directory = QString());

// ONE pack file (userPackPath() by default). A missing file is not an error — no user pack is the
// normal case; an unreadable or malformed one is reported through report() and yields an empty
// pack, so a broken community pack costs the library that pack and nothing else.
std::unique_ptr<ICharacteristicPackProvider> makeFilePackProvider(
    const QString &packFile = QString(), PackOrigin origin = PackOrigin::LocalUser);

// One provider per pack file in the directory, in characteristicPackFilesIn() order, with the
// origin each file's name implies: `user.json` is LocalUser (the editor writes it), everything else
// is Community (it was imported from someone else). Feed the result straight to
// makeMergedPackProvider().
//
// One provider PER FILE, which is what the merger has always expected and what nothing built until
// now: a single directory-wide provider can only hold one pack, so every pack after the first was
// parsed, had its faults reported, and then had its content silently dropped.
std::vector<std::unique_ptr<ICharacteristicPackProvider>> makeFilePackProviders(
    const QString &directory = QString());

// Core + user layers, in that order, and any number of user layers. Collision policy depends on
// origin (see PackOrigin):
//   * LocalUser entities keep their ids and REPLACE a core entity with the same id — an override.
//     They are not prefixed, because a prefix would make overriding impossible.
//   * Community entities are prefixed with "<packId>:", and on a surviving collision CORE WINS,
//     with the loser reported as a warning.
std::unique_ptr<ICharacteristicPackProvider> makeMergedPackProvider(
    std::unique_ptr<ICharacteristicPackProvider> core,
    std::vector<std::unique_ptr<ICharacteristicPackProvider>> user);

// The default assembly: shipped core plus whatever the user has installed.
std::unique_ptr<ICharacteristicPackProvider> makeCharacteristicPackProvider();

// A provider over a pack that is already in memory, validated on construction exactly as a loaded
// one is.
//
// This exists for ONE reason and it is worth stating, because a provider that reads nothing looks
// like a test double: an editor holds a WORKING COPY of the user pack that has not been saved yet,
// and every surface showing that edit has to read an assembly containing it. Without this seam the
// only assembly available is the one on disk, so an unsaved edit is invisible until it is written —
// which forces per-field saves, and per-field saves are what make an undo stack impossible to
// promise anything with. Feed this to makeMergedPackProvider() beside a core provider and the
// result is "the library as it would be if you saved now".
std::unique_ptr<ICharacteristicPackProvider> makeMemoryPackProvider(
    const CharacteristicPack &pack, const QString &label = QString(),
    PackOrigin origin = PackOrigin::LocalUser);

} // namespace pinpoint::analysis
