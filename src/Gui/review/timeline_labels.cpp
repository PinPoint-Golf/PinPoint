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

#include "timeline_labels.h"

#include <QFont>
#include <QFontMetricsF>
#include <QVariantMap>
#include <QVector>
#include <algorithm>

namespace {

// Phase-name tables. Indices match Analysis Phase enum (swing_analysis.h); the array
// is the single source of truth that PpTransitTimeline (full names) and PpChartPlot
// (short tags) both read, replacing the lists that used to be duplicated in QML.
const char *const kFullNames[] = {
    "Address", "Takeaway", "Top", "Transition", "Downswing", "Impact",
    "Release", "Finish", "Mid-backswing", "Delivery", "Max speed", "Follow-through",
    "Shaft-parallel back", "Arm-parallel down", "Shaft-parallel through"
};
const char *const kShortTags[] = {
    "ADR", "TKW", "TOP", "TRN", "DWN", "IMP", "REL", "FIN", "MBK", "DLV", "SPD", "FLW",
    "SPB", "APD", "SPT"
};
constexpr int kPhaseCount = int(sizeof(kFullNames) / sizeof(kFullNames[0]));

constexpr int kImpactPhase = 5;   // Phase::Impact — the emphasised station

} // namespace

QVariantList TimelineLabels::distribute(const QVariantList &centers, const QVariantList &sizes,
                                        double gap, double lo, double hi) const
{
    const int n = centers.size();
    QVector<double> out(n);
    QVector<double> sz(n);
    for (int i = 0; i < n; ++i) {
        out[i] = centers.at(i).toDouble();
        sz[i]  = (i < sizes.size()) ? sizes.at(i).toDouble() : 0.0;
    }

    // Pass 1 — left→right: push each item right so it clears the previous one (or the
    // lower bound for the first), keeping a half-size + gap clearance.
    for (int i = 0; i < n; ++i) {
        const double h  = sz[i] / 2.0;
        const double mn = (i == 0) ? lo + h
                                   : out[i - 1] + sz[i - 1] / 2.0 + gap + h;
        if (out[i] < mn) out[i] = mn;
    }
    // Pass 2 — right→left: clamp each item left so it clears the next one (or the upper
    // bound for the last). Resolves the overflow the first pass can create at the end.
    for (int i = n - 1; i >= 0; --i) {
        const double h  = sz[i] / 2.0;
        const double mx = (i == n - 1) ? hi - h
                                       : out[i + 1] - sz[i + 1] / 2.0 - gap - h;
        if (out[i] > mx) out[i] = mx;
    }

    QVariantList res;
    res.reserve(n);
    for (int i = 0; i < n; ++i) res.append(out[i]);
    return res;
}

QVariantList TimelineLabels::stationLayout(const QVariantList &phases, qint64 startUs,
                                           qint64 endUs, bool horizontal, double mainLength,
                                           double gap, const QString &fontFamily,
                                           double fontPx) const
{
    const int n = phases.size();
    QVariantList out;
    if (n == 0) return out;

    const double span = qMax<double>(1.0, double(endUs - startUs));

    QFont font(fontFamily);
    if (fontPx > 0.0) font.setPixelSize(qRound(fontPx));
    const QFontMetricsF fm(font);
    const double lineH = fm.height();

    QVariantList centers, sizes;
    centers.reserve(n);
    sizes.reserve(n);
    QVector<double>  fracs(n);
    QVector<QString> names(n);
    QVector<int>     phaseIds(n);
    QVector<qint64>  tus(n);

    for (int i = 0; i < n; ++i) {
        const QVariantMap m = phases.at(i).toMap();
        const int    ph = m.value(QStringLiteral("phase")).toInt();
        const qint64 t  = m.value(QStringLiteral("t_us")).toLongLong();

        const double frac   = qBound(0.0, double(t - startUs) / span, 1.0);
        const double center = frac * mainLength;
        const QString name  = phaseFullName(ph);
        // Main-axis size: horizontal uses measured text width; vertical uses a uniform
        // line height (the labels stack down the right of the line).
        const double size = horizontal ? fm.horizontalAdvance(name) + 4.0 : lineH;

        fracs[i] = frac; names[i] = name; phaseIds[i] = ph; tus[i] = t;
        centers.append(center);
        sizes.append(size);
    }

    const QVariantList labelPos = distribute(centers, sizes, gap, 0.0, mainLength);

    for (int i = 0; i < n; ++i) {
        const double center = centers.at(i).toDouble();
        const double label  = labelPos.at(i).toDouble();
        QVariantMap r;
        r.insert(QStringLiteral("phase"),    phaseIds[i]);
        r.insert(QStringLiteral("tUs"),      tus[i]);
        r.insert(QStringLiteral("frac"),     fracs[i]);
        r.insert(QStringLiteral("center"),   center);
        r.insert(QStringLiteral("label"),    label);
        r.insert(QStringLiteral("name"),     names[i]);
        r.insert(QStringLiteral("isImpact"), phaseIds[i] == kImpactPhase);
        r.insert(QStringLiteral("elbow"),    qAbs(label - center) > 1.5);
        out.append(r);
    }
    return out;
}

