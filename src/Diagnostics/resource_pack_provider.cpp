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

#include <QFile>

namespace pinpoint::analysis {

namespace {

// The shipped core pack. The runtime copy is a Qt resource; the SOURCE OF TRUTH is the reviewable
// JSON committed at src/Diagnostics/packs/core.json, from which the resource is generated at build
// time. That split is what lets a community contribution arrive as a pull request against readable
// content rather than a binary blob.
class ResourcePackProvider final : public ICharacteristicPackProvider {
public:
    explicit ResourcePackProvider(const QString &resourcePath)
    {
        m_label = resourcePath;

        QFile f(resourcePath);
        if (!f.open(QIODevice::ReadOnly)) {
            m_report.issues.push_back(ValidationIssue{
                IssueSeverity::Error, QStringLiteral("parse"), resourcePath,
                QStringLiteral("Could not open the core pack at '%1'.").arg(resourcePath) });
            return;
        }

        PackLoadResult res = loadPack(f.readAll(), resourcePath);
        m_pack             = std::move(res.pack);
        m_report           = std::move(res.report);
        m_pack.readOnly    = true;   // the shipped pack is never edited in place
    }

    const CharacteristicPack &pack() const override { return m_pack; }
    const ValidationReport   &report() const override { return m_report; }
    QString                   label() const override { return m_label; }

private:
    CharacteristicPack m_pack;
    ValidationReport   m_report;
    QString            m_label;
};

} // namespace

std::unique_ptr<ICharacteristicPackProvider> makeResourcePackProvider(const QString &resourcePath)
{
    return std::make_unique<ResourcePackProvider>(resourcePath);
}

} // namespace pinpoint::analysis
