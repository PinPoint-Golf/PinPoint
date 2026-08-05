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

#include "context_tree.h"
#include "norm_pack.h"
#include "pack_provider.h"   // PackOrigin

#include <memory>
#include <vector>

// The norm provider seam. Assembled ON DEMAND (makeNormProvider), mirroring
// makeCharacteristicPackProvider(), makeMetricCatalogue() and makeReferenceBandProvider() — no
// self-registering statics, no startup registration, nothing that runs before main().

namespace pinpoint::analysis {

// What a lookup found, and where it found it. `contextId` is the context the norm was ACTUALLY
// authored at, which is not necessarily the one asked for — that is the whole point of the tree,
// and the UI has to be able to say "inherited from full swing" rather than implying the driver was
// specifically considered.
struct NormResolution {
    const Norm *norm      = nullptr;
    QString     contextId;              // where the norm was found; empty when nothing resolved
    bool        inherited = false;      // the norm came from an ancestor, not the requested context

    // A non-core layer supplies the row that resolved. This is the "you changed this" fact, and it
    // has to be tracked rather than inferred: a user row that happens to hold the same numbers as
    // the shipped one is still a user row, and comparing values would call it shipped.
    bool        overridden = false;

    bool  found() const { return norm != nullptr; }
    Grade grade(double value, const GradePolicy &policy = {}) const
    {
        return norm ? pinpoint::analysis::grade(value, *norm, policy) : Grade::NotMeasured;
    }

    // WHICH COHORT ANSWERED — the third half of the same provenance `contextId` and `overridden`
    // carry, and a surface must be able to say it: a golfer whose grades improve after entering
    // their date of birth has to read that as the corridor becoming right for them, not as the app
    // going soft on them.
    //
    // An ACCESSOR and not a field, deliberately. The answering row already states its own cohort, so
    // a copy here would be a second source of truth for one fact — and a value type that carries the
    // same answer twice is a value type where the two can disagree. `contextId` is a field because
    // it is genuinely not on the norm: a row does not know it was reached from a descendant.
    //
    // Unqualified when nothing resolved, which is the same answer as "this corridor applies to
    // everyone" and is correct in both readings: nothing was segmented.
    Cohort cohort() const { return norm ? norm->cohort : Cohort{}; }
};

// One layer in an assembled norm set, for the census the UI shows and — from the corridor editor
// onward — for deciding which set a write lands in. A provider that assembles others reports its
// CHILDREN here, not itself: "merged" is an implementation word, and a user reading the norm-set
// list needs to see the shipped set and their own set as separate things, because that is what the
// override relationship between them means.
struct NormSetInfo {
    QString    id;
    QString    label;
    PackOrigin origin    = PackOrigin::Core;
    int        normCount = 0;
    bool       readOnly  = true;
};

class INormProvider {
public:
    virtual ~INormProvider() = default;

    // The assembled norm set and context tree. A provider that fails to load reports it through
    // `report()` and returns whatever it could read — the UI shows a broken norm set rather than an
    // empty one with no explanation.
    virtual const NormPack    &norms() const = 0;
    virtual const ContextTree &contexts() const = 0;

    // Load-time and structural findings. Warnings here ARE part of the health list.
    virtual const ValidationReport &report() const = 0;

    virtual QString    label() const = 0;
    virtual PackOrigin origin() const = 0;

    // Resolve a norm for this measure in this context, walking UP the tree: driver, then
    // full_swing, then any, then nothing. Non-virtual because every provider must resolve
    // identically — a layer that resolved by its own rules would make grades depend on where a norm
    // happened to be stored.
    //
    // An empty or unknown contextId falls back to the default context (full_swing) and the caller
    // is responsible for marking the finding inferred; see the engine's confidence demotion. That
    // fallback is deliberate and narrow: it applies to the SHOT not declaring a context, never to a
    // context the tree does not recognise, which resolves to nothing.
    //
    // `athlete` is the cohort of the GOLFER — their sex and the age band their date of birth put
    // them in on the day of the swing. The walk is CONTEXT-MAJOR: at each node of the context chain
    // the whole cohort probe order is tried, most specific first, and only when none of them is
    // present does it move up the tree. See cohortProbeOrder() for the order and why it is fixed.
    //
    // Defaults to unqualified, which is the honest answer until the athlete record carries the two
    // fields — and which yields a single probe, so an athlete we know nothing about resolves exactly
    // as everyone did before cohorts existed.
    NormResolution resolve(const QString &measureId, const QString &contextId,
                           const Cohort &athlete = {}) const;

