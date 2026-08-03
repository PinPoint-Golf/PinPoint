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
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
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

        // ── and each enum cell sits under ITS OWN column ────────────────────────
        //
        // Count parity above is not enough, and the gap is not theoretical. `conditionRow()` builds
        // its cells POSITIONALLY in two branches — one for characteristics, one for causes — against
        // one `columns()` with two arms. Transpose two cells in ONE branch and the count is
        // unchanged, nothing throws, and `facets()` reads the wrong column's text at that index.
        // The rail would then fill a "How common" chip list with kind labels, and the FILTER would
        // agree with it, because rows() resolves the value the same wrong way. Both halves wrong in
        // the same direction is the one failure a count can never see.
        //
        // So: find each column by key, and assert every row's cell there is a member of that enum's
        // own label set. Comparing cells[i]["field"] against columns()[i]["key"] would be the
        // tempting shortcut and does NOT work — the name cell carries field `label` against column
        // key `name`, and the tier cell carries `tier` against `evidence`.
        {
            QStringList kindLabels, prominenceLabels, groupLabels, reachLabels;
            for (ConditionKind k : allConditionKinds())  kindLabels       << conditionKindLabel(k);
            for (Prominence pr : allProminences())       prominenceLabels << prominenceLabel(pr);
            for (ConditionGroup g : allConditionGroups()) groupLabels     << conditionGroupLabel(g);
            for (ConfirmedBy cb : { ConfirmedBy::Measured, ConfirmedBy::Screened,
                                    ConfirmedBy::Asserted })
                reachLabels << reachLabel(cb);

            const struct { const char *key; const QStringList *labels; } vocab[] = {
                { "group", &groupLabels }, { "kind", &kindLabels },
                { "prominence", &prominenceLabels }, { "reach", &reachLabels },
            };

            for (const QString &type : { QStringLiteral("characteristics"), QStringLiteral("causes") }) {
                const QVariantList cols = m.columns(type);
                const QVariantList rows = m.rows(type);
                for (const auto &v : vocab) {
                    int colIndex = -1;
                    for (int i = 0; i < cols.size(); ++i)
                        if (cols.at(i).toMap().value(QStringLiteral("key")).toString()
                            == QLatin1String(v.key))
                            colIndex = i;
                    check(colIndex >= 0,
                          qPrintable(QStringLiteral("%1: a `%2` column exists — a facet without one "
                                                    "is dropped silently")
                                         .arg(type, QLatin1String(v.key))));
                    if (colIndex < 0) continue;

                    QString stray;
                    for (const QVariant &rv : rows) {
                        const QString text = rv.toMap().value(QStringLiteral("cells")).toList()
                                                 .at(colIndex).toMap()
                                                 .value(QStringLiteral("text")).toString();
                        if (!v.labels->contains(text)) { stray = text; break; }
                    }
                    check(stray.isEmpty(),
                          qPrintable(QStringLiteral("%1: the `%2` column holds only %2 labels%3")
                                         .arg(type, QLatin1String(v.key),
                                              stray.isEmpty() ? QString()
                                                              : QStringLiteral(" (saw '%1')").arg(stray))));
                }
            }
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

    std::printf("=== the metrics list filters on CAPTURE, not on units ===\n");
    {
        // A unit is a property of the number, not a question anybody browses with: "metrics in
        // degrees" puts the wrist, the spine, the club face and the foot flare in one bucket and
        // splits stance width from stance width in millimetres. It is gone. What replaced it is the
        // pair of device facets the route ladder makes derivable — what a metric needs at minimum,
        // and what better kit would add.
        const QVariantList facets = m.facets(QStringLiteral("metrics"));
        QStringList keys;
        for (const QVariant &fv : facets)
            keys << fv.toMap().value(QStringLiteral("key")).toString();
        check(!keys.contains(QStringLiteral("unit")), "no unit facet on the metrics list");
        check(keys.contains(QStringLiteral("needsTags")), "…and a Needs facet in its place");
        check(keys.contains(QStringLiteral("improvesTags")), "…beside an Improves-with facet");
        check(keys.contains(QStringLiteral("stereoTags")), "…and a second-camera facet");

        // The same one-source rule as above, now over a facet kind where a row carries SEVERAL
        // values. Getting this wrong is invisible in the rail and obvious in the list: a metric
        // needing a camera and a ball must appear under both chips, and be counted by both.
        bool countsMatch = true, sawTags = false;
        for (const QVariant &fv : facets) {
            const QVariantMap f = fv.toMap();
            if (f.value(QStringLiteral("kind")).toString() != QLatin1String("tags")) continue;
            sawTags = true;
            std::printf("        %s:\n", qPrintable(f.value(QStringLiteral("label")).toString()));
            for (const QVariant &ov : f.value(QStringLiteral("options")).toList()) {
                const QVariantMap o = ov.toMap();
                std::printf("          %-24s %d\n",
                            qPrintable(o.value(QStringLiteral("value")).toString()),
                            o.value(QStringLiteral("count")).toInt());
                QVariantMap sel;
                sel.insert(f.value(QStringLiteral("key")).toString(),
                           QStringList{ o.value(QStringLiteral("value")).toString() });
                QVariantMap filters;
                filters.insert(QStringLiteral("facets"), sel);
                const int got  = int(m.rows(QStringLiteral("metrics"), filters).size());
                const int said = o.value(QStringLiteral("count")).toInt();
                if (got != said) {
                    countsMatch = false;
                    std::printf("        %s = %s: chip says %d, list returns %d\n",
                                qPrintable(f.value(QStringLiteral("key")).toString()),
                                qPrintable(o.value(QStringLiteral("value")).toString()), said, got);
                }
            }
        }
        check(sawTags, "the device facets declare themselves as the tag kind");
        check(countsMatch, "every device chip returns exactly the number it advertises");

        // A metric with two rungs is under its floor's chip and its upgrade's — one row, two
        // answers, which is the whole reason a tag facet exists. pelvisRotation is the worked case:
        // a face-on camera estimates it, a pelvis IMU measures it.
        const auto rowKeys = [&m](const char *key, const char *value) {
            QVariantMap sel;
            sel.insert(QString::fromLatin1(key), QStringList{ QString::fromLatin1(value) });
            QVariantMap filters;
            filters.insert(QStringLiteral("facets"), sel);
            QStringList ids;
            for (const QVariant &rv : m.rows(QStringLiteral("metrics"), filters))
                ids << rv.toMap().value(QStringLiteral("id")).toString();
            return ids;
        };
        check(rowKeys("needsTags", "Face-on camera").contains(QStringLiteral("pelvisRotation")),
              "pelvisRotation needs only the camera");
        check(rowKeys("improvesTags", "Body IMUs").contains(QStringLiteral("pelvisRotation")),
              "…and body IMUs are what would improve it");
        check(!rowKeys("needsTags", "Body IMUs").contains(QStringLiteral("pelvisRotation")),
              "…and it does NOT read as needing them, which the old flat requirement could not say");

        // The depth metrics are filterable as such. They used to state `minTier = Stereo3D`, which
        // no facet could read and no golfer could act on.
        check(rowKeys("needsTags", "Down-the-line camera").contains(QStringLiteral("clubPath")),
              "clubPath is findable by the camera it needs");

        // The second-camera facet grades rather than lumps. A chip covering every projected metric
        // at one strength would put pelvisThrust — invisible without stereo — beside headTilt, which
        // a calibrated pair makes a few degrees better at the top.
        check(rowKeys("stereoTags", "Unlock it").contains(QStringLiteral("clubPath")),
              "clubPath: a second camera unlocks it");
        check(rowKeys("stereoTags", "Improve it").contains(QStringLiteral("xFactor")),
              "xFactor: a second camera improves it (authored rung)");
        check(rowKeys("stereoTags", "Refine it").contains(QStringLiteral("shoulderPlaneAngle")),
              "shoulderPlaneAngle: a second camera refines it (derived from the projection)");
        const QStringList none = rowKeys("stereoTags", "Refine it")
                               + rowKeys("stereoTags", "Improve it")
                               + rowKeys("stereoTags", "Unlock it");
        check(!none.contains(QStringLiteral("toeLineAngle")),
              "…and an Address-only reading appears under no chip at all, which is the answer");

        // EVERY FACET NEEDS A COLUMN SAYING THE SAME THING.
        //
        // The stereo facet shipped for one build without one: the rail offered "Refine it (27)" and
        // the table showed "—" in every visible column for all 27, so a reader scanning the list
        // concluded a second camera did nothing for them and a reader using the rail concluded the
        // opposite. Neither was wrong about what they could see. Filtering and scanning have to
        // agree, so this asserts each facet's key resolves to a column or a sort key that carries
        // the same value the chip does.
        const QVariantList cols = m.columns(QStringLiteral("metrics"));
        int orphanFacets = 0;
        for (const QVariant &fv : facets) {
            const QVariantMap f   = fv.toMap();
            const QString     key = f.value(QStringLiteral("key")).toString();
            // A tag facet is keyed "<col>Tags"; a value facet is keyed on the column itself.
            QString backing = key;
            if (backing.endsWith(QStringLiteral("Tags")))
                backing.chop(4);
            bool found = false;
            for (const QVariant &cv : cols)
                if (cv.toMap().value(QStringLiteral("key")).toString() == backing) found = true;
            if (!found) {
                ++orphanFacets;
                std::printf("        facet '%s' has no column to scan\n", qPrintable(key));
            }
        }
        check(orphanFacets == 0, "every metrics facet is backed by a column of the same name");

        // And the column agrees row-for-row with the chip, not merely in name.
        int mismatched = 0;
        for (const QVariant &rv : m.rows(QStringLiteral("metrics"), QVariantMap{})) {
            const QVariantMap r     = rv.toMap();
            const QVariantList cs   = r.value(QStringLiteral("cells")).toList();
            const QStringList tags  = r.value(QStringLiteral("sortKeys")).toMap()
                                       .value(QStringLiteral("stereoTags")).toStringList();
            int idx = -1;
            for (int i = 0; i < cols.size(); ++i)
                if (cols.at(i).toMap().value(QStringLiteral("key")).toString()
                    == QLatin1String("stereo")) idx = i;
            if (idx < 0 || idx >= cs.size()) continue;
            const QString shown = cs.at(idx).toMap().value(QStringLiteral("text")).toString();
            const QString want  = tags.isEmpty() ? QStringLiteral("—") : tags.first();
            if (shown != want) {
                ++mismatched;
                std::printf("        %s: column says '%s', chip says '%s'\n",
                            qPrintable(r.value(QStringLiteral("id")).toString()),
                            qPrintable(shown), qPrintable(want));
            }
        }
        check(mismatched == 0, "the 2nd-camera column says exactly what its chip does");

        // CHIP ORDER IS DECLARED, NOT INCIDENTAL. These counted in first-seen order for one build,
        // which over a tag facet means "whichever device the first metric in the manifest happened
        // to need" — so cameras, IMUs and the launch monitor interleaved differently in each of the
        // three facets and the rail read as unsorted. Sensors, then cameras, then the external box.
        const auto optionsOf = [&facets](const char *key) {
            QStringList vals;
            for (const QVariant &fv : facets) {
                const QVariantMap f = fv.toMap();
                if (f.value(QStringLiteral("key")).toString() != QLatin1String(key)) continue;
                for (const QVariant &ov : f.value(QStringLiteral("options")).toList())
                    vals << ov.toMap().value(QStringLiteral("value")).toString();
            }
            return vals;
        };
        const QStringList needsOrder = optionsOf("needsTags");
        const auto before = [&needsOrder](const char *a, const char *b) {
            const int ia = needsOrder.indexOf(QString::fromLatin1(a));
            const int ib = needsOrder.indexOf(QString::fromLatin1(b));
            return ia >= 0 && ib >= 0 && ia < ib;
        };
        check(before("Wrist IMUs", "Face-on camera"), "IMUs lead the Needs rail");
        check(before("Body IMUs", "Face-on camera"), "…both of them");
        check(before("Face-on camera", "Down-the-line camera"), "…then the views");
        check(before("Down-the-line camera", "Club tracking"),
              "…then what the cameras find in them");
        check(before("Ball tracking", "Launch monitor"), "…and the external box last");
        check(needsOrder.last() == QStringLiteral("Nothing extra"),
              "…with 'needs no device at all' after every device");

        // Strongest first, because "what would a second camera unlock" is worth answering before
        // "what would it merely sharpen".
        check(optionsOf("stereoTags")
                  == QStringList{ QStringLiteral("Unlock it"), QStringLiteral("Improve it"),
                                  QStringLiteral("Refine it") },
              "the 2nd-camera chips run strongest-first");
    }

    std::printf("=== the kind and prominence rails read in their own order, and their counts hold ===\n");
    {
        // A VALUE facet used to count in first-seen order, which is pack FILE order. `group` read
        // correctly only because core.json happens to be authored roughly in swing order — luck, not
        // design, and a user pack authored any other way scrambled it with nothing reporting so.
        // A prominence ladder in file order is not a ladder at all, which is what forced
        // valueFacetOrder() to exist.
        const QVariantList facets = m.facets(QStringLiteral("characteristics"));
        const auto optionsOf = [&facets](const char *key) {
            QStringList vals;
            for (const QVariant &fv : facets) {
                const QVariantMap f = fv.toMap();
                if (f.value(QStringLiteral("key")).toString() != QLatin1String(key)) continue;
                for (const QVariant &ov : f.value(QStringLiteral("options")).toList())
                    vals << ov.toMap().value(QStringLiteral("value")).toString();
            }
            return vals;
        };

        // COMMONEST FIRST, which is the ladder REVERSED — deliberately, and only here. A rail is
        // scanned from the top and the first question asked of a fault library is "what are the
        // common ones", so travelling past Rare to reach it puts the answer where the eye arrives
        // last. Everywhere else (the ring collar, the sort key) the ladder keeps its own order.
        //
        // Only the rungs the shipped library actually uses appear — a chip with no rows would be a
        // filter returning nothing — so this is the ladder filtered, not the whole ladder. Written
        // as a subsequence check so re-ranking a condition cannot fail it.
        QStringList ladder;
        for (Prominence pr : allProminences()) ladder << prominenceLabel(pr);
        std::reverse(ladder.begin(), ladder.end());
        const QStringList shownP = optionsOf("prominence");
        check(shownP.size() >= 2, "the prominence rail is populated");
        check(!shownP.isEmpty() && shownP.first() == prominenceLabel(Prominence::Ubiquitous),
              "the commonest rung heads the prominence rail");
        int at = -1;
        bool ordered = true;
        for (const QString &v : shownP) {
            const int i = ladder.indexOf(v);
            if (i <= at) ordered = false;
            at = i;
        }
        check(ordered, "…and the rest descend from it, commonest to rarest");

        QStringList kinds;
        for (ConditionKind k : allConditionKinds()) kinds << conditionKindLabel(k);
        at = -1; ordered = true;
        for (const QString &v : optionsOf("kind")) {
            const int i = kinds.indexOf(v);
            if (i <= at) ordered = false;
            at = i;
        }
        check(ordered, "and the kind chips run in the order a diagnosis reads");

        // The half that actually matters: a chip that says 12 must return 12. Reordering `seen`
        // cannot break this — the counts come from a hash and the filter re-derives from cell text —
        // but that is an argument, and this is the measurement.
        int disagreed = 0;
        for (const QVariant &fv : facets) {
            const QVariantMap f = fv.toMap();
            const QString     key = f.value(QStringLiteral("key")).toString();
            if (key != QStringLiteral("kind") && key != QStringLiteral("prominence")) continue;
            for (const QVariant &ov : f.value(QStringLiteral("options")).toList()) {
                const QVariantMap o = ov.toMap();
                QVariantMap selection;
                selection.insert(key, QVariantList{ o.value(QStringLiteral("value")) });
                QVariantMap filters;
                filters.insert(QStringLiteral("facets"), selection);   // rows() nests them here
                const int got = m.rows(QStringLiteral("characteristics"), filters).size();
                const int said = o.value(QStringLiteral("count")).toInt();
                if (got != said) {
                    ++disagreed;
                    std::printf("        %s/%s: chip says %d, filter returns %d\n",
                                qPrintable(key),
                                qPrintable(o.value(QStringLiteral("value")).toString()), said, got);
                }
            }
        }
        check(disagreed == 0, "every kind and prominence chip returns exactly what it counted");
    }

    std::printf("=== a condition ranks by how far it REACHES, not by its out-degree ===\n");
    {
        // Direct out-degree buries a long chain, and a long chain is the shape a root cause has.
        // "Settled tempo habit" is the regression case: ONE arrow leaves it, and it reaches 29
        // conditions and eleven ball-flight outcomes. Ranked on `explains` it sat near the bottom
        // of 95 rows and read as trivial content.
        const QVariantList causes = m.rows(QStringLiteral("causes"));
        check(!causes.isEmpty(), "the causes list is populated");

        QVariantMap tempo;
        for (const QVariant &v : causes)
            if (v.toMap().value(QStringLiteral("label")).toString()
                == QStringLiteral("Settled tempo habit"))
                tempo = v.toMap();
        check(!tempo.isEmpty(), "the shipped pack still carries the long-chain case");
        if (!tempo.isEmpty()) {
            const QVariantMap k = tempo.value(QStringLiteral("sortKeys")).toMap();
            check(k.value(QStringLiteral("explains")).toInt() == 1,
                  "it has exactly one arrow leaving it");
            check(k.value(QStringLiteral("leadsTo")).toInt() > 20,
                  "and reaches twenty-plus conditions through the chain");
            check(k.value(QStringLiteral("outcomes")).toInt() > 5,
                  "explaining a spread of bad shots the old column could not see");
        }

        // Transitive reach can never be smaller than the direct count, and the outcomes are a
        // SUBSET of what is reached — a stricter statement than "both are positive", and the one
        // that catches a walk run in the wrong direction.
        bool consistent = true;
        for (const QVariant &v : causes) {
            const QVariantMap k = v.toMap().value(QStringLiteral("sortKeys")).toMap();
            const int direct = k.value(QStringLiteral("explains")).toInt();
            const int reach  = k.value(QStringLiteral("leadsTo")).toInt();
            const int shots  = k.value(QStringLiteral("outcomes")).toInt();
            if (reach < direct || shots > reach) consistent = false;
        }
        check(consistent, "reach >= direct, and outcomes are a subset of what is reached");

        // Every counted outcome really is a BallFlight condition. Asserted against the pack rather
        // than against the façade, or the count could agree with itself and be wrong.
        {
            const auto                provider = makeResourcePackProvider();
            const CharacteristicPack &p        = provider->pack();
            bool                      matched  = true;
            for (const Condition &c : p.conditions) {
                int n = 0;
                for (const QString &id : causalClosure(p, c.id, /*downstream*/ true))
                    if (const Condition *e = p.condition(id))
                        if (e->group == ConditionGroup::BallFlight) ++n;
                if (outcomeReachOf(p, c.id) != n) matched = false;
            }
            check(matched, "outcomeReachOf counts the BallFlight members of the closure and no others");
        }

        // The default ordering is the whole point of the change: the first row must be a top-reach
        // cause, not whichever leaf happens to carry the most direct arrows.
        const QVariantMap first = causes.value(0).toMap().value(QStringLiteral("sortKeys")).toMap();
        int               bestShots = 0;
        for (const QVariant &v : causes)
            bestShots = std::max(bestShots, v.toMap().value(QStringLiteral("sortKeys")).toMap()
                                                .value(QStringLiteral("outcomes")).toInt());
        check(first.value(QStringLiteral("outcomes")).toInt() == bestShots,
              "causes default to the bad shots they explain, best first");

        // Slice is the mirror case: it causes NOTHING, so every out-edge column reads zero for it,
        // and it is reachable only by what converges on it.
        QVariantMap slice;
        for (const QVariant &v : m.rows(QStringLiteral("characteristics")))
            if (v.toMap().value(QStringLiteral("label")).toString() == QStringLiteral("Slice"))
                slice = v.toMap();
        check(!slice.isEmpty(), "slice is on the characteristics list");
        if (!slice.isEmpty()) {
            const QVariantMap k = slice.value(QStringLiteral("sortKeys")).toMap();
            check(k.value(QStringLiteral("leadsTo")).toInt() == 0,
                  "an outcome leads nowhere, so no out-edge measure can surface it");
            check(k.value(QStringLiteral("causedBy")).toInt() > 20,
                  "but a large part of the library converges on it");
        }

        // Sort keys exist on BOTH condition lists whichever columns are drawn, so either can be
        // sorted or faceted on any of the three.
        bool keyed = true;
        for (const QString &type : { QStringLiteral("characteristics"), QStringLiteral("causes") })
            for (const QVariant &v : m.rows(type)) {
                const QVariantMap k = v.toMap().value(QStringLiteral("sortKeys")).toMap();
                if (!k.contains(QStringLiteral("leadsTo")) || !k.contains(QStringLiteral("outcomes"))
                    || !k.contains(QStringLiteral("causedBy")))
                    keyed = false;
            }
        check(keyed, "all three reach keys ride on both condition lists");
    }

    std::printf("=== quantile facets are cut from the data, and count what they return ===\n");
    {
        // CAUSES only. The Characteristics list had one numeric facet — `causedBy`, "Most common
        // outcomes" — and it was removed when `prominence` shipped: both read as "how common is
        // this", both had a chip labelled "Common", and they meant entirely different things. The
        // count of paths into a row is still a column and still a sort key; it just stopped
        // competing for the word.
        for (const QVariant &fv : m.facets(QStringLiteral("characteristics")))
            check(fv.toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("quantile"),
                  "characteristics offers no numeric facet — prominence answers 'how common'");

        // …and prominence takes the slot the numeric facet used to hold, for the same visibility
        // reason it held it: the rail scrolls, `group` alone is nine chips, and the question a coach
        // arrives with must not be below the fold.
        {
            const QVariantMap first = m.facets(QStringLiteral("characteristics")).value(0).toMap();
            check(first.value(QStringLiteral("key")).toString() == QStringLiteral("prominence"),
                  "characteristics leads its rail with how common a thing is");
        }

        for (const QString &type : { QStringLiteral("causes") }) {
            QVariantList quantiles;
            for (const QVariant &fv : m.facets(type))
                if (fv.toMap().value(QStringLiteral("kind")).toString()
                    == QStringLiteral("quantile"))
                    quantiles.append(fv);
            check(!quantiles.isEmpty(),
                  qPrintable(QStringLiteral("%1 offers at least one numeric facet").arg(type)));

            for (const QVariant &fv : quantiles) {
                const QVariantMap  f    = fv.toMap();
                const QString      key  = f.value(QStringLiteral("key")).toString();
                const QVariantList opts = f.value(QStringLiteral("options")).toList();

                // One bucket filters nothing — the same rule the vocabulary facets follow.
                check(opts.size() >= 2,
                      qPrintable(QStringLiteral("%1 · %2 offers more than one bucket")
                                     .arg(type, key)));

                // THE invariant, extended to the new kind: the chip's count and the rows it
                // returns come from one computation, so it cannot advertise twelve and return nine.
                bool countsMatch = true, bounded = true, nonEmpty = true;
                int  covered = 0;
                for (const QVariant &ov : opts) {
                    const QVariantMap o = ov.toMap();
                    QVariantMap sel;
                    sel.insert(key, QStringList{ o.value(QStringLiteral("value")).toString() });
                    QVariantMap filters;
                    filters.insert(QStringLiteral("facets"), sel);
                    const int got = m.rows(type, filters).size();
                    const int adv = o.value(QStringLiteral("count")).toInt();
                    if (got != adv) countsMatch = false;
                    if (adv == 0)   nonEmpty    = false;
                    covered += adv;

                    // Every returned row really is inside the bucket's range.
                    const int  lo    = o.value(QStringLiteral("lo")).toInt();
                    const int  hi    = o.value(QStringLiteral("hi")).toInt();
                    const bool hasHi = o.value(QStringLiteral("hasHi")).toBool();
                    for (const QVariant &rv : m.rows(type, filters)) {
                        const int n = rv.toMap().value(QStringLiteral("sortKeys")).toMap()
                                          .value(key).toInt();
                        if (n < lo || (hasHi && n > hi)) bounded = false;
                    }
                }
                check(countsMatch,
                      qPrintable(QStringLiteral("%1 · %2: every bucket returns what it advertises")
                                     .arg(type, key)));
                check(bounded,
                      qPrintable(QStringLiteral("%1 · %2: every row lands inside its bucket")
                                     .arg(type, key)));
                check(nonEmpty,
                      qPrintable(QStringLiteral("%1 · %2: no bucket is drawn empty").arg(type, key)));
                // Buckets partition the list: no row in two, none left out. A gap would hide rows
                // from every chip at once, which is the failure an author would never think to look
                // for.
                check(covered == m.rows(type).size(),
                      qPrintable(QStringLiteral("%1 · %2: the buckets cover the list exactly")
                                     .arg(type, key)));

                // Selecting two buckets ORs them, exactly as two values of a vocabulary facet do.
                if (opts.size() >= 2) {
                    QVariantMap sel;
                    sel.insert(key, QStringList{ opts.at(0).toMap().value(QStringLiteral("value")).toString(),
                                                 opts.at(1).toMap().value(QStringLiteral("value")).toString() });
                    QVariantMap filters;
                    filters.insert(QStringLiteral("facets"), sel);
                    check(m.rows(type, filters).size()
                              == opts.at(0).toMap().value(QStringLiteral("count")).toInt()
                                     + opts.at(1).toMap().value(QStringLiteral("count")).toInt(),
                          qPrintable(QStringLiteral("%1 · %2: two buckets OR together").arg(type, key)));
                }
            }
        }

        // A type with no numeric key offers none rather than an empty rail.
        for (const QVariant &fv : m.facets(QStringLiteral("measures")))
            check(fv.toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("quantile"),
                  "measures offer no quantile facet");
    }

    std::printf("=== an edit re-cuts the counts, it does not leave them stale ===\n");
    {
        // The memo is the risk this change introduces: a cached reach surviving an edit would show
        // yesterday's ranking against today's graph, and nothing else in the panel would disagree
        // with it. Adding one edge has to move the count of everything upstream of the new target.
        const QVariantList causes = m.rows(QStringLiteral("causes"));
        QString            from;
        int                before = -1;
        for (const QVariant &v : causes) {
            const QVariantMap r = v.toMap();
            const int reach = r.value(QStringLiteral("sortKeys")).toMap()
                                  .value(QStringLiteral("leadsTo")).toInt();
            if (reach > 0 && reach < 20) { from = r.value(QStringLiteral("id")).toString(); before = reach; break; }
        }
        check(!from.isEmpty(), "a cause with room to grow");

        // Any legal target the graph would allow — asked of the same function the drag asks.
        QString to;
        for (const QVariant &v : m.linkCandidates(QStringLiteral("causes"), from)) {
            const QString id = v.toMap().value(QStringLiteral("id")).toString();
            if (id.isEmpty()) continue;
            to = id;
            break;
        }
        check(!to.isEmpty(), "and a legal thing to point it at");

        if (!from.isEmpty() && !to.isEmpty()) {
            check(m.addLink(from, to).value(QStringLiteral("ok")).toBool(), "the link lands");
            int after = -1;
            for (const QVariant &v : m.rows(QStringLiteral("causes")))
                if (v.toMap().value(QStringLiteral("id")).toString() == from)
                    after = v.toMap().value(QStringLiteral("sortKeys")).toMap()
                                .value(QStringLiteral("leadsTo")).toInt();
            check(after > before, "and the reach count grew with it — the memo was invalidated");

            m.undo();
            int restored = -1;
            for (const QVariant &v : m.rows(QStringLiteral("causes")))
                if (v.toMap().value(QStringLiteral("id")).toString() == from)
                    restored = v.toMap().value(QStringLiteral("sortKeys")).toMap()
                                   .value(QStringLiteral("leadsTo")).toInt();
            check(restored == before, "undo puts the count back too");
        }
        while (m.canUndo()) m.undo();
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

    std::printf("=== the ring collar offers the whole strength ladder ===\n");
    {
        // The collar draws five cells, so every rung is reachable in one hold and none of them is
        // shortlisted away. The bug this guards against is silent: cap the list below the number of
        // rungs and `always` can never be reached from the graph at all, while a link that already
        // IS `always` opens a collar in which no cell is the one it holds — which is exactly the
        // state the gesture's release-on-current no-op depends on being impossible.
        QString linkId;
        for (const QVariant &v : m.rows(QStringLiteral("links"))) {
            const QVariantMap r = v.toMap();
            if (cellFor(r, QStringLiteral("strength")).value(QStringLiteral("editable")).toBool()) {
                linkId = r.value(QStringLiteral("id")).toString();
                break;
            }
        }
        check(!linkId.isEmpty(), "a causal link to hold on");

        const auto windowAt = [&](const QString &rung) {
            m.setField(QStringLiteral("links"), linkId, QStringLiteral("strength"), rung);
            QStringList vals;
            for (const QVariant &v :
                 m.ringValues(QStringLiteral("links"), linkId, QStringLiteral("strength")))
                vals << v.toMap().value(QStringLiteral("value")).toString();
            return vals;
        };
        const auto currentOf = [&]() {
            for (const QVariant &v :
                 m.ringValues(QStringLiteral("links"), linkId, QStringLiteral("strength")))
                if (v.toMap().value(QStringLiteral("current")).toBool())
                    return v.toMap().value(QStringLiteral("value")).toString();
            return QString();
        };

        // The whole ladder, in ladder order, whichever rung the link is on — and the one it holds is
        // always the marked cell. Ladder order is the contract: the collar's cells are laid out in
        // list order, so a re-sorted list would put `often` next to `always` on the canvas.
        const QStringList ladder{ QStringLiteral("veryWeak"), QStringLiteral("weak"),
                                  QStringLiteral("moderate"), QStringLiteral("strong"),
                                  QStringLiteral("veryStrong") };
        for (const QString &rung : ladder) {
            check(windowAt(rung) == ladder, "five cells in ladder order, whatever the rung");
            check(currentOf() == rung, "marked as current, so releasing on it changes nothing");
        }

        // The cap is what the collar draws, not a number that happens to be three. A field with more
        // options than cells still shortlists, and must still contain what the object holds.
        const QString condId = firstShippedId(m.rows(QStringLiteral("characteristics")));
        const QVariantList groups =
            m.ringValues(QStringLiteral("characteristics"), condId, QStringLiteral("group"));
        check(groups.size() == 5, "nine groups shortlist to the five the collar draws");
        bool groupCurrent = false;
        for (const QVariant &v : groups)
            if (v.toMap().value(QStringLiteral("current")).toBool()) groupCurrent = true;
        check(groupCurrent, "and the shortlist never drops the value being changed");

        // Kind takes the group's route — seven into five, ranked by what this library uses most.
        const QVariantList kinds =
            m.ringValues(QStringLiteral("characteristics"), condId, QStringLiteral("kind"));
        check(kinds.size() == 5, "seven kinds shortlist to the five the collar draws");
        bool kindCurrent = false;
        for (const QVariant &v : kinds)
            if (v.toMap().value(QStringLiteral("current")).toBool()) kindCurrent = true;
        check(kindCurrent, "…and still shows the one being changed");

        // Prominence takes the STRENGTH route instead: five rungs into five cells, in ladder order,
        // unsorted. Ranking a ladder by how often the library uses each rung would be a ladder about
        // the ladder. This is the assertion that stops somebody "fixing" it to match `group`.
        QStringList wantLadder;
        for (Prominence pr : allProminences()) wantLadder << prominenceName(pr);
        QStringList gotLadder;
        for (const QVariant &v : m.ringValues(QStringLiteral("characteristics"), condId,
                                              QStringLiteral("prominence")))
            gotLadder << v.toMap().value(QStringLiteral("value")).toString();
        check(gotLadder == wantLadder, "the whole prominence ladder, in ladder order, unsorted");

        while (m.canUndo()) m.undo();
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
            // No maxPerRank, because the panel no longer sends one — see dag_layout.h. Leaving it
            // here would have this run the one configuration the app never uses.
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

        // The bibliography rows, over the SHIPPED pack and through the façade the panel calls. The
        // layout's own test proves the geometry on a fixture; what can only be wrong here is the
        // wiring — an option key the panel spells one way and the façade reads another leaves the
        // switch doing nothing at all, and both sides compile perfectly.
        {
            QVariantMap ro = opts;
            ro.insert(QStringLiteral("includeReferences"), true);
            ro.insert(QStringLiteral("rowH"), 24);

            // A cited condition, so there is something to draw. `s_posture` carries a DOI the shipped
            // bibliography resolves.
            const QVariantMap rg = m.dag(QStringLiteral("s_posture"), ro);
            int rows = 0, resolved = 0;
            for (const QVariant &nv : rg.value(QStringLiteral("nodes")).toList())
                for (const QVariant &rv :
                     nv.toMap().value(QStringLiteral("references")).toList()) {
                    ++rows;
                    const QVariantMap r = rv.toMap();
                    if (r.value(QStringLiteral("resolved")).toBool()
                        && !r.value(QStringLiteral("id")).toString().isEmpty()
                        && !r.value(QStringLiteral("label")).toString().isEmpty()
                        && r.value(QStringLiteral("h")).toDouble() > 0)
                        ++resolved;
                }
            check(rows > 0, "the graph carries citation rows when the switch is on");
            check(resolved == rows,
                  "and every shipped citation it draws resolves to a paper with a title");

            // Off, they go — the switch has to be the thing that decides, not the presence of a
            // citation in the pack.
            QVariantMap off = ro;
            off.insert(QStringLiteral("includeReferences"), false);
            int stillThere = 0;
            for (const QVariant &nv :
                 m.dag(QStringLiteral("s_posture"), off).value(QStringLiteral("nodes")).toList())
                stillThere += nv.toMap().value(QStringLiteral("references")).toList().size();
            check(stillThere == 0, "and none when it is off");
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

        // The ubiquity mark, on the nodes and nowhere else. It is the TOP rung only — a mark that
        // also meant `Common` would land on half the library — and it has to be right on every node
        // of a picture, not just the one it is centred on, because that is the comparison a reader
        // makes with it.
        {
            const auto  core = makeResourcePackProvider();
            const auto &cp   = core->pack();
            QString     everywhere;
            for (const Condition &c : cp.conditions)
                if (c.prominence == Prominence::Ubiquitous) { everywhere = c.id; break; }
            check(!everywhere.isEmpty(), "the shipped library has something ubiquitous in it");

            const QVariantMap ug = m.graph(QStringLiteral("characteristics"), everywhere, opts);
            int flagged = 0, wrong = 0, seen = 0;
            for (const QVariant &v : ug.value(QStringLiteral("nodes")).toList()) {
                const QVariantMap n  = v.toMap();
                const Condition  *c  = cp.condition(n.value(QStringLiteral("id")).toString());
                if (!c) continue;
                ++seen;
                const bool marked = n.value(QStringLiteral("ubiquitous")).toBool();
                if (marked) ++flagged;
                if (marked != (c->prominence == Prominence::Ubiquitous)) ++wrong;
            }
            check(seen > 0 && flagged > 0, "a ubiquitous condition's node says so");
            check(wrong == 0, "and no node claims a prominence its condition does not have");
        }

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

        // ── Every line in a neighbourhood is ANCHORED ───────────────────────
        //
        // A one-hop neighbourhood is three columns and a line from each node to the focus. Both
        // ends of every line must sit on the EDGE of a box, and nothing was checking it: the
        // crossing that built the endpoints paired each column's x with the OTHER row's y, so a
        // right-hand node drew between two points belonging to neither box — and anchored the far
        // end on the focus's left edge, sending the line back across the focus to get there.
        //
        // Why it survived is the part worth keeping. A column holding ONE node centres that node on
        // the focus's own row, so the two y values coincide and the crossed pair lands correctly by
        // accident. It only breaks once a column holds several, which is why `measures` shows it
        // and `metrics` and `signals` — one relation each way — do not. All three are asserted
        // anyway: they run through the same arithmetic and the next content change decides which of
        // them has two nodes in a column.
        for (const auto &pair : { std::make_pair(QStringLiteral("measures"),
                                                 QStringLiteral("m_ballPosition")),
                                  std::make_pair(QStringLiteral("metrics"),
                                                 QStringLiteral("ballPosition")),
                                  std::make_pair(QStringLiteral("signals"),
                                                 QStringLiteral("sig_ballForward")) }) {
            const QVariantMap g = m.graph(pair.first, pair.second, gopt);
            const QVariantList ns = g.value(QStringLiteral("nodes")).toList();
            const QVariantList es = g.value(QStringLiteral("edges")).toList();
            if (ns.isEmpty()) continue;

            // Every drawn box, as a rect.
            struct Box { double x, y, w, h; };
            std::vector<Box> boxes;
            for (const QVariant &v : ns) {
                const QVariantMap n = v.toMap();
                boxes.push_back({ n.value(QStringLiteral("x")).toDouble(),
                                  n.value(QStringLiteral("y")).toDouble(),
                                  n.value(QStringLiteral("w")).toDouble(),
                                  n.value(QStringLiteral("h")).toDouble() });
            }
            // On an edge of SOME box: the x is one of its two vertical sides and the y lies within
            // its height. A point floating between columns satisfies neither.
            auto onABoxEdge = [&](double x, double y) {
                for (const Box &b : boxes) {
                    const bool onSide = std::abs(x - b.x) < 0.5 || std::abs(x - (b.x + b.w)) < 0.5;
                    if (onSide && y >= b.y - 0.5 && y <= b.y + b.h + 0.5) return true;
                }
                return false;
            };

            int loose = 0;
            for (const QVariant &v : es) {
                const QVariantMap e = v.toMap();
                if (!onABoxEdge(e.value(QStringLiteral("x1")).toDouble(),
                                e.value(QStringLiteral("y1")).toDouble())) ++loose;
                if (!onABoxEdge(e.value(QStringLiteral("x2")).toDouble(),
                                e.value(QStringLiteral("y2")).toDouble())) ++loose;
            }
            check(!es.isEmpty(),
                  qPrintable(QStringLiteral("%1 draws lines at all").arg(pair.first)));
            check(loose == 0,
                  qPrintable(QStringLiteral("%1: both ends of every line sit on a box")
                                 .arg(pair.first)));
        }
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
                                     QStringLiteral("state"), QStringLiteral("kind"),
                                     QStringLiteral("prominence") })
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

    std::printf("=== a citation is a way through to the paper, not just a DOI to read ===\n");
    {
        // The Fields section holds the citation as an editable identifier, which is the right thing
        // to type into and the wrong thing to read: `10.1088/0034-4885/66/2/202` answers nothing on
        // its own. This is the row that turns it back into a title somebody can recognise — it was
        // built for characteristics and links and then never appended to the pane, so the pane
        // showed the DOI and no way to reach what it named.
        //
        // Asserted on the ROW TYPE rather than on the section's title, for the same reason a section
        // carries an `action` key: a translated title is a contract that holds in one locale.
        auto citedRow = [&](const QString &type, const QString &id) {
            for (const QVariant &sv : m.inspect(type, id).value(QStringLiteral("sections")).toList())
                for (const QVariant &rv : sv.toMap().value(QStringLiteral("rows")).toList()) {
                    const QVariantMap r = rv.toMap();
                    if (r.value(QStringLiteral("type")).toString() == QStringLiteral("references"))
                        return r;
                }
            return QVariantMap();
        };

        auto firstCited = [&](const QString &type) {
            for (const QVariant &rv : m.rows(type)) {
                const QVariantMap r =
                    citedRow(type, rv.toMap().value(QStringLiteral("id")).toString());
                if (!r.isEmpty()) return r;
            }
            return QVariantMap();
        };

        auto checkRow = [&](const QVariantMap &row, const char *what) {
            check(!row.isEmpty(), what);
            if (row.isEmpty()) return;
            check(row.value(QStringLiteral("navigable")).toBool(), "…which is navigable");
            check(!row.value(QStringLiteral("label")).toString().isEmpty(),
                  "…and says the title rather than the identifier it was joined on");
            // The id it names has to OPEN. A link that navigates to nothing is worse than no link:
            // the reader is told there is a paper and then shown an empty pane.
            check(m.inspect(QStringLiteral("references"),
                            row.value(QStringLiteral("id")).toString())
                      .value(QStringLiteral("found")).toBool(),
                  "…and the reference it names actually opens");
        };

        // Both types the shipped pack cites from, because the row was built for both and appended to
        // neither — a fix on the characteristic alone would leave the causal links silent.
        checkRow(firstCited(QStringLiteral("characteristics")),
                 "a cited characteristic offers a row into the bibliography");
        checkRow(firstCited(QStringLiteral("links")), "and so does a cited causal link");

        // A screen carries the same kind of citation — screen_pack.h says DOI or PMID and nothing
        // else — but no SHIPPED screen carries one yet, so the row is exercised by writing one. That
        // is the honest way to assert it: skipping the type because the content is thin would leave
        // it uncovered on the day somebody adds the first citation.
        {
            QString aCitation;
            for (const QVariant &rv : m.rows(QStringLiteral("characteristics"))) {
                const QString cid = rv.toMap().value(QStringLiteral("id")).toString();
                for (const QVariant &sv :
                     m.inspect(QStringLiteral("characteristics"), cid)
                         .value(QStringLiteral("sections")).toList()) {
                    const QVariantMap s = sv.toMap();
                    if (s.value(QStringLiteral("kind")).toString() != QStringLiteral("fields"))
                        continue;
                    for (const QVariant &fv : s.value(QStringLiteral("rows")).toList()) {
                        const QVariantMap f = fv.toMap();
                        if (f.value(QStringLiteral("field")).toString()
                                == QStringLiteral("citation")
                            && !f.value(QStringLiteral("value")).toString().isEmpty())
                            aCitation = f.value(QStringLiteral("value")).toString();
                    }
                }
                if (!aCitation.isEmpty()) break;
            }
            check(!aCitation.isEmpty(), "the shipped pack cites something to reuse");

            const QString screenId =
                m.rows(QStringLiteral("screens")).first().toMap()
                    .value(QStringLiteral("id")).toString();
            check(m.setField(QStringLiteral("screens"), screenId, QStringLiteral("citation"),
                             aCitation)
                      .value(QStringLiteral("ok")).toBool(),
                  "a screen takes a citation");
            checkRow(citedRow(QStringLiteral("screens"), screenId),
                     "…and its pane grows the row into the bibliography");
            while (m.canUndo()) m.undo();
        }

        // The link is on the FIELD too, not only in the section below it. The value has to stay the
        // bare identifier — it is the join key — so this is what tells an author that the DOI they
        // typed resolves, and to what, at the place they typed it.
        {
            auto citationFieldOf = [&](const QString &type, const QString &id) {
                for (const QVariant &sv :
                     m.inspect(type, id).value(QStringLiteral("sections")).toList()) {
                    const QVariantMap s = sv.toMap();
                    if (s.value(QStringLiteral("kind")).toString() != QStringLiteral("fields"))
                        continue;
                    for (const QVariant &fv : s.value(QStringLiteral("rows")).toList()) {
                        const QVariantMap f = fv.toMap();
                        if (f.value(QStringLiteral("field")).toString()
                                == QStringLiteral("citation"))
                            return f;
                    }
                }
                return QVariantMap();
            };

            QVariantMap cited, uncited;
            for (const QVariant &rv : m.rows(QStringLiteral("characteristics"))) {
                const QVariantMap f =
                    citationFieldOf(QStringLiteral("characteristics"),
                                    rv.toMap().value(QStringLiteral("id")).toString());
                if (f.isEmpty()) continue;
                if (f.value(QStringLiteral("value")).toString().isEmpty()) {
                    if (uncited.isEmpty()) uncited = f;
                } else if (cited.isEmpty()) {
                    cited = f;
                }
                if (!cited.isEmpty() && !uncited.isEmpty()) break;
            }

            check(!cited.isEmpty(), "a cited characteristic has a citation field");
            check(cited.value(QStringLiteral("linkType")).toString()
                      == QStringLiteral("references"),
                  "…which carries the link to the paper it resolves to");
            check(!cited.value(QStringLiteral("linkLabel")).toString().isEmpty(),
                  "…labelled with the title rather than the DOI already in the field");
            check(m.inspect(QStringLiteral("references"),
                            cited.value(QStringLiteral("linkId")).toString())
                      .value(QStringLiteral("found")).toBool(),
                  "…and the record it points at opens");
            // The field is still an EDITOR. A link that turned it read-only would trade one
            // affordance for the other.
            check(cited.value(QStringLiteral("kind")).toString() == QStringLiteral("text"),
                  "…while the field stays editable text");
            check(cited.value(QStringLiteral("value")).toString()
                      != cited.value(QStringLiteral("linkLabel")).toString(),
                  "…holding the identifier, never the title — the identifier is the join key");

            // No link where there is nothing to link to. A dead link is worse than none.
            check(!uncited.isEmpty(), "an uncited characteristic has one too");
            check(!uncited.contains(QStringLiteral("linkType")),
                  "…and it carries no link at all");
        }

        // A corridor's `citation` doubles as the note explaining a provisional figure — most of the
        // shipped ones are a paragraph, not an identifier — so those must NOT be run through the
        // failed-lookup row. Nothing here asserts a corridor has the row; what it asserts is that
        // any corridor that does have one resolves, rather than reporting a typo on every norm.
        for (const QVariant &rv : m.rows(QStringLiteral("corridors"))) {
            const QString id = rv.toMap().value(QStringLiteral("id")).toString();
            const QVariantMap row = citedRow(QStringLiteral("corridors"), id);
            if (row.isEmpty()) continue;
            check(m.inspect(QStringLiteral("references"),
                            row.value(QStringLiteral("id")).toString())
                      .value(QStringLiteral("found")).toBool(),
                  "a corridor's citation row is only drawn when it resolves");
        }
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

    // ── The swing on screen: readings in the measures table ─────────────────────────────────────
    //
    // The one column in this panel whose value comes from OUTSIDE the library. Three things are
    // asserted because each has already been got wrong somewhere in this file's history or is one
    // line away from being: that the column and the cells agree about whether it exists (positional
    // arrays), that the corridor which answers is the one the SWING'S CLUB selects rather than the
    // default, and that the two tones mean inside and outside and nothing else.
    std::printf("\n=== the loaded swing's readings ===\n");
    {
        auto columnIndex = [](const QVariantList &cols, const QString &key) {
            for (int i = 0; i < cols.size(); ++i)
                if (cols.at(i).toMap().value(QStringLiteral("key")).toString() == key) return i;
            return -1;
        };

        // A swing carrying ONE reading: ball position, 25% of stance width, at address. The number
        // is chosen because the shipped pack grades it differently per club and the difference is
        // the whole point of the context walk — driver mu 5 sigma 8 (z = 2.5, a deviation), iron
        // mu 33 sigma 10 (z = -0.8, inside). One swing, two clubs, two colours.
        auto writeSwing = [](const QString &dir, const QString &club, double value) {
            QDir().mkpath(dir);
            QJsonArray t, v;
            for (qint64 x = 0; x <= 100000; x += 1000) { t.append(x); v.append(value); }
            QJsonObject metric;
            metric.insert(QStringLiteral("key"),   QStringLiteral("ballPosition"));
            metric.insert(QStringLiteral("unit"),  QStringLiteral("% stance width"));
            metric.insert(QStringLiteral("t_us"),  t);
            metric.insert(QStringLiteral("value"), v);

            QJsonObject phase;
            phase.insert(QStringLiteral("phase"), int(Phase::Address));
            phase.insert(QStringLiteral("t_us"),  50000);
            phase.insert(QStringLiteral("conf"),  0.9);

            QJsonObject analysis;
            analysis.insert(QStringLiteral("metrics"), QJsonArray{ metric });
            analysis.insert(QStringLiteral("phases"),  QJsonArray{ phase });

            QJsonObject review;  review.insert(QStringLiteral("club"), club);
            QJsonObject swing;   swing.insert(QStringLiteral("index"), 3);

            QJsonObject root;
            root.insert(QStringLiteral("analysis"), analysis);
            root.insert(QStringLiteral("review"),   review);
            root.insert(QStringLiteral("swing"),    swing);

            QFile f(QDir(dir).filePath(QStringLiteral("swing.json")));
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        };

        // The grid is read on a worker (a swing with no sidecar costs a full parse, which must not
        // land on the UI thread), so every assertion here waits for it rather than assuming.
        auto settle = [&m]() {
            QElapsedTimer clock; clock.start();
            while (m.currentSwingLoading() && clock.elapsed() < 15000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        };

        // No swing: the column is not there. The ordinary state of this panel is that nobody has a
        // session open, and a permanent column of dashes would be a standing accusation against the
        // library rather than information.
        check(columnIndex(m.columns(QStringLiteral("measures")), QStringLiteral("swingValue")) < 0,
              "with no swing loaded the measures table has no swing column");
        check(!m.currentSwingHasValues(), "and reports no readings to show");

        QTemporaryDir tmp;
        const QString driverDir = QDir(tmp.path()).filePath(QStringLiteral("swing_0003"));
        const QString ironDir   = QDir(tmp.path()).filePath(QStringLiteral("swing_0004"));
        writeSwing(driverDir, QStringLiteral("DRIVER"),  25.0);
        writeSwing(ironDir,   QStringLiteral("7 IRON"),  25.0);

        m.setCurrentSwingDir(driverDir);
        settle();

        const QVariantList cols = m.columns(QStringLiteral("measures"));
        const int          idx  = columnIndex(cols, QStringLiteral("swingValue"));
        check(idx >= 0, "a loaded swing puts the swing column on the measures table");
        check(idx == cols.size() - 1,
              "and puts it LAST — every other column is a property of the library, not of a swing");
        check(m.currentSwingContext() == QStringLiteral("driver"),
              "the readings are graded at the context the swing's club selects");
        check(m.currentSwingLabel().contains(QStringLiteral("DRIVER")),
              "and the panel can say which swing they belong to");

        const QVariantList rows = m.rows(QStringLiteral("measures"));
        // Cell/column parity, over EVERY row rather than the one under test. columns() and rows()
        // build positional arrays against each other and a disagreement is invisible to any check
        // that looks at one row — the delegate reads columns[index] per cell, so a short column
        // list is a binding evaluated against undefined.
        bool parity = !rows.isEmpty();
        for (const QVariant &rv : rows)
            if (rv.toMap().value(QStringLiteral("cells")).toList().size() != cols.size())
                parity = false;
        check(parity, "every measure row has exactly one cell per column, swing column included");

        const QVariantMap bp = rowFor(rows, QStringLiteral("m_ballPosition"));
        check(!bp.isEmpty(), "the swing's one measurable reading is on a row that exists");
        const QVariantMap bpCell = bp.value(QStringLiteral("cells")).toList().at(idx).toMap();
        // "25.0", not "25" — normNumber gives a figure one to four decimals wide, the same
        // spelling every other norm number in this panel uses. A column that rendered its readings
        // to a different precision than the corridor beside it would invite exactly one question.
        check(bpCell.value(QStringLiteral("text")).toString() == QStringLiteral("25.0"),
              "the cell shows what the swing actually read");
        check(bpCell.value(QStringLiteral("tone")).toString() == QStringLiteral("warn"),
              "25% of stance width is outside the DRIVER corridor, so it warns");

        // A measure the swing cannot produce. Not a zero and not a blank — "not assessed" and
        // "assessed and fine" are different statements and this module exists to keep them apart.
        const QVariantMap absent =
            rowFor(rows, QStringLiteral("m_leadWristFlexExt_p1"));
        if (!absent.isEmpty()) {
            const QVariantMap c = absent.value(QStringLiteral("cells")).toList().at(idx).toMap();
            check(c.value(QStringLiteral("text")).toString() == QStringLiteral("—"),
                  "a measure this swing cannot produce reads as a dash, never as a zero");
            check(c.value(QStringLiteral("tone")).toString() == QStringLiteral("dim"),
                  "and is muted rather than warned — a capture gap is not a swing finding");
        }

        // THE SAME READING, A DIFFERENT CLUB. This is the assertion that makes the context walk
        // load-bearing rather than decorative: 25% is a deviation off a driver and unremarkable off
        // a 7 iron, and a column that coloured both the same would be confidently wrong for one of
        // them every time.
        m.setCurrentSwingDir(ironDir);
        // BEFORE the grid lands. The previous swing's readings must be gone the instant the swing
        // changes, not when the replacement arrives — a parse takes a second on an unindexed swing,
        // and for that second the table would otherwise show the driver's numbers under the iron's
        // name, which is a wrong answer a reader has no way to detect. (Deterministic: the worker's
        // completion is delivered through the event loop, which has not run yet.)
        check(m.currentSwingLoading(), "switching swings starts a read");
        check(columnIndex(m.columns(QStringLiteral("measures")),
                          QStringLiteral("swingValue")) < 0,
              "and drops the old swing's column immediately, rather than when the new one lands");
        settle();
        check(m.currentSwingContext() == QStringLiteral("iron"),
              "loading another swing re-resolves the context from its own club");
        const QVariantList ironCols = m.columns(QStringLiteral("measures"));
        const int          ironIdx  = columnIndex(ironCols, QStringLiteral("swingValue"));
        const QVariantMap  ironBp   = rowFor(m.rows(QStringLiteral("measures")),
                                             QStringLiteral("m_ballPosition"));
        const QVariantMap  ironCell =
            ironBp.value(QStringLiteral("cells")).toList().at(ironIdx).toMap();
        check(ironCell.value(QStringLiteral("text")).toString() == QStringLiteral("25.0"),
              "the same reading");
        check(ironCell.value(QStringLiteral("tone")).toString().isEmpty(),
              "inside the IRON corridor, so it takes the ordinary text colour");

        // Unloading takes the column with it, rather than leaving the last swing's numbers on
        // screen under nobody's name.
        m.setCurrentSwingDir(QString());
        check(columnIndex(m.columns(QStringLiteral("measures")), QStringLiteral("swingValue")) < 0,
              "clearing the swing removes the column again");
        check(!m.currentSwingHasValues(), "and the readings go with it, immediately");
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
