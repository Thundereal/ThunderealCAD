/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ***************************************************************************/

#ifndef APP_SAVETRIGGER_H
#define APP_SAVETRIGGER_H

#include <string>
#include <FCGlobal.h>

namespace App {

/** Who asked for a save (docs/HandsOnQueue.md entry 167).
 *
 * A scope opened by whoever starts a save -- a command, the "Save" message
 * on its way to a view, a close-confirm box, a Python binding with its
 * caller's file and line, the MCP console with its client -- and read by
 * the save itself: the history records it with the save (meta
 * `save:<id>`, the copy's `save_trigger`), and the console line every
 * save writes names it. Scopes nest, the outermost first, joined by '/':
 * "Std_Save/Save@TechDrawGui::MDIViewPage". A Python binding names its
 * caller only when nothing outside Python has: a command's doCommand runs
 * the binding, and the MCP console's "mcp:<client>" stands alone, the
 * block itself being in that console's log. A save with no scope open is
 * "unknown", which is also what a record from before triggers were kept
 * reads as.
 */
class AppExport SaveTrigger
{
public:
    explicit SaveTrigger(const std::string& name);
    ~SaveTrigger();
    SaveTrigger(const SaveTrigger&) = delete;
    SaveTrigger& operator=(const SaveTrigger&) = delete;

    /// The scopes open on this thread, joined; unknown() with none
    static std::string current();
    /// Whether a scope is open on this thread
    static bool active();
    /// The Python caller, "python:<file>:<line>", or "python" with no frame.
    /// The GIL must be held.
    static std::string pythonCaller();
    /// What a save with no scope, and a record without one, reads as
    static const char* unknown()
    {
        return "unknown";
    }
};

} // namespace App

#endif // APP_SAVETRIGGER_H
