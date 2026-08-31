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

// ── ppWarn()/ppInfo(), and NOTHING ELSE ─────────────────────────────────────
//
// ⛔ THIS IS ITS OWN TRANSLATION UNIT BECAUSE THE APPLICATION LOG MUST NOT COST
// WHISPER, GGML AND FFMPEG TO USE.
//
// `PpLogStream` — every ppWarn()/ppInfo() in the application — used to live in
// pp_debug.cpp beside `PinPointDebug::install()`, which silences whisper, ggml,
// OpenCV and FFmpeg's loggers and therefore includes whisper.h and ggml.h.  A
// component wanting one log line had to link all of it, and the answer across
// the tree was to stop logging instead: SIX suites (Core, Analysis, Update,
// LaunchMonitor and both Gui harnesses) link `pp_log_stub.cpp`, which satisfies
// the linker and THROWS THE LOG AWAY, and `lm_repair` writes its own.  A class
// compiled into one of those — VideoInputPpcp is the one that brought this to
// light on 31 Aug 2026 — cannot log at all, and reaching for qWarning() instead
// puts PinPoint Studio's one log on a second channel.
//
// The primitive has no business knowing about any of that: it formats a message
// and hands it to PpMessageLog.  So it lives here, needing Qt and PpMessageLog
// and nothing else, and pp_debug.cpp keeps the message handler and the
// noisy-library silencing that genuinely do need those headers.
//
// ⚠ THE STUBS ARE NOW OPTIONAL RATHER THAN NECESSARY.  Any suite still linking
// `pp_log_stub.cpp` can link this file instead and get the real log — worth
// doing wherever a test would rather assert on a log line than discard it.

#include "pp_debug.h"
#include "PpMessageLog.h"

#include <cstdio>

PpLogStream::PpLogStream(QtMsgType t)
    : m_type(t), m_dbg(std::in_place, &m_buf)
{}

PpLogStream::~PpLogStream()
{
    m_dbg.reset();  // destructs QDebug, flushing its internal state to m_buf

    if (m_type == QtDebugMsg) {
        fprintf(stderr, "%s\n", m_buf.trimmed().toLocal8Bit().constData());
    } else {
        PpMessageLog::instance()->append(m_type, m_buf.trimmed());
    }

    if (m_type == QtFatalMsg) ::abort();
}
