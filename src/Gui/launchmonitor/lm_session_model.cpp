/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "lm_session_model.h"

#include "../../Analysis/lm_flight_path.h"
#include "../../Analysis/lm_inferred_reads.h"
#include "../../Analysis/lm_session_reductions.h"
#include "../../Analysis/swing_analysis.h"

#include <QPointF>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <limits>
#include <optional>

using namespace pinpoint::analysis;

namespace {

// One shot's launch monitor readings, out of the row's analysisDetail.
//
// The `series` entries are the swing.json metric objects verbatim; a launch monitor
// entry is an EMPTY curve carrying a single phaseSample at Impact (see lmMetricEntries
// — a device reports one number per shot, and inventing a curve for it would be a lie
// the charts would draw). So the value is looked up by PHASE, exactly as the writer
// tagged it, rather than by taking whatever sample happens to be first.
LmShotValues readingsFor(const QVariantMap &analysisDetail)
{
    LmShotValues out;
    const QVariantList series = analysisDetail.value(QStringLiteral("series")).toList();
    for (const QVariant &sv : series) {
        const QVariantMap m = sv.toMap();
        const QString key = m.value(QStringLiteral("key")).toString();
        if (!key.startsWith(QStringLiteral("lm.")))
            continue;
        for (const QVariant &pv : m.value(QStringLiteral("phaseSamples")).toList()) {
            const QVariantMap ps = pv.toMap();
            if (ps.value(QStringLiteral("phase")).toInt() != int(Phase::Impact))
                continue;
            bool ok = false;
            const double v = ps.value(QStringLiteral("value")).toDouble(&ok);
            if (ok) out.insert(key, v);
            break;
        }
    }
    return out;
}

QVariantMap tileFor(const LmFieldStats &st)
{
    return QVariantMap{
        { QStringLiteral("key"),       st.key },
        { QStringLiteral("label"),     QString::fromUtf8(st.def->label) },
        { QStringLiteral("abbrev"),    QString::fromUtf8(st.def->abbrev) },
        { QStringLiteral("unit"),      QString::fromUtf8(st.def->unit) },
        { QStringLiteral("group"),     QString::fromUtf8(st.def->group) },
        // Formatted here and not in QML, so this panel and any future export cannot
        // disagree about what a reading looks like.
        { QStringLiteral("latest"),    lmValueText(st) },
        { QStringLiteral("mean"),      lmMeanText(st) },
        { QStringLiteral("sd"),        lmSdText(st) },
        { QStringLiteral("n"),         st.n },
        { QStringLiteral("hasLatest"), st.hasLatest },
        { QStringLiteral("hasSpread"), st.hasSpread },
        { QStringLiteral("z"),         st.z },
        // The strip's tick position, decided here for the same reason as the numbers.
        { QStringLiteral("tickPct"),   lmTickPercent(st.z) },
    };
}

// ── graphics mode ────────────────────────────────────────────────────────────
//
// The schematics need two things the tiles board never did: the RAW value (a formatted
// string cannot be rotated by), and the band's INDEX (the annotation is coloured by the
// band its metric belongs to, not by the card it sits on — the launch ray inside the
// IMPACT card is Launch teal). Both come off the same LmFieldStats the tiles came from.

int bandIndexOf(const char *group)
{
    const QString g = QString::fromLatin1(group ? group : "");
    const auto &groups = pinpoint::lm::fieldGroups();
    for (size_t i = 0; i < groups.size(); ++i)
        if (QString::fromLatin1(groups[i]) == g) return int(i);
    return 0;
}

// One field, as an annotation needs it.
QVariantMap annotationFor(const LmFieldStats &st)
{
    return QVariantMap{
        { QStringLiteral("value"),     st.latest },       // RAW — geometry rotates by this
        { QStringLiteral("text"),      lmValueText(st) }, // formatted — the card prints this
        { QStringLiteral("mean"),      lmMeanText(st) },
        { QStringLiteral("sd"),        lmSdText(st) },
        { QStringLiteral("unit"),      QString::fromUtf8(st.def->unit) },
        { QStringLiteral("abbrev"),    QString::fromUtf8(st.def->abbrev) },
        { QStringLiteral("label"),     QString::fromUtf8(st.def->label) },
        { QStringLiteral("bandIndex"), bandIndexOf(st.def->group) },
        { QStringLiteral("has"),       st.hasLatest },
    };
}

// A field's focused-shot value, or nothing. THE `std::optional` IS THE POINT: it is what
// carries "the monitor did not report this" all the way into the inferred reads, which
// answer with no read rather than with a default one.
std::optional<double> valueOf(const std::vector<LmFieldStats> &stats, const char *key)
{
    const QString k = QString::fromLatin1(key);
    for (const LmFieldStats &st : stats)
        if (st.key == k && st.hasLatest) return st.latest;
    return std::nullopt;
}

std::optional<double> meanOf(const std::vector<LmFieldStats> &stats, const char *key)
{
    const QString k = QString::fromLatin1(key);
    for (const LmFieldStats &st : stats)
        if (st.key == k && st.n > 0) return st.mean;
    return std::nullopt;
}

// The flight polyline, thinned to what a 428 px card can resolve.
//
// The integration produces thousands of points at half a millisecond each; drawing every
// one would hand QML a list two orders of magnitude longer than the card has pixels for.
// Thinned HERE rather than in the view because it is a decision about the data, and
// because a Shape rebuilt from 5,000 elements on every focus change is a stutter the
// panel does not need to have.
QVariantList thin(const std::vector<LmPoint> &pts, bool lateral, int want = 120)
{
    QVariantList out;
    if (pts.empty()) return out;
    const int n = int(pts.size());
    const int step = std::max(1, n / std::max(2, want));
    out.reserve(n / step + 2);
    for (int i = 0; i < n; i += step)
        out.append(QPointF(pts[size_t(i)].x, lateral ? pts[size_t(i)].z : pts[size_t(i)].y));
    // The last point always survives the thinning: it is the landing, and it is the one
    // point on the curve the card labels.
    const LmPoint &last = pts.back();
    out.append(QPointF(last.x, lateral ? last.z : last.y));
    return out;
}

QVariantMap pointMap(const LmPoint &p)
{
    return QVariantMap{ { QStringLiteral("x"), p.x },
                        { QStringLiteral("y"), p.y },
                        { QStringLiteral("z"), p.z } };
}

} // namespace

