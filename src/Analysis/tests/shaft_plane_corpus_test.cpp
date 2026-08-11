// GOLDEN CROSS-CHECK for the face-on swing-plane producer (shaft_plane.h)
// against the corpus reference the research note published
// (docs/research/data/wrist_cock_model/transition_plane_corpus.csv, produced by
// `python3 tools/shaftlab/plane_probe.py corpus`).
//
// It reads the EXISTING result.json at each swing directory — no pipeline
// re-run, no decode, no OpenCV, and nothing under the corpus root is written —
// reconstructs the measured shaft-vector samples exactly as plane_probe.load_run
// selects them, and diffs every column the reference emits.
//
// SKIPS (ctest "Skipped", exit 77) when the corpus is not mounted, so a
// developer without /mnt/swingdata still gets a green suite. The golden CSV is
// in-tree, so ITS absence is a real failure.
//
//   cmake --build build/analyzer-tests --target shaft_plane_corpus_test
//   ctest --test-dir build/analyzer-tests -R shaft_plane_corpus --output-on-failure
//
// Point it elsewhere without reconfiguring:
//   PINPOINT_PLANE_CORPUS=/some/run/root ./shaft_plane_corpus_test

#include "../shaft_plane.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

using namespace pinpoint::analysis;

#ifndef PP_PLANE_CORPUS_ROOT
#define PP_PLANE_CORPUS_ROOT "/mnt/swingdata/stagegate/corpm3-off"
#endif
#ifndef PP_PLANE_GOLDEN_CSV
#define PP_PLANE_GOLDEN_CSV "transition_plane_corpus.csv"
#endif

// Phase ladder ints as serialized (Phase enum is append-only and persists raw).
static constexpr int kPhAddress = 0, kPhTakeaway = 1, kPhTop = 2, kPhImpact = 5;
static constexpr int kFlagSynthesized = 0x100;   // ShaftSampleFlags::ShaftSynthesized

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// ── The golden file ─────────────────────────────────────────────────────────
struct GoldenRow {
    double iotaBack = 0, iotaDown = 0, delta = 0, nodeBack = 0, nodeDown = 0;
    int    nBack = 0, nDown = 0;
    double residBack = 0, residDown = 0;
    double splitBack = -1, splitDown = -1;   // −1 = the CSV cell was empty
};

// ── What load_run would have produced for one swing ─────────────────────────
enum class LoadReject { Ok, NoSamples, NoLadder, NoFrameSize, TooFewUsable };

struct Loaded {
    LoadReject reject = LoadReject::Ok;
    std::vector<ShaftPlanePoint> measured, synth;
    std::vector<PlaneAnchor>     anchors;
    std::int64_t takeawayUs = 0, topUs = 0, impactUs = 0;
};

