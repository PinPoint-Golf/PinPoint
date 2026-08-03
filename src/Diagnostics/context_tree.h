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

#pragma once

#include "characteristic_pack.h"   // ValidationReport

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

// The context tree — what kind of shot a norm applies to.
//
// This is the mechanism that lets one measure hold genuinely different norms for a driver and a
// wedge without duplicating the measure, and it is why norms key on (measureId, contextId) rather
// than carrying a club field. Ball position is the motivating case: a wedge sits near the middle of
// the stance and a driver off the lead heel, so a single corridor is right for one club and
// misleading-red for every other.
//
// Resolution WALKS UP the tree. An author writes a row only where the value genuinely differs from
// its parent; everything else inherits. That is what keeps a norm set readable — a reader can see
// at a glance which contexts were deliberately distinguished, because those are the only ones with
// their own row.
//
// Contexts are user-editable content (add a node, set its parent). Anatomy roles are not — they map
// to keypoint indices and are not opinions.

namespace pinpoint::analysis {

// The context an unstated binding means. A shot that declares no context resolves here and is
// marked inferred by the caller; see the engine's confidence demotion.
inline const QString &kDefaultContextId()
{
    static const QString id = QStringLiteral("full_swing");
    return id;
}

struct ContextNode {
    QString id;         // stable, never reused
    QString label;
    QString parentId;   // empty => a root
};

// A parsed, validated forest. Several roots are legal and expected: `any`, `partial`, `bunker` and
// `specialty` are siblings at the top, not children of one another.
class ContextTree {
public:
    ContextTree() = default;
    explicit ContextTree(std::vector<ContextNode> nodes) : m_nodes(std::move(nodes)) {}

    const std::vector<ContextNode> &nodes() const { return m_nodes; }
    bool                            isEmpty() const { return m_nodes.empty(); }
    const ContextNode              *node(const QString &id) const;
    bool                            contains(const QString &id) const { return node(id) != nullptr; }

    // The resolution chain for `id`, nearest first, INCLUDING `id` itself. A norm lookup tries each
    // in turn and takes the first hit — driver, then full_swing, then any, then none.
    //
    // An unknown id yields an empty chain rather than falling back to the default: silently grading
    // an unrecognised context against the full-swing norm would be a wrong answer wearing a right
    // answer's clothes. The caller decides what to do about it.
    QStringList chain(const QString &id) const;

    // Depth from this node's root, 0-based. Drives the indentation in the norms-by-context list.
    int depth(const QString &id) const;

    // Every node in tree order — roots in declaration order, each followed by its subtree. This is
    // the order the UI renders, so it lives here rather than being re-derived per view.
    std::vector<QString> inOrder() const;

    // Direct children of `id`, in declaration order. Empty `id` yields the roots.
    QStringList children(const QString &id) const;

    // True when `ancestorId` is on `id`'s chain (excluding `id` itself).
    bool isDescendantOf(const QString &id, const QString &ancestorId) const;

private:
    std::vector<ContextNode> m_nodes;
};

// ── Binding resolution ──────────────────────────────────────────────────────
//
// Where a CONDITION applies, resolved the same way a norm is: walk up the chain, nearest row wins.
// One rule for both, because two rules would eventually disagree about what "inherited" means and
// only one of them would be the one the UI drew.
//
// The default when nothing on the chain carries a row is APPLICABLE and MATERIAL. That is not a
// convenience — it is what makes a binding an exception rather than a declaration, and it is why
// the shipped pack carries no binding rows at all while every condition applies everywhere.
//
// An UNKNOWN context resolves to the default with `found == false`, matching ContextTree::chain()'s
// refusal to guess. A shot whose context this tree does not recognise is not evidence that a
// condition was deliberately switched off.
struct BindingResolution {
    bool    applicable = true;
    bool    material   = true;
    QString contextId;            // where the row was found; empty when nothing resolved
    bool    inherited  = false;   // the row came from an ancestor, not the requested context
    bool    found      = false;   // a row exists somewhere on the chain
};

BindingResolution resolveContextBinding(const Condition &c, const ContextTree &tree,
                                        const QString &contextId);

// The condition's OWN row at this exact context id, ignoring the chain. Null when it inherits.
// This is the "is this yours or the parent's?" question the editor's checkboxes rest on, and it is
// deliberately separate from resolution — they differ exactly where inheritance does.
const ContextBinding *ownContextBinding(const Condition &c, const QString &contextId);

// ── What context a SHOT is in ───────────────────────────────────────────────
//
// The club a swing was hit with, as a node in this tree. A swing.json records the club (the token
// vocabulary in Core/club_vocabulary.h) and no context id at all, so something has to bridge the
// two — and it is here, once, rather than at each surface that grades a swing. Ball position is the
// case that makes it matter: the shipped pack authors five corridors for it (any, driver,
// fairway_wood, iron, wedge), and reading a driver against the full-swing row throws all five away.
//
// It resolves to the MOST SPECIFIC node the tree carries — `iron_7`, not `iron` — and the walk does
// the rest. That is the point of the second storey: a measure that genuinely varies club by club
// says so with a club row, one that does not writes a family row, and both are reached by the same
// lookup. Returning the family here instead would make per-club corridors unauthorable, since
// nothing would ever ask for one.
//
// Judgement calls, stated rather than buried:
//   · a HYBRID resolves to one `hybrid` node rather than to a node per number. The figures that
//     would distinguish a 3 from a 4 hybrid do not exist, and a node whose corridor can only ever
//     be its parent's is a row an author has to read and dismiss.
//   · a PUTTER resolves to `putt`, which hangs off `any` and NOT off full_swing. This used to
//     return the default, so a putt was graded against corridors authored for a full swing — the
//     clubhead-speed rows would have called every putt a fault.
//   · an iron outside 3–9 resolves to the family. Not a fallback: it inherits every row the family
//     carries and grades normally, it is simply not distinguished.
//
// An unrecognised or empty club yields the default — the same answer a shot that declares no
// context gets, which is what kDefaultContextId() is for. This never returns a node the shipped
// tree lacks, so a caller may resolve against it without checking; `context_tree_test` sweeps the
// whole club vocabulary against the shipped tree to keep that true.
QString contextIdForClub(const QString &club);

// ── Validation ──────────────────────────────────────────────────────────────
//
// ERRORS:
//   duplicateContextId   two nodes share an id
//   unknownParent        parentId names a node that does not exist
//   contextCycle         a node is its own ancestor
//   emptyContextId       a node with no id
ValidationReport validateContextTree(const ContextTree &tree);

// ── Persistence ─────────────────────────────────────────────────────────────
struct ContextTreeLoadResult {
    ContextTree      tree;
    ValidationReport report;
    bool             loaded = false;   // parsed AND validated clean
    bool             parsed = false;
};

ContextTreeLoadResult loadContextTree(const QJsonObject &root, const QString &sourceLabel = QString());
ContextTreeLoadResult loadContextTree(const QByteArray &json, const QString &sourceLabel = QString());

QJsonObject saveContextTree(const ContextTree &tree);

} // namespace pinpoint::analysis
