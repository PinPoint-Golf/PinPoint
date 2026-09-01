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

#include "notification_center.h"

#include "pp_debug.h"

#include <QDateTime>

NotificationCenter::NotificationCenter(QObject *parent)
    : QAbstractListModel(parent)
    , m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
{
    // One sweeping timer for the whole model rather than a QTimer per row.  A
    // second of granularity on a 7 s dwell is not perceptible, and the old
    // per-instance QML timers are exactly what made "restart the timer with new
    // text" the accidental behaviour this class exists to remove.
    m_sweep.setInterval(1000);
    connect(&m_sweep, &QTimer::timeout, this, &NotificationCenter::sweep);
    m_sweep.start();
}

int NotificationCenter::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_live.size());
}

QVariant NotificationCenter::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= int(m_live.size()))
        return {};
    const Row &r = m_live[std::size_t(index.row())];
    switch (role) {
    case IdRole:          return r.n.id;
    case SeverityRole:    return int(r.n.severity);
    case KindRole:        return int(r.n.kind);
    case TitleRole:       return r.n.title;
    case DetailRole:      return r.n.detail;
    case CauseRole:       return r.n.cause;
    case CountRole:       return r.count;
    case GlyphRole:       return r.n.glyph;
    case ActionLabelRole: return r.n.actionLabel;
    case ActionIdRole:    return r.n.actionId;
    default:              return {};
    }
}

QHash<int, QByteArray> NotificationCenter::roleNames() const
{
    return {
        { IdRole,          "notificationId" },
        { SeverityRole,    "severity"       },
        { KindRole,        "kind"           },
        { TitleRole,       "title"          },
        { DetailRole,      "detail"         },
        { CauseRole,       "cause"          },
        { CountRole,       "count"          },
        { GlyphRole,       "glyph"          },
        { ActionLabelRole, "actionLabel"    },
        { ActionIdRole,    "actionId"       },
    };
}

int NotificationCenter::indexOf(const QString &id) const
{
    for (std::size_t i = 0; i < m_live.size(); ++i)
        if (m_live[i].n.id == id) return int(i);
    return -1;
}

bool NotificationCenter::isLive(const QString &id) const
{
    return indexOf(id) >= 0;
}

int NotificationCenter::countOf(const QString &id) const
{
    const int i = indexOf(id);
    return i < 0 ? 0 : m_live[std::size_t(i)].count;
}

bool NotificationCenter::causeIsLive(const QString &causeId, int depth) const
{
    if (causeId.isEmpty()) return false;
    if (depth > 8) return false;              // a cause cycle, described by mistake
    if (indexOf(causeId) >= 0) return true;
    // R2's grace window: a cause that has just left the screen is still
    // something the user knows about.
    const auto it = m_recentlyLive.constFind(causeId);
    if (it != m_recentlyLive.constEnd() && (nowMs() - it.value()) <= kCauseGraceMs)
        return true;
    // The middle of a cascade: this cause was itself suppressed under something
    // the user IS looking at, so it still stands in for it.
    const auto up = m_suppressedUnder.constFind(causeId);
    return up != m_suppressedUnder.constEnd() && causeIsLive(up.value(), depth + 1);
}

void NotificationCenter::logPost(const Notification &n, int count, bool shown) const
{
    // ⚠ ALWAYS, whether shown or not.  R2 suppresses a consequence from the
    // SCREEN, never from the record: the log is where the cascade stays legible
    // afterwards, and on 1 September the cascade was the whole story.
    const QString tail = QStringLiteral("[notify] %1%2 %3%4%5")
        .arg(n.id,
             count > 1 ? QStringLiteral(" (x%1)").arg(count) : QString(),
             n.title,
             n.detail.isEmpty() ? QString() : QStringLiteral(" — ") + n.detail,
             shown ? QString()
                   : QStringLiteral(" [suppressed; consequence of ") + n.cause + QLatin1Char(']'));

    if (n.severity == Error)     ppError() << tail;
    else if (n.severity == Warn) ppWarn()  << tail;
    else                         ppInfo()  << tail;
}

