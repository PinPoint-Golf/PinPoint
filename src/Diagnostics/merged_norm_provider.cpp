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

#include <QSet>

#include <algorithm>

namespace pinpoint::analysis {

namespace {

// Core + user norm sets as one set, mirroring MergedPackProvider.
//
// Collision policy follows PackOrigin for the same reasons documented there:
//   LocalUser — the user's own deliberate edits, so they REPLACE a core row at the same
//               (measureId, contextId). Not namespaced, because a namespace would make overriding
//               impossible, which is the entire point of the layer.
//   Community — imported from someone else. CORE WINS on collision, and the loser is reported, so
//               a downloaded set cannot silently redefine a shipped norm under a user who has no
//               way to tell which one they are looking at.
//
// Context specificity beats layer precedence — see the note on makeMergedNormProvider(). It falls
// out of assembling one set and resolving over it, rather than resolving per layer.
class MergedNormProvider final : public INormProvider {
public:
    MergedNormProvider(std::unique_ptr<INormProvider>              core,
                       std::vector<std::unique_ptr<INormProvider>> user,
                       const QStringList                          &disabled)
        : m_core(std::move(core)), m_user(std::move(user)), m_disabled(disabled)
    {
        // A disabled layer is dropped HERE, before anything of it is merged. Filtering later (at
        // resolve time, or by removing its rows afterwards) would leave its contexts in the tree
        // and its validation issues in the report — a set that is off would still be shaping the
        // library, which is not what "off" means.
        if (m_core && isDisabled(*m_core)) m_core.reset();
        m_user.erase(std::remove_if(m_user.begin(), m_user.end(),
                                    [this](const std::unique_ptr<INormProvider> &p) {
                                        return !p || isDisabled(*p);
                                    }),
                     m_user.end());

        std::vector<ContextNode> nodes;

        if (m_core) {
            m_norms = m_core->norms();
            for (const ValidationIssue &i : m_core->report().issues) m_report.issues.push_back(i);
            nodes = m_core->contexts().nodes();
        }
        m_norms.id       = QStringLiteral("merged");
        m_norms.readOnly = false;

        for (const auto &up : m_user) {
            if (!up) continue;
            for (const ValidationIssue &i : up->report().issues) m_report.issues.push_back(i);
            mergeNorms(up->norms(), up->origin());
            mergeContexts(up->contexts(), nodes);
        }

        m_contexts = ContextTree(std::move(nodes));

        // Re-validate the assembled set. Cross-layer collisions can produce a duplicate that
        // neither set contained on its own, and a user row can reference a context only their own
        // tree defines — exactly the cases a per-set check cannot see.
        const ValidationReport combined = validateNormPack(m_norms);
        for (const ValidationIssue &i : combined.issues) m_report.issues.push_back(i);

        const ValidationReport ctx = validateContextTree(m_contexts);
        for (const ValidationIssue &i : ctx.issues) m_report.issues.push_back(i);
    }