LmSessionModel::LmSessionModel(QObject *parent) : QAbstractListModel(parent) {}

int LmSessionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_bands.size());
}

QVariant LmSessionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_bands.size())
        return {};
    const Band &b = m_bands.at(index.row());
    switch (role) {
    case BandRole:  return b.name;
    case CountRole: return int(b.tiles.size());
    case TilesRole: return b.tiles;
    }
    return {};
}

QHash<int, QByteArray> LmSessionModel::roleNames() const
{
    return { { BandRole, "band" }, { CountRole, "count" }, { TilesRole, "tiles" } };
}

void LmSessionModel::setShotModel(ShotListModel *m)
{
    if (m_shots == m)
        return;
    if (m_shots)
        m_shots->disconnect(this);
    m_shots = m;
    connectShotModel();
    emit shotModelChanged();
    rebuild();
}

void LmSessionModel::connectShotModel()
{
    if (!m_shots)
        return;
    // Everything that can change what the board says, and nothing that cannot. A club
    // edit writes through as dataChanged on the row, a reading arriving does the same
    // via refreshShot, and loadSessionDir is a model reset.
    connect(m_shots, &QAbstractItemModel::rowsInserted,   this, &LmSessionModel::rebuild);
    connect(m_shots, &QAbstractItemModel::rowsRemoved,    this, &LmSessionModel::rebuild);
    connect(m_shots, &QAbstractItemModel::modelReset,     this, &LmSessionModel::rebuild);
    connect(m_shots, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &, const QModelIndex &, const QVector<int> &roles) {
                if (roles.isEmpty()
                    || roles.contains(int(ShotListModel::AnalysisDetailRole))
                    || roles.contains(int(ShotListModel::MetricsRole))
                    || roles.contains(int(ShotListModel::ClubRole)))
                    rebuild();
            });
    connect(m_shots, &QObject::destroyed, this, [this]() { rebuild(); });
}

