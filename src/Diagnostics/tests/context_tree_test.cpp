// Standalone tests for the context tree (src/Diagnostics/context_tree.*).
//
// The tree exists so one measure can carry genuinely different norms for a driver and a wedge
// without duplicating the measure. Two behaviours matter most:
//
//   1. Resolution walks UP, so an author writes a row only where the value differs from its parent.
//   2. An UNKNOWN context resolves to nothing, never to the default. Silently grading an
//      unrecognised context against the full-swing norm would be a wrong answer wearing a right
//      answer's clothes.
//
// Every traversal is also bounded, because a malformed tree (a community file, a half-edited draft)
// must degrade rather than hang the UI thread.
//
//   cmake --build build/analyzer-tests --target context_tree_test
//   ctest --test-dir build/analyzer-tests -R context_tree_test --output-on-failure

#include "../context_tree.h"

#include <QFile>

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static ContextTree treeOf(std::initializer_list<ContextNode> nodes)
{
    return ContextTree(std::vector<ContextNode>(nodes));
}

static bool hasCode(const ValidationReport &r, const char *code)
{
    for (const ValidationIssue &i : r.issues)
        if (i.code == QLatin1String(code)) return true;
    return false;
}

// The shape the shipped tree has, small enough to reason about.
static ContextTree sampleTree()
{
    return treeOf({
        { QStringLiteral("any"),         QStringLiteral("Any shot"),    QString() },
        { QStringLiteral("full_swing"),  QStringLiteral("Full swing"),  QStringLiteral("any") },
        { QStringLiteral("driver"),      QStringLiteral("Driver"),      QStringLiteral("full_swing") },
        { QStringLiteral("iron"),        QStringLiteral("Iron"),        QStringLiteral("full_swing") },
        { QStringLiteral("partial"),     QStringLiteral("Partial"),     QStringLiteral("any") },
        { QStringLiteral("chip"),        QStringLiteral("Chip"),        QStringLiteral("partial") },
    });
}

