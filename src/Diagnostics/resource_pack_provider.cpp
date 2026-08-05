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

// The shipped core pack. The runtime copy is a Qt resource; the SOURCE OF TRUTH is the reviewable
// JSON committed at src/Resources/diagnostics/core.json, from which the resource is built at build
// time. That split is what lets a community contribution arrive as a pull request against readable
// content rather than a binary blob.
//
// Nothing else to say, which is the point: loadShipped() carries the PINPOINT_CORE_PACK seam, the
// open-or-report-an-error, the read-only mark and the Core origin, and it carries the identical
// versions of all four for the norm set beside it. See provider_leaf_p.h.
class ResourcePackProvider final : public detail::PackLeaf {
public:
    explicit ResourcePackProvider(const QString &resourcePath) { loadShipped(resourcePath); }
};

} // namespace

std::unique_ptr<ICharacteristicPackProvider> makeResourcePackProvider(const QString &resourcePath)
{
    return std::make_unique<ResourcePackProvider>(resourcePath);
}

} // namespace pinpoint::analysis
