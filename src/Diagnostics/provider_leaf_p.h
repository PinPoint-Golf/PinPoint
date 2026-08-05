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

#pragma once

#include "norm_provider.h"
#include "pack_io.h"
#include "pack_provider.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <optional>
#include <utility>
#include <vector>

// What a LEAF provider does the same way on both sides of the seam.
//
// There are six of them — {resource, file, memory} × {characteristic pack, norm set} — and only two
// were written. The other four were cloned across, and a clone brings the shape while leaving the
// arguments behind: by the time this header was written the PINPOINT_CORE_* read existed twice with
// two spellings (one a named helper, one inline), the open-or-report dance four times with two
// severities and four message strings, "append this report to mine" five times, and the
// label/origin/report trio six times over six identical members.
//
// So: one copy of each, here, with the argument written once. This header is the counterpart of
// pack_io.h one level up — that one collapses what the LOADERS share, this one collapses what the
// PROVIDERS wrapped around them share.
//
// WHAT IS DELIBERATELY NOT COLLAPSED. The two interfaces are not unified and must not be.
// INormProvider's contract is genuinely richer — resolve(), the context tree, shippedNorm(),
// isOverridden(), the layers()/disabled-set census — and the two MERGED providers implement
// materially different collision semantics (namespaced-and-core-wins for packs, upsert-by-key with
// context specificity beating layer precedence for norms). What is shared here is only the
// mechanical skeleton of a leaf: where the bytes came from, what to say when they will not come,
// and the three answers (`report()`, `label()`, `origin()`) whose signatures are identical on both
// interfaces because they mean the same thing on both.
//
// This is `_p` — internal to src/Diagnostics, included by the six provider .cpp files and by
// nothing else. A caller outside gets the factory functions, never the skeleton.

namespace pinpoint::analysis::detail {

// ── The env-var seam ────────────────────────────────────────────────────────
//
// The Qt resource that holds the shipped content only exists inside the app binary, so a standalone
// test or an offline tool has no way to reach it. Every PINPOINT_CORE_* variable points the
// corresponding provider at the reviewable JSON in the repo instead, and every one of them is read
// HERE — the names live in the traits below, so the set of variables the build honours can be read
// off one page rather than grepped for. Unset in every normal run.
inline QString overridePath(const char *env, const QString &fallback)
{
    const QByteArray ov = qgetenv(env);
    return ov.isEmpty() ? fallback : QString::fromLocal8Bit(ov);
}

// ── Getting the bytes, and what it means when you cannot ────────────────────
//
// Two readers, because a missing file means opposite things at the two ends of the layer stack and
// that difference is the only interesting thing about either of them.

// SHIPPED content. It is built into the app, so its absence is an ERROR: something is wrong with the
// build or with the env override, and the library the user sees will be missing content nobody
// removed.
inline std::optional<QByteArray> readShipped(const QString &path, const QString &what,
                                             ValidationReport &report)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        report.issues.push_back(ValidationIssue{
            IssueSeverity::Error, QStringLiteral("parse"), path,
            QStringLiteral("Could not open the core %1 at '%2'.").arg(what, path) });
        return std::nullopt;
    }
    return f.readAll();
}

// A LAYER on disk. Not being there at all is the NORMAL case — most users author nothing — so it is
// silent. Being there and unreadable is a warning against the file's own name, so a health list can
// say which file to fix, and the layer contributes nothing rather than taking its neighbours down
// with it.
inline std::optional<QByteArray> readLayer(const QString &path, const QString &what,
                                           ValidationReport &report)
{
    QFile f(path);
    if (!f.exists()) return std::nullopt;
    if (!f.open(QIODevice::ReadOnly)) {
        report.issues.push_back(ValidationIssue{
            IssueSeverity::Warning, QStringLiteral("parse"), path,
            QStringLiteral("Could not read %1 '%2'.")
                .arg(what, QFileInfo(path).fileName()) });
        return std::nullopt;
    }
    return f.readAll();
}

inline void appendIssues(ValidationReport &into, const ValidationReport &from)
{
    for (const ValidationIssue &i : from.issues) into.issues.push_back(i);
}

// ── What differs between the two registries ─────────────────────────────────
//
// The traits ARE the statement of the difference, which is why both live on one page: side by side,
// a divergence is a line you can point at rather than a thing you would have to open two files to
// notice.

struct PackTraits {
    using Iface = ICharacteristicPackProvider;
    using Pack  = CharacteristicPack;

    static constexpr const char *kShippedEnv = "PINPOINT_CORE_PACK";

    // The nouns the two readers above put in their messages. Spelled once each, because the message
    // is what an author reads when their file will not load and it should name the thing they think
    // they installed.
    static QString shippedNoun() { return QStringLiteral("pack"); }
    static QString layerNoun() { return QStringLiteral("user pack"); }

    static PackLoadResult   load(const QByteArray &b, const QString &src) { return loadPack(b, src); }
    static ValidationReport validate(const Pack &p) { return validatePack(p); }

    // Keep every issue for the health list, but drop the REFERENTIAL ones: an overlay pack's edges
    // point at core conditions it does not contain, so standalone referential integrity is
    // meaningless for a layer. The merged provider re-validates the ASSEMBLED library and that is
    // the authoritative check — see the note at its re-validation for why this drop is what stops
    // the two passes reporting the same finding twice.
    static bool admitLayerIssue(const ValidationIssue &i)
    {
        return !(i.code == QLatin1String("unknownCondition")
                 || i.code == QLatin1String("unknownSignal")
                 || i.code == QLatin1String("unknownMeasure")
                 || i.code == QLatin1String("noCause")
                 || i.code == QLatin1String("orphanCause")
                 || i.code == QLatin1String("observableNoSignal"));
    }
};

