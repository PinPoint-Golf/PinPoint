// Standalone tests for the causal DAG layout (src/Diagnostics/dag_layout.*).
//
// The layout is in C++ precisely so it can be asserted: rank assignment, ordering and overlap are
// the only testable things about this surface, and none of them would be reachable if the geometry
// lived in delegate bindings. The four properties that matter:
//
//   1. NO OVERLAP. Two boxes on top of each other is not a cosmetic defect on a navigation surface
//      — it hides a node, and a hidden node reads as a graph that does not contain it.
//   2. DETERMINISM. The same focus must produce the same picture every time. A graph that
//      reshuffles between visits teaches the reader that position means nothing.
//   3. THE DEPTH BOUND HOLDS, every step the UI offers is a step the layout takes, and whatever the
//      bound cut off is counted rather than dropped silently — and reachable, by opening the box it
//      was counted on.
//   4. AN ISOLATED NODE lays out. It is the empty state of this view and the most likely one for a
//      freshly authored characteristic.
//
//   cmake --build build/analyzer-tests --target dag_layout_test
//   ctest --test-dir build/analyzer-tests -R dag_layout --output-on-failure

#include "../dag_layout.h"
#include "../reference_pack.h"

#include <cstdio>
#include <set>
#include <utility>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// ── Fixture ─────────────────────────────────────────────────────────────────
//
//   deepA ─┐                         ┌─► effect1 ─► deepEffect
//          ├─► cause1 ─┐             │
//   deepB ─┘           ├─► FOCUS ────┤
//              cause2 ─┘             └─► effect2
//              cause3 ─┘  (asserted, and also causes cause1 — a same-rank edge)
//
// Plus a latent screened cause, a measure with no producer behind the focus, and a node connected
// to nothing at all.
//
// And a chain running away from the focus on both sides, deep enough to reach past every bound:
//
//   far ─► origin ─► root1 ─┐
//                           ├─► deepA ─► cause1 ─► FOCUS ─► effect1 ─► deepEffect ─► farEffect
//                    root2 ─┘
//
// `far` sits at rank -5 and can therefore never be drawn — which is what makes the clamp
// assertable rather than assumed.
static CharacteristicPack fixture()
{
    CharacteristicPack p;
    p.id            = QStringLiteral("t");
    p.schemaVersion = kPackSchemaVersion;

    auto measure = [&](const char *id, MeasureStatus st) {
        Measure m;
        m.id             = QString::fromLatin1(id);
        m.label          = QString::fromLatin1(id);
        m.kind           = MeasureKind::Composed;
        m.series         = Series{ AnatomyRole::PelvisCentre, Quantity::Distance, AnatomyRole::TrailAnkle };
        m.reducer.kind   = ReducerKind::At;
        m.reducer.anchor = Phase::Top;
        m.status         = st;
        p.measures.push_back(m);
    };
    measure("mLive", MeasureStatus::Live);
    measure("mGhost", MeasureStatus::NoProducer);

    auto signal = [&](const char *id, const char *mid) {
        Signal s;
        s.id        = QString::fromLatin1(id);
        s.test      = SignalTest::OutsideCorridor;
        s.measures  = { QString::fromLatin1(mid) };
        s.direction = Direction::High;
        p.signalDefs.push_back(s);
    };
    signal("sLive", "mLive");
    signal("sGhost", "mGhost");

    auto cond = [&](const char *id, const char *label, ConfirmedBy by, Observability obs,
                    std::initializer_list<const char *> detectedBy) {
        Condition c;
        c.id            = QString::fromLatin1(id);
        c.label         = QString::fromLatin1(label);
        c.confirmedBy   = by;
        c.observability = obs;
        for (const char *s : detectedBy) c.detectedBy << QString::fromLatin1(s);
        p.conditions.push_back(c);
    };
    cond("focus",      "The focus",       ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("cause1",     "First cause",     ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("cause2",     "Second cause",    ConfirmedBy::Measured, Observability::Observable, { "sGhost" });
    cond("cause3",     "Third cause",     ConfirmedBy::Asserted, Observability::Latent,     {});
    cond("deepA",      "Deep cause A",    ConfirmedBy::Screened, Observability::Latent,     {});
    cond("deepB",      "Deep cause B",    ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("effect1",    "First effect",    ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("effect2",    "Second effect",   ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("deepEffect", "Deep effect",     ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("island",     "Connected to nothing", ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("root1",      "Root cause one",  ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("root2",      "Root cause two",  ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("origin",     "The origin",      ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("far",        "Further than any bound", ConfirmedBy::Measured, Observability::Observable, { "sLive" });
    cond("farEffect",  "Far effect",      ConfirmedBy::Measured, Observability::Observable, { "sLive" });

    auto edge = [&](const char *from, const char *to, Strength st) {
        Edge e;
        e.from     = QString::fromLatin1(from);
        e.to       = QString::fromLatin1(to);
        e.type     = EdgeType::Causes;
        e.strength = st;
        p.edges.push_back(e);
    };
    edge("cause1", "focus", Strength::Strong);
    edge("cause2", "focus", Strength::Moderate);
    edge("cause3", "focus", Strength::Weak);
    edge("cause3", "cause1", Strength::Weak);       // same-rank edge
    edge("deepA", "cause1", Strength::Moderate);
    edge("deepB", "cause1", Strength::Moderate);
    edge("focus", "effect1", Strength::Strong);
    edge("focus", "effect2", Strength::Weak);
    edge("effect1", "deepEffect", Strength::Moderate);
    // A cause that ALSO causes an effect: rank -1 straight to rank +1, across the focus column.
    // This is the edge that used to be drawn as one long curve through the focus box.
    edge("cause2", "effect2", Strength::Moderate);
    // The deep chain. Two causes on `deepA` so an opened node has more than one thing to admit, and
    // one more hop past `origin` than any bound can reach.
    edge("root1", "deepA", Strength::Moderate);
    edge("root2", "deepA", Strength::Moderate);
    edge("origin", "root1", Strength::Moderate);
    edge("far", "origin", Strength::Moderate);
    edge("deepEffect", "farEffect", Strength::Moderate);

    // Citations, for the bibliography rows. `focus` cites a paper the registry knows and also
    // carries a measure, so the two stacks meet in one box; `cause2` cites one it has never heard
    // of; `cause3` cites nothing. Between them that is every state a citation row can be in.
    for (Condition &c : p.conditions) {
        if (c.id == QLatin1String("focus"))  c.provenance.citation = QStringLiteral("10.1234/known");
        if (c.id == QLatin1String("cause2")) c.provenance.citation = QStringLiteral("30479527");
    }
    return p;
}

// The bibliography, HANDED to the layout rather than read out of the shared registry — see the note
// on DagLayoutOptions::references. It is what lets this test state what resolves and what does not.
static ReferenceSet referenceFixture()
{
    ReferenceSet set;
    Reference    r;
    r.id      = QStringLiteral("ref.known");
    r.doi     = QStringLiteral("10.1234/known");
    r.title   = QStringLiteral("A paper the registry knows");
    r.authors = QStringLiteral("Someone");
    r.year    = 2005;
    set.references.push_back(r);
    return set;
}

static const DagNode *nodeById(const DagLayout &l, const char *id)
{
    for (const DagNode &n : l.nodes)
        if (n.id == QLatin1String(id)) return &n;
    return nullptr;
}

static bool overlaps(const DagNode &a, const DagNode &b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Where a cubic actually goes. The line-through-a-box defect is invisible to any check on the
// endpoints alone — both ends can sit correctly on their own node while the middle of the curve
// crosses a third one.
static void cubicAt(const DagEdge &e, double t, double &x, double &y)
{
    const double u = 1.0 - t;
    x = u * u * u * e.x1 + 3 * u * u * t * e.c1x + 3 * u * t * t * e.c2x + t * t * t * e.x2;
    y = u * u * u * e.y1 + 3 * u * u * t * e.c1y + 3 * u * t * t * e.c2y + t * t * t * e.y2;
}

// Does any drawn line pass through a node that is not one of its own endpoints?
static bool linesCrossBoxes(const DagLayout &l, QString *why = nullptr)
{
    for (const DagEdge &e : l.edges) {
        for (double t = 0.10; t <= 0.901; t += 0.05) {
            double x = 0, y = 0;
            cubicAt(e, t, x, y);
            for (const DagNode &n : l.nodes) {
                if (n.id == e.from || n.id == e.to) continue;
                // A little inset: a curve grazing the very edge of a box is not what makes a
                // picture unreadable, and demanding pixel clearance would fail on rounding.
                const double m = 2.0;
                if (x > n.x + m && x < n.x + n.w - m && y > n.y + m && y < n.y + n.h - m) {
                    if (why)
                        *why = QStringLiteral("%1->%2 crosses %3").arg(e.from, e.to, n.id);
                    return true;
                }
            }
        }
    }
    return false;
}

int main()
{
    const CharacteristicPack p = fixture();

    // ── Ranking ─────────────────────────────────────────────────────────────
    std::printf("Rank is signed causal distance\n");
    {
        const DagLayout l = layoutDag(p, QStringLiteral("focus"));

        check(nodeById(l, "focus") && nodeById(l, "focus")->rank == 0, "the focus is rank 0");
        check(nodeById(l, "cause1") && nodeById(l, "cause1")->rank == -1, "a cause is negative");
        check(nodeById(l, "effect1") && nodeById(l, "effect1")->rank == 1, "an effect is positive");
        check(nodeById(l, "focus")->kind == DagNodeKind::Focus, "the focus knows it is the focus");
        check(nodeById(l, "cause1")->kind == DagNodeKind::Cause, "kind follows the sign");
        check(nodeById(l, "effect1")->kind == DagNodeKind::Effect, "and on the other side too");

        // Left-to-right reads as causes. This is the one thing a reader takes from the shape
        // without a legend, so it is asserted rather than assumed from the rank sign.
        check(nodeById(l, "cause1")->x < nodeById(l, "focus")->x, "causes are drawn to the LEFT");
        check(nodeById(l, "effect1")->x > nodeById(l, "focus")->x, "effects to the RIGHT");
    }

    // ── The depth bound ─────────────────────────────────────────────────────
    std::printf("The depth bound holds, and says what it cut off\n");
    {
        const DagLayout d1 = layoutDag(p, QStringLiteral("focus"));   // depth 1 by default
        check(!nodeById(d1, "deepA"), "depth 1 stops at the first rank of causes");
        check(!nodeById(d1, "deepEffect"), "and at the first rank of effects");
        for (const DagNode &n : d1.nodes)
            check(std::abs(n.rank) <= 1, "no node past the bound");

        // What is off screen is COUNTED on the node it hangs off, not dropped. A graph that
        // silently omits half of what it knows is worse than one that draws nothing.
        const DagNode *c1 = nodeById(d1, "cause1");
        check(c1 && c1->hiddenCauses == 2, "cause1 reports its two unseen causes");
        check(d1.truncated, "the layout says it is showing a part");

        DagLayoutOptions o2;
        o2.depth = 2;
        const DagLayout d2 = layoutDag(p, QStringLiteral("focus"), o2);
        check(nodeById(d2, "deepA") && nodeById(d2, "deepA")->rank == -2, "Expand reaches rank -2");
        check(nodeById(d2, "deepEffect") && nodeById(d2, "deepEffect")->rank == 2, "and rank +2");
        check(nodeById(d2, "cause1")->hiddenCauses == 0, "and cause1 has nothing left hidden");

        // Every step the control offers is a step the layout takes. The `+` on the scope pill went
        // to 4 while this clamped at 2, so two of its four positions changed the number in the pill
        // and nothing else — which is how the causes of `over the top` came to be unreachable from
        // `slice` by any setting the UI had.
        DagLayoutOptions o3;
        o3.depth = 3;
        const DagLayout d3 = layoutDag(p, QStringLiteral("focus"), o3);
        check(nodeById(d3, "root1") && nodeById(d3, "root1")->rank == -3, "scope 3 reaches rank -3");
        check(!nodeById(d3, "origin"), "and stops there");

        DagLayoutOptions o4;
        o4.depth = 4;
        const DagLayout d4 = layoutDag(p, QStringLiteral("focus"), o4);
        check(nodeById(d4, "origin") && nodeById(d4, "origin")->rank == -4, "scope 4 reaches rank -4");
        check(nodeById(d4, "farEffect") && nodeById(d4, "farEffect")->rank == 3,
              "and the effects side runs as far as it has anything to draw");

        // The bound is still a bound: 5 clamps to 4 rather than walking the library.
        DagLayoutOptions o5;
        o5.depth = 5;
        const DagLayout d5 = layoutDag(p, QStringLiteral("focus"), o5);
        for (const DagNode &n : d5.nodes)
            check(std::abs(n.rank) <= 4, "depth clamps at 4");
        check(!nodeById(d5, "far"), "and the node past it is not drawn");

        check(!nodeById(d2, "island"), "an unconnected condition is not in someone else's graph");
    }

    // ── Opening one box ─────────────────────────────────────────────────────
    //
    // The reason this exists at all: raising the scope to reach ONE box widens every rank at once,
    // and hands back a hairball to answer a question about one node. Opening is the local answer.
    std::printf("An opened node admits its own neighbours, whatever the bound says\n");
    {
        DagLayoutOptions o;
        o.depth    = 1;
        o.expanded = { QStringLiteral("cause1") };
        const DagLayout l = layoutDag(p, QStringLiteral("focus"), o);

        check(nodeById(l, "deepA") && nodeById(l, "deepA")->rank == -2,
              "its causes arrive past the bound");
        check(nodeById(l, "deepB") != nullptr, "all of them, not the first");
        check(nodeById(l, "cause1")->expanded, "and it says it is open");
        check(!nodeById(l, "cause2")->expanded, "while its neighbours do not");
        check(nodeById(l, "cause1")->hiddenCauses == 0, "nothing of its own is left hidden");

        // LOCAL, which is the whole claim. Opening one box must not quietly become scope 2.
        check(!nodeById(l, "deepEffect"), "no other rank was widened");
        check(!nodeById(l, "root1"), "and it opened one hop, not the rest of the chain");

        // Direction follows the side of the picture. A cause opens towards ITS causes; pulling its
        // effects in would put them in a column that means "effects of the focus".
        DagLayoutOptions oc;
        oc.depth    = 1;
        oc.expanded = { QStringLiteral("cause2") };
        const DagLayout lc = layoutDag(p, QStringLiteral("focus"), oc);
        check(!nodeById(lc, "effect2") || nodeById(lc, "effect2")->rank == 1,
              "an opened cause does not drag its effects to a rank of its own");

        // An id that is not on this picture costs nothing — which is what lets the view keep one
        // list rather than pruning it against every redraw.
        DagLayoutOptions og;
        og.depth    = 1;
        og.expanded = { QStringLiteral("nosuch"), QStringLiteral("far") };
        const DagLayout lg = layoutDag(p, QStringLiteral("focus"), og);
        check(lg.nodes.size() == layoutDag(p, QStringLiteral("focus")).nodes.size(),
              "an opened id that is off the picture changes nothing");
    }

    std::printf("Opening chains, in whichever order the boxes were opened\n");
    {
        // `deepA` is only reachable once `cause1` is open, so a single pass in list order would
        // drop it whenever the author happened to open the far one first. Both orders must agree.
        for (int order = 0; order < 2; ++order) {
            DagLayoutOptions o;
            o.depth = 1;
            o.expanded = order == 0
                             ? QStringList{ QStringLiteral("cause1"), QStringLiteral("deepA") }
                             : QStringList{ QStringLiteral("deepA"), QStringLiteral("cause1") };
            const DagLayout l = layoutDag(p, QStringLiteral("focus"), o);
            check(nodeById(l, "root1") && nodeById(l, "root1")->rank == -3,
                  order == 0 ? "near box opened first" : "far box opened first");
            check(nodeById(l, "root2") != nullptr, "and the chain arrives whole");
        }
    }

    std::printf("An opened node answers to its OWN budget, not to the rank's\n");
    {
        // maxPerRank exists to stop an automatic walk fanning out; it must not be what decides how
        // much an explicit request returns. With room for one node per rank, opening the focus
        // still brings every cause it has.
        //
        // The FOCUS is opened rather than cause1, and that is not arbitrary. Under a cap of one the
        // single seat on rank -1 goes to cause3, which reaches furthest — so cause1 is not on the
        // picture to be opened, and an `expanded` id that is not drawn is ignored by design. Opening
        // the box the cap actually cut from is the case this is really about anyway: it is what a
        // reader does when a node says there are more causes behind it.
        DagLayoutOptions o;
        o.depth      = 2;
        o.maxPerRank = 1;
        o.expanded   = { QStringLiteral("focus") };
        const DagLayout l = layoutDag(p, QStringLiteral("focus"), o);
        check(nodeById(l, "cause1") && nodeById(l, "cause2"),
              "the per-rank cap does not apply to what was asked for");
        check(nodeById(l, "focus")->hiddenCauses == 0, "and the box it was asked of has nothing left");

        // Its own budget does apply, and what it drops is counted rather than lost.
        DagLayoutOptions ob;
        ob.depth        = 1;
        ob.maxPerExpand = 1;
        ob.expanded     = { QStringLiteral("cause1") };
        const DagLayout lb = layoutDag(p, QStringLiteral("focus"), ob);
        int drawn = (nodeById(lb, "deepA") ? 1 : 0) + (nodeById(lb, "deepB") ? 1 : 0);
        check(drawn == 1, "the budget bounds it");
        check(nodeById(lb, "cause1")->hiddenCauses == 1, "and the remainder is still counted");
        check(lb.truncated, "the layout still says it is showing a part");
    }

    // ── The physical-screen layer ────────────────────────────────────────────
    //
    // ON by default and removable, which is the opposite way round from measures. It is a NODE
    // filter and not an edge one, so what it removes has to reappear in the hidden counts or the
    // picture would claim the chain simply ends there.
    std::printf("The screened layer can be taken out, and says that it was\n");
    {
        DagLayoutOptions on;
        on.depth = 2;
        const DagLayout lOn = layoutDag(p, QStringLiteral("focus"), on);
        check(nodeById(lOn, "deepA") != nullptr, "a screened cause is drawn by default");

        DagLayoutOptions off = on;
        off.includeScreened = false;
        const DagLayout lOff = layoutDag(p, QStringLiteral("focus"), off);
        check(!nodeById(lOff, "deepA"), "and is gone when the layer is off");
        check(nodeById(lOff, "deepB") != nullptr, "while a MEASURED cause at the same rank stays");

        // Counted, not silently dropped — the same bargain every other bound here makes.
        const DagNode *c1On  = nodeById(lOn,  "cause1");
        const DagNode *c1Off = nodeById(lOff, "cause1");
        check(c1On->hiddenCauses == 0, "with the layer on, cause1 has nothing hidden");
        check(c1Off->hiddenCauses == 1, "with it off, cause1 reports the one it lost");
        check(lOff.truncated, "and the layout says it is showing a part");

        // The FOCUS is exempt. A filter that deleted the thing it was asked to draw would leave a
        // reader on an empty canvas with no way to tell it from a stale link.
        DagLayoutOptions fo;
        fo.includeScreened = false;
        const DagLayout lFocus = layoutDag(p, QStringLiteral("deepA"), fo);
        check(nodeById(lFocus, "deepA") != nullptr,
              "centring on a screened cause still draws it with the layer off");
        check(lFocus.nodes.size() >= 1 && lFocus.width > 0, "…and lays out around it");

        // It must apply to an OPENED box too, or the filter would hold on the automatic walk and
        // quietly not on the one route the reader asked for by name.
        DagLayoutOptions ex;
        ex.depth = 1;
        ex.includeScreened = false;
        ex.expanded = { QStringLiteral("cause1") };
        const DagLayout lEx = layoutDag(p, QStringLiteral("focus"), ex);
        check(!nodeById(lEx, "deepA"), "an opened box does not smuggle a screened cause back in");
        check(nodeById(lEx, "deepB") != nullptr, "…while still opening everything else");
    }

    std::printf("An opened graph is still a laid-out graph\n");
    {
        DagLayoutOptions o;
        o.depth    = 2;
        o.expanded = { QStringLiteral("deepA"), QStringLiteral("root1"), QStringLiteral("effect1") };
        const DagLayout l = layoutDag(p, QStringLiteral("focus"), o);

        bool ok = true;
        for (size_t i = 0; i < l.nodes.size(); ++i)
            for (size_t j = i + 1; j < l.nodes.size(); ++j)
                if (overlaps(l.nodes[i], l.nodes[j])) ok = false;
        check(ok, "no two nodes overlap");

        QString why;
        const bool bad = linesCrossBoxes(l, &why);
        if (bad) std::printf("      %s\n", qPrintable(why));
        check(!bad, "no line is drawn through a box that is not its own end");

        const DagLayout again = layoutDag(p, QStringLiteral("focus"), o);
        bool same = l.nodes.size() == again.nodes.size();
        for (size_t i = 0; same && i < l.nodes.size(); ++i)
            same = l.nodes[i].id == again.nodes[i].id && l.nodes[i].x == again.nodes[i].x
                   && l.nodes[i].y == again.nodes[i].y;
        check(same, "and it lays out the same way every time");
    }

    // ── Overlap ─────────────────────────────────────────────────────────────
    std::printf("No two nodes overlap\n");
    {
        DagLayoutOptions o;
        o.depth = 2;
        for (const char *focus : { "focus", "cause1", "effect1", "deepA", "island" }) {
            const DagLayout l  = layoutDag(p, QString::fromLatin1(focus), o);
            bool            ok = true;
            for (size_t i = 0; i < l.nodes.size(); ++i)
                for (size_t j = i + 1; j < l.nodes.size(); ++j)
                    if (overlaps(l.nodes[i], l.nodes[j])) ok = false;
            check(ok, focus);
        }

        // A long label widens its column rather than spilling into the next one — the case that
        // makes an approximate text width safe.
        CharacteristicPack wide = p;
        for (Condition &c : wide.conditions)
            if (c.id == QLatin1String("cause1"))
                c.label = QStringLiteral("A characteristic with a very long authored name indeed");
        const DagLayout lw = layoutDag(wide, QStringLiteral("focus"));
        bool            ok = true;
        for (size_t i = 0; i < lw.nodes.size(); ++i)
            for (size_t j = i + 1; j < lw.nodes.size(); ++j)
                if (overlaps(lw.nodes[i], lw.nodes[j])) ok = false;
        check(ok, "a long label does not push a node into its neighbour");
        check(nodeById(lw, "cause1")->w <= DagLayoutOptions{}.maxW + 0.001,
              "and is capped rather than unbounded");
    }

    // ── Lines do not cross boxes ────────────────────────────────────────────
    //
    // The defect this replaced: a cause that also causes an effect drew ONE long curve from rank
    // -1 to rank +1, straight over the focus. Both endpoints were correct, so nothing about the
    // ends could catch it — the picture just read as several arrows fighting for one box.
    std::printf("No line is drawn through a box that is not its own end\n");
    {
        for (int depth : { 1, 2, 3, 4 }) {
            DagLayoutOptions o;
            o.depth = depth;
            for (const char *focus : { "focus", "cause1", "effect1", "deepA", "island" }) {
                QString         why;
                const DagLayout l = layoutDag(p, QString::fromLatin1(focus), o);
                const bool      bad = linesCrossBoxes(l, &why);
                if (bad) std::printf("      %s\n", qPrintable(why));
                check(!bad, focus);
            }
        }
    }

    // ── The lines say what they mean ────────────────────────────────────────
    std::printf("The picture is annotated, not left to be inferred\n");
    {
        DagLayoutOptions o;
        o.depth = 2;
        const DagLayout l = layoutDag(p, QStringLiteral("focus"), o);

        bool causedBy = false, leadsTo = false, measuredBy = false;
        for (const DagHeading &h : l.headings) {
            if (h.label == QLatin1String("Caused by"))   causedBy   = true;
            if (h.label == QLatin1String("Leads to"))    leadsTo    = true;
            if (h.label == QLatin1String("Measured by")) measuredBy = true;
        }
        check(causedBy && leadsTo, "both sides of the graph are named");
        // …and there is no third heading. `Measured by` named a lane that no longer exists; the
        // measures are rows inside the boxes, which needs no word over an empty region.
        check(!measuredBy, "and nothing still names the lane that was removed");

        // The heading has to sit OVER its own side, or it names the wrong half of the picture.
        for (const DagHeading &h : l.headings) {
            if (h.label != QLatin1String("Caused by")) continue;
            const DagNode *c = nodeById(l, "cause1");
            check(h.x <= c->x + 0.001 && h.x + h.w >= c->x + c->w - 0.001,
                  "the cause heading spans the cause columns");
        }

        // Exactly one strength word per causal relationship — a label on every segment would
        // repeat itself down a routed line.
        std::map<QString, int> labelled;
        for (const DagEdge &e : l.edges)
            if (!e.label.isEmpty()) ++labelled[e.from + QStringLiteral("->") + e.to];
        bool once = !labelled.empty();
        for (const auto &kv : labelled) if (kv.second != 1) once = false;
        check(once, "each relationship states its strength once");

        for (const DagEdge &e : l.edges)
            if (e.detects) check(e.label.isEmpty(), "a measure has no strength to state");

        // An isolated focus has nothing to name on either side, and must not invent a heading over
        // an empty column.
        const DagLayout iso = layoutDag(p, QStringLiteral("island"));
        for (const DagHeading &h : iso.headings)
            check(h.label != QLatin1String("Caused by") && h.label != QLatin1String("Leads to"),
                  "no heading over a side with nothing on it");
    }

    // ── Determinism ─────────────────────────────────────────────────────────
    std::printf("The same focus lays out the same way every time\n");
    {
        DagLayoutOptions o;
        o.depth = 2;
        const DagLayout a = layoutDag(p, QStringLiteral("focus"), o);
        const DagLayout b = layoutDag(p, QStringLiteral("focus"), o);

        bool same = a.nodes.size() == b.nodes.size() && a.edges.size() == b.edges.size();
        for (size_t i = 0; same && i < a.nodes.size(); ++i)
            same = a.nodes[i].id == b.nodes[i].id && a.nodes[i].x == b.nodes[i].x
                   && a.nodes[i].y == b.nodes[i].y;
        check(same, "node order and position are identical across runs");
        check(a.width == b.width && a.height == b.height, "so is the bounding box");
    }

    // ── The isolated node ───────────────────────────────────────────────────
    std::printf("A condition connected to nothing still lays out\n");
    {
        const DagLayout l = layoutDag(p, QStringLiteral("island"));
        check(l.nodes.size() >= 1, "the focus is drawn");
        check(nodeById(l, "island") != nullptr, "and it is the one asked for");
        check(l.width > 0 && l.height > 0, "the bounding box is real");
        check(!l.truncated, "nothing was cut off, and it does not claim otherwise");

        bool onlyEdgesAreDetection = true;
        for (const DagEdge &e : l.edges)
            if (!e.detects) onlyEdgesAreDetection = false;
        check(onlyEdgesAreDetection, "no causal edge is invented for it");
    }

    std::printf("An unknown focus draws nothing\n");
    {
        const DagLayout l = layoutDag(p, QStringLiteral("nosuch"));
        check(l.nodes.empty() && l.edges.empty(), "a stale link lands on nothing, not on a guess");
        check(l.width == 0 && l.height == 0, "and takes no space");
    }

    // ── Measures are rows in a box, not nodes ───────────────────────────────
    //
    // They used to be a lane under the band with a line up to every condition they detected. The
    // lines were the problem: one measure serving three conditions drew three of them across the
    // picture, and a line into a box that was not the lowest in its column had to be routed up the
    // gutter beside it to avoid crossing the boxes underneath. A row inside the box says the same
    // thing with no line at all.
    std::printf("Measures are drawn inside the condition they detect\n");
    {
        const DagLayout l = layoutDag(p, QStringLiteral("focus"));

        check(!nodeById(l, "mLive"), "a measure is not a node any more");
        for (const DagEdge &e : l.edges)
            check(!e.detects, "and nothing draws a detection line");

        const DagNode *f = nodeById(l, "focus");
        check(f && f->measures.size() == 1, "the focus carries its own detector as a row");
        check(f->measures[0].id == QLatin1String("mLive"), "…the right one");
        check(f->measures[0].available, "…and reports it live");

        // EVERY drawn condition answers, not the focus alone. cause2 is detected by a different
        // measure; listing detectors for one box would leave the rest looking undetectable.
        const DagNode *c2 = nodeById(l, "cause2");
        check(c2 && c2->measures.size() == 1 && c2->measures[0].id == QLatin1String("mGhost"),
              "so does every other drawn condition");
        check(!c2->measures[0].available, "with a measure nothing produces marked as a gap");
        check(!c2->measures[0].statusLabel.isEmpty(), "…and named rather than left blank");

        // cause3 has no measure at all and must not invent one — an unmeasurable cause drawn with a
        // detector is the worst thing this could say.
        const DagNode *c3 = nodeById(l, "cause3");
        check(c3 && c3->measures.empty(), "a condition nothing detects carries no row");

        // The BOX GREW. That is the whole visible claim, and the rows have to sit inside it or the
        // view would draw them over the node below.
        check(f->h > c3->h, "a box with a row is taller than one without");
        for (const DagMeasure &dm : f->measures) {
            check(dm.y >= f->y && dm.y + dm.h <= f->y + f->h + 0.001,
                  "every row lies inside its own box");
            check(dm.y >= f->y + DagLayoutOptions{}.nodeH - 0.001,
                  "…and below the condition's own row, never over it");
        }

        // THE BOX HAS TO BE WIDE ENOUGH FOR THE ROW. A row the reader can see and cannot read is
        // worse than no row, and a measure label is a sentence where a condition's name is a
        // phrase — which is why the rows have their own cap rather than sharing maxW.
        {
            CharacteristicPack wide = p;
            for (Measure &mm : wide.measures)
                if (mm.id == QLatin1String("mLive"))
                    mm.label = QStringLiteral("Thorax centre distance relative to the trail "
                                              "ankle, change from P1 to P5");
            DagLayoutOptions o;
            o.maxW = 200;      // the condition-name cap…
            o.rowMaxW = 360;   // …and the roomier one the rows answer to
            const DagLayout lw = layoutDag(wide, QStringLiteral("focus"), o);
            const DagNode *f = nodeById(lw, "focus");
            check(f->w > o.maxW, "a long measure label widens the box past the NAME cap");
            check(f->w <= o.rowMaxW + 0.001, "…and no further than the row cap");

            // Only the box that carries it. A wide row must not widen the picture wholesale.
            const DagNode *c3 = nodeById(lw, "cause3");
            check(c3 && c3->w < f->w, "a box with no rows is not widened by somebody else's");

            // And the same graph without measures keeps the narrow box, so the width is the ROWS'
            // doing rather than something the condition label was always going to ask for.
            DagLayoutOptions no = o;
            no.includeMeasures = false;
            check(nodeById(layoutDag(wide, QStringLiteral("focus"), no), "focus")->w < f->w,
                  "…and it is the rows that widened it, not the name");
        }

        // Switched off, the rows go and the boxes shrink back.
        DagLayoutOptions off;
        off.includeMeasures = false;
        const DagLayout lOff = layoutDag(p, QStringLiteral("focus"), off);
        check(nodeById(lOff, "focus")->measures.empty(), "no rows when measures are off");
        check(nodeById(lOff, "focus")->h < f->h, "and the box is back to one row high");

        // No overlap once the boxes have grown — the reason the rows are counted before the columns
        // are stacked rather than attached afterwards.
        DagLayoutOptions on;
        on.depth = 2;
        const DagLayout l2 = layoutDag(p, QStringLiteral("focus"), on);
        bool ok = true;
        for (size_t i = 0; i < l2.nodes.size(); ++i)
            for (size_t j = i + 1; j < l2.nodes.size(); ++j)
                if (overlaps(l2.nodes[i], l2.nodes[j])) ok = false;
        check(ok, "grown boxes still do not overlap");
        QString why;
        const bool bad = linesCrossBoxes(l2, &why);
        if (bad) std::printf("      %s\n", qPrintable(why));
        check(!bad, "and no line is drawn through one");
    }

    // ── Citations are rows too ──────────────────────────────────────────────
    //
    // Same contract as the measures and for the same reason: a source is not a cause, so it takes a
    // row inside the box rather than a node with a line to it. What is worth asserting beyond the
    // shared geometry is the resolution — a citation the bibliography does not hold must be drawn
    // as the gap it is, and must not be pressable, because there is no page behind it to open.
    std::printf("Citations are drawn inside the condition that rests on them\n");
    {
        const ReferenceSet refs = referenceFixture();
        DagLayoutOptions   on;
        on.includeReferences = true;
        on.references        = &refs;

        const DagLayout l = layoutDag(p, QStringLiteral("focus"), on);

        const DagNode *f = nodeById(l, "focus");
        check(f && f->references.size() == 1, "a cited condition carries its source as a row");
        check(f->references[0].id == QLatin1String("ref.known"), "…named by reference id");
        check(f->references[0].resolved, "…and resolved");
        check(f->references[0].label == QLatin1String("A paper the registry knows"),
              "the row says the TITLE, not the DOI — a DOI answers nothing on its own");
        check(f->references[0].detailLabel == QLatin1String("2005"),
              "with the year beside it, which survives the title being elided");
        check(f->references[0].citation == QLatin1String("10.1234/known"),
              "and it keeps the join key it was resolved from");

        // The dangling citation. This is the one the pane is best placed to surface: it is invisible
        // everywhere else in the view, and drawing nothing would make it look like no citation.
        const DagNode *c2 = nodeById(l, "cause2");
        check(c2 && c2->references.size() == 1, "an unresolved citation still gets its row");
        check(!c2->references[0].resolved, "marked as unresolved");
        check(c2->references[0].id.isEmpty(),
              "…with no reference id, which is what makes it unpressable");
        check(c2->references[0].label.contains(QLatin1String("30479527")),
              "and rendered as the identifier a reader would recognise");

        const DagNode *c3 = nodeById(l, "cause3");
        check(c3 && c3->references.empty(), "a condition citing nothing carries no row");

        // The box grew, and the rows sit inside it — below the condition's own row, and below the
        // measures, which is the order the layout fixes rather than the view.
        check(f->h > c3->h, "a box with a citation is taller than one without");
        for (const DagReference &dr : f->references) {
            check(dr.y >= f->y && dr.y + dr.h <= f->y + f->h + 0.001,
                  "every citation row lies inside its own box");
            for (const DagMeasure &dm : f->measures)
                check(dr.y >= dm.y + dm.h - 0.001, "…and below the measures, never over them");
        }

        // Both stacks in one box, with no gap and no overlap between them: the two are placed from
        // ONE cursor, and a second cursor is how the second stack starts in the wrong place.
        {
            DagLayoutOptions both = on;
            both.includeMeasures = true;
            // The layout is held in a named local, deliberately: nodeById() returns a pointer INTO
            // it, and reading that out of a temporary is a dangle the optimiser is free to fill with
            // anything.
            const DagLayout lb = layoutDag(p, QStringLiteral("focus"), both);
            const DagNode  *fb = nodeById(lb, "focus");
            check(fb->measures.size() == 1 && fb->references.size() == 1,
                  "a box can carry a measure and a citation at once");
            check(std::abs(fb->references[0].y - (fb->measures[0].y + fb->measures[0].h)) < 0.001,
                  "and the citation begins exactly where the measures stopped");
            check(std::abs(fb->y + fb->h - (fb->references[0].y + fb->references[0].h)) < 0.001,
                  "…with the last row reaching the bottom of the box");
        }

        // A long title widens the box against the SAME cap the measure rows answer to. One geometry
        // for both kinds; two would be two numbers that must never differ.
        {
            ReferenceSet wide = refs;
            wide.references[0].title = QStringLiteral("Kinematic sequencing of the golf swing and "
                                                     "its relationship to clubhead speed in "
                                                     "low-handicap players");
            DagLayoutOptions o = on;
            o.references = &wide;
            o.maxW       = 200;
            o.rowMaxW    = 360;
            const DagLayout lw2 = layoutDag(p, QStringLiteral("focus"), o);
            const DagNode  *fw  = nodeById(lw2, "focus");
            check(fw->w > o.maxW, "a long title widens the box past the NAME cap");
            check(fw->w <= o.rowMaxW + 0.001, "…and no further than the row cap");
        }

        // Switched off, the rows go and the boxes shrink back — and a registry the caller did not
        // supply is the same answer, rather than every citation drawn as a dangling one.
        DagLayoutOptions off = on;
        off.includeReferences = false;
        check(nodeById(layoutDag(p, QStringLiteral("focus"), off), "focus")->references.empty(),
              "no rows when sources are off");
        check(nodeById(layoutDag(p, QStringLiteral("focus"), off), "focus")->h < f->h,
              "and the box is back to one row high");

        DagLayoutOptions noSet = on;
        noSet.references = nullptr;
        check(nodeById(layoutDag(p, QStringLiteral("focus"), noSet), "focus")->references.empty(),
              "no registry means no rows, never a box full of dangling citations");

        // And the picture still holds together once the boxes have grown.
        DagLayoutOptions deep = on;
        deep.includeMeasures = true;
        deep.depth           = 2;
        const DagLayout l2 = layoutDag(p, QStringLiteral("focus"), deep);
        bool ok = true;
        for (size_t i = 0; i < l2.nodes.size(); ++i)
            for (size_t j = i + 1; j < l2.nodes.size(); ++j)
                if (overlaps(l2.nodes[i], l2.nodes[j])) ok = false;
        check(ok, "boxes carrying both kinds of row still do not overlap");
        QString why;
        const bool bad = linesCrossBoxes(l2, &why);
        if (bad) std::printf("      %s\n", qPrintable(why));
        check(!bad, "and no line is drawn through one");
    }

    std::printf("What the boxes claim\n");
    {
        const DagLayout l = layoutDag(p, QStringLiteral("focus"));

        check(nodeById(l, "cause3")->offeredOnly, "an asserted cause is offered, never concluded");
        check(nodeById(l, "cause3")->latent, "and it is latent");
        check(!nodeById(l, "cause1")->offeredOnly, "a measured one is not");

        // Availability is about MEASUREMENT. cause2 is detected by a signal whose measure has no
        // producer; cause3 was never going to be measured at all, and greying it would report a
        // capability gap that does not exist.
        check(!nodeById(l, "cause2")->available, "a condition with no producer is unavailable");
        check(!nodeById(l, "cause2")->unavailableReason.isEmpty(), "and names what is missing");
        check(nodeById(l, "cause2")->unavailableReason.contains(QLatin1String("mGhost")),
              "by name, not as 'unavailable'");
        check(nodeById(l, "cause3")->available, "an asserted condition is not 'unavailable'");
        check(nodeById(l, "cause1")->available, "a live one is available");

        // Strength is a weight, never a probability. Asserted as an ORDER rather than as three
        // numbers: the band is spread over however many rungs the ladder has, so pinning 1/2/3 here
        // would fail the next time one is added without anything actually being wrong.
        double wStrong = 0, wModerate = 0, wWeak = 0;
        for (const DagEdge &e : l.edges) {
            if (e.detects) continue;
            if (e.from == QLatin1String("cause1")) wStrong = e.weight;
            if (e.from == QLatin1String("cause2")) wModerate = e.weight;
            if (e.from == QLatin1String("cause3") && e.to == QLatin1String("focus")) wWeak = e.weight;
        }
        check(wStrong > wModerate, "strong is the heavier line");
        check(wModerate > wWeak, "moderate outdraws weak");
        check(wWeak >= 1.0, "and the lightest is still drawn");

        // The two outer rungs sit outside the three the fixture uses, and inside the band the view
        // is allowed to stroke — this is what the even spread has to keep true.
        check(strokeWidthFor(Strength::VeryWeak) < wWeak, "rarely is lighter than sometimes");
        check(strokeWidthFor(Strength::VeryStrong) > wStrong, "always is heavier than usually");
        check(strokeWidthFor(Strength::VeryWeak) >= 1.0 && strokeWidthFor(Strength::VeryStrong) <= 3.0,
              "and the whole ladder stays inside 1..3px");

        // An edge whose CAUSE is asserted carries that forward: a solid arrow into the focus reads
        // as a finding on its own, whatever the box at its tail looks like.
        for (const DagEdge &e : l.edges)
            if (e.from == QLatin1String("cause3") && e.to == QLatin1String("focus"))
                check(e.offeredOnly, "an offered cause draws an offered link");
    }

    // ── The per-rank cap, and the order it cuts in ──────────────────────────
    //
    // The cap is OFF by default now. It was 10 here and 8 at the call site, and over the shipped
    // pack it was removing five of `over_the_top`'s thirteen causes at the default depth while
    // reporting only a number. What replaces it is the depth bound, which is the reader's own.
    std::printf("A hub is drawn whole unless a caller asks for a cap\n");
    {
        CharacteristicPack hub = p;
        for (int i = 0; i < 20; ++i) {
            Condition c;
            c.id    = QStringLiteral("extra%1").arg(i);
            c.label = QStringLiteral("Extra %1").arg(i);
            hub.conditions.push_back(c);
            Edge e;
            e.from = QStringLiteral("focus");
            e.to   = c.id;
            e.type = EdgeType::Causes;
            hub.edges.push_back(e);
        }

        // Twenty-two effects, and by default all twenty-two are on the picture. `truncated` is not
        // asserted either way: the causes behind cause1 are past depth 1 and it is right that they
        // still say so.
        DagLayoutOptions def;
        def.depth = 1;
        const DagLayout wide = layoutDag(hub, QStringLiteral("focus"), def);
        int atPlusOneWide = 0;
        for (const DagNode &n : wide.nodes)
            if (n.rank == 1) ++atPlusOneWide;
        check(atPlusOneWide == 22, "no cap by default: a hub keeps every effect it has");
        check(nodeById(wide, "focus")->hiddenEffects == 0, "and claims nothing is hidden");

        // Still available to a caller who wants one, unchanged.
        DagLayoutOptions o;
        o.maxPerRank = 6;
        const DagLayout l = layoutDag(hub, QStringLiteral("focus"), o);

        int atPlusOne = 0;
        for (const DagNode &n : l.nodes)
            if (n.rank == 1) ++atPlusOne;
        check(atPlusOne == 6, "the cap holds when asked for");
        check(l.truncated, "and the layout says so");
        check(nodeById(l, "focus")->hiddenEffects == 16, "the rest are counted on the focus");
    }

    // ── Ranked, not authored ────────────────────────────────────────────────
    //
    // A rank arrives in the order ADDENDUM-03 established for the Causes list — how far a cause
    // reaches — rather than in the order the edges happen to sit in the file. Pack order decided
    // both the column's reading order and, whenever a budget bit, which claims survived; the newest
    // edge was always the first to fall off, whatever it said.
    //
    // In this fixture cause3 reaches six conditions (it causes cause1 as well as the focus) while
    // cause1 and cause2 reach five each. Pack order alone reads cause1, cause2, cause3.
    std::printf("A rank is ordered by reach, not by the order the edges were written\n");
    {
        DagLayoutOptions o;
        o.depth = 1;
        const DagLayout l = layoutDag(p, QStringLiteral("focus"), o);

        const DagNode *c1 = nodeById(l, "cause1");
        const DagNode *c2 = nodeById(l, "cause2");
        const DagNode *c3 = nodeById(l, "cause3");
        check(c1 && c2 && c3, "all three causes are drawn");
        check(c3->y < c1->y, "the furthest-reaching cause leads the column");
        check(c1->y < c2->y, "and a tie keeps the authored order behind it");
    }

    std::printf("What a cap drops is the least-reaching, not the last-authored\n");
    {
        // The property that matters more than the cap's existence: with room for one, the seat goes
        // to the cause that explains the most, and the two that stand down are still counted.
        DagLayoutOptions o;
        o.depth      = 1;
        o.maxPerRank = 1;
        const DagLayout l = layoutDag(p, QStringLiteral("focus"), o);

        check(nodeById(l, "cause3") != nullptr, "the one seat goes to the cause that reaches furthest");
        check(!nodeById(l, "cause1") && !nodeById(l, "cause2"), "and the shorter reaches stand down");
        check(nodeById(l, "focus")->hiddenCauses == 2, "counted on the focus, as always");
    }

    // ── The non-causal relations ────────────────────────────────────────────
    //
    // Rank is signed causal distance, so a symmetric relation has no direction to rank by. It sits
    // on the focus's OWN rank and draws no arrowhead — an arrow would assert a direction these two
    // do not have, and a reader would take it as causal.
    std::printf("\nCorroborates and excludes are drawn, and drawn differently\n");
    {
        CharacteristicPack q = fixture();
        q.conditions.push_back(Condition{});
        q.conditions.back().id    = QStringLiteral("twin");
        q.conditions.back().label = QStringLiteral("Twin");
        q.conditions.push_back(Condition{});
        q.conditions.back().id    = QStringLiteral("rival");
        q.conditions.back().label = QStringLiteral("Rival");
        q.edges.push_back(Edge{ QStringLiteral("focus"), QStringLiteral("twin"),
                                EdgeType::Corroborates, Strength::Strong, {} });
        // Written the other way round on purpose: the edge means the same from either end, and a
        // reader must not have to know which way an author happened to type it.
        q.edges.push_back(Edge{ QStringLiteral("rival"), QStringLiteral("focus"),
                                EdgeType::Excludes, Strength::Strong, {} });

        const DagLayout l = layoutDag(q, QStringLiteral("focus"));

        check(nodeById(l, "twin") && nodeById(l, "twin")->rank == 0,
              "a corroborating partner joins the focus's own rank, not the causal flow");
        check(nodeById(l, "rival") && nodeById(l, "rival")->rank == 0,
              "…and so does an excluded one, written from either end");

        // ⚠ THE REGRESSION. Rank 0 used to MEAN "the focus", so a partner sharing that rank was
        // typed as one. It then took the focus's frame and the long-press menu skipped every item
        // — the node looked special and did nothing when pressed, with no error anywhere.
        check(nodeById(l, "twin")->kind == DagNodeKind::Related,
              "a partner on rank 0 is Related, NOT the focus");
        check(nodeById(l, "rival")->kind == DagNodeKind::Related, "…both of them");
        check(nodeById(l, "focus")->kind == DagNodeKind::Focus,
              "…and the focus is still the only Focus");

        int corroborates = 0, excludes = 0, tipped = 0;
        for (const DagEdge &e : l.edges) {
            if (e.relation == QLatin1String("corroborates")) { ++corroborates; if (e.tip) ++tipped; }
            if (e.relation == QLatin1String("excludes"))     { ++excludes;     if (e.tip) ++tipped; }
        }
        check(corroborates > 0, "the corroborates edge is emitted");
        check(excludes > 0, "the excludes edge is emitted");
        check(tipped == 0, "neither carries an arrowhead — they claim no direction");

        int symmetric = 0, labelled = 0;
        for (const DagEdge &e : l.edges) {
            if (e.symmetric) ++symmetric;
            if (e.relation == QLatin1String("excludes") && !e.strengthLabel.isEmpty()) ++labelled;
        }
        check(symmetric == corroborates + excludes, "every non-causal edge says it is symmetric");
        check(labelled == 0,
              "an exclusion carries no strength word — the pair is incompatible or it is not");

        // Evaluated into a local FIRST: the arguments to check() are evaluated in an unspecified
        // order, so building the message inline can read `why` before the call has filled it.
        QString       why;
        const bool    crosses = linesCrossBoxes(l, &why);
        const QString msg     = QStringLiteral("…and they route around the boxes too%1")
                                    .arg(why.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(why));
        check(!crosses, qPrintable(msg));

        // Scoped to the FOCUS. A corroborates edge between two of the focus's causes is a fact about
        // those two, not about the thing being read, and drawing it would answer a question nobody
        // asked on this page.
        CharacteristicPack r = fixture();
        r.edges.push_back(Edge{ QStringLiteral("cause1"), QStringLiteral("cause2"),
                                EdgeType::Corroborates, Strength::Strong, {} });
        const DagLayout lr = layoutDag(r, QStringLiteral("focus"));
        int offFocus = 0;
        for (const DagEdge &e : lr.edges)
            if (e.relation == QLatin1String("corroborates")) ++offFocus;
        check(offFocus == 0, "a non-causal edge that does not touch the focus is not drawn");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PASSED" : "FAILED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