// Replicates plane_probe.load_run EXACTLY. Every gate below moves the 33/28
// split if it drifts, including the Address requirement — Address is unused by
// the fit, but load_run demands it, so a swing missing only Address is a
// "no usable shaft samples" skip and not a conic failure.
static Loaded loadRun(const QString &resultPath)
{
    Loaded out;
    QFile f(resultPath);
    if (!f.open(QIODevice::ReadOnly)) { out.reject = LoadReject::NoSamples; return out; }
    const QJsonObject an = QJsonDocument::fromJson(f.readAll()).object()
                               .value(QStringLiteral("analysis")).toObject();
    const QJsonObject club = an.value(QStringLiteral("club")).toObject();
    const QJsonArray  samples = club.value(QStringLiteral("samples")).toArray();
    if (samples.isEmpty()) { out.reject = LoadReject::NoSamples; return out; }

    // setdefault semantics: FIRST occurrence of each phase wins.
    std::map<int, std::int64_t> ev;
    for (const QJsonValue &pv : an.value(QStringLiteral("phases")).toArray()) {
        const QJsonObject p = pv.toObject();
        ev.emplace(p.value(QStringLiteral("phase")).toInt(),
                   std::int64_t(p.value(QStringLiteral("t_us")).toDouble()));
    }
    for (const int need : { kPhAddress, kPhTakeaway, kPhTop, kPhImpact })
        if (!ev.count(need)) { out.reject = LoadReject::NoLadder; return out; }

    const double W = club.value(QStringLiteral("frameWidth")).toDouble();
    const double H = club.value(QStringLiteral("frameHeight")).toDouble();
    if (!(W != 0.0 && H != 0.0)) { out.reject = LoadReject::NoFrameSize; return out; }

    out.takeawayUs = ev[kPhTakeaway];
    out.topUs      = ev[kPhTop];
    out.impactUs   = ev[kPhImpact];

    // The measured channel: head and grip both present, headConf > 0 (an absent
    // key reads 0.0, NOT −1), and the synth flag clear. Denormalised to px.
    for (const QJsonValue &sv : samples) {
        const QJsonObject s = sv.toObject();
        const QJsonArray head = s.value(QStringLiteral("head")).toArray();
        const QJsonArray grip = s.value(QStringLiteral("grip")).toArray();
        if (head.isEmpty() || grip.isEmpty()) continue;
        if (!(s.value(QStringLiteral("headConf")).toDouble(0.0) > 0.0)) continue;
        if (s.value(QStringLiteral("flags")).toInt(0) & kFlagSynthesized) continue;
        out.measured.push_back({ std::int64_t(s.value(QStringLiteral("t_us")).toDouble()),
                                 (head.at(0).toDouble() - grip.at(0).toDouble()) * W,
                                 (head.at(1).toDouble() - grip.at(1).toDouble()) * H });
    }
    // load_run's ">= 12" gate is on the WHOLE track, before any windowing.
    if (out.measured.size() < 12) { out.reject = LoadReject::TooFewUsable; return out; }

    // The synth tier — not part of the golden comparison, only of the mirror
    // stat. No headConf filter: the Hermite carries a decayed conf, not a head
    // measurement.
    for (const QJsonValue &sv : club.value(QStringLiteral("synth")).toArray()) {
        const QJsonObject s = sv.toObject();
        const QJsonArray head = s.value(QStringLiteral("head")).toArray();
        const QJsonArray grip = s.value(QStringLiteral("grip")).toArray();
        if (head.isEmpty() || grip.isEmpty()) continue;
        out.synth.push_back({ std::int64_t(s.value(QStringLiteral("t_us")).toDouble()),
                              (head.at(0).toDouble() - grip.at(0).toDouble()) * W,
                              (head.at(1).toDouble() - grip.at(1).toDouble()) * H });
    }
    for (const QJsonValue &pv : club.value(QStringLiteral("positions")).toArray()) {
        const QJsonObject p = pv.toObject();
        out.anchors.push_back({ std::int64_t(p.value(QStringLiteral("t_us")).toDouble()),
                                float(p.value(QStringLiteral("conf")).toDouble(-1.0)) });
    }
    return out;
}

// Spearman with average ranks on ties, so a future run is comparable.
static double spearman(std::vector<double> a, std::vector<double> b)
{
    const std::size_t n = a.size();
    if (n < 3 || b.size() != n) return 0.0;
    auto rank = [n](const std::vector<double> &v) {
        std::vector<std::size_t> idx(n);
        for (std::size_t i = 0; i < n; ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&v](std::size_t x, std::size_t y) { return v[x] < v[y]; });
        std::vector<double> r(n);
        for (std::size_t i = 0; i < n; ) {
            std::size_t j = i;
            while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
            const double avg = 0.5 * (double(i) + double(j)) + 1.0;
            for (std::size_t k = i; k <= j; ++k) r[idx[k]] = avg;
            i = j + 1;
        }
        return r;
    };
    const std::vector<double> ra = rank(a), rb = rank(b);
    double ma = 0, mb = 0;
    for (std::size_t i = 0; i < n; ++i) { ma += ra[i]; mb += rb[i]; }
    ma /= n; mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = ra[i] - ma, db = rb[i] - mb;
        sab += da * db; saa += da * da; sbb += db * db;
    }
    return (saa > 0 && sbb > 0) ? sab / std::sqrt(saa * sbb) : 0.0;
}

static double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