void LmSessionModel::setFocusedShotId(int id)
{
    if (m_focusedShotId == id)
        return;
    m_focusedShotId = id;
    emit focusedShotIdChanged();
    rebuild();
}

void LmSessionModel::setConnected(bool on)
{
    if (m_connected == on) return;
    m_connected = on;
    emit stateTextChanged();
}

void LmSessionModel::setSaving(bool on)
{
    if (m_saving == on) return;
    m_saving = on;
    emit stateTextChanged();
}

void LmSessionModel::setDeviceName(const QString &name)
{
    if (m_deviceName == name) return;
    m_deviceName = name;
    rebuild();               // the scope line leads with it
    emit headerChanged();
}

void LmSessionModel::setLeftHanded(bool on)
{
    if (m_leftHanded == on) return;
    m_leftHanded = on;
    rebuild();               // both inferred reads are worded from it
}

QVariantList LmSessionModel::bandCounts() const
{
    QVariantList out;
    out.reserve(m_bands.size());
    for (const Band &b : m_bands)
        out.append(int(b.tiles.size()));
    return out;
}

QString LmSessionModel::emptyText() const
{
    // Ordered by what the user can do about it. "No monitor" outranks "not saving"
    // outranks "nothing yet", because the fix for each is a different screen and only
    // the innermost one is a matter of hitting another ball.
    if (!m_connected)
        return tr("No launch monitor connected — Settings → Launch Monitor");
    if (!m_saving)
        return tr("Launch monitor readings are not being saved — Settings → Storage");
    if (!m_anyReadings)
        return tr("No readings yet this session");
    // Readings exist, but not for the club the focused shot was hit with. Saying "no
    // readings this session" there would be false, and drawing an empty board would
    // look like a fault — the scope is the answer, so the scope is what it names.
    if (m_bands.isEmpty())
        return tr("No readings yet for this club");
    return {};
}

