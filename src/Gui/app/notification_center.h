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

#include <QAbstractListModel>
#include <QHash>
#include <QQmlEngine>
#include <QString>
#include <QTimer>
#include <functional>

// NotificationCenter — the ONE sink every session- and shot-scoped notification
// posts to.  Exposed in main.cpp as the QML context property `notifications`
// and rendered by a single PpNotificationHost; QML holds no policy.
//
// ⛔ WHAT THIS REPLACES, AND WHY.  Before this class a notification was a bare
// QString handed to one of four PpToast instances wired to four signals.
// PpToast::show() sets the text and restarts a hide timer, so on 1 September
// 2026 ten consecutive save failures produced ONE toast, showing the tenth
// message, with nothing to say it had happened ten times — and the signal that
// means "the shot produced nothing at all" (ShotProcessor::shotFailed) raised
// no toast anywhere.  See docs/design/live_capture_collection_design.md §7.
//
// ⭐ THE KEY IS `id`, AND EVERYTHING ELSE FOLLOWS FROM IT.  A notification is
// identified by a STABLE key ("swing.save.no-directory"), never by its message
// text.  Without one, two occurrences of a single fault cannot be told from two
// different faults, so nothing can be counted, coalesced or named as a cause.
// The count is the diagnostically valuable part and is exactly what the old
// shape threw away.
//
// The four rules of §7.5 are enforced HERE, in C++, so the model is the truth
// and the view only renders:
//
//   R1  Coalesce on `id`.  A repeat updates in place and increments `count`.
//   R2  Suppress a consequence whose `cause` is live (or was, within a grace
//       window): logged, not shown.  The inverse holds — a consequence with no
//       live cause IS shown on its own merits.
//   R3  Conditions latch until resolved or dismissed; events expire.
//   R4  One terminal notification per shot, not one per stage (the callers'
//       side of the bargain — see ShotProcessor).
//
// Every post is ALSO written to the one application log through ppWarn()/
// ppInfo(), which is the house rule: the toast is the act-now half, the log
// keeps the stage detail.
class NotificationCenter : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("NotificationCenter is a context property; the type exists for enum access")

    // How many are on screen right now.  Drives whether the host reserves space.
    Q_PROPERTY(int liveCount READ liveCount NOTIFY liveCountChanged)

public:
    enum Severity { Info, Warn, Error, Progress };
    Q_ENUM(Severity)

    // ⚠ THE DISTINCTION THE OLD SHAPE COULD NOT MAKE.  An Event is news: it
    // happened, it is over, it auto-hides.  A Condition is a STATE OF THE
    // MACHINE — "the library path is unwritable" does not stop being true after
    // 21 ticks of a timer — so it latches until something resolves it.
    enum Kind { Event, Condition };
    Q_ENUM(Kind)

    enum Roles {
        IdRole = Qt::UserRole + 1,
        SeverityRole,
        KindRole,
        TitleRole,
        DetailRole,
        CauseRole,
        CountRole,
        GlyphRole,
        ActionLabelRole,
        ActionIdRole,
    };

    struct Notification {
        QString  id;                        // STABLE KEY — never the message text
        Severity severity = Info;
        Kind     kind     = Event;
        QString  title;                     // "Swings are not being saved"
        QString  detail;                    // "…/mnt/swingdata/… could not be created"
        QString  cause;                     // "" or the id this is a consequence of
        // Optional. Empty takes the severity's default; set it to keep a
        // meaningful symbol the severity cannot express — "⊘" for a shot this
        // host declined is the sentence, where "⚠" would only be the mood.
        QString  glyph;
        QString  actionLabel;               // "" or "Open Storage settings"
        QString  actionId;                  // "" or e.g. "settings.storage"
    };

    explicit NotificationCenter(QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int liveCount() const { return int(m_live.size()); }

    // The one way in.  Applies R1, R2 and R3; always logs.
    void post(const Notification &n);

    // A condition ended because the world changed — the path became writable,
    // the link came back.  Distinct from dismiss(), which is the user saying
    // "I have read this"; a resolved condition may legitimately return.
    void resolve(const QString &id);

    Q_INVOKABLE void dismiss(const QString &id);
    Q_INVOKABLE void invokeAction(const QString &id);

    // True while `id` is on screen.  Callers consult a live condition instead of
    // raising anything per-shot (§7.5 R3).
    Q_INVOKABLE bool isLive(const QString &id) const;
    Q_INVOKABLE int  countOf(const QString &id) const;

    // Retire expired Events.  Driven by a 1 s timer in normal use; called
    // directly by tests, which drive the clock instead of sleeping.
    void sweep();

    // ⚠ TEST SEAM.  Tests substitute a clock they control rather than sleeping
    // through a 7 s expiry and a 30 s grace window.  Nothing in the application
    // calls this.
    void setClockForTest(std::function<qint64()> clock);

    // §7.5 R3 — an Event's dwell, matched to PpToast's own hide timer so the
    // felt behaviour of an ordinary notification is unchanged.
    static constexpr qint64 kEventDwellMs = 7000;
    // §7.5 R2 — how long after a cause leaves the screen its consequences stay
    // suppressed.  A cascade does not arrive all in one frame: the export that
    // failed BECAUSE the folder was missing is reported seconds later, and the
    // user has not forgotten in the meantime.
    static constexpr qint64 kCauseGraceMs = 30000;

signals:
    void liveCountChanged();
    // A notification's action was invoked.  The centre knows nothing about
    // navigation — main.cpp owns that join, exactly as it owns every other
    // cross-object wiring.
    void actionRequested(const QString &actionId);

private:
    struct Row {
        Notification n;
        int    count    = 1;
        qint64 postedAt = 0;   // last post, i.e. what the dwell is measured from
    };

    int  indexOf(const QString &id) const;
    void removeAt(int row);
    // ⚠ FOLLOWS THE CHAIN.  A cascade is not two deep, it is as deep as the
    // system is layered: an unwritable library root suppresses "no swing
    // folder", which is in turn the cause named by "shot not saved" and "shot
    // analysis failed".  If suppression stopped at one hop, the middle link
    // going quiet would let the outer ones through — and the user would be
    // shown the consequences of a fault whose cause is already on screen, which
    // is the exact defect R2 exists to remove.  `depth` bounds a cause cycle a
    // caller could describe by mistake.
    bool causeIsLive(const QString &causeId, int depth = 0) const;
    void logPost(const Notification &n, int count, bool shown) const;
    qint64 nowMs() const { return m_clock(); }

    std::vector<Row>        m_live;
    // id → when it left the screen.  R2's grace window reads this.
    QHash<QString, qint64>  m_recentlyLive;
    // id → the cause it was last suppressed under.  A suppressed notification
    // still REPRESENTS its cause to anything downstream of it.
    QHash<QString, QString> m_suppressedUnder;
    std::function<qint64()> m_clock;
    QTimer                  m_sweep;
};
