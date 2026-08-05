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

#include "pack_provider.h"

#include "provider_leaf_p.h"

namespace pinpoint::analysis {

namespace {

// A pack that is already in memory, presented through the provider seam so it can be merged with
// core exactly as a file-backed one is. See makeMemoryPackProvider()'s comment for WHY this exists:
// it is the editor's unsaved working copy, not a test double.
//
// adopt() validates it on construction exactly as a loaded pack is validated, and the argument for
// doing so on advisory content lives with it in provider_leaf_p.h.
class MemoryPackProvider final : public detail::PackLeaf {
public:
    MemoryPackProvider(const CharacteristicPack &content, QString label, PackOrigin origin)
    {
        adopt(content, std::move(label), origin);
    }
};

} // namespace

std::unique_ptr<ICharacteristicPackProvider> makeMemoryPackProvider(const CharacteristicPack &pack,
                                                                    const QString            &label,
                                                                    PackOrigin                origin)
{
    return std::make_unique<MemoryPackProvider>(
        pack, label.isEmpty() ? QStringLiteral("in memory") : label, origin);
}

} // namespace pinpoint::analysis