    const NormPack         &norms() const override { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override { return m_report; }
    QString                 label() const override { return QStringLiteral("merged"); }
    PackOrigin              origin() const override { return PackOrigin::Core; }

    // The shipped row, from the core layer only. Null when core carries nothing at this key — the
    // case where dropping a user override means "inherit from the parent" rather than "go back to
    // what shipped", which is a different promise for a button to make.
    const Norm *shippedNorm(const QString &measureId, const QString &contextId,
                            const Cohort &cohort) const override
    {
        return m_core ? m_core->norms().find(measureId, contextId, cohort) : nullptr;
    }

    // Recorded at merge time, not derived by comparing values: a user row holding exactly the
    // shipped numbers is still the user's row, and a value comparison would quietly un-mark it the
    // moment someone dragged a handle back to where it started.
    bool isOverridden(const QString &measureId, const QString &contextId,
                      const Cohort &cohort) const override
    {
        return m_overridden.contains(key(measureId, contextId, cohort));
    }

    // The CHILDREN, shipped first — never this object. "merged" is an implementation word, and a
    // user looking at the norm-set list needs to see the shipped set and their own as separate
    // things: that separation is what the override relationship between them means.
    std::vector<NormSetInfo> layers() const override
    {
        std::vector<NormSetInfo> out;
        if (m_core)
            for (const NormSetInfo &i : m_core->layers()) out.push_back(i);
        for (const auto &up : m_user)
            if (up)
                for (const NormSetInfo &i : up->layers()) out.push_back(i);
        return out;
    }

private:
    // A machine key, not a label — '\n' cannot occur in an id, so nothing needs escaping. The cohort
    // is part of it: a user row qualified to one cohort overrides the shipped row for THAT cohort,
    // and marking the unqualified row beside it as overridden would put "you changed this" on a
    // corridor nobody touched.
    static QString key(const QString &measureId, const QString &contextId, const Cohort &cohort)
    {
        return measureId + QLatin1Char('\n') + contextId + QLatin1Char('\n')
               + (cohort.sex.has_value() ? sexName(*cohort.sex) : QString())
               + QLatin1Char('\n')
               + (cohort.age.has_value() ? ageBandName(*cohort.age) : QString());
    }

    // A provider is disabled when EVERY layer it reports is disabled. Written against layers()
    // rather than the pack id so a nested assembly answers correctly too, and so the ids the UI
    // shows are exactly the ids the switch acts on — a selector that named layers differently from
    // the thing it disabled would be unusable the first time the two disagreed.
    bool isDisabled(const INormProvider &p) const
    {
        const std::vector<NormSetInfo> ls = p.layers();
        if (ls.empty())
            return false;                 // reports no layer at all; nothing to switch off
        for (const NormSetInfo &l : ls)
            if (!m_disabled.contains(l.id)) return false;
        return true;
    }

    void mergeNorms(const NormPack &src, PackOrigin origin)
    {
        const bool isLocal = (origin == PackOrigin::LocalUser);

        for (const Norm &n : src.norms) {
            if (n.measureId.isEmpty() || n.contextId.isEmpty())
                continue;

            const bool collides = m_norms.contains(n.measureId, n.contextId, n.cohort);
            if (collides && !isLocal) {
                m_report.issues.push_back(ValidationIssue{
                    IssueSeverity::Warning, QStringLiteral("duplicateNorm"), normKeyLabel(n),
                    QStringLiteral("A community norm set redefines the norm '%1'; the shipped norm "
                                   "wins.").arg(normKeyLabel(n)) });
                continue;
            }
            m_norms.upsert(n);   // replaces in place, preserving order
            if (isLocal) m_overridden.insert(key(n.measureId, n.contextId, n.cohort));
        }
    }

    // Contexts are additive across layers: a user set may name a club the shipped tree does not,
    // but must never silently re-parent a shipped node — that would move every norm beneath it.
    void mergeContexts(const ContextTree &src, std::vector<ContextNode> &into)
    {
        QSet<QString> have;
        for (const ContextNode &n : into) have.insert(n.id);

        for (const ContextNode &n : src.nodes()) {
            if (n.id.isEmpty()) continue;
            if (have.contains(n.id)) {
                const auto it = std::find_if(into.begin(), into.end(),
                                             [&](const ContextNode &e) { return e.id == n.id; });
                if (it != into.end() && it->parentId != n.parentId)
                    m_report.issues.push_back(ValidationIssue{
                        IssueSeverity::Warning, QStringLiteral("contextReparented"), n.id,
                        QStringLiteral("A norm set re-parents context '%1'; the shipped placement "
                                       "wins.").arg(n.id) });
                continue;
            }
            have.insert(n.id);
            into.push_back(n);
        }
    }

    std::unique_ptr<INormProvider>              m_core;
    std::vector<std::unique_ptr<INormProvider>> m_user;
    QStringList                                 m_disabled;
    QSet<QString>                               m_overridden;   // keys a LocalUser layer supplied
    NormPack                                    m_norms;
    ContextTree                                 m_contexts;
    ValidationReport                            m_report;
};

} // namespace

std::unique_ptr<INormProvider> makeMergedNormProvider(
    std::unique_ptr<INormProvider>              core,
    std::vector<std::unique_ptr<INormProvider>> user,
    const QStringList                          &disabled)
{
    return std::make_unique<MergedNormProvider>(std::move(core), std::move(user), disabled);
}

std::unique_ptr<INormProvider> makeNormProvider()
{
    std::vector<std::unique_ptr<INormProvider>> user;
    user.push_back(makeFileNormProvider());
    return makeMergedNormProvider(makeResourceNormProvider(), std::move(user));
}

namespace {
// Function-local static, so nothing runs before main(). See the header for why this is cached at
// all: the compiled table it replaced was free, and callers build a band provider per lookup.
std::shared_ptr<const INormProvider> &sharedSlot()
{
    static std::shared_ptr<const INormProvider> slot;
    return slot;
}
} // namespace

std::shared_ptr<const INormProvider> sharedNormProvider()
{
    std::shared_ptr<const INormProvider> &slot = sharedSlot();
    if (!slot)
        slot = std::shared_ptr<const INormProvider>(makeNormProvider().release());
    return slot;
}

void resetSharedNormProvider()
{
    // Existing shared_ptr holders keep the old set alive until they release it, so a reset during
    // an in-flight read cannot pull the rug out — it just means that reader finishes on the norms
    // it started with, which is the correct outcome for a single analysis pass.
    sharedSlot().reset();
}

} // namespace pinpoint::analysis
