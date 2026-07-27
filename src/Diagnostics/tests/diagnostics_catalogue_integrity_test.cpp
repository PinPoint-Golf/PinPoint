// Cross-registry integrity between the diagnostics pack and the metric catalogue.
//
// These are two hand-authored registries that must agree, and nothing else checks that they do.
// The catalogue's `usedBy` is documented as "static, hand-authored" — exactly the kind of field
// that silently rots as content changes around it, and a wrong reverse index is worse than an
// absent one because it looks authoritative.
//
// Every assertion here is computed from the pack and compared against the catalogue, in BOTH
// directions. A one-way check would pass a catalogue that claims uses which no longer exist.
//
//   cmake --build build/analyzer-tests --target diagnostics_catalogue_integrity_test
//   ctest --test-dir build/analyzer-tests -R diagnostics_catalogue_integrity --output-on-failure

#include "../characteristic_pack.h"

#include "characteristic_library_model.h"
#include "metric_catalogue.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaMethod>
#include <QRegularExpression>
#include <QFile>
#include <QSet>

#include <cstdio>
#include <map>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static const QString kPrefix = QStringLiteral("characteristic:");

int main()
{
    std::printf("diagnostics_catalogue_integrity_test\n");

    QFile f(QStringLiteral(PP_CORE_PACK_PATH));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("  [FAIL] cannot open %s\n", PP_CORE_PACK_PATH);
        return 1;
    }
    const PackLoadResult      res = loadPack(f.readAll(), QStringLiteral("core.json"));
    const CharacteristicPack &p   = res.pack;
    const MetricCatalogue     cat = makeMetricCatalogue();

    check(res.loaded, "the pack loads");

    // ── Ground truth: (metricKey -> characteristics) computed from the pack ─────
    std::map<QString, QSet<QString>> packUses;      // metricKey -> condition ids
    QSet<QString>                    packMeasuresWithoutKey;

    for (const Condition &c : p.conditions) {
        for (const QString &sid : c.detectedBy) {
            const Signal *s = p.signal(sid);
            if (!s) continue;
            for (const QString &mid : s->measures) {
                const Measure *m = p.measure(mid);
                if (!m) continue;
                if (m->metricKey.isEmpty()) packMeasuresWithoutKey.insert(m->id);
                else                        packUses[m->metricKey].insert(c.id);
            }
        }
    }

    // ── 1. Every characteristic resolves to a catalogue metric ──────────────────
    // The whole point of the exercise: no characteristic may dangle on a measure that names nothing
    // in the catalogue, or the two registries have already diverged.
    {
        check(packMeasuresWithoutKey.isEmpty(),
              "every measure a characteristic uses names a catalogue metric");
        if (!packMeasuresWithoutKey.isEmpty())
            for (const QString &id : packMeasuresWithoutKey)
                std::printf("        measure with no metricKey: %s\n", qPrintable(id));

        int unresolved = 0;
        for (const Condition &c : p.conditions) {
            if (c.observability == Observability::Latent) continue;
            // A condition only establishable by asking, or by a physical screen, cannot resolve to
            // a metric BY DEFINITION — and `validatePack` already refuses it a signal, so requiring
            // one here would demand the two rules contradict each other. The ball-flight outcomes
            // that need a CONJUNCTION of readings (a chunk is low point behind AND speed collapse,
            // which the engine's OR over signals cannot express) ship this way deliberately: the
            // golfer knows, and the app does not yet.
            if (isOutsideCaptureReach(c.confirmedBy)) continue;
            bool resolves = false;
            for (const QString &sid : c.detectedBy) {
                const Signal *s = p.signal(sid);
                if (!s) continue;
                for (const QString &mid : s->measures) {
                    const Measure *m = p.measure(mid);
                    if (m && !m->metricKey.isEmpty()) resolves = true;
                }
            }
            if (!resolves) {
                ++unresolved;
                std::printf("        unresolved characteristic: %s\n", qPrintable(c.id));
            }
        }
        check(unresolved == 0, "every characteristic resolves to a metric");
    }

    // ── 2. Every metricKey the pack names exists in the catalogue ───────────────
    {
        int missing = 0;
        for (const auto &[key, users] : packUses) {
            if (cat.descriptor(key) == nullptr) {
                ++missing;
                std::printf("        pack names unknown metric: %s\n", qPrintable(key));
            }
        }
        check(missing == 0, "every metric the pack names exists in the catalogue");
    }

    // ── 3. usedBy agrees with the pack — CATALOGUE -> PACK ──────────────────────
    // Catches a stale entry: a characteristic that was renamed, retired, or moved to a different
    // measure, leaving the catalogue asserting a use that no longer happens.
    {
        int stale = 0;
        for (const MetricDescriptor *d : cat.all()) {
            for (const QString &u : d->usedBy) {
                if (!u.startsWith(kPrefix)) continue;   // other consumers are not ours to police
                const QString cid = u.mid(kPrefix.size());

                if (p.condition(cid) == nullptr) {
                    ++stale;
                    std::printf("        %s claims characteristic '%s', which does not exist\n",
                                qPrintable(d->key), qPrintable(cid));
                    continue;
                }
                if (!packUses[d->key].contains(cid)) {
                    ++stale;
                    std::printf("        %s claims characteristic '%s', which does not use it\n",
                                qPrintable(d->key), qPrintable(cid));
                }
            }
        }
        check(stale == 0, "every characteristic the catalogue claims really uses that metric");
    }

    // ── 4. usedBy agrees with the pack — PACK -> CATALOGUE ──────────────────────
    // Catches the commoner rot: a new characteristic authored against an existing metric, with
    // nobody remembering to update the reverse index.
    {
        int unrecorded = 0;
        for (const auto &[key, users] : packUses) {
            const MetricDescriptor *d = cat.descriptor(key);
            if (!d) continue;   // already reported above

            const QSet<QString> declared = [&] {
                QSet<QString> s;
                for (const QString &u : d->usedBy)
                    if (u.startsWith(kPrefix)) s.insert(u.mid(kPrefix.size()));
                return s;
            }();

            for (const QString &cid : users)
                if (!declared.contains(cid)) {
                    ++unrecorded;
                    std::printf("        %s is used by '%s' but does not record it\n",
                                qPrintable(key), qPrintable(cid));
                }
        }
        check(unrecorded == 0, "every use in the pack is recorded in the catalogue's usedBy");
    }

    // ── 5. The pack's status never over-claims what the catalogue provides ──────
    // A measure marked `live` on a metric the catalogue calls planned would tell the user a value
    // is available when nothing can produce it — the single most damaging kind of disagreement
    // between these two registries.
    {
        int overclaimed = 0;
        for (const Measure &m : p.measures) {
            if (m.metricKey.isEmpty()) continue;
            const MetricDescriptor *d = cat.descriptor(m.metricKey);
            if (!d) continue;

            if (m.status == MeasureStatus::Live && d->planned) {
                ++overclaimed;
                std::printf("        measure '%s' claims live, but metric '%s' is planned\n",
                            qPrintable(m.id), qPrintable(m.metricKey));
            }
            if (m.status == MeasureStatus::Planned && !d->planned) {
                // Understating is harmless but still a disagreement worth surfacing.
                std::printf("        note: measure '%s' says planned, metric '%s' has a producer\n",
                            qPrintable(m.id), qPrintable(m.metricKey));
            }
        }
        check(overclaimed == 0, "no measure claims a producer the catalogue does not have");
    }

    // ── 6. Capture gaps are marked consistently in both registries ─────────────
    {
        int inconsistent = 0;
        for (const Measure &m : p.measures) {
            if (m.status != MeasureStatus::NotCapturable) continue;
            if (m.metricKey.isEmpty()) {
                ++inconsistent;
                std::printf("        capture gap '%s' names no metric\n", qPrintable(m.id));
                continue;
            }
            const MetricDescriptor *d = cat.descriptor(m.metricKey);
            if (!d) { ++inconsistent; continue; }
            // A capture gap must be catalogued as planned (never as having a producer) and must say
            // in its own text that it cannot be measured, or a reader of the catalogue alone would
            // reasonably expect it to arrive.
            if (!d->planned) {
                ++inconsistent;
                std::printf("        capture gap '%s' is not marked planned in the catalogue\n",
                            qPrintable(m.metricKey));
            }
            if (!d->howToRead.contains(QStringLiteral("NOT MEASURABLE"))) {
                ++inconsistent;
                std::printf("        capture gap '%s' does not say so in howToRead\n",
                            qPrintable(m.metricKey));
            }
        }
        check(inconsistent == 0, "capture gaps are marked as such in both registries");
    }

    // ── 7. New metrics are fully described ─────────────────────────────────────
    // A catalogue entry with an empty description is worse than no entry: it occupies the name and
    // teaches nobody anything.
    {
        int thin = 0;
        for (const auto &[key, users] : packUses) {
            const MetricDescriptor *d = cat.descriptor(key);
            if (!d) continue;
            const bool full = !d->label.isEmpty() && !d->shortLabel.isEmpty()
                              && !d->unit.isEmpty() && !d->group.isEmpty()
                              && d->description.size() > 80 && d->howToRead.size() > 80
                              && !d->phases.empty();
            if (!full) {
                ++thin;
                std::printf("        thinly described: %s\n", qPrintable(key));
            }
        }
        check(thin == 0, "every metric the pack depends on is fully described");
    }

    // ── 8. No brand names reached the catalogue either ─────────────────────────
    // The pack has this check; the catalogue is the other half of the same content surface.
    {
        const char *forbidden[] = { "titleist", "tpi", "trackman", "flightscope",
                                    "performance institute" };
        int hits = 0;
        for (const auto &[key, users] : packUses) {
            const MetricDescriptor *d = cat.descriptor(key);
            if (!d) continue;
            const QString blob = (d->description + d->howToRead + d->label).toLower();
            for (const char *needle : forbidden)
                if (blob.contains(QLatin1String(needle))) {
                    ++hits;
                    std::printf("        brand token '%s' in metric %s\n", needle, qPrintable(key));
                }
        }
        check(hits == 0, "no commercial brand is named in the metrics the pack depends on");
    }

    // ── The roadmap ranks by SERIES, not by reduced measure ────────────────────
    // One producer unblocks every reducer over its series. Pelvis lateral sway carries sway, slide
    // and hanging back at three phases, so it is ONE piece of work worth three characteristics —
    // and it must rank as such. Listing the three samples separately spreads it across three rows
    // of "unblocks 1" and buries the item that should lead, which is exactly what happened before
    // this was fixed.
    {
        CharacteristicLibraryModel model;
        const QVariantList         rows = model.roadmap();

        check(!rows.isEmpty(), "the roadmap has rows");

        int  pelvisSwayRows = 0, pelvisSwayBlocks = 0, pelvisSwaySamples = 0;
        for (const QVariant &v : rows) {
            const QVariantMap r = v.toMap();
            if (r.value(QStringLiteral("metricKey")).toString() != QStringLiteral("pelvisSway"))
                continue;
            ++pelvisSwayRows;
            pelvisSwayBlocks  = r.value(QStringLiteral("blocks")).toInt();
            pelvisSwaySamples = r.value(QStringLiteral("samples")).toInt();
        }
        check(pelvisSwayRows == 1, "a series with several reducers is ONE roadmap row");
        check(pelvisSwaySamples == 4, "that row knows it carries four reducers");
        check(pelvisSwayBlocks == 4, "and that it unblocks four characteristics");

        check(!rows.isEmpty()
                  && rows.first().toMap().value(QStringLiteral("blocks")).toInt() >= pelvisSwayBlocks,
              "rows are ranked by how much they unblock");

        // A capture gap must never appear as roadmap work, however many characteristics it blocks.
        bool gapInRoadmap = false;
        for (const QVariant &v : rows)
            if (v.toMap().value(QStringLiteral("status")) == QStringLiteral("notCapturable"))
                gapInRoadmap = true;
        check(!gapInRoadmap, "capture gaps never appear as roadmap work");
        // The seed pack currently has NO capture gaps: the two spinal measures were reclassified as
        // roadmap items once it was clear a down-the-line back-contour producer would resolve them.
        // The separation still has to hold — this asserts the rule, not a non-empty list.
        for (const QVariant &v : model.captureGaps())
            check(v.toMap().value(QStringLiteral("status")) == QStringLiteral("notCapturable"),
                  "anything under the capture-gap heading really is one");
        check(model.captureGaps().isEmpty(),
              "the seed pack has no capture gaps left — both spinal measures are roadmap items");

        // Every roadmap row names a metric that really exists, so the export is actionable.
        bool allNamed = true;
        for (const QVariant &v : rows) {
            const QString k = v.toMap().value(QStringLiteral("metricKey")).toString();
            if (k.isEmpty() || cat.descriptor(k) == nullptr) allNamed = false;
        }
        check(allNamed, "every roadmap row names a real catalogue metric");

        // The export must still SEPARATE the two, but with no capture gaps left there is no
        // section to emit — asserting its presence would pin content, not behaviour.
        const QString md = model.roadmapMarkdown();
        check(!md.isEmpty(), "the roadmap export is generated");
        check(model.captureGaps().isEmpty()
                  || md.contains(QStringLiteral("Not resolvable from current capture")),
              "a capture gap, if one exists, is exported under its own heading");
        check(md.contains(QStringLiteral("Causes, by how much they explain")),
              "the export carries the screen list alongside");
    }

    // ── The directory's free-text search ───────────────────────────────────────
    // The library directory filters through query()'s `search` key, so the box in the UI is
    // only as good as this. Checked against a label taken from the pack itself rather than a
    // hard-coded word, so the case survives content edits.
    {
        CharacteristicLibraryModel model;

        const QVariantList all = model.query(QVariantMap{});
        check(!all.isEmpty(), "the directory has rows to search");

        // A word from the middle of some row's label — a substring match, not a prefix one.
        const QVariantMap first = all.isEmpty() ? QVariantMap{} : all.first().toMap();
        const QString     label = first.value(QStringLiteral("label")).toString();
        const QString     id    = first.value(QStringLiteral("id")).toString();

        QVariantMap f;
        f.insert(QStringLiteral("search"), label);
        const QVariantList byLabel = model.query(f);
        bool foundByLabel = false;
        for (const QVariant &v : byLabel)
            if (v.toMap().value(QStringLiteral("id")).toString() == id) foundByLabel = true;
        check(foundByLabel, "searching a row's label finds that row");
        check(byLabel.size() <= all.size(), "search never adds rows");

        // Case-insensitive: a coach types lower case, the pack is written in sentence case.
        f.insert(QStringLiteral("search"), label.toUpper());
        check(model.query(f).size() == byLabel.size(), "search ignores case");

        // The id is searchable too — it is what a deep link, a swing.json and this test all
        // name a characteristic by, and it is invisible in the row.
        f.insert(QStringLiteral("search"), id);
        bool foundById = false;
        for (const QVariant &v : model.query(f))
            if (v.toMap().value(QStringLiteral("id")).toString() == id) foundById = true;
        check(foundById, "searching a row's id finds that row");

        f.insert(QStringLiteral("search"), QStringLiteral("zzzznothingmatchesthis"));
        check(model.query(f).isEmpty(), "a search that matches nothing returns nothing");

        // An empty search is not a filter — it must not quietly drop rows.
        f.insert(QStringLiteral("search"), QStringLiteral("   "));
        check(model.query(f).size() == all.size(), "a blank search filters nothing");
    }

    // ── Census ─────────────────────────────────────────────────────────────────
    {
        int live = 0, planned = 0, gap = 0;
        for (const auto &[key, users] : packUses) {
            const MetricDescriptor *d = cat.descriptor(key);
            if (!d) continue;
            const Measure *m = nullptr;
            for (const Measure &mm : p.measures)
                if (mm.metricKey == key) { m = &mm; break; }
            if (m && m->status == MeasureStatus::NotCapturable) ++gap;
            else if (d->planned) ++planned;
            else ++live;
        }
        std::printf("        (%d metrics referenced: %d with a producer, %d planned, %d capture gaps)\n",
                    int(packUses.size()), live, planned, gap);
    }

    // ── The reference registries reach the marshaller ──────────────────────────
    //
    // The trap this guards is the one the developer guide names twice: a field can be complete on
    // both sides and reach nothing, because QML reads `undefined` and renders silence. The screen
    // registry, the drill registry and the glossary are all new content whose ONLY route to a
    // reader is through these three invokables, so a marshaller that dropped a key would produce
    // three empty views and no error anywhere.
    std::printf("=== screens, drills and the glossary reach QML ===\n");
    {
        CharacteristicLibraryModel model;

        const QVariantList screens = model.screens();
        check(!screens.isEmpty(), "the screen registry is marshalled");
        int settling = 0, withProtocol = 0;
        for (const QVariant &v : screens) {
            const QVariantMap r = v.toMap();
            if (!r.value(QStringLiteral("protocol")).toString().isEmpty()) ++withProtocol;
            if (r.value(QStringLiteral("settlesCount")).toInt() > 0) ++settling;
        }
        check(withProtocol == screens.size(), "every screen row carries its protocol");
        check(settling > 0, "…and the join back to the conditions each would settle works");
        // Ranked by what they settle, which is the argument the model makes: a handful of physical
        // tests, needing no capture hardware, explain most of what the library detects.
        check(screens.first().toMap().value(QStringLiteral("settlesCount")).toInt()
                  >= screens.last().toMap().value(QStringLiteral("settlesCount")).toInt(),
              "screens are ranked by how much they settle, not alphabetically");

        const QVariantList drills = model.drills();
        check(!drills.isEmpty(), "the drill registry is marshalled");
        int answering = 0;
        for (const QVariant &v : drills)
            if (v.toMap().value(QStringLiteral("answersCount")).toInt() > 0) ++answering;
        check(answering > 0, "…and drills join back to the characteristics they answer");

        const QVariantList glossary = model.glossary();
        check(glossary.size() == int(p.conditions.size()),
              "the glossary covers every characteristic — it IS the rule set, not a subset of it");

        int withAliases = 0, withMeaning = 0;
        for (const QVariant &v : glossary) {
            const QVariantMap r = v.toMap();
            if (!r.value(QStringLiteral("aliases")).toStringList().isEmpty()) ++withAliases;
            if (!r.value(QStringLiteral("meaning")).toString().isEmpty()) ++withMeaning;
        }
        check(withMeaning == glossary.size(), "every entry says what it means");
        check(withAliases > 0, "…and the coach terms reached it");

        // The search is the whole point: a golfer types the word they were TAUGHT, which is
        // usually not the word the library was written in.
        const QVariantList byAlias = model.glossary(QStringLiteral("flip"));
        bool foundScooping = false;
        for (const QVariant &v : byAlias)
            if (v.toMap().value(QStringLiteral("id")).toString() == QLatin1String("scooping"))
                foundScooping = true;
        check(foundScooping, "searching a coach term finds the characteristic it names");
        check(model.glossary(QStringLiteral("zzzz-no-such-term")).isEmpty(),
              "…and a term nothing answers to returns nothing, rather than everything");

        // ── The bibliography ────────────────────────────────────────────────
        //
        // Same trap, one registry later. The References view's whole value is the PAIRING — this
        // paper, and the claims resting on it at the tier each earned — so a marshaller that
        // shipped the papers and dropped `cites` would render a plausible, useless appendix and
        // nothing would report it.
        const QVariantList refs = model.references();
        check(!refs.isEmpty(), "the reference registry is marshalled");

        int withUrl = 0, withCites = 0, withTier = 0, totalCites = 0;
        for (const QVariant &v : refs) {
            const QVariantMap r = v.toMap();
            if (r.value(QStringLiteral("url")).toString().startsWith(QLatin1String("https://doi.org/")))
                ++withUrl;
            const QVariantList cites = r.value(QStringLiteral("cites")).toList();
            if (!cites.isEmpty()) ++withCites;
            totalCites += cites.size();
            for (const QVariant &cv : cites) {
                const QVariantMap c = cv.toMap();
                // Every claim row needs the tier LABEL (what the chip renders) and the id the tap
                // navigates to. Either one missing and the row is decoration.
                if (!c.value(QStringLiteral("tierLabel")).toString().isEmpty()
                    && !c.value(QStringLiteral("fromId")).toString().isEmpty())
                    ++withTier;
            }
        }
        check(withUrl == refs.size(), "every reference carries an openable doi.org URL");
        check(withCites > 0, "references carry the claims that rest on them");
        check(totalCites > 0 && withTier == totalCites,
              "and every claim row carries its tier label and a target to navigate to");

        // Ordering is the argument: the paper four claims rest on is a different kind of object
        // from the one cited once, and an alphabetical bibliography hides exactly that.
        int prev = 1 << 30;
        bool descending = true;
        for (const QVariant &v : refs) {
            const int n = v.toMap().value(QStringLiteral("citeCount")).toInt();
            if (n > prev) descending = false;
            prev = n;
        }
        check(descending, "references are ordered by how much of the library they hold up");
    }

    // ── Every Connections handler names a signal that exists ────────────────────
    //
    // `Connections { target: library; function onLibraryChanged() {…} }` is not an error at build
    // time and not an error at load time. It warns at INSTANTIATION — which for a view inside a
    // lazily-loaded settings panel means the first time a human opens that panel, and never in a
    // headless start. So it shipped, and the only thing that caught it was Mark opening the page.
    //
    // The handler is a string and the signal list is in the metaobject, so the check is a join.
    // This is the cheapest available answer to a class of defect that no compile, no test and no
    // screenshot can otherwise see: the view goes on rendering, it simply stops updating.
    std::printf("=== every Connections handler on the model names a real signal ===\n");
    {
        const QMetaObject *mo = &CharacteristicLibraryModel::staticMetaObject;
        QSet<QString>      handlers;                    // "onHealthChanged", …
        for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
            const QMetaMethod m = mo->method(i);
            if (m.methodType() != QMetaMethod::Signal) continue;
            QString n = QString::fromLatin1(m.name());
            handlers.insert(QStringLiteral("on") + n.at(0).toUpper() + n.mid(1));
        }
        // Q_PROPERTY NOTIFY signals count too — a view keying off one is legitimate.
        check(!handlers.isEmpty(), "the model exposes signals to connect to");

        const QDir      dir(QStringLiteral(PP_DIAG_QML_DIR));
        const QFileInfoList files = dir.entryInfoList({ QStringLiteral("*.qml") }, QDir::Files);
        check(!files.isEmpty(), "the diagnostics QML directory was found");

        // Only blocks whose target is the library model — a Connections on anything else is not
        // ours to judge from here.
        const QRegularExpression block(
            QStringLiteral("Connections\\s*\\{[^}]*?target:\\s*[A-Za-z_.]*\\blibrary\\b[^}]*?\\}"),
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpression fn(QStringLiteral("function\\s+(on[A-Za-z0-9_]+)\\s*\\("));

        int checked = 0, bogus = 0;
        for (const QFileInfo &fi : files) {
            QFile qf(fi.absoluteFilePath());
            if (!qf.open(QIODevice::ReadOnly)) continue;
            const QString src = QString::fromUtf8(qf.readAll());

            auto bit = block.globalMatch(src);
            while (bit.hasNext()) {
                const QString body = bit.next().captured(0);
                auto          hit  = fn.globalMatch(body);
                while (hit.hasNext()) {
                    const QString h = hit.next().captured(1);
                    ++checked;
                    if (!handlers.contains(h)) {
                        ++bogus;
                        std::printf("        %s connects '%s', which the model does not emit\n",
                                    qPrintable(fi.fileName()), qPrintable(h));
                    }
                }
            }
        }
        std::printf("        (%d handlers checked across %d files)\n", checked, int(files.size()));
        check(checked > 0, "there are handlers to check — the regex still matches the QML");
        check(bogus == 0, "no view connects a signal the model does not have");
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
