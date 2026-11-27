// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#ifndef TECHDRAW_STARTMAKING_H
#define TECHDRAW_STARTMAKING_H

#include <memory>

#include <QFutureWatcher>
#include <QtConcurrentRun>

#include "DrawView.h"

namespace TechDraw
{

/** A view's projection, faces, section or detail cut: `work` on a worker
 * thread, then `finished` on this one when it ends, through `watcher` --
 * or, where views make inline (DrawView::makesInline: a console process,
 * or the preference), both now, in that order, on this thread, done when
 * this returns (docs/WorkerProcess.md sec 39). `finished` tells a failure
 * of `work` by the progress `work` cancels, as it does from a thread.
 */
template<class Work, class Finished>
void startMaking(std::unique_ptr<QFutureWatcher<void>>& watcher, Work work, Finished finished)
{
    if (DrawView::makesInline()) {
        watcher.reset();
        work();
        finished();
        return;
    }
    watcher.reset(new QFutureWatcher<void>());
    QObject::connect(watcher.get(), &QFutureWatcherBase::finished, watcher.get(), finished);
    watcher->setFuture(QtConcurrent::run(work));
}

}  // namespace TechDraw

#endif  // TECHDRAW_STARTMAKING_H
