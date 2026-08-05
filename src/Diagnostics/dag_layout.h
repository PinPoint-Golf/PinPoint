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

#include "characteristic_pack.h"

#include <QString>
#include <QStringList>

#include <vector>

// Layout for the navigable causal DAG — a LOCAL view of the graph around one condition.
//
// All of it is here, in C++, and none of it is in QML. The view is a Repeater over nodes and a
// Shape over edges; it positions nothing and decides nothing. That is not a style preference: rank
// assignment, ordering, routing and overlap are the only things about this surface that CAN be
// tested, and a layout computed inside delegate bindings is a layout nothing can assert.
//
// ── What the picture claims ────────────────────────────────────────────────────────────────────
//
// Rank is SIGNED CAUSAL DISTANCE from the focus: causes to the left (negative), effects to the
// right (positive). Left-to-right therefore reads as "causes", every time, which is the one thing a
// reader must be able to take from the shape — and the headings say it in words rather than leaving
// it to be inferred. A node reached by two paths takes the SHORTEST — the nearest explanation is
// the one a coach acts on, and drawing it twice would make one condition look like two.
//
// The graph is bounded by `depth` and stops. The whole-library picture is the cause-coverage bar
// list, not this: fifty nodes and eighty-one edges is a hairball that says nothing, and a reader who
// cannot tell which node is the focus has been given a decoration rather than a tool. Whatever the
// bound cuts off is COUNTED on the node it was cut from (`hiddenCauses` / `hiddenEffects`) — a graph
// that silently omits half of what it knows is worse than one that draws nothing.
//
// A count is not an answer, though, and `expanded` is the answer: the reader names the boxes they
// want opened and those boxes admit their own neighbours whatever the bound says. That keeps the
// picture local where it is not being asked about and complete where it is, which a single global
// radius cannot do — raising it widens every rank at once to reach the one node in question, and
// hands back a hairball to answer a question about one box.
//
// ── Lines must not cross boxes ─────────────────────────────────────────────────────────────────
//
// An edge that spans more than one rank is routed through WAYPOINTS in the ranks it crosses, and
// those waypoints take up vertical space in their column exactly as a node does. Without them a
// cause that also causes an effect draws a line straight through the focus, and the picture reads
// as several arrows competing for the same box — which is precisely how it looked before this was
// built. Waypoints are never drawn; they only exist to reserve the room the line needs.
//
// Edges also FAN at their ends: every arrow into a node arrives at its own point on that node's
// edge rather than all of them converging on one, so two lines entering the same box stay two
// lines. And an edge between two nodes of the SAME rank bulges into the empty gutter beside its
// column, never across the focus.
//
// ── Measures are not causes, and they are not nodes either ────────────────────────────────────
//
// The measures are drawn, because "what would have to be true to see this" is the other half of the
// question the page answers. They are drawn INSIDE the condition they detect: the box grows and the
// measures are rows in it. A measure does not cause a condition, it detects one, and giving it a
// node of its own in a causal graph asked the reader to decide what the box meant every time.
//
// This replaced a detection LANE under the band, with a line from each measure up to every
// condition it detected. The lane was honest and it was unreadable. A measure serving three
// conditions drew three lines across the picture; a line into a box that was not the lowest in its
// column had to be routed up the gutter beside that column to avoid crossing the boxes beneath it;
// and the whole apparatus existed to say something a row inside the box says with no line at all.
// Inside the box, "what detects this" is answered where the question is asked.
//
// The cost is honest: a measure detecting several drawn conditions now appears in each of them,
// where the lane drew it once. That repetition is the price of locality, and it is the right way
// round — a reader looking at one condition wants its detectors listed under it, not a shared box
// three columns away with a line they have to trace.
//
// Every drawn condition answers, not the focus alone. A picture that listed detectors for one box
// left the rest looking undetectable, which is a claim about the pack and a false one.
//
// ── And neither is a citation ─────────────────────────────────────────────────────────────────
//
// The references are drawn the SAME WAY and for the same reason. "What does the app think it knows
// this from" is the third question the page answers, next to what a condition costs and what would
// have to be true to see it — and it was the one the graph could not answer at all. A citation is
// no more a cause than a measure is, so it takes a row inside the box rather than a node, and
// pressing it opens the paper.
//
// One row at most, because a condition carries one `provenance.citation`. The citations on the
// EDGES are deliberately not gathered into the boxes at either end: an edge's paper backs the LINK,
// and lifting it into the effect's box would make it read as backing the effect. An edge's evidence
// belongs on the edge, and the inspector is where it is shown.
//
// A citation the bibliography has never heard of still gets its row, marked as the gap it is —
// exactly as a measure nothing can produce does. A dangling citation is invisible everywhere else
// in this view, and the box that carries it is the only place the reader is asking about it.

