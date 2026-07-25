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
};

// The shipped core pack, read from the Qt resource. Read-only.
std::unique_ptr<ICharacteristicPackProvider> makeResourcePackProvider(
    const QString &resourcePath = QStringLiteral(":/diagnostics/core.json"));

// User packs from a directory (QStandardPaths::AppDataLocation/diagnostics by default). Every
// *.json in the directory is a pack; an unreadable one is reported, not fatal.
std::unique_ptr<ICharacteristicPackProvider> makeFilePackProvider(const QString &directory = QString());

// Core + user, in that order. Namespacing and collision policy:
//   * A user pack's entities are prefixed with "<packId>:" UNLESS they already carry a prefix.
//   * On an id collision after prefixing, CORE WINS and the loser is reported as a warning.
// Core winning is deliberate: a community pack must not be able to silently redefine a shipped
// characteristic, because the user would have no way to tell which definition they were reading.
std::unique_ptr<ICharacteristicPackProvider> makeMergedPackProvider(
    std::unique_ptr<ICharacteristicPackProvider> core,
    std::vector<std::unique_ptr<ICharacteristicPackProvider>> user);

// The default assembly: shipped core plus whatever the user has installed.
std::unique_ptr<ICharacteristicPackProvider> makeCharacteristicPackProvider();

} // namespace pinpoint::analysis
