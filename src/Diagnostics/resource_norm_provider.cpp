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

// ── The shared resolution rule ──────────────────────────────────────────────
// Non-virtual on the interface, defined once here, so every provider resolves identically.

NormResolution INormProvider::resolve(const QString &measureId, const QString &contextId,
                                      const Cohort &athlete) const
{
    NormResolution out;
    if (measureId.isEmpty())
        return out;

    const ContextTree &tree = contexts();

    // A shot that declared no context is a normal, expected case — it resolves to the default and
    // the caller marks the finding inferred. A context the tree does NOT recognise is different:
    // grading it against full-swing norms would be a wrong answer wearing a right answer's clothes,
    // so it resolves to nothing and the caller reports it unavailable.
    const QString requested = contextId.isEmpty() ? kDefaultContextId() : contextId;

    const QStringList chain = tree.chain(requested);
    if (chain.isEmpty())
        return out;

    // CONTEXT-MAJOR, and the nesting order is the whole rule. The cohort probes run INSIDE the
    // context walk, so an unqualified driver row beats a senior row at `any` for a driver shot —
    // stance width is club-mechanical, and if senior-driver matters it gets authored. Inverting the
    // loops would make every cohort row shadow every club row beneath it.
    const std::vector<Cohort> probes = cohortProbeOrder(athlete);

    for (const QString &ctx : chain) {
        for (const Cohort &who : probes) {
            const Norm *n = norms().find(measureId, ctx, who);
            if (n == nullptr) continue;

            out.norm       = n;
            out.contextId  = ctx;
            out.inherited  = (ctx != requested);
            // Where the row that WON came from, keyed on the context AND cohort it was actually
            // found at — not on the ones asked for. A driver inheriting a shipped full-swing
            // corridor is not edited, and a driver inheriting the user's full-swing override is.
            out.overridden = isOverridden(measureId, ctx, who);
            return out;
        }
    }
    return out;
}

QStringList INormProvider::overriddenContextsFor(const QString &measureId) const
{
    QStringList out;
    if (measureId.isEmpty())
        return out;

    // Cohort-blind on purpose: this answers "which contexts does this measure have rows of its own
    // at", and a context carrying a male row and a female row is still one entry in an indented
    // list of contexts. contextsFor() already returns each context once.
    const QStringList own = norms().contextsFor(measureId);

    // Tree order, not pack order — this drives an indented list, and a list that jumps around as
    // rows are added would be unreadable.
    for (const QString &ctx : contexts().inOrder())
        if (own.contains(ctx)) out.append(ctx);
    return out;
}

const Norm *INormProvider::shippedNorm(const QString &measureId, const QString &contextId,
                                       const Cohort &cohort) const
{
    // A leaf provider IS one layer, so it answers for itself: a core leaf carries the shipped row,
    // a user leaf carries nothing shipped at all. Only the merged provider has to look further.
    if (origin() != PackOrigin::Core)
        return nullptr;
    return norms().find(measureId, contextId, cohort);
}

bool INormProvider::isOverridden(const QString &, const QString &, const Cohort &) const
{
    // Same reasoning inverted: a leaf's rows all come from its own layer, so a core leaf overrides
    // nothing and a user leaf's rows are, by definition, the user's.
    return origin() != PackOrigin::Core;
}

std::vector<NormSetInfo> INormProvider::layers() const
{
    // A leaf provider IS its own single layer. Only an assembling provider has anything else to
    // say, and it overrides this.
    //
    // The pack's OWN id names it, not the provider's label: a leaf provider labels itself with
    // where it was read from (":/diagnostics/norms.json", or a directory path), and a file path is
    // not what a norm set is called.
    const NormPack &p = norms();
    const QString   name = p.id.isEmpty() ? label() : p.id;
    return { NormSetInfo{ name, name, origin(), int(p.norms.size()), p.readOnly } };
}

namespace {

// The shipped core norm set and context tree. The runtime copies are Qt resources; the SOURCE OF
// TRUTH is the reviewable JSON committed at src/Resources/diagnostics/, from which the resources
// are built. That split is what lets a community contribution arrive as a pull request against
// readable content rather than a binary blob.
class ResourceNormProvider final : public detail::NormLeaf {
public:
    ResourceNormProvider(const QString &normsPath, const QString &contextsPath)
    {
        // Contexts FIRST, and the order is deliberate rather than incidental: the tree is what the
        // norm set's rows are referentially checked against, so a report that lists the tree's own
        // faults before the corridors that hang on them reads in the order an author would fix them.
        //
        // Hand-written where the norm set beside it is loadShipped()'s, because this is the SECOND
        // shipped file and the leaf skeleton knows about one. Both halves still go through the same
        // env seam and the same open-or-report — see provider_leaf_p.h; what is spelled here is only
        // the sequencing and the tree's own loader.
        const QString cp = detail::overridePath(detail::NormTraits::kContextsEnv, contextsPath);
        if (const auto data = detail::readShipped(cp, QStringLiteral("context tree"), m_report)) {
            ContextTreeLoadResult res = loadContextTree(*data, cp);
            m_contexts                = std::move(res.tree);
            detail::appendIssues(m_report, res.report);
        }

        // PINPOINT_CORE_NORMS, the label, the read-only mark and the Core origin all come with this.
        loadShipped(normsPath);
    }
};

} // namespace

std::unique_ptr<INormProvider> makeResourceNormProvider(const QString &normsPath,
                                                        const QString &contextsPath)
{
    return std::make_unique<ResourceNormProvider>(normsPath, contextsPath);
}

} // namespace pinpoint::analysis
