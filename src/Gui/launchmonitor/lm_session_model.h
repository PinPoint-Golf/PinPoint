/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

// Included rather than forward-declared: the QML type registration takes the address of
// a ShotListModel* property, and a pointer meta-type has to point at a complete type.
#include "shot_list_model.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QHash>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <vector>

// Forward-declared rather than included: buildGraphics() only takes references, and
// pulling lm_session_reductions.h in here would drag the whole launch monitor field
// catalogue into every translation unit that touches the model.
//
// The two registries are forward-declared for the same reason and held behind smart
// pointers, which is why this class now declares a destructor it does not need: a
// unique_ptr to an incomplete type has to be destroyed where the type is complete. Same
// shape as MetricCatalog, the other QML façade that resolves a corridor.
namespace pinpoint::analysis {
struct LmFieldStats;
class ICharacteristicPackProvider;
class INormProvider;
using LmShotValues = QHash<QString, double>;
}

// The launch monitor session board's model (PpLaunchMonitorPanel).
//
// Every number is lmSessionStats()' — this class only decides WHICH SHOTS it is asked
// about, and turns the answer into rows. Instantiated by the panel rather than living
// in main.cpp as a context property, because the panel is off by default: a golfer with
// no monitor should not be paying for a model that recomputes on every shot.
//
// ONE ROW PER BAND, not per field, which is where this departs from the brief's shape.
// The board is banded — a gutter rule, a name, a count, then that band's tiles — and a
// flat field-per-row model cannot express that without either a proxy per band or
// DelegateModel groups populated from JS. A band row carrying its own tiles is the same
// data in the shape the thing being drawn actually has, and it keeps ONE projection of
// the statistics rather than two that can disagree.
//
// WHERE THE NUMBERS COME FROM, and why not the obvious place. ShotListModel's
// MetricsRole looks like the right input — it is a map keyed by exactly these metric
// keys — but its values are DISPLAY STRINGS, and SwingDocReader rounds every degree
// metric to a whole degree on the way in. Fifteen of the twenty-five reading fields are
// in degrees, so a standard deviation computed from that map would not be a suppressed
// statistic, it would be a wrong one printed confidently. AnalysisDetailRole's `series`
// carries the raw doubles the writer put there, and it is populated by the same code
// path for a live shot (updateLaunchMonitor → refreshShot) and a reloaded one
// (readSwingJson), so there is one source here and not two.
//
// AND THE ONE JUDGEMENT IT DOES MAKE: whether the focused shot's reading sits outside its
// corridor. Every tile and every annotation carries a `grade` — "ideal" | "good" | "watch"
// | "action", or empty — resolved through the norm set at the scope's own club context.
// The panel paints only the last two, so a reading inside its corridor still looks exactly
// as it did: no green, no reassurance, and no colour on a metric nobody has authored a
// norm for. That is the point of resolving it here rather than compiling a table — 7 of
// the 25 reading measures carry a norm today, the rest say nothing rather than guessing,
// and authoring one in the model browser lights its tile with no change to this file.
//
// IT DOES NOT LIVE IN lm_session_reductions.h AND MUST NOT MOVE THERE. That header is a
// READOUT — the golfer against their own other shots, no norm and no grade — and it stays
// one. The join to the norm set is a projection for a surface to paint, which is this
// class's job, and it is why the dispersion strip and the corridor cannot be confused:
// the strip is geometry the reductions computed, the corridor is colour resolved here.
class LmSessionModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    // The session's shots. Any ShotListModel — the panel passes whichever list the
    // screen it sits on is showing, so a review screen boards its own session.
    Q_PROPERTY(ShotListModel *shotModel READ shotModel WRITE setShotModel NOTIFY shotModelChanged)
    // The swing the rest of the stage is showing (SessionMode.focusedShotId). -1 for
    // "nothing focused", which falls back to the newest shot in scope.
    Q_PROPERTY(int focusedShotId READ focusedShotId WRITE setFocusedShotId NOTIFY focusedShotIdChanged)

    // §5's gating facts, passed in rather than read here: the panel already has
    // `launchMonitor` and `appSettings` in scope, and a model that reached for the
    // singletons itself would be untestable and would couple the board to main.cpp.
    Q_PROPERTY(bool connected READ connected WRITE setConnected NOTIFY stateTextChanged)
    Q_PROPERTY(bool saving READ saving WRITE setSaving NOTIFY stateTextChanged)
    // Whether the shots being boarded are a LOADED session's rather than the live one's.
    // It changes no reading and no statistic — only which empty line is honest, and
    // whether the live device's name may head the scope. `connected` and `saving` are
    // facts about capturing the NEXT shot; neither can unwrite readings already on disk,
    // so neither may hide them.
    Q_PROPERTY(bool reviewing READ reviewing WRITE setReviewing NOTIFY stateTextChanged)
    // "GC Quad" — the short device name for the header. Empty is fine; the scope line
    // then starts with the club.
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY headerChanged)
    // The pack-wide grade policy, by NAME ("standard" | "strict" | "lenient"), passed in
    // for the same reason as the gates above: the panel has `appSettings` in scope. It is
    // the same property MetricCatalog exposes and it must be bound to the same setting —
    // a corridor graded against the default while the golfer has chosen Strict would put a
    // different colour on this panel than on the dashboard for one reading.
    Q_PROPERTY(QString gradePolicy READ gradePolicy WRITE setGradePolicy NOTIFY gradePolicyChanged)

    // The athlete's handedness, passed in for the same reason as `connected` and
    // `saving`: the panel already has athleteController in scope and a model that
    // reached for the singleton itself would be untestable. It changes only the two
    // INFERRED reads' wording and the strike diagram's mirroring — never a reading.
    Q_PROPERTY(bool leftHanded READ leftHanded WRITE setLeftHanded NOTIFY graphicsChanged)

    // "GC Quad · 7 iron · 18 shots". States the scope in words, because a mean whose
    // scope you cannot see is a number you cannot use.
    Q_PROPERTY(QString scopeText READ scopeText NOTIFY headerChanged)
    // "latest" when the focused shot IS the newest in scope, "this shot" when it is
    // not. The header must never claim something the number isn't.
    Q_PROPERTY(QString valueLabel READ valueLabel NOTIFY headerChanged)
    // The one line the panel shows INSTEAD of the board, or empty when the board
    // renders. Empty is the only "we are fine" signal — see §5.
    Q_PROPERTY(QString emptyText READ emptyText NOTIFY stateTextChanged)
    // Shots in scope (the club's, or the session's when the club is unknown).
    Q_PROPERTY(int shotCount READ shotCount NOTIFY headerChanged)
    // WHICH CORRIDORS THESE ARE, in words — "7 iron", "Driver" — or empty when the scope
    // graded nothing. Empty is the honest answer in two different situations that look the
    // same on screen (no reading has a norm; the club is unknown so the scope is the whole
    // session), and in both of them the panel must not print a legend for colour it is not
    // showing. It names the context ASKED FOR rather than the one a norm was found at: the
    // tree's whole point is that a wedge row can answer a lob wedge, and a legend reading
    // "Wedge" over a board scoped to a lob wedge would misdescribe the scope, not the norm.
    Q_PROPERTY(QString corridorScope READ corridorScope NOTIFY headerChanged)
    // Tiles per band, in row order. The panel sizes itself to fit the whole board on
    // screen, and it cannot do that arithmetic without knowing the shape up front —
    // a Repeater only reveals a band's tile count once the band is already laid out.
    Q_PROPERTY(QVariantList bandCounts READ bandCounts NOTIFY headerChanged)

    // EVERYTHING GRAPHICS MODE DRAWS, in one map. See graphics() in the .cpp for the
    // shape and for why it is one property rather than a second model: the two modes
    // are two projections of ONE set of statistics, computed once in rebuild(), and a
    // separate model would be a second chance for the board and the schematics to
    // disagree about the same shot.
    Q_PROPERTY(QVariantMap graphics READ graphics NOTIFY graphicsChanged)

