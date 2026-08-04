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

#include "launch_monitor_factory.h"

#include "gcquad_monitor.h"

#include <QCoreApplication>

namespace pinpoint::lm {

LaunchMonitorBase *makeLaunchMonitor(Kind kind, QObject *parent)
{
    switch (kind) {
    case Kind::GcQuad:
        return new GcQuadMonitor(parent);
    case Kind::None:
        break;
    }
    return new InertLaunchMonitor(parent);
}

QList<Kind> availableKinds()
{
    return { Kind::None, Kind::GcQuad };
}

QString kindLabel(Kind kind)
{
    switch (kind) {
    case Kind::None:
        return QCoreApplication::translate("LaunchMonitor", "None");
    case Kind::GcQuad:
        return QCoreApplication::translate("LaunchMonitor", "Foresight GC Quad (FSX2020)");
    }
    return QCoreApplication::translate("LaunchMonitor", "None");
}

} // namespace pinpoint::lm