namespace pinpoint::analysis {

struct ReferenceSet;

// NB `Related` shares rank 0 with the focus but is NOT the focus. Typing it as one — which is what
// "rank 0 means focus" did — gave it the focus's frame and made every menu item skip it, so a
// press-and-hold on a corroborating partner opened nothing at all.
enum class DagNodeKind {
    Focus,     // the condition the view is centred on
    Cause,     // rank < 0
    Effect,    // rank > 0
    Related,   // rank 0, beside the focus: corroborates or excludes it. Not a cause, not the focus
};

QString dagNodeKindName(DagNodeKind k);

// One measure, drawn as a row inside the box of a condition it detects.
//
// `y` is ABSOLUTE, in the same space as DagNode::y, rather than an offset the view has to add. The
// row is a hit target as well as a drawing — pressing it opens the measure — and a drawn rect and a
// clickable rect derived from two different numbers is the defect that arithmetic invites.
struct DagMeasure {
    QString id;
    QString label;
    QString statusLabel;        // "Live", "No producer", …
    QString metricKey;
    bool    available = true;   // Live; anything else is a gap and is drawn as one
    QString unavailableReason;
    double  y = 0, h = 0;
};

// One citation, drawn as a row inside the box of the condition that rests on it. Same geometry
// contract as DagMeasure — absolute `y`, one number for the drawing and the hit test both.
struct DagReference {
    QString id;             // `ref.*`. EMPTY when the citation resolves to nothing, which is what
                            // makes an unresolved row unpressable: there is no page to open.
    QString citation;       // the DOI, PMID or ISBN as the pack stores it — the join key itself
    QString label;          // the paper's title; the citation's own label when it does not resolve
    QString detailLabel;    // the year, or the reason the row is a gap
    bool    resolved = true;
    double  y = 0, h = 0;
};

struct DagNode {
    QString     id;
    DagNodeKind kind = DagNodeKind::Cause;
    QString     label;
    int         rank = 0;      // signed causal distance; a measure carries its condition's rank

    // Geometry, in layout units. The caller supplies the unit scale through DagLayoutOptions, so
    // "layout units" are whatever Theme.sp() gave it — this code does no scaling of its own.
    double x = 0, y = 0, w = 0, h = 0;

    // ── Encoding ───────────────────────────────────────────────────────────
    // Decided here rather than in the delegate, because each of these is a claim about the content
    // and the view must not be free to make a different one.

    // Cannot be seen in the swing; it is inferred from what it explains. Drawn as an outline, so a
    // reader can tell at a glance which boxes are things the app saw and which are things it worked
    // out. (Observability::Both counts as observable — something CAN be seen.)
    bool    latent = false;

    // ConfirmedBy::Asserted. The app may OFFER it as an explanation and must never conclude it, so
    // it has to look different everywhere it appears, including here.
    bool    offeredOnly = false;
    QString reach;         // measured | screened | asserted
    QString reachLabel;

    // False when this condition is supposed to be MEASURED and nothing can currently produce the
    // measure it needs. `unavailableReason` names that measure — a greyed box with no explanation
    // is indistinguishable from a rendering bug.
    //
    // Latent, screened and asserted conditions are never "unavailable": they were never going to be
    // measured, and greying them would report a capability gap that does not exist.
    bool    available = true;
    QString unavailableReason;

    // The measures that detect this condition, drawn as rows INSIDE its box — which is why the box
    // is taller than `nodeH` whenever this is non-empty. Empty when the caller did not ask for
    // measures, and on a condition nothing detects.
    std::vector<DagMeasure> measures;

    // What this condition is cited from, as one more row under the measures. At most one — a
    // condition carries a single `provenance.citation` — and empty when the caller did not ask for
    // references, or the condition cites nothing.
    //
    // BELOW the measures, always, so the stack reads the way the question does: what this is, what
    // would show it, then what says so. The order is fixed here rather than left to the view, which
    // hit-tests the same y values it draws from.
    std::vector<DagReference> references;

    int     coverage      = 0;   // how many conditions it explains directly
    int     hiddenCauses  = 0;   // neighbours the depth bound (or the per-rank cap) cut off
    int     hiddenEffects = 0;

    // The reader opened this one — it is in DagLayoutOptions::expanded AND it made it onto the
    // picture. Reported rather than left for the view to re-derive from the same list, so a node
    // opened and then walked away from cannot draw itself as open on a graph it is not in.
    bool    expanded      = false;
    QString groupLabel;
};

// One drawn curve. A single causal relationship spanning more than one rank is emitted as SEVERAL
// of these — `segment` of `segments` — sharing `from` and `to` and meeting exactly at their
// waypoints, with horizontal tangents on both sides of the join so the chain reads as one line.
struct DagEdge {
    QString from;      // the CAUSE (Edge's own orientation, unchanged)
    QString to;        // the EFFECT
    QString strength;  // veryWeak | weak | moderate | strong | veryStrong
    QString strengthLabel;

