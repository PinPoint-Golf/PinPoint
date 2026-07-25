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

#include <QSet>

namespace pinpoint::analysis {

namespace {

// Core + user packs as one library.
//
// Collision policy: user entities are namespaced with "<packId>:", and on a surviving collision
// CORE WINS. That asymmetry is deliberate — a community pack must not be able to silently redefine
// a shipped characteristic, because a user reading the detail page would have no way to tell which
// definition they were looking at. The loser is reported so the pack author can see it happened.
class MergedPackProvider final : public ICharacteristicPackProvider {
public:
    MergedPackProvider(std::unique_ptr<ICharacteristicPackProvider>              core,
                       std::vector<std::unique_ptr<ICharacteristicPackProvider>> user)
        : m_core(std::move(core)), m_user(std::move(user))
    {
        if (m_core) {
            m_pack = m_core->pack();
            for (const ValidationIssue &i : m_core->report().issues) m_report.issues.push_back(i);
        }
        m_pack.id       = QStringLiteral("merged");
        m_pack.readOnly = false;

        for (const auto &up : m_user) {
            if (!up) continue;
            for (const ValidationIssue &i : up->report().issues) m_report.issues.push_back(i);
            mergeOne(up->pack());
        }

        // Re-validate the assembled library: cross-pack edges can create cycles that neither pack
        // contained on its own, and that is exactly the case a per-pack check cannot see.
        const ValidationReport combined = validatePack(m_pack);
        for (const ValidationIssue &i : combined.issues) m_report.issues.push_back(i);
    }

    const CharacteristicPack &pack() const override { return m_pack; }
    const ValidationReport   &report() const override { return m_report; }
    QString                   label() const override { return QStringLiteral("merged"); }

private:
    QString qualify(const QString &packId, const QString &id) const
    {
        if (id.isEmpty() || id.contains(QLatin1Char(':'))) return id;
        return packId + QLatin1Char(':') + id;
    }

    void mergeOne(const CharacteristicPack &src)
    {
        if (src.id.isEmpty()) return;

        QSet<QString> measureIds, signalIds, conditionIds;
        for (const Measure &m : m_pack.measures)     measureIds.insert(m.id);
        for (const Signal &s : m_pack.signalDefs)    signalIds.insert(s.id);
        for (const Condition &c : m_pack.conditions) conditionIds.insert(c.id);

        auto collide = [this](const QString &id, const QString &kind) {
            m_report.issues.push_back(ValidationIssue{
                IssueSeverity::Warning, QStringLiteral("duplicateId"), id,
                QStringLiteral("A user pack redefines %1 '%2'; the shipped definition wins.")
                    .arg(kind, id) });
        };

        for (Measure m : src.measures) {
            m.id = qualify(src.id, m.id);
            if (measureIds.contains(m.id)) { collide(m.id, QStringLiteral("measure")); continue; }
            m_pack.measures.push_back(std::move(m));
        }

        for (Signal s : src.signalDefs) {
            s.id = qualify(src.id, s.id);
            for (QString &mid : s.measures) mid = qualify(src.id, mid);
            if (signalIds.contains(s.id)) { collide(s.id, QStringLiteral("signal")); continue; }
            m_pack.signalDefs.push_back(std::move(s));
        }

        for (Condition c : src.conditions) {
            c.id = qualify(src.id, c.id);
            for (QString &sid : c.detectedBy) sid = qualify(src.id, sid);
            if (!c.axis.isEmpty())      c.axis = qualify(src.id, c.axis);
            if (!c.supersededBy.isEmpty()) c.supersededBy = qualify(src.id, c.supersededBy);
            if (conditionIds.contains(c.id)) { collide(c.id, QStringLiteral("condition")); continue; }
            m_pack.conditions.push_back(std::move(c));
        }

        // Edges may legitimately point at CORE conditions — a community pack adding a cause for a
        // shipped characteristic is the main thing community packs are for. So an endpoint is
        // qualified only when the unqualified id is not already a core condition.
        for (Edge e : src.edges) {
            e.from = conditionIds.contains(e.from) ? e.from : qualify(src.id, e.from);
            e.to   = conditionIds.contains(e.to) ? e.to : qualify(src.id, e.to);
            m_pack.edges.push_back(std::move(e));
        }
    }

    std::unique_ptr<ICharacteristicPackProvider>              m_core;
    std::vector<std::unique_ptr<ICharacteristicPackProvider>> m_user;
    CharacteristicPack                                        m_pack;
    ValidationReport                                          m_report;
};

} // namespace

std::unique_ptr<ICharacteristicPackProvider> makeMergedPackProvider(
    std::unique_ptr<ICharacteristicPackProvider>              core,
    std::vector<std::unique_ptr<ICharacteristicPackProvider>> user)
{
    return std::make_unique<MergedPackProvider>(std::move(core), std::move(user));
}

std::unique_ptr<ICharacteristicPackProvider> makeCharacteristicPackProvider()
{
    std::vector<std::unique_ptr<ICharacteristicPackProvider>> user;
    user.push_back(makeFilePackProvider());
    return makeMergedPackProvider(makeResourcePackProvider(), std::move(user));
}

} // namespace pinpoint::analysis
