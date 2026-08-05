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

#include "pack_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace pinpoint::analysis {

// ── ValidationReport ────────────────────────────────────────────────────────

bool ValidationReport::ok() const { return errorCount() == 0; }

int ValidationReport::errorCount() const
{
    return int(std::count_if(issues.begin(), issues.end(),
                             [](const ValidationIssue &i) { return i.severity == IssueSeverity::Error; }));
}

int ValidationReport::warningCount() const
{
    return int(issues.size()) - errorCount();
}

std::vector<ValidationIssue> ValidationReport::withCode(const QString &code) const
{
    std::vector<ValidationIssue> out;
    std::copy_if(issues.begin(), issues.end(), std::back_inserter(out),
                 [&](const ValidationIssue &i) { return i.code == code; });
    return out;
}

std::vector<ValidationIssue> ValidationReport::withSeverity(IssueSeverity s) const
{
    std::vector<ValidationIssue> out;
    std::copy_if(issues.begin(), issues.end(), std::back_inserter(out),
                 [&](const ValidationIssue &i) { return i.severity == s; });
    return out;
}

QStringList ValidationReport::messages(IssueSeverity s) const
{
    QStringList out;
    for (const ValidationIssue &i : issues)
        if (i.severity == s) out << i.message;
    return out;
}

void add(ValidationReport &r, IssueSeverity sev, const QString &code, const QString &subject,
         const QString &message)
{
    r.issues.push_back(ValidationIssue{ sev, code, subject, message });
}

void err(ValidationReport &r, const QString &code, const QString &subject, const QString &message)
{
    add(r, IssueSeverity::Error, code, subject, message);
}

void warn(ValidationReport &r, const QString &code, const QString &subject, const QString &message)
{
    add(r, IssueSeverity::Warning, code, subject, message);
}

void appendUnreported(ValidationReport &into, const ValidationReport &revalidation)
{
    // A machine key, not a label — '\n' cannot occur in a code or an id, so nothing needs escaping.
    // Same construction as MergedNormProvider::key(), and for the same reason: two fields joined by
    // a character neither can contain is a pair, cheaply.
    const auto keyOf = [](const ValidationIssue &i) {
        return i.code + QLatin1Char('\n') + i.subject;
    };

    QSet<QString> said;
    said.reserve(int(into.issues.size()));
    for (const ValidationIssue &i : into.issues) said.insert(keyOf(i));

    // Insert as we go, so a re-validation that legitimately reports one subject twice under one
    // code (nothing does today, but the validators are not forbidden to) still says it once.
    for (const ValidationIssue &i : revalidation.issues) {
        const QString k = keyOf(i);
        if (said.contains(k)) continue;
        said.insert(k);
        into.issues.push_back(i);
    }
}

// ── Reading ─────────────────────────────────────────────────────────────────

QStringList readStringList(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &e : v.toArray()) out << e.toString();
    return out;
}

// ── Writing ─────────────────────────────────────────────────────────────────

QString diagnosticsDataDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/diagnostics");
}

QString diagnosticsFilePath(const QString &fileName)
{
    const QString dir = diagnosticsDataDir();
    return dir.isEmpty() ? QString() : dir + QLatin1Char('/') + fileName;
}

bool atomicWrite(const QString &path, const QByteArray &bytes, QString *whyNot)
{
    if (path.isEmpty()) {
        if (whyNot) *whyNot = QStringLiteral("No writable application data location.");
        return false;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());

    const QString tmpPath = path + QStringLiteral(".tmp");
    QFile         tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (whyNot) *whyNot = QStringLiteral("Could not write to %1.").arg(tmpPath);
        return false;
    }

    // Both halves are checked, and a failure takes the temporary away with it: leaving a short
    // `.tmp` beside the good file would be picked up by nothing and explain itself to nobody. The
    // ORIGINAL is still whole at this point, which is the whole reason to write somewhere else
    // first, and it stays that way.
    const qint64 written = tmp.write(bytes);
    tmp.close();   // flushes; a buffered write that fails only on flush surfaces in error() here
    if (written != bytes.size() || tmp.error() != QFile::NoError) {
        if (whyNot) *whyNot = QStringLiteral("Could not write to %1 — the disk may be full.").arg(tmpPath);
        QFile::remove(tmpPath);
        return false;
    }

    // remove-then-rename rather than a straight rename, because QFile::rename does not replace an
    // existing file on every platform this ships to. The window between the two is the one thing
    // this dance cannot close; it is microseconds wide and the content it would lose is still in
    // the temporary beside it.
    QFile::remove(path);
    if (!QFile::rename(tmpPath, path)) {
        if (whyNot) *whyNot = QStringLiteral("Could not replace %1.").arg(path);
        return false;
    }
    return true;
}

} // namespace pinpoint::analysis