    // Every context beneath (and including) `contextId` where this measure has its OWN row. Drives
    // the "overridden" markers in the norms-by-context list.
    QStringList overriddenContextsFor(const QString &measureId) const;

    // ── Shipped vs yours ────────────────────────────────────────────────────
    //
    // Two questions the UI cannot answer by comparing numbers, and both are needed to offer a
    // reset honestly:
    //
    //   * "Reset to shipped" and "Remove your override" are the SAME operation (drop the user row)
    //     with different outcomes — the shipped corridor resolves again, or the parent's does.
    //     Which one a button should promise depends on whether core carries a row at this exact
    //     key, and offering the wrong promise is how a destructive action gets a reassuring label.
    //   * A user row holding the same numbers as the shipped one is still a user row. Deriving
    //     "edited" from a value comparison would silently un-mark it.

    // The CORE row at this exact (measureId, contextId, cohort), ignoring every user layer. Null
    // when core carries nothing there — which is exactly when a reset means "inherit from the
    // parent" rather than "go back to what shipped". No tree walk and no cohort probe: this is about
    // one key, not about resolution. A user row qualified to one cohort overrides the SHIPPED row
    // for that cohort, never the unqualified one beside it.
    virtual const Norm *shippedNorm(const QString &measureId, const QString &contextId,
                                    const Cohort &cohort = {}) const;

    // True when a NON-CORE layer supplies the row at this exact key. False for a leaf core
    // provider, true for a leaf user provider, and tracked per key by the merged provider.
    virtual bool isOverridden(const QString &measureId, const QString &contextId,
                              const Cohort &cohort = {}) const;

