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

#include "pack_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

namespace pinpoint::analysis {

namespace {

// User norm sets live alongside user characteristic packs, distinguished by suffix: `*.norms.json`
// for norms, `*.json` otherwise. One directory, because a user thinks of their diagnostics content
// as one thing, and two directories would guarantee that a backup captured half of it.
const QLatin1String kNormSuffix(".norms.json");

// The file name the user's OWN norm set is written under. Three decisions key on it — where
// saveUserNormPack() writes, which file normSetFilesIn() layers first, and which file is LocalUser
// rather than Community — so it is named once rather than spelled out at each.
QString userNormFileName() { return QStringLiteral("user") + kNormSuffix; }

// The context tree the whole directory shares, under a fixed name so there is exactly one place to
// look. Not a norm set and never enumerated as one: a set describes corridors, this describes the
// shape of the tree they hang on.
const QLatin1String kContextsName("contexts.json");

// ONE user norm set from disk, plus optionally the directory's shared context tree. A set that
// fails to parse is REPORTED and skipped, never fatal: a single malformed community set must not
// take the whole library down with it, and — now that a directory of several sets becomes several
// of these — must not take its NEIGHBOURS down either.
class FileNormProvider final : public INormProvider {
public:
    FileNormProvider(const QString &normsFile, PackOrigin origin, const QString &contextsFile)
        : m_path(normsFile), m_origin(origin)
    {
        m_label = m_path.isEmpty() ? contextsFile : m_path;

        if (!m_path.isEmpty()) {
            QFile f(m_path);
            if (!f.exists()) {
                // No user norm set is the normal case, not a problem.
            } else if (!f.open(QIODevice::ReadOnly)) {
                m_report.issues.push_back(ValidationIssue{
                    IssueSeverity::Warning, QStringLiteral("parse"), m_path,
                    QStringLiteral("Could not read user norm set '%1'.")
                        .arg(QFileInfo(m_path).fileName()) });
            } else {
                NormPackLoadResult res = loadNormPack(f.readAll(), m_path);
                for (const ValidationIssue &i : res.report.issues)
                    m_report.issues.push_back(i);

                // `parsed`, not `loaded` — an overlay set is validated referentially only once the
                // library is assembled, so keying off `loaded` would discard sets that are
                // perfectly fine in context.
                if (res.parsed) m_norms = std::move(res.pack);
            }
        }

        // A user set may add contexts of its own (a club the shipped tree does not name). Handed in
        // rather than looked up, because the tree belongs to the DIRECTORY and not to any one norm
        // set: see makeFileNormProviders() for why exactly one provider in a directory carries it.
        if (!contextsFile.isEmpty() && QFile::exists(contextsFile)) {
            QFile cf(contextsFile);
            if (cf.open(QIODevice::ReadOnly)) {
                ContextTreeLoadResult res = loadContextTree(cf.readAll(), contextsFile);
                m_contexts = std::move(res.tree);
                for (const ValidationIssue &i : res.report.issues)
                    m_report.issues.push_back(i);
            }
        }
    }

