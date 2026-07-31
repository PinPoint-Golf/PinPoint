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

#include "norm_provider.h"

namespace pinpoint::analysis {

namespace {

// A norm set already in memory, presented through the provider seam. See makeMemoryNormProvider()'s
// comment for WHY this exists: it is the editor's unsaved working copy, not a test double.
class MemoryNormProvider final : public INormProvider {
public:
    MemoryNormProvider(NormPack pack, ContextTree contexts, QString label, PackOrigin origin)
        : m_pack(std::move(pack)), m_contexts(std::move(contexts)), m_label(std::move(label)),
          m_origin(origin)
    {
        // Validated on construction exactly as a loaded set is. A user layer routinely fails
        // referential checks on its own — its rows name measures it does not itself contain — so
        // this report is advisory and the caller reads the ASSEMBLED one. Skipping it entirely
        // would still be wrong: a duplicate key or a mismatched unit is a fault of this set alone.
        m_report = validateNormPack(m_pack);
    }

    const NormPack         &norms() const override { return m_pack; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override { return m_report; }
    QString                 label() const override { return m_label; }
    PackOrigin              origin() const override { return m_origin; }

private:
    NormPack         m_pack;
    ContextTree      m_contexts;
    ValidationReport m_report;
    QString          m_label;
    PackOrigin       m_origin;
};

} // namespace

std::unique_ptr<INormProvider> makeMemoryNormProvider(const NormPack &pack,
                                                      const ContextTree &contexts,
                                                      const QString &label, PackOrigin origin)
{
    return std::make_unique<MemoryNormProvider>(
        pack, contexts, label.isEmpty() ? QStringLiteral("in memory") : label, origin);
}

} // namespace pinpoint::analysis
