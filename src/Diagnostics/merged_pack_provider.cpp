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

#include <algorithm>
#include <cstddef>

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
            mergeOne(up->pack(), up->origin());
        }

        // Re-validate the assembled library: cross-pack edges can create cycles that neither pack
        // contained on its own, and that is exactly the case a per-pack check cannot see.
        //
        // Through appendUnreported(), not a plain append, and the difference is the whole reason
        // this line is interesting. The layer reports above ALREADY carry each pack's standalone
        // validatePack() findings, and most of them survive the merge unchanged — an uncited tier on
        // a shipped condition is still an uncited tier afterwards. Appending the assembled report
        // wholesale therefore listed every one of them twice, so the health list's counts were
        // roughly double the number of things actually wrong.
        //
        // What must NOT be lost is the other half: this pass is the authoritative check for
        // cross-pack referential integrity, which is why file_pack_provider drops the referential
        // codes from its own layer report (see the note there — an overlay's edges point at core
        // content it does not contain, so standalone referential integrity is meaningless for it).
        // Those findings have never been said by a layer, so they land here, which is exactly the
        // asymmetry appendUnreported()'s (code, subject) key preserves.
        //
        // One wrinkle the (code, subject) key cannot see on its own: a COMMUNITY layer's entities
        // are renamed on the way in. Its own report says `c_flip`, because that is what the file
        // calls it and that is the name its author has to search for; the assembled library calls
        // the same thing `theirpack:c_flip`. Two different subjects, one finding, and the dedupe
        // would keep both. So the already-said set is seeded with the qualified spelling too — the
        // merge is the only thing that knows the mapping, and it knows it exactly, because
        // qualify() is the same function that did the renaming. Nothing is REPORTED under the
        // alias; it only suppresses the second telling.
        ValidationReport said = m_report;
        for (const auto &up : m_user) {
            if (!up || up->origin() == PackOrigin::LocalUser) continue;   // LocalUser ids are verbatim
            for (const ValidationIssue &i : up->report().issues) {
                ValidationIssue alias = i;
                alias.subject = qualify(up->pack().id, i.subject);
                if (alias.subject != i.subject) said.issues.push_back(alias);
            }
        }

        const std::size_t base = said.issues.size();
        appendUnreported(said, validatePack(m_pack));
        m_report.issues.insert(m_report.issues.end(),
                               said.issues.begin() + std::ptrdiff_t(base), said.issues.end());
    }

    const CharacteristicPack &pack() const override { return m_pack; }
    const ValidationReport   &report() const override { return m_report; }

    // "merged" and Core are the ASSEMBLY's own name for itself, and they stay. They are what a
    // provider is called when something asks the assembly what it is — the id stamped on the
    // library, matching m_pack.id above — and Core is right because the assembly is read-only-ish
    // by construction: it is not any single layer, so no writer can be pointed at it.
    //
    // They are NOT a census, and the temptation to fix them into one is what layers() exists to
    // remove. A user reading a list of installed packs must never see the word "merged": they need
    // the shipped library and their own as separate rows, because that separation is what the
    // override relationship between them means. Making label() return, say, the core layer's label
    // would half-answer that — it would name one layer and silently drop the rest, which is worse
    // than an implementation word a UI has no reason to render. Exactly the resolution the norm
    // side reached; MergedNormProvider::label() still returns "merged" beside its own layers().
    QString    label() const override { return QStringLiteral("merged"); }
    PackOrigin origin() const override { return PackOrigin::Core; }

    // The CHILDREN, shipped first — never this object. The counterpart of
    // MergedNormProvider::layers(), and the same doctrine: one row per layer, named by the pack's
    // own id, carrying that layer's OWN counts rather than the assembly's. A layer that read
    // nothing reports no row (see FilePackProvider::layers()), so an installation with no user
    // packs is a census of one rather than a census with a blank in it.
    std::vector<PackLayerInfo> layers() const override
    {
        std::vector<PackLayerInfo> out;
        if (m_core)
            for (const PackLayerInfo &i : m_core->layers()) out.push_back(i);
        for (const auto &up : m_user)
            if (up)
                for (const PackLayerInfo &i : up->layers()) out.push_back(i);
        return out;
    }

