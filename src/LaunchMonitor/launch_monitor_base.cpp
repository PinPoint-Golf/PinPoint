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

#include "launch_monitor_base.h"

namespace pinpoint::lm {

// The settings token. Stable across releases — it is what `launchmonitor/kind`
// holds in the user's INI, so renaming one silently disables a configured monitor.
QString kindKey(Kind k)
{
    switch (k) {
    case Kind::None:   return QStringLiteral("none");
    case Kind::GcQuad: return QStringLiteral("gcquad");
    }
    return QStringLiteral("none");
}

Kind kindFromKey(const QString &key)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("gcquad"))
        return Kind::GcQuad;
    return Kind::None;
}

} // namespace pinpoint::lm
