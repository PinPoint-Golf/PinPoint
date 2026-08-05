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

#include "pack_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

namespace pinpoint::analysis {

namespace {

// The file name the user's OWN pack is written under. Named here rather than spelled out at each
// use, because three separate decisions key on it: where saveUserPack() writes, which file
// characteristicPackFilesIn() layers first, and which file is LocalUser rather than Community.
const QLatin1String kUserPackName("user.json");

// ONE user pack file from disk. A pack that fails to parse is REPORTED and skipped, never fatal: a
// single malformed community pack must not take the whole library down with it, and — now that a
// directory of several packs becomes several of these — must not take its NEIGHBOURS down either.
// That containment is the reason this is one file per provider rather than one directory: a
// provider holds exactly one pack, so "which file was that fault in" has an answer, and a fault in
// one file cannot decide what another file contributes.
class FilePackProvider final : public ICharacteristicPackProvider {
public:
    FilePackProvider(const QString &packFile, PackOrigin origin)
        : m_path(packFile.isEmpty() ? userPackPath() : packFile), m_origin(origin)
    {
        m_label = m_path;
        if (m_path.isEmpty()) return;   // no writable app data location at all

        QFile f(m_path);
        if (!f.exists()) return;        // no user pack is the normal case, not a problem
        if (!f.open(QIODevice::ReadOnly)) {
            m_report.issues.push_back(ValidationIssue{
                IssueSeverity::Warning, QStringLiteral("parse"), m_path,
                QStringLiteral("Could not read user pack '%1'.")
                    .arg(QFileInfo(m_path).fileName()) });
            return;
        }

        PackLoadResult res = loadPack(f.readAll(), m_path);

        // Keep every issue for the health list, but drop only the REFERENTIAL ones: an overlay
        // pack's edges point at core conditions it does not contain, so standalone referential
        // integrity is meaningless here. The merged provider re-validates the assembled
        // library, and that is the authoritative check — see the note at its re-validation for why
        // this drop is what stops the two passes reporting the same finding twice.
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
        if (res.parsed) m_pack = std::move(res.pack);
    }

    const CharacteristicPack &pack() const override { return m_pack; }
    const ValidationReport   &report() const override { return m_report; }
    QString                   label() const override { return m_label; }
    PackOrigin                origin() const override { return m_origin; }

private:
    CharacteristicPack m_pack;
    ValidationReport   m_report;
    QString            m_path;
    QString            m_label;
    PackOrigin         m_origin = PackOrigin::LocalUser;
};

} // namespace

QStringList characteristicPackFilesIn(const QString &directory)
{
    const QString dirPath = directory.isEmpty() ? diagnosticsDataDir() : directory;
    if (dirPath.isEmpty()) return {};

    QDir dir(dirPath);
    if (!dir.exists()) return {};   // no user packs is the normal case, not a problem

    QStringList user, others;
    for (const QString &name : dir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name)) {
        // This directory is shared with every other user registry (norm sets, contexts, screens,
        // drills, references) — see the note in pack_io.h on why it is one directory rather than
        // several. Left unfiltered, `*.json` picks up their files too: each has no top-level `id`,
        // so it either injects a spurious "Pack has no id" issue into a provider's report, or,
        // worse, if it DOES have a non-empty `id` (unlikely today but not contractually ruled out),
        // QDir::Name sorts it ahead of `user.json` and it silently becomes a pack in the assembled
        // library. Skip every sibling registry by name.
        //
        // THE ONE PLACE this list lives. It used to sit inside the provider's constructor, which
        // was fine while a directory produced exactly one provider; with the enumeration split out
        // it would otherwise have had to be repeated by every caller that wanted the same answer,
        // and a skip list kept in two places is a skip list that will one day skip different files.
        if (name.endsWith(QLatin1String(".norms.json"))) continue;
        if (name == QLatin1String("screens.json") || name == QLatin1String("drills.json")
            || name == QLatin1String("references.json")
            || name == QLatin1String("contexts.json")) continue;

        (name == kUserPackName ? user : others) << dir.filePath(name);
    }

    // LAYERING ORDER: `user.json` first, then the rest in QDir::Name (alphabetical) order.
    //
    // The user's own pack goes first because it is the only layer that may REPLACE core content,
    // and putting it at the front means a later community pack is merged against a library that
    // already reflects the user's overrides rather than against a library the user has since
    // changed. Alphabetical for the rest is an arbitrary rule and is chosen for being one: it is
    // STABLE and it is VISIBLE, so two machines with the same directory assemble the same library,
    // and an author who needs one community pack to land after another can say so by renaming.
    // Directory order (whatever the filesystem hands back) would have neither property, and file
    // mtime would mean re-downloading a pack silently re-ordered the library.
    //
    // Order matters less here than it does for norms, because community packs are namespaced and
    // lose every collision with core anyway (see PackOrigin) — but it decides which of two
    // community packs is reported as the loser when they collide with each other, and that has to
    // be reproducible or the health list changes between runs.
    return user + others;
}

std::unique_ptr<ICharacteristicPackProvider> makeFilePackProvider(const QString &packFile,
                                                                  PackOrigin     origin)
{
    return std::make_unique<FilePackProvider>(packFile, origin);
}

std::vector<std::unique_ptr<ICharacteristicPackProvider>> makeFilePackProviders(
    const QString &directory)
{
    std::vector<std::unique_ptr<ICharacteristicPackProvider>> out;
    for (const QString &path : characteristicPackFilesIn(directory)) {
        // ORIGIN BY FILE NAME, which is the only evidence there is. `user.json` is the file the
        // editor writes (userPackPath()), so it is by construction the user's own library and gets
        // LocalUser — the origin that may override shipped characteristics, because refusing the
        // user's own edit would make the editor useless for every shipped row.
        //
        // Everything else in that directory got there by being imported from somebody else, so it
        // is Community: namespaced, and core wins on collision. That is the protection PackOrigin
        // was written for, and until this function existed nothing applied it — a single provider
        // covered the whole directory with one origin, so a downloaded pack that happened to sort
        // first was merged as though the user had authored it.
        out.push_back(makeFilePackProvider(
            path, QFileInfo(path).fileName() == kUserPackName ? PackOrigin::LocalUser
                                                              : PackOrigin::Community));
    }
    return out;
}

// Spelled through kUserPackName, so the file the editor WRITES and the file the enumeration treats
// as the user's own layer cannot drift apart — they were two independent string literals before,
// which is one rename away from a user pack that saves and never loads.
QString userPackPath() { return diagnosticsFilePath(kUserPackName); }

bool saveUserPack(const CharacteristicPack &pack, QString *whyNot)
{
    // Temporary-and-rename, so an interrupted save cannot truncate a library the user has spent
    // time building — see atomicWrite().
    return atomicWrite(userPackPath(),
                       QJsonDocument(savePack(pack)).toJson(QJsonDocument::Indented), whyNot);
}

} // namespace pinpoint::analysis
