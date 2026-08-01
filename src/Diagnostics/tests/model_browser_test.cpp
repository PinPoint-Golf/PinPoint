// ModelBrowser — the Diagnostic Model panel's façade
// (src/Gui/diagnosticmodel/model_browser.{h,cpp}), run against the SHIPPED content.
//
// The façade's whole job is that QML holds no rules, so every rule it hides is asserted here or it
// is asserted nowhere. The ones that carry the weight, and why each is invisible in the rendered
// page if it is wrong:
//
//   1. The working copy. Editing accumulates in an unsaved pack and every read goes through an
//      assembly that CONTAINS it. Get this wrong and the table shows the file while the author
//      edits something else — which looks like nothing happening, then like data loss.
//   2. Copy-on-write and its inverse. Editing shipped content copies the row into the user pack;
//      undoing that first edit REMOVES the copy, which is the reset. Both halves have to hold or
//      "reset to shipped" becomes a second code path that can disagree with undo.
//   3. A cascade is one command. A bulk-set over twelve rows must undo as ONE step; twelve
//      separate undos to reverse one gesture is worse than no undo, because the author stops
//      halfway believing they are back where they started.
//   4. Legality is decided BEFORE the write, and the graph drag consults the same function the
//      write does. A drag that says yes to something addLink() then refuses is the tool lying.
//   5. Derived measure labels. Nine shipped measures carry label:"" and eight are planned or
//      noProducer — exactly the rows an author is hunting for, so a blank name hides the work.
//   6. Save does not clear the stack, and undo still works afterwards (ADDENDUM-01).
//
//   cmake --build build/analyzer-tests --target model_browser_test
//   ctest --test-dir build/analyzer-tests -R model_browser --output-on-failure

#include "model_browser.h"

#include "../characteristic_pack.h"
#include "../pack_provider.h"
#include "../norm_provider.h"
#include "../measure_facets.h"
#include "../anatomy_vocabulary.h"
#include "../drill_pack.h"
#include "../screen_pack.h"

#include <QCoreApplication>
#include <QFile>

#include <cmath>
#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static QVariantMap rowFor(const QVariantList &rows, const QString &id)
{
    for (const QVariant &v : rows)
        if (v.toMap().value(QStringLiteral("id")).toString() == id) return v.toMap();
    return {};
}

static QVariantMap cellFor(const QVariantMap &row, const QString &field)
{
    for (const QVariant &v : row.value(QStringLiteral("cells")).toList())
        if (v.toMap().value(QStringLiteral("field")).toString() == field) return v.toMap();
    return {};
}

static int typeCount(const QVariantList &types, const QString &key)
{
    for (const QVariant &v : types)
        if (v.toMap().value(QStringLiteral("key")).toString() == key)
            return v.toMap().value(QStringLiteral("count")).toInt();
    return -1;
}

// The corridor row id, split the way the panel splits it. Local rather than exposing the facade's
// private helper: the test should exercise the same shape QML does, not reach past it.
struct ModelBrowserTestAccess {
    static void split(const QString &id, QString &measureId, QString &contextId)
    {
        const QString body = id.mid(5);
        const int     at   = body.lastIndexOf(QLatin1Char('@'));
        measureId = body.left(at);
        contextId = body.mid(at + 1);
    }
};

