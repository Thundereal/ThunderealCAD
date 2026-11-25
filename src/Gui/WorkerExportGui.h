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


#ifndef GUI_WORKER_EXPORT_GUI_H
#define GUI_WORKER_EXPORT_GUI_H

#include <string>
#include <vector>

#include <FCGlobal.h>

namespace App
{
class Document;
class DocumentObject;
}  // namespace App

namespace Gui
{

/** An export the user asks for (Std_Export) handed to the document's workers
 * (docs/WorkerProcess.md sec 24), when ExportCommand of Preferences/Workers
 * says so (on by default; the Document preferences' "Export in a worker
 * process") and `module` is one of ExportModules there ("Import;Part;Mesh":
 * modules that export the same without a GUI, and ask for no options): a
 * worker writes `file` from its copy of the design as it is now, while the
 * window stays free, and this process tells when it is written.
 *
 * `code` is the export as it is run here: recorded in the macro as if it
 * had been, and run here if the worker cannot make it and the document did
 * not change since. False when it is not handed over, and the caller exports
 * here.
 */
GuiExport bool exportByWorkers(App::Document& doc, const std::vector<App::DocumentObject*>& objs,
                               const std::string& module, const std::string& file,
                               const std::string& code);

}  // namespace Gui

#endif  // GUI_WORKER_EXPORT_GUI_H
