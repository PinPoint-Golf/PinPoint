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

#include "resource_monitor_controller.h"
#include "app_settings.h"
#include "camera_manager.h"
#include "imu_manager.h"
#include "pp_debug.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <cmath>

ResourceMonitorController::ResourceMonitorController(
    pinpoint::EventBuffer *buffer,
    CameraManager         *cameras,
    ImuManager            *imu,
    QObject               *parent)
    : QObject(parent)
    , m_buffer(buffer)
    , m_cameras(cameras)
    , m_imu(imu)
{
    m_ageTimer.start();
}

static QString bufferStateString(pinpoint::BufferState s)
{
    switch (s) {
    case pinpoint::BufferState::Idle:      return QStringLiteral("idle");
    case pinpoint::BufferState::Capturing: return QStringLiteral("capturing");
    case pinpoint::BufferState::Paused:    return QStringLiteral("paused");
    case pinpoint::BufferState::Stopping:  return QStringLiteral("stopping");
    }
    return QStringLiteral("idle");
}

static QString fmtCount(quint64 n)
{
    if (n >= 1000000) return QString::number(n / 1000000.0, 'f', 2) + QStringLiteral(" M");
    if (n >= 1000)    return QString::number(n / 1000.0,    'f', 1) + QStringLiteral(" K");
    return QString::number(n);
}

static QString fmtBytes(quint64 n)
{
    if (n >= 1073741824ULL) return QString::number(n / 1073741824.0, 'f', 2) + QStringLiteral(" GB");
    if (n >= 1048576ULL)    return QString::number(n / 1048576.0,    'f', 1) + QStringLiteral(" MB");
    if (n >= 1024ULL)       return QString::number(n / 1024.0,       'f', 0) + QStringLiteral(" KB");
    return QString::number(n) + QStringLiteral(" B");
}

static QString fmtInterArrival(int64_t us)
{
    if (us <= 0)         return QStringLiteral("—");
    if (us >= 1000000)   return QString::number(us / 1000000.0, 'f', 1) + QStringLiteral(" s");
    if (us >= 1000)      return QString::number(us / 1000.0,    'f', 1) + QStringLiteral(" ms");
    return QString::number(us) + QStringLiteral(" µs");
}