private:
    QString qualify(const QString &packId, const QString &id) const
    {
        if (id.isEmpty() || id.contains(QLatin1Char(':'))) return id;
        return packId + QLatin1Char(':') + id;
    }

    // Replace an existing entity in place, preserving pack order so the library does not reshuffle
    // when a user overrides one shipped characteristic.
    template <typename T>
    static bool replaceById(std::vector<T> &v, const T &item)
    {
        for (T &existing : v)
            if (existing.id == item.id) { existing = item; return true; }
        return false;
    }

    void mergeOne(const CharacteristicPack &src, PackOrigin origin)
    {
        if (src.id.isEmpty()) return;
        const bool isLocal = (origin == PackOrigin::LocalUser);

        QSet<QString> measureIds, signalIds, conditionIds;
        for (const Measure &m : m_pack.measures)     measureIds.insert(m.id);
        for (const Signal &s : m_pack.signalDefs)    signalIds.insert(s.id);
        for (const Condition &c : m_pack.conditions) conditionIds.insert(c.id);

        auto collide = [this](const QString &id, const QString &kind) {
            m_report.issues.push_back(ValidationIssue{
                IssueSeverity::Warning, QStringLiteral("duplicateId"), id,
                QStringLiteral("A community pack redefines %1 '%2'; the shipped definition wins.")
                    .arg(kind, id) });
        };

        // LocalUser ids are kept verbatim so an override can match a core id. Community ids are
        // namespaced so they cannot collide with core in the first place.
        auto id_ = [&](const QString &id) { return isLocal ? id : qualify(src.id, id); };

        for (Measure m : src.measures) {
            m.id = id_(m.id);
            if (measureIds.contains(m.id)) {
                if (isLocal) { replaceById(m_pack.measures, m); continue; }
                collide(m.id, QStringLiteral("measure"));
                continue;
            }
            m_pack.measures.push_back(std::move(m));
        }

        for (Signal s : src.signalDefs) {
            s.id = id_(s.id);
            for (QString &mid : s.measures) mid = id_(mid);
            if (signalIds.contains(s.id)) {
                if (isLocal) { replaceById(m_pack.signalDefs, s); continue; }
                collide(s.id, QStringLiteral("signal"));
                continue;
            }
            m_pack.signalDefs.push_back(std::move(s));
        }

        for (Condition c : src.conditions) {
            c.id = id_(c.id);
            for (QString &sid : c.detectedBy) sid = id_(sid);
            if (!c.axis.isEmpty())         c.axis = id_(c.axis);
            if (!c.supersededBy.isEmpty()) c.supersededBy = id_(c.supersededBy);
            if (conditionIds.contains(c.id)) {
                if (isLocal) { replaceById(m_pack.conditions, c); continue; }
                collide(c.id, QStringLiteral("condition"));
                continue;
            }
            m_pack.conditions.push_back(std::move(c));
        }

        // Edges may legitimately point at CORE conditions — a user or community pack adding a cause
        // for a shipped characteristic is the main thing extra packs are for. So an endpoint is
        // qualified only when the unqualified id is not already a known condition.
        //
        // A LocalUser pack REPLACES the CAUSAL edge set for any condition it names as an effect:
        // otherwise removing a cause in the editor could never take effect, because the core edge
        // would survive alongside the user's edited list.
        //
        // Scoped to `Causes` on BOTH sides, and the scope is load-bearing in a way that is easy to
        // miss. The user's editor writes its causal edges as a whole set keyed on the effect, which
        // is what justifies the wholesale erase — but a symmetric edge is written INDIVIDUALLY, by
        // linkRelation, and belongs to neither end. Unscoped, a user corroborates edge naming B
        // would silently erase every SHIPPED CAUSE of B from the assembled library, because B
        // appeared in the rewritten set. Nothing downstream could detect that: the library would
        // simply have fewer explanations than it shipped with.
        if (isLocal) {
            QSet<QString> rewrittenEffects;
            for (const Edge &e : src.edges)
                if (e.type == EdgeType::Causes) rewrittenEffects.insert(e.to);
            m_pack.edges.erase(std::remove_if(m_pack.edges.begin(), m_pack.edges.end(),
                                              [&](const Edge &e) {
                                                  return e.type == EdgeType::Causes
                                                         && rewrittenEffects.contains(e.to);
                                              }),
                               m_pack.edges.end());

            // A symmetric edge overrides per PAIR instead. That is what lets the user change what a
            // shipped relation SAYS — corroborates to excludes — without ending up with both rows
            // in the assembled library, each contradicting the other.
            for (const Edge &u : src.edges) {
                if (u.type == EdgeType::Causes) continue;
                m_pack.edges.erase(
                    std::remove_if(m_pack.edges.begin(), m_pack.edges.end(),
                                   [&](const Edge &e) {
                                       return e.type != EdgeType::Causes
                                              && ((e.from == u.from && e.to == u.to)
                                                  || (e.from == u.to && e.to == u.from));
                                   }),
                    m_pack.edges.end());
            }
        }
        for (Edge e : src.edges) {
            e.from = conditionIds.contains(e.from) ? e.from : id_(e.from);
            e.to   = conditionIds.contains(e.to) ? e.to : id_(e.to);
            m_pack.edges.push_back(std::move(e));
        }

        // Tombstones, applied LAST so a pack can retire an edge and state a replacement in one
        // file without the retirement eating its own new row. Only a LocalUser pack may retire:
        // a community pack silently deleting a shipped relationship is the collision policy this
        // provider exists to prevent, one step further.
        if (isLocal) {
            for (const Edge &t : src.retiredEdges) {
                const bool symmetric = t.type != EdgeType::Causes;
                m_pack.edges.erase(
                    std::remove_if(m_pack.edges.begin(), m_pack.edges.end(),
                                   [&](const Edge &e) {
                                       if (e.type != t.type) return false;
                                       // Symmetric edges match unordered: the retirement means the
                                       // same whichever way round either row was written.
                                       return (e.from == t.from && e.to == t.to)
                                              || (symmetric && e.from == t.to && e.to == t.from);
                                   }),
                    m_pack.edges.end());
            }
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
    // ONE PROVIDER PER PACK FILE. This used to construct a single file provider over the whole
    // directory, which could only ever hold one pack — so a second installed pack was opened,
    // parsed, had its validation issues added to the health list, and then had every characteristic
    // in it dropped on the floor. The report said the pack was there; the library did not contain
    // it, and nothing said so.
    //
    // makeFilePackProviders() decides the order and the origin of each layer; see its comment for
    // why `user.json` leads and why everything else is Community. MergedPackProvider has always
    // taken a vector, so no generalisation was needed there — the contract simply had nobody
    // fulfilling it.
    return makeMergedPackProvider(makeResourcePackProvider(), makeFilePackProviders());
}

} // namespace pinpoint::analysis
