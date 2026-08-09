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

// Link stubs for the ReanalysisController seam test. The controller now calls
// into the full offline analysis pipeline (reanalyzeSwingDir) and the swing.json
// writer (writeSwingJson); pulling those in would drag the whole analyzer +
// OpenCV + pose stack into what is deliberately a thin funnel test. The seam
// test only exercises queue/de-dupe/signal behaviour and never reaches the
// worker body (no event loop runs, so QFutureWatcher::finished never fires), so
// trivial definitions satisfy the linker without faking observable behaviour.

#include "../Analysis/swing_reanalyzer.h"
#include "../Export/swing_doc.h"

namespace pinpoint::analysis {

ReanalyzeResult reanalyzeSwingDir(const QString & /*swingDir*/, const ReanalyzeOptions & /*opts*/)
{
    return {};   // ok == false; never observed by the seam test
}

} // namespace pinpoint::analysis

namespace pinpoint {

bool SwingDocWriter::writeSwingJson(const QString & /*swingDir*/, const QJsonObject & /*rawManifest*/,
                                    const analysis::SwingAnalysis * /*analysis*/, QString * /*error*/,
                                    const QString & /*club*/)
{
    return true;   // unreached: onWorkerFinished needs an event loop the test never runs
}

} // namespace pinpoint
