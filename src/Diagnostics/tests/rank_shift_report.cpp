// rank_shift_report — what wiring `prominenceWeight()` into the ranking actually reordered.
//
// NOT A TEST, and deliberately not registered with ctest. `characteristic.h` requires that anything
// moving these weights measures the reordering the way the strength re-cut did; this is that
// measurement. It must not become a gate: an assertion like "98% of pairings are preserved" would
// pin the shipped prominence values, and the first coach-led re-rank would fail a gate nobody
// wanted. That is the frozen-parity mistake this repo has already deleted twice. The OUTPUT is the
// deliverable, and it belongs in the commit message.
//
//   cmake --build build/tests-Analysis --target rank_shift_report
//   ./build/tests-Analysis/rank_shift_report
//
// THE CONTROL NEEDS NO FLAG, which is the one idea here worth keeping. Both runs go through the
// same new code; the difference is the pack. With every prominence forced to one rung,
// score(c) = k * SUM w(c,e) — a positive scalar multiple of the pre-change score — so the ordering
// that run produces IS the old ordering, exactly. No `ExplainOptions`, and nothing reimplements the
// greedy pass, so what is measured is the code that ships rather than a copy of it.

#include "../characteristic_pack.h"
#include "../relation_resolver.h"

#include <QFile>

#include <algorithm>
#include <cstdio>
#include <map>

using namespace pinpoint::analysis;

namespace {

// One synthetic diagnosis. `explain()` reads only {conditionId, Fired, confidence, material} off a
// DetectionResult, so there is no need for detect(), a measure source, norms or a context tree.
DetectionResult firedSet(const QStringList &ids)
{
    DetectionResult d;
    for (const QString &id : ids) {
        Finding f;
        f.conditionId = id;
        f.state       = FindingState::Fired;
        f.confidence  = 1.0f;
        f.material    = true;
        d.findings.push_back(f);
    }
    return d;
}

CharacteristicPack flattened(const CharacteristicPack &src, Prominence rung)
{
    CharacteristicPack p = src;
    for (Condition &c : p.conditions) c.prominence = rung;
    return p;
}

QStringList rootOrder(const Explanation &ex)
{
    QStringList out;
    for (const RankedCause &rc : ex.roots) out << rc.conditionId;
    return out;
}

// The CANDIDATE score — what a cause is worth over everything it covers, before the greedy cover
// trims it.
//
// This is not the number `Explanation::roots` carries, and the difference matters enough to be worth
// the eight lines. The greedy pass re-scores each surviving cause over only the findings still
// uncovered, so once the two runs pick a different cause first, every score after that is computed
// over a different remaining set. Printing those side by side compares two different quantities and
// makes near-ties look like landslides — the first version of this report did exactly that, and
// showed a 4x margin being overturned that did not exist.
//
// So the ORDER comparison below reads the greedy output, because that order is what a coach is shown
// and is the thing that actually changed. The MARGINS are these, because they isolate the weight
// from the cover dynamics and are comparable across runs. Reimplementing one summation is worth it;
// reimplementing the cover would not be.
double candidateScore(const CharacteristicPack &pack, const DetectionResult &d, const QString &cause)
{
    const double base = pack.condition(cause)
                            ? prominenceWeight(pack.condition(cause)->prominence) : 0.0;
    double sum = 0.0;
    for (const QString &effect : findingsCoveredBy(pack, d, cause))
        for (const Edge &e : pack.edges)
            if (e.type == EdgeType::Causes && e.from == cause && e.to == effect)
                sum += base * strengthWeight(e.strength);
    return sum;
}

} // namespace

