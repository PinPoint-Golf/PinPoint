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

#include "norm_provider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

namespace pinpoint::analysis {

namespace {

// User norm sets live alongside user characteristic packs, distinguished by suffix: `*.norms.json`
// for norms, `*.json` otherwise. One directory, because a user thinks of their diagnostics content
// as one thing, and two directories would guarantee that a backup captured half of it.
const QLatin1String kNormSuffix(".norms.json");

// One user norm set from disk. A set that fails to parse is REPORTED and skipped, never fatal: a
// single malformed community set must not take the whole library down with it.
class FileNormProvider final : public INormProvider {
public:
    FileNormProvider(const QString &directory, PackOrigin origin) : m_origin(origin)
    {
        m_dir   = directory.isEmpty() ? defaultDirectory() : directory;
        m_label = m_dir;

        QDir dir(m_dir);
        if (!dir.exists()) return;   // no user norms is the normal case, not a problem

        const QStringList files =
            dir.entryList({ QStringLiteral("*.norms.json") }, QDir::Files, QDir::Name);
        for (const QString &name : files) {
            const QString path = dir.filePath(name);
            QFile         f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                m_report.issues.push_back(ValidationIssue{
                    IssueSeverity::Warning, QStringLiteral("parse"), path,
                    QStringLiteral("Could not read user norm set '%1'.").arg(name) });
                continue;
            }

            NormPackLoadResult res = loadNormPack(f.readAll(), path);
            for (const ValidationIssue &i : res.report.issues)
                m_report.issues.push_back(i);

            // `parsed`, not `loaded` — an overlay set is validated referentially only once the
            // library is assembled, so keying off `loaded` would discard sets that are perfectly
            // fine in context.
            if (!res.parsed) continue;

            // First readable set becomes this provider's set; a directory holding several is
            // represented by several providers, which is what the merger expects.
            if (m_norms.norms.empty() && m_norms.id.isEmpty()) m_norms = std::move(res.pack);
        }

        // A user set may add contexts of its own (a club the shipped tree does not name). Loaded
        // from the same directory under a fixed name so there is exactly one place to look.
        const QString ctxPath = dir.filePath(QStringLiteral("contexts.json"));
        if (QFile::exists(ctxPath)) {
            QFile cf(ctxPath);
            if (cf.open(QIODevice::ReadOnly)) {
                ContextTreeLoadResult res = loadContextTree(cf.readAll(), ctxPath);
                m_contexts = std::move(res.tree);
                for (const ValidationIssue &i : res.report.issues)
                    m_report.issues.push_back(i);
            }
        }
    }

    static QString defaultDirectory()
    {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        return base.isEmpty() ? QString() : base + QStringLiteral("/diagnostics");
    }

    const NormPack         &norms() const override { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override { return m_report; }
    QString                 label() const override { return m_label; }
    PackOrigin              origin() const override { return m_origin; }

private:
    NormPack         m_norms;
    ContextTree      m_contexts;
    ValidationReport m_report;
    QString          m_dir;
    QString          m_label;
    PackOrigin       m_origin = PackOrigin::LocalUser;
};

} // namespace

std::unique_ptr<INormProvider> makeFileNormProvider(const QString &directory, PackOrigin origin)
{
    return std::make_unique<FileNormProvider>(directory, origin);
}

QString userNormPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) return QString();
    return base + QStringLiteral("/diagnostics/user") + kNormSuffix;
}

bool saveUserNormPack(const NormPack &pack, QString *whyNot)
{
    const QString path = userNormPath();
    if (path.isEmpty()) {
        if (whyNot) *whyNot = QStringLiteral("No writable application data location.");
        return false;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());

    // Write to a temporary and rename, so an interrupted save cannot truncate a norm set the user
    // has spent time seating.
    const QString tmpPath = path + QStringLiteral(".tmp");
    QFile         tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (whyNot) *whyNot = QStringLiteral("Could not write to %1.").arg(tmpPath);
        return false;
    }
    tmp.write(QJsonDocument(saveNormPack(pack)).toJson(QJsonDocument::Indented));
    tmp.close();

    QFile::remove(path);
    if (!QFile::rename(tmpPath, path)) {
        if (whyNot) *whyNot = QStringLiteral("Could not replace %1.").arg(path);
        return false;
    }
    return true;
}

} // namespace pinpoint::analysis
