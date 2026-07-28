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

#include <QFile>

#include <optional>

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
class ResourceNormProvider final : public INormProvider {
public:
    ResourceNormProvider(const QString &normsPath, const QString &contextsPath)
    {
        // Tooling/test seam, mirroring PINPOINT_CORE_PACK: the Qt resource only exists inside the
        // app binary, so a standalone test or an offline tool has no way to reach the shipped
        // content. Unset in every normal run.
        const QString np = overridePath("PINPOINT_CORE_NORMS", normsPath);
        const QString cp = overridePath("PINPOINT_CORE_CONTEXTS", contextsPath);
        m_label = np;

        // Contexts first: the norm set's referential validation needs the tree.
        if (const auto data = readAll(cp, QStringLiteral("context tree"))) {
            ContextTreeLoadResult res = loadContextTree(*data, cp);
            m_contexts = std::move(res.tree);
            appendIssues(res.report);
        }

        if (const auto data = readAll(np, QStringLiteral("norm set"))) {
            NormPackLoadResult res = loadNormPack(*data, np);
            m_norms          = std::move(res.pack);
            m_norms.readOnly = true;   // the shipped set is never edited in place
            appendIssues(res.report);
        }
    }

    const NormPack         &norms() const override { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override { return m_report; }
    QString                 label() const override { return m_label; }
    PackOrigin              origin() const override { return PackOrigin::Core; }

private:
    static QString overridePath(const char *env, const QString &fallback)
    {
        const QByteArray ov = qgetenv(env);
        return ov.isEmpty() ? fallback : QString::fromLocal8Bit(ov);
    }

    std::optional<QByteArray> readAll(const QString &path, const QString &what)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            m_report.issues.push_back(ValidationIssue{
                IssueSeverity::Error, QStringLiteral("parse"), path,
                QStringLiteral("Could not open the core %1 at '%2'.").arg(what, path) });
            return std::nullopt;
        }
        return f.readAll();
    }

    void appendIssues(const ValidationReport &r)
    {
        for (const ValidationIssue &i : r.issues) m_report.issues.push_back(i);
    }

    NormPack         m_norms;
    ContextTree      m_contexts;
    ValidationReport m_report;
    QString          m_label;
};

} // namespace

std::unique_ptr<INormProvider> makeResourceNormProvider(const QString &normsPath,
                                                        const QString &contextsPath)
{
    return std::make_unique<ResourceNormProvider>(normsPath, contextsPath);
}

} // namespace pinpoint::analysis
