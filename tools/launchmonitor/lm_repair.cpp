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

// lm_repair — put the `lm.*` metric rows back into a swing.json from the raw
// `launchMonitor` block that is already in the file.
//
// WHY THIS EXISTS. A launch monitor reading lands in a swing.json in two places: the raw
// `launchMonitor` block, and one `lm.`-prefixed row per field inside analysis.metrics[].
// Only the second has readers — the reload path takes `kind` out of the raw block and
// nothing else (swing_doc.cpp, readSwingJson), and the session board, the tiles and every
// `lm.` grade read the metric rows. Until the carry-forward in writeSwingJson, re-analysis
// rebuilt analysis.metrics[] from the stages alone and dropped every row, because no stage
// produces one: a reading is paired to a swing AFTER the stages have run. The result was a
// file that still looked complete and a launch monitor that read as never connected.
//
// The repair is exact rather than reconstructive. lm::fieldDefs() is the single enumeration
// point for both shapes — `rawName` names the raw column, `key` names the metric — so every
// value the rows carried is still sitting in the block under a different name.
//
// IT GOES THROUGH THE PRODUCTION EMITTER, deliberately. Rebuilding the rows here would be a
// second implementation of updateLaunchMonitor(), and the derived `compoundMiss` would be a
// second implementation of a classifier whose own header exists to stop exactly that (see
// lm_inferred_reads.h — "two derivations of one judgement is the drift the whole header is
// arranged to prevent"). So this reads the block back into a LaunchMonitorReading and hands
// it to SwingDocWriter::updateLaunchMonitor(), which is idempotent by construction: it
// rewrites the raw block, strips any surviving `lm.` and derived rows before re-adding, and
// refreshes the summary sidecar guard that the rewrite invalidates.
//
// Safe to re-run. Safe on a swing that never had a reading (there is nothing to read back,
// and it is skipped). Reports what it changed and touches nothing else in the document.
//
//   lm_repair [--dry-run] <path> [<path> ...]
//
// A path may be a swing directory (one holding swing.json) or a session directory, in which
// case every swing_* beneath it is visited.

#include "swing_doc.h"
#include "launch_monitor_reading.h"
#include "pp_debug.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTextStream>

using namespace pinpoint;

// PpLogStream, provided here rather than linked. The real implementation (pp_debug.cpp)
// includes whisper.h and would drag ggml and FFmpeg into a tool that reads and writes one
// JSON file; the Core test stub links but SWALLOWS the line, which is the wrong trade for
// something that writes to a golfer's library. This routes to stderr, so anything the writer
// says on the way past reaches whoever is running the repair — separate from stdout, which
// carries the per-swing report.
PpLogStream::PpLogStream(QtMsgType t) : m_type(t) { m_dbg.emplace(&m_buf); }
PpLogStream::~PpLogStream()
{
    m_dbg.reset();                       // flush QDebug into m_buf before reading it
    if (!m_buf.isEmpty())
        QTextStream(stderr) << m_buf << "\n";
}

namespace {

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

// Read the raw block back into a reading. The inverse of lmRawBlock(), and it walks the same
// table, so a field added to fieldDefs() is picked up here with no edit.
//
// An ABSENT column stays an empty optional rather than becoming zero. That distinction is the
// one the whole `std::optional` treatment of a reading exists to preserve — "the device did
// not report this" and "the device measured zero" are different facts, and on these swings it
// is load-bearing: a single sticker on the club yields path and speed but no face angle, and a
// faceAngle of 0.0 would be a measurement nobody made.
std::optional<lm::LaunchMonitorReading> readingFromRawBlock(const QJsonObject &raw)
{
    if (raw.isEmpty())
        return std::nullopt;

    lm::LaunchMonitorReading r;
    r.deviceKind   = raw.value(QStringLiteral("kind")).toString();
    r.deviceShotId = raw.value(QStringLiteral("deviceShotId")).toString();
    r.deviceClub   = raw.value(QStringLiteral("deviceClub")).toString();
    r.sourcePath   = raw.value(QStringLiteral("sourcePath")).toString();
    r.readAtMs     = qint64(raw.value(QStringLiteral("readAtMs")).toDouble());

    for (const lm::FieldDef &f : lm::fieldDefs()) {
        const QJsonValue v = raw.value(QString::fromLatin1(f.rawName));
        if (v.isDouble())
            r.*(f.member) = v.toDouble();
    }

    // A block carrying provenance and no numbers is not a reading, and attributing it to a
    // shot would put an empty launch monitor on a swing that never had one.
    if (!r.hasAnyValue())
        return std::nullopt;
    return r;
}

int countLmRows(const QJsonObject &root)
{
    int n = 0;
    for (const QJsonValue &v : root.value(QStringLiteral("analysis")).toObject()
                                   .value(QStringLiteral("metrics")).toArray())
        if (v.toObject().value(QStringLiteral("key")).toString().startsWith(QStringLiteral("lm.")))
            ++n;
    return n;
}

bool loadDoc(const QString &path, QJsonObject *root)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close();
    if (pe.error != QJsonParseError::NoError)
        return false;
    *root = d.object();
    return true;
}