    // STROKE WIDTH in px, 1..3, spread evenly across the five rungs — not strengthWeight(), which
    // orders causes and has no business setting a line width. The band is what it always was, so the
    // two new rungs cost half a pixel each at the ends rather than a fatter graph. The view scales a
    // line by it; nothing prints it.
    double  weight = 2.0;

    // Which relationship this line states: "causes" | "corroborates" | "excludes". The graph is
    // RANKED by causal distance, so a symmetric relation has no direction to rank by and is only
    // ever drawn on the focus's own rank — see the admission rule in the .cpp. It also gets no
    // arrowhead: an arrow claims a direction these two do not have.
    QString relation;
    bool    symmetric = false;

    // The cause is Asserted, so the LINK is offered rather than concluded. Carried on the edge as
    // well as the node because a solid arrow into the focus reads as a finding on its own.
    bool    offeredOnly = false;

    int     segment  = 0;
    int     segments = 1;

    // A cubic, anchored on the two node edges. QML draws it; it does not compute it.
    double  x1 = 0, y1 = 0, c1x = 0, c1y = 0, c2x = 0, c2y = 0, x2 = 0, y2 = 0;

    // The strength IN WORDS, on the one segment with room for it. Empty everywhere else, so the
    // view can render every edge the same way and let this decide whether a label appears.
    QString label;
    double  labelX = 0, labelY = 0;

    // Arrowhead, on the last segment only. Emitted as a triangle rather than an angle so the view
    // does no trigonometry: which end is the effect is the whole claim of a causal graph, and it
    // must not be left to a rotation the delegate works out.
    bool    tip = false;
    double  tipAx = 0, tipAy = 0, tipBx = 0, tipBy = 0, tipCx = 0, tipCy = 0;
};

// The stroke width `DagEdge::weight` carries, 1..3px, spread evenly over the strength ladder. Public
// because it is the view's rule about the ladder and not a private detail of one walk — the layout
// is where the picture's numbers are decided, and a test that pins three literals instead breaks
// every time a rung is added without anything being wrong.
double strokeWidthFor(Strength s);

// A word over a region of the picture: "Caused by", "Leads to", "Measured by". The direction of a
// causal graph is not self-evident from arrowheads alone, and a reader should not have to work out
// what the left-hand side of the screen means.
struct DagHeading {
    QString label;
    double  x = 0, y = 0, w = 0;
};

struct DagLayoutOptions {
    double nodeH   = 40;    // every node is one row high — the boxes differ in width, never in rank
    double gapX    = 110;   // between columns; wide enough for a label to sit on the line
    double gapY    = 26;    // between nodes within a column
    // ── The rows INSIDE a box ──────────────────────────────────────────────
    // One geometry for both kinds. A measure row and a reference row are the same object drawn with
    // different words — same height, same text size, same cap — and giving each its own triple of
    // options would be three numbers that must never differ, which is three ways for them to.
    double rowH = 26;      // one inner row, measure or reference

    // The advance for an inner row's text, which the view draws smaller than the condition's own
    // name. Sizing those rows with `charW` overestimated their width by the ratio between the two
    // font sizes and pushed almost every box with a measure straight to `maxW` — a graph that
    // doubled in width when the switch was flipped, for text that was never that wide. Supplied by
    // the caller for the same reason charW is: the layout has no font metrics and must be told.
    double rowCharW = 5.8;

    // The width cap for a box carrying inner rows, separate from maxW and deliberately larger.
    // maxW bounds a condition's own name, which is a phrase; a measure's label is a sentence —
    // "thorax centre distance relative to trail ankle, change P1 to P5" is 63 characters, and a
    // paper's title is longer again. Sharing the one cap fitted 32 % of the shipped measure rows
    // whole and elided the rest, which is a row the reader can see is there and cannot read. At 360
    // it is 95 %, and the remainder elide as long condition labels always have.
    //
    // The cost is a wider graph whenever the rows are on. That is the trade the switches make: the
    // view opens fitted, so a wider picture arrives more zoomed out rather than off the canvas.
    double rowMaxW = 360;
    double padX    = 16;    // horizontal padding inside a node
    double charW   = 7.0;   // approximate advance per character; see the note in the .cpp
    double minW    = 110;
    double maxW    = 260;
    double headerH = 26;    // reserved above the band for the headings

    // How far the automatic walk reaches, either side. Values outside [1, 4] are clamped rather
    // than honoured: this is a local view by design, and "show me everything" is a different
    // surface. Four is where a real chain in the pack runs out — slice ← out-to-in ← over the top ←
    // its own causes is already three — and past it the per-rank cap does all the work anyway.
    int    depth = 1;

