/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#ifndef GUI_WORKER_RECOMPUTE_GUI_H
#define GUI_WORKER_RECOMPUTE_GUI_H

#include <FCGlobal.h>

namespace App
{
class Document;
}

namespace Gui
{

/** The recompute the user asks for -- Std_Refresh, the migration prompt --
 * handed to the document's workers (docs/WorkerProcess.md sec 19), when
 * RecomputeCommand of Preferences/Workers says so (on by default; the
 * Document preferences' "Recompute in a worker process"): the window stays free
 * while a worker makes it, and its result is put in when it comes. False
 * when it is not handed over -- off, no workers, nothing to recompute --
 * and the caller recomputes here. A recompute the workers then fail or
 * cannot make is run here.
 */
GuiExport bool recomputeByWorkers(App::Document& doc);

/** Whether the recompute an edit calls for -- the property editor's, as
 * it commits an edit -- is for the workers (docs/WorkerProcess.md sec 28):
 * RecomputeEdits of Preferences/Workers on (by default), and what it
 * reaches took at least RecomputeEditSeconds (0.5 s by default) when last
 * made (App::WorkerRecompute::expectedSeconds). A short one is made here,
 * at once, as it was: a worker's would come later than that. Asked before
 * the edit's transaction is committed; recomputeEditByWorkers() after.
 */
GuiExport bool editRecomputeIsLong(const App::Document& doc);
/// The edit's recompute handed over, as recomputeByWorkers(); false and
/// the caller recomputes here
GuiExport bool recomputeEditByWorkers(App::Document& doc);

}  // namespace Gui

#endif  // GUI_WORKER_RECOMPUTE_GUI_H