QVariantList TimelineLabels::positionLayout(const QVariantList &positions, qint64 startUs,
                                            qint64 endUs, bool horizontal, double mainLength,
                                            double gap, const QString &fontFamily,
                                            int fontPx) const
{
    QVariantList out;
    if (positions.isEmpty()) return out;

    // Keep only valid P1..P8 + P10 (Finish) entries, sorted by time — unlike phases
    // (already time-ordered by the analyzer), milestone fits can be revised/added out
    // of order, so we re-sort defensively.
    QVector<QVariantMap> rows;
    rows.reserve(positions.size());
    for (const QVariant &pv : positions) {
        const QVariantMap m = pv.toMap();
        const int p = m.value(QStringLiteral("p")).toInt();
        if (p >= 1 && p <= 10) rows.append(m);
    }
    std::sort(rows.begin(), rows.end(), [](const QVariantMap &a, const QVariantMap &b) {
        return a.value(QStringLiteral("t_us")).toLongLong()
             < b.value(QStringLiteral("t_us")).toLongLong();
    });

    const int n = rows.size();
    if (n == 0) return out;

    const double span = qMax<double>(1.0, double(endUs - startUs));

    QFont font(fontFamily);
    if (fontPx > 0) font.setPixelSize(fontPx);
    const QFontMetricsF fm(font);
    const double lineH = fm.height();

    QVariantList centers, sizes;
    centers.reserve(n);
    sizes.reserve(n);
    QVector<double>  fracs(n);
    QVector<QString> labels(n);
    QVector<int>     ps(n);
    QVector<qint64>  tus(n);
    QVector<int>     sources(n);
    QVector<double>  confs(n);

    for (int i = 0; i < n; ++i) {
        const QVariantMap &m = rows[i];
        const int    p = m.value(QStringLiteral("p")).toInt();
        const qint64 t = m.value(QStringLiteral("t_us")).toLongLong();

        const double  frac   = qBound(0.0, double(t - startUs) / span, 1.0);
        const double  center = frac * mainLength;
        const QString label  = QStringLiteral("P%1").arg(p);
        // Main-axis size, same rule as stationLayout: measured text width when
        // horizontal (labels run along the line), a uniform line height when vertical.
        const double size = horizontal ? fm.horizontalAdvance(label) + 4.0 : lineH;

        fracs[i] = frac; labels[i] = label; ps[i] = p; tus[i] = t;
        sources[i] = m.value(QStringLiteral("source")).toInt();
        confs[i]   = m.value(QStringLiteral("conf")).toDouble();
        centers.append(center);
        sizes.append(size);
    }

    const QVariantList solved = distribute(centers, sizes, gap, 0.0, mainLength);

    for (int i = 0; i < n; ++i) {
        const double rawCenter = centers.at(i).toDouble();
        const double center    = solved.at(i).toDouble();
        QVariantMap r;
        r.insert(QStringLiteral("p"),      ps[i]);
        r.insert(QStringLiteral("tUs"),    tus[i]);
        r.insert(QStringLiteral("frac"),   fracs[i]);
        r.insert(QStringLiteral("center"), center);
        r.insert(QStringLiteral("label"),  labels[i]);
        r.insert(QStringLiteral("source"), sources[i]);
        r.insert(QStringLiteral("conf"),   confs[i]);
        r.insert(QStringLiteral("elbow"),  qAbs(center - rawCenter) > 1.5);
        out.append(r);
    }
    return out;
}

int TimelineLabels::activeStation(const QVariantList &phases, qint64 pos) const
{
    int best = -1;
    for (int i = 0; i < phases.size(); ++i) {
        const qint64 t = phases.at(i).toMap().value(QStringLiteral("t_us")).toLongLong();
        if (t <= pos) best = i;   // greatest index that still precedes the playhead
    }
    return best;
}

double TimelineLabels::valueAtNearest(const QVariantList &tUs, const QVariantList &value,
                                      qint64 pos) const
{
    const int n = qMin(tUs.size(), value.size());
    if (n == 0) return 0.0;
    if (pos <= tUs.at(0).toLongLong())     return value.at(0).toDouble();
    if (pos >= tUs.at(n - 1).toLongLong()) return value.at(n - 1).toDouble();

    // tUs is ascending — binary search for the first sample at/after pos.
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (tUs.at(mid).toLongLong() < pos) lo = mid + 1;
        else                                hi = mid;
    }
    const qint64 tHi = tUs.at(lo).toLongLong();
    const qint64 tLo = tUs.at(lo - 1).toLongLong();
    const int idx = (pos - tLo) <= (tHi - pos) ? lo - 1 : lo;
    return value.at(idx).toDouble();
}

qint64 TimelineLabels::snap(const QVariantList &phases, qint64 us, qint64 tolUs) const
{
    qint64 bestT = us;
    qint64 bestD = tolUs;     // a candidate must fall within the tolerance
    bool   found = false;
    for (int i = 0; i < phases.size(); ++i) {
        const qint64 t = phases.at(i).toMap().value(QStringLiteral("t_us")).toLongLong();
        const qint64 d = qAbs(t - us);
        if (d <= bestD) { bestD = d; bestT = t; found = true; }   // nearest within tol wins
    }
    return found ? bestT : us;
}

QString TimelineLabels::phaseFullName(int phase) const
{
    if (phase >= 0 && phase < kPhaseCount) return QString::fromLatin1(kFullNames[phase]);
    return QStringLiteral("P%1").arg(phase);
}

QString TimelineLabels::phaseShortTag(int phase) const
{
    if (phase >= 0 && phase < kPhaseCount) return QString::fromLatin1(kShortTags[phase]);
    return QStringLiteral("P%1").arg(phase);
}
