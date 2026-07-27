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
//   3. THE DEPTH BOUND HOLDS, and whatever it cut off is counted rather than dropped silently.
//   4. AN ISOLATED NODE lays out. It is the empty state of this view and the most likely one for a
//      freshly authored characteristic.
//
//   cmake --build build/analyzer-tests --target dag_layout_test
//   ctest --test-dir build/analyzer-tests -R dag_layout --output-on-failure

#include "../dag_layout.h"

#include <cstdio>

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
    return p;
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
            if (n.kind != DagNodeKind::Measure)
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

        // The bound is a bound, not a suggestion: 3 clamps to 2 rather than walking the library.
        DagLayoutOptions o3;
        o3.depth = 3;
        const DagLayout d3 = layoutDag(p, QStringLiteral("focus"), o3);
        for (const DagNode &n : d3.nodes)
            if (n.kind != DagNodeKind::Measure)
                check(std::abs(n.rank) <= 2, "depth clamps at 2");

        check(!nodeById(d2, "island"), "an unconnected condition is not in someone else's graph");
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
        for (int depth : { 1, 2 }) {
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
        check(measuredBy, "so is the detection lane");

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

    // ── The detection lane ──────────────────────────────────────────────────
    std::printf("Measures are detection, not causation\n");
    {
        const DagLayout l = layoutDag(p, QStringLiteral("focus"));
        const DagNode  *m = nodeById(l, "mLive");
        check(m != nullptr, "the focus's measure is drawn");
        check(m->kind == DagNodeKind::Measure, "as a measure");

        // Below the whole causal band, so nothing in the left-to-right flow can be read as caused
        // by a measure.
        double bandBottom = 0;
        for (const DagNode &n : l.nodes)
            if (n.kind != DagNodeKind::Measure) bandBottom = std::max(bandBottom, n.y + n.h);
        check(m->y >= bandBottom, "in its own lane beneath the band");

        int detects = 0, causal = 0;
        for (const DagEdge &e : l.edges) (e.detects ? detects : causal)++;
        check(detects == 1, "one detection edge");
        // Every Causes edge whose BOTH ends are drawn — the three into the focus, the two out of
        // it, the same-rank cause3 -> cause1, and cause2 -> effect2, which spans the focus column
        // and is therefore emitted as TWO segments. Cross-links are not extra: an edge dropped for
        // not lying between adjacent ranks would show two independent causes where the pack holds
        // a chain.
        check(causal == 8, "and every causal edge among the drawn nodes, in segments");

        int spanning = 0;
        for (const DagEdge &e : l.edges)
            if (e.from == QLatin1String("cause2") && e.to == QLatin1String("effect2")) ++spanning;
        check(spanning == 2, "an edge crossing a rank is routed through it, not over it");

        int tips = 0;
        for (const DagEdge &e : l.edges) if (e.tip) ++tips;
        check(tips == 7, "one arrowhead per causal relationship, on its last segment only");
        for (const DagEdge &e : l.edges)
            if (e.detects) check(!e.tip, "a detection edge carries no arrowhead");

        // A same-rank edge (cause3 -> cause1) is still drawn. Dropping it would show two independent
        // causes where the pack holds a chain.
        bool sameRank = false;
        for (const DagEdge &e : l.edges)
            if (e.from == QLatin1String("cause3") && e.to == QLatin1String("cause1")) sameRank = true;
        check(sameRank, "an edge between two nodes of the same rank is drawn");
    }

    // ── The encoding ────────────────────────────────────────────────────────
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

        // Strength is a weight, never a probability. Three words in, three weights out.
        for (const DagEdge &e : l.edges) {
            if (e.detects) continue;
            if (e.from == QLatin1String("cause1")) check(e.weight == 3, "strong is the heaviest line");
            if (e.from == QLatin1String("cause2")) check(e.weight == 2, "moderate is the middle");
            if (e.from == QLatin1String("cause3") && e.to == QLatin1String("focus"))
                check(e.weight == 1, "weak is the lightest");
        }

        // An edge whose CAUSE is asserted carries that forward: a solid arrow into the focus reads
        // as a finding on its own, whatever the box at its tail looks like.
        for (const DagEdge &e : l.edges)
            if (e.from == QLatin1String("cause3") && e.to == QLatin1String("focus"))
                check(e.offeredOnly, "an offered cause draws an offered link");
    }

    // ── The per-rank cap ────────────────────────────────────────────────────
    std::printf("A hub does not become a hairball\n");
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

        DagLayoutOptions o;
        o.maxPerRank = 6;
        const DagLayout l = layoutDag(hub, QStringLiteral("focus"), o);

        int atPlusOne = 0;
        for (const DagNode &n : l.nodes)
            if (n.kind != DagNodeKind::Measure && n.rank == 1) ++atPlusOne;
        check(atPlusOne == 6, "the cap holds");
        check(l.truncated, "and the layout says so");
        check(nodeById(l, "focus")->hiddenEffects == 16, "the rest are counted on the focus");
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
