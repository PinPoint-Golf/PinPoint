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

#include "reference_pack.h"

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

// The same estimate at an inner row's own text size, and against its own cap. A row is INSET from
// the box edge and carries a second word to the right of its label, so both are charged for here —
// a box sized for the label alone puts that word into the ellipsis, and on a measure row that word
// is the half that says the app cannot see this yet.
double rowWidth(const QString &label, const QString &detail, const DagLayoutOptions &opt)
{
    int chars = label.size();
    if (!detail.isEmpty()) chars += detail.size() + 1;
    const double raw = opt.padX * 2.0 + opt.rowCharW * double(chars);
    return std::clamp(raw, opt.minW, std::max(opt.maxW, opt.rowMaxW));
}

// What a measure row actually charges for: its status word appears only when the measure is a gap,
// so a live one is sized on its label alone.
double measureRowWidth(const DagMeasure &dm, const DagLayoutOptions &opt)
{
    return rowWidth(dm.label, dm.available ? QString() : dm.statusLabel, opt);
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

    // One admission gate for every route into the picture — the automatic walk, the focus's
    // symmetric partners and the opened boxes all ask this, so a filter cannot apply to some of
    // them and quietly not to others. The focus itself never comes through here: it is seated
    // before the walk starts.
    auto admissible = [&](const QString &id) {
        const Condition *c = pack.condition(id);
        if (!c) return false;
        if (!opt.includeScreened && c->confirmedBy == ConfirmedBy::Screened) return false;
        return true;
    };

    // ── 0. The order the neighbours arrive in ───────────────────────────────
    //
    // RANKED, not pack order. Pack order is the order an author happened to append the edges in,
    // and it silently decided two things it has no business deciding: which nodes survive a budget,
    // and — at rank ±1, where every barycentre ties and the stable sort below therefore keeps what
    // it was given — the top-to-bottom order of the column. The newest claim was always the first
    // to fall off, whatever it said: `hips_under_rotated_at_top -> over_the_top` is `strong` and
    // was written last, so it lost its seat to the eight edges authored before it, six of which are
    // only `moderate`.
    //
    // The ranking is the one ADDENDUM-03 established for the Causes and Characteristics lists, so
    // the picture and the tables agree about which cause matters. Causes rank by WHAT THEY REACH
    // (bad shots explained, then total downstream reach); effects rank by WHAT REACHES THEM, which
    // is the only measure an endpoint can score on at all — `slice` causes nothing and would sit
    // last on any out-edge key.
    //
    // Counts, never a strength-weighted score. Same refusal this repo has already made twice, for
    // the same reason: Strength is three-valued because nobody can author 0.73 meaningfully, and a
    // count can always answer "why is this above that" by naming its members.
    struct Reach {
        int outcomes = 0, leadsTo = 0, causedBy = 0;
    };
    // Memoised for the reason the browser memoises the same three numbers: causalClosure() re-scans
    // the whole edge list per frontier node by design, and this walk asks about the same node once
    // per parent that reaches it. Returned BY VALUE — a reference into the hash would dangle the
    // moment the comparator's second lookup rehashed it.
    QHash<QString, Reach> reachMemo;
    auto reachOf = [&](const QString &id) -> Reach {
        const auto it = reachMemo.constFind(id);
        if (it != reachMemo.constEnd()) return it.value();
        Reach r;
        r.leadsTo  = int(causalClosure(pack, id, /*downstream*/ true).size());
        r.outcomes = outcomeReachOf(pack, id);
        r.causedBy = int(causalClosure(pack, id, /*downstream*/ false).size());
        reachMemo.insert(id, r);
        return r;
    };
    // Stable, so pack order remains the tie-break and the picture stays deterministic between
    // visits — which is the one property a reader has to be able to rely on here.
    auto rankedNeighbours = [&](const QString &id, int dir) {
        QStringList nb = dir < 0 ? causesOf(pack, id) : effectsOf(pack, id);
        std::stable_sort(nb.begin(), nb.end(), [&](const QString &a, const QString &b) {
            const Reach ra = reachOf(a);
            const Reach rb = reachOf(b);
            if (dir > 0) return ra.causedBy > rb.causedBy;
            if (ra.outcomes != rb.outcomes) return ra.outcomes > rb.outcomes;
            return ra.leadsTo > rb.leadsTo;
        });
        return nb;
    };

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
                const QStringList nb = rankedNeighbours(id, dir);
                for (const QString &n : nb) {
                    if (rankOf.contains(n)) continue;
                    if (!admissible(n)) continue;
                    // OFF by default — see the note on maxPerRank in the header. When a caller does
                    // set one, the node past it is simply not admitted. It is not lost: it has no
                    // rank, so it lands in its parent's hidden count below, which is where the view
                    // says how much is off screen. What falls off is now the lowest-ranked rather
                    // than the last-authored.
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
            if (other.isEmpty() || rankOf.contains(other) || !admissible(other)) continue;
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
                    // Ranked here too. maxPerExpand is a real budget and still bites, so what it
                    // drops must be the least of what was asked for rather than the most recent.
                    const QStringList nb = rankedNeighbours(id, dir);
                    for (const QString &n : nb) {
                        if (rankOf.contains(n)) continue;
                        if (!admissible(n)) continue;
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
    // in admission order — the one place the reader has just said they are looking.
    //
    // The sort is STABLE and the admission order is ranked, so the two compose rather than compete:
    // crossing minimisation decides the column wherever it has an opinion, and the reach ranking
    // decides it wherever the barycentres tie. At rank ±1 they tie in the ordinary case — the focus
    // is the only thing on rank 0 for every one of them to point at — so that column reads
    // top-to-bottom in exactly the order the Causes list would put it in. A focus carrying
    // corroborating partners puts other boxes on rank 0 and the barycentres separate again, which
    // is correct: those lines are on the picture and should not be made to cross for this.
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
                // position rather than inventing one, so the ranked admission order survives.
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

    // ── 3b. What detects each drawn condition ───────────────────────────────
    //
    // Resolved here rather than at node-emit time because the ROWS DECIDE THE BOX HEIGHT, and the
    // column stacking below reads that height to place everything under it. Working it out later
    // would mean laying the picture out twice.
    QHash<QString, std::vector<DagMeasure>> measuresFor;
    if (opt.includeMeasures) {
        for (const auto &entry : byRank) {
            for (const QString &cid : entry.second) {
                const Condition *c = pack.condition(cid);
                if (!c) continue;
                std::vector<DagMeasure> rows;
                QSet<QString>           seen;
                for (const QString &sid : c->detectedBy) {
                    const Signal *sg = pack.signal(sid);
                    if (!sg) continue;
                    for (const QString &mid : sg->measures) {
                        const Measure *m = pack.measure(mid);
                        if (!m || seen.contains(mid)) continue;
                        seen.insert(mid);
                        DagMeasure dm;
                        dm.id          = mid;
                        dm.label       = measureLabelOf(*m);
                        dm.statusLabel = measureStatusLabel(m->status);
                        dm.metricKey   = m->metricKey;
                        dm.available   = m->status == MeasureStatus::Live;
                        if (!dm.available) dm.unavailableReason = m->gapReason;
                        dm.h           = opt.rowH;
                        rows.push_back(dm);
                    }
                }
                if (!rows.empty()) measuresFor.insert(cid, rows);
            }
        }
    }

    // ── 3c. What each drawn condition is cited from ─────────────────────────
    //
    // Gathered here for the same reason the measures are: these rows add to the box height, and the
    // columns are stacked from that height.
    //
    // No registry means no rows at all, rather than a box full of dangling citations. The layout has
    // no bibliography of its own to fall back on and must not invent the claim that the pack's
    // sources are missing when it is the caller that did not supply them.
    QHash<QString, std::vector<DagReference>> referencesFor;
    if (opt.includeReferences && opt.references) {
        for (const auto &entry : byRank) {
            for (const QString &cid : entry.second) {
                const Condition *c = pack.condition(cid);
                if (!c || c->provenance.citation.isEmpty()) continue;

                const Reference *ref = opt.references->byCitation(c->provenance.citation);
                DagReference     dr;
                dr.citation = c->provenance.citation;
                dr.resolved = ref != nullptr;
                dr.h        = opt.rowH;
                if (ref) {
                    dr.id    = ref->id;
                    dr.label = ref->title.isEmpty() ? ref->identifierLabel() : ref->title;
                    // The YEAR beside the title, not the identifier. A DOI is what the join is made
                    // on and it is unreadable — reference_pack.h says so in its opening lines — while
                    // how current a source is is a judgement the reader can actually make from four
                    // characters, and it survives the title being elided.
                    dr.detailLabel = ref->year > 0 ? QString::number(ref->year) : QString();
                } else {
                    // The citation as a reader should SEE it, which is where the "PMID " and "ISBN "
                    // prefixes come from — a bare eight digits could be a year or a count.
                    dr.label       = citationLabel(c->provenance.citation, *opt.references);
                    dr.detailLabel = QStringLiteral("not in the bibliography");
                }
                referencesFor.insert(cid, { dr });
            }
        }
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
            // A measure label — and a paper's title even more so — is routinely longer than the
            // condition's, so the box takes the widest of what it has to hold, clamped by maxW like
            // everything else with the view eliding past that. A row that has to be elided is still
            // a row the reader can see is there.
            const auto mit = measuresFor.constFind(id);
            if (mit != measuresFor.constEnd()) {
                for (const DagMeasure &dm : mit.value())
                    s.w = std::max(s.w, measureRowWidth(dm, opt));
                s.h += double(mit.value().size()) * opt.rowH;
            }
            const auto rit = referencesFor.constFind(id);
            if (rit != referencesFor.constEnd()) {
                for (const DagReference &dr : rit.value())
                    s.w = std::max(s.w, rowWidth(dr.label, dr.detailLabel, opt));
                s.h += double(rit.value().size()) * opt.rowH;
            }
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
            // Absolute, stacked under the condition's own row — measures first, then the citation
            // beneath them. The view draws from these and so does the hit test; neither adds an
            // offset of its own, and ONE cursor runs through both kinds so the two stacks cannot
            // disagree about where the second one starts.
            double rowY = s.y + opt.nodeH;
            const auto mit = measuresFor.constFind(s.id);
            if (mit != measuresFor.constEnd()) {
                n.measures = mit.value();
                for (DagMeasure &dm : n.measures) { dm.y = rowY; rowY += dm.h; }
            }
            const auto rit = referencesFor.constFind(s.id);
            if (rit != referencesFor.constEnd()) {
                n.references = rit.value();
                for (DagReference &dr : n.references) { dr.y = rowY; rowY += dr.h; }
            }
            n.groupLabel  = conditionGroupLabel(c->group);
            resolveAvailability(pack, *c, n.available, n.unavailableReason);

            nodeIndex.insert(s.id, int(out.nodes.size()));
            out.nodes.push_back(n);
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
        for (DagNode &n : out.nodes) {
            for (DagMeasure &dm : n.measures)   dm.y -= minY;
            for (DagReference &dr : n.references) dr.y -= minY;
        }

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

    return out;
}

} // namespace pinpoint::analysis
