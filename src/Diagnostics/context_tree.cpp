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

#include "context_tree.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

namespace pinpoint::analysis {

namespace {

// The walk guard. Every traversal here is bounded by the node count rather than by trusting the
// tree to be acyclic: validateContextTree() reports a cycle, but a caller may hold an unvalidated
// tree (a community file, a half-edited draft) and an unbounded walk would hang the UI thread
// rather than degrade.
constexpr int kMaxDepth = 64;

} // namespace

const ContextNode *ContextTree::node(const QString &id) const
{
    if (id.isEmpty())
        return nullptr;
    for (const ContextNode &n : m_nodes)
        if (n.id == id) return &n;
    return nullptr;
}

QStringList ContextTree::chain(const QString &id) const
{
    QStringList out;
    const ContextNode *n = node(id);
    if (n == nullptr)
        return out;                      // unknown context: no chain, never a silent default

    QSet<QString> seen;
    for (int guard = 0; n != nullptr && guard < kMaxDepth; ++guard) {
        if (seen.contains(n->id))
            break;                       // cycle: stop where it closes, report it in validation
        seen.insert(n->id);
        out.append(n->id);
        if (n->parentId.isEmpty())
            break;
        n = node(n->parentId);
    }
    return out;
}

int ContextTree::depth(const QString &id) const
{
    const QStringList c = chain(id);
    return c.isEmpty() ? 0 : int(c.size()) - 1;
}

QStringList ContextTree::children(const QString &id) const
{
    QStringList out;
    for (const ContextNode &n : m_nodes)
        if (n.parentId == id) out.append(n.id);
    return out;
}

std::vector<QString> ContextTree::inOrder() const
{
    std::vector<QString> out;
    out.reserve(m_nodes.size());
    QSet<QString> emitted;

    // Depth-first from each root, preserving declaration order at every level. Recursion is
    // expressed as an explicit stack so a malformed tree cannot blow the call stack.
    struct Frame { QString id; int childIndex; };

    for (const ContextNode &root : m_nodes) {
        if (!root.parentId.isEmpty())
            continue;
        std::vector<Frame> stack{ { root.id, 0 } };
        while (!stack.empty()) {
            Frame &f = stack.back();
            if (f.childIndex == 0) {
                if (emitted.contains(f.id)) { stack.pop_back(); continue; }
                emitted.insert(f.id);
                out.push_back(f.id);
            }
            const QStringList kids = children(f.id);
            if (f.childIndex < kids.size() && int(stack.size()) < kMaxDepth) {
                const QString next = kids.at(f.childIndex);
                ++f.childIndex;
                stack.push_back({ next, 0 });
            } else {
                stack.pop_back();
            }
        }
    }

    // Anything not reachable from a root (an unknown parent, or a node inside a cycle) still has to
    // appear, or the UI would silently hide content the file plainly contains. Validation is what
    // tells the user it is wrong; the view's job is to show it.
    for (const ContextNode &n : m_nodes)
        if (!emitted.contains(n.id)) { emitted.insert(n.id); out.push_back(n.id); }

    return out;
}

bool ContextTree::isDescendantOf(const QString &id, const QString &ancestorId) const
{
    const QStringList c = chain(id);
    for (int i = 1; i < c.size(); ++i)
        if (c.at(i) == ancestorId) return true;
    return false;
}

// ── Binding resolution ──────────────────────────────────────────────────────

const ContextBinding *ownContextBinding(const Condition &c, const QString &contextId)
{
    if (contextId.isEmpty())
        return nullptr;
    for (const ContextBinding &b : c.bindings)
        if (b.context == contextId) return &b;
    return nullptr;
}

BindingResolution resolveContextBinding(const Condition &c, const ContextTree &tree,
                                        const QString &contextId)
{
    BindingResolution out;

    // An empty context is "the shot did not say". Unlike a norm, there is nothing to grade and
    // nothing to demote, so it resolves to the default rather than to the default CONTEXT — a
    // condition switched off for wedges must not be switched off for a shot that named no club.
    const QStringList chain = tree.chain(contextId);
    if (chain.isEmpty())
        return out;

    for (int i = 0; i < chain.size(); ++i) {
        const ContextBinding *b = ownContextBinding(c, chain.at(i));
        if (b == nullptr) continue;
        out.applicable = b->applicable;
        out.material   = b->material;
        out.contextId  = b->context;
        out.inherited  = i > 0;
        out.found      = true;
        return out;
    }
    return out;
}

// ── Validation ──────────────────────────────────────────────────────────────

ValidationReport validateContextTree(const ContextTree &tree)
{
    ValidationReport rep;
    auto err = [&rep](const QString &code, const QString &subject, const QString &message) {
        rep.issues.push_back(ValidationIssue{ IssueSeverity::Error, code, subject, message });
    };

    QSet<QString> ids;
    for (const ContextNode &n : tree.nodes()) {
        if (n.id.isEmpty()) {
            err(QStringLiteral("emptyContextId"), QString(),
                QStringLiteral("A context has no id."));
            continue;
        }
        if (ids.contains(n.id))
            err(QStringLiteral("duplicateContextId"), n.id,
                QStringLiteral("Two contexts share the id '%1'.").arg(n.id));
        ids.insert(n.id);
    }

    for (const ContextNode &n : tree.nodes()) {
        if (n.id.isEmpty())
            continue;

        if (!n.parentId.isEmpty() && !tree.contains(n.parentId))
            err(QStringLiteral("unknownParent"), n.id,
                QStringLiteral("Context '%1' names a parent '%2' that does not exist.")
                    .arg(n.id, n.parentId));

        // A cycle shows up as a chain that fails to terminate at a root: chain() stops when it
        // revisits a node, so the last entry still having a resolvable parent means it closed a
        // loop rather than reaching the top.
        const QStringList c = tree.chain(n.id);
        if (!c.isEmpty()) {
            const ContextNode *last = tree.node(c.last());
            if (last != nullptr && !last->parentId.isEmpty() && tree.contains(last->parentId))
                err(QStringLiteral("contextCycle"), n.id,
                    QStringLiteral("Context '%1' is its own ancestor.").arg(n.id));
        }
    }

    return rep;
}

// ── Persistence ─────────────────────────────────────────────────────────────

namespace {

ContextTreeLoadResult loadFrom(const QJsonObject &root, const QString &sourceLabel)
{
    ContextTreeLoadResult out;

    const QJsonValue contexts = root.value(QStringLiteral("contexts"));
    if (!contexts.isArray()) {
        out.report.issues.push_back(ValidationIssue{
            IssueSeverity::Error, QStringLiteral("badContextFile"), sourceLabel,
            QStringLiteral("No 'contexts' array in %1.")
                .arg(sourceLabel.isEmpty() ? QStringLiteral("the context file") : sourceLabel) });
        return out;
    }

    std::vector<ContextNode> nodes;
    const QJsonArray arr = contexts.toArray();
    nodes.reserve(size_t(arr.size()));
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        ContextNode n;
        n.id       = o.value(QStringLiteral("id")).toString();
        n.label    = o.value(QStringLiteral("label")).toString();
        n.parentId = o.value(QStringLiteral("parent")).toString();
        nodes.push_back(std::move(n));
    }

