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

#include "provider_leaf_p.h"

namespace pinpoint::analysis {

namespace {

// A norm set already in memory, presented through the provider seam. See makeMemoryNormProvider()'s
// comment for WHY this exists: it is the editor's unsaved working copy, not a test double.
//
// adopt() validates it on construction exactly as a loaded set is validated, and the argument for
// doing so on advisory content lives with it in provider_leaf_p.h. The tree is handed in for the
// same reason it is everywhere on this side: a norm set does not carry one, and resolution is
// meaningless without it.
class MemoryNormProvider final : public detail::NormLeaf {
public:
    MemoryNormProvider(NormPack pack, ContextTree contexts, QString label, PackOrigin origin)
    {
        m_contexts = std::move(contexts);
        adopt(std::move(pack), std::move(label), origin);
    }
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