void LmSessionModel::rebuild()
{
    beginResetModel();
    m_bands.clear();
    m_graphics.clear();
    m_scopeText.clear();
    m_valueLabel = tr("latest");
    m_shotCount = 0;
    m_anyReadings = false;

    if (!m_shots) {
        endResetModel();
        emit headerChanged();
        emit stateTextChanged();
        emit graphicsChanged();
        return;
    }

    const int rows = m_shots->rowCount();

    // The focused row, and the club that scopes everything. Rows are newest-first, so
    // the first row of the scope is also the newest shot in it.
    int focusedRow = -1;
    for (int r = 0; r < rows && m_focusedShotId >= 0; ++r) {
        if (m_shots->data(m_shots->index(r), ShotListModel::ShotIdRole).toInt() == m_focusedShotId) {
            focusedRow = r;
            break;
        }
    }

    // Aggregating across a mixed bag is meaningless — a driver and a 9-iron in one
    // carry mean is a bug, not a feature. An unknown club on the focused shot falls
    // back to the whole session and SAYS SO in the header; it never silently mixes.
    const QString club = focusedRow >= 0
        ? m_shots->data(m_shots->index(focusedRow), ShotListModel::ClubRole).toString().trimmed()
        : QString();

    std::vector<LmShotValues> scoped;
    scoped.reserve(size_t(rows));
    int latestIndex = -1;
    for (int r = 0; r < rows; ++r) {
        LmShotValues v = readingsFor(
            m_shots->data(m_shots->index(r), ShotListModel::AnalysisDetailRole).toMap());
        // Session-wide, deliberately: it separates "the monitor has given us nothing"
        // from "this club has nothing yet", which are different sentences to read.
        if (!v.isEmpty())
            m_anyReadings = true;

        if (!club.isEmpty()) {
            const QString rc = m_shots->data(m_shots->index(r), ShotListModel::ClubRole)
                                   .toString().trimmed();
            if (rc.compare(club, Qt::CaseInsensitive) != 0)
                continue;
        }
        if (r == focusedRow)
            latestIndex = int(scoped.size());
        scoped.push_back(std::move(v));
    }
    m_shotCount = int(scoped.size());

    // Nothing focused: the newest shot in scope is the one to headline, which is row 0
    // because the list is newest-first. Then the header's word really is "latest".
    const bool focusIsNewest = latestIndex <= 0;
    if (latestIndex < 0 && !scoped.empty())
        latestIndex = 0;
    m_valueLabel = focusIsNewest ? tr("latest") : tr("this shot");

    QStringList bits;
    if (!m_deviceName.isEmpty()) bits << m_deviceName;
    bits << (club.isEmpty() ? tr("all clubs") : club);
    // Spelled out rather than tr()'s %n plural form: with no translation catalogue
    // loaded, %n falls back to the SOURCE string, and the header read "1 shot(s)".
    bits << (m_shotCount == 1 ? tr("1 shot") : tr("%1 shots").arg(m_shotCount));
    m_scopeText = bits.join(QStringLiteral(" · "));

    // The statistics, then the same vector split into the board's bands. Walking
    // fieldGroups() outside and the stats inside keeps both orders where they are
    // declared — bands in display order, fields in fieldDefs() order within a band.
    const std::vector<LmFieldStats> stats = lmSessionStats(scoped, latestIndex);
    for (const char *g : pinpoint::lm::fieldGroups()) {
        const QString name = QString::fromLatin1(g);
        Band band;
        band.name = name;
        for (const LmFieldStats &st : stats)
            if (st.def && QString::fromLatin1(st.def->group) == name)
                band.tiles.append(tileFor(st));
        // A band no shot produced a reading for is not drawn at all — an empty gutter
        // rule with a zero beside it says the device measured nothing, which is a
        // different claim from "the device does not report spin".
        if (!band.tiles.isEmpty())
            m_bands.push_back(band);
    }

    buildGraphics(stats);

    endResetModel();
    emit headerChanged();
    emit stateTextChanged();
    emit graphicsChanged();
}