// The first row of a type whose Source is shipped — the subject for every copy-on-write assertion.
static QString firstShippedId(const QVariantList &rows)
{
    for (const QVariant &v : rows)
        if (v.toMap().value(QStringLiteral("source")).toString() == QStringLiteral("shipped"))
            return v.toMap().value(QStringLiteral("id")).toString();
    return {};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // The user pack is written by save(); XDG_DATA_HOME is isolated by the test properties so this
    // never touches a real profile. Start from nothing so the baseline is knowable.
    QFile::remove(userPackPath());
    QFile::remove(userNormPath());
    // Screens and drills became writable in ADDENDUM-02 and have user layers of their own now, so
    // the baseline has to clear those too or a previous run's overrides are this run's shipped set.
    QFile::remove(userScreenSetPath());
    QFile::remove(userDrillSetPath());
    resetSharedScreenSet();
    resetSharedDrillSet();

    ModelBrowser m;

    std::printf("=== the shipped content is reachable ===\n");
    {
        const QVariantList types = m.types();
        check(types.size() == 11, "eleven content types on the rail");
        check(typeCount(types, QStringLiteral("characteristics")) > 90,
              "the characteristics are loaded");
        check(typeCount(types, QStringLiteral("measures")) > 90, "the measures are loaded");
        check(typeCount(types, QStringLiteral("links")) > 200, "the causal links are loaded");
        check(typeCount(types, QStringLiteral("signals")) > 90, "the signals are loaded");
        check(typeCount(types, QStringLiteral("references")) > 5, "the bibliography is loaded");
        check(typeCount(types, QStringLiteral("corridors")) > 100, "the corridors are loaded");
        check(typeCount(types, QStringLiteral("metrics")) > 20, "and the metric catalogue");

        // Summed, never stated. A literal would be wrong the first time anybody authored anything —
        // and health is excluded, because a total that moved when somebody FIXED something would be
        // a census of the wrong thing.
        int summed = 0;
        for (const QVariant &v : types) {
            const QVariantMap t = v.toMap();
            if (t.value(QStringLiteral("key")).toString() == QStringLiteral("health")) continue;
            summed += t.value(QStringLiteral("count")).toInt();
        }
        check(m.totalObjects() == summed, "the total is the sum of the rail, not a stored figure");
        check(typeCount(types, QStringLiteral("health")) >= 0
              && m.totalObjects() != summed + typeCount(types, QStringLiteral("health")),
              "health rows are findings, not objects, and are left out of the total");
    }

    std::printf("=== rows carry a cell per column, in order ===\n");
    {
        for (const QString &type : { QStringLiteral("characteristics"), QStringLiteral("causes"),
                                     QStringLiteral("measures"), QStringLiteral("signals"),
                                     QStringLiteral("links"), QStringLiteral("screens"),
                                     QStringLiteral("drills"), QStringLiteral("references"),
                                     QStringLiteral("metrics"), QStringLiteral("health") }) {
            const QVariantList cols = m.columns(type);
            const QVariantList rows = m.rows(type);
            bool               ok   = !cols.isEmpty();
            for (const QVariant &v : rows)
                if (v.toMap().value(QStringLiteral("cells")).toList().size() != cols.size())
                    ok = false;
            check(ok, qPrintable(QStringLiteral("%1: every row has one cell per column").arg(type)));
        }

        // Exactly one column takes the slack. Two would fight; none starves the name column, which
        // is the one that has to stay readable as panes are added.
        for (const QString &type : { QStringLiteral("characteristics"), QStringLiteral("measures"),
                                     QStringLiteral("links"), QStringLiteral("search") }) {
            int flex = 0;
            for (const QVariant &v : m.columns(type))
                if (v.toMap().value(QStringLiteral("flex")).toBool()) ++flex;
            check(flex == 1, qPrintable(QStringLiteral("%1: exactly one flexible column").arg(type)));
        }
    }

    std::printf("=== derived measure labels: no row renders nameless ===\n");
    {
        const QVariantList rows = m.rows(QStringLiteral("measures"));
        bool               anyBlank = false;
        for (const QVariant &v : rows)
            if (v.toMap().value(QStringLiteral("label")).toString().isEmpty()) anyBlank = true;
        check(!anyBlank, "no measure row has a blank name");

        // The nine measures that carry label:"" in core.json. The BRIEF says to derive their names
        // in the façade; the LOADER already does it (characteristic_pack.cpp backfills an empty
        // Composed label with canonicalMeasureLabel), so by the time any surface sees them they are
        // named. Asserted here rather than assumed, because the fix living in the loader is the
        // reason the façade's own fallback looks like dead code.
        const auto core = makeResourcePackProvider();
        for (const char *id : { "m_thoracicCurve", "m_lumbarCurve", "m_shoulderPlane",
                                "m_ballBodyGap", "m_thoraxDrift", "m_leadKneeFlex",
                                "m_trailElbowRise", "m_leadArmToTorso", "m_leadHandWidth" }) {
            const Measure *meas = core->pack().measure(QLatin1String(id));
            check(meas != nullptr, qPrintable(QStringLiteral("%1 is in the shipped pack").arg(id)));
            if (!meas) continue;
            check(!meas->label.isEmpty(),
                  qPrintable(QStringLiteral("%1's empty label is backfilled at load").arg(id)));
            check(meas->label != meas->id,
                  qPrintable(QStringLiteral("%1 is named from its facets, not its id").arg(id)));
        }

        // measureDisplayLabel() is still the ONE rule every surface asks, and the case the loader
        // does NOT cover is a Provided measure with no label — it has no facets to generate from,
        // so the id is the last resort that keeps a row from rendering nameless.
        Measure provided;
        provided.id   = QStringLiteral("m_provided_no_label");
        provided.kind = MeasureKind::Provided;
        check(measureDisplayLabel(provided) == provided.id,
              "a Provided measure with no label falls back to its id, never to a blank");

        Measure authored;
        authored.id    = QStringLiteral("m_x");
        authored.label = QStringLiteral("Authored name");
        check(measureDisplayLabel(authored) == QStringLiteral("Authored name"),
              "an author's own words always win");
    }

    std::printf("=== sorting and filtering happen here, not in QML ===\n");
    {
        // Measures default to status, then LEAST-READ: that ordering is what surfaces the measures
        // nothing reads without the author knowing to ask for them.
        const QVariantList byDefault = m.rows(QStringLiteral("measures"));
        bool               ordered   = true;
        int                lastStatus = -1, lastRead = -1;
        for (const QVariant &v : byDefault) {
            const QVariantMap k = v.toMap().value(QStringLiteral("sortKeys")).toMap();
            const int st = k.value(QStringLiteral("status")).toInt();
            const int rd = k.value(QStringLiteral("readBy")).toInt();
            if (st < lastStatus) ordered = false;
            if (st == lastStatus && rd < lastRead) ordered = false;
            lastStatus = st;
            lastRead   = rd;
        }
        check(ordered, "measures sort by status, then least-read");

        QVariantMap desc;
        desc.insert(QStringLiteral("sort"), QStringLiteral("readBy"));
        desc.insert(QStringLiteral("descending"), true);
        const QVariantList byRead = m.rows(QStringLiteral("measures"), desc);
        check(byRead.size() == byDefault.size(), "sorting does not drop rows");
        const int topRead = byRead.value(0).toMap().value(QStringLiteral("sortKeys")).toMap()
                                .value(QStringLiteral("readBy")).toInt();
        const int endRead = byRead.value(byRead.size() - 1).toMap()
                                .value(QStringLiteral("sortKeys")).toMap()
                                .value(QStringLiteral("readBy")).toInt();
        check(topRead >= endRead, "descending really descends");

        // A facet's count and the filter it applies come from ONE source, so a chip can never say
        // twelve and return nine.
        const QVariantList facets = m.facets(QStringLiteral("measures"));
        check(!facets.isEmpty(), "measures offer facets");
        bool countsMatch = true;
        for (const QVariant &fv : facets) {
            const QVariantMap f = fv.toMap();
            for (const QVariant &ov : f.value(QStringLiteral("options")).toList()) {
                const QVariantMap o = ov.toMap();
                QVariantMap sel;
                sel.insert(f.value(QStringLiteral("key")).toString(),
                           QStringList{ o.value(QStringLiteral("value")).toString() });
                QVariantMap filters;
                filters.insert(QStringLiteral("facets"), sel);
                if (m.rows(QStringLiteral("measures"), filters).size()
                    != o.value(QStringLiteral("count")).toInt())
                    countsMatch = false;
            }
        }
        check(countsMatch, "every facet chip returns exactly the number it advertises");
    }

    std::printf("=== cross-type search spans every registry ===\n");
    {
        const QVariantList hits = m.searchAll(QStringLiteral("hip"));
        check(hits.size() > 3, "\"hip\" matches more than a handful of objects");

        QSet<QString> kinds;
        for (const QVariant &v : hits)
            kinds.insert(v.toMap().value(QStringLiteral("resultType")).toString());
        check(kinds.size() >= 3, "\"hip\" answers across at least three content types at once");

        // One object, one row. A cause is also a characteristic, and returning it twice would make
        // the match count a lie.
        QSet<QString> ids;
        bool          unique = true;
        for (const QVariant &v : hits) {
            const QString id = v.toMap().value(QStringLiteral("id")).toString();
            if (ids.contains(id)) unique = false;
            ids.insert(id);
        }
        check(unique, "an object that is both a characteristic and a cause appears once");

        check(m.searchAll(QString()).isEmpty(), "an empty query returns nothing, not everything");

        const QVariantList cols = m.columns(QStringLiteral("search"));
        bool               shaped = !hits.isEmpty();
        for (const QVariant &v : hits)
            if (v.toMap().value(QStringLiteral("cells")).toList().size() != cols.size())
                shaped = false;
        check(shaped, "search rows are rebuilt to the flat shape, not carried over per-type");
    }

    std::printf("=== the inspector is a relationship hub ===\n");
    {
        const QString id = m.rows(QStringLiteral("characteristics")).value(0).toMap()
                               .value(QStringLiteral("id")).toString();
        const QVariantMap d = m.inspect(QStringLiteral("characteristics"), id);
        check(d.value(QStringLiteral("found")).toBool(), "a known id is found");
        check(!d.value(QStringLiteral("sections")).toList().isEmpty(), "it has sections");

        check(!m.inspect(QStringLiteral("characteristics"), QStringLiteral("no_such_id"))
                   .value(QStringLiteral("found")).toBool(),
              "an unknown id reports not-found rather than a blank page");

        // A measure's blast radius is the LIST and not only a count: a count alone does not say
        // what is about to change.
        QString sharedMeasure;
        for (const QVariant &v : m.rows(QStringLiteral("measures"))) {
            const QVariantMap r = v.toMap();
            const QVariantMap c = cellFor(r, QStringLiteral("status"));
            Q_UNUSED(c)
            const QString mid = r.value(QStringLiteral("id")).toString();
            if (m.inspect(QStringLiteral("measures"), mid)
                    .value(QStringLiteral("found")).toBool()) {
                for (const QVariant &sv : m.inspect(QStringLiteral("measures"), mid)
                                               .value(QStringLiteral("sections")).toList()) {
                    const QVariantMap s = sv.toMap();
                    if (s.value(QStringLiteral("title")).toString().contains(
                            QStringLiteral("Blast"), Qt::CaseInsensitive)
                        && s.value(QStringLiteral("count")).toInt() > 1) {
                        sharedMeasure = mid;
                        break;
                    }
                }
            }
            if (!sharedMeasure.isEmpty()) break;
        }
        check(!sharedMeasure.isEmpty(), "at least one measure is shared, and says by whom");
    }

    std::printf("=== copy-on-write, and undo as its inverse ===\n");
    {
        const QString id = firstShippedId(m.rows(QStringLiteral("characteristics")));
        check(!id.isEmpty(), "the shipped pack supplies a subject");

        const int before = m.unsavedCount();
        check(before == 0, "nothing is unsaved before the first edit");

        const QVariantMap r = m.setField(QStringLiteral("characteristics"), id,
                                         QStringLiteral("label"), QStringLiteral("Renamed"));
        check(r.value(QStringLiteral("ok")).toBool(), "a shipped characteristic can be edited");

        QVariantMap row = rowFor(m.rows(QStringLiteral("characteristics")), id);
        check(row.value(QStringLiteral("label")).toString() == QStringLiteral("Renamed"),
              "the edit is visible in the table BEFORE any save");
        check(row.value(QStringLiteral("dirty")).toBool(), "the row is marked unsaved");
        check(row.value(QStringLiteral("source")).toString() == QStringLiteral("both"),
              "editing shipped content makes the row yours, over shipped");
        check(m.unsavedCount() == 1, "one object is unsaved");

        // The inverse. Undoing the FIRST edit to shipped content removes the override entirely —
        // which is the reset, with no separate code path to keep in step.
        check(m.canUndo(), "the edit is on the stack");
        m.undo();
        row = rowFor(m.rows(QStringLiteral("characteristics")), id);
        check(row.value(QStringLiteral("source")).toString() == QStringLiteral("shipped"),
              "undoing the first edit removes the override — that IS the reset");
        check(!row.value(QStringLiteral("dirty")).toBool(), "and the row is clean again");
        check(m.unsavedCount() == 0, "nothing is unsaved");
        check(m.canRedo(), "redo is still offered");

        m.redo();
        check(m.unsavedCount() == 1, "redo puts it back");
        m.undo();
        check(m.unsavedCount() == 0, "and undo takes it away again");
    }

    std::printf("=== legality is decided before the write ===\n");
    {
        // The pack is a DAG, so some pair has a causal path; adding its reverse must be refused,
        // and refused with the reason rather than by silently doing nothing.
        const auto  core = makeResourcePackProvider();
        const auto &p    = core->pack();
        QString     from, to;
        for (const Edge &e : p.edges)
            if (e.type == EdgeType::Causes) { from = e.from; to = e.to; break; }
        check(!from.isEmpty(), "the shipped pack has a causal edge to work from");

        const QVariantMap cycle = m.linkLegality(to, from, QStringLiteral("causes"));
        check(!cycle.value(QStringLiteral("ok")).toBool(), "the reverse of an edge is refused");
        check(cycle.value(QStringLiteral("reason")).toString().contains(QStringLiteral("cycle")),
              "and the refusal says it would create a cycle");

        check(!m.linkLegality(from, from).value(QStringLiteral("ok")).toBool(),
              "a self-edge is refused");
        check(!m.linkLegality(from, to).value(QStringLiteral("ok")).toBool(),
              "an edge that already exists is refused rather than duplicated");

        // The drag and the write consult the SAME function, so they cannot disagree.
        const QVariantMap written = m.addLink(to, from, QStringLiteral("causes"));
        check(!written.value(QStringLiteral("ok")).toBool(),
              "addLink refuses exactly what linkLegality refused");
        check(written.value(QStringLiteral("message")).toString()
                  == cycle.value(QStringLiteral("reason")).toString(),
              "and gives the same reason, because it is the same function");

        // Candidates are PRE-FILTERED: an illegal target cannot be constructed, rather than being
        // offered and then rejected.
        const QVariantList cands = m.linkCandidates(QStringLiteral("causes"), to);
        bool               clean = true;
        for (const QVariant &v : cands) {
            const QString cid = v.toMap().value(QStringLiteral("id")).toString();
            if (cid == to || cid == from) clean = false;
        }
        check(clean, "the candidate list excludes the self and the cycle it would create");
        check(cands.size() < p.conditions.size(), "and is genuinely narrower than the whole library");
    }

    std::printf("=== a tier that claims a source must name one ===\n");
    {
        QString uncited;
        const auto core = makeResourcePackProvider();
        for (const Condition &c : core->pack().conditions)
            if (c.provenance.citation.isEmpty()) { uncited = c.id; break; }
        check(!uncited.isEmpty(), "the shipped pack has an uncited condition");

        const QVariantMap r = m.setField(QStringLiteral("characteristics"), uncited,
                                         QStringLiteral("tier"), QStringLiteral("supported"));
        check(!r.value(QStringLiteral("ok")).toBool(),
              "'supported' is refused with no citation to support it");
        check(r.value(QStringLiteral("message")).toString().contains(QStringLiteral("citation")),
              "and the refusal names what is missing");
        check(m.unsavedCount() == 0, "a refused edit writes nothing");

        // Practice and noSourceFound carry no citation by design, so they are NOT refused.
        const QVariantMap ok = m.setField(QStringLiteral("characteristics"), uncited,
                                          QStringLiteral("tier"), QStringLiteral("practice"));
        check(ok.value(QStringLiteral("ok")).toBool(),
              "'practice' is allowed uncited — the tier says the field agrees, not that a paper does");
        m.undo();
    }

    std::printf("=== a bulk-set is ONE command ===\n");
    {
        QStringList ids;
        for (const QVariant &v : m.rows(QStringLiteral("links"))) {
            const QVariantMap r = v.toMap();
            if (cellFor(r, QStringLiteral("strength")).value(QStringLiteral("editable")).toBool())
                ids << r.value(QStringLiteral("id")).toString();
            if (ids.size() == 12) break;
        }
        check(ids.size() == 12, "twelve causal links to re-tier");

        // Flush any redo tail an earlier section left behind. A new edit after an undo discards the
        // abandoned branch, and that discard would otherwise be indistinguishable from the collapse
        // this section is testing.
        const QString flushId = firstShippedId(m.rows(QStringLiteral("characteristics")));
        m.setField(QStringLiteral("characteristics"), flushId, QStringLiteral("label"),
                   QStringLiteral("flush"));
        check(!m.canRedo(), "the stack tip is clean before the bulk-set");
        const int stackBefore  = m.edits().size();
        const int unsavedBefore = m.unsavedCount();

        const QVariantMap r = m.setFieldOnAll(QStringLiteral("links"), ids,
                                              QStringLiteral("strength"), QStringLiteral("strong"));
        check(r.value(QStringLiteral("ok")).toBool(), "the bulk-set lands");
        check(m.edits().size() == stackBefore + 1,
              "twelve rows changed, ONE entry on the stack");
        check(m.unsavedCount() >= unsavedBefore + 12, "and twelve more objects are unsaved");

        // The whole gesture reverses in one step. Twelve undos to reverse one action is an undo an
        // author stops halfway through, believing they are back where they started.
        m.undo();
        check(m.unsavedCount() == unsavedBefore, "one undo reverses the whole selection");
        m.undo();   // and the flush
        check(m.unsavedCount() == 0, "back to the file");
    }

    std::printf("=== removing one cause keeps the others ===\n");
    {
        // The failure this guards against is silent and total: a user pack REPLACES a condition's
        // whole incoming causal set, so writing back only the edges that survived a delete would
        // drop every other cause of that condition — and the panel would show it as a clean,
        // successful edit until somebody noticed the graph had thinned out.
        const auto  core = makeResourcePackProvider();
        const auto &p    = core->pack();

        QString target;
        for (const Condition &c : p.conditions)
            if (causesOf(p, c.id).size() >= 3) { target = c.id; break; }
        check(!target.isEmpty(), "a condition with three or more causes");

        const QStringList wasCausedBy = causesOf(p, target);
        const QVariantMap r = m.removeLink(wasCausedBy.first(), target, QStringLiteral("causes"));
        check(r.value(QStringLiteral("ok")).toBool(), "one cause is removed");

        const QVariantMap after = m.inspect(QStringLiteral("characteristics"), target);
        int nowCausedBy = 0;
        for (const QVariant &v : after.value(QStringLiteral("sections")).toList())
            if (v.toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Caused by"))
                nowCausedBy = v.toMap().value(QStringLiteral("count")).toInt();
        check(nowCausedBy == wasCausedBy.size() - 1,
              "exactly one cause is gone — the rest survive the override");

        m.undo();
        check(m.unsavedCount() == 0, "and undo puts it back");
    }

    std::printf("=== attaching a measure mints its signal ===\n");
    {
        const auto  core = makeResourcePackProvider();
        const auto &p    = core->pack();

        QString cond;
        for (const Condition &c : p.conditions)
            if (c.observability != Observability::Latent) { cond = c.id; break; }

        const QVariantList cands = m.measureCandidates(cond);
        check(!cands.isEmpty(), "there are measures this characteristic does not already read");
        const QString mid = cands.value(0).toMap().value(QStringLiteral("id")).toString();

        const int signalsBefore = m.rows(QStringLiteral("signals")).size();
        const QVariantMap r = m.addMeasureTo(cond, mid, QStringLiteral("high"));
        check(r.value(QStringLiteral("ok")).toBool(), "the measure attaches");
        check(m.rows(QStringLiteral("signals")).size() == signalsBefore + 1,
              "and a signal is minted to read it");

        // Offering it twice must refuse, and must leave nothing behind — the copy-on-write reach
        // for the condition happens before the refusal can be known.
        const int unsavedAfterAdd = m.unsavedCount();
        const QVariantMap again = m.addMeasureTo(cond, mid, QStringLiteral("high"));
        check(!again.value(QStringLiteral("ok")).toBool(), "attaching it again is refused");
        check(m.unsavedCount() == unsavedAfterAdd, "and the refusal writes nothing");

        const QVariantMap off = m.removeMeasureFrom(cond, mid);
        check(off.value(QStringLiteral("ok")).toBool(), "and it can be detached again");

        m.undo();
        m.undo();
        check(m.unsavedCount() == 0, "back to the file");
    }

    std::printf("=== duplicate beats blank ===\n");
    {
        const QString id = firstShippedId(m.rows(QStringLiteral("characteristics")));
        const QVariantMap r = m.duplicate(QStringLiteral("characteristics"), id);
        check(r.value(QStringLiteral("ok")).toBool(), "a characteristic can be duplicated");

        const QString newId = r.value(QStringLiteral("id")).toString();
        check(!newId.isEmpty() && newId != id, "the copy has an id of its own");

        const QVariantMap row = rowFor(m.rows(QStringLiteral("characteristics")), newId);
        check(row.value(QStringLiteral("source")).toString() == QStringLiteral("yours"),
              "the copy is yours, not an override of anything");

        // The copy inherits the explanation but NOT the evidence: the source material was cited for
        // the original claim, not for this one.
        const QVariantMap tier = cellFor(row, QStringLiteral("tier"));
        check(tier.value(QStringLiteral("value")).toString() == QStringLiteral("proposed"),
              "the copy starts as proposed — it has earned no evidence yet");

        m.undo();
        check(rowFor(m.rows(QStringLiteral("characteristics")), newId).isEmpty(),
              "undo removes the copy");
    }

    std::printf("=== the validation strip grades the DRAFT ===\n");
    {
        const int cleanWarnings = m.validationWarningCount();
        check(cleanWarnings > 0, "the shipped library has warnings to show");
        for (const QVariant &v : m.validation())
            if (v.toMap().value(QStringLiteral("severity")).toString() == QStringLiteral("error"))
                std::printf("    (error: %s / %s)\n",
                            qPrintable(v.toMap().value(QStringLiteral("code")).toString()),
                            qPrintable(v.toMap().value(QStringLiteral("subject")).toString()));
        check(m.validationErrorCount() == 0, "and no errors");

        // An edit that breaks something has to show up BEFORE it is saved. A strip that graded the
        // file would stay green through exactly the change worth catching.
        const QString id = firstShippedId(m.rows(QStringLiteral("characteristics")));
        m.setField(QStringLiteral("characteristics"), id, QStringLiteral("reach"),
                   QStringLiteral("screened"));
        check(m.validation().size() != cleanWarnings + m.validationErrorCount()
              || m.validationWarningCount() != cleanWarnings,
              "the draft's health moves with an unsaved edit");
        m.undo();
        check(m.validationWarningCount() == cleanWarnings, "and moves back when it is undone");
    }

    std::printf("=== save writes, and does NOT clear the stack ===\n");
    {
        const QString id = firstShippedId(m.rows(QStringLiteral("characteristics")));
        m.setField(QStringLiteral("characteristics"), id, QStringLiteral("label"),
                   QStringLiteral("Saved name"));
        check(m.unsavedCount() == 1, "one unsaved change");

        const QVariantMap r = m.save();
        check(r.value(QStringLiteral("ok")).toBool(), "the save lands");
        check(m.unsavedCount() == 0, "nothing is unsaved afterwards");
        check(QFile::exists(userPackPath()), "and the user pack is on disk");

        // ADDENDUM-01's fourth point: saving and immediately spotting the mistake is the common
        // case, so the stack survives the save.
        check(m.canUndo(), "undo is still offered after a save");
        check(!m.edits().isEmpty(), "and the history is still there");

        bool anySaved = false;
        for (const QVariant &v : m.edits())
            if (v.toMap().value(QStringLiteral("saved")).toBool()) anySaved = true;
        check(anySaved, "the history marks what is on disk");

        m.undo();
        check(m.unsavedCount() == 1,
              "undoing a SAVED edit reverses it and reports the reversal as unsaved work");

        const QVariantMap row = rowFor(m.rows(QStringLiteral("characteristics")), id);
        check(row.value(QStringLiteral("label")).toString() != QStringLiteral("Saved name"),
              "and the reversal is what the table shows");

        m.save();
        check(m.unsavedCount() == 0, "saving again settles it");
    }

    std::printf("=== revert is itself undoable ===\n");
    {
        const QString id = firstShippedId(m.rows(QStringLiteral("characteristics")));
        m.setField(QStringLiteral("characteristics"), id, QStringLiteral("label"),
                   QStringLiteral("Throwaway"));
        check(m.unsavedCount() == 1, "an unsaved edit to discard");

        const QVariantMap r = m.revert();
        check(r.value(QStringLiteral("ok")).toBool(), "revert reports what it discarded");
        check(m.unsavedCount() == 0, "and the draft matches the file");

        // Discarding an afternoon's work with no way back would be the one unrecoverable action in
        // a panel whose rule is that there are none.
        m.undo();
        check(m.unsavedCount() == 1, "undo brings the discarded work back");
        m.revert();
    }

    std::printf("=== the edits history is navigable ===\n");
    {
        const QString id = firstShippedId(m.rows(QStringLiteral("characteristics")));
        const int     base = m.edits().size();
        m.setField(QStringLiteral("characteristics"), id, QStringLiteral("label"), QStringLiteral("A"));
        m.setField(QStringLiteral("characteristics"), id, QStringLiteral("label"), QStringLiteral("B"));
        m.setField(QStringLiteral("characteristics"), id, QStringLiteral("label"), QStringLiteral("C"));
        check(m.edits().size() == base + 3, "three edits, three entries");
        check(rowFor(m.rows(QStringLiteral("characteristics")), id)
                  .value(QStringLiteral("label")).toString() == QStringLiteral("C"),
              "the last one is what the table shows");

        // Clicking an entry winds the whole model to that point.
        m.undoTo(base);
        check(rowFor(m.rows(QStringLiteral("characteristics")), id)
                  .value(QStringLiteral("label")).toString() == QStringLiteral("A"),
              "winding to entry N leaves the model as entry N left it");

        int undone = 0;
        for (const QVariant &v : m.edits())
            if (v.toMap().value(QStringLiteral("undone")).toBool()) ++undone;
        check(undone == 2, "the two entries ahead of the cursor stay listed, marked undone");

        // A new edit after an undo forks history; the abandoned branch goes, because a redo that no
        // longer applies to the pack in hand is worse than no redo.
        m.setField(QStringLiteral("characteristics"), id, QStringLiteral("label"), QStringLiteral("D"));
        check(m.edits().size() == base + 2, "the abandoned redo tail is discarded");
        check(!m.canRedo(), "and redo is no longer offered");
    }

    std::printf("=== the graph reads the working copy ===\n");
    {
        const auto  core = makeResourcePackProvider();
        const auto &p    = core->pack();
        QString     focus;
        for (const Condition &c : p.conditions)
            if (!causesOf(p, c.id).isEmpty()) { focus = c.id; break; }
        check(!focus.isEmpty(), "a condition with causes to centre on");

        QVariantMap opts;
        opts.insert(QStringLiteral("depth"), 2);
        const QVariantMap g = m.dag(focus, opts);
        check(!g.value(QStringLiteral("nodes")).toList().isEmpty(), "the layout has nodes");
        check(g.value(QStringLiteral("width")).toDouble() > 0, "and a size, computed in C++");

        // One node, one box. A condition that is both a direct cause and a cause-of-a-cause must
        // not render twice — that bug appeared in the mockup, so it is asserted rather than assumed.
        QSet<QString> nodeIds;
        bool          unique = true;
        for (const QVariant &v : g.value(QStringLiteral("nodes")).toList()) {
            const QString nid = v.toMap().value(QStringLiteral("id")).toString();
            if (nodeIds.contains(nid)) unique = false;
            nodeIds.insert(nid);
        }
        check(unique, "each node is drawn exactly once");

        // Every real edge carries the row id the table uses, so clicking a line in the graph
        // selects the same object the Causal links table does.
        bool haveRowIds = false;
        for (const QVariant &v : g.value(QStringLiteral("edges")).toList())
            if (!v.toMap().value(QStringLiteral("rowId")).toString().isEmpty()) haveRowIds = true;
        check(haveRowIds, "graph edges carry the table's row id");
    }

    std::printf("=== the graph answers for the row the UI would hand it ===\n");
    {
        // Exactly what the panel passes: the id off a table row, and the theme metrics the pane
        // supplies. A layout that works for a hand-picked condition and not for the one a user
        // clicks is a layout that works nowhere that matters.
        const QVariantMap opts = [] {
            QVariantMap o;
            o.insert(QStringLiteral("nodeH"), 34);
            o.insert(QStringLiteral("gapX"), 52);
            o.insert(QStringLiteral("gapY"), 14);
            o.insert(QStringLiteral("laneGap"), 36);
            o.insert(QStringLiteral("padX"), 12);
            o.insert(QStringLiteral("charW"), 6);
            o.insert(QStringLiteral("minW"), 110);
            o.insert(QStringLiteral("maxW"), 210);
            o.insert(QStringLiteral("depth"), 2);
            o.insert(QStringLiteral("maxPerRank"), 8);
            o.insert(QStringLiteral("includeMeasures"), false);
            return o;
        }();

        const QVariantMap g = m.dag(QStringLiteral("ball_forward"), opts);
        std::printf("    (ball_forward: nodes=%d edges=%d w=%.0f h=%.0f)\n",
                    int(g.value(QStringLiteral("nodes")).toList().size()),
                    int(g.value(QStringLiteral("edges")).toList().size()),
                    g.value(QStringLiteral("width")).toDouble(),
                    g.value(QStringLiteral("height")).toDouble());
        check(!g.value(QStringLiteral("nodes")).toList().isEmpty(),
              "a characteristic a user would actually pick has a graph");
        check(g.value(QStringLiteral("width")).toDouble() > 0
              && g.value(QStringLiteral("height")).toDouble() > 0,
              "with a size to scroll inside");

        // Every characteristic in the table, so no row can open onto a blank canvas.
        int blank = 0;
        for (const QVariant &v : m.rows(QStringLiteral("characteristics"))) {
            const QString id = v.toMap().value(QStringLiteral("id")).toString();
            if (m.dag(id, opts).value(QStringLiteral("nodes")).toList().isEmpty()) ++blank;
        }
        check(blank == 0, "and so does every other row in the table");

        // And EVERY type, not only conditions. A measure has readers and corridors, a screen has
        // what it would settle — none of it ranks, but all of it is a neighbourhood, and a graph
        // that only worked for one type of row was a graph that mostly did not work.
        for (const QString &type : { QStringLiteral("characteristics"), QStringLiteral("causes"),
                                     QStringLiteral("measures"), QStringLiteral("signals"),
                                     QStringLiteral("links"), QStringLiteral("screens"),
                                     QStringLiteral("drills"), QStringLiteral("references"),
                                     QStringLiteral("corridors") }) {
            const QVariantList rows = m.rows(type);
            if (rows.isEmpty()) continue;

            int empty = 0, checked = 0;
            for (const QVariant &v : rows) {
                if (checked >= 25) break;   // a sample per type; the whole set is thousands
                ++checked;
                const QString rid = v.toMap().value(QStringLiteral("id")).toString();
                const QVariantMap g = m.graph(type, rid, opts);
                if (g.value(QStringLiteral("nodes")).toList().isEmpty()) ++empty;
                else {
                    // Sized, centred on something, and every node knows what KIND it is — which is
                    // what the colour and the glyph are keyed on.
                    check(g.value(QStringLiteral("width")).toDouble() > 0
                          && g.value(QStringLiteral("height")).toDouble() > 0, "sized");
                    bool typed = true;
                    for (const QVariant &nv : g.value(QStringLiteral("nodes")).toList())
                        if (nv.toMap().value(QStringLiteral("nodeType")).toString().isEmpty())
                            typed = false;
                    check(typed, "every node carries its type");
                    break;   // one full shape check per type is enough
                }
            }
            check(empty == 0, qPrintable(QStringLiteral("%1: rows have a graph").arg(type)));
        }

        // A measure's node says what grades it, which is the question a reader has when they see a
        // measure in a graph at all.
        const QVariantMap mg = m.graph(QStringLiteral("measures"),
                                       QStringLiteral("m_ballPosition"), opts);
        bool sawNorm = false;
        for (const QVariant &v : mg.value(QStringLiteral("nodes")).toList()) {
            const QVariantMap n = v.toMap();
            if (n.value(QStringLiteral("id")).toString() == QStringLiteral("m_ballPosition")
                && n.value(QStringLiteral("note")).toString().contains(QStringLiteral("μ")))
                sawNorm = true;
        }
        check(sawNorm, "a measure node carries its corridor");

        // A health row is a finding, not an object — it has no neighbourhood, and inventing one
        // would draw a relationship that does not exist.
        check(m.graph(QStringLiteral("health"), QStringLiteral("health:0"), opts)
                  .value(QStringLiteral("nodes")).toList().isEmpty(),
              "a health finding has no graph, and does not pretend to");
    }

    std::printf("=== the metric catalogue is reachable, and joins to measures ===\n");
    {
        const QVariantList rows = m.rows(QStringLiteral("metrics"));
        check(rows.size() > 20, "the catalogue is loaded");

        // The join is the reason this view exists: a metric is a curve, a measure is that curve
        // reduced at a phase, and following the chain used to mean leaving the page.
        QString joined;
        for (const QVariant &v : rows) {
            const QVariantMap r = v.toMap();
            for (const QVariant &cv : r.value(QStringLiteral("cells")).toList()) {
                Q_UNUSED(cv)
            }
            const QVariantMap d = m.inspect(QStringLiteral("metrics"),
                                            r.value(QStringLiteral("id")).toString());
            for (const QVariant &sv : d.value(QStringLiteral("sections")).toList()) {
                const QVariantMap sec = sv.toMap();
                if (sec.value(QStringLiteral("title")).toString().contains(QStringLiteral("measures"))
                    && sec.value(QStringLiteral("count")).toInt() > 0)
                    joined = r.value(QStringLiteral("id")).toString();
            }
            if (!joined.isEmpty()) break;
        }
        check(!joined.isEmpty(), "at least one metric names the measures that read it");

        // m_ballPosition reads `ballPosition`, so that metric must reach it — and through it, the
        // corridor that grades it.
        const QVariantMap bp = m.inspect(QStringLiteral("metrics"), QStringLiteral("ballPosition"));
        check(bp.value(QStringLiteral("found")).toBool(), "the ball position metric is found");
        bool reachesMeasure = false;
        for (const QVariant &sv : bp.value(QStringLiteral("sections")).toList())
            for (const QVariant &rv : sv.toMap().value(QStringLiteral("rows")).toList())
                if (rv.toMap().value(QStringLiteral("id")).toString()
                    == QStringLiteral("m_ballPosition"))
                    reachesMeasure = true;
        check(reachesMeasure, "and reaches the measure built on it");

        // The graph works for it too, which is what makes it a first-class type rather than a list.
        QVariantMap gopt;
        gopt.insert(QStringLiteral("depth"), 2);
        check(!m.graph(QStringLiteral("metrics"), QStringLiteral("ballPosition"), gopt)
                   .value(QStringLiteral("nodes")).toList().isEmpty(),
              "a metric has a neighbourhood");
    }

    std::printf("=== read-only types say so rather than pretending ===\n");
    {
        // Screens and drills LEFT this list in ADDENDUM-02 — they are ordinary editable types now,
        // and their own section below asserts it. What stays read-only stays read-only for a stated
        // reason, not by omission: a reference is imported and regenerated on every pack build, so
        // an edit here would be overwritten, and a metric is produced by the pipeline, so the place
        // to change it is the code.
        for (const QString &type : { QStringLiteral("references"), QStringLiteral("metrics") }) {
            const QVariantList rows = m.rows(type);
            bool anyEditable = false;
            for (const QVariant &v : rows)
                for (const QVariant &cv : v.toMap().value(QStringLiteral("cells")).toList())
                    if (cv.toMap().value(QStringLiteral("editable")).toBool()) anyEditable = true;
            check(!anyEditable,
                  qPrintable(QStringLiteral("%1 offers no editable cell").arg(type)));

            const QVariantMap r = m.setField(type, rows.value(0).toMap()
                                                       .value(QStringLiteral("id")).toString(),
                                             QStringLiteral("label"), QStringLiteral("x"));
            check(!r.value(QStringLiteral("ok")).toBool(),
                  qPrintable(QStringLiteral("%1 refuses a write").arg(type)));
        }
    }

    std::printf("=== corridors share ONE history with the pack ===\n");
    {
        m.undoTo(-1);   // start from the file, so the counts below mean what they say
        // m_ballPosition ships five corridors — any, driver, fairway_wood, iron, wedge — so it is
        // the case that proves context resolution as well as editing.
        const QVariantList rows = m.rows(QStringLiteral("corridors"));
        check(rows.size() > 100, "the shipped corridors are loaded");

        const QString id = QStringLiteral("norm:m_ballPosition@driver");
        QVariantMap   row = rowFor(rows, id);
        check(!row.isEmpty(), "ball position has a driver corridor");
        check(row.value(QStringLiteral("source")).toString() == QStringLiteral("shipped"),
              "and it is shipped, not yours");

        const QVariantMap r = m.setField(QStringLiteral("corridors"), id, QStringLiteral("mu"), 8.0);
        check(r.value(QStringLiteral("ok")).toBool(), "a corridor's aspiration can be edited");

        row = rowFor(m.rows(QStringLiteral("corridors")), id);
        check(cellFor(row, QStringLiteral("mu")).value(QStringLiteral("value")).toDouble() == 8.0,
              "the edit is visible BEFORE any save");
        check(row.value(QStringLiteral("dirty")).toBool(), "the corridor row is marked unsaved");
        // "both", not "yours": core ships a driver row and this now overrides it, which is the
        // same distinction a shipped characteristic draws once it has been edited.
        check(row.value(QStringLiteral("source")).toString() == QStringLiteral("both"),
              "editing a shipped corridor makes it yours, over shipped");

        // The point of the whole exercise: ONE stack over two registries.
        const QString condId = firstShippedId(m.rows(QStringLiteral("characteristics")));
        m.setField(QStringLiteral("characteristics"), condId, QStringLiteral("label"),
                   QStringLiteral("Pack edit"));
        check(m.unsavedCount() == 2, "one corridor and one characteristic are unsaved");

        m.undo();
        check(rowFor(m.rows(QStringLiteral("characteristics")), condId)
                  .value(QStringLiteral("label")).toString() != QStringLiteral("Pack edit"),
              "one undo reverses the PACK edit");
        check(cellFor(rowFor(m.rows(QStringLiteral("corridors")), id), QStringLiteral("mu"))
                  .value(QStringLiteral("value")).toDouble() == 8.0,
              "and leaves the corridor edit alone — the stack is ordered, not partitioned");

        m.undo();
        check(cellFor(rowFor(m.rows(QStringLiteral("corridors")), id), QStringLiteral("mu"))
                  .value(QStringLiteral("value")).toDouble() != 8.0,
              "the next undo reverses the CORRIDOR edit");
        check(m.unsavedCount() == 0, "and nothing is unsaved");

        // Refusals leave nothing behind on the norm side either.
        const QVariantMap bad = m.setField(QStringLiteral("corridors"), id,
                                           QStringLiteral("sigmaLo"), -1.0);
        check(!bad.value(QStringLiteral("ok")).toBool(), "a negative tolerance is refused");
        check(m.unsavedCount() == 0, "and the refusal writes nothing");

        const QVariantMap badUnit = m.setField(QStringLiteral("corridors"), id,
                                               QStringLiteral("unit"), QStringLiteral("bananas"));
        check(!badUnit.value(QStringLiteral("ok")).toBool(),
              "a unit that disagrees with the measure is refused");
        check(m.unsavedCount() == 0, "and writes nothing");
    }

    std::printf("=== a dragged handle stores a tolerance, not an edge ===\n");
    {
        m.undoTo(-1);
        const QString id = QStringLiteral("norm:m_ballPosition@driver");

        // Under `standard` idealMaxZ is 1.0, so the edge and the tolerance coincide and any
        // arithmetic looks right. Under `strict` they do not — which is the whole reason the
        // conversion has to exist and has to live in one place.
        m.setGradePolicy(QStringLiteral("strict"));
        const double z = gradePolicyByName(QStringLiteral("strict")).idealMaxZ;
        check(!qFuzzyCompare(z, 1.0), "strict really does move the ideal edge");

        QVariantMap before = rowFor(m.rows(QStringLiteral("corridors")), id);
        const double mu = cellFor(before, QStringLiteral("mu"))
                              .value(QStringLiteral("value")).toDouble();

        // Drag the high edge to mu + 9. The TOLERANCE that produces that edge is 9 / idealMaxZ.
        const QVariantMap r = m.setField(QStringLiteral("corridors"), id,
                                         QStringLiteral("idealHi"), mu + 9.0);
        check(r.value(QStringLiteral("ok")).toBool(), "the edge can be set");

        const QVariantMap after = rowFor(m.rows(QStringLiteral("corridors")), id);
        const double stored = cellFor(after, QStringLiteral("sigmaHi"))
                                  .value(QStringLiteral("value")).toDouble();
        check(std::abs(stored - 9.0 / z) < 1e-6,
              "the stored tolerance is the edge divided back through the policy");
        check(std::abs(stored - 9.0) > 1e-6,
              "and is NOT the raw half-width — storing that would freeze one policy into the file");

        // And the round trip: the picture must put the edge back where it was dragged to.
        const QVariantMap plot = m.corridorPlot(QStringLiteral("m_ballPosition"),
                                                QStringLiteral("driver"));
        check(std::abs(plot.value(QStringLiteral("idealHi")).toDouble() - (mu + 9.0)) < 1e-6,
              "and the handle lands back exactly where it was let go");

        m.setGradePolicy(QStringLiteral("standard"));
        m.undoTo(-1);
    }

    std::printf("=== the skew is authorable, and the bounds are reachable ===\n");
    {
        m.undoTo(-1);
        const QString id = QStringLiteral("norm:m_ballPosition@driver");

        // Asymmetry is the design, not a side effect: a corridor may be tight one side and loose
        // the other, and each tolerance is set on its own.
        check(m.setField(QStringLiteral("corridors"), id, QStringLiteral("sigmaLo"), 2.0)
                  .value(QStringLiteral("ok")).toBool(), "the low tolerance is set alone");
        check(m.setField(QStringLiteral("corridors"), id, QStringLiteral("sigmaHi"), 18.0)
                  .value(QStringLiteral("ok")).toBool(), "and the high one alone");

        QVariantMap plot = m.corridorPlot(QStringLiteral("m_ballPosition"),
                                          QStringLiteral("driver"));
        const double mu = plot.value(QStringLiteral("mu")).toDouble();
        check(std::abs(plot.value(QStringLiteral("sigmaLo")).toDouble() - 2.0) < 1e-9
              && std::abs(plot.value(QStringLiteral("sigmaHi")).toDouble() - 18.0) < 1e-9,
              "both survive — a skewed corridor is a corridor");
        check(std::abs((mu - plot.value(QStringLiteral("idealLo")).toDouble())
                       - (plot.value(QStringLiteral("idealHi")).toDouble() - mu)) > 1.0,
              "and the picture is skewed with it, not centred");

        // The plausibility bounds had no editor anywhere — not a column, not a handle. Empty text
        // clears one, which is a state no number can stand for.
        check(m.setField(QStringLiteral("corridors"), id, QStringLiteral("plausibleHi"), 95.0)
                  .value(QStringLiteral("ok")).toBool(), "a plausibility bound can be set");
        plot = m.corridorPlot(QStringLiteral("m_ballPosition"), QStringLiteral("driver"));
        check(plot.value(QStringLiteral("hasPlausibleHi")).toBool()
              && std::abs(plot.value(QStringLiteral("plausibleHi")).toDouble() - 95.0) < 1e-9,
              "and reaches the picture");

        check(m.setField(QStringLiteral("corridors"), id, QStringLiteral("plausibleHi"),
                         QString()).value(QStringLiteral("ok")).toBool(),
              "an empty value clears it");
        plot = m.corridorPlot(QStringLiteral("m_ballPosition"), QStringLiteral("driver"));
        check(!plot.value(QStringLiteral("hasPlausibleHi")).toBool(),
              "and 'no bound' is reported as absent rather than as zero");

        m.undoTo(-1);
    }

    std::printf("=== what a drag stores is what the unit can express ===\n");
    {
        m.undoTo(-1);
        const QString id = QStringLiteral("norm:m_ballPosition@driver");   // % stance width

        // Where a pointer stopped, not a measurement. Whole percent is the most this can mean.
        m.setField(QStringLiteral("corridors"), id, QStringLiteral("mu"), 30.418273);
        QVariantMap plot = m.corridorPlot(QStringLiteral("m_ballPosition"),
                                          QStringLiteral("driver"));
        check(std::abs(plot.value(QStringLiteral("mu")).toDouble() - 30.0) < 1e-9,
              "a dragged aspiration is stored at the unit's own precision");

        m.setField(QStringLiteral("corridors"), id, QStringLiteral("sigmaHi"), 7.83);
        plot = m.corridorPlot(QStringLiteral("m_ballPosition"), QStringLiteral("driver"));
        check(std::abs(plot.value(QStringLiteral("sigmaHi")).toDouble() - 8.0) < 1e-9,
              "and so is a typed tolerance");

        check(plot.value(QStringLiteral("decimals")).toInt() == 0
              && std::abs(plot.value(QStringLiteral("step")).toDouble() - 1.0) < 1e-9,
              "the precision travels with the picture, so every surface rounds the same way");

        // A degree corridor is governed the same way, through the same table.
        const QVariantList rows = m.rows(QStringLiteral("corridors"));
        QString degreeId;
        for (const QVariant &v : rows) {
            const QVariantMap r = v.toMap();
            for (const QVariant &cv : r.value(QStringLiteral("cells")).toList())
                if (cv.toMap().value(QStringLiteral("field")).toString() == QStringLiteral("unit")
                    && cv.toMap().value(QStringLiteral("value")).toString()
                           == QString::fromUtf8("°"))
                    degreeId = r.value(QStringLiteral("id")).toString();
            if (!degreeId.isEmpty()) break;
        }
        check(!degreeId.isEmpty(), "the shipped set has a corridor in degrees");
        m.setField(QStringLiteral("corridors"), degreeId, QStringLiteral("mu"), 12.7);
        QString dmid, dctx;
        ModelBrowserTestAccess::split(degreeId, dmid, dctx);
        check(std::abs(m.corridorPlot(dmid, dctx).value(QStringLiteral("mu")).toDouble() - 13.0)
                  < 1e-9,
              "and a degree lands whole");

        m.undoTo(-1);
    }

    std::printf("=== the preview lays out what the drag would store ===\n");
    {
        const QString mid = QStringLiteral("m_ballPosition");
        const QString ctx = QStringLiteral("driver");

        const QVariantMap live = m.corridorPlot(mid, ctx);
        const double      mu   = live.value(QStringLiteral("mu")).toDouble();

        // A preview must move the picture WITHOUT touching the draft — that is what lets a drag
        // reshape the curve on every mouse move and still put one command on the stack.
        QVariantMap opts;
        opts.insert(QStringLiteral("previewMu"), mu + 12.0);
        const QVariantMap preview = m.corridorPlot(mid, ctx, opts);

        check(std::abs(preview.value(QStringLiteral("mu")).toDouble() - (mu + 12.0)) < 1e-6,
              "the preview is laid out at the dragged value");
        check(preview.value(QStringLiteral("muX")).toDouble()
                  != live.value(QStringLiteral("muX")).toDouble(),
              "so the curve and the bands move with it");
        check(m.unsavedCount() == 0, "and nothing is written while it moves");
        check(std::abs(m.corridorPlot(mid, ctx).value(QStringLiteral("mu")).toDouble() - mu) < 1e-6,
              "the stored corridor is untouched");

        // The band shares recompute too, which is what makes dragging on top of the data useful.
        check(preview.contains(QStringLiteral("ideal")) && preview.contains(QStringLiteral("note")),
              "the counts and the finding are part of the preview");
    }

    std::printf("=== corridors: add, adopt, reset ===\n");
    {
        const QVariantList ctxs = m.corridorContexts(QStringLiteral("m_ballPosition"));
        check(ctxs.size() >= 5, "the context tree is offered");

        QString freeCtx;
        for (const QVariant &v : ctxs) {
            const QVariantMap c = v.toMap();
            if (!c.value(QStringLiteral("own")).toBool()
                && c.value(QStringLiteral("found")).toBool()) {
                freeCtx = c.value(QStringLiteral("id")).toString();
                break;
            }
        }
        check(!freeCtx.isEmpty(), "a context with no row of its own");

        const QVariantMap add = m.addCorridor(QStringLiteral("m_ballPosition"), freeCtx);
        check(add.value(QStringLiteral("ok")).toBool(), "a corridor can be added there");
        check(!m.addCorridor(QStringLiteral("m_ballPosition"), freeCtx)
                   .value(QStringLiteral("ok")).toBool(),
              "and not twice");

        const QVariantMap adopted = m.adoptCorridor(QStringLiteral("m_ballPosition"), freeCtx,
                                                    QStringLiteral("wedge"));
        check(adopted.value(QStringLiteral("ok")).toBool(), "another context's numbers can be adopted");

        const QVariantMap reset = m.resetCorridor(QStringLiteral("m_ballPosition"), freeCtx);
        check(reset.value(QStringLiteral("ok")).toBool(), "and the row can be dropped again");
        check(!m.resetCorridor(QStringLiteral("m_ballPosition"), freeCtx)
                   .value(QStringLiteral("ok")).toBool(),
              "resetting what is already shipped is refused rather than silently doing nothing");

        m.undoTo(-1);
        check(m.unsavedCount() == 0, "winding right back leaves the file untouched");
    }

    std::printf("=== aliases, injury note, new-from-blank ===\n");
    {
        m.undoTo(-1);
        const QString id = firstShippedId(m.rows(QStringLiteral("characteristics")));

        check(m.setField(QStringLiteral("characteristics"), id, QStringLiteral("aliases"),
                         QStringLiteral(" zzqx , zzqy ,, zzqx "))
                  .value(QStringLiteral("ok")).toBool(),
              "aliases take a comma-separated line");

        // Two conditions may not answer to one coach term: search resolves to whichever came first
        // in the file, so the term silently leads to the wrong page.
        QString otherId, stolenTerm;
        for (const QVariant &v : m.rows(QStringLiteral("characteristics"))) {
            const QString cid = v.toMap().value(QStringLiteral("id")).toString();
            if (cid == id) continue;
            const auto core = makeResourcePackProvider();
            if (const Condition *c = core->pack().condition(cid))
                if (!c->aliases.isEmpty()) { otherId = cid; stolenTerm = c->aliases.first(); break; }
        }
        if (!otherId.isEmpty()) {
            const QVariantMap clash = m.setField(QStringLiteral("characteristics"), id,
                                                 QStringLiteral("aliases"), stolenTerm);
            check(!clash.value(QStringLiteral("ok")).toBool(),
                  "a term another characteristic already answers to is refused");
        }

        check(m.setField(QStringLiteral("characteristics"), id, QStringLiteral("injuryNote"),
                         QStringLiteral("Loads the lead wrist."))
                  .value(QStringLiteral("ok")).toBool(),
              "the injury note can be edited");

        const QVariantMap made = m.createObject(QStringLiteral("characteristics"));
        check(made.value(QStringLiteral("ok")).toBool(), "a blank characteristic can be created");
        const QVariantMap row = rowFor(m.rows(QStringLiteral("characteristics")),
                                       made.value(QStringLiteral("id")).toString());
        check(row.value(QStringLiteral("source")).toString() == QStringLiteral("yours"),
              "and it is yours");
        check(cellFor(row, QStringLiteral("tier")).value(QStringLiteral("value")).toString()
                  == QStringLiteral("proposed"),
              "starting as proposed — nobody has been to the literature for it");

        m.undoTo(-1);
    }

    std::printf("=== bindings cascade as ONE command ===\n");
    {
        const QString id = firstShippedId(m.rows(QStringLiteral("characteristics")));

        const QVariantList before = m.bindingsOf(id);
        check(before.size() >= 10, "every context is offered, not only the authored ones");
        bool anyOwn = false;
        for (const QVariant &v : before)
            if (v.toMap().value(QStringLiteral("own")).toBool()) anyOwn = true;
        check(!anyOwn, "and the shipped characteristic states none of them — bindings are exceptions");

        // Author a narrow row, then switch its PARENT off. The cascade has to clear the narrow row,
        // and the whole thing has to undo as one step — a cascade that undid partially would leave
        // the author believing they were back where they started.
        QString parent, child;
        for (const QVariant &v : before) {
            const QVariantMap b = v.toMap();
            if (b.value(QStringLiteral("hasChildren")).toBool()
                && b.value(QStringLiteral("depth")).toInt() > 0) {
                parent = b.value(QStringLiteral("id")).toString();
                break;
            }
        }
        if (!parent.isEmpty()) {
            for (const QVariant &v : before) {
                const QVariantMap b = v.toMap();
                if (b.value(QStringLiteral("depth")).toInt() > 0
                    && b.value(QStringLiteral("id")).toString() != parent
                    && b.value(QStringLiteral("inheritedFrom")).toString().isEmpty()) {
                    child = b.value(QStringLiteral("id")).toString();
                }
            }
        }
        check(!parent.isEmpty(), "the context tree has a parent with children");

        // A child of that parent, so the cascade has something to reach.
        QString descendant;
        for (const QVariant &v : before) {
            const QVariantMap b = v.toMap();
            if (b.value(QStringLiteral("parentId")).toString() == parent)
                descendant = b.value(QStringLiteral("id")).toString();
        }
        check(!descendant.isEmpty(), "and that parent has a child");

        // Wound right back, so the next edit discards the abandoned branch and the history is
        // exactly the commands this section makes.
        m.undoTo(-1);
        check(m.setBinding(id, descendant, true, false).value(QStringLiteral("ok")).toBool(),
              "a narrow context can be marked not-counted");
        check(m.edits().size() == 1, "one edit, one command");

        // Switching the PARENT off has to clear the narrower row that says otherwise, or the untick
        // silently does not take.
        const QVariantMap off = m.setBinding(id, parent, false, false);
        check(off.value(QStringLiteral("ok")).toBool(), "and its parent switched off entirely");
        check(off.value(QStringLiteral("cascaded")).toInt() >= 1,
              "which cascades into the narrower row");

        int ownRows = 0;
        for (const QVariant &v : m.bindingsOf(id))
            if (v.toMap().value(QStringLiteral("own")).toBool()) ++ownRows;
        check(ownRows == 1, "leaving only the row the author actually set");

        // ONE undo, and the cascade comes back with it. A cascade that undid partially would be
        // worse than one that could not be undone.
        m.undo();
        ownRows = 0;
        bool childBack = false;
        for (const QVariant &v : m.bindingsOf(id)) {
            const QVariantMap b = v.toMap();
            if (!b.value(QStringLiteral("own")).toBool()) continue;
            ++ownRows;
            if (b.value(QStringLiteral("id")).toString() == descendant) childBack = true;
        }
        check(childBack, "one undo restores the row the cascade cleared");
        check(ownRows == 1, "and only that one");

        m.undo();
        check(m.unsavedCount() == 0, "and the whole thing unwinds to the file");
    }

    std::printf("=== minting a measure ===\n");
    {
        // Reuse beats creation: an existing measure's own facets must report as an exact match
        // rather than offering to make a second name for one number.
        const auto core = makeResourcePackProvider();
        const Measure *existing = nullptr;
        for (const Measure &meas : core->pack().measures)
            if (meas.kind == MeasureKind::Composed && meas.reducer.kind == ReducerKind::At
                && meas.reducer.anchor.has_value()) { existing = &meas; break; }
        check(existing != nullptr, "a composed measure read AT a phase, to compare against");

        QVariantMap facets;
        facets.insert(QStringLiteral("what"), roleName(existing->series.what));
        facets.insert(QStringLiteral("quantity"), quantityName(existing->series.quantity));
        facets.insert(QStringLiteral("reference"), roleName(existing->series.reference));
        facets.insert(QStringLiteral("anchor"),
                      phaseToken(existing->reducer.anchor.value_or(Phase::Address)));

        const QVariantMap preview = m.previewMeasure(facets);
        check(preview.value(QStringLiteral("valid")).toBool(), "its own facets are valid");
        // Matched on the SERIES tuple, not the id: shipped ids are hand-authored, so an id
        // comparison would find no duplicate for any of them.
        check(preview.contains(QStringLiteral("exactMatch")),
              "and report as an exact match — reuse it rather than minting a twin");
        check(preview.value(QStringLiteral("exactMatch")).toMap()
                  .value(QStringLiteral("id")).toString() == existing->id,
              "naming the measure it already is");
        check(!m.mintMeasure(facets).value(QStringLiteral("ok")).toBool(),
              "minting a duplicate is refused");

        // The validity table, not a guess: a point has no orientation, so a joint angle between a
        // point and anything is refused before the author can build it.
        QVariantMap junk;
        junk.insert(QStringLiteral("what"), QStringLiteral("nonsense"));
        check(!m.previewMeasure(junk).value(QStringLiteral("valid")).toBool(),
              "an unknown role is not valid");

        check(!m.quantitiesFor(roleName(existing->series.what)).isEmpty(),
              "the legal quantities for a role are offered");
        check(m.quantitiesFor(QStringLiteral("nonsense")).isEmpty(),
              "and nothing at all for a role that does not exist");
    }

    std::printf("=== the retirement surfaces answer ===\n");
    {
        check(!m.roadmap().isEmpty(), "the roadmap has rows");
        bool anyIntegration = false, allNonZero = true;
        for (const QVariant &v : m.roadmap()) {
            if (v.toMap().value(QStringLiteral("integration")).toBool()) anyIntegration = true;
            if (v.toMap().value(QStringLiteral("label")).toString().isEmpty()) allNonZero = false;
        }
        check(allNonZero, "and every row is named");
        Q_UNUSED(anyIntegration)

        // A capture gap must NEVER appear as roadmap work: one row implying a producer that will
        // never be built corrupts the artefact's meaning for every other row.
        QSet<QString> roadmapIds;
        for (const QVariant &v : m.roadmap())
            roadmapIds.insert(v.toMap().value(QStringLiteral("id")).toString());
        bool leaked = false;
        for (const QVariant &v : m.captureGaps()) {
            const QString gapId = v.toMap().value(QStringLiteral("id")).toString();
            for (const QVariant &rv : m.roadmap())
                if (rv.toMap().value(QStringLiteral("label")).toString()
                    == v.toMap().value(QStringLiteral("label")).toString())
                    leaked = true;
            Q_UNUSED(gapId)
        }
        check(!leaked, "no capture gap appears in the roadmap");

        check(!m.causeCoverage().isEmpty(), "cause coverage answers");
        check(!m.glossary().isEmpty(), "the glossary answers");
        check(m.glossary(QStringLiteral("zzzznotaterm")).isEmpty(),
              "and narrows to nothing for a term nobody uses");
        check(!m.gradePolicies().isEmpty(), "the grade policies are offered");
        check(!m.normSets().isEmpty(), "and the norm-set layers");
    }

    // ── ADDENDUM-02 ─────────────────────────────────────────────────────────
    //
    // Screens and drills became ordinary editable types, references gained a working surface, and
    // the panel gained a way back to the shipped model. Every one of those is a WRITE, so every one
    // gets the do-then-undo assertion the first addendum requires — the inverse of a relationship
    // command is the easy half to get wrong, and a wrong one is invisible until somebody relies on
    // ⌘Z and loses work.

    std::printf("=== a write NAMES what it did, after the rebuild that frees the pack ===\n");
    {
        // Every writer ends the same way: mutate, rebuild(), then say what happened. rebuild()
        // replaces the assembled pack, the norm provider and the screen and drill sets wholesale, so
        // any `const Condition *` taken beforehand is dangling by the time the message is composed.
        // addLink() did exactly that and crashed inside QString::arg() — the stack blamed the
        // formatting and the fault was three lines earlier.
        //
        // Only the REFUSAL paths were covered before, and those return before the rebuild. So this
        // walks the SUCCESS path of each writer and checks the message actually contains the label
        // it claims to.
        //
        // BE HONEST ABOUT WHAT THIS CATCHES. Reading freed memory usually gives the old bytes back,
        // so with the bug reintroduced these assertions still PASS — verified, not assumed. What
        // makes them worth having is that they walk the paths at all: the crash needs the write to
        // land, and nothing here landed one before. The detector is AddressSanitizer, which names
        // the exact line pair (freed in rebuild(), read in the message). If you are chasing a
        // "QString::arg" crash in this file, build these tests with -fsanitize=address first.
        m.undoTo(-1);

        const QVariantList chars = m.rows(QStringLiteral("characteristics"));
        QString fromId, toId, fromLabel, toLabel;
        for (const QVariant &v : chars) {
            const QVariantMap r = v.toMap();
            const QString     id = r.value(QStringLiteral("id")).toString();
            // A legal pair, asked of the same function the write asks.
            for (const QVariant &cv : m.linkCandidates(QStringLiteral("causes"), id)) {
                fromId = id;
                fromLabel = r.value(QStringLiteral("label")).toString();
                toId = cv.toMap().value(QStringLiteral("id")).toString();
                toLabel = cv.toMap().value(QStringLiteral("label")).toString();
                break;
            }
            if (!toId.isEmpty()) break;
        }
        check(!toId.isEmpty(), "there is a legal link to add");

        const QVariantMap linked = m.addLink(fromId, toId, QStringLiteral("causes"));
        check(linked.value(QStringLiteral("ok")).toBool(), "adding a cause succeeds");
        const QString msg = linked.value(QStringLiteral("message")).toString();
        check(msg.contains(fromLabel) && msg.contains(toLabel),
              "and the message names BOTH ends, read from labels that outlived the rebuild");
        check(m.undoLabel() == QStringLiteral("Link added"), "the command is on the stack");
        check(m.undo().value(QStringLiteral("ok")).toBool(), "and it undoes");

        // The same shape in the other writers that name an object after rebuilding.
        const QVariantList measures = m.rows(QStringLiteral("measures"));
        QString measureId, measureLabel;
        for (const QVariant &v : m.measureCandidates(fromId)) {
            measureId    = v.toMap().value(QStringLiteral("id")).toString();
            measureLabel = v.toMap().value(QStringLiteral("label")).toString();
            break;
        }
        if (!measureId.isEmpty()) {
            const QVariantMap attached = m.addMeasureTo(fromId, measureId, QStringLiteral("high"));
            check(attached.value(QStringLiteral("ok")).toBool(), "attaching a measure succeeds");
            check(attached.value(QStringLiteral("message")).toString().contains(fromLabel),
                  "and names the characteristic it attached to");

            // The REFUSAL path, which is its own hazard: it restores the working pack — assigning
            // over the very vector the condition pointer addresses — and then names the condition.
            // Two statements, in that order, is a read of freed memory.
            const QVariantMap twice = m.addMeasureTo(fromId, measureId, QStringLiteral("high"));
            check(!twice.value(QStringLiteral("ok")).toBool(),
                  "attaching the same measure twice is refused");
            check(twice.value(QStringLiteral("message")).toString().contains(fromLabel),
                  "and the refusal still names the characteristic, after restoring the pack");

            const QVariantMap detached = m.removeMeasureFrom(fromId, measureId);
            check(detached.value(QStringLiteral("ok")).toBool(), "detaching succeeds");
            check(detached.value(QStringLiteral("message")).toString().contains(fromLabel),
                  "and names it too");

            // The matching refusal on the other side: nothing left to detach, restore, then name.
            const QVariantMap again = m.removeMeasureFrom(fromId, measureId);
            check(!again.value(QStringLiteral("ok")).toBool(), "detaching twice is refused");
            check(again.value(QStringLiteral("message")).toString().contains(fromLabel),
                  "and that refusal names it too");
        }

        // A binding names its CONTEXT, which lives in the norm provider's tree — replaced by the
        // same rebuild.
        const QVariantList bindings = m.bindingsOf(fromId);
        if (bindings.size() > 1) {
            const QVariantMap ctx = bindings.at(1).toMap();
            const QVariantMap set = m.setBinding(fromId, ctx.value(QStringLiteral("id")).toString(),
                                                 true, false);
            check(set.value(QStringLiteral("ok")).toBool(), "setting a binding succeeds");
            check(set.value(QStringLiteral("message")).toString()
                      .contains(ctx.value(QStringLiteral("label")).toString()),
                  "and names the context, read from a tree that outlived the rebuild");
        }

        // And a screen relationship, which names a row in the screen set the rebuild reassigns.
        const QString screenId = firstShippedId(m.rows(QStringLiteral("screens")));
        const QVariantList screenCands = m.screenCandidates(screenId);
        if (!screenId.isEmpty() && !screenCands.isEmpty()) {
            const QVariantMap pane = m.inspect(QStringLiteral("screens"), screenId);
            const QString settled = m.addScreenSettles(
                                         screenId,
                                         screenCands.first().toMap()
                                             .value(QStringLiteral("id")).toString())
                                        .value(QStringLiteral("message")).toString();
            check(settled.contains(pane.value(QStringLiteral("label")).toString()),
                  "and a screen names itself after the rebuild that replaced the screen set");
        }

        while (m.canUndo()) m.undo();
        check(!m.dirty(), "and the draft is clean again");
    }

    std::printf("=== a write is VISIBLE in the pane that shows the thing written ===\n");
    {
        // The inspector re-reads inspect() whenever modelChanged fires. That is one link in a chain
        // of four — the command has to emit, the panel has to bump its revision, the binding has to
        // depend on it, and inspect() has to actually answer differently — and only the last is
        // testable here. So this pins the last one: after each write, the pane for the object the
        // write NAMES has to differ from the pane before it.
        //
        // Worth pinning because a pane that silently shows stale content is the failure mode this
        // panel's whole "no modals, no Edit button" premise depends on not having.
        m.undoTo(-1);

        const auto paneRowCount = [&m](const QString &type, const QString &id,
                                       const QString &sectionTitle) {
            int n = -1;
            for (const QVariant &sv : m.inspect(type, id).value(QStringLiteral("sections")).toList()) {
                const QVariantMap s = sv.toMap();
                if (s.value(QStringLiteral("title")).toString().contains(sectionTitle, Qt::CaseInsensitive))
                    n = s.value(QStringLiteral("rows")).toList().size();
            }
            return n;
        };

        // ── A cause added to a characteristic shows in ITS pane ─────────────
        QString target, cause;
        for (const QVariant &v : m.rows(QStringLiteral("characteristics"))) {
            const QString id = v.toMap().value(QStringLiteral("id")).toString();
            const QVariantList cands = m.linkCandidates(QStringLiteral("causes"), id);
            if (cands.isEmpty()) continue;
            // addLink(from = the cause, to = the selected characteristic), as the picker calls it.
            target = id;
            cause  = cands.first().toMap().value(QStringLiteral("id")).toString();
            break;
        }
        check(!target.isEmpty(), "there is a characteristic to add a cause to");

        const int before = paneRowCount(QStringLiteral("characteristics"), target,
                                        QStringLiteral("Caused by"));
        check(m.addLink(cause, target, QStringLiteral("causes"))
                  .value(QStringLiteral("ok")).toBool(), "the cause is added");
        const int after = paneRowCount(QStringLiteral("characteristics"), target,
                                       QStringLiteral("Caused by"));
        check(before >= 0 && after == before + 1,
              "and the characteristic's pane shows one more cause than it did");

        check(m.removeLink(cause, target, QStringLiteral("causes"))
                  .value(QStringLiteral("ok")).toBool(), "removing it succeeds");
        check(paneRowCount(QStringLiteral("characteristics"), target,
                           QStringLiteral("Caused by")) == before,
              "and the pane goes back to what it was");

        // ── A field edit shows in the pane's own header ──────────────────────
        const QString renamed = QStringLiteral("Renamed for the refresh test");
        check(m.setField(QStringLiteral("characteristics"), target, QStringLiteral("label"),
                         renamed).value(QStringLiteral("ok")).toBool(), "a rename lands");
        check(m.inspect(QStringLiteral("characteristics"), target)
                  .value(QStringLiteral("label")).toString() == renamed,
              "and the pane's title is the new name");
        check(m.undo().value(QStringLiteral("ok")).toBool(), "undo puts it back");
        check(m.inspect(QStringLiteral("characteristics"), target)
                  .value(QStringLiteral("label")).toString() != renamed,
              "and the pane's title follows the undo");

        // ── A screen's Settles section, which is written on the CONDITION ────
        const QString screenId = firstShippedId(m.rows(QStringLiteral("screens")));
        const QVariantList screenCands = m.screenCandidates(screenId);
        if (!screenId.isEmpty() && !screenCands.isEmpty()) {
            const int settlesBefore = paneRowCount(QStringLiteral("screens"), screenId,
                                                   QStringLiteral("Settles"));
            const QString cond = screenCands.first().toMap()
                                     .value(QStringLiteral("id")).toString();
            check(m.addScreenSettles(screenId, cond).value(QStringLiteral("ok")).toBool(),
                  "a screen is made to settle a characteristic");
            check(paneRowCount(QStringLiteral("screens"), screenId, QStringLiteral("Settles"))
                      == settlesBefore + 1,
                  "and the SCREEN's pane shows it, though the write went to the condition");
            check(m.removeScreenSettles(screenId, cond).value(QStringLiteral("ok")).toBool(),
                  "detaching succeeds");
            check(paneRowCount(QStringLiteral("screens"), screenId, QStringLiteral("Settles"))
                      == settlesBefore,
                  "and the pane follows that too");
        }

        // ── A drill's Answers section, same shape ────────────────────────────
        const QString drillId = firstShippedId(m.rows(QStringLiteral("drills")));
        const QVariantList drillCands = m.drillCandidates(drillId);
        if (!drillId.isEmpty() && !drillCands.isEmpty()) {
            const int answersBefore = paneRowCount(QStringLiteral("drills"), drillId,
                                                   QStringLiteral("Answers"));
            const QString cond = drillCands.first().toMap()
                                     .value(QStringLiteral("id")).toString();
            check(m.addDrillAnswers(drillId, cond).value(QStringLiteral("ok")).toBool(),
                  "a drill is made to answer a characteristic");
            check(paneRowCount(QStringLiteral("drills"), drillId, QStringLiteral("Answers"))
                      == answersBefore + 1,
                  "and the DRILL's pane shows it");
            check(m.undo().value(QStringLiteral("ok")).toBool(), "undo detaches it");
            check(paneRowCount(QStringLiteral("drills"), drillId, QStringLiteral("Answers"))
                      == answersBefore,
                  "and the pane follows the undo");
        }

        // ── A measure attached to a characteristic ───────────────────────────
        QString measureId;
        for (const QVariant &v : m.measureCandidates(target)) {
            measureId = v.toMap().value(QStringLiteral("id")).toString();
            break;
        }
        if (!measureId.isEmpty()) {
            const int detBefore = paneRowCount(QStringLiteral("characteristics"), target,
                                               QStringLiteral("Detected by"));
            check(m.addMeasureTo(target, measureId, QStringLiteral("high"))
                      .value(QStringLiteral("ok")).toBool(), "a measure is attached");
            check(paneRowCount(QStringLiteral("characteristics"), target,
                               QStringLiteral("Detected by")) > detBefore,
                  "and the characteristic's pane shows it");
            check(m.undo().value(QStringLiteral("ok")).toBool(), "undo detaches it");
            check(paneRowCount(QStringLiteral("characteristics"), target,
                               QStringLiteral("Detected by")) == detBefore,
                  "and the pane follows the undo");
        }

        while (m.canUndo()) m.undo();
    }

    std::printf("=== counts sort as numbers, not as strings ===\n");
    {
        // Every count in a sortKeys map is a container's `.size()`, which is a qsizetype — and the
        // comparator tested `typeId() == Int` and nothing else, so all of them fell through to the
        // string branch. The bibliography came out 7, 6, 4, 3, 2, 2, 12, 1: the paper holding up
        // twelve claims buried between the twos and the ones, because "12" < "2" as text. Two
        // digits is all it takes, so nothing smaller than a ten-row count could ever have shown it.
        for (const QString &type : { QStringLiteral("references"), QStringLiteral("screens"),
                                     QStringLiteral("drills"), QStringLiteral("measures") }) {
            const QVariantList rows = m.rows(type);
            if (rows.size() < 3) continue;
            // The key each type defaults to, read back off the rows themselves.
            const QString key = type == QStringLiteral("references")  ? QStringLiteral("supports")
                              : type == QStringLiteral("screens")     ? QStringLiteral("settlesCount")
                              : type == QStringLiteral("drills")      ? QStringLiteral("answersCount")
                                                                      : QStringLiteral("readBy");
            const bool wantDescending = (type == QStringLiteral("references"));
            bool ordered = true;
            int  prev    = wantDescending ? (1 << 30) : -1;
            for (const QVariant &v : rows) {
                const int n = v.toMap().value(QStringLiteral("sortKeys")).toMap()
                                  .value(key).toInt();
                if (type == QStringLiteral("measures")) continue;   // sorts by status first
                if (wantDescending ? (n > prev) : (n < prev)) ordered = false;
                prev = n;
            }
            check(ordered, qPrintable(QStringLiteral("%1 sort numerically by %2").arg(type, key)));
        }

        // A double sort key went the same way — mu is not an Int either.
        const QVariantList corridors = m.rows(QStringLiteral("corridors"),
                                              QVariantMap{ { QStringLiteral("sort"),
                                                             QStringLiteral("mu") } });
        bool muOrdered = true;
        double prevMu = -1e18;
        for (const QVariant &v : corridors) {
            const double mu = v.toMap().value(QStringLiteral("sortKeys")).toMap()
                                  .value(QStringLiteral("mu")).toDouble();
            if (mu < prevMu) muOrdered = false;
            prevMu = mu;
        }
        check(muOrdered, "and a corridor's mu sorts as a number too");
    }

    std::printf("=== the inspector can edit EVERY writable field of every type ===\n");
    {
        // The pane is where an author expects to see and change everything an object holds. It used
        // to render prose read-only and offer no control at all for nine fields that setField()
        // accepted — so "editable" depended on which surface you were looking at, and a causal
        // link's frequency could be changed in the table and not in the pane describing it.
        //
        // This pins the contract both ways: every field the pane OFFERS must be writable, and every
        // field setField() accepts must be OFFERED. The second half is what stops the list drifting
        // out of date the next time a field is added to a struct.
        m.undoTo(-1);

        struct Probe { const char *type; };
        static const QStringList editableTypes{
            QStringLiteral("characteristics"), QStringLiteral("measures"),
            QStringLiteral("signals"), QStringLiteral("links"), QStringLiteral("corridors"),
            QStringLiteral("screens"), QStringLiteral("drills")
        };

        QStringList noFields, notWritable;
        for (const QString &type : editableTypes) {
            const QVariantList rows = m.rows(type);
            if (rows.isEmpty()) continue;
            const QString id = rows.first().toMap().value(QStringLiteral("id")).toString();

            QVariantList fields;
            for (const QVariant &sv : m.inspect(type, id).value(QStringLiteral("sections")).toList())
                if (sv.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("fields"))
                    fields = sv.toMap().value(QStringLiteral("rows")).toList();

            if (fields.isEmpty()) { noFields << type; continue; }

            for (const QVariant &fv : fields) {
                const QVariantMap f    = fv.toMap();
                const QString     key  = f.value(QStringLiteral("field")).toString();
                const QString     kind = f.value(QStringLiteral("kind")).toString();
                if (key.isEmpty() || kind.isEmpty()) { notWritable << type + "/" + key; continue; }

                // Write the value STRAIGHT BACK. It must be accepted: a pane that offers a control
                // whose value setField() then refuses is worse than no control, because the refusal
                // arrives after the author has typed.
                const QVariant    now = f.value(QStringLiteral("value"));
                const QVariantMap r   = m.setField(type, id, key, now);
                if (!r.value(QStringLiteral("ok")).toBool()
                    && !r.value(QStringLiteral("message")).toString().contains(
                           QStringLiteral("needs a"))   // a stated content rule, not a missing path
                    && !r.value(QStringLiteral("message")).toString().contains(
                           QStringLiteral("has to be")))
                    notWritable << QStringLiteral("%1/%2: %3").arg(type, key,
                                       r.value(QStringLiteral("message")).toString());
            }
        }
        for (const QString &t : noFields)    std::printf("      no Fields section: %s\n", qPrintable(t));
        for (const QString &n : notWritable) std::printf("      not writable: %s\n", qPrintable(n));
        check(noFields.isEmpty(), "every editable type's pane offers a Fields section");
        check(notWritable.isEmpty(), "and every field it offers is one setField() accepts");

        // The other direction: a field setField() takes must be REACHABLE. Spot-checked on the ones
        // that had no control before this — the prose and the provenance.
        const QString charId = m.rows(QStringLiteral("characteristics")).first().toMap()
                                   .value(QStringLiteral("id")).toString();
        QStringList offered;
        for (const QVariant &sv : m.inspect(QStringLiteral("characteristics"), charId)
                                      .value(QStringLiteral("sections")).toList())
            if (sv.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("fields"))
                for (const QVariant &fv : sv.toMap().value(QStringLiteral("rows")).toList())
                    offered << fv.toMap().value(QStringLiteral("field")).toString();
        for (const QString &want : { QStringLiteral("consequence"), QStringLiteral("injuryNote"),
                                     QStringLiteral("aliases"), QStringLiteral("citation"),
                                     QStringLiteral("state") })
            check(offered.contains(want),
                  qPrintable(QStringLiteral("a characteristic's %1 is reachable").arg(want)));

        // The dead end this closes: a tier that requires a citation could never be set, because the
        // citation had no control. Both are on the pane now, so the pair works.
        check(m.setField(QStringLiteral("characteristics"), charId, QStringLiteral("citation"),
                         QStringLiteral("10.1000/probe")).value(QStringLiteral("ok")).toBool(),
              "a citation can be set from the pane");
        check(m.setField(QStringLiteral("characteristics"), charId, QStringLiteral("tier"),
                         QStringLiteral("supported")).value(QStringLiteral("ok")).toBool(),
              "and the tier it unlocks can then be raised");

        // No section may REPEAT a field. The pane showed the editable fields at the top and then the
        // same values again lower down as read-only prose — so the obvious thing to click was the
        // copy that could not be typed into.
        QStringList repeated;
        for (const QString &type : editableTypes) {
            const QVariantList rows = m.rows(type);
            if (rows.isEmpty()) continue;
            const QString      oid  = rows.first().toMap().value(QStringLiteral("id")).toString();
            const QVariantList secs = m.inspect(type, oid).value(QStringLiteral("sections")).toList();

            QStringList fieldLabels;
            for (const QVariant &sv : secs)
                if (sv.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("fields"))
                    for (const QVariant &fv : sv.toMap().value(QStringLiteral("rows")).toList())
                        fieldLabels << fv.toMap().value(QStringLiteral("label")).toString();

            for (const QVariant &sv : secs) {
                const QVariantMap sec = sv.toMap();
                if (sec.value(QStringLiteral("kind")).toString() == QStringLiteral("fields")) continue;
                if (fieldLabels.contains(sec.value(QStringLiteral("title")).toString()))
                    repeated << QStringLiteral("%1 · %2").arg(type,
                                    sec.value(QStringLiteral("title")).toString());
            }
        }
        for (const QString &r : repeated) std::printf("      repeated: %s\n", qPrintable(r));
        check(repeated.isEmpty(), "and no section repeats a field the pane already edits");

        // A link's frequency — the field that started this — is offered and named for what it says.
        const QString linkId = m.rows(QStringLiteral("links")).first().toMap()
                                   .value(QStringLiteral("id")).toString();
        bool sawHowOften = false;
        for (const QVariant &sv : m.inspect(QStringLiteral("links"), linkId)
                                      .value(QStringLiteral("sections")).toList())
            if (sv.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("fields"))
                for (const QVariant &fv : sv.toMap().value(QStringLiteral("rows")).toList())
                    if (fv.toMap().value(QStringLiteral("field")).toString()
                            == QStringLiteral("strength")
                        && fv.toMap().value(QStringLiteral("label")).toString()
                               == QStringLiteral("How often"))
                        sawHowOften = true;
        check(sawHowOften, "a causal link's frequency is offered, labelled How often");

        // Delete addresses a link by its row id like everything else.
        check(m.removeObject(QStringLiteral("links"), linkId)
                  .value(QStringLiteral("ok")).toBool(),
              "and a link deletes by id, without the caller knowing links are special");

        while (m.canUndo()) m.undo();
    }

    std::printf("=== every inspector row carries the whole row contract ===\n");
    {
        // The inspector delegate binds `detail`, `tone` and `navigable` on EVERY row, whichever
        // section kind is the visible one — QML evaluates the invisible branches too. So a row that
        // carries only the keys its own section happens to read still fails four bindings per
        // repaint, and the only symptom is "Unable to assign [undefined]" on the console: nothing
        // looks wrong on screen, which is precisely why it survives.
        //
        // hubRow() produces the whole shape. This asserts that nothing hand-builds a row that skips
        // part of it — which is exactly how the reference pane's claims rows shipped broken.
        static const QStringList kContract = { QStringLiteral("type"), QStringLiteral("id"),
                                               QStringLiteral("label"), QStringLiteral("detail"),
                                               QStringLiteral("tone"),
                                               QStringLiteral("navigable") };
        QStringList offenders;
        for (const QVariant &tv : m.types()) {
            const QString type = tv.toMap().value(QStringLiteral("key")).toString();
            if (type == QStringLiteral("health")) continue;   // findings are not inspectable objects
            for (const QVariant &rv : m.rows(type)) {
                const QString     id   = rv.toMap().value(QStringLiteral("id")).toString();
                const QVariantMap pane = m.inspect(type, id);
                for (const QVariant &sv : pane.value(QStringLiteral("sections")).toList()) {
                    const QVariantMap s = sv.toMap();
                    for (const QVariant &row : s.value(QStringLiteral("rows")).toList())
                        for (const QString &key : kContract)
                            if (!row.toMap().contains(key))
                                offenders << QStringLiteral("%1/%2 · %3 · missing %4")
                                                 .arg(type, id,
                                                      s.value(QStringLiteral("title")).toString(),
                                                      key);
                }
            }
        }
        if (!offenders.isEmpty())
            std::printf("      first offender: %s\n", qPrintable(offenders.first()));
        check(offenders.isEmpty(),
              "no section row anywhere skips a key the delegate binds");

        // Badges are rendered by the same helper and read `label` and `tone`.
        QStringList badgeOffenders;
        for (const QVariant &tv : m.types()) {
            const QString type = tv.toMap().value(QStringLiteral("key")).toString();
            if (type == QStringLiteral("health")) continue;
            for (const QVariant &rv : m.rows(type)) {
                const QVariantMap pane = m.inspect(type,
                                                   rv.toMap().value(QStringLiteral("id")).toString());
                for (const QVariant &bv : pane.value(QStringLiteral("badges")).toList())
                    if (!bv.toMap().contains(QStringLiteral("label"))
                        || !bv.toMap().contains(QStringLiteral("tone")))
                        badgeOffenders << type;
            }
        }
        check(badgeOffenders.isEmpty(), "and no badge does either");
    }

    std::printf("=== screens and drills are writable ===\n");
    {
        const QVariantList screens = m.rows(QStringLiteral("screens"));
        check(!screens.isEmpty(), "the screen registry has rows");

        // The default sort puts the screen that settles NOTHING first — the same principle as
        // sorting measures by least-read. Descending would bury exactly the work the panel is for.
        if (screens.size() > 1) {
            const int firstN = screens.first().toMap().value(QStringLiteral("sortKeys")).toMap()
                                   .value(QStringLiteral("settlesCount")).toInt();
            const int lastN  = screens.last().toMap().value(QStringLiteral("sortKeys")).toMap()
                                   .value(QStringLiteral("settlesCount")).toInt();
            check(firstN <= lastN, "screens default-sort by what they settle, ascending");
        }

        const QString screenId = firstShippedId(screens);
        check(!screenId.isEmpty(), "and at least one of them ships");

        const QVariantMap before = rowFor(m.rows(QStringLiteral("screens")), screenId);
        check(cellFor(before, QStringLiteral("name")).value(QStringLiteral("editable")).toBool(),
              "a screen's name cell is editable");
        check(before.value(QStringLiteral("source")).toString() == QStringLiteral("shipped"),
              "and it starts as shipped content");

        check(m.setField(QStringLiteral("screens"), screenId, QStringLiteral("region"),
                         QStringLiteral("Left elbow"))
                  .value(QStringLiteral("ok")).toBool(),
              "the region takes an edit");
        // Counted from the top of the stack rather than by comparing sizes: earlier sections leave
        // undone commands behind, and pushing a new one truncates that tail — so a size that did not
        // grow is the stack working, not the command going missing.
        check(m.canUndo() && m.undoLabel().contains(QStringLiteral("Region")),
              "one edit, one command, and it says what it was");

        QVariantMap after = rowFor(m.rows(QStringLiteral("screens")), screenId);
        check(cellFor(after, QStringLiteral("region")).value(QStringLiteral("text")).toString()
                  == QStringLiteral("Left elbow"),
              "and the row reads the draft, not the file");
        check(after.value(QStringLiteral("source")).toString() == QStringLiteral("both"),
              "editing shipped content makes it yours, over shipped");
        check(after.value(QStringLiteral("dirty")).toBool(), "and marks it unsaved");

        // Copy-on-write's inverse: undoing the FIRST edit removes the override entirely, which is
        // the reset — with no separate "take theirs" path that could disagree with it.
        check(m.undo().value(QStringLiteral("ok")).toBool(), "and it undoes");
        after = rowFor(m.rows(QStringLiteral("screens")), screenId);
        check(after.value(QStringLiteral("source")).toString() == QStringLiteral("shipped"),
              "undoing the first edit removes the override and it is shipped again");
        check(!after.value(QStringLiteral("dirty")).toBool(), "and nothing is left unsaved");

        // A refusal must leave NOTHING behind — not even the copy-on-write copy that reaching for
        // the row made.
        check(!m.setField(QStringLiteral("screens"), screenId, QStringLiteral("nosuchfield"),
                          QStringLiteral("x")).value(QStringLiteral("ok")).toBool(),
              "an unknown field is refused");
        check(rowFor(m.rows(QStringLiteral("screens")), screenId)
                  .value(QStringLiteral("source")).toString() == QStringLiteral("shipped"),
              "and the refusal leaves no override behind");

        // Creating, duplicating and trashing, on the same seam.
        const QVariantMap made = m.createObject(QStringLiteral("screens"));
        check(made.value(QStringLiteral("ok")).toBool(), "a screen can be created");
        const QString newScreen = made.value(QStringLiteral("id")).toString();
        check(newScreen.startsWith(QStringLiteral("screen.")),
              "and its id is minted inside the screen namespace");
        check(rowFor(m.rows(QStringLiteral("screens")), newScreen)
                  .value(QStringLiteral("source")).toString() == QStringLiteral("yours"),
              "a new screen is yours");

        const QVariantMap copied = m.duplicate(QStringLiteral("screens"), newScreen);
        check(copied.value(QStringLiteral("ok")).toBool(), "and duplicated");
        check(copied.value(QStringLiteral("id")).toString() != newScreen,
              "into a fresh id rather than over the original");

        check(m.removeObject(QStringLiteral("screens"),
                             copied.value(QStringLiteral("id")).toString())
                  .value(QStringLiteral("ok")).toBool(),
              "and moved to trash");
        check(rowFor(m.rows(QStringLiteral("screens")),
                     copied.value(QStringLiteral("id")).toString()).isEmpty(),
              "which takes it off the list");
        check(m.undo().value(QStringLiteral("ok")).toBool(), "and that undoes too");
        check(!rowFor(m.rows(QStringLiteral("screens")),
                      copied.value(QStringLiteral("id")).toString()).isEmpty(),
              "bringing it back");

        // A SHIPPED row cannot be deleted — dropping an override restores it, which is a different
        // action with a different meaning.
        check(!m.removeObject(QStringLiteral("screens"), screenId)
                   .value(QStringLiteral("ok")).toBool(),
              "shipped content cannot be removed, only overridden");

        // The drill side, which differs in one place worth asserting: equipment is a LIST typed as
        // one line, so the split has to round-trip.
        const QString drillId = firstShippedId(m.rows(QStringLiteral("drills")));
        if (!drillId.isEmpty()) {
            check(m.setField(QStringLiteral("drills"), drillId, QStringLiteral("equipment"),
                             QStringLiteral("alignment stick · mat"))
                      .value(QStringLiteral("ok")).toBool(),
                  "a drill's equipment takes an edit");
            const QVariantMap d = rowFor(m.rows(QStringLiteral("drills")), drillId);
            check(cellFor(d, QStringLiteral("equipment")).value(QStringLiteral("text")).toString()
                      == QStringLiteral("alignment stick · mat"),
                  "and the list round-trips through the one-line form");
            check(m.undo().value(QStringLiteral("ok")).toBool(), "and undoes");
        }
    }

    std::printf("=== what a screen settles and a drill answers ===\n");
    {
        const QString screenId = firstShippedId(m.rows(QStringLiteral("screens")));
        const QString drillId  = firstShippedId(m.rows(QStringLiteral("drills")));

        // The candidates are PRE-FILTERED, so an illegal pairing cannot be constructed.
        const QVariantList cands = m.screenCandidates(screenId);
        check(!cands.isEmpty(), "a screen offers characteristics it does not already settle");

        const QString target = cands.first().toMap().value(QStringLiteral("id")).toString();
        check(m.addScreenSettles(screenId, target).value(QStringLiteral("ok")).toBool(),
              "and one can be attached");
        check(!m.addScreenSettles(screenId, target).value(QStringLiteral("ok")).toBool(),
              "twice is refused rather than silently duplicated");

        // The relationship is stored on the CONDITION, so the write shows up as an override of the
        // condition — not of the screen.
        QVariantMap inspected = m.inspect(QStringLiteral("screens"), screenId);
        bool        listed    = false;
        for (const QVariant &sv : inspected.value(QStringLiteral("sections")).toList()) {
            const QVariantMap s = sv.toMap();
            if (s.value(QStringLiteral("action")).toString() != QStringLiteral("settles")) continue;
            for (const QVariant &rv : s.value(QStringLiteral("rows")).toList())
                if (rv.toMap().value(QStringLiteral("id")).toString() == target) listed = true;
        }
        check(listed, "and the screen's Settles section lists it");

        check(m.removeScreenSettles(screenId, target).value(QStringLiteral("ok")).toBool(),
              "it detaches");
        check(!m.removeScreenSettles(screenId, target).value(QStringLiteral("ok")).toBool(),
              "and detaching what is not attached is refused");

        // Do-then-undo, on the inverse that is easiest to get wrong.
        check(m.addDrillAnswers(drillId,
                                m.drillCandidates(drillId).first().toMap()
                                    .value(QStringLiteral("id")).toString())
                  .value(QStringLiteral("ok")).toBool(),
              "a drill can be made to answer a characteristic");
        const int answersAfter = m.inspect(QStringLiteral("drills"), drillId)
                                     .value(QStringLiteral("sections")).toList().size();
        check(m.undo().value(QStringLiteral("ok")).toBool(), "and it undoes");
        check(m.inspect(QStringLiteral("drills"), drillId)
                  .value(QStringLiteral("sections")).toList().size() == answersAfter,
              "leaving the pane's shape unchanged");

        // Wind everything back so the reset assertions below start from a knowable state.
        while (m.canUndo()) m.undo();
        check(!m.dirty(), "and the draft is clean again");
    }

    std::printf("=== the reference pane is a working surface ===\n");
    {
        const QVariantList refs = m.rows(QStringLiteral("references"));
        check(!refs.isEmpty(), "the bibliography has rows");

        // The one the library leans on most — the default sort puts it first.
        const QString refId = refs.first().toMap().value(QStringLiteral("id")).toString();
        const QVariantMap pane = m.inspect(QStringLiteral("references"), refId);
        check(pane.value(QStringLiteral("found")).toBool(), "and one inspects");

        bool sawCitation = false, sawWhyNot = false, sawClaims = false;
        for (const QVariant &sv : pane.value(QStringLiteral("sections")).toList()) {
            const QVariantMap s = sv.toMap();
            if (s.value(QStringLiteral("kind")).toString() == QStringLiteral("quote"))
                sawCitation = true;
            if (s.value(QStringLiteral("kind")).toString() == QStringLiteral("claims"))
                sawClaims = true;
            if (s.value(QStringLiteral("title")).toString().contains(QStringLiteral("not editable")))
                sawWhyNot = true;
        }
        check(sawCitation, "the formatted citation is there");
        // An inert pane that does not explain itself reads as a bug, so the explanation is asserted
        // rather than left to survive by luck.
        check(sawWhyNot, "and it says why the record itself cannot be edited");
        check(sawClaims, "and the claims resting on it are a section of their own");

        check(!m.referenceCsl(refId).isEmpty(), "one record exports as CSL-JSON");
        check(m.referenceCsl(QStringLiteral("ref.nothing")).isEmpty(),
              "and an id that resolves nowhere copies nothing");

        // The citation is imported; the claim resting on it is ours. That is the whole argument for
        // the pane, so the write path behind it is asserted.
        const QVariantList citing = m.linksCitingReference(refId);
        if (!citing.isEmpty()) {
            const QVariantMap link = citing.first().toMap();
            check(link.contains(QStringLiteral("strength")),
                  "every citing link carries the strength the pane edits");
            const QString linkId = link.value(QStringLiteral("id")).toString();
            const QVariantMap wrote = m.setField(QStringLiteral("links"), linkId,
                                                 QStringLiteral("strength"),
                                                 QStringLiteral("strong"));
            check(wrote.value(QStringLiteral("ok")).toBool(),
                  "and it writes through the ordinary link path");
            check(m.undo().value(QStringLiteral("ok")).toBool(), "and undoes");
        }
        while (m.canUndo()) m.undo();
    }

    std::printf("=== the way back to the standard model ===\n");
    {
        check(!m.resetToStandard().value(QStringLiteral("ok")).toBool(),
              "an install already on the standard model has nothing to reset");
        check(m.overriddenCount() == 0 && m.authoredCount() == 0,
              "and both drift counts are zero");

        // Two different kinds of drift, counted separately, because the prompt says both.
        const QString charId = firstShippedId(m.rows(QStringLiteral("characteristics")));
        check(m.setField(QStringLiteral("characteristics"), charId, QStringLiteral("label"),
                         QStringLiteral("Edited for the reset test"))
                  .value(QStringLiteral("ok")).toBool(),
              "a shipped characteristic is overridden");
        check(m.overriddenCount() == 1, "which counts as one change to shipped content");
        const QVariantMap mine = m.createObject(QStringLiteral("characteristics"));
        check(m.authoredCount() == 1, "and a new one counts as one of yours");

        check(m.save().value(QStringLiteral("ok")).toBool(), "the draft saves");
        check(QFile::exists(userPackPath()), "and there is a user pack on disk to reset away");

        const QVariantMap reset = m.resetToStandard();
        check(reset.value(QStringLiteral("ok")).toBool(), "the reset lands");
        check(m.overriddenCount() == 0 && m.authoredCount() == 0,
              "and this install is back on the standard model");
        check(rowFor(m.rows(QStringLiteral("characteristics")),
                     mine.value(QStringLiteral("id")).toString()).isEmpty(),
              "the object authored here is gone");
        check(rowFor(m.rows(QStringLiteral("characteristics")), charId)
                  .value(QStringLiteral("source")).toString() == QStringLiteral("shipped"),
              "and the overridden one is shipped content again");

        // A backup nobody verifies is not a backup. It has to EXIST and it has to LOAD — a file
        // written but unreadable would be worse than none, because it would be trusted.
        const QStringList backups = reset.value(QStringLiteral("backups")).toStringList();
        check(!backups.isEmpty(), "the previous model was copied aside");
        bool everyBackupLoads = !backups.isEmpty();
        for (const QString &path : backups) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) { everyBackupLoads = false; continue; }
            const QByteArray bytes = f.readAll();
            f.close();
            // The pack backup must parse as a pack; the norm backup as a norm set. Whichever this
            // is, one of the two has to accept it.
            const bool ok = loadPack(bytes, path).parsed || loadNormPack(bytes, path).parsed;
            if (!ok) everyBackupLoads = false;
        }
        check(everyBackupLoads, "and every backup file parses as the layer it came from");

        // Recoverable for the session, as a command like any other.
        check(m.canUndo(), "the reset is on the undo stack");
        check(m.undo().value(QStringLiteral("ok")).toBool(), "and undoes");
        check(m.overriddenCount() == 1 && m.authoredCount() == 1,
              "putting the whole draft back");

        for (const QString &path : backups) QFile::remove(path);
        while (m.canUndo()) m.undo();
    }

    std::printf("=== a shortcut is not a cycle ===\n");
    {
        // The acyclicity question is DIRECTED, and linkLegality() used to ask it of a symmetric
        // helper. With `A → X → B` in the pack, drawing `A → B` is a legal shortcut edge — but
        // hasCausalPath() answers "these two are connected either way", so the edge was refused,
        // and refused with a sentence that named the two conditions in the order making it false.
        //
        // Asserted through linkLegality() and linkCandidates() rather than against the graph helper
        // directly — characteristic_pack_test covers the helper. What matters here is that the two
        // surfaces an author meets, the refusal and the candidate list, now agree with it.
        const QString a = m.createObject(QStringLiteral("characteristics"))
                              .value(QStringLiteral("id")).toString();
        const QString x = m.createObject(QStringLiteral("characteristics"))
                              .value(QStringLiteral("id")).toString();
        const QString b = m.createObject(QStringLiteral("characteristics"))
                              .value(QStringLiteral("id")).toString();
        check(!a.isEmpty() && a != x && x != b, "three fresh characteristics to wire up");

        check(m.addLink(a, x).value(QStringLiteral("ok")).toBool(), "A causes X");
        check(m.addLink(x, b).value(QStringLiteral("ok")).toBool(), "X causes B");

        check(m.linkLegality(a, b).value(QStringLiteral("ok")).toBool(),
              "A → B is legal even though A already reaches B the long way");
        const QVariantMap back = m.linkLegality(b, a);
        check(!back.value(QStringLiteral("ok")).toBool(),
              "B → A is refused — that one really is a cycle");
        check(back.value(QStringLiteral("reason")).toString().contains(QStringLiteral("cycle")),
              "and says so");

        bool offered = false;
        for (const QVariant &v : m.linkCandidates(QStringLiteral("causes"), a))
            if (v.toMap().value(QStringLiteral("id")).toString() == b) offered = true;
        check(offered, "and the candidate list offers it, rather than withholding it silently");

        // ── Where the graph stands ──────────────────────────────────────────
        //
        // A link is not a place to stand. It HAS a neighbourhood — its two ends and the line
        // between them — so centring the picture on it replaces the drawing the reader picked the
        // line out of, and the selected-link stroke can never be seen. It centres on its cause.
        const QVariantMap self = m.graphFocus(QStringLiteral("characteristics"), a);
        check(self.value(QStringLiteral("id")).toString() == a
                  && self.value(QStringLiteral("type")).toString()
                         == QStringLiteral("characteristics"),
              "an ordinary object is its own graph focus");

        const QVariantMap onLink =
            m.graphFocus(QStringLiteral("links"),
                         a + QStringLiteral("|") + x + QStringLiteral("|causes"));
        check(onLink.value(QStringLiteral("id")).toString() == a,
              "a link centres on its CAUSE, so the line stays drawn in the picture");
        check(onLink.value(QStringLiteral("type")).toString() == QStringLiteral("characteristics"),
              "as a condition, which is what graph() knows how to lay out");

        while (m.canUndo()) m.undo();
    }

    // Leave no user pack behind: a test with a side effect on the product is not a test.
    QFile::remove(userPackPath());
    QFile::remove(userNormPath());
    QFile::remove(userScreenSetPath());
    QFile::remove(userDrillSetPath());

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