public:
    enum Roles {
        BandRole = Qt::UserRole + 1,   // "Club"
        CountRole,                     // tiles in this band
        TilesRole,                     // QVariantList of tile maps, in fieldDefs() order
    };

    explicit LmSessionModel(QObject *parent = nullptr);
    ~LmSessionModel() override;

    int                    rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant               data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    ShotListModel *shotModel() const { return m_shots; }
    void           setShotModel(ShotListModel *m);
    int            focusedShotId() const { return m_focusedShotId; }
    void           setFocusedShotId(int id);
    bool           connected() const { return m_connected; }
    void           setConnected(bool on);
    bool           saving() const { return m_saving; }
    void           setSaving(bool on);
    bool           reviewing() const { return m_reviewing; }
    void           setReviewing(bool on);
    QString        deviceName() const { return m_deviceName; }
    void           setDeviceName(const QString &name);
    bool           leftHanded() const { return m_leftHanded; }
    void           setLeftHanded(bool on);
    QString        gradePolicy() const { return m_policyName; }
    void           setGradePolicy(const QString &name);

    QString scopeText()  const { return m_scopeText; }
    QString valueLabel() const { return m_valueLabel; }
    QString      emptyText()  const;
    int          shotCount()  const { return m_shotCount; }
    QString      corridorScope() const { return m_corridorScope; }
    QVariantList bandCounts() const;
    QVariantMap  graphics()   const { return m_graphics; }