struct NormTraits {
    using Iface = INormProvider;
    using Pack  = NormPack;

    static constexpr const char *kShippedEnv = "PINPOINT_CORE_NORMS";
    // The norm side has a SECOND shipped file the pack side has no counterpart for: the context tree
    // the corridors hang on. Named here beside the first so the two PINPOINT_CORE_* variables this
    // registry honours are read off one line; the load itself is hand-written at each norm leaf,
    // because the tree belongs to the DIRECTORY rather than to any one set and the two leaves
    // therefore want it on materially different terms. See the notes there.
    static constexpr const char *kContextsEnv = "PINPOINT_CORE_CONTEXTS";

    static QString shippedNoun() { return QStringLiteral("norm set"); }
    static QString layerNoun() { return QStringLiteral("user norm set"); }

    static NormPackLoadResult load(const QByteArray &b, const QString &src)
    {
        return loadNormPack(b, src);
    }
    static ValidationReport validate(const Pack &p) { return validateNormPack(p); }

    // Every issue survives. A norm layer's referential faults ARE reported twice today — once here
    // and once by the assembled re-validation — but appendUnreported() dedupes them on (code,
    // subject) at the merge, so there is nothing for a filter to do. The pack side needs its filter
    // because a community pack's ids are RENAMED on the way in and the two tellings would then have
    // different subjects; norm rows are keyed, never renamed.
    static bool admitLayerIssue(const ValidationIssue &) { return true; }
};

// ── The skeleton ────────────────────────────────────────────────────────────
//
// Storage for the three answers both interfaces ask for, and the three ways a leaf comes by its
// content. A leaf is then its constructor: pick one of loadShipped / loadLayer / adopt, and say
// what it means.
template <typename Traits>
class LeafProvider : public Traits::Iface {
public:
    const ValidationReport &report() const override { return m_report; }
    QString                 label() const override { return m_label; }
    PackOrigin              origin() const override { return m_origin; }

protected:
    // The shipped copy, through the env seam. Read-only and Core by construction — those are what
    // "shipped" means, so a leaf cannot get one and forget the other.
    void loadShipped(const QString &resourcePath)
    {
        const QString path = overridePath(Traits::kShippedEnv, resourcePath);
        m_label            = path;
        m_origin           = PackOrigin::Core;

        const std::optional<QByteArray> bytes = readShipped(path, Traits::shippedNoun(), m_report);
        if (!bytes) return;

        LoadResult<typename Traits::Pack> res = Traits::load(*bytes, path);
        m_pack                                = std::move(res.pack);
        m_pack.readOnly                       = true;   // never edited in place
        appendIssues(m_report, res.report);
    }

    // ONE layer file. The caller has already decided the path and the origin; what this adds is the
    // containment: a file that is absent, unreadable or malformed costs this layer and nothing else.
    void loadLayer(const QString &path)
    {
        m_label = path;
        if (path.isEmpty()) return;   // no writable app data location at all

        const std::optional<QByteArray> bytes = readLayer(path, Traits::layerNoun(), m_report);
        if (!bytes) return;

        LoadResult<typename Traits::Pack> res = Traits::load(*bytes, path);
        for (ValidationIssue &i : res.report.issues)
            if (Traits::admitLayerIssue(i)) m_report.issues.push_back(std::move(i));

        // `parsed`, not `loaded` — see LoadResult. Keying off `loaded` would discard every layer
        // that references shipped content, which is most of them, and the next save would then
        // write the empty result over the author's own file.
        if (res.parsed) m_pack = std::move(res.pack);
    }

    // Content that is already in memory — the editor's unsaved working copy. Validated on adoption
    // exactly as a loaded layer is: the report is advisory (an overlay routinely fails standalone
    // referential integrity and the caller reads the ASSEMBLED one), but a duplicate id, a
    // malformed reducer or a mismatched unit is a fault of this content alone and is worth naming
    // before the merge buries it.
    void adopt(typename Traits::Pack content, QString label, PackOrigin origin)
    {
        m_pack   = std::move(content);
        m_label  = std::move(label);
        m_origin = origin;
        m_report = Traits::validate(m_pack);
    }

    typename Traits::Pack m_pack;
    ValidationReport      m_report;
    QString               m_label;
    PackOrigin            m_origin = PackOrigin::LocalUser;
};

// The two shapes, which differ only in what the interface calls the payload — and, on the norm side,
// in there being a second one. A leaf derives from the shape rather than from LeafProvider directly,
// so no leaf spells an accessor.
class PackLeaf : public LeafProvider<PackTraits> {
public:
    const CharacteristicPack &pack() const override { return m_pack; }
};

class NormLeaf : public LeafProvider<NormTraits> {
public:
    const NormPack    &norms() const override { return m_pack; }
    const ContextTree &contexts() const override { return m_contexts; }

protected:
    // A norm set does not carry its own tree — resolution is meaningless without one, and the tree
    // belongs to the directory rather than to any set in it. Every norm leaf therefore has to be
    // handed one from somewhere, and where from is the one thing the three of them genuinely
    // disagree about.
    ContextTree m_contexts;
};

} // namespace pinpoint::analysis::detail
