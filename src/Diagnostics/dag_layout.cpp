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

#include "dag_layout.h"

#include <QHash>
#include <QPointF>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <map>

namespace pinpoint::analysis {

namespace {

// Node width from label length, not from font metrics.
//
// This module is Qt-only and has no QFontMetrics, which is the right trade: the node is a
// fixed-height pill with ELIDED text, so an approximate width costs a little slack at the ends of a
// label and nothing else. What must be exact is that no two nodes overlap, and overlap is decided
// from these same numbers — the layout and the collision test cannot disagree about a width they
// both read from here. The caller passes `charW` alongside the font size it will actually render
// with, so the estimate tracks the theme rather than assuming one.
double nodeWidth(const QString &label, const DagLayoutOptions &opt)
{
    const double raw = opt.padX * 2.0 + opt.charW * double(label.size());
    return std::clamp(raw, opt.minW, opt.maxW);
}

QString measureLabelOf(const Measure &m)
{
    return m.label.isEmpty() ? canonicalMeasureLabel(m.series, m.reducer) : m.label;
}

// Can this condition be measured TODAY, and if not, what is missing?
//
// Only a Measured condition can be unavailable. A latent cause was never going to be measured, and
// a screened one is resolved by a physical test — greying either would report a capability gap that
// does not exist and put two very different "we don't know" states behind one grey box.
void resolveAvailability(const CharacteristicPack &pack, const Condition &c,
                         bool &available, QString &reason)
{
    available = true;
    reason.clear();
    if (c.confirmedBy != ConfirmedBy::Measured) return;

    if (c.detectedBy.isEmpty()) {
        available = false;
        reason    = QStringLiteral("Nothing detects this yet.");
        return;
    }

    // One signal whose every measure is live is enough — a condition with two ways of being seen is
    // available while either of them works.
    QString firstGap;
    for (const QString &sid : c.detectedBy) {
        const Signal *s = pack.signal(sid);
        if (!s) continue;
        bool allLive = true;
        for (const QString &mid : s->measures) {
            const Measure *m = pack.measure(mid);
            if (m && m->status == MeasureStatus::Live) continue;
            allLive = false;
            if (firstGap.isEmpty() && m)
                firstGap = QStringLiteral("Needs %1 — %2.")
                               .arg(measureLabelOf(*m), measureStatusLabel(m->status));
            else if (firstGap.isEmpty())
                firstGap = QStringLiteral("Needs a measure that is not in the library.");
        }
        if (allLive) return;
    }
    available = false;
    reason    = firstGap.isEmpty() ? QStringLiteral("Nothing detects this yet.") : firstGap;
}

// ── Internal layout types ───────────────────────────────────────────────────

// A row in a column. Either a real node or a WAYPOINT — an invisible placeholder that an edge
// crossing this rank passes through, and which takes up vertical space so the line has somewhere to
// go that is not through a box.
struct Slot {
    QString id;                 // empty => waypoint
    int     routeIndex = -1;    // waypoint: which route it belongs to
    double  x = 0, y = 0, w = 0, h = 0;