void ResourceMonitorController::refresh()
{
    if (!m_buffer) return;

    m_snapshot = m_buffer->diagnostics();
    const bool capturing = (m_snapshot.state == pinpoint::BufferState::Capturing);

    // Alias maps — read once per refresh so devices and sources share the same snapshot.
    AppSettings appSettings;
    const QVariantMap camAliasMap = appSettings.cameraAlias();
    const QVariantMap imuAliasMap = appSettings.imuAlias();

    // sourceId → alias, populated while building the devices section below.
    QMap<pinpoint::SourceId, QString> sourceAliases;

    // ── Sources ──────────────────────────────────────────────────────────────
    // Built after devices so sourceAliases is populated first — see below.
    auto buildSources = [&]() {
    m_sources.clear();
    for (const auto &src : m_snapshot.sources) {
        double fill = src.slot_count > 0
            ? std::min(1.0, double(src.events_written) / double(src.slot_count))
            : 0.0;
        // Cumulative counters use the LIFETIME totals (folded pre-reset history +
        // current ring) so they survive each resume()/ring->reset() — i.e. every
        // shot-replay cycle. Matches the camera/IMU device rows below. The shot
        // marker source is only shown here (it is not a DeviceEnumerator device),
        // so without this its count zeroed on every replay. ringFill stays on the
        // raw events_written — it reflects CURRENT ring occupancy, not lifetime.
        quint64 overwritten = quint64(src.lifetime_events_overwritten);
        int64_t maxIa = src.max_inter_arrival_us;
        const QString srcAlias = sourceAliases.value(src.id, QString::fromStdString(src.name));
        QVariantMap m;
        m[QStringLiteral("name")]               = srcAlias;
        m[QStringLiteral("identifier")]         = QString::fromStdString(src.identifier);
        m[QStringLiteral("eventsWritten")]       = quint64(src.lifetime_events_written);
        m[QStringLiteral("eventsOverwritten")]   = overwritten;
        m[QStringLiteral("bytesWritten")]        = quint64(src.lifetime_bytes_written);
        m[QStringLiteral("slotCount")]           = quint64(src.slot_count);
        m[QStringLiteral("stalled")]             = src.stalled;
        m[QStringLiteral("maxInterArrivalUs")]   = qint64(maxIa);
        m[QStringLiteral("boundsViolations")]    = quint64(src.bounds_violations);
        m[QStringLiteral("monoViolations")]      = quint64(src.monotonicity_violations);
        m[QStringLiteral("ringFillFraction")]    = fill;
        // Pre-formatted strings for QML (no JS formatting needed)
        m[QStringLiteral("eventsWrittenStr")]    = fmtCount(quint64(src.lifetime_events_written));
        m[QStringLiteral("eventsOverwrittenStr")]= overwritten > 0 ? QString::number(overwritten) : QStringLiteral("—");
        m[QStringLiteral("bytesWrittenStr")]     = fmtBytes(quint64(src.lifetime_bytes_written));
        m[QStringLiteral("maxInterArrivalStr")]  = fmtInterArrival(maxIa);
        m_sources.append(m);
    }
    }; // end buildSources lambda — called after devices so sourceAliases is ready

    // ── Devices ──────────────────────────────────────────────────────────────
    m_devices.clear();

    auto findSourceById = [this](pinpoint::SourceId sid)
        -> const pinpoint::EventBuffer::DiagnosticsSnapshot::SourceInfo*
    {
        for (const auto &src : m_snapshot.sources)
            if (src.id == sid) return &src;
        return nullptr;
    };

    // Session-lifetime totals for a device whose source is no longer registered
    // (deselected). Keyed by the same identifier the source was registered with
    // (serial-or-device-id for cameras, serial-or-id for IMUs).
    auto findLifetimeByIdent = [this](const QString &ident)
        -> const pinpoint::EventBuffer::DiagnosticsSnapshot::LifetimeInfo*
    {
        const std::string key = ident.toStdString();
        for (const auto &lt : m_snapshot.lifetime)
            if (lt.identifier == key) return &lt;
        return nullptr;
    };

    // Camera entries — one per enumerated device, mirrors the IMU pattern.
    {
        const QList<Device> camDevices =
            DeviceEnumerator::instance()->devices(DeviceType::VideoInput);

        for (const Device &camDev : camDevices) {
            const auto cam = m_cameras->liveDeviceStats(camDev.id);

            const pinpoint::SourceId sid = cam.sourceId;
            const auto *srcInfo = findSourceById(sid);

            QString backendStr;
            switch (camDev.backend) {
            case VideoInputFactory::Backend::Aravis:    backendStr = QStringLiteral("Aravis");         break;
            case VideoInputFactory::Backend::Spinnaker: backendStr = QStringLiteral("Spinnaker");      break;
            default:                                    backendStr = QStringLiteral("Qt Multimedia");  break;
            }

            const QString sn = camDev.capabilities.serialNumber;
            const QString camKey = camDev.description + QStringLiteral("|") +
                                   (sn.isEmpty() ? camDev.id : sn);
            const QString camAlias = camAliasMap.value(camKey,
                sn.isEmpty() ? camDev.description
                             : camDev.description + QStringLiteral(" (") + sn + QStringLiteral(")")).toString();

            if (sid != pinpoint::kInvalidSourceId)
                sourceAliases[sid] = camAlias;

            // Live frame rate and resolution from the controller when available;
            // fall back to enumerated capabilities before any frame has arrived.
            double rate = cam.fps;
            int    fw   = cam.width;
            int    fh   = cam.height;
            if (fw == 0 || fh == 0) {
                fw = camDev.capabilities.resolution.defaultResolution.width;
                fh = camDev.capabilities.resolution.defaultResolution.height;
            }

            QString camStatus;
            if (sid == pinpoint::kInvalidSourceId) camStatus = QStringLiteral("idle");
            else if (cam.recording)                camStatus = QStringLiteral("streaming");
            else                                   camStatus = QStringLiteral("ready");

            // Events/bytes are session-lifetime totals so the card keeps showing
            // them after the camera is deselected (source deregistered) and
            // across pause→resume ring clears. Live source → folded+current;
            // deselected → folded total looked up by the device's identifier.
            const QString camIdent = sn.isEmpty() ? camDev.id : sn;
            const auto *camLife = srcInfo ? nullptr : findLifetimeByIdent(camIdent);
            quint64 evW  = srcInfo  ? quint64(srcInfo->lifetime_events_written)
                         : camLife  ? quint64(camLife->events_written)      : 0;
            quint64 byW  = srcInfo  ? quint64(srcInfo->lifetime_bytes_written)
                         : camLife  ? quint64(camLife->bytes_written)       : 0;
            quint64 evOW = srcInfo  ? quint64(srcInfo->lifetime_events_overwritten)
                         : camLife  ? quint64(camLife->events_overwritten)  : 0;
            double  fill = srcInfo && srcInfo->slot_count > 0
                ? std::min(1.0, double(srcInfo->events_written) / double(srcInfo->slot_count))
                : 0.0;
            quint64 ringBytes = (sid != pinpoint::kInvalidSourceId)
                ? quint64(m_buffer->getSlotCapacity(sid)) * quint64(m_buffer->getSlotCount(sid))
                : 0;
            QString srcName = srcInfo ? QString::fromStdString(srcInfo->name) : QString();

            QVariantMap dev;
            dev[QStringLiteral("kind")]         = QStringLiteral("Camera");
            dev[QStringLiteral("name")]         = camAlias;
            dev[QStringLiteral("model")]        = camDev.description;
            dev[QStringLiteral("serialNumber")] = sn;
            dev[QStringLiteral("backend")]      = backendStr;
            dev[QStringLiteral("status")]       = camStatus;
            dev[QStringLiteral("dataRateHz")]   = rate;
            dev[QStringLiteral("batteryPct")]   = -1;
            dev[QStringLiteral("sourceName")]   = srcName;
            dev[QStringLiteral("ringFill")]     = fill;
            dev[QStringLiteral("hasWarning")]   = capturing && srcInfo && srcInfo->stalled;
            dev[QStringLiteral("eventsWritten")]     = evW;
            dev[QStringLiteral("bytesWritten")]      = byW;
            dev[QStringLiteral("eventsOverwritten")] = evOW;
            dev[QStringLiteral("dataRateStr")]   = rate > 0
                ? QString::number(rate, 'f', 1) + QStringLiteral(" fps")
                : QStringLiteral("—");
            dev[QStringLiteral("eventsWrittenStr")]    = fmtCount(evW);
            dev[QStringLiteral("bytesWrittenStr")]     = fmtBytes(byW);
            dev[QStringLiteral("eventsOverwrittenStr")] = evOW > 0
                ? QString::number(evOW) : QStringLiteral("0");
            dev[QStringLiteral("ringCapacityStr")] = ringBytes > 0
                ? fmtBytes(ringBytes) : QStringLiteral("—");
            dev[QStringLiteral("resolutionStr")] = (fw > 0 && fh > 0)
                ? QString::number(fw) + QStringLiteral(" × ") + QString::number(fh)
                : QStringLiteral("—");
            {
                // The crop is applied at connect, so a live controller's
                // frameWidth/frameHeight already ARE the cropped dimensions —
                // multiplying by the normalized crop again would double-
                // discount. When no capture controller is active, fall back
                // to the persisted AppSettings value (as percentages — the
                // sensor dims aren't known here) so the row still shows for
                // cameras that are configured but not currently selected.
                QString cropStr = QStringLiteral("—");
                if (!cam.cropRoi.isEmpty() && fw > 0 && fh > 0) {
                    cropStr = QString::number(fw) + QStringLiteral(" × ")
                            + QString::number(fh);
                } else {
                    const QVariantMap roiMap = appSettings.cameraRoi();
                    if (roiMap.contains(camKey)) {
                        const QVariantMap r = roiMap.value(camKey).toMap();
                        double w = r.value(QStringLiteral("w")).toDouble();
                        double h = r.value(QStringLiteral("h")).toDouble();
                        if (w > 0 && h > 0)
                            cropStr = QString::number(qRound(w * 100)) + QStringLiteral("% × ")
                                    + QString::number(qRound(h * 100)) + QStringLiteral("%");
                    }
                }
                dev[QStringLiteral("cropStr")] = cropStr;
            }
            m_devices.append(dev);
        }
    }

    // IMU entries — one per enumerated device (mirrors cameraList pattern).
    // Devices appear regardless of connection state; the live instance (if
    // selected) supplies data rates, battery, and buffer source info.
    {
        const QList<Device> imuDevices =
            DeviceEnumerator::instance()->devices(DeviceType::Imu);

        for (const Device &imuDev : imuDevices) {
            const auto imu = m_imu->liveDeviceStats(imuDev.id);

            const QString backend = imuDev.imuTransport == ImuBase::Transport::Ble
                ? QStringLiteral("Bluetooth LE")
                : QStringLiteral("Serial");
            const QString model = imuDev.imuCapabilities.modelName.isEmpty()
                ? imuDev.description
                : imuDev.imuCapabilities.modelName;

            // ⚠ Status keys off `selected`, not off source presence. A Phase A
            // HackMotion registers ZERO EventBuffer sources even while fully
            // connected (see ImuDeviceStats::selected and hm_instance.h) — the
            // old "sourceId == invalid ⇒ idle" test would have shown a live wG3
            // as idle forever. `selected` is the direct signal instead.
            QString imuStatus;
            if (!imu.selected)      imuStatus = QStringLiteral("idle");
            else if (imu.connected) imuStatus = QStringLiteral("connected");
            else if (imu.busy)      imuStatus = QStringLiteral("connecting");
            else                    imuStatus = QStringLiteral("disconnected");

            double  imuRate      = imu.dataRateHz;
            int     batPct       = imu.batteryPercent;
            // Session-lifetime totals (see camera section) — keyed by the same
            // serial-or-device-id identifier ImuInstance registers the source with.
            const QString imuIdent = imuDev.imuCapabilities.serialNumber.isEmpty()
                ? imuDev.id
                : imuDev.imuCapabilities.serialNumber;

            const QString imuId = imuDev.imuCapabilities.serialNumber.isEmpty()
                ? imuDev.id
                : imuDev.imuCapabilities.serialNumber;
            const QString imuKey   = imuDev.description + QStringLiteral("|") + imuDev.id;
            const QString imuAlias = imuAliasMap.value(imuKey,
                imuId.isEmpty() ? imuDev.description
                                : imuDev.description + QStringLiteral(" (") + imuId + QStringLiteral(")")).toString();

            // ⚠ PLURAL, AND THAT IS THE POINT (see ImuDeviceBase::sourceIds()).
            // A HackMotion carries two sources (lower arm, palm, from Phase B
            // onward); this device-level loop already runs per enumerated
            // Device, so a second inner loop turns each source into its own
            // row rather than the row silently reporting only the first one.
            // An HmInstance that registers none (e.g. mid-connect) falls back
            // through the empty-vector case below into exactly one row — live
            // state, no buffer statistics — instead of hiding the device or
            // indexing past the end of an empty vector.
            const bool multiSource = imu.sourceIds.size() > 1;
            const int  rowCount    = imu.sourceIds.empty() ? 1 : int(imu.sourceIds.size());
            // ⚠ Labels beat "#N": both wG3 sources would otherwise resolve to
            // the SAME device-level alias below, so the two buffer-source
            // lanes in the one view whose job is telling you which lane is
            // recording would be indistinguishable from each other. Only used
            // when the instance supplies exactly one label per row — a
            // mismatched count (e.g. mid-registration) falls back to "#N"
            // rather than pairing labels to the wrong rows.
            const bool hasLabels = imu.sourceLabels.size() == rowCount;

            for (int row = 0; row < rowCount; ++row) {
                const pinpoint::SourceId sid = imu.sourceIds.empty()
                    ? pinpoint::kInvalidSourceId : imu.sourceIds[row];
                const auto *imuSrc = findSourceById(sid);
                const auto *imuLife = imuSrc ? nullptr : findLifetimeByIdent(imuIdent);

                quint64 imuEW = imuSrc  ? quint64(imuSrc->lifetime_events_written)
                              : imuLife ? quint64(imuLife->events_written)      : 0;
                quint64 imuBW = imuSrc  ? quint64(imuSrc->lifetime_bytes_written)
                              : imuLife ? quint64(imuLife->bytes_written)       : 0;
                quint64 imuOW = imuSrc  ? quint64(imuSrc->lifetime_events_overwritten)
                              : imuLife ? quint64(imuLife->events_overwritten)  : 0;
                quint64 imuRingBytes = imuSrc
                    ? quint64(m_buffer->getSlotCapacity(imuSrc->id)) * quint64(imuSrc->slot_count)
                    : 0;
                double fill = imuSrc && imuSrc->slot_count > 0
                    ? std::min(1.0, double(imuSrc->events_written) / double(imuSrc->slot_count))
                    : 0.0;
                QString imuSrcName = imuSrc
                    ? QString::fromStdString(imuSrc->name) : QString();

                // One physical device, more than one row: distinguish them by
                // the device's own source label ("Lower arm" / "Palm") when it
                // supplies one, since a "#N" suffix by itself doesn't tell a
                // coach which strap moved. Falls back to "#N" when multiSource
                // but hasLabels is false, and is empty for a single-source
                // device — a Witmotion row is byte-identical to before this
                // change (multiSource is false, so rowSuffix stays "").
                const QString rowSuffix = hasLabels
                    ? QStringLiteral(" · ") + imu.sourceLabels[row]
                    : (multiSource ? QStringLiteral(" #%1").arg(row + 1) : QString());

                // sourceAliases keys the resource monitor's separate Sources
                // table (buildSources() below) — without rowSuffix here, both
                // wG3 sources would collapse onto the SAME device alias and
                // be indistinguishable in that table even though they are
                // distinguishable in the device rows above.
                if (sid != pinpoint::kInvalidSourceId)
                    sourceAliases[sid] = imuAlias + rowSuffix;

                QVariantMap dev;
                dev[QStringLiteral("kind")]               = QStringLiteral("IMU");
                dev[QStringLiteral("name")]               = imuAlias + rowSuffix;
                dev[QStringLiteral("model")]              = model;
                dev[QStringLiteral("backend")]            = backend;
                dev[QStringLiteral("identifier")]         = imuDev.id + rowSuffix;
                dev[QStringLiteral("status")]             = imuStatus;
                dev[QStringLiteral("dataRateHz")]         = imuRate;
                dev[QStringLiteral("batteryPct")]         = batPct;
                dev[QStringLiteral("sourceName")]         = imuSrcName;
                dev[QStringLiteral("ringFill")]           = fill;
                dev[QStringLiteral("hasWarning")]         = capturing && imuSrc && imuSrc->stalled;
                dev[QStringLiteral("eventsWritten")]      = imuEW;
                dev[QStringLiteral("bytesWritten")]       = imuBW;
                dev[QStringLiteral("eventsOverwritten")]  = imuOW;
                dev[QStringLiteral("dataRateStr")]        = imuRate > 0
                    ? QString::number(imuRate, 'f', 1) + QStringLiteral(" Hz")
                    : QStringLiteral("—");
                dev[QStringLiteral("eventsWrittenStr")]   = fmtCount(imuEW);
                dev[QStringLiteral("bytesWrittenStr")]    = fmtBytes(imuBW);
                dev[QStringLiteral("eventsOverwrittenStr")] = imuOW > 0
                    ? QString::number(imuOW) : QStringLiteral("0");
                dev[QStringLiteral("batteryStr")]         = batPct >= 0
                    ? QString::number(batPct) + QStringLiteral(" %")
                    : QStringLiteral("—");
                dev[QStringLiteral("ringCapacityStr")]    = imuRingBytes > 0
                    ? fmtBytes(imuRingBytes) : QStringLiteral("—");
                dev[QStringLiteral("gimbalDropCount")]    = imu.gimbalDropCount;
                dev[QStringLiteral("gimbalDropCountStr")] = imu.gimbalDropCount > 0
                    ? QString::number(imu.gimbalDropCount)
                    : QStringLiteral("0");
                m_devices.append(dev);
            }
        }
    }

    // sourceAliases is now populated — build sources with alias-resolved names.
    buildSources();

    // ── Warnings — only generated while actively capturing ───────────────────
    // Edge-trigger stall/anomaly conditions through ppWarn() so they flow into
    // PpMessageLog automatically.  m_activeWarnings prevents repeated firing.
    m_warnings.clear();
    if (capturing) for (const auto &src : m_snapshot.sources) {
        QString name = QString::fromStdString(src.name);
        if (src.stalled) {
            int64_t stalledMs = (m_snapshot.snapshot_timestamp_us - src.last_write_timestamp_us) / 1000;
            // events_written is a lifetime counter; the ring holds at most
            // slot_count entries (older ones are overwritten), so clamp.
            int fillPct = int(std::round(
                src.slot_count > 0
                    ? double(std::min<uint64_t>(src.events_written, src.slot_count))
                          / double(src.slot_count) * 100.0
                    : 0.0));
            QString msg = QStringLiteral("%1 stalled — no events for %2 ms, ring %3% full")
                              .arg(name).arg(stalledMs).arg(fillPct);
            m_warnings.append(msg);
            if (!m_activeWarnings.contains(msg))
                ppWarn() << qPrintable(msg);
        }
        if (src.bounds_violations > 0 || src.monotonicity_violations > 0) {
            QString msg = QStringLiteral("%1: %2 timestamp anomalies (bounds: %3, monotonicity: %4)")
                              .arg(name)
                              .arg(src.bounds_violations + src.monotonicity_violations)
                              .arg(src.bounds_violations)
                              .arg(src.monotonicity_violations);
            m_warnings.append(msg);
            if (!m_activeWarnings.contains(msg))
                ppWarn() << qPrintable(msg);
        }
    }
    m_activeWarnings = QSet<QString>(m_warnings.begin(), m_warnings.end());

    // ── Pull new entries from the global PpMessageLog into the display list ──
    // Entries arrive oldest-first from fetchSince(); prepend each so the list
    // stays newest-first for the QML view.
    {
        const auto newEntries = PpMessageLog::instance()->fetchSince(m_logSeq);
        for (auto it = newEntries.crbegin(); it != newEntries.crend(); ++it) {
            QVariantMap row;
            row[QStringLiteral("timestamp")] = it->timestamp;
            row[QStringLiteral("severity")]  = it->severity;
            row[QStringLiteral("message")]   = it->message;
            m_messageLog.prepend(row);
        }
        while (m_messageLog.size() > kMaxLogEntries)
            m_messageLog.removeLast();
    }

    // ── Timeline history (delta per tick) ────────────────────────────────────
    quint64 curr = m_snapshot.timeline_entries;
    quint64 delta = (curr >= m_prevTimelineEntries) ? (curr - m_prevTimelineEntries) : 0;
    m_prevTimelineEntries = curr;
    m_timelineHistory.append(delta);
    while (m_timelineHistory.size() > kHistoryCount)
        m_timelineHistory.removeFirst();

    m_ageTimer.restart();
    emit snapshotChanged();
}

