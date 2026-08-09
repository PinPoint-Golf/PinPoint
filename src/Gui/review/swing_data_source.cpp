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

#include "swing_data_source.h"

#include "swing_coverage_model.h"
#include "swing_series_model.h"
#include "../Analysis/swing_analysis.h"   // SegmentRole + segmentRoleForSlot/Name

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace pinpoint {

namespace {
constexpr double kConfThreshold = 0.5;
constexpr double kRad2Deg       = 57.29577951308232;

// ── catalogue helpers ───────────────────────────────────────────────────────
QString phaseLabel(int p)
{
    switch (p) {
    case 0: return QStringLiteral("Address");
    case 1: return QStringLiteral("Takeaway");
    case 2: return QStringLiteral("Top");
    case 3: return QStringLiteral("Transition");
    case 4: return QStringLiteral("Downswing");
    case 5: return QStringLiteral("Impact");
    case 6: return QStringLiteral("Release");
    case 7: return QStringLiteral("Finish");
    case 8: return QStringLiteral("Mid-BS");
    case 9: return QStringLiteral("Delivery");
    case 10: return QStringLiteral("Max Speed");
    case 11: return QStringLiteral("Follow-Through");
    case 12: return QStringLiteral("Shaft-Parallel Back");
    case 13: return QStringLiteral("Arm-Parallel Down");
    case 14: return QStringLiteral("Shaft-Parallel Through");
    default: return QString::number(p);
    }
}

// Three-letter abbreviation for a phase label, used in segment chips.
QString phaseAbbrev(const QString &label)
{
    static const QHash<QString, QString> m = {
        { QStringLiteral("Address"),    QStringLiteral("ADR") },
        { QStringLiteral("Takeaway"),   QStringLiteral("TKA") },
        { QStringLiteral("Top"),        QStringLiteral("TOP") },
        { QStringLiteral("Transition"), QStringLiteral("TRN") },
        { QStringLiteral("Downswing"),  QStringLiteral("DSW") },
        { QStringLiteral("Impact"),     QStringLiteral("IMP") },
        { QStringLiteral("Release"),    QStringLiteral("REL") },
        { QStringLiteral("Finish"),     QStringLiteral("FIN") },
        { QStringLiteral("Mid-BS"),     QStringLiteral("MBS") },
        { QStringLiteral("Delivery"),               QStringLiteral("DLV") },
        { QStringLiteral("Max Speed"),               QStringLiteral("SPD") },
        { QStringLiteral("Follow-Through"),          QStringLiteral("FLW") },
        { QStringLiteral("Shaft-Parallel Back"),     QStringLiteral("SPB") },
        { QStringLiteral("Arm-Parallel Down"),       QStringLiteral("APD") },
        { QStringLiteral("Shaft-Parallel Through"),  QStringLiteral("SPT") },
    };
    return m.value(label, label.left(3).toUpper());
}

// Short, column-friendly header for a metric key.
QString metricHeader(const QString &key)
{
    static const QHash<QString, QString> m = {
        { QStringLiteral("leadWristFlexExt"), QStringLiteral("FlexExt") },
        { QStringLiteral("leadWristRadUln"),  QStringLiteral("RadUln")  },
        { QStringLiteral("forearmPronation"), QStringLiteral("Pron")    },
        { QStringLiteral("leadArmFlexion"),   QStringLiteral("ArmFlex") },
        { QStringLiteral("impactShaftLean"),  QStringLiteral("Lean")    },
        { QStringLiteral("xFactor"),          QStringLiteral("X-fac")   },
    };
    return m.value(key, key.left(8));
}

// Presentation for an IMU resolved to an anatomical role. Empty label => Unknown
// (fall back to the alias-based labelling).
struct RolePresentation { QString label; QString header; QString colorKey; };
RolePresentation rolePresentation(int role)
{
    using R = pinpoint::analysis::SegmentRole;
    switch (R(role)) {
    case R::Pelvis:       return { QStringLiteral("IMU · Pelvis"),         QStringLiteral("Pelvis"), QStringLiteral("teal")    };
    case R::Thorax:       return { QStringLiteral("IMU · Thorax"),         QStringLiteral("Thorax"), QStringLiteral("blue")    };
    case R::T12:          return { QStringLiteral("IMU · T12"),            QStringLiteral("T12"),    QStringLiteral("blue")    };
    case R::LeadUpperArm: return { QStringLiteral("IMU · Lead upper-arm"), QStringLiteral("L-Arm"),  QStringLiteral("purple2") };
    case R::LeadForearm:  return { QStringLiteral("IMU · Lead forearm"),   QStringLiteral("L-Fore"), QStringLiteral("purple")  };
    case R::LeadHand:     return { QStringLiteral("IMU · Lead hand"),      QStringLiteral("L-Hand"), QStringLiteral("coral")   };
    case R::TrailThigh:   return { QStringLiteral("IMU · Trail thigh"),    QStringLiteral("T-Thigh"),QStringLiteral("teal")    };
    case R::LeadThigh:    return { QStringLiteral("IMU · Lead thigh"),     QStringLiteral("L-Thigh"),QStringLiteral("teal")    };
    case R::Club:         return { QStringLiteral("IMU · Club"),           QStringLiteral("Club"),   QStringLiteral("coral")   };
    case R::Unknown:      break;
    }
    return {};
}

// Pose keypoint-set catalogue (COCO indices). part → {indices, label, header, color}.
struct PoseSet { QString part; QVector<int> idx; QString label; QString header; QString color; };
const QVector<PoseSet> &poseSets()
{
    static const QVector<PoseSet> v = {
        { QStringLiteral("shoulders"), {5, 6},   QStringLiteral("Pose · Shoulders"), QStringLiteral("Shldrs"), QStringLiteral("purple")  },
        { QStringLiteral("elbows"),    {7, 8},   QStringLiteral("Pose · Elbows"),    QStringLiteral("Elbows"), QStringLiteral("purple2") },
        { QStringLiteral("wrists"),    {9, 10},  QStringLiteral("Pose · Wrists"),    QStringLiteral("Wrists"), QStringLiteral("coral")   },
        { QStringLiteral("hips"),      {11, 12}, QStringLiteral("Pose · Hips"),      QStringLiteral("Hips"),   QStringLiteral("blue")    },
        { QStringLiteral("knees"),     {13, 14}, QStringLiteral("Pose · Knees"),     QStringLiteral("Knees"),  QStringLiteral("teal")    },
        { QStringLiteral("ankles"),    {15, 16}, QStringLiteral("Pose · Ankles"),    QStringLiteral("Ankles"), QStringLiteral("blue")    },
    };
    return v;
}

QString kindToStr(SwingSeries::Kind k)
{
    switch (k) {
    case SwingSeries::Imu:    return QStringLiteral("imu");
    case SwingSeries::Pose:   return QStringLiteral("pose");
    case SwingSeries::Club:   return QStringLiteral("club");
    case SwingSeries::Metric: return QStringLiteral("metric");
    case SwingSeries::Ball:   return QStringLiteral("ball");
    }
    return {};
}

qint64 pctl(QVector<qint64> v, double q)   // v copied; sorted in place
{
    if (v.isEmpty()) return 0;
    std::sort(v.begin(), v.end());
    const int i = int((v.size() - 1) * q);
    return v[std::clamp(i, 0, int(v.size()) - 1)];
}

void computeCadence(SwingSeries &s)
{
    const int n = s.t.size();
    if (n < 2) { s.nominalPeriodUs = 0; s.medianDeltaUs = 0; s.gapToleranceUs = 40000; return; }
    s.nominalPeriodUs = (s.t.last() - s.t.first()) / (n - 1);
    QVector<qint64> d; d.reserve(n - 1);
    for (int i = 1; i < n; ++i) d.push_back(s.t[i] - s.t[i - 1]);
    s.medianDeltaUs   = pctl(d, 0.5);
    const qint64 p90  = pctl(d, 0.90);
    s.gapToleranceUs  = std::max<qint64>(40000, qint64(1.8 * double(p90)));
}

// Body-frame z angular rate (deg/s) from successive quaternions (w,x,y,z),
// ω ≈ 2·vec(conj(q_prev)⊗q_cur)/dt. Hemisphere-corrected to avoid 2π flips.
void fillFusedRate(SwingSeries &s, const QVector<std::array<double, 4>> &q)
{
    const int n = q.size();
    s.fusedRateZ.resize(n);
    if (n) s.fusedRateZ[0] = 0.0;
    for (int i = 1; i < n; ++i) {
        const double dt = double(s.t[i] - s.t[i - 1]) / 1.0e6;
        if (dt <= 0.0) { s.fusedRateZ[i] = (i > 0 ? s.fusedRateZ[i - 1] : 0.0); continue; }
        double pw = q[i - 1][0], px = q[i - 1][1], py = q[i - 1][2], pz = q[i - 1][3];
        double w  = q[i][0],     x  = q[i][1],     y  = q[i][2],     z  = q[i][3];
        if ((pw * w + px * x + py * y + pz * z) < 0.0) { w = -w; x = -x; y = -y; z = -z; }
        // conj(prev)=(pw,-px,-py,-pz); (a⊗b).z = aw*bz + ax*by - ay*bx + az*bw
        const double rz = pw * z + (-px) * y - (-py) * x + (-pz) * w;
        s.fusedRateZ[i] = 2.0 * rz / dt * kRad2Deg;
    }
}
} // namespace

// ── lifecycle ────────────────────────────────────────────────────────────────
SwingDataSource::SwingDataSource(QObject *parent)
    : QObject(parent),
      m_coverage(new SwingCoverageModel(this)),
      m_table(new SwingSeriesModel(this))
{
}

QObject *SwingDataSource::coverage() const { return m_coverage; }
QObject *SwingDataSource::table() const { return m_table; }

QVariantList SwingDataSource::regionOptions() const
{
    return { QStringLiteral("Axial"), QStringLiteral("Lower"), QStringLiteral("Upper"),
             QStringLiteral("Delivery"), QStringLiteral("Custom") };
}

// ── property setters ─────────────────────────────────────────────────────────
void SwingDataSource::setSwingDir(const QString &dir)
{
    if (m_swingDir == dir) return;
    m_swingDir = dir;
    emit swingDirChanged();
    scheduleReload();
}

void SwingDataSource::setSessionType(int v)
{
    if (m_sessionType == v) return;
    m_sessionType = v;
    if (!m_swingDir.isEmpty()) scheduleReload();   // role resolution depends on session type
    else emit viewChanged();
}

void SwingDataSource::setImuPlacement(const QVariantMap &m)
{
    if (m_imuPlacement == m) return;
    m_imuPlacement = m;
    if (!m_swingDir.isEmpty()) scheduleReload();   // settings fallback for missing device.role
    else emit viewChanged();
}

// Collapse the burst of setSwingDir/setSessionType/setImuPlacement that QML fires
// in one binding pass on a swing switch into a single deferred reload(). Safe
// against destruction: the queued call is cancelled if `this` dies first.
void SwingDataSource::scheduleReload()
{
    if (m_reloadPending)
        return;
    m_reloadPending = true;
    QMetaObject::invokeMethod(this, [this] {
        m_reloadPending = false;
        reload();
    }, Qt::QueuedConnection);
}

void SwingDataSource::setRegion(const QString &r)
{
    if (m_region == r) return;
    m_region = r;
    rebuild();
}

void SwingDataSource::setFillMode(const QString &m)
{
    const QString v = (m == QLatin1String("nearest")) ? QStringLiteral("nearest") : QStringLiteral("off");
    if (m_fillMode == v) return;
    m_fillMode = v;
    rebuild();
}

void SwingDataSource::setWindowStartUs(qint64 v)
{
    if (m_windowStartUs == v) return;
    m_windowStartUs = v;
    rebuild();
}

void SwingDataSource::setWindowEndUs(qint64 v)
{
    if (m_windowEndUs == v) return;
    m_windowEndUs = v;
    rebuild();
}

void SwingDataSource::setWindow(qint64 startUs, qint64 endUs)
{
    if (m_windowStartUs == startUs && m_windowEndUs == endUs) return;
    m_windowStartUs = startUs;
    m_windowEndUs   = endUs;
    rebuild();
}

QString SwingDataSource::exportCsv() const
{
    if (!m_loaded || !m_table) return {};
    const QString csv = m_table->toCsv();
    if (csv.isEmpty()) return {};

    // Name from the swing folder (its stable id) + the active region, so exports of
    // different regions of the same swing don't collide. Sanitise to a safe filename.
    QString base = QFileInfo(m_swingDir).fileName();
    if (base.isEmpty()) base = QStringLiteral("swing");
    QString stem = base + QLatin1Char('-') + m_region;
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));

    const QDir home(QDir::homePath());
    QString path = home.filePath(stem + QStringLiteral(".csv"));
    for (int n = 2; QFileInfo::exists(path); ++n)   // never overwrite an existing file
        path = home.filePath(stem + QStringLiteral(" (%1).csv").arg(n));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    // Lead with a UTF-8 BOM so Excel reads the °, ·, Δ and em-dash glyphs correctly.
    ts << QChar(0xFEFF) << csv << '\n';
    f.close();
    return f.error() == QFileDevice::NoError ? path : QString();
}

