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

#include "../../Analysis/lm_session_reductions.h"
#include "../../Analysis/swing_analysis.h"

#include <QStringList>
#include <QVariantMap>

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
    m_scopeText.clear();
    m_valueLabel = tr("latest");
    m_shotCount = 0;
    m_anyReadings = false;

    if (!m_shots) {
        endResetModel();
        emit headerChanged();
        emit stateTextChanged();
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

    endResetModel();
    emit headerChanged();
    emit stateTextChanged();
}