    bool   isWaypoint() const { return id.isEmpty(); }
    double centreY() const { return y + h / 2.0; }
    double centreX() const { return x + w / 2.0; }
};

// One causal relationship, as the chain of points its line actually visits.
struct Route {
    int              edgeIndex = -1;   // into pack.edges
    QString          from, to;
    int              rankFrom = 0, rankTo = 0;
    std::vector<int> waypointRanks;    // the ranks it crosses, in travel order
};

// Evaluate a cubic at t. Used for the label anchor and for the arrowhead's direction.
QPointF cubicAt(QPointF p0, QPointF c1, QPointF c2, QPointF p3, double t)
{
    const double u = 1.0 - t;
    return QPointF(u * u * u * p0.x() + 3 * u * u * t * c1.x() + 3 * u * t * t * c2.x() + t * t * t * p3.x(),
                   u * u * u * p0.y() + 3 * u * u * t * c1.y() + 3 * u * t * t * c2.y() + t * t * t * p3.y());
}

} // namespace

QString dagNodeKindName(DagNodeKind k)
{
    switch (k) {
    case DagNodeKind::Focus:   return QStringLiteral("focus");
    case DagNodeKind::Cause:   return QStringLiteral("cause");
    case DagNodeKind::Effect:  return QStringLiteral("effect");
    case DagNodeKind::Related: return QStringLiteral("related");
    case DagNodeKind::Measure: return QStringLiteral("measure");
    }
    return QString();
}

DagLayout layoutDag(const CharacteristicPack &pack, const QString &focusId,
                    const DagLayoutOptions &optIn)
{
    DagLayout out;

    const Condition *focus = pack.condition(focusId);
    if (!focus) return out;   // a stale link lands on nothing, never on a nearest guess

    DagLayoutOptions opt = optIn;
    opt.depth = std::clamp(opt.depth, 1, 4);

    const QSet<QString> openSet(opt.expanded.begin(), opt.expanded.end());

    // ── 1. Rank assignment ──────────────────────────────────────────────────
    // Breadth-first in each direction, FIRST VISIT WINS, so a node reachable by two paths takes the
    // shortest one. Drawing it at both distances would make one condition look like two, and the
    // nearer explanation is the one a coach acts on.
    QHash<QString, int>                 rankOf;
    std::map<int, std::vector<QString>> byRank;   // ordered by key: leftmost column first

    rankOf.insert(focusId, 0);
    byRank[0].push_back(focusId);

    auto walk = [&](int dir) {
        std::vector<QString> frontier{ focusId };
        for (int d = 1; d <= opt.depth && !frontier.empty(); ++d) {
            const int            r = dir * d;
            std::vector<QString> next;
            for (const QString &id : frontier) {
                const QStringList nb = dir < 0 ? causesOf(pack, id) : effectsOf(pack, id);
                for (const QString &n : nb) {
                    if (rankOf.contains(n)) continue;
                    if (!pack.condition(n)) continue;
                    // Past the cap the node is simply not admitted. It is not lost: it has no rank,
                    // so it lands in its parent's hidden count below, which is where the view says
                    // how much is off screen.
                    if (opt.maxPerRank > 0 && int(byRank[r].size()) >= opt.maxPerRank) continue;
                    rankOf.insert(n, r);
                    byRank[r].push_back(n);
                    next.push_back(n);
                }
            }
            frontier = next;
        }
    };
    walk(-1);
    walk(+1);

    // ── 1b. The focus's non-causal partners, admitted to its OWN rank ───────
    //
    // Corroborates and Excludes are symmetric, so they have no direction to rank by and cannot join
    // the left-to-right causal flow without stating a claim the pack does not hold. They belong
    // beside the focus, on rank 0.
    //
    // Scoped to the FOCUS deliberately. A corroborates edge between two of the focus's causes is a
    // fact about those two, not about the thing being read, and drawing every non-causal edge
    // anywhere in the subgraph would fill the picture with lines that answer a question nobody
    // asked. The claim this makes is narrow and true: "this finding — and what else says the same
    // thing, or rules it out".
    //
    // A partner already ranked as a cause or an effect keeps that rank: it is in the picture, the
    // edge will still be drawn to it, and moving it would break the causal reading to serve the
    // secondary one.
    for (EdgeType t : { EdgeType::Corroborates, EdgeType::Excludes }) {
        for (const Edge &e : pack.edges) {
            if (e.type != t) continue;
            const QString other = (e.from == focusId) ? e.to
                                : (e.to == focusId)   ? e.from
                                                      : QString();
            if (other.isEmpty() || rankOf.contains(other) || !pack.condition(other)) continue;
            if (opt.maxPerRank > 0 && int(byRank[0].size()) >= opt.maxPerRank) continue;
            rankOf.insert(other, 0);
            // ABOVE the focus, so the focus stays at the bottom of its own column. The detection
            // lane hangs beneath rank 0 and its edges run straight up into the focus; a partner
            // stacked below would sit in that path, and the test caught exactly that
            // (`mLive->focus crosses rival`). Keeping the focus adjacent to its lane is a property
            // of the ordering, not something the router has to work around.
            byRank[0].insert(byRank[0].begin(), other);
        }
    }

    // ── 1c. What the reader opened by hand ─────────────────────────────────
    //
    // Runs AFTER the automatic walk and after the partners, so neither can be displaced by it: a
    // node admitted by the walk keeps its shortest-path rank, and a partner of the focus keeps its
    // seat beside the focus. Opening only ever ADDS.
    //
    // Direction follows the side of the picture the node is on. A cause opens further left, an
    // effect further right, and rank 0 — the focus and its partners — opens both ways. The
    // alternative, opening every neighbour in both directions, would put a cause's effects to the
    // right of it in a column that means "effects of the FOCUS", which is a claim the pack does not
    // hold.
    //
    // maxPerRank is deliberately not consulted here. It exists to stop an automatic walk from
    // fanning a rank into a hairball nobody asked for; this fan-out is precisely what was asked
    // for, and it answers to its own budget instead.
    //
    // Iterated to a fixed point rather than swept once: opening a node two hops out only puts it on
    // the picture once whatever opened its parent has run, and a single pass in pack order would
    // silently drop whichever of the two the author happened to open second.
    if (!openSet.isEmpty()) {
        QHash<QString, int> spent;   // per opened node, across both directions
        bool                grew = true;
        while (grew) {
            grew = false;
            for (const QString &id : opt.expanded) {
                const auto rit = rankOf.constFind(id);
                if (rit == rankOf.constEnd()) continue;   // opened, but not on this picture
                const int r = rit.value();
                for (int dir : { -1, 1 }) {
                    if ((r < 0 && dir > 0) || (r > 0 && dir < 0)) continue;
                    const int         nr = r + dir;
                    const QStringList nb = dir < 0 ? causesOf(pack, id) : effectsOf(pack, id);
                    for (const QString &n : nb) {
                        if (rankOf.contains(n)) continue;
                        if (!pack.condition(n)) continue;
                        if (opt.maxPerExpand > 0 && spent.value(id) >= opt.maxPerExpand) break;
                        ++spent[id];
                        rankOf.insert(n, nr);
                        byRank[nr].push_back(n);
                        grew = true;
                    }
                }
            }
        }
    }

    // ── 2. Ordering within each rank — barycentre, sweeping OUTWARD ─────────
    // Each rank is ordered against the rank one step closer to the focus, which by then is fixed.
    // Sweeping outward rather than iterating to convergence keeps it deterministic in one pass, and
    // at the depths this view allows the extra passes buy nothing.
    //
    // The sweep runs to the OUTERMOST RANK PRESENT, not to `depth`. An opened node puts a rank past
    // the bound on the picture, and bounding the sweep by `depth` instead left exactly those ranks
    // in pack order — the one place the reader has just said they are looking.
    int outermost = 0;
    for (const auto &entry : byRank) outermost = std::max(outermost, std::abs(entry.first));
    for (int k = 1; k <= outermost; ++k) {
        for (int dir : { -1, 1 }) {
            const int r    = dir * k;
            const int prev = dir * (k - 1);
            if (!byRank.count(r) || !byRank.count(prev)) continue;

            QHash<QString, int> prevIndex;
            for (int i = 0; i < int(byRank[prev].size()); ++i) prevIndex.insert(byRank[prev][i], i);

            std::vector<QString> &ids = byRank[r];
            std::vector<double>   bary(ids.size(), 0.0);
            for (int i = 0; i < int(ids.size()); ++i) {
                const QStringList toward = dir < 0 ? effectsOf(pack, ids[i]) : causesOf(pack, ids[i]);
                double sum = 0.0;
                int    n   = 0;
                for (const QString &t : toward) {
                    const auto it = prevIndex.constFind(t);
                    if (it == prevIndex.constEnd()) continue;
                    sum += it.value();
                    ++n;
                }
                // No neighbour on the adjacent rank (it is reached through a skip edge): hold its
                // position rather than inventing one, so pack order survives.
                bary[i] = n > 0 ? sum / n : double(i);
            }

            std::vector<int> order(ids.size());
            for (int i = 0; i < int(order.size()); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(),
                             [&](int a, int b) { return bary[a] < bary[b]; });

            std::vector<QString> sorted;
            sorted.reserve(ids.size());
            for (int i : order) sorted.push_back(ids[i]);
            ids = sorted;
        }
    }

    // ── 3. Routes, and the waypoints they need ──────────────────────────────
    // An edge spanning more than one rank gets a waypoint in every rank it crosses. Without them it
    // would be drawn as one long curve straight over whatever sits in between — which is how a
    // cause that also causes an effect ends up with its line running through the focus box, and why
    // the picture read as several arrows competing for the same node.
    std::vector<Route> routes;
    for (int i = 0; i < int(pack.edges.size()); ++i) {
        const Edge &e = pack.edges[size_t(i)];
        // Causal edges anywhere in the admitted subgraph; symmetric ones only where they touch the
        // focus, matching the admission rule above. A corroborates edge between two causes would
        // otherwise appear as a line with no arrow between two boxes, saying nothing a reader of
        // THIS page came for.
        if (e.type != EdgeType::Causes && e.from != focusId && e.to != focusId) continue;
        const auto rf = rankOf.constFind(e.from);
        const auto rt = rankOf.constFind(e.to);
        if (rf == rankOf.constEnd() || rt == rankOf.constEnd()) continue;

        Route r;
        r.edgeIndex = i;
        r.from      = e.from;
        r.to        = e.to;
        r.rankFrom  = rf.value();
        r.rankTo    = rt.value();
        // In TRAVEL order, whichever way that runs. An edge whose cause sits further from the focus
        // than its effect walks right-to-left, and the ranks it crosses need waypoints just as much
        // — bounding this to the ascending case left those lines to be drawn as one long curve over
        // whatever was in between, which is the defect waypoints exist to prevent. Rare at depth 2,
        // ordinary once a node is opened three ranks out.
        if (r.rankTo != r.rankFrom) {
            const int step = r.rankTo > r.rankFrom ? 1 : -1;
            for (int rr = r.rankFrom + step; rr != r.rankTo; rr += step) r.waypointRanks.push_back(rr);
        }
        routes.push_back(r);
    }

    // ── 4. Columns ──────────────────────────────────────────────────────────
    // Laid out left to right at the width of their widest node, so no two nodes from adjacent ranks
    // can ever overlap however long a label is. Within a column the stacking pitch is height + gapY,
    // so no two rows in one column can either. Overlap is therefore a property of these two rules,
    // which is what makes it assertable.
    std::map<int, std::vector<Slot>> cols;
    for (auto &entry : byRank) {
        for (const QString &id : entry.second) {
            const Condition *c = pack.condition(id);
            Slot s;
            s.id = id;
            s.w  = nodeWidth(c ? c->label : id, opt);
            s.h  = opt.nodeH;
            cols[entry.first].push_back(s);
        }
    }

    // Where a waypoint sits in its column: interpolate between the ordinal positions of the two
    // real ends, in fractions so ranks of different sizes are comparable, then insert at the
    // matching place. It is an approximation — the real y values move once everything is stacked —
    // but it only decides ORDER, and a deterministic approximation beats an iterative one nobody
    // can predict.
    {
        auto ordinalFraction = [&](const QString &id, int rank) {
            const std::vector<QString> &ids = byRank[rank];
            const auto                  it  = std::find(ids.begin(), ids.end(), id);
            if (it == ids.end() || ids.empty()) return 0.5;
            return (double(it - ids.begin()) + 0.5) / double(ids.size());
        };

        struct Pending { int rank; Slot slot; double key; };
        std::vector<Pending> pending;

        for (int ri = 0; ri < int(routes.size()); ++ri) {
            const Route &r = routes[size_t(ri)];
            if (r.waypointRanks.empty()) continue;
            const double f0 = ordinalFraction(r.from, r.rankFrom);
            const double f1 = ordinalFraction(r.to, r.rankTo);
            for (int rr : r.waypointRanks) {
                const double t = double(rr - r.rankFrom) / double(r.rankTo - r.rankFrom);
                Slot         s;
                s.routeIndex = ri;
                s.w          = 0.0;
                // Half a row: enough for a line to pass with clearance either side, without pushing
                // the column apart as far as another box would.
                s.h          = opt.nodeH * 0.5;
                const double frac = f0 + (f1 - f0) * t;
                pending.push_back({ rr, s, frac * double(std::max<size_t>(1, cols[rr].size())) - 0.5 });
            }
        }

        for (const Pending &p : pending) {
            std::vector<Slot> &col = cols[p.rank];
            const int          at  = std::clamp(int(std::lround(p.key)) + 1, 0, int(col.size()));
            col.insert(col.begin() + at, p.slot);
        }
    }

    // Positions. Every column is centred on ONE band centre, and that centre is pushed down far
    // enough that the tallest column still clears the header strip. Centring each column on the
    // header height alone puts the top of a tall column ABOVE the headings, which is how the
    // headings ended up level with the boxes they were supposed to be naming.
    {
        double maxHalf = 0.0;
        for (auto &entry : cols) {
            double total = 0.0;
            for (const Slot &s : entry.second) total += s.h + opt.gapY;
            total  -= entry.second.empty() ? 0.0 : opt.gapY;
            maxHalf = std::max(maxHalf, total / 2.0);
        }
        const double bandCentre = opt.headerH + opt.gapY + maxHalf;

        double cursor = 0.0;
        for (auto &entry : cols) {
            double wMax = opt.minW;
            for (const Slot &s : entry.second) wMax = std::max(wMax, s.w);

            double total = 0.0;
            for (const Slot &s : entry.second) total += s.h + opt.gapY;
            total -= entry.second.empty() ? 0.0 : opt.gapY;

            double y = bandCentre - total / 2.0;
            for (Slot &s : entry.second) {
                s.x = cursor + (wMax - s.w) / 2.0;   // centred in its column
                s.y = y;
                y  += s.h + opt.gapY;
            }
            cursor += wMax + opt.gapX;
        }
    }

    // ── 5. Nodes ────────────────────────────────────────────────────────────
    QHash<QString, int> nodeIndex;   // id -> index into out.nodes
    for (auto &entry : cols) {
        const int r = entry.first;
        for (const Slot &s : entry.second) {
            if (s.isWaypoint()) continue;
            const Condition *c = pack.condition(s.id);
            if (!c) continue;

            DagNode n;
            n.id    = s.id;
            n.label = c->label;
            n.rank  = r;
            // Rank 0 is no longer the focus ALONE — the focus's symmetric partners share it, and
            // typing them as the focus gave them its frame and made the long-press menu skip every
            // item, so pressing one did nothing at all.
            n.kind  = (s.id == focusId) ? DagNodeKind::Focus
                    : r == 0            ? DagNodeKind::Related
                    : r < 0             ? DagNodeKind::Cause
                                        : DagNodeKind::Effect;
            n.x = s.x; n.y = s.y; n.w = s.w; n.h = s.h;

            n.latent      = c->observability == Observability::Latent;
            n.offeredOnly = c->confirmedBy == ConfirmedBy::Asserted;
            n.reach       = confirmedByName(c->confirmedBy);
            n.reachLabel  = reachLabel(c->confirmedBy);
            n.coverage    = coverageOf(pack, s.id);
            n.expanded    = openSet.contains(s.id);
            n.groupLabel  = conditionGroupLabel(c->group);
            resolveAvailability(pack, *c, n.available, n.unavailableReason);

            nodeIndex.insert(s.id, int(out.nodes.size()));
            out.nodes.push_back(n);
        }
    }

    const DagNode focusNode = out.nodes[nodeIndex.value(focusId, 0)];
    const double  focusCentreX = focusNode.x + focusNode.w / 2.0;

    // ── 6. The detection lane ───────────────────────────────────────────────
    //
    // Below the causal band and never inside it: a measure detects a condition, and putting it in
    // the same left-to-right flow would state a causal relationship the pack does not hold.
    //
    // EVERY condition on the picture, not only the focus. The switch is called "measures" and the
    // reader asks it a question about the picture, not about one box in it — a lane that answered
    // for the focus alone left the other boxes looking like conditions nothing can see, which is a
    // claim about the pack and a false one. A cause the app cannot detect is exactly the thing this
    // view exists to make obvious, and it can only be obvious next to the ones it can.
    //
    // A measure is drawn ONCE however many of the drawn conditions it detects, and gets one line to
    // each. Two boxes for one measure would read as two ways of seeing when there is one, and the
    // shared detector — the reason two conditions are hard to tell apart — is precisely what the
    // reader is being shown.
    const size_t firstLaneNode = out.nodes.size();
    double       laneY         = 0.0;
    double       laneX0 = 0.0, laneX1 = 0.0;

    // Who each lane node detects, indexed as out.nodes[firstLaneNode + i]. Held here so the edge
    // pass below draws from what the lane was BUILT from rather than re-deriving it and risking a
    // different answer.
    std::vector<QStringList> laneDetects;

    // The band's underside, and the x range of every column. The edge pass needs both to decide how
    // a detection line reaches its condition without going through the ones stacked beneath it.
    double                    bandBottom = 0.0;
    std::map<int, std::pair<double, double>> colSpan;

    if (opt.includeMeasures) {
        // In drawn order, which is column-major left to right — so a measure's first sighting is
        // already near where it belongs, and the ordering below only has to refine that.
        QStringList              measureIds;
        QHash<QString, int>      laneIndex;
        std::vector<double>      bary;      // mean x-centre of the conditions it detects
        std::vector<int>         nearest;   // |rank| of the closest of them, for DagNode::rank
        std::vector<int>         nearestSigned;

        for (size_t i = 0; i < firstLaneNode; ++i) {
            const DagNode   &cn = out.nodes[i];
            const Condition *c  = pack.condition(cn.id);
            if (!c) continue;
            const double cx = cn.x + cn.w / 2.0;
            for (const QString &sid : c->detectedBy) {
                const Signal *s = pack.signal(sid);
                if (!s) continue;
                for (const QString &mid : s->measures) {
                    if (!pack.measure(mid)) continue;
                    auto it = laneIndex.constFind(mid);
                    if (it == laneIndex.constEnd()) {
                        laneIndex.insert(mid, int(measureIds.size()));
                        measureIds << mid;
                        laneDetects.push_back({ cn.id });
                        bary.push_back(cx);
                        nearest.push_back(std::abs(cn.rank));
                        nearestSigned.push_back(cn.rank);
                        continue;
                    }
                    const int k = it.value();
                    if (laneDetects[size_t(k)].contains(cn.id)) continue;
                    // Running mean, so a measure shared across the picture settles between the
                    // boxes it serves rather than under whichever it happened to be found from.
                    const int n = laneDetects[size_t(k)].size();
                    bary[size_t(k)] = (bary[size_t(k)] * n + cx) / double(n + 1);
                    laneDetects[size_t(k)] << cn.id;
                    if (std::abs(cn.rank) < nearest[size_t(k)]) {
                        nearest[size_t(k)]       = std::abs(cn.rank);
                        nearestSigned[size_t(k)] = cn.rank;
                    }
                }
            }
        }

        if (!measureIds.isEmpty()) {
            // Ordered by where the boxes they answer for actually are, then packed. Placing each
            // one AT its barycentre would overlap the moment two conditions share a column; packing
            // a barycentre-sorted row keeps the left-to-right correspondence without it.
            std::vector<int> order(measureIds.size());
            for (int i = 0; i < int(order.size()); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(),
                             [&](int a, int b) { return bary[size_t(a)] < bary[size_t(b)]; });

            double bandX0 = 0.0, bandX1 = 0.0;
            bool   firstSlot  = true;
            for (auto &entry : cols)
                for (const Slot &s : entry.second) {
                    bandBottom = std::max(bandBottom, s.y + s.h);
                    if (s.isWaypoint()) continue;
                    auto it = colSpan.find(entry.first);
                    if (it == colSpan.end()) colSpan.insert({ entry.first, { s.x, s.x + s.w } });
                    else {
                        it->second.first  = std::min(it->second.first, s.x);
                        it->second.second = std::max(it->second.second, s.x + s.w);
                    }
                    bandX0    = firstSlot ? s.x : std::min(bandX0, s.x);
                    bandX1    = firstSlot ? s.x + s.w : std::max(bandX1, s.x + s.w);
                    firstSlot = false;
                }
            laneY            = bandBottom + opt.laneGap;
            const double gap = opt.gapX * 0.5;

            double rowW = 0.0;
            for (int i : order)
                rowW += nodeWidth(measureLabelOf(*pack.measure(measureIds[i])), opt) + gap;
            rowW -= gap;

            // Centred on the BAND, not on the focus. The lane now answers for the whole picture, and
            // a row of a dozen measures centred on one box would sit off to one side of the thing it
            // is describing.
            double x = (firstSlot ? focusCentreX : (bandX0 + bandX1) / 2.0) - rowW / 2.0;
            laneX0   = x;
            laneX1   = x + rowW;

            std::vector<QStringList> orderedDetects;
            orderedDetects.reserve(order.size());
            for (int i : order) {
                const Measure *m = pack.measure(measureIds[i]);

                DagNode n;
                n.id          = measureIds[i];
                n.kind        = DagNodeKind::Measure;
                n.label       = measureLabelOf(*m);
                // A measure carries the rank of the nearest condition it detects — it is not in the
                // ranking, and this is the only sense in which it has a place in it.
                n.rank        = nearestSigned[size_t(i)];
                n.w           = nodeWidth(n.label, opt);
                n.h           = opt.nodeH;
                n.x           = x;
                n.y           = laneY;
                n.available   = m->status == MeasureStatus::Live;
                n.statusLabel = measureStatusLabel(m->status);
                n.metricKey   = m->metricKey;
                if (!n.available) n.unavailableReason = m->gapReason;
                x += n.w + gap;

                nodeIndex.insert(n.id, int(out.nodes.size()));
                out.nodes.push_back(n);
                orderedDetects.push_back(laneDetects[size_t(i)]);
            }
            laneDetects = orderedDetects;
        } else {
            laneDetects.clear();
        }
    }

    // ── 7. Headings ─────────────────────────────────────────────────────────
    // Left-to-right means "causes", and a reader should not have to infer that from arrowheads.
    {
        double causeX0 = 0, causeX1 = 0, effectX0 = 0, effectX1 = 0;
        bool   anyCause = false, anyEffect = false;
        for (auto &entry : cols) {
            if (entry.second.empty()) continue;
            double x0 = entry.second.front().x, x1 = x0;
            for (const Slot &s : entry.second) {
                x0 = std::min(x0, s.x);
                x1 = std::max(x1, s.x + s.w);
            }
            if (entry.first < 0) {
                causeX0  = anyCause ? std::min(causeX0, x0) : x0;
                causeX1  = anyCause ? std::max(causeX1, x1) : x1;
                anyCause = true;
            } else if (entry.first > 0) {
                effectX0  = anyEffect ? std::min(effectX0, x0) : x0;
                effectX1  = anyEffect ? std::max(effectX1, x1) : x1;
                anyEffect = true;
            }
        }
        if (anyCause)
            out.headings.push_back({ QStringLiteral("Caused by"), causeX0, 0.0, causeX1 - causeX0 });
        if (anyEffect)
            out.headings.push_back({ QStringLiteral("Leads to"), effectX0, 0.0, effectX1 - effectX0 });
        if (out.nodes.size() > firstLaneNode)
            out.headings.push_back({ QStringLiteral("Measured by"), laneX0,
                                     laneY - opt.headerH, laneX1 - laneX0 });
    }

    // ── 8. Normalise to the origin ──────────────────────────────────────────
    {
        double minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool   first = true;
        auto   grow  = [&](double x0, double y0, double x1, double y1) {
            if (first) { minX = x0; minY = y0; maxX = x1; maxY = y1; first = false; return; }
            minX = std::min(minX, x0); minY = std::min(minY, y0);
            maxX = std::max(maxX, x1); maxY = std::max(maxY, y1);
        };
        for (const DagNode &n : out.nodes)    grow(n.x, n.y, n.x + n.w, n.y + n.h);
        for (const DagHeading &h : out.headings) grow(h.x, h.y, h.x + h.w, h.y + opt.headerH);

        for (DagNode &n : out.nodes)          { n.x -= minX; n.y -= minY; }
        for (DagHeading &h : out.headings)    { h.x -= minX; h.y -= minY; }
        for (auto &entry : cols)
            for (Slot &s : entry.second)      { s.x -= minX; s.y -= minY; }
        laneY -= minY;

        out.width  = maxX - minX;
        out.height = maxY - minY;

        const DagNode &f = out.nodes[nodeIndex.value(focusId, 0)];
        out.focusX = f.x + f.w / 2.0;
        out.focusY = f.y + f.h / 2.0;
    }

    // ── 9. What the bound cut off ───────────────────────────────────────────
    // Counted per node, not summed globally, because "there are more" is only useful where it is —
    // a reader needs to know WHICH box has something behind it before they tap it.
    for (DagNode &n : out.nodes) {
        if (n.kind == DagNodeKind::Measure) continue;
        for (const QString &c : causesOf(pack, n.id))
            if (!nodeIndex.contains(c)) ++n.hiddenCauses;
        for (const QString &e : effectsOf(pack, n.id))
            if (!nodeIndex.contains(e)) ++n.hiddenEffects;
        if (n.hiddenCauses > 0 || n.hiddenEffects > 0) out.truncated = true;
    }

    // ── 10. Edge routing ────────────────────────────────────────────────────
    //
    // Every hop of every route is resolved to two points before anything is drawn. The two rules
    // that keep the picture readable both live here:
    //
    //   FAN. Each arrow gets its OWN point on a node's edge, ordered by where it is going, so two
    //   lines entering one box stay two lines instead of merging into a single stroke at the last
    //   twenty pixels. All of them converging on the exact vertical centre is what made three
    //   arrows into the focus look like one thick smudge.
    //
    //   GUTTER. An edge between two nodes of the same rank bulges into the empty column gap beside
    //   them, never across the focus. Same-rank causal links are real — one cause causing another —
    //   and drawing them through the middle put a line over the one box the reader is looking at.

    // Resolve every hop as (slot, rank) pairs so anchors can be assigned before geometry.
    struct Hop { int routeIndex; int fromRank; int toRank; const Slot *a; const Slot *b; };

    auto slotAt = [&](int rank, const QString &id) -> const Slot * {
        for (const Slot &s : cols[rank])
            if (s.id == id) return &s;
        return nullptr;
    };
    auto waypointAt = [&](int rank, int routeIndex) -> const Slot * {
        for (const Slot &s : cols[rank])
            if (s.isWaypoint() && s.routeIndex == routeIndex) return &s;
        return nullptr;
    };

    std::vector<std::vector<Hop>> hopsPerRoute(routes.size());
    for (int ri = 0; ri < int(routes.size()); ++ri) {
        const Route &r = routes[size_t(ri)];
        const Slot  *a = slotAt(r.rankFrom, r.from);
        const Slot  *b = slotAt(r.rankTo, r.to);
        if (!a || !b) continue;

        std::vector<const Slot *> chain{ a };
        std::vector<int>          chainRanks{ r.rankFrom };
        for (int rr : r.waypointRanks)
            if (const Slot *w = waypointAt(rr, ri)) { chain.push_back(w); chainRanks.push_back(rr); }
        chain.push_back(b);
        chainRanks.push_back(r.rankTo);

        for (size_t i = 0; i + 1 < chain.size(); ++i)
            hopsPerRoute[size_t(ri)].push_back(
                { ri, chainRanks[i], chainRanks[i + 1], chain[i], chain[i + 1] });
    }

    // Fan: gather every departure from and arrival at each real node, order them by where the line
    // is heading, and spread the anchors over the middle half of that edge of the box.
    QHash<QString, std::vector<std::pair<double, std::pair<int, int>>>> outgoing, incoming;
    for (int ri = 0; ri < int(routes.size()); ++ri) {
        const auto &hops = hopsPerRoute[size_t(ri)];
        if (hops.empty()) continue;
        if (!hops.front().a->isWaypoint())
            outgoing[hops.front().a->id].push_back({ hops.front().b->centreY(), { ri, 0 } });
        if (!hops.back().b->isWaypoint())
            incoming[hops.back().b->id].push_back(
                { hops.back().a->centreY(), { ri, int(hops.size()) - 1 } });
    }

    QHash<QString, double> anchorOut, anchorIn;   // keyed "<routeIndex>|1" / "<routeIndex>|0"
    QHash<int, double>     tipScale;              // route -> arrowhead size, by how tight the fan is
    auto assign = [&](QHash<QString, std::vector<std::pair<double, std::pair<int, int>>>> &table,
                      QHash<QString, double> &store, bool isOut) {
        // The middle two thirds of the edge of the box. Wider than a token spread, because five
        // causes into one condition is ordinary in this pack and they have to arrive as five
        // distinguishable lines.
        constexpr double kSpan = 0.68;
        for (auto it = table.begin(); it != table.end(); ++it) {
            auto &list = it.value();
            std::stable_sort(list.begin(), list.end(),
                             [](const auto &l, const auto &r) { return l.first < r.first; });
            const Slot *s = nullptr;
            for (auto &entry : cols) {
                for (const Slot &c : entry.second)
                    if (c.id == it.key()) { s = &c; break; }
                if (s) break;
            }
            if (!s) continue;
            const int    n       = int(list.size());
            const double spacing = n <= 1 ? s->h : (s->h * kSpan / double(n));
            for (int k = 0; k < n; ++k) {
                const double t = n == 1 ? 0.5
                                        : ((1.0 - kSpan) / 2.0
                                           + kSpan * (double(k) + 0.5) / double(n));
                const int route = list[size_t(k)].second.first;
                store.insert(QStringLiteral("%1|%2").arg(route).arg(isOut ? 1 : 0),
                             s->y + s->h * t);
                // An arrowhead wider than the gap to its neighbour merges with it, and five arrows
                // into one box become a solid comb that says nothing. Size the head to the room it
                // actually has — small and separate beats large and indistinguishable.
                if (!isOut)
                    tipScale.insert(route, std::clamp(spacing / 12.0, 0.45, 1.0));
            }
        }
    };
    assign(outgoing, anchorOut, true);
    assign(incoming, anchorIn, false);

    for (int ri = 0; ri < int(routes.size()); ++ri) {
        const Route &r    = routes[size_t(ri)];
        const auto  &hops = hopsPerRoute[size_t(ri)];
        if (hops.empty()) continue;

        const Edge &src = pack.edges[size_t(r.edgeIndex)];
        const auto  fi  = nodeIndex.constFind(r.from);
        const bool  offeredOnly =
            fi != nodeIndex.constEnd() && out.nodes[fi.value()].offeredOnly;

        // Which segment carries the strength word: the one with the most horizontal room, so the
        // label never has to sit on top of a box.
        int    labelSeg  = 0;
        double labelSpan = -1;
        for (size_t i = 0; i < hops.size(); ++i) {
            const double span = std::abs(hops[i].b->centreX() - hops[i].a->centreX());
            if (span > labelSpan) { labelSpan = span; labelSeg = int(i); }
        }

        for (size_t i = 0; i < hops.size(); ++i) {
            const Hop  &h    = hops[i];
            const bool  last = (i + 1 == hops.size());

            DagEdge e;
            e.from          = r.from;
            e.to            = r.to;
            e.strength      = strengthName(src.strength);
            e.strengthLabel = strengthLabel(src.strength);
            e.weight        = src.strength == Strength::Weak ? 1
                            : (src.strength == Strength::Strong ? 3 : 2);
            e.offeredOnly   = offeredOnly;
            e.relation      = edgeTypeName(src.type);
            e.symmetric     = (src.type != EdgeType::Causes);
            e.segment       = int(i);
            e.segments      = int(hops.size());

            // Strength is a claim about how OFTEN a cause produces an effect. An exclusion is not a
            // matter of degree — the pair is incompatible or it is not — so it carries no word and
            // no weight, and saying "usually" on one would invent a certainty nobody authored.
            if (src.type == EdgeType::Excludes) {
                e.strengthLabel.clear();
                e.weight = 1;
            }

            const double ay = h.a->isWaypoint()
                                  ? h.a->centreY()
                                  : anchorOut.value(QStringLiteral("%1|1").arg(ri), h.a->centreY());
            const double by = h.b->isWaypoint()
                                  ? h.b->centreY()
                                  : anchorIn.value(QStringLiteral("%1|0").arg(ri), h.b->centreY());

            if (h.fromRank == h.toRank) {
                // Same rank: out into the gutter beside the column, never across the middle. Which
                // side depends on which half of the picture it is in, so the bulge always lands in
                // empty space rather than over the next column of boxes.
                const bool   leftSide = h.fromRank < 0;
                const double bulge    = opt.gapX * 0.42;
                e.x1  = leftSide ? h.a->x : h.a->x + h.a->w;   e.y1 = ay;
                e.x2  = leftSide ? h.b->x : h.b->x + h.b->w;   e.y2 = by;
                e.c1x = e.x1 + (leftSide ? -bulge : bulge);    e.c1y = e.y1;
                e.c2x = e.x2 + (leftSide ? -bulge : bulge);    e.c2y = e.y2;
            } else {
                const bool forward = h.toRank > h.fromRank;
                e.x1 = h.a->isWaypoint() ? h.a->centreX()
                                         : (forward ? h.a->x + h.a->w : h.a->x);
                e.y1 = ay;
                e.x2 = h.b->isWaypoint() ? h.b->centreX()
                                         : (forward ? h.b->x : h.b->x + h.b->w);
                e.y2 = by;
                const double dx = (e.x2 - e.x1) * 0.5;
                e.c1x = e.x1 + dx; e.c1y = e.y1;
                e.c2x = e.x2 - dx; e.c2y = e.y2;
            }

            if (int(i) == labelSeg && !e.strengthLabel.isEmpty()) {
                const QPointF m = cubicAt(QPointF(e.x1, e.y1), QPointF(e.c1x, e.c1y),
                                          QPointF(e.c2x, e.c2y), QPointF(e.x2, e.y2), 0.5);
                e.label  = e.strengthLabel;
                e.labelX = m.x();
                e.labelY = m.y();
            }

            // The arrowhead. Which end is the effect is the whole claim of a causal graph, so the
            // triangle is emitted as three points rather than an angle for a delegate to rotate by.
            //
            // A SYMMETRIC relation gets none. Corroborates and Excludes read the same from either
            // end — the author may write them whichever way round they think of them — so an arrow
            // would assert a direction that does not exist, and a reader would take it as causal.
            if (last && !e.symmetric) {
                const QPointF tipPt(e.x2, e.y2);
                const QPointF back = cubicAt(QPointF(e.x1, e.y1), QPointF(e.c1x, e.c1y),
                                             QPointF(e.c2x, e.c2y), tipPt, 0.94);
                double        dx = tipPt.x() - back.x(), dy = tipPt.y() - back.y();
                const double  len = std::hypot(dx, dy);
                if (len > 0.001) {
                    dx /= len; dy /= len;
                    const double sc = tipScale.value(ri, 1.0);
                    const double L = 10.0 * sc, W = 4.5 * sc;
                    e.tip  = true;
                    e.tipAx = tipPt.x();              e.tipAy = tipPt.y();
                    e.tipBx = tipPt.x() - dx * L - dy * W;
                    e.tipBy = tipPt.y() - dy * L + dx * W;
                    e.tipCx = tipPt.x() - dx * L + dy * W;
                    e.tipCy = tipPt.y() - dy * L - dx * W;
                }
            }

            out.edges.push_back(e);
        }
    }

    // Detection edges run UP from the lane into the condition they detect, so they read as "this is
    // how that is seen" rather than as another arrow in the causal flow. No arrowhead and no
    // strength: neither would be true of a measure.
    //
    // Both ends FAN, for the reason the causal edges do. A condition with three detectors and a
    // measure serving three conditions are both ordinary here, and lines that all met at one point
    // would be a smudge at exactly the box the reader is counting them on.
    {
        // Arrivals per condition, so each line lands on its own point along that box's underside.
        // Counted first because the spread depends on the total, which is not known until every
        // lane node has been walked.
        QHash<QString, int> arrivals;
        for (size_t i = firstLaneNode; i < out.nodes.size(); ++i)
            for (const QString &cid : laneDetects[i - firstLaneNode]) ++arrivals[cid];

        QHash<QString, int> arrived;
        for (size_t i = firstLaneNode; i < out.nodes.size(); ++i) {
            const DagNode    &n  = out.nodes[i];
            const QStringList &to = laneDetects[i - firstLaneNode];

            int departed = 0;
            for (const QString &cid : to) {
                const auto ci = nodeIndex.constFind(cid);
                if (ci == nodeIndex.constEnd()) continue;
                const DagNode &f = out.nodes[ci.value()];

                // The middle 40% of each edge, which is where a line can leave or arrive without
                // reaching the corners a neighbouring box's line is heading for.
                auto spread = [](int k, int n) {
                    return n <= 1 ? 0.5 : (0.3 + 0.4 * (double(k) + 0.5) / double(n));
                };
                const int  na = arrivals.value(cid, 1);
                const int  ka = arrived[cid]++;
                const double td = spread(departed++, to.size());
                const double ta = spread(ka, na);

                const double mx = n.x + n.w * td;

                // Which way in. Straight up into the underside is the calm route and the right one
                // whenever the box is the lowest thing in the band — the line then travels sideways
                // only in the empty strip between the band and the lane, and can meet nothing.
                //
                // Any HIGHER box has its own column stacked beneath it, and a line to its underside
                // would be drawn through every one of them. That one goes up the GUTTER beside the
                // column instead and comes in from the side: the gutter is the gap between columns
                // and holds no boxes at any height, so the route is clear by construction rather
                // than by luck. Same reasoning as the same-rank causal bulge, one lane down.
                const bool lowestInBand = (f.y + f.h) >= bandBottom - 0.5;

                DagEdge e;
                e.from    = n.id;
                e.to      = cid;
                e.detects = true;
                e.weight  = 1;

                if (lowestInBand) {
                    e.x1 = mx;              e.y1 = n.y;
                    e.x2 = f.x + f.w * ta;  e.y2 = f.y + f.h;
                    // Vertical tangents at both ends, so it reads as rising out of the lane rather
                    // than as another line in the flow.
                    const double dy = (e.y1 - e.y2) * 0.5;
                    e.c1x = e.x1; e.c1y = e.y1 - dy;
                    e.c2x = e.x2; e.c2y = e.y2 + dy;
                    out.edges.push_back(e);
                    continue;
                }

                const auto span = colSpan.find(f.rank);
                const double cx0 = span == colSpan.end() ? f.x : span->second.first;
                const double cx1 = span == colSpan.end() ? f.x + f.w : span->second.second;
                // The gutter nearest the measure, unless that is the outer edge of the picture —
                // the outermost column has a neighbour on one side only, and the other side is
                // empty canvas the bounding box does not cover.
                const bool haveLeft  = colSpan.count(f.rank - 1) > 0;
                const bool haveRight = colSpan.count(f.rank + 1) > 0;
                bool       useLeft   = mx < (cx0 + cx1) / 2.0;
                if (useLeft && !haveLeft)  useLeft = !haveRight;
                if (!useLeft && !haveRight) useLeft = haveLeft;
                const double gx = useLeft ? cx0 - opt.gapX * 0.5 : cx1 + opt.gapX * 0.5;

                // Where the riser leaves the strip: high enough to be clear of the lane row, below
                // the band so the sideways half of the trip meets nothing.
                const double riserY = bandBottom + opt.laneGap * 0.45;
                const double by     = f.y + f.h * ta;

                // Two segments meeting at the riser, both with VERTICAL tangents at the join, so the
                // corner reads as one continuous line rather than two.
                DagEdge a = e, b = e;
                a.segment = 0; a.segments = 2;
                b.segment = 1; b.segments = 2;

                a.x1 = mx;  a.y1 = n.y;
                a.x2 = gx;  a.y2 = riserY;
                a.c1x = a.x1; a.c1y = a.y1 - (a.y1 - a.y2) * 0.5;
                a.c2x = a.x2; a.c2y = a.y2 + (a.y1 - a.y2) * 0.5;

                b.x1 = gx;                          b.y1 = riserY;
                b.x2 = useLeft ? f.x : f.x + f.w;   b.y2 = by;
                b.c1x = b.x1; b.c1y = b.y1 - (b.y1 - b.y2) * 0.55;
                // Horizontal into the side of the box, which is what makes it read as arriving
                // rather than as passing through.
                b.c2x = b.x2 + (useLeft ? -opt.gapX * 0.25 : opt.gapX * 0.25);
                b.c2y = b.y2;

                out.edges.push_back(a);
                out.edges.push_back(b);
            }
        }
    }

    return out;
}

} // namespace pinpoint::analysis
