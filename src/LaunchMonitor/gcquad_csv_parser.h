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

#include "launch_monitor_reading.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace pinpoint::lm {

// Reads FSX2020's LastShot.CSV — a two-line file, one header row and one shot.
//
// PURE FUNCTION BY DESIGN: no settings, no logging, no timers, no file I/O. The
// polling, the watermark and the attribution to a swing all live in GcQuadMonitor,
// so everything decided here can be tested against a string literal.
//
// COLUMNS ARE MATCHED BY NAME, NEVER BY POSITION. Foresight has shipped several
// column orders and an installation can be switched between metric and imperial,
// which changes the units declared in the header without changing the layout. So
// the header is normalised ("Club head Speed (m/s)" → token `clubheadspeed`, unit
// `m/s`) and the declared unit drives the conversion into the catalogue's units.
// A column we do not recognise is ignored rather than fatal — a newer FSX2020
// adding a column must not stop the shot being read.
//
// UNMEASURED QUANTITIES ARE DROPPED, NOT READ. A GCQuad tracks the club from
// reflective stickers on it; with none fitted the ball numbers are still measured
// and good, while every club quantity is unknown. FSX2020 fills those cells with
// an in-band marker (16777215, sometimes unit-converted) rather than leaving them
// empty, so the parser filters them out and leaves the field absent. See kNoData
// in the .cpp for the marker and why a plain constant match is not sufficient.
//
// Returns nullopt when the input is not a usable reading: no header, no data row,
// no Shot ID, or no numeric value at all. A torn read — the file caught
// mid-rewrite — lands here, and the caller must treat it as "nothing yet" and
// retry rather than consuming its watermark. `error` receives a reason when given.
std::optional<LaunchMonitorReading> parseLastShotCsv(const QByteArray &bytes,
                                                     QString *error = nullptr);

// Exposed for tests: the header normalisation the column match relies on.
// "Club head Speed (m/s)" → token "clubheadspeed", unit "m/s".
void normaliseHeaderCell(const QString &cell, QString *token, QString *unit);

// Exposed for tests: quote-aware split of one CSV line, cells trimmed.
QStringList splitCsvLine(const QString &line);

} // namespace pinpoint::lm
