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
