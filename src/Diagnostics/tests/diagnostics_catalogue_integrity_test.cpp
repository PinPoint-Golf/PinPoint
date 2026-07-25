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
        check(pelvisSwaySamples == 3, "that row knows it carries three reducers");
        check(pelvisSwayBlocks == 3, "and that it unblocks three characteristics");

        check(!rows.isEmpty()
                  && rows.first().toMap().value(QStringLiteral("blocks")).toInt() >= pelvisSwayBlocks,
              "rows are ranked by how much they unblock");

        // A capture gap must never appear as roadmap work, however many characteristics it blocks.
        bool gapInRoadmap = false;
        for (const QVariant &v : rows)
            if (v.toMap().value(QStringLiteral("status")) == QStringLiteral("notCapturable"))
                gapInRoadmap = true;
        check(!gapInRoadmap, "capture gaps never appear as roadmap work");
        check(!model.captureGaps().isEmpty(), "...they appear under their own heading instead");

        // Every roadmap row names a metric that really exists, so the export is actionable.
        bool allNamed = true;
        for (const QVariant &v : rows) {
            const QString k = v.toMap().value(QStringLiteral("metricKey")).toString();
            if (k.isEmpty() || cat.descriptor(k) == nullptr) allNamed = false;
        }
        check(allNamed, "every roadmap row names a real catalogue metric");

        const QString md = model.roadmapMarkdown();
        check(md.contains(QStringLiteral("Not resolvable from current capture")),
              "the export separates capture gaps from the work queue");
        check(md.contains(QStringLiteral("Causes, by how much they explain")),
              "the export carries the screen list alongside");
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

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