// The outcomes are counted apart rather than summed into one "skipped", because a sweep is
// read for what it did NOT have to change and those answers are not interchangeable: a swing
// that never met a launch monitor is fine, a swing whose rows are intact is fine, and a swing
// with no swing.json at all is an unanalysed capture — which is worth seeing on a report about
// missing data even though it is not this tool's business to fix.
struct Tally {
    int visited = 0, repaired = 0, intact = 0, noReading = 0, noDoc = 0, failed = 0, rows = 0;
};

void repairSwing(const QString &swingDir, bool dryRun, Tally &t)
{
    const QString path = swingDir + QStringLiteral("/swing.json");
    if (!QFile::exists(path)) {
        // SAID OUT LOUD, not passed over. A silent skip here reads as a clean session on the
        // report, which is the one thing a recovery sweep must never do.
        out() << "  " << QDir(swingDir).dirName() << ": no swing.json — never analysed\n";
        ++t.noDoc;
        return;
    }

    ++t.visited;
    const QString name = QDir(swingDir).dirName();

    QJsonObject root;
    if (!loadDoc(path, &root)) {
        out() << "  " << name << ": FAILED — cannot read or parse swing.json\n";
        ++t.failed;
        return;
    }

    const auto reading = readingFromRawBlock(root.value(QStringLiteral("launchMonitor")).toObject());
    if (!reading) {
        out() << "  " << name << ": no launch monitor reading in this document\n";
        ++t.noReading;
        return;
    }

    const int before = countLmRows(root);

    // Count what the block can yield, so a dry run states the outcome rather than promising one.
    int available = 0;
    for (const lm::FieldDef &f : lm::fieldDefs())
        if ((*reading).*(f.member))
            ++available;

    if (before >= available) {
        out() << "  " << name << ": already intact — " << before << " lm.* rows\n";
        ++t.intact;
        return;
    }

    if (dryRun) {
        out() << "  " << name << ": would restore " << available << " lm.* rows (has "
              << before << ")\n";
        ++t.repaired;
        t.rows += available - before;
        return;
    }

    QString err;
    if (!SwingDocWriter::updateLaunchMonitor(swingDir, *reading, &err)) {
        out() << "  " << name << ": FAILED — " << err << "\n";
        ++t.failed;
        return;
    }

    QJsonObject after;
    if (!loadDoc(path, &after)) {
        out() << "  " << name << ": FAILED — wrote, but cannot re-read to verify\n";
        ++t.failed;
        return;
    }
    const int now = countLmRows(after);
    if (now < available) {
        out() << "  " << name << ": FAILED — restored " << now << " of " << available << " rows\n";
        ++t.failed;
        return;
    }

    out() << "  " << name << ": restored " << now << " lm.* rows (was " << before << ")\n";
    ++t.repaired;
    t.rows += now - before;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QStringList paths;
    bool dryRun = false;
    for (const QString &a : QCoreApplication::arguments().mid(1)) {
        if (a == QStringLiteral("--dry-run") || a == QStringLiteral("-n"))
            dryRun = true;
        else if (a.startsWith(QStringLiteral("-"))) {
            out() << "unknown option: " << a << "\n";
            return 2;
        } else
            paths << a;
    }

    if (paths.isEmpty()) {
        out() << "usage: lm_repair [--dry-run] <swing-or-session-dir> [...]\n";
        return 2;
    }

    Tally t;
    for (const QString &p : paths) {
        const QString clean = QDir::cleanPath(p);
        if (!QDir(clean).exists()) {
            out() << clean << ": no such directory\n";
            ++t.failed;
            continue;
        }

        if (QFile::exists(clean + QStringLiteral("/swing.json"))) {
            out() << clean << "\n";
            repairSwing(clean, dryRun, t);
            continue;
        }

        // A session directory: every swing_* beneath it, in order.
        QDir d(clean);
        const QStringList swings =
            d.entryList({ QStringLiteral("swing_*") }, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        if (swings.isEmpty()) {
            out() << clean << ": neither a swing directory nor a session with swings in it\n";
            ++t.failed;
            continue;
        }
        out() << clean << "\n";
        for (const QString &s : swings)
            repairSwing(d.filePath(s), dryRun, t);
    }

    out() << "\n" << (dryRun ? "would repair " : "repaired ") << t.repaired
          << " of " << t.visited << " swings carrying a swing.json"
          << " (" << t.rows << " lm.* rows)\n"
          << "  " << t.intact    << " already intact\n"
          << "  " << t.noReading << " never had a launch monitor reading\n"
          << "  " << t.noDoc     << " with no swing.json at all — never analysed\n"
          << "  " << t.failed    << " failed\n";
    out().flush();
    return t.failed == 0 ? 0 : 1;
}