QString ResourceMonitorController::bufferState() const
{
    return bufferStateString(m_snapshot.state);
}

quint64 ResourceMonitorController::totalEvents() const
{
    // Lifetime totals (folded history + current ring) so the headline count does
    // not dip on every resume()/ring->reset() — i.e. each shot-replay cycle.
    quint64 total = 0;
    for (const auto &src : m_snapshot.sources)
        total += src.lifetime_events_written;
    return total;
}

quint64 ResourceMonitorController::timelineEntries() const
{
    return m_snapshot.timeline_entries;
}

int ResourceMonitorController::sourceCount() const
{
    return int(m_snapshot.sources.size());
}

QVariantList ResourceMonitorController::sources() const { return m_sources; }
QVariantList ResourceMonitorController::devices() const { return m_devices; }
QStringList  ResourceMonitorController::warnings() const { return m_warnings; }

QString ResourceMonitorController::totalEventsStr() const     { return fmtCount(totalEvents()); }
QString ResourceMonitorController::timelineEntriesStr() const { return fmtCount(m_snapshot.timeline_entries); }

int ResourceMonitorController::snapshotAgeMs() const
{
    return int(m_ageTimer.elapsed());
}

QVariantList ResourceMonitorController::timelineHistory() const
{
    QVariantList result;
    for (quint64 v : m_timelineHistory)
        result.append(QVariant::fromValue(v));
    return result;
}

