// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

#ifndef GUI_DRAGINPROGRESS_H
#define GUI_DRAGINPROGRESS_H

#include <functional>

#include <FCGlobal.h>

namespace Gui
{

/** Whether a handle is being dragged in a view -- a scope the draggers set,
 * so that what must not move the view under the pointer while one is
 * dragged asks this and nothing about mouse buttons.
 *
 * The first user: the Report view's pop on an error (or a warning, a
 * message) a recompute writes while a handle is dragged. The pop raises or
 * shows the Report view's dock, which takes room from the 3D view; the
 * model then moves under a pointer that has not moved. While a drag is in
 * progress the text is written as always and the pop is held; once the
 * drag has ended and the edited object's recompute has said whether it is
 * still in error, the pop happens if it is. The stop-sign cursor is the
 * in-drag signal of a failing recompute (docs/HandsOnQueue.md entry 143).
 */
class GuiExport DragInProgress
{
public:
    /// A drag begins; scopes nest (two handles cannot, but a scope may be
    /// entered by more than one layer)
    static void begin();
    /// The drag that began ends
    static void end();
    static bool active();

    /** Hold \a pop until the drag has ended and the edited object is known
     * to be in error still -- told by recomputed(), or found so by
     * whoever holds it once no recompute has come for a while. One held at a
     * time: a later one replaces it.
     */
    static void holdPop(std::function<void()> pop);
    /// The edited object recomputed, \a errorStands if it is in error: a
    /// held pop is let go if the drag has ended, dropped if not in error
    static void recomputed(bool errorStands);
};

}  // namespace Gui

#endif  // GUI_DRAGINPROGRESS_H
