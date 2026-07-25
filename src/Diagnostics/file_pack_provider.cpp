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
#include <QStandardPaths>

namespace pinpoint::analysis {

namespace {

// One user pack from disk. A pack that fails to parse is REPORTED and skipped, never fatal: a
// single malformed community pack must not take the whole library down with it.
class FilePackProvider final : public ICharacteristicPackProvider {
public:
    explicit FilePackProvider(const QString &directory)
    {
        m_dir = directory.isEmpty() ? defaultDirectory() : directory;
        m_label = m_dir;

        QDir dir(m_dir);
        if (!dir.exists()) return;   // no user packs is the normal case, not a problem

        const QStringList files = dir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
        for (const QString &name : files) {
            const QString path = dir.filePath(name);
            QFile         f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                m_report.issues.push_back(ValidationIssue{
                    IssueSeverity::Warning, QStringLiteral("parse"), path,
                    QStringLiteral("Could not read user pack '%1'.").arg(name) });
                continue;
            }

            PackLoadResult res = loadPack(f.readAll(), path);
            for (ValidationIssue &i : res.report.issues) m_report.issues.push_back(std::move(i));
            if (!res.loaded) continue;

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

private:
    CharacteristicPack m_pack;
    ValidationReport   m_report;
    QString            m_dir;
    QString            m_label;
};

} // namespace

std::unique_ptr<ICharacteristicPackProvider> makeFilePackProvider(const QString &directory)
{
    return std::make_unique<FilePackProvider>(directory);
}

} // namespace pinpoint::analysis