int main()
{
    const QString corpusRoot = qEnvironmentVariableIsSet("PINPOINT_PLANE_CORPUS")
        ? qEnvironmentVariable("PINPOINT_PLANE_CORPUS")
        : QStringLiteral(PP_PLANE_CORPUS_ROOT);
    const QString goldenPath = qEnvironmentVariableIsSet("PINPOINT_PLANE_GOLDEN")
        ? qEnvironmentVariable("PINPOINT_PLANE_GOLDEN")
        : QStringLiteral(PP_PLANE_GOLDEN_CSV);

    std::printf("=== shaft plane corpus cross-check ===\n");
    std::printf("  corpus: %s\n  golden: %s\n",
                qPrintable(corpusRoot), qPrintable(goldenPath));

    QDir dir(corpusRoot);
    if (!dir.exists()) {
        std::printf("  corpus not mounted — SKIPPED\n");
        return 77;
    }

    // ── the golden file ─────────────────────────────────────────────────────
    QFile gf(goldenPath);
    if (!gf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::printf("  [FAIL] the golden CSV is in-tree and must be readable\n");
        return 1;
    }
    const QStringList lines = QString::fromUtf8(gf.readAll()).split(QLatin1Char('\n'),
                                                                   Qt::SkipEmptyParts);
    std::map<QString, GoldenRow> golden;
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList c = lines.at(i).trimmed().split(QLatin1Char(','));
        if (c.size() < 14) continue;
        GoldenRow g;
        g.iotaBack = c[3].toDouble();  g.iotaDown = c[4].toDouble();
        g.delta    = c[5].toDouble();
        g.nodeBack = c[6].toDouble();  g.nodeDown = c[7].toDouble();
        g.nBack    = c[8].toInt();     g.nDown    = c[9].toInt();
        g.residBack = c[10].toDouble(); g.residDown = c[11].toDouble();
        g.splitBack = c[12].isEmpty() ? -1.0 : c[12].toDouble();
        g.splitDown = c[13].isEmpty() ? -1.0 : c[13].toDouble();
        golden.emplace(c[0], g);
    }
    std::printf("  golden rows: %d\n", int(golden.size()));
    check(golden.size() == 33, "the golden file carries 33 measured-channel rows");

    // ── walk the corpus ─────────────────────────────────────────────────────
    int nDirs = 0, nFitted = 0;
    int skipNoUsable = 0, skipBack = 0, skipDown = 0, skipBoth = 0;
    double worstAngleErr = 0.0, worstResidErr = 0.0, worstSplitErr = 0.0;
    std::set<QString> fittedNames;
    QStringList mismatches, countMismatches, backOnlyFailures, illConditioned;
    QStringList splitMismatches, splitRefused;

    // THE ONE DELIBERATE DIVERGENCE from tools/shaftlab/plane_probe.py, pinned by
    // NAME so it reads as a decision and never as drift. The reference has no
    // conditioning floor, so these three of its 33 fits are needles — one window
    // each collapsed to an axis ratio far below kMinAxisRatio, giving iota 81.7,
    // 82.9 and 89.0 and deltas of -58.5, +46.6 and +56.7 that no golf swing
    // performs. All three are from 2026-07-04, the session the research note's §12
    // flags as pathological, and in every case it is the SPARSER window. Their
    // split-halves were 'n/a', 4.29 and 0.00 respectively — which is exactly why
    // the gate is on the axis ratio and not on the split-half.
    const QStringList kKnownNeedles = {
        QStringLiteral("2026-07-04_Mark-Liversedge_Wrist_01__swing_0005"),   // down ι 81.72, δ −58.46
        QStringLiteral("2026-07-04_Mark-Liversedge_Wrist_01__swing_0006"),   // back ι 82.89, δ +46.59
        QStringLiteral("2026-07-04_Mark-Liversedge_Wrist_01__swing_0010"),   // back ι 89.03, δ +56.74
    };
    // mirror-stat accumulators (brief §8.3): recorded, never gated.
    int synthFitted = 0, bothFitted = 0;
    std::vector<double> mDelta, sDelta, absGap;

    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : entries) {
        const QString rj = dir.filePath(name) + QStringLiteral("/result.json");
        if (!QFile::exists(rj)) continue;
        ++nDirs;

        const Loaded ld = loadRun(rj);
        if (ld.reject != LoadReject::Ok) { ++skipNoUsable; continue; }

        ShaftPlaneInput in;
        in.measured    = ld.measured;
        in.synth       = ld.synth;
        in.anchors     = ld.anchors;
        in.takeawayUs  = ld.takeawayUs;
        in.topUs       = ld.topUs;
        in.impactUs    = ld.impactUs;
        in.haveWindows = true;
        const ShaftPlaneResult r = fitShaftPlane(in);

        if (r.synth.fitted) ++synthFitted;
        if (r.measured.fitted && r.synth.fitted) {
            ++bothFitted;
            mDelta.push_back(r.measured.deltaDeg);
            sDelta.push_back(r.synth.deltaDeg);
            absGap.push_back(std::fabs(r.measured.deltaDeg - r.synth.deltaDeg));
        }

        if (!r.measured.fitted) {
            if (r.measured.back.fit.reject == ConicReject::IllConditioned
                || r.measured.down.fit.reject == ConicReject::IllConditioned)
                illConditioned << name;
            const bool bBad = !r.measured.back.fit.ok, dBad = !r.measured.down.fit.ok;
            if (bBad && dBad) ++skipBoth;
            else if (bBad)    { ++skipBack; backOnlyFailures << name; }
            else              ++skipDown;
            continue;
        }
        ++nFitted;
        fittedNames.insert(name);

        const auto it = golden.find(name);
        if (it == golden.end()) {
            mismatches << (name + QStringLiteral(" (fitted here, absent from the golden file)"));
            continue;
        }
        const GoldenRow &g = it->second;
        const ConicFit &b = r.measured.back.fit;
        const ConicFit &d = r.measured.down.fit;

        auto angle = [&](double got, double want) {
            worstAngleErr = std::max(worstAngleErr, std::fabs(got - want));
            return std::fabs(got - want) <= 0.1;
        };
        bool ok = angle(b.iotaDeg, g.iotaBack) && angle(d.iotaDeg, g.iotaDown)
               && angle(r.measured.deltaDeg, g.delta)
               && angle(b.nodeDeg, g.nodeBack) && angle(d.nodeDeg, g.nodeDown);
        if (!ok) mismatches << name;

        if (b.n != g.nBack || d.n != g.nDown)
            countMismatches << QStringLiteral("%1 n=(%2,%3) golden=(%4,%5)")
                                  .arg(name).arg(b.n).arg(d.n).arg(g.nBack).arg(g.nDown);

        // The CSV rounds the residual to 4 dp; the split-halves are full precision.
        worstResidErr = std::max({ worstResidErr, std::fabs(b.conicResid - g.residBack),
                                   std::fabs(d.conicResid - g.residDown) });
        auto splitErr = [&](double got, double want, const char *w) {
            if (want < 0.0) {                                 // empty cell ⇒ must be −1
                if (got != -1.0) splitMismatches << QStringLiteral("%1 %2: got %3, golden empty")
                                                        .arg(name, QLatin1String(w)).arg(got);
                return;
            }
            if (got < 0.0) {   // the conditioning floor refused a HALF-fit the reference accepted
                splitRefused << QStringLiteral("%1 %2 (golden %3)")
                                    .arg(name, QLatin1String(w)).arg(want, 0, 'f', 2);
                return;
            }
            worstSplitErr = std::max(worstSplitErr, std::fabs(got - want));
        };
        splitErr(r.measured.back.splitHalfDeg, g.splitBack, "back");
        splitErr(r.measured.down.splitHalfDeg, g.splitDown, "down");
    }

    std::printf("\n  swings scanned: %d   measured-channel fits: %d   skips: %d\n",
                nDirs, nFitted, nDirs - nFitted);
    std::printf("  skip reasons: no usable shaft samples %d, down window %d, back window %d, both %d\n",
                skipNoUsable, skipDown, skipBack, skipBoth);
    std::printf("  worst error vs golden: angle %.6f deg, conic residual %.6g, split-half %.6g\n",
                worstAngleErr, worstResidErr, worstSplitErr);
    for (const QString &m : mismatches)      std::printf("    MISMATCH  %s\n", qPrintable(m));
    for (const QString &m : countMismatches) std::printf("    COUNT     %s\n", qPrintable(m));

    check(nDirs == 61, "61 swing directories at the run root");
    check(nFitted == 30, "the measured channel fits both windows on 30 swings (33 - 3 needles)");

    // The fitted set must be the golden column MINUS exactly the three named
    // needles — no extras, and no accidental extra losses. Asserting the
    // difference by name is the point: a blanket parity check would have hidden
    // the conditioning floor, and a bare count would let a DIFFERENT swing drop
    // out unnoticed.
    QStringList extra, missing;
    for (const QString &n : fittedNames)
        if (!golden.count(n)) extra << n;
    for (const auto &kv : golden)
        if (!fittedNames.count(kv.first)) missing << kv.first;
    missing.sort();
    QStringList wantMissing = kKnownNeedles;
    wantMissing.sort();
    check(extra.isEmpty(), "no swing fits here that the reference did not fit");
    check(missing == wantMissing,
          "the ONLY swings dropped vs the reference are the three named needles");
    for (const QString &m : missing) std::printf("    dropped vs reference: %s\n", qPrintable(m));

    // …and each was dropped for the RIGHT reason, not merely dropped.
    QStringList illSorted = illConditioned;
    illSorted.sort();
    check(illSorted == wantMissing, "each was refused as IllConditioned, by the axis-ratio floor");

    check(mismatches.isEmpty(), "every fitted swing matches iota/delta/node within 0.1 deg");
    check(countMismatches.isEmpty(),
          "sample counts match exactly (the inclusive window bounds are preserved)");
    check(worstResidErr <= 1e-3, "conic residuals match the golden file to its 4-dp rounding");
    check(worstSplitErr <= 1e-6, "split-halves match wherever both sides computed one");
    check(splitMismatches.isEmpty(), "no split-half appears where the reference had none");
    for (const QString &m : splitRefused)
        std::printf("    split-half refused by the ratio floor: %s\n", qPrintable(m));
    // The floor applies to the HALF-fits too, which is deliberate: a split-half
    // computed from two needles is not an error bar, it is two artifacts agreeing.
    // Exactly one corpus row is affected — swing_0007's downswing passes the floor
    // as a whole (ι 71.66) while one of its halves does not — and the reference
    // value it replaces, 6.30°, was already past the 5° mark the brief §5 uses to
    // flag a fit as low quality. "Cannot compute" is the more honest answer there.
    check(splitRefused.size() == 1
          && splitRefused.first().startsWith(QStringLiteral("2026-06-11_Mark-Liversedge_Wrist_01__swing_0007 down")),
          "exactly one split-half is refused by the floor, and it is the known 6.30° row");

    // The skip census the reference reports.
    check(skipNoUsable == 2, "2 swings carry no usable shaft samples");
    // 25 -> 26 and 1 -> 3: the three needles join the census, one in the down
    // window (swing_0005) and two in the back (swing_0006, swing_0010).
    check(skipDown == 26, "26 swings fail on the downswing window (25 sparse + 1 needle)");
    check(skipBack == 3, "3 swings fail on the backswing window (1 sparse + 2 needles)");
    check(skipBoth == 0, "no swing fails both windows");
    for (const QString &n : backOnlyFailures)
        std::printf("    back-window failure: %s\n", qPrintable(n));

    // ── The mirror stat (brief §8.3): RECORDED, NEVER GATED ─────────────────
    // No pass/fail here on purpose. This number IS the experiment — whether the
    // synth plane tracks the measured plane — and §9 makes it the evidence any
    // future promotion of the synth channel has to argue from. Asserting it
    // would freeze a baseline that is supposed to move as data accumulates.
    std::printf("\n  --- mirror stat (recorded, not gated) ---\n");
    std::printf("  measured-channel yield : %d / %d\n", nFitted, nDirs);
    std::printf("  synth-channel yield    : %d / %d\n", synthFitted, nDirs);
    std::printf("  both channels fitted   : %d\n", bothFitted);
    if (bothFitted >= 3) {
        std::printf("  median |delta_measured - delta_synth| : %.2f deg\n", median(absGap));
        std::printf("  Spearman rho(measured, synth)         : %+.3f\n", spearman(mDelta, sDelta));
    }
    // What the producer would actually EMIT: the measured channel wherever it
    // fits, the synth channel wherever it alone does. The gap is the number of
    // swings that stay silent on both channels.
    const int synthOnly = synthFitted - bothFitted;
    std::printf("  emitted coverage       : %d / %d  (%d measured + %d synth-only, %d silent)\n",
                nFitted + synthOnly, nDirs, nFitted, synthOnly, nDirs - nFitted - synthOnly);
    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail;
}