    const NormPack         &norms() const override { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override { return m_report; }
    QString                 label() const override { return m_label; }
    PackOrigin              origin() const override { return m_origin; }

    // A provider that read NOTHING is not a layer. No user norms is the normal case, and reporting
    // the empty directory as a norm set would put a second row in the census that a user cannot
    // act on and did not create — indistinguishable from a real but empty set they authored.
    std::vector<NormSetInfo> layers() const override
    {
        if (m_norms.id.isEmpty() && m_norms.norms.empty())
            return {};
        return INormProvider::layers();
    }

private:
    NormPack         m_norms;
    ContextTree      m_contexts;
    ValidationReport m_report;
    QString          m_path;
    QString          m_label;
    PackOrigin       m_origin = PackOrigin::LocalUser;
};

} // namespace

QStringList normSetFilesIn(const QString &directory)
{
    const QString dirPath = directory.isEmpty() ? diagnosticsDataDir() : directory;
    if (dirPath.isEmpty()) return {};

    QDir dir(dirPath);
    if (!dir.exists()) return {};   // no user norms is the normal case, not a problem

    // The suffix IS the filter here, which is why this needs none of the sibling-registry skip list
    // characteristicPackFilesIn() carries: `*.norms.json` cannot match `screens.json` or a
    // characteristic pack, so the shared directory costs the norm side nothing.
    QStringList user, others;
    const QString userName = userNormFileName();
    for (const QString &name :
         dir.entryList({ QStringLiteral("*") + kNormSuffix }, QDir::Files, QDir::Name))
        (name == userName ? user : others) << dir.filePath(name);

    // LAYERING ORDER: the user's own set first, then the rest alphabetically — the same rule and
    // the same argument as characteristicPackFilesIn(). Order carries more weight here than it does
    // for packs: norm rows collide on (measure, context, cohort), the assembled set is what
    // resolution walks, and a later LocalUser layer upserts over an earlier one. Alphabetical is
    // chosen for being stable and visible, so two machines with the same directory grade a swing
    // identically and an author can re-order by renaming.
    return user + others;
}

std::unique_ptr<INormProvider> makeFileNormProvider(const QString &normsFile, PackOrigin origin,
                                                    const QString &contextsFile)
{
    return std::make_unique<FileNormProvider>(normsFile.isEmpty() ? userNormPath() : normsFile,
                                              origin, contextsFile);
}

std::vector<std::unique_ptr<INormProvider>> makeFileNormProviders(const QString &directory)
{
    const QString dirPath = directory.isEmpty() ? diagnosticsDataDir() : directory;
    const QString ctxPath =
        dirPath.isEmpty() ? QString() : QDir(dirPath).filePath(kContextsName);
    const bool haveCtx = !ctxPath.isEmpty() && QFile::exists(ctxPath);

    std::vector<std::unique_ptr<INormProvider>> out;
    const QStringList files = normSetFilesIn(dirPath);

    for (const QString &path : files) {
        // ORIGIN BY FILE NAME, the only evidence there is, and exactly the rule
        // makeFilePackProviders() applies: `user.norms.json` is what the corridor editor writes
        // (userNormPath()), so it is the user's own set and may override shipped rows. Anything
        // else in that directory arrived from somebody else, so it is Community — core wins on
        // collision and the loser is reported, which is the protection PackOrigin exists for and
        // which nothing applied while one provider covered the whole directory.
        //
        // The shared context tree goes to the FIRST provider only. It belongs to the directory, not
        // to any one set, and the merger unions trees by id — so loading it N times would change
        // nothing about the assembled tree while reporting each of its faults N times, which is the
        // duplication the merged providers were just fixed to stop producing.
        const bool isUser = QFileInfo(path).fileName() == userNormFileName();
        out.push_back(makeFileNormProvider(
            path, isUser ? PackOrigin::LocalUser : PackOrigin::Community,
            (haveCtx && out.empty()) ? ctxPath : QString()));
    }

    // A directory can hold a context tree and no norm sets at all — a user who has named their own
    // club but not yet authored a corridor for it. Their tree still has to reach the assembly, or
    // the context they added would silently stop existing the moment they deleted their last norm.
    // The provider is empty of norms, and FileNormProvider::layers() reports no layer for it, so it
    // adds nothing to the census the user could not act on.
    //
    // Constructed directly rather than through makeFileNormProvider(), which substitutes
    // userNormPath() for an empty path: that substitution is right for the single-file entry point
    // and wrong here, because it would reach OUT of the directory being enumerated and pull in the
    // real AppData set — in a test over a temporary directory, the developer's own corridors.
    if (out.empty() && haveCtx)
        out.push_back(std::make_unique<FileNormProvider>(QString(), PackOrigin::LocalUser, ctxPath));

    return out;
}

QString userNormPath()
{
    return diagnosticsFilePath(userNormFileName());
}

bool saveUserNormPack(const NormPack &pack, QString *whyNot)
{
    // Temporary-and-rename, so an interrupted save cannot truncate a norm set the user has spent
    // time seating — see atomicWrite().
    return atomicWrite(userNormPath(),
                       QJsonDocument(saveNormPack(pack)).toJson(QJsonDocument::Indented), whyNot);
}

} // namespace pinpoint::analysis