void NotificationCenter::post(const Notification &n)
{
    if (n.id.isEmpty()) {
        // A notification without a stable key cannot be counted, coalesced or
        // named as a cause, which is the entire point of this class.  Refusing
        // it here keeps the defect at the call site instead of on the screen.
        ppWarn() << "[notify] refused a notification with no id:" << n.title;
        return;
    }

    // R1 — coalesce on id.
    const int existing = indexOf(n.id);
    if (existing >= 0) {
        Row &r = m_live[std::size_t(existing)];
        ++r.count;
        // The text may legitimately differ between occurrences (a second path
        // with a different detail); the LATEST wins, as it did before, but now
        // the count says the earlier ones happened.
        r.n = n;
        // ⚠ Only an Event's dwell restarts.  A Condition does not auto-hide, so
        // there is nothing to restart, and refreshing it would be the old
        // "silently restarted timer with new text" defect wearing a new hat.
        if (n.kind == Event) r.postedAt = nowMs();
        logPost(n, r.count, /*shown=*/true);
        const QModelIndex ix = index(existing, 0);
        emit dataChanged(ix, ix);
        return;
    }

    // R2 — a consequence whose cause the user is already looking at is recorded
    // and not shown.  Report the cause, count the consequences.
    if (causeIsLive(n.cause)) {
        // Remember what it stood down for, so its OWN consequences stand down
        // too rather than surfacing because the middle link went quiet.
        m_suppressedUnder.insert(n.id, n.cause);
        logPost(n, 1, /*shown=*/false);
        return;
    }
    // Shown on its own merits, so it no longer stands in for anything.
    m_suppressedUnder.remove(n.id);

    // Appended, not prepended: the host stacks upward from the bottom, so the
    // first thing raised keeps the anchor position it had under the old
    // hand-placed toasts and later news piles above it.
    beginInsertRows(QModelIndex(), int(m_live.size()), int(m_live.size()));
    Row r;
    r.n        = n;
    r.count    = 1;
    r.postedAt = nowMs();
    m_live.push_back(r);
    endInsertRows();

    logPost(n, 1, /*shown=*/true);
    emit liveCountChanged();
}

void NotificationCenter::removeAt(int row)
{
    if (row < 0 || row >= int(m_live.size())) return;
    const QString id = m_live[std::size_t(row)].n.id;
    beginRemoveRows(QModelIndex(), row, row);
    m_live.erase(m_live.begin() + row);
    endRemoveRows();
    // Remember when it left, so R2's grace window can still suppress the
    // consequences that are only now arriving.
    m_recentlyLive.insert(id, nowMs());
    emit liveCountChanged();
}

void NotificationCenter::resolve(const QString &id)
{
    removeAt(indexOf(id));
}

void NotificationCenter::dismiss(const QString &id)
{
    removeAt(indexOf(id));
}

void NotificationCenter::invokeAction(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    const QString actionId = m_live[std::size_t(i)].n.actionId;
    if (actionId.isEmpty()) return;
    emit actionRequested(actionId);
}

void NotificationCenter::sweep()
{
    const qint64 now = nowMs();

    // R3 — events expire, conditions latch.  Reverse order so an erase does not
    // move a row we have yet to test.
    for (int i = int(m_live.size()) - 1; i >= 0; --i) {
        const Row &r = m_live[std::size_t(i)];
        if (r.n.kind != Event) continue;
        if (now - r.postedAt >= kEventDwellMs) removeAt(i);
    }

    // Keep the grace map from growing for the life of the process.
    for (auto it = m_recentlyLive.begin(); it != m_recentlyLive.end(); ) {
        if (now - it.value() > kCauseGraceMs) it = m_recentlyLive.erase(it);
        else                                  ++it;
    }
    // A suppression stands only while the cause it stood down for does.
    for (auto it = m_suppressedUnder.begin(); it != m_suppressedUnder.end(); ) {
        if (!causeIsLive(it.value())) it = m_suppressedUnder.erase(it);
        else                          ++it;
    }
}

void NotificationCenter::setClockForTest(std::function<qint64()> clock)
{
    if (clock) m_clock = std::move(clock);
}