int main()
{
    QFile f(QStringLiteral(PP_CORE_PACK_PATH));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("cannot open %s\n", PP_CORE_PACK_PATH);
        return 1;
    }
    const PackLoadResult res = loadPack(f.readAll(), QStringLiteral("core.json"));
    if (!res.loaded) { std::printf("core.json does not load\n"); return 1; }

    const CharacteristicPack &shipped = res.pack;
    const CharacteristicPack  control = flattened(shipped, Prominence::Occasional);

    std::printf("rank_shift_report — %d conditions, %d edges\n\n",
                int(shipped.conditions.size()), int(shipped.edges.size()));

    // The universe is EVERY condition, not the subset that can fire today. The model is authored
    // ahead of its producers on purpose, so a measurement scoped to what is currently detectable
    // would measure the pipeline rather than the model — and would go stale every time a producer
    // landed.
    QStringList all;
    for (const Condition &c : shipped.conditions) all << c.id;

    // Deterministic and seedless. Three families, chosen because they are the shapes a real
    // diagnosis takes: one finding, a cause-and-effect pair, and a fan of several findings that a
    // single upstream cause might account for. Strides rather than a generator so the same sets are
    // produced on every machine and every run — a measurement nobody can reproduce is an assertion.
    std::vector<QStringList> sets;
    for (const QString &id : all) sets.push_back({ id });
    for (const Edge &e : shipped.edges)
        if (e.type == EdgeType::Causes) sets.push_back({ e.from, e.to });
    for (int stride = 3; stride <= 11; stride += 2)
        for (int start = 0; start < stride; ++start) {
            QStringList fan;
            for (int i = start; i < all.size(); i += stride) fan << all.at(i);
            if (fan.size() >= 2) sets.push_back(fan);
        }

    long long pairsTotal = 0, pairsPreserved = 0;
    int       setsWithRoots = 0, setsReordered = 0;

    // Every reordered pair, with both candidate scores, so "the pairs it reorders are near-ties" is
    // a claim a reader can check rather than take. Keyed on the pair so one cause pairing that flips
    // in nine different finding sets is reported once rather than nine times.
    //
    // TWO KINDS OF REORDERING LIVE IN THIS COUNT and they mean different things, so they are split.
    // A pair is SCORE-DRIVEN when the candidate scores themselves swapped — prominence genuinely
    // decided that pairing, which is the change doing its job and the thing to review. The rest are
    // the greedy cover reshuffling: once it picks a different cause first, what remains uncovered
    // changes, and causes downstream of that pick move without any score having inverted. Those are
    // consequences of the first pick, not independent judgements, and counting them as "prominence
    // reordered this" would overstate the change.
    struct Flip { QString a, b; double sa, sb, ca, cb; int seen; bool scoreDriven; };
    std::map<QString, Flip> flips;
    long long pairsScoreDriven = 0;

    for (const QStringList &ids : sets) {
        const DetectionResult d = firedSet(ids);
        const Explanation     now  = explain(shipped, d);
        const Explanation     then = explain(control, d);
        if (now.roots.empty()) continue;
        ++setsWithRoots;

        const QStringList a = rootOrder(now), b = rootOrder(then);
        std::map<QString, double> scoreNow, scoreThen;
        for (const RankedCause &rc : now.roots)
            scoreNow[rc.conditionId] = candidateScore(shipped, d, rc.conditionId);
        for (const RankedCause &rc : then.roots)
            scoreThen[rc.conditionId] = candidateScore(control, d, rc.conditionId);

        bool anyFlip = false;
        for (int i = 0; i < a.size(); ++i)
            for (int j = i + 1; j < a.size(); ++j) {
                const int bi = b.indexOf(a.at(i)), bj = b.indexOf(a.at(j));
                if (bi < 0 || bj < 0) continue;      // a cause the control did not rank at all
                ++pairsTotal;
                if (bi < bj) { ++pairsPreserved; continue; }
                anyFlip = true;
                const double sa = scoreNow[a.at(i)],  sb = scoreNow[a.at(j)];
                const double ca = scoreThen[a.at(i)], cb = scoreThen[a.at(j)];
                const bool driven = (ca < cb) && (sa > sb);   // the candidate scores swapped
                if (driven) ++pairsScoreDriven;
                const QString key = a.at(i) + QLatin1Char('|') + a.at(j);
                auto it = flips.find(key);
                if (it == flips.end())
                    flips.insert({ key, Flip{ a.at(i), a.at(j), sa, sb, ca, cb, 1, driven } });
                else
                    ++it->second.seen;
            }
        if (anyFlip) ++setsReordered;
    }

    std::printf("%d synthetic finding sets, %d of which produced a ranking\n",
                int(sets.size()), setsWithRoots);
    std::printf("%lld cause pairings compared, %lld preserved (%.1f%%)\n",
                pairsTotal, pairsPreserved,
                pairsTotal ? 100.0 * double(pairsPreserved) / double(pairsTotal) : 100.0);
    std::printf("  of the %lld reordered, %lld are SCORE-DRIVEN (%.1f%% of all pairings) and %lld\n"
                "  follow from the greedy cover picking differently\n",
                pairsTotal - pairsPreserved, pairsScoreDriven,
                pairsTotal ? 100.0 * double(pairsScoreDriven) / double(pairsTotal) : 0.0,
                pairsTotal - pairsPreserved - pairsScoreDriven);
    std::printf("%d of %d ranked sets reordered at all\n\n", setsReordered, setsWithRoots);

    if (flips.empty()) { std::printf("nothing reordered.\n"); return 0; }

    // Score-driven first, then by how CLOSE the control scores were — a pairing the old ranking
    // separated confidently and the new one inverts is the one that has to be defensible, and it
    // belongs at the top where it cannot be missed.
    std::vector<Flip> byMargin;
    for (const auto &kv : flips) byMargin.push_back(kv.second);
    std::sort(byMargin.begin(), byMargin.end(), [](const Flip &x, const Flip &y) {
        if (x.scoreDriven != y.scoreDriven) return x.scoreDriven;
        const double mx = x.ca ? std::abs(x.ca - x.cb) / x.ca : 0.0;
        const double my = y.ca ? std::abs(y.ca - y.cb) / y.ca : 0.0;
        return mx > my;
    });

    int driven = 0;
    for (const Flip &fl : byMargin) if (fl.scoreDriven) ++driven;
    std::printf("%d distinct cause pairings reordered; the %d score-driven ones first, widest\n"
                "control margin first within each group:\n\n", int(byMargin.size()), driven);
    std::printf("  %-32s %-32s %11s %11s  %s\n",
                "now ranked above", "…this", "was", "now", "seen");
    for (const Flip &fl : byMargin)
        std::printf("%s %-32s %-32s %5.2f/%-5.2f %5.2f/%-5.2f x%d\n",
                    fl.scoreDriven ? "!" : " ",
                    qPrintable(fl.a), qPrintable(fl.b), fl.ca, fl.cb, fl.sa, fl.sb, fl.seen);

    return 0;
}
