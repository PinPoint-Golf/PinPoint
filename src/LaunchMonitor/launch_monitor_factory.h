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

#include "launch_monitor_base.h"

#include <QList>

class QObject;

namespace pinpoint::lm {

// Build the connector for `kind`.
//
// NEVER RETURNS NULL: an unknown or unconfigured kind yields an InertLaunchMonitor
// reporting Disabled, so no caller has to null-check and the "no monitor" case is
// exercised by the same code path as a real one. Same contract as makeUpdateBackend.
//
// Note what is NOT here: a platform #ifdef. FSX2020 is Windows-only, but reading a
// CSV out of a folder is not, and the folder is normally a share from that Windows
// box anyway. Gating the connector on Q_OS_WIN would make the integration
// untestable on the machines it is developed on and would buy nothing.
LaunchMonitorBase *makeLaunchMonitor(Kind kind, QObject *parent = nullptr);

// Every kind the build can offer, in settings-menu order. Always includes None.
QList<Kind> availableKinds();

// The name shown in the settings combo box.
QString kindLabel(Kind kind);

} // namespace pinpoint::lm
