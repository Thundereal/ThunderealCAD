// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

#ifndef GUI_TOOLCONTEXT_H
#define GUI_TOOLCONTEXT_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <FCGlobal.h>

namespace App
{
class DocumentObject;
}

namespace Gui
{
class ToolBarItem;

/** Tool bars a workbench keeps out of sight until something asks for them:
 * the Surface tool bar in PartDesign, offered while a surface group is
 * active in the view or a Surface feature is in edit (docs/SheetMetal.md
 * sec 8).
 *
 * The module owning the tool bars registers a context once. In each of the
 * context's workbenches its tool bars are added unavailable -- not shown,
 * no toggle in the menus -- unless the workbench has one of that name
 * already, and they are offered while
 * - the active view has an object under the context's active-object key (a
 *   group or a solid made active by a double-click), or
 * - an object the context applies to is in edit.
 * Leaving the edit takes them away again, unless the key still holds.
 */
class GuiExport ToolContext
{
public:
    struct Spec
    {
        /// The context's name, e.g. "Surface"
        std::string name;
        /// The workbenches that take its tool bars, e.g. "PartDesignWorkbench"
        std::vector<std::string> workbenches;
        /// Its tool bars: a name each, and what fills it with commands
        std::vector<std::pair<std::string, std::function<void(ToolBarItem&)>>> toolbars;
        /// The view's active-object key that holds it on, e.g. "pdsurface"
        std::string activeKey;
        /// The kind of object it is for, as a module registered it with a
        /// body (Part::BodyBase::allowType), e.g. "Surface"
        std::string kind;
        /// Whether it is for objects in a solid: a solid holding one is the
        /// context's to make active (PartDesign::SolidBody)
        bool inSolid = false;
        /// The icon a solid holding one has overlaid; empty for none
        std::string overlay;
        /// Whether an object in edit is one it is for
        std::function<bool(const App::DocumentObject*)> applies;
    };

    /// Register a context; one of the same name is replaced
    static void add(Spec spec);
    static const std::vector<Spec>& contexts();
    /// The context of that name, or null
    static const Spec* find(const std::string& name);
    /// The first context for objects of this kind, or null
    static const Spec* forKind(const std::string& kind);
    /// Whether the context's tool bars are offered now
    static bool isOffered(const std::string& name);
    /// Offer or take away each context's tool bars as things stand
    static void refresh();
    /// refresh() on the next turn of the event loop; several calls make one
    static void refreshLater();
};

}  // namespace Gui

#endif  // GUI_TOOLCONTEXT_H
