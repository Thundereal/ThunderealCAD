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

#ifndef GUI_DLG_IMPORT_CLASH_H
#define GUI_DLG_IMPORT_CLASH_H

#include <vector>

#include <App/Document.h>
#include <FCGlobal.h>

namespace Gui
{

/** Objects brought into a document -- pasted, merged, dropped -- of which a
 * live object there has the UID (docs/TransactionLog.md sec 36.4): a list,
 * each row Assign (the live object takes the state brought) or Duplicate (a
 * new object, as a paste was), with Assign all and Duplicate all. Sets each
 * clash's `assign`; false when cancelled.
 */
GuiExport bool askImportClashes(std::vector<App::Document::ImportClash>& clashes);

/// What the user command brings is asked about (sec 36.4), for its scope.
class GuiExport ImportClashAsk: public App::Document::ImportClashScope
{
public:
    ImportClashAsk();
    /// Every clash the last import asked about was assigned (a macro line
    /// can say so), and there was one.
    bool allAssigned() const { return _all; }

private:
    bool _all {false};
};

}  // namespace Gui

#endif  // GUI_DLG_IMPORT_CLASH_H
