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
                       std::vector<std::unique_ptr<INormProvider>> user)
        : m_core(std::move(core)), m_user(std::move(user))
    {
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

private:
    void mergeNorms(const NormPack &src, PackOrigin origin)
    {
        const bool isLocal = (origin == PackOrigin::LocalUser);

        for (const Norm &n : src.norms) {
            if (n.measureId.isEmpty() || n.contextId.isEmpty())
                continue;

            const bool collides = m_norms.contains(n.measureId, n.contextId);
            if (collides && !isLocal) {
                m_report.issues.push_back(ValidationIssue{
                    IssueSeverity::Warning, QStringLiteral("duplicateNorm"),
                    QStringLiteral("%1 @ %2").arg(n.measureId, n.contextId),
                    QStringLiteral("A community norm set redefines the norm for '%1' in context "
                                   "'%2'; the shipped norm wins.")
                        .arg(n.measureId, n.contextId) });
                continue;
            }
            m_norms.upsert(n);   // replaces in place, preserving order
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
    NormPack                                    m_norms;
    ContextTree                                 m_contexts;
    ValidationReport                            m_report;
};

} // namespace

std::unique_ptr<INormProvider> makeMergedNormProvider(
    std::unique_ptr<INormProvider>              core,
    std::vector<std::unique_ptr<INormProvider>> user)
{
    return std::make_unique<MergedNormProvider>(std::move(core), std::move(user));
}

std::unique_ptr<INormProvider> makeNormProvider()
{
    std::vector<std::unique_ptr<INormProvider>> user;
    user.push_back(makeFileNormProvider());
    return makeMergedNormProvider(makeResourceNormProvider(), std::move(user));
}

namespace {
// Function-local static, so nothing runs before main(). See the header for why this is cached at
// all: the compiled table it replaces was free, and MetricCatalogue::corridor() builds a band
// provider per call.
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