signals:
    void shotModelChanged();
    void focusedShotIdChanged();
    void headerChanged();
    void stateTextChanged();
    void graphicsChanged();
    void gradePolicyChanged();

private:
    // Recompute everything. 25 fields across tens of shots is arithmetic no profiler
    // will ever find, so there are no running sums to keep correct across an edit —
    // a club change or a re-read simply rebuilds.
    void rebuild();
    void connectShotModel();
    // The graphics-mode projection of the SAME statistics rebuild() banded for the
    // tiles. Takes them as an argument rather than recomputing, which is what keeps the
    // two modes incapable of disagreeing about one shot.
    //
    // `opticalLowPoint` is the FOCUSED shot's camera-estimated low point, or empty. It
    // arrives separately from the readings because it is not one — see opticalLowPointFor()
    // in the .cpp for why an estimate must never enter the `lm.` map.
    //
    // `grades` is gradesFor()'s answer, HANDED IN for the same reason: the schematics and
    // the tiles must colour one reading the same way, and two calls to the resolver is two
    // chances for them not to.
    void buildGraphics(const std::vector<pinpoint::analysis::LmFieldStats> &stats,
                       const std::vector<pinpoint::analysis::LmShotValues> &scoped,
                       const QHash<QString, QString> &grades,
                       const std::optional<double> &opticalLowPoint);

    // The focused shot's grade for every reading that has both a value and a corridor,
    // keyed by metric key. Computed ONCE per rebuild and handed to both projections, for
    // exactly the reason buildGraphics() takes its statistics rather than recomputing
    // them: a tile and the schematic beside it must not be able to grade one reading two
    // different ways. A key absent from the map has no corridor, no reading, or a value
    // the norm calls implausible — three facts the panel draws identically, on purpose.
    QHash<QString, QString> gradesFor(const std::vector<pinpoint::analysis::LmFieldStats> &stats,
                                      const QString &contextId);

    struct Band {
        QString      name;
        QVariantList tiles;
    };

    QPointer<ShotListModel> m_shots;
    int                     m_focusedShotId = -1;
    bool                    m_connected = false;
    bool                    m_saving = false;
    bool                    m_reviewing = false;
    bool                    m_leftHanded = false;
    QString                 m_deviceName;

    QVector<Band> m_bands;
    QVariantMap   m_graphics;
    QString       m_scopeText;
    QString       m_valueLabel;
    QString       m_corridorScope;
    int           m_shotCount = 0;
    bool          m_anyReadings = false;

    // The two registries a corridor comes from. Assembled once and read-only thereafter,
    // same as MetricCatalog's: the norm provider is the process-wide cached one, so a
    // board opening costs no file read, and the pack is this object's own.
    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_pack;
    std::shared_ptr<const pinpoint::analysis::INormProvider>         m_norms;
    QString                                                          m_policyName;
};
