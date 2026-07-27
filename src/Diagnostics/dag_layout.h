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
// ── Measures are not causes ────────────────────────────────────────────────────────────────────
//
// The focus's measures are drawn, because "what would have to be true to see this" is the other
// half of the question the page answers — but they live in their own LANE below the causal band,
// never in the ranking. A measure does not cause a condition; it detects one, and placing it in the
// same left-to-right flow as the causes would state a relationship the pack does not hold.

namespace pinpoint::analysis {

enum class DagNodeKind {
    Focus,     // the condition the view is centred on
    Cause,     // rank < 0
    Effect,    // rank > 0
    Measure,   // the detection lane — navigates into the measure, never re-centres the graph
};

QString dagNodeKindName(DagNodeKind k);

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

    int     coverage      = 0;   // how many conditions it explains directly
    int     hiddenCauses  = 0;   // neighbours the depth bound (or the per-rank cap) cut off
    int     hiddenEffects = 0;
    QString groupLabel;
    QString statusLabel;         // measure nodes only — "Live", "No producer", …
    QString metricKey;           // measure nodes only
};

// One drawn curve. A single causal relationship spanning more than one rank is emitted as SEVERAL
// of these — `segment` of `segments` — sharing `from` and `to` and meeting exactly at their
// waypoints, with horizontal tangents on both sides of the join so the chain reads as one line.
struct DagEdge {
    QString from;      // the CAUSE (Edge's own orientation, unchanged)
    QString to;        // the EFFECT
    QString strength;  // weak | moderate | strong; empty on a detection edge
    QString strengthLabel;

    // 1..3, for stroke weight. It is a RANKING WEIGHT and never a probability — the same reason
    // strength is three words and not a percentage. The view scales a line by it; nothing prints it.
    int     weight = 2;

    // The detection lane: measure -> condition. Not a causal claim, and drawn differently.
    bool    detects = false;

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
    double laneGap = 64;    // between the causal band and the detection lane
    double padX    = 16;    // horizontal padding inside a node
    double charW   = 7.0;   // approximate advance per character; see the note in the .cpp
    double minW    = 110;
    double maxW    = 260;
    double headerH = 26;    // reserved above the band for the headings

    // 1 by default, 2 on Expand, and it stops there. Values outside [1, 2] are clamped rather than
    // honoured: this is a local view by design, and "show me everything" is a different surface.
    int    depth = 1;

    // Per-rank cap. A hub cause explains a dozen conditions, and at depth 2 an uncapped fan-out
    // turns the navigation surface back into the hairball this exists to avoid. Nodes past the cap
    // are dropped in pack order and reappear in their parent's hidden counts, so nothing is lost
    // silently. 0 disables the cap.
    int    maxPerRank = 10;

    bool   includeMeasures = true;
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
