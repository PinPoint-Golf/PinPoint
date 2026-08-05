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

#include "pack_provider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

namespace pinpoint::analysis {

namespace {

// One user pack from disk. A pack that fails to parse is REPORTED and skipped, never fatal: a
// single malformed community pack must not take the whole library down with it.
class FilePackProvider final : public ICharacteristicPackProvider {
public:
    FilePackProvider(const QString &directory, PackOrigin origin) : m_origin(origin)
    {
        m_dir = directory.isEmpty() ? defaultDirectory() : directory;
        m_label = m_dir;

        QDir dir(m_dir);
        if (!dir.exists()) return;   // no user packs is the normal case, not a problem

        const QStringList files = dir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
        for (const QString &name : files) {
            // This directory is shared with every other user registry (norm sets, contexts, screens,
            // drills, references) — see the note in file_norm_provider.cpp on why it is one directory rather
            // than several. Left unfiltered, `*.json` picks up their files too: each has no top-level
            // `id`, so it either injects a spurious "Pack has no id" issue into this report, or, worse,
            // if it DOES have a non-empty `id` (unlikely today but not contractually ruled out), QDir::Name
            // sorts it ahead of `user.json` and it silently BECOMES the user pack. Skip every sibling
            // registry by name so this provider only ever sees files that are actually packs.
            if (name.endsWith(QLatin1String(".norms.json"))) continue;
            if (name == QLatin1String("screens.json") || name == QLatin1String("drills.json")
                || name == QLatin1String("references.json")
                || name == QLatin1String("contexts.json")) continue;

            const QString path = dir.filePath(name);
            QFile         f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                m_report.issues.push_back(ValidationIssue{
                    IssueSeverity::Warning, QStringLiteral("parse"), path,
                    QStringLiteral("Could not read user pack '%1'.").arg(name) });
                continue;
            }

            PackLoadResult res = loadPack(f.readAll(), path);

            // Keep every issue for the health list, but drop only the REFERENTIAL ones: an overlay
            // pack's edges point at core conditions it does not contain, so standalone referential
            // integrity is meaningless here. The merged provider re-validates the assembled
            // library, and that is the authoritative check.
            for (ValidationIssue &i : res.report.issues) {
                const bool crossPack = (i.code == QLatin1String("unknownCondition")
                                        || i.code == QLatin1String("unknownSignal")
                                        || i.code == QLatin1String("unknownMeasure")
                                        || i.code == QLatin1String("noCause")
                                        || i.code == QLatin1String("orphanCause")
                                        || i.code == QLatin1String("observableNoSignal"));
                if (!crossPack) m_report.issues.push_back(std::move(i));
            }

            // `parsed`, not `loaded` — see PackLoadResult. Keying off `loaded` here would discard
            // every user pack that references a shipped characteristic, which is most of them.
            if (!res.parsed) continue;

            // First readable pack becomes this provider's pack; a directory holding several is
            // represented by several providers, which is what the merger expects.
            if (m_pack.id.isEmpty()) m_pack = std::move(res.pack);
        }
    }

    static QString defaultDirectory()
    {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        return base.isEmpty() ? QString() : base + QStringLiteral("/diagnostics");
    }

    const CharacteristicPack &pack() const override { return m_pack; }
    const ValidationReport   &report() const override { return m_report; }
    QString                   label() const override { return m_label; }
    PackOrigin                origin() const override { return m_origin; }

private:
    CharacteristicPack m_pack;
    ValidationReport   m_report;
    QString            m_dir;
    QString            m_label;
    PackOrigin         m_origin = PackOrigin::LocalUser;
};

} // namespace

std::unique_ptr<ICharacteristicPackProvider> makeFilePackProvider(const QString &directory,
                                                                  PackOrigin     origin)
{
    return std::make_unique<FilePackProvider>(directory, origin);
}

QString userPackPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) return QString();
    return base + QStringLiteral("/diagnostics/user.json");
}

bool saveUserPack(const CharacteristicPack &pack, QString *whyNot)
{
    const QString path = userPackPath();
    if (path.isEmpty()) {
        if (whyNot) *whyNot = QStringLiteral("No writable application data location.");
        return false;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());

    // Write to a temporary and rename, so an interrupted save cannot truncate a library the user
    // has spent time building.
    const QString tmpPath = path + QStringLiteral(".tmp");
    QFile         tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (whyNot) *whyNot = QStringLiteral("Could not write to %1.").arg(tmpPath);
        return false;
    }
    tmp.write(QJsonDocument(savePack(pack)).toJson(QJsonDocument::Indented));
    tmp.close();

    QFile::remove(path);
    if (!QFile::rename(tmpPath, path)) {
        if (whyNot) *whyNot = QStringLiteral("Could not replace %1.").arg(path);
        return false;
    }
    return true;
}

} // namespace pinpoint::analysis