// The second projection of the same statistics: what the five schematics draw.
//
// ONE MAP RATHER THAN A SECOND MODEL. Both modes answer questions about one shot out of
// one scope, and the numbers behind them are the vector this is handed. A second model
// would recompute them, and two computations of one mean is two chances to print
// different numbers on two tabs of the same panel.
//
// EVERY DERIVED NUMBER IS DECIDED HERE, not in a binding: the flight polyline, both
// inferred reads, their evidence strings, the drawn-metric count. QML positions and
// paints (analysis pipeline guide §6.2).
void LmSessionModel::buildGraphics(const std::vector<LmFieldStats> &stats)
{
    QVariantMap g;
    g.insert(QStringLiteral("leftHanded"), m_leftHanded);

    // Every reported field, keyed, with its raw value and its band. Keyed rather than
    // ordered because the schematics reach for a specific metric at a specific anchor —
    // the ordered walk is the tiles board's need, not this one's.
    QVariantMap values;
    int reported = 0;
    for (const LmFieldStats &st : stats) {
        if (!st.def) continue;
        values.insert(st.key, annotationFor(st));
        if (st.hasLatest) ++reported;
    }
    g.insert(QStringLiteral("values"), values);
    g.insert(QStringLiteral("has"), reported > 0);

    // The headline strip: the six figures a golfer reads first, in the design's order.
    // Named explicitly because this IS an editorial choice about what leads — unlike the
    // board below it, which never hand-lists a key.
    static const char *kHeadline[] = {
        "lm.ballSpeed", "lm.clubheadSpeed", "lm.smashFactor",
        "lm.carryDistance", "lm.totalDistance", "lm.spinRate",
    };
    QVariantList headline;
    for (const char *k : kHeadline) {
        const QString key = QString::fromLatin1(k);
        if (values.contains(key)) headline.append(values.value(key));
    }
    g.insert(QStringLiteral("headline"), headline);

    // ── the flight ──────────────────────────────────────────────────────────
    const std::optional<double> ballSpeed = valueOf(stats, "lm.ballSpeed");
    const std::optional<double> launchAng = valueOf(stats, "lm.launchAngle");
    const std::optional<double> startDir  = valueOf(stats, "lm.launchDirection");
    const std::optional<double> spinRate  = valueOf(stats, "lm.spinRate");
    const std::optional<double> spinAxis  = valueOf(stats, "lm.spinAxis");
    const std::optional<double> carry     = valueOf(stats, "lm.carryDistance");
    const std::optional<double> total     = valueOf(stats, "lm.totalDistance");
    const std::optional<double> offline   = valueOf(stats, "lm.offline");
    const std::optional<double> peak      = valueOf(stats, "lm.peakHeight");

    QVariantMap flight{ { QStringLiteral("has"), false } };
    if (ballSpeed && launchAng && spinRate) {
        const LmLaunch launch{ *ballSpeed, *launchAng, startDir.value_or(0.0),
                               *spinRate,  spinAxis.value_or(0.0) };
        const LmFlightIntegration raw = lmIntegrateFlight(launch);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const LmFlightPath p = lmNormalisedPath(raw,
                                                carry.value_or(nan),
                                                peak.value_or(nan),
                                                offline.value_or(nan),
                                                total.value_or(nan));
        if (p.has) {
            flight = QVariantMap{
                { QStringLiteral("has"),            true },
                { QStringLiteral("profile"),        thin(p.points, false) },
                { QStringLiteral("track"),          thin(p.points, true) },
                { QStringLiteral("landing"),        pointMap(p.landing) },
                { QStringLiteral("finish"),         pointMap(p.finish) },
                { QStringLiteral("launchTangent"),  pointMap(p.launchTangent) },
                { QStringLiteral("landingTangent"), pointMap(p.landingTangent) },
                { QStringLiteral("carryFraction"),  p.carryFraction },
                { QStringLiteral("apexAtX"),        p.apexAtX },
                { QStringLiteral("lateralExtentYd"), p.lateralExtentYd },
                { QStringLiteral("carryYd"),        p.carryYd },
                { QStringLiteral("totalYd"),        p.totalYd },
                { QStringLiteral("apexFt"),         p.apexFt },
                { QStringLiteral("offlineYd"),      p.offlineYd },
                // Carried out of the model but not drawn yet: the design brief's open
                // question 1 wants shots in known conditions before the card says
                // whether a large residual is wind or a soft spin-axis reading.
                { QStringLiteral("residualOfflineYd"), p.residualOfflineYd },
            };
        }
    }
    g.insert(QStringLiteral("flight"), flight);

    // ── the two inferred reads ──────────────────────────────────────────────
    const LmFlightShape shape = lmFlightShape(startDir,
                                              valueOf(stats, "lm.faceToPath"),
                                              spinAxis, offline, m_leftHanded);
    g.insert(QStringLiteral("shape"), QVariantMap{
        { QStringLiteral("has"),       shape.has },
        { QStringLiteral("name"),      shape.name },
        { QStringLiteral("evidence"),  shape.evidence },
        { QStringLiteral("windowIdx"), shape.windowIdx },
        { QStringLiteral("curveIdx"),  shape.curveIdx },
    });

    const LmStrikeRead strike = lmStrikeQuality(valueOf(stats, "lm.strikeLocation"),
                                                valueOf(stats, "lm.strikeHeight"),
                                                valueOf(stats, "lm.smashFactor"),
                                                meanOf(stats, "lm.smashFactor"));
    g.insert(QStringLiteral("strike"), QVariantMap{
        { QStringLiteral("has"),      strike.has },
        { QStringLiteral("name"),     strike.name },
        { QStringLiteral("evidence"), strike.evidence },
    });

    m_graphics = g;
}