    out.tree   = ContextTree(std::move(nodes));
    out.parsed = true;
    out.report = validateContextTree(out.tree);
    out.loaded = out.report.ok();
    return out;
}

} // namespace

ContextTreeLoadResult loadContextTree(const QJsonObject &root, const QString &sourceLabel)
{
    return loadFrom(root, sourceLabel);
}

ContextTreeLoadResult loadContextTree(const QByteArray &json, const QString &sourceLabel)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        ContextTreeLoadResult out;
        out.report.issues.push_back(ValidationIssue{
            IssueSeverity::Error, QStringLiteral("badContextFile"), sourceLabel,
            QStringLiteral("Could not parse %1: %2")
                .arg(sourceLabel.isEmpty() ? QStringLiteral("the context file") : sourceLabel,
                     perr.errorString()) });
        return out;
    }
    return loadFrom(doc.object(), sourceLabel);
}

QString contextIdForClub(const QString &club)
{
    // Matched on WORDS and a leading number rather than on the exact vocabulary strings, so a
    // future "2 IRON" lands on `iron_2` if the tree grows one and on `iron` if it does not, without
    // a table here that has to be kept in step with club_vocabulary.h. Upper-cased first because
    // the token is stored upper-case but a hand-edited or imported swing.json need not be.
    const QString c = club.trimmed().toUpper();
    if (c.isEmpty()) return kDefaultContextId();

    // The leading number, where there is one: "7 IRON" -> 7, "PITCHING WEDGE" -> -1.
    int  digits = 0;
    while (digits < c.size() && c.at(digits).isDigit()) ++digits;
    const int number = digits > 0 ? c.left(digits).toInt() : -1;

    if (c.contains(QLatin1String("DRIVER"))) return QStringLiteral("driver");

    // A putt is NOT a full swing, and this is the line that says so. It resolves under `any`, which
    // inherits nothing authored for a swing — before the node existed a putter fell through to the
    // full-swing default and was graded against corridors written for one.
    if (c.contains(QLatin1String("PUTTER"))) return QStringLiteral("putt");

    // WEDGE before IRON, and it is load-bearing: "PITCHING WEDGE" is a wedge, and an iron corridor
    // is 17% of stance width away from a wedge one for ball position.
    if (c.contains(QLatin1String("WEDGE"))) {
        if (c.contains(QLatin1String("PITCH"))) return QStringLiteral("wedge_pitching");
        if (c.contains(QLatin1String("GAP")))   return QStringLiteral("wedge_gap");
        if (c.contains(QLatin1String("SAND")))  return QStringLiteral("wedge_sand");
        if (c.contains(QLatin1String("LOB")))   return QStringLiteral("wedge_lob");
        return QStringLiteral("wedge");
    }

    // Before IRON, because a hybrid is named for the iron it replaces — "3 HYBRID" carries no
    // "IRON", but a future "4 IRON HYBRID" would. ONE node for the family rather than one per
    // number: the figures that would distinguish a 3 from a 4 hybrid do not exist, and a node whose
    // corridor can only ever be its parent's is a row an author has to read and dismiss.
    if (c.contains(QLatin1String("HYBRID"))) return QStringLiteral("hybrid");

    if (c.contains(QLatin1String("WOOD"))) {
        if (number == 3) return QStringLiteral("wood_3");
        if (number == 5) return QStringLiteral("wood_5");
        return QStringLiteral("fairway_wood");
    }

    // 3 through 9 are the nodes the tree ships. Anything outside that — a 2 iron, a 1 iron — lands
    // on the family, which is the correct answer rather than a fallback: it inherits every row the
    // family carries and is graded, just not distinguished.
    if (c.contains(QLatin1String("IRON"))) {
        if (number >= 3 && number <= 9) return QStringLiteral("iron_%1").arg(number);
        return QStringLiteral("iron");
    }

    // Everything unrecognised. See the header for why that is the default rather than nothing.
    return kDefaultContextId();
}

QJsonObject saveContextTree(const ContextTree &tree)
{
    QJsonArray arr;
    for (const ContextNode &n : tree.nodes()) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), n.id);
        if (!n.label.isEmpty())    o.insert(QStringLiteral("label"), n.label);
        if (!n.parentId.isEmpty()) o.insert(QStringLiteral("parent"), n.parentId);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("contexts"), arr);
    return root;
}

} // namespace pinpoint::analysis