    // The layers behind this provider, shipped first. The default reports THIS provider as its own
    // single layer, which is right for every leaf; only the merged provider overrides it.
    virtual std::vector<NormSetInfo> layers() const;
};

// ── User norm set on disk ───────────────────────────────────────────────────
// Exposed so the corridor editor writes to the same file the provider reads, rather than each
// guessing the path.
QString userNormPath();
bool    saveUserNormPack(const NormPack &pack, QString *whyNot = nullptr);

// ── Concrete providers ──────────────────────────────────────────────────────

// The shipped core norm set and context tree, read from Qt resources. Read-only.
//
// PINPOINT_CORE_NORMS / PINPOINT_CORE_CONTEXTS override the paths for standalone tests and offline
// tooling, exactly as PINPOINT_CORE_PACK does for the characteristic pack: the Qt resource exists
// only inside the app binary, so without this seam no test outside the app could reach the shipped
// content.
std::unique_ptr<INormProvider> makeResourceNormProvider(
    const QString &normsPath    = QStringLiteral(":/diagnostics/norms.json"),
    const QString &contextsPath = QStringLiteral(":/diagnostics/contexts.json"));

// Every norm-set file in a directory (QStandardPaths::AppDataLocation/diagnostics by default), in
// LAYERING ORDER: `user.norms.json` first if present, then the rest alphabetically. The `*.norms
// .json` suffix is the whole filter — unlike the pack side, nothing else in the shared directory
// can match it. Exposed for the same reason characteristicPackFilesIn() is: the ordering rule has
// more than one caller and must not be copied.
QStringList normSetFilesIn(const QString &directory = QString());

// ONE norm-set file (userNormPath() by default), plus optionally the directory's shared
// `contexts.json`. A missing file is not an error — no user norm set is the normal case; an
// unreadable or malformed one is reported through report() and yields an empty set, so a broken
// community set costs the library that set and nothing else.
//
// `contextsFile` is handed in rather than derived, because the context tree belongs to the
// DIRECTORY and not to any one norm set: several sets in one directory share one tree, and exactly
// one provider should carry it. See makeFileNormProviders().
std::unique_ptr<INormProvider> makeFileNormProvider(
    const QString &normsFile = QString(), PackOrigin origin = PackOrigin::LocalUser,
    const QString &contextsFile = QString());

// One provider per norm-set file in the directory, in normSetFilesIn() order, with the origin each
// file's name implies: `user.norms.json` is LocalUser (the corridor editor writes it), everything
// else is Community. The shared `contexts.json` goes to the first provider only — the merger unions
// trees, so a second copy would change nothing and report its faults twice. A directory holding a
// context tree and no norm sets still yields one (norm-less) provider, so a club the user named
// does not vanish with their last corridor.
//
// One provider PER FILE, which is what the merger has always expected and what nothing built until
// now: a single directory-wide provider can only hold one set, so every set after the first was
// parsed, had its faults reported, and then had its corridors silently dropped.
std::vector<std::unique_ptr<INormProvider>> makeFileNormProviders(
    const QString &directory = QString());

// Core + user, in that order.
//
// COLLISION POLICY, and the reason it is not the obvious one: rows are merged into a single
// assembled set keyed by (measureId, contextId), with a LocalUser row replacing a core row at the
// SAME key. Resolution then walks the context chain over the assembled set — so context
// specificity beats layer precedence.
//
// The alternative (walk the whole chain in the user layer first, then in core) would mean a user
// adjusting the general full-swing norm silently overrode every club-specific shipped norm beneath
// it. They said something about full swings; they said nothing about drivers, and core's driver row
// is the more specific statement. To change the driver they override the driver.
//
// `disabled` names layers (by NormSetInfo::id) to leave OUT of the assembly. A disabled layer is
// not merged and does not appear in layers() — it is absent, not present-and-ignored, because
// "your set is off" has to mean the shipped corridor is what grades, not a shipped corridor with
// an invisible override still sitting on it. The CORE layer can be disabled like any other; a
// merged provider with nothing left resolves nothing, which reports honestly rather than
// pretending.
std::unique_ptr<INormProvider> makeMergedNormProvider(
    std::unique_ptr<INormProvider>              core,
    std::vector<std::unique_ptr<INormProvider>> user,
    const QStringList                          &disabled = {});

// The default assembly: shipped core plus whatever the user has authored.
std::unique_ptr<INormProvider> makeNormProvider();

// A norm set that is already in memory, presented through the provider seam so it can be merged
// with core exactly as a file-backed one is. The context tree comes from `contexts`, because a norm
// set does not carry one and resolution is meaningless without it.
//
// Same reason as makeMemoryPackProvider(): an editor holds a WORKING COPY of the user norm set that
// has not been saved yet, and every surface showing that edit — the corridor list, the measure's
// blast radius, the health strip — has to read an assembly containing it. Without this seam the
// only assembly available is the one on disk, so an unsaved corridor is invisible until it is
// written, which forces a per-corridor save and makes one undo history across both registries
// impossible.
std::unique_ptr<INormProvider> makeMemoryNormProvider(
    const NormPack &pack, const ContextTree &contexts, const QString &label = QString(),
    PackOrigin origin = PackOrigin::LocalUser);

// The process-wide assembled norm set, built on FIRST USE and cached.
//
// This exists because norms are read from files while the compiled table they replaced was free,
// and callers build a band provider per lookup rather than holding one — re-reading two JSON files
// on every corridor lookup would be a real regression. Caching keeps the old cost profile.
//
// It is a function-local static, so nothing runs before main() and the "no self-registering
// statics" rule the pack providers follow is not broken — this is lazy initialisation, not startup
// registration.
//
// Call resetSharedNormProvider() after writing a user norm set, or edits will not be visible until
// the next launch. Not thread-safe against concurrent readers: call it from the UI thread while no
// analysis is running, which is the only place a norm is ever edited.
std::shared_ptr<const INormProvider> sharedNormProvider();
void                                 resetSharedNormProvider();

} // namespace pinpoint::analysis
