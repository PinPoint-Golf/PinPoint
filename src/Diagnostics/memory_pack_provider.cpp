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

namespace pinpoint::analysis {

namespace {

// A pack that is already in memory, presented through the provider seam so it can be merged with
// core exactly as a file-backed one is. See makeMemoryPackProvider()'s comment for WHY this exists:
// it is the editor's unsaved working copy, not a test double.
class MemoryPackProvider final : public ICharacteristicPackProvider {
public:
    MemoryPackProvider(const CharacteristicPack &pack, QString label, PackOrigin origin)
        : m_pack(pack), m_label(std::move(label)), m_origin(origin)
    {
        // Validated on construction, exactly as a loaded pack is. An overlay routinely fails
        // STANDALONE referential integrity — its edges point at core conditions it does not itself
        // contain — so this report is advisory and the caller is expected to read the ASSEMBLED
        // one. Skipping validation here would still be wrong: a duplicate id or a malformed reducer
        // is a fault of this pack alone, and it is worth naming before the merge buries it.
        m_report = validatePack(m_pack);
    }

    const CharacteristicPack &pack() const override { return m_pack; }
    const ValidationReport   &report() const override { return m_report; }
    QString                   label() const override { return m_label; }
    PackOrigin                origin() const override { return m_origin; }

private:
    CharacteristicPack m_pack;
    ValidationReport   m_report;
    QString            m_label;
    PackOrigin         m_origin;
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