    // Per-rank cap, OFF. It was 10 here and 8 at the call site, and it was answering a fear rather
    // than a measurement: the worry was that a hub cause explains a dozen conditions and an
    // uncapped depth-2 fan-out returns the hairball this view exists to avoid.
    //
    // Measured over the shipped pack, that does not happen, because FIRST VISIT WINS converges the
    // walk fast — a node already ranked is never admitted again, so the second rank is the frontier
    // minus everything the first already took. The worst focus in the pack is `over_the_top`:
    //
    //     depth 1   19 nodes, widest rank 13        depth 3   44 nodes, widest rank 24
    //     depth 2   32 nodes, widest rank 18        depth 4   50 nodes, widest rank 24
    //
    // Fifty boxes at the maximum depth the UI offers, and depth is already the reader's own choice.
    // What the cap bought against that was a picture that silently omitted five of `over_the_top`'s
    // thirteen causes at the default depth — including the strongest one — and the reader had no
    // way to know which five, because the count says how many and never which.
    //
    // Kept as an option rather than deleted: it is the only backstop if the pack grows several
    // times over, and it costs one comparison. A caller that sets one gets the same bargain as
    // before, improved — what it drops is the LOWEST RANKED (see the ordering note in the .cpp),
    // not whatever was authored last, and it still reappears in the parent's hidden counts.
    int    maxPerRank = 0;

    // Nodes the reader has opened BY HAND. Their neighbours are admitted whatever `depth` says.
    //
    // This is the other half of the answer to a bounded picture, and the better half: raising the
    // radius widens every rank at once to reach the one box you were actually asking about, which
    // costs the reader the picture they had. Opening one node costs them nothing.
    //
    // Direction follows the side the node is on — a CAUSE opens further left, an EFFECT further
    // right, and rank 0 opens both ways. Opening a cause towards its effects would fold the graph
    // back over itself and break the one claim the shape makes.
    QStringList expanded;

    // Per-OPENED-NODE fan-out, not per rank. Sharing the rank's cap is what would make opening a
    // hub useless: its dozen causes arrive at a rank already holding somebody else's eight, and the
    // reader gets nothing for the click. Same bargain as maxPerRank — what it drops is counted on
    // the node it was dropped from. 0 disables it.
    int    maxPerExpand = 12;

    bool   includeMeasures = true;

    // The bibliography rows. OFF by default like measures, and for the same reason: the causal band
    // is what the picture is about, and a detail hung under every box belongs behind a switch.
    //
    // Needs `references` to be set as well. A layout asked for citation rows with no registry to
    // resolve them against would draw every one of them as a dangling citation, which is a claim
    // about the pack and would be a false one — so it draws none instead.
    bool   includeReferences = false;

    // The bibliography to resolve `provenance.citation` against. NOT reached for through
    // sharedReferenceSet() from inside the layout: this module takes what it needs as arguments, and
    // a global read here would make the picture depend on process state no test could set.
    const ReferenceSet *references = nullptr;

    // The physical-screen layer: conditions whose ConfirmedBy is Screened — limited trail-hip
    // internal rotation, poor core stability, thoracic kyphosis and the rest. ON by default, unlike
    // measures, because they are the far LEFT of the causal band rather than a detail hung under
    // it: they are where the chain of technique faults bottoms out, and a picture that opened
    // without them would show a golfer their swing and hide the reason for it.
    //
    // Turning it off is for reading the swing on its own terms — a coach working on the move today
    // does not need a mobility finding they cannot change in the lesson. What it removes is COUNTED
    // like every other bound: a screened cause dropped here lands in its child's hiddenCauses, so
    // the picture still says the explanation continues.
    //
    // The FOCUS is exempt. Centring the graph on a screened cause and having it vanish would be a
    // filter deleting the thing it was asked to draw.
    bool   includeScreened = true;
};

struct DagLayout {
    std::vector<DagNode>    nodes;
    std::vector<DagEdge>    edges;
    std::vector<DagHeading> headings;

    double width = 0, height = 0;   // bounding box; the origin is (0, 0)
    double focusX = 0, focusY = 0;  // centre of the focus node, for scroll-to-centre

    // True when the depth bound or the per-rank cap left something out. The view says so; it does
    // not have to work it out by summing hidden counts.
    bool   truncated = false;
};

// Lay out the neighbourhood of `focusId`. An unknown id returns an EMPTY layout rather than a
// nearest guess — a stale deep link must land on nothing visible, not on the wrong condition.
DagLayout layoutDag(const CharacteristicPack &pack,
                    const QString            &focusId,
                    const DagLayoutOptions   &opt = {});

} // namespace pinpoint::analysis