// ── parse ────────────────────────────────────────────────────────────────────
void SwingDataSource::reload()
{
    m_all.clear();
    m_resolved.clear();
    m_phases.clear();
    m_metadata.clear();
    m_doc = QJsonObject();
    m_loaded = false;
    m_spanUs = 0;
    m_anyImuRoleKnown = false;

    if (!m_swingDir.isEmpty()) {
        QFile f(QDir(m_swingDir).filePath(QStringLiteral("swing.json")));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) m_doc = doc.object();
        }
    }

    const QJsonObject clock = m_doc.value(QStringLiteral("clock")).toObject();
    const qint64 t0 = qint64(clock.value(QStringLiteral("t0_us")).toDouble());
    // Analysis t_us are ABSOLUTE (t0-based) for live captures but WINDOW-RELATIVE
    // for re-analysed swings (the reconstructed window is 0-based). Normalise to
    // window-relative either way: subtract t0 only in the absolute domain (≥ t0);
    // already-relative values (≪ t0) pass through unchanged.
    const auto toRel = [t0](double raw) -> qint64 {
        const qint64 t = qint64(raw);
        return t >= t0 ? t - t0 : t;
    };
    const QJsonObject win = m_doc.value(QStringLiteral("window")).toObject();
    qint64 spanEnd = qint64(win.value(QStringLiteral("end_us")).toDouble());

    // ── streams (window-relative t_us) ───────────────────────────────────────
    int imuOrdinal = 0;
    const QString imuColors[] = { QStringLiteral("teal"), QStringLiteral("blue"),
                                  QStringLiteral("purple2"), QStringLiteral("coral") };
    const QJsonArray streams = m_doc.value(QStringLiteral("streams")).toArray();
    for (const QJsonValue &sv : streams) {
        const QJsonObject st = sv.toObject();
        const QString kind = st.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("imu")) {
            SwingSeries s;
            s.kind = SwingSeries::Imu;
            s.ref  = st.value(QStringLiteral("alias")).toString();
            if (s.ref.isEmpty()) s.ref = st.value(QStringLiteral("source")).toObject()
                                            .value(QStringLiteral("serial")).toString();
            s.label = QStringLiteral("IMU · ") + s.ref;
            QString shortName = s.ref; shortName.remove(QStringLiteral(" Sensor"));
            s.header = shortName;
            s.unit = QStringLiteral("deg/s");
            s.colorKey = imuColors[std::min(imuOrdinal, 3)];
            ++imuOrdinal;

            // Anatomical body role: prefer the stream's own device.role (new export),
            // else fall back to the global imu/placement map + session type. A known
            // role drives anatomical labelling + strict-by-role region resolution.
            const QString serial = st.value(QStringLiteral("source")).toObject()
                                      .value(QStringLiteral("serial")).toString();
            const QJsonObject dev = st.value(QStringLiteral("device")).toObject();
            int role = 0;
            if (dev.contains(QStringLiteral("role")))
                role = dev.value(QStringLiteral("role")).toInt();
            else
                role = int(pinpoint::analysis::segmentRoleForSlot(
                            m_sessionType, m_imuPlacement.value(serial).toString()));
            s.imuRole = role;
            const RolePresentation rp = rolePresentation(role);
            if (!rp.label.isEmpty()) {
                s.label    = rp.label;        // anatomical; alias stays in Properties
                s.header   = rp.header;
                s.colorKey = rp.colorKey;
                m_anyImuRoleKnown = true;
            }

            const QJsonObject samples = st.value(QStringLiteral("samples")).toObject();
            QVector<std::array<double, 4>> quats;
            const QJsonArray dataArr = samples.value(QStringLiteral("data")).toArray();
            const QJsonArray tArr    = samples.value(QStringLiteral("t_us")).toArray();
            const int count = std::min(tArr.size(), dataArr.size());
            s.t.reserve(count); s.value.reserve(count); s.gyroZ.reserve(count); quats.reserve(count);
            for (int i = 0; i < count; ++i) {
                const QJsonArray row = dataArr.at(i).toArray();
                if (row.size() < 10) continue;
                s.t.push_back(qint64(tArr.at(i).toDouble()));
                const double gz = row.at(5).toDouble();         // gyro z (deg/s)
                s.gyroZ.push_back(gz);
                s.value.push_back(gz);
                quats.push_back({ row.at(6).toDouble(), row.at(7).toDouble(),
                                  row.at(8).toDouble(), row.at(9).toDouble() });
            }
            if (!s.t.isEmpty()) {
                fillFusedRate(s, quats);
                computeCadence(s);
                if (s.t.last() > spanEnd) spanEnd = s.t.last();
                m_all.push_back(s);
            }
        }
        // video streams are parsed only for provenance (no default lane).
    }

    // ── analysis (absolute OR window-relative t_us → window-relative via toRel) ─
    const QJsonObject analysis = m_doc.value(QStringLiteral("analysis")).toObject();

    // metrics
    const QJsonArray metrics = analysis.value(QStringLiteral("metrics")).toArray();
    for (const QJsonValue &mv : metrics) {
        const QJsonObject mo = mv.toObject();
        SwingSeries s;
        s.kind  = SwingSeries::Metric;
        s.ref   = mo.value(QStringLiteral("key")).toString();
        s.unit  = mo.value(QStringLiteral("unit")).toString();
        s.label = QStringLiteral("Metric · ") + mo.value(QStringLiteral("label")).toString();
        s.header = metricHeader(s.ref);
        s.colorKey = QStringLiteral("amber");
        const QJsonArray tArr = mo.value(QStringLiteral("t_us")).toArray();
        const QJsonArray vArr = mo.value(QStringLiteral("value")).toArray();
        const int count = std::min(tArr.size(), vArr.size());
        s.t.reserve(count); s.value.reserve(count);
        for (int i = 0; i < count; ++i) {
            s.t.push_back(toRel(tArr.at(i).toDouble()));
            s.value.push_back(vArr.at(i).toDouble());
        }
        if (!s.t.isEmpty()) { computeCadence(s); m_all.push_back(s); }
    }

    // pose keypoint sets (from analysis.pose2d)
    const QJsonObject pose2d = analysis.value(QStringLiteral("pose2d")).toObject();
    const QJsonArray poseFrames = pose2d.value(QStringLiteral("frames")).toArray();
    if (!poseFrames.isEmpty()) {
        for (const PoseSet &ps : poseSets()) {
            SwingSeries s;
            s.kind = SwingSeries::Pose;
            s.ref  = ps.part;
            s.part = ps.part;
            s.label = ps.label;
            s.header = ps.header;
            s.colorKey = ps.color;
            s.t.reserve(poseFrames.size());
            s.value.reserve(poseFrames.size());
            s.conf.reserve(poseFrames.size());
            for (const QJsonValue &fv : poseFrames) {
                const QJsonObject fo = fv.toObject();
                const QJsonArray kp = fo.value(QStringLiteral("kp")).toArray();
                double sum = 0.0; int got = 0;
                for (int k : ps.idx) {
                    const int ci = k * 3 + 2;          // (·,·,score) triplet
                    if (ci < kp.size()) { sum += kp.at(ci).toDouble(); ++got; }
                }
                const double meanScore = got ? sum / got : 0.0;
                s.t.push_back(toRel(fo.value(QStringLiteral("t_us")).toDouble()));
                s.value.push_back(meanScore);
                s.conf.push_back(meanScore);
            }
            if (!s.t.isEmpty()) { computeCadence(s); m_all.push_back(s); }
        }
    }

    // club shaft (only when the track is valid)
    const QJsonObject club = analysis.value(QStringLiteral("club")).toObject();
    if (club.value(QStringLiteral("valid")).toBool()) {
        SwingSeries s;
        s.kind = SwingSeries::Club;
        s.ref  = QStringLiteral("shaft");
        s.part = QStringLiteral("shaft");
        s.label = QStringLiteral("Club · Shaft");
        s.header = QStringLiteral("Shaft°");
        s.unit = QStringLiteral("°");
        s.colorKey = QStringLiteral("coral");
        const QJsonArray cs = club.value(QStringLiteral("samples")).toArray();
        s.t.reserve(cs.size()); s.value.reserve(cs.size()); s.conf.reserve(cs.size());
        for (const QJsonValue &cv : cs) {
            const QJsonObject co = cv.toObject();
            s.t.push_back(toRel(co.value(QStringLiteral("t_us")).toDouble()));
            s.value.push_back(co.value(QStringLiteral("theta")).toDouble() * kRad2Deg);
            s.conf.push_back(co.value(QStringLiteral("conf")).toDouble());
        }
        if (!s.t.isEmpty()) { computeCadence(s); m_all.push_back(s); }

        // club head — visible grip→head length as % of frame height. conf reads
        // low where the head is projected/off-frame (assumed length, not measured),
        // mirroring the overlay's measured-vs-projected honesty.
        SwingSeries h;
        h.kind = SwingSeries::Club;
        h.ref  = QStringLiteral("head");
        h.part = QStringLiteral("head");
        h.label = QStringLiteral("Club · Head");
        h.header = QStringLiteral("ClubLen%");
        h.unit = QStringLiteral("%");
        h.colorKey = QStringLiteral("purple2");
        const double fh = club.value(QStringLiteral("frameHeight")).toDouble();
        const double fw = club.value(QStringLiteral("frameWidth")).toDouble();
        const double aspect = fh > 0.0 ? fw / fh : 1.0;   // grip/head x is normalized by width
        h.t.reserve(cs.size()); h.value.reserve(cs.size()); h.conf.reserve(cs.size());
        for (const QJsonValue &cv : cs) {
            const QJsonObject co = cv.toObject();
            const int flags = co.value(QStringLiteral("flags")).toInt();
            const bool projected =
                flags & (analysis::ShaftHeadProjected | analysis::ShaftHeadOffFrame);
            const double headConf = co.contains(QStringLiteral("headConf"))
                                    ? co.value(QStringLiteral("headConf")).toDouble() : -1.0;
            const double conf = projected ? 0.0
                              : (headConf >= 0.0 ? headConf
                                                 : co.value(QStringLiteral("conf")).toDouble());
            // Drawn grip→head length as % of frame height (aspect-correct, matches
            // the replay overlay). head is always placed (measured or projected),
            // so the lane is dense; conf marks measured (bright) vs projected (dim).
            const QJsonArray g  = co.value(QStringLiteral("grip")).toArray();
            const QJsonArray hd = co.value(QStringLiteral("head")).toArray();
            double lenPctH = 0.0;
            if (g.size() == 2 && hd.size() == 2) {
                const double dxH = (g.at(0).toDouble() - hd.at(0).toDouble()) * aspect;
                const double dyH =  g.at(1).toDouble() - hd.at(1).toDouble();
                lenPctH = std::hypot(dxH, dyH) * 100.0;
            }
            h.t.push_back(toRel(co.value(QStringLiteral("t_us")).toDouble()));
            h.value.push_back(lenPctH);
            h.conf.push_back(conf);
        }
        if (!h.t.isEmpty()) { computeCadence(h); m_all.push_back(h); }
    }

    // ball track (analysis.ball preferred, else the raw kind:"ball" ball_v2 stream). One
    // lane: value holds x, ballY holds y, ballR holds radius, conf holds detection
    // confidence — all normalized 0..1. Only found=true frames are kept, so the ball
    // vanishing at launch renders as trailing holes (matching the replay overlay).
    {
        SwingSeries s;
        s.kind  = SwingSeries::Ball;
        s.ref   = QStringLiteral("ball");
        s.part  = QStringLiteral("ball");
        s.label = QStringLiteral("Ball");
        s.header = QStringLiteral("Ball");
        s.colorKey = QStringLiteral("green");

        const QJsonObject ball = analysis.value(QStringLiteral("ball")).toObject();
        if (!ball.isEmpty()) {
            const QJsonArray bs = ball.value(QStringLiteral("samples")).toArray();
            s.t.reserve(bs.size()); s.value.reserve(bs.size());
            s.ballY.reserve(bs.size()); s.ballR.reserve(bs.size()); s.conf.reserve(bs.size());
            for (const QJsonValue &bv : bs) {
                const QJsonObject bo = bv.toObject();
                if (!bo.value(QStringLiteral("found")).toBool()) continue;   // gap → hole
                s.t.push_back(toRel(bo.value(QStringLiteral("t_us")).toDouble()));
                s.value.push_back(bo.value(QStringLiteral("x")).toDouble());
                s.ballY.push_back(bo.value(QStringLiteral("y")).toDouble());
                s.ballR.push_back(bo.value(QStringLiteral("r")).toDouble());
                s.conf.push_back(bo.value(QStringLiteral("conf")).toDouble());
            }
        } else {
            // Fallback: the raw ball_v2 stream (window-relative t_us; data layout
            // found,x,y,r,conf) — a skip-analysis capture that still recorded ball frames.
            for (const QJsonValue &sv : streams) {
                const QJsonObject st = sv.toObject();
                if (st.value(QStringLiteral("kind")).toString() != QLatin1String("ball")) continue;
                const QJsonObject fr = st.value(QStringLiteral("frames")).toObject();
                const QJsonArray tArr = fr.value(QStringLiteral("t_us")).toArray();
                const QJsonArray dArr = fr.value(QStringLiteral("data")).toArray();
                const int count = std::min(tArr.size(), dArr.size());
                s.t.reserve(count); s.value.reserve(count);
                s.ballY.reserve(count); s.ballR.reserve(count); s.conf.reserve(count);
                for (int i = 0; i < count; ++i) {
                    const QJsonArray row = dArr.at(i).toArray();
                    if (row.size() < 5) continue;
                    if (row.at(0).toDouble() == 0.0) continue;               // found=false → hole
                    s.t.push_back(qint64(tArr.at(i).toDouble()));
                    s.value.push_back(row.at(1).toDouble());
                    s.ballY.push_back(row.at(2).toDouble());
                    s.ballR.push_back(row.at(3).toDouble());
                    s.conf.push_back(row.at(4).toDouble());
                }
                break;   // first ball stream only
            }
        }
        if (!s.t.isEmpty()) { computeCadence(s); m_all.push_back(s); }
    }

    // phases (absolute OR window-relative → window-relative via toRel)
    const QJsonArray phases = analysis.value(QStringLiteral("phases")).toArray();
    for (const QJsonValue &pv : phases) {
        const QJsonObject po = pv.toObject();
        QVariantMap p;
        p[QStringLiteral("label")] = phaseLabel(po.value(QStringLiteral("phase")).toInt());
        p[QStringLiteral("t_us")]  = toRel(po.value(QStringLiteral("t_us")).toDouble());
        p[QStringLiteral("kind")]  = QStringLiteral("phase");
        m_phases.append(p);
    }

    if (spanEnd <= 0) spanEnd = 1;
    m_spanUs = spanEnd;
    m_windowStartUs = 0;
    m_windowEndUs   = spanEnd;
    m_loaded = !m_doc.isEmpty();

    // Phase-bounded segments for vertical table windowing ([0] = Full swing).
    m_segments.clear();
    {
        QVariantMap full;
        full[QStringLiteral("label")]   = QStringLiteral("Full");
        full[QStringLiteral("startUs")] = qint64(0);
        full[QStringLiteral("endUs")]   = m_spanUs;
        m_segments.append(full);
        QVector<QPair<qint64, QString>> ev;
        for (const QVariant &pv : m_phases) {
            const QVariantMap p = pv.toMap();
            ev.append({ p.value(QStringLiteral("t_us")).toLongLong(),
                        p.value(QStringLiteral("label")).toString() });
        }
        std::sort(ev.begin(), ev.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (int i = 0; i + 1 < ev.size(); ++i) {
            QVariantMap seg;
            seg[QStringLiteral("label")]   = phaseAbbrev(ev[i].second) + QStringLiteral("→")
                                           + phaseAbbrev(ev[i + 1].second);
            seg[QStringLiteral("startUs")] = ev[i].first;
            seg[QStringLiteral("endUs")]   = ev[i + 1].first;
            m_segments.append(seg);
        }
    }

    // Content-aware region defaulting: prefer the region surfacing the most IMU lanes
    // for THIS swing, then the most lanes overall, tie-broken by catalogue order.
    m_regionLaneCounts.clear();
    {
        const QStringList regions = { QStringLiteral("Axial"), QStringLiteral("Lower"),
                                      QStringLiteral("Upper"), QStringLiteral("Delivery") };
        int bestImu = -1, bestTotal = -1;
        m_bestRegion = QStringLiteral("Axial");
        for (const QString &rg : regions) {
            const QVector<SwingSeries> r = resolve(specsForRegion(rg));
            int imu = 0;
            for (const SwingSeries &s : r) if (s.kind == SwingSeries::Imu) ++imu;
            m_regionLaneCounts[rg] = int(r.size());
            if (imu > bestImu || (imu == bestImu && int(r.size()) > bestTotal)) {
                bestImu = imu; bestTotal = int(r.size()); m_bestRegion = rg;
            }
        }
    }

    // ── provenance metadata (grouped, ordered, defensive) ────────────────────
    auto row = [](const QString &k, const QVariant &v) {
        QVariantMap r; r[QStringLiteral("k")] = k; r[QStringLiteral("v")] = v.toString(); return QVariant(r);
    };
    auto pushGroup = [this](const QString &title, const QVariantList &rows) {
        if (rows.isEmpty()) return;
        QVariantMap g; g[QStringLiteral("group")] = title; g[QStringLiteral("rows")] = rows;
        m_metadata.append(g);
    };

    {   // Swing
        QVariantList r;
        const QJsonObject sw = m_doc.value(QStringLiteral("swing")).toObject();
        const QJsonObject at = m_doc.value(QStringLiteral("athlete")).toObject();
        const QJsonObject se = m_doc.value(QStringLiteral("session")).toObject();
        if (sw.contains(QStringLiteral("id")))   r << row(QStringLiteral("Swing"), sw.value(QStringLiteral("id")).toString());
        if (at.contains(QStringLiteral("name"))) r << row(QStringLiteral("Athlete"), at.value(QStringLiteral("name")).toString());
        if (at.contains(QStringLiteral("handedness"))) r << row(QStringLiteral("Handed"), at.value(QStringLiteral("handedness")).toString());
        if (se.contains(QStringLiteral("dir")))  r << row(QStringLiteral("Session"), se.value(QStringLiteral("dir")).toString());
        if (clock.contains(QStringLiteral("wallclock"))) r << row(QStringLiteral("Captured"), clock.value(QStringLiteral("wallclock")).toString());
        r << row(QStringLiteral("Duration"), QString::number(double(spanEnd) / 1.0e6, 'f', 2) + QStringLiteral(" s"));
        if (m_doc.contains(QStringLiteral("schema"))) r << row(QStringLiteral("Schema"), m_doc.value(QStringLiteral("schema")).toString());
        pushGroup(QStringLiteral("Swing"), r);
    }
    {   // Analysis
        QVariantList r;
        if (analysis.contains(QStringLiteral("score"))) r << row(QStringLiteral("Score"), QString::number(analysis.value(QStringLiteral("score")).toInt()));
        if (analysis.contains(QStringLiteral("tier")))  r << row(QStringLiteral("Tier"),  QString::number(analysis.value(QStringLiteral("tier")).toInt()));
        if (analysis.contains(QStringLiteral("schema"))) r << row(QStringLiteral("Schema"), analysis.value(QStringLiteral("schema")).toString());
        const QJsonObject seg = analysis.value(QStringLiteral("segmentation")).toObject();
        if (!seg.isEmpty()) {
            r << row(QStringLiteral("Seg conf"), QString::number(seg.value(QStringLiteral("conf")).toDouble(), 'f', 2));
            r << row(QStringLiteral("Seg ver"),  QString::number(seg.value(QStringLiteral("version")).toInt()));
        }
        // Fused club length (analysis.club.lengths) — the per-swing fusion result,
        // independent of shaft-track validity (ball+band+prior can fuse without a
        // valid θ track). Absent block / fusedPx <= 0 (pre-fusion swing or total
        // abstain) ⇒ no rows. Same %H unit as the Club·Head lane samples.
        const QJsonObject cl      = analysis.value(QStringLiteral("club")).toObject();
        const QJsonObject lengths = cl.value(QStringLiteral("lengths")).toObject();
        const double fusedPx = lengths.value(QStringLiteral("fusedPx")).toDouble(-1.0);
        if (fusedPx > 0.0) {
            const double fh = cl.value(QStringLiteral("frameHeight")).toDouble();
            r << row(QStringLiteral("Club len"),
                     fh > 0.0 ? QString::number(fusedPx / fh * 100.0, 'f', 1) + QStringLiteral(" %H")
                              : QString::number(fusedPx, 'f', 0) + QStringLiteral(" px"));
            r << row(QStringLiteral("Len conf"),
                     QString::number(lengths.value(QStringLiteral("fusedConf")).toDouble(), 'f', 2)
                     + QStringLiteral(" · est ")
                     + QString::number(lengths.value(QStringLiteral("nEstimators")).toInt())
                     + QStringLiteral(" · prior n=")
                     + QString::number(lengths.value(QStringLiteral("priorN")).toInt())
                     + QStringLiteral(" · rung ")
                     + QString::number(lengths.value(QStringLiteral("ladderRung")).toInt()));
        }
        // Coaching P-positions (analysis.club.positions, shaft_position_first §2
        // Layer B) — per-P coverage n/9 (P1–P8 + P10/Finish; P9 reserved) + the
        // weakest per-P confidence. Absent block ⇒ no row (extraction off / pre-v3.5).
        const QJsonArray positions = cl.value(QStringLiteral("positions")).toArray();
        if (!positions.isEmpty()) {
            double minConf = 1.0;
            for (const QJsonValue &pv : positions)
                minConf = std::min(minConf, pv.toObject().value(QStringLiteral("conf")).toDouble());
            r << row(QStringLiteral("Positions"),
                     QString::number(positions.size()) + QStringLiteral("/9 · min conf ")
                     + QString::number(minConf, 'f', 2));
        }
        pushGroup(QStringLiteral("Analysis"), r);
    }
    {   // Ball track (face-on) — provenance for the analysis.ball lane
        const QJsonObject ball = analysis.value(QStringLiteral("ball")).toObject();
        if (!ball.isEmpty()) {
            QVariantList r;
            if (ball.contains(QStringLiteral("camera")))
                r << row(QStringLiteral("Camera"), QString::number(ball.value(QStringLiteral("camera")).toInt()));
            const qint64 launch = qint64(ball.value(QStringLiteral("launchTUs")).toDouble());
            r << row(QStringLiteral("Launch"), launch >= 0
                        ? QString::number(double(toRel(double(launch))) / 1000.0, 'f', 1) + QStringLiteral(" ms")
                        : QStringLiteral("—"));
            r << row(QStringLiteral("Samples"),
                     QString::number(ball.value(QStringLiteral("samples")).toArray().size()));
            pushGroup(QStringLiteral("Ball"), r);
        }
    }
    {   // Capture / host (often absent — render whatever exists)
        QVariantList r;
        const QJsonObject cap = m_doc.value(QStringLiteral("capture")).toObject();
        const QJsonObject host = cap.value(QStringLiteral("host")).toObject();
        if (host.contains(QStringLiteral("app")))     r << row(QStringLiteral("App"),     host.value(QStringLiteral("app")).toString());
        if (host.contains(QStringLiteral("version"))) r << row(QStringLiteral("Version"), host.value(QStringLiteral("version")).toString());
        if (host.contains(QStringLiteral("gitSha")))  r << row(QStringLiteral("Git SHA"), host.value(QStringLiteral("gitSha")).toString());
        if (host.contains(QStringLiteral("hostname")))r << row(QStringLiteral("Host"),    host.value(QStringLiteral("hostname")).toString());
        if (host.contains(QStringLiteral("platform")))r << row(QStringLiteral("Platform"),host.value(QStringLiteral("platform")).toString());
        if (host.contains(QStringLiteral("poseBackend"))) r << row(QStringLiteral("Pose"), host.value(QStringLiteral("poseBackend")).toString());
        if (cap.contains(QStringLiteral("swingDetectionSensitivity"))) r << row(QStringLiteral("Detect"), cap.value(QStringLiteral("swingDetectionSensitivity")).toString());
        pushGroup(QStringLiteral("Capture"), r);
    }
    for (const QJsonValue &sv : streams) {   // per-stream
        const QJsonObject st = sv.toObject();
        const QString kind = st.value(QStringLiteral("kind")).toString();
        const QString alias = st.value(QStringLiteral("alias")).toString();
        QVariantList r;
        r << row(QStringLiteral("Kind"), kind);
        const QJsonObject src = st.value(QStringLiteral("source")).toObject();
        if (src.contains(QStringLiteral("serial")) && !src.value(QStringLiteral("serial")).toString().isEmpty())
            r << row(QStringLiteral("Serial"), src.value(QStringLiteral("serial")).toString());
        if (kind == QLatin1String("video")) {
            if (src.contains(QStringLiteral("pixelFormat")))
                r << row(QStringLiteral("Format"), QStringLiteral("%1 %2×%3")
                            .arg(src.value(QStringLiteral("pixelFormat")).toString())
                            .arg(src.value(QStringLiteral("width")).toInt())
                            .arg(src.value(QStringLiteral("height")).toInt()));
            const QJsonObject capt = st.value(QStringLiteral("capture")).toObject();
            if (capt.contains(QStringLiteral("fps_num")) && capt.value(QStringLiteral("fps_den")).toInt() != 0)
                r << row(QStringLiteral("Capture fps"), QString::number(
                            double(capt.value(QStringLiteral("fps_num")).toInt()) /
                            double(capt.value(QStringLiteral("fps_den")).toInt()), 'f', 0));
            // Exposure (additive) — feeds the shaft detector's blur assessment.
            // "derived" is the fps fallback (webcams); "auto" flags auto-exposure.
            if (capt.contains(QStringLiteral("exposureUs"))) {
                const double us  = capt.value(QStringLiteral("exposureUs")).toDouble();
                const QString sc = capt.value(QStringLiteral("exposureSource")).toString();
                QString v = us >= 1000.0
                                ? QString::number(us / 1000.0, 'f', 2) + QStringLiteral(" ms")
                                : QString::number(us, 'f', us < 10.0 ? 1 : 0) + QStringLiteral(" µs");
                if (sc == QLatin1String("derived"))
                    v = QStringLiteral("≈") + v + QStringLiteral(" (derived)");
                else if (capt.value(QStringLiteral("exposureAuto")).toBool())
                    v += QStringLiteral(" (auto)");
                r << row(QStringLiteral("Exposure"), v);
            }
            const QJsonObject setup = st.value(QStringLiteral("setup")).toObject();
            if (setup.contains(QStringLiteral("perspectiveName")))
                r << row(QStringLiteral("Perspective"), setup.value(QStringLiteral("perspectiveName")).toString());
            if (setup.contains(QStringLiteral("fixedInPlace")))
                r << row(QStringLiteral("Fixed"), setup.value(QStringLiteral("fixedInPlace")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
            // Ball-detector search box (hitting-area ROI) + any calibrated ball position.
            const QJsonObject bd = setup.value(QStringLiteral("ballDetection")).toObject();
            const QJsonArray roi = bd.value(QStringLiteral("searchRoi")).toArray();
            if (roi.size() == 4)
                r << row(QStringLiteral("Search ROI"),
                         QStringLiteral("[%1, %2, %3, %4]")
                             .arg(roi.at(0).toDouble(), 0, 'f', 2).arg(roi.at(1).toDouble(), 0, 'f', 2)
                             .arg(roi.at(2).toDouble(), 0, 'f', 2).arg(roi.at(3).toDouble(), 0, 'f', 2));
            if (bd.value(QStringLiteral("positionSource")).toString() == QLatin1String("calibrated")) {
                const QJsonArray c = bd.value(QStringLiteral("center")).toArray();
                if (c.size() == 2)
                    r << row(QStringLiteral("Ball pos"),
                             QStringLiteral("[%1, %2] r%3")
                                 .arg(c.at(0).toDouble(), 0, 'f', 3).arg(c.at(1).toDouble(), 0, 'f', 3)
                                 .arg(bd.value(QStringLiteral("radiusNorm")).toDouble(), 0, 'f', 3));
            }
        } else if (kind == QLatin1String("imu")) {
            const QJsonObject dev = st.value(QStringLiteral("device")).toObject();
            if (dev.contains(QStringLiteral("outputRateHz"))) r << row(QStringLiteral("Rate"), QString::number(dev.value(QStringLiteral("outputRateHz")).toInt()) + QStringLiteral(" Hz"));
            if (dev.contains(QStringLiteral("fusionMode")))   r << row(QStringLiteral("Fusion"), dev.value(QStringLiteral("fusionMode")).toString());
            if (dev.contains(QStringLiteral("orientationFilter"))) r << row(QStringLiteral("Filter"), dev.value(QStringLiteral("orientationFilter")).toString());
            if (dev.contains(QStringLiteral("placementSlot"))) r << row(QStringLiteral("Slot"), dev.value(QStringLiteral("placementSlot")).toString());
            // Effective body role: from the stream (new export) or the settings fallback.
            QString roleName = dev.value(QStringLiteral("roleName")).toString();
            if (roleName.isEmpty() && dev.contains(QStringLiteral("role")))
                roleName = pinpoint::analysis::segmentRoleName(
                               pinpoint::analysis::SegmentRole(dev.value(QStringLiteral("role")).toInt()));
            if (roleName.isEmpty())
                roleName = pinpoint::analysis::segmentRoleName(pinpoint::analysis::segmentRoleForSlot(
                               m_sessionType, m_imuPlacement.value(src.value(QStringLiteral("serial")).toString()).toString()));
            if (!roleName.isEmpty() && roleName != QLatin1String("Unknown"))
                r << row(QStringLiteral("Role"), roleName);
            const QJsonObject sm = st.value(QStringLiteral("samples")).toObject();
            if (sm.contains(QStringLiteral("count"))) r << row(QStringLiteral("Samples"), QString::number(sm.value(QStringLiteral("count")).toInt()));
        }
        pushGroup(QStringLiteral("Stream · ") + (alias.isEmpty() ? kind : alias), r);
    }
    {   // Bindings (IMU calibration audit)
        const QJsonArray binds = analysis.value(QStringLiteral("bindings")).toArray();
        for (const QJsonValue &bv : binds) {
            const QJsonObject bo = bv.toObject();
            QVariantList r;
            if (bo.contains(QStringLiteral("serial"))) r << row(QStringLiteral("Serial"), bo.value(QStringLiteral("serial")).toString());
            r << row(QStringLiteral("Calibrated"), bo.value(QStringLiteral("calibrated")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
            if (bo.contains(QStringLiteral("mountDeviationDeg"))) r << row(QStringLiteral("Mount dev"), QString::number(bo.value(QStringLiteral("mountDeviationDeg")).toDouble(), 'f', 1) + QStringLiteral("°"));
            pushGroup(QStringLiteral("Binding · role ") + QString::number(bo.value(QStringLiteral("role")).toInt()), r);
        }
    }
    {   // Review
        const QJsonObject rev = m_doc.value(QStringLiteral("review")).toObject();
        QVariantList r;
        if (rev.contains(QStringLiteral("rating"))) r << row(QStringLiteral("Rating"), QString::number(rev.value(QStringLiteral("rating")).toInt()) + QStringLiteral(" ★"));
        if (rev.contains(QStringLiteral("note")))   r << row(QStringLiteral("Note"), rev.value(QStringLiteral("note")).toString());
        pushGroup(QStringLiteral("Review"), r);
    }

    emit loadedChanged();
    rebuild();
}

// ── region resolution ────────────────────────────────────────────────────────
QVector<SwingDataSource::PartSpec> SwingDataSource::specsForRegion(const QString &region) const
{
    using R = pinpoint::analysis::SegmentRole;
    const QString imu  = QStringLiteral("imu");
    const QString pose = QStringLiteral("pose");
    const QString club = QStringLiteral("club");
    const QString met  = QStringLiteral("metric");
    auto imuRole = [&](R r) { return PartSpec{ imu, {}, {}, int(r) }; };  // strict-by-role
    // "lead-wrist" in the §2 catalogue spans both wrist-area sensors; each shows under
    // its own role, data-aware (only present ones appear).

    if (region == QLatin1String("Custom"))
        return m_customSpecs;
    if (region == QLatin1String("Lower"))
        return { {pose, {}, QStringLiteral("ankles")}, {pose, {}, QStringLiteral("knees")},
                 {pose, {}, QStringLiteral("hips")},   imuRole(R::Pelvis) };
    if (region == QLatin1String("Upper"))
        return { {pose, {}, QStringLiteral("shoulders")}, {pose, {}, QStringLiteral("elbows")},
                 {pose, {}, QStringLiteral("wrists")},
                 imuRole(R::LeadUpperArm), imuRole(R::LeadForearm), imuRole(R::LeadHand),
                 {met, QStringLiteral("leadArmFlexion"), {}}, {met, QStringLiteral("forearmPronation"), {}} };
    if (region == QLatin1String("Delivery"))
        return { {pose, {}, QStringLiteral("wrists")}, {pose, {}, QStringLiteral("elbows")},
                 {club, QStringLiteral("shaft"), QStringLiteral("shaft")},
                 {club, QStringLiteral("head"),  QStringLiteral("head")},
                 {QStringLiteral("ball"), QStringLiteral("ball"), QStringLiteral("ball")},
                 imuRole(R::LeadForearm), imuRole(R::LeadHand),
                 {met, QStringLiteral("leadWristFlexExt"), {}}, {met, QStringLiteral("leadWristRadUln"), {}} };
    // Axial (default)
    return { imuRole(R::Pelvis), imuRole(R::Thorax),
             {pose, {}, QStringLiteral("hips")}, {pose, {}, QStringLiteral("shoulders")},
             {met, QStringLiteral("xFactor"), {}} };
}

const SwingSeries *SwingDataSource::findSeries(const QString &kind, const QString &ref,
                                               const QString &part) const
{
    for (const SwingSeries &s : m_all) {
        if (kindToStr(s.kind) != kind) continue;
        if (s.kind == SwingSeries::Pose)  { if (s.part == part) return &s; }
        else if (s.kind == SwingSeries::Imu) { if (s.ref == ref) return &s; }
        else if (s.kind == SwingSeries::Metric) { if (s.ref == ref) return &s; }
        else if (s.kind == SwingSeries::Club) { if (s.ref == ref) return &s; }
        else if (s.kind == SwingSeries::Ball) { return &s; }
    }
    return nullptr;
}

QVector<SwingSeries> SwingDataSource::resolve(const QVector<PartSpec> &specs) const
{
    QVector<SwingSeries> out;
    auto seen = [&out](const SwingSeries &s) {
        for (const SwingSeries &o : out)
            if (o.kind == s.kind && o.ref == s.ref && o.part == s.part) return true;
        return false;
    };
    bool wantedImuByRole = false, matchedImuByRole = false;
    for (const PartSpec &spec : specs) {
        if (spec.kind == QLatin1String("imu")) {
            if (spec.role >= 1) {                         // strict-by-role
                wantedImuByRole = true;
                for (const SwingSeries &s : m_all)
                    if (s.kind == SwingSeries::Imu && s.imuRole == spec.role && !seen(s)) {
                        out.push_back(s); matchedImuByRole = true;
                    }
            } else if (spec.ref == QLatin1String("*")) {  // generic all
                for (const SwingSeries &s : m_all)
                    if (s.kind == SwingSeries::Imu && !seen(s)) out.push_back(s);
            } else {                                      // concrete alias (Custom)
                const SwingSeries *p = findSeries(spec.kind, spec.ref, spec.part);
                if (p && !seen(*p)) out.push_back(*p);
            }
            continue;
        }
        const SwingSeries *p = findSeries(spec.kind, spec.ref, spec.part);
        if (p && !seen(*p)) out.push_back(*p);
    }
    // Graceful fallback: the region wants IMUs by role, none matched, and the swing
    // carries no role info at all (pre-role export AND device not in settings) → show
    // every IMU generically so old files are not silently IMU-less.
    if (wantedImuByRole && !matchedImuByRole && !m_anyImuRoleKnown)
        for (const SwingSeries &s : m_all)
            if (s.kind == SwingSeries::Imu && !seen(s)) out.push_back(s);

    // The first IMU in the resolved set is the primary (raw+fused, Δt/State scope).
    for (SwingSeries &s : out)
        if (s.kind == SwingSeries::Imu) { s.primary = true; break; }
    return out;
}

void SwingDataSource::rebuild()
{
    m_resolved = resolve(specsForRegion(m_region));
    // Coverage is a stable full-swing overview; the table windows to the detail
    // range, which the strip marks with its selection band.
    m_coverage->setLanes(m_resolved, 0, m_spanUs, 64);
    m_table->configure(m_resolved, m_windowStartUs, m_windowEndUs, m_fillMode);
    emit viewChanged();
}

QVariantList SwingDataSource::resolvedSources() const
{
    QVariantList out;
    for (const SwingSeries &s : m_resolved) {
        QVariantMap m;
        m[QStringLiteral("label")]     = s.label;
        m[QStringLiteral("colorKey")]  = s.colorKey;
        m[QStringLiteral("kind")]      = kindToStr(s.kind);
        m[QStringLiteral("ref")]       = s.ref;
        m[QStringLiteral("part")]      = s.part;
        m[QStringLiteral("removable")] = s.removable;
        out.append(m);
    }
    return out;
}

// ── source editing (region → Custom) ─────────────────────────────────────────
void SwingDataSource::removeSource(int index)
{
    if (index < 0 || index >= m_resolved.size()) return;
    QVector<PartSpec> specs;
    for (int i = 0; i < m_resolved.size(); ++i) {
        if (i == index) continue;
        const SwingSeries &s = m_resolved[i];
        specs.push_back({ kindToStr(s.kind), s.ref, s.part });
    }
    m_customSpecs = specs;
    m_region = QStringLiteral("Custom");
    rebuild();
}

void SwingDataSource::addSource(const QString &kind, const QString &ref, const QString &part)
{
    // Capture the current resolved set as concrete specs, then append the new one.
    QVector<PartSpec> specs;
    for (const SwingSeries &s : m_resolved)
        specs.push_back({ kindToStr(s.kind), s.ref, s.part });
    specs.push_back({ kind, ref, part });
    m_customSpecs = specs;
    m_region = QStringLiteral("Custom");
    rebuild();
}

} // namespace pinpoint