int main()
{
    std::printf("=== context tree: upward resolution ===\n");
    {
        const ContextTree t = sampleTree();

        const QStringList driver = t.chain(QStringLiteral("driver"));
        check(driver == QStringList({ QStringLiteral("driver"), QStringLiteral("full_swing"),
                                      QStringLiteral("any") }),
              "driver resolves driver -> full_swing -> any, nearest first");

        check(t.chain(QStringLiteral("any")) == QStringList({ QStringLiteral("any") }),
              "a root resolves to itself alone");

        check(t.chain(QStringLiteral("chip")) ==
                  QStringList({ QStringLiteral("chip"), QStringLiteral("partial"),
                                QStringLiteral("any") }),
              "a sibling branch resolves through its own parent, not through full_swing");
    }

    std::printf("=== context tree: an unknown context is not the default ===\n");
    {
        const ContextTree t = sampleTree();
        check(t.chain(QStringLiteral("hovercraft")).isEmpty(),
              "an unknown context yields an EMPTY chain, never a fallback to full_swing");
        check(t.chain(QString()).isEmpty(), "an empty context id yields an empty chain");
        check(!t.contains(QStringLiteral("hovercraft")), "contains() agrees");
    }

    std::printf("=== context tree: depth and order ===\n");
    {
        const ContextTree t = sampleTree();
        check(t.depth(QStringLiteral("any")) == 0,        "a root is depth 0");
        check(t.depth(QStringLiteral("full_swing")) == 1, "its child is depth 1");
        check(t.depth(QStringLiteral("driver")) == 2,     "its grandchild is depth 2");

        const std::vector<QString> order = t.inOrder();
        check(order.size() == 6, "every node appears exactly once in tree order");
        check(order[0] == QLatin1String("any") && order[1] == QLatin1String("full_swing")
                  && order[2] == QLatin1String("driver") && order[3] == QLatin1String("iron"),
              "a parent is followed by its subtree, in declaration order");

        check(t.children(QStringLiteral("full_swing")) ==
                  QStringList({ QStringLiteral("driver"), QStringLiteral("iron") }),
              "children() is declaration order");
        check(t.children(QString()) ==
                  QStringList({ QStringLiteral("any") }),
              "an empty id yields the roots");
    }

    std::printf("=== context tree: several roots are legal ===\n");
    {
        const ContextTree t = treeOf({
            { QStringLiteral("any"),      QStringLiteral("Any"),      QString() },
            { QStringLiteral("bunker"),   QStringLiteral("Bunker"),   QString() },
            { QStringLiteral("specialty"),QStringLiteral("Specialty"),QString() },
        });
        check(validateContextTree(t).ok(), "a forest with three roots validates clean");
        check(t.inOrder().size() == 3, "all three roots appear");
    }

    std::printf("=== context tree: descendant test ===\n");
    {
        const ContextTree t = sampleTree();
        check(t.isDescendantOf(QStringLiteral("driver"), QStringLiteral("any")),
              "driver is a descendant of any");
        check(t.isDescendantOf(QStringLiteral("driver"), QStringLiteral("full_swing")),
              "driver is a descendant of full_swing");
        check(!t.isDescendantOf(QStringLiteral("driver"), QStringLiteral("driver")),
              "a node is NOT its own descendant");
        check(!t.isDescendantOf(QStringLiteral("driver"), QStringLiteral("partial")),
              "driver is not a descendant of a sibling branch");
    }

    std::printf("=== context tree: validation rejects broken trees ===\n");
    {
        const ContextTree dup = treeOf({
            { QStringLiteral("iron"), QStringLiteral("Iron"),  QString() },
            { QStringLiteral("iron"), QStringLiteral("Iron2"), QString() },
        });
        check(hasCode(validateContextTree(dup), "duplicateContextId"), "duplicate id is an error");

        const ContextTree orphan = treeOf({
            { QStringLiteral("iron"), QStringLiteral("Iron"), QStringLiteral("nowhere") },
        });
        check(hasCode(validateContextTree(orphan), "unknownParent"), "unknown parent is an error");

        const ContextTree empty = treeOf({
            { QString(), QStringLiteral("Nameless"), QString() },
        });
        check(hasCode(validateContextTree(empty), "emptyContextId"), "an id-less node is an error");

        // A two-node cycle. This is the case that would hang an unbounded walk.
        const ContextTree cycle = treeOf({
            { QStringLiteral("a"), QStringLiteral("A"), QStringLiteral("b") },
            { QStringLiteral("b"), QStringLiteral("B"), QStringLiteral("a") },
        });
        check(hasCode(validateContextTree(cycle), "contextCycle"), "a cycle is an error");
        check(!cycle.chain(QStringLiteral("a")).isEmpty(),
              "chain() over a cycle terminates rather than hanging");
        check(cycle.inOrder().size() == 2,
              "inOrder() over a cycle still emits every node exactly once");
    }

    std::printf("=== context tree: a node under an unknown parent still shows ===\n");
    {
        // Validation is what tells the user the file is wrong. The view's job is to show what the
        // file actually contains — hiding it would make the error impossible to find and fix.
        const ContextTree t = treeOf({
            { QStringLiteral("any"),  QStringLiteral("Any"),  QString() },
            { QStringLiteral("lost"), QStringLiteral("Lost"), QStringLiteral("nowhere") },
        });
        const std::vector<QString> order = t.inOrder();
        check(order.size() == 2, "an unreachable node is still emitted");
    }

    std::printf("=== context tree: JSON round-trip ===\n");
    {
        const ContextTree in = sampleTree();
        const ContextTreeLoadResult res = loadContextTree(saveContextTree(in));
        check(res.loaded, "a saved tree loads clean");
        check(res.tree.nodes().size() == in.nodes().size(), "node count survives the round-trip");
        check(res.tree.chain(QStringLiteral("driver")) == in.chain(QStringLiteral("driver")),
              "resolution is identical after a round-trip");
        const ContextNode *n = res.tree.node(QStringLiteral("driver"));
        check(n != nullptr && n->label == QLatin1String("Driver"), "labels survive");
    }

    std::printf("=== context tree: bad input is reported, not thrown ===\n");
    {
        const ContextTreeLoadResult bad = loadContextTree(QByteArray("{ not json"));
        check(!bad.parsed && !bad.report.ok(), "unparseable JSON reports an error");

        const ContextTreeLoadResult noArray =
            loadContextTree(QByteArray(R"({"id":"core"})"));
        check(!noArray.parsed && hasCode(noArray.report, "badContextFile"),
              "a file with no contexts array reports badContextFile");
    }

    std::printf("=== context tree: binding resolution ===\n");
    {
        const ContextTree t = sampleTree();

        // A condition with NO bindings applies everywhere. This is the shipped case — all 50 core
        // conditions carry no rows — and if it ever inverted, the whole library would go silent in
        // every context at once rather than fail visibly in one.
        Condition bare;
        bare.id = QStringLiteral("bare");
        for (const QString &id : { QStringLiteral("driver"), QStringLiteral("chip"),
                                   QStringLiteral("any") }) {
            const BindingResolution r = resolveContextBinding(bare, t, id);
            check(r.applicable && r.material && !r.found,
                  "with no bindings a condition applies, and says nothing was found");
        }

        // A row at a parent covers everything beneath it — the reason an author writes one row at
        // `partial` rather than one at pitch and one at chip.
        Condition narrowed = bare;
        narrowed.bindings.push_back(ContextBinding{ QStringLiteral("partial"), false, true, {} });

        const BindingResolution atPartial = resolveContextBinding(narrowed, t, QStringLiteral("partial"));
        check(atPartial.found && !atPartial.applicable && !atPartial.inherited,
              "the context that carries the row reports it as its own");

        const BindingResolution atChip = resolveContextBinding(narrowed, t, QStringLiteral("chip"));
        check(atChip.found && !atChip.applicable && atChip.inherited
                  && atChip.contextId == QLatin1String("partial"),
              "a child inherits the parent's row and names where it came from");

        const BindingResolution atDriver = resolveContextBinding(narrowed, t, QStringLiteral("driver"));
        check(atDriver.applicable && !atDriver.found,
              "a sibling subtree is untouched by it");

        // NEAREST WINS, so an explicit exception beneath a switched-off parent survives — which is
        // exactly why the editor has to clear such rows when it switches the parent off, or the
        // untick would not take.
        narrowed.bindings.push_back(ContextBinding{ QStringLiteral("chip"), true, true, {} });
        const BindingResolution chipAgain = resolveContextBinding(narrowed, t, QStringLiteral("chip"));
        check(chipAgain.applicable && !chipAgain.inherited,
              "a nearer row beats an ancestor's, both ways");

        // Materiality resolves through the same walk and is INDEPENDENT of applicability: reported,
        // but not counted when ranking.
        Condition immaterial = bare;
        immaterial.bindings.push_back(ContextBinding{ QStringLiteral("full_swing"), true, false, {} });
        const BindingResolution ironR = resolveContextBinding(immaterial, t, QStringLiteral("iron"));
        check(ironR.applicable && !ironR.material && ironR.inherited,
              "materiality inherits too, and does not imply inapplicable");

        // An unknown context is not evidence that anything was switched off.
        const BindingResolution unknown = resolveContextBinding(narrowed, t, QStringLiteral("no_such"));
        check(unknown.applicable && unknown.material && !unknown.found,
              "an unknown context resolves to the default, never to a row");
        const BindingResolution unstated = resolveContextBinding(narrowed, t, QString());
        check(unstated.applicable && !unstated.found,
              "a shot that named no context is not a shot the author excluded");

        check(ownContextBinding(immaterial, QStringLiteral("full_swing")) != nullptr
                  && ownContextBinding(immaterial, QStringLiteral("iron")) == nullptr,
              "ownContextBinding answers 'is this yours', not 'does something resolve here'");
    }

    std::printf("=== context tree: the SHIPPED tree ===\n");
    {
        // The shipped file has to be valid, and full_swing has to be reachable from it — the engine
        // falls back there for any shot that declares no context.
        QFile f(QStringLiteral(PP_CONTEXTS_PATH));
        if (!f.open(QIODevice::ReadOnly)) {
            check(false, "the shipped contexts.json is readable");
        } else {
            const ContextTreeLoadResult res = loadContextTree(f.readAll(),
                                                              QStringLiteral(PP_CONTEXTS_PATH));
            check(res.loaded, "the shipped context tree validates clean");
            check(res.tree.contains(kDefaultContextId()),
                  "the shipped tree contains the default context (full_swing)");
            check(res.tree.chain(kDefaultContextId()).contains(QStringLiteral("any")),
                  "full_swing resolves up to 'any'");
            check(res.tree.contains(QStringLiteral("driver"))
                      && res.tree.isDescendantOf(QStringLiteral("driver"), kDefaultContextId()),
                  "driver sits under full_swing, so it inherits full-swing norms");
            // The archetype nodes carry the face corridor that stage 2 migrates out of
            // reference_bands.cpp; without them there is nowhere for those rows to live.
            check(res.tree.contains(QStringLiteral("archetype_bowed"))
                      && res.tree.contains(QStringLiteral("archetype_cupped")),
                  "the archetype contexts exist for the migrated face corridors");
        }
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