QVariantList ResourceMonitorController::messageLog() const { return m_messageLog; }

void ResourceMonitorController::clearLog()
{
    m_messageLog.clear();
    m_activeWarnings.clear();
    m_logSeq = PpMessageLog::instance()->currentSeq();
    emit snapshotChanged();
}

// Write the current message log to a timestamped text file in the user's
// home directory.  Returns the full path on success, an empty string on
// failure.  Entries are written oldest-first (m_messageLog is newest-first).
QString ResourceMonitorController::exportLog() const
{
    const QDateTime now = QDateTime::currentDateTime();
    const QString   ts  = now.toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString   path =
        QDir(QDir::homePath()).filePath(QStringLiteral("PinPointStudio_log_%1.txt").arg(ts));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        ppError() << "[ResourceMonitor] Cannot open log export file for writing:" << path;
        return QString();
    }

    QTextStream out(&file);
    out << "PinPointStudio message log — exported "
        << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\n\n";

    for (auto it = m_messageLog.crbegin(); it != m_messageLog.crend(); ++it) {
        const QVariantMap row = it->toMap();
        out << row.value(QStringLiteral("timestamp")).toString() << "  "
            << row.value(QStringLiteral("severity")).toString().leftJustified(5) << "  "
            << row.value(QStringLiteral("message")).toString() << "\n";
    }

    return path;
}

bool ResourceMonitorController::scanning() const { return m_scanning; }

QString ResourceMonitorController::scanStatus() const
{
    if (m_scanning)              return tr("Scan in progress");
    if (!m_lastScanTime.isEmpty()) return tr("Last scan at %1").arg(m_lastScanTime);
    return QString();
}

void ResourceMonitorController::scanDevices()
{
    if (m_scanning) return;
    m_scanning = true;
    emit scanStatusChanged();

    if (m_cameras) m_cameras->enumerate();

    connect(DeviceEnumerator::instance(), &DeviceEnumerator::imuScanFinished,
            this, [this]() {
                m_scanning = false;
                m_lastScanTime = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
                emit scanStatusChanged();
                refresh();
            }, static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection));

    if (m_imu) m_imu->rescanImu();
}
