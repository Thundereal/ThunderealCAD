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

#ifndef APP_SHUTTING_DOWN_H
#define APP_SHUTTING_DOWN_H

#include <FCGlobal.h>

namespace App
{

/** True once the application is being taken down (Application::destruct()),
 * on any thread. A task of the shared thread pool that has not begun its
 * work ends at once when it sees this: destruct() waits for what the pool
 * runs before it lets go of what such a task reads -- the parameters, the
 * console, the interpreter -- and a task queued behind others would
 * otherwise do all of its work first, for no one.
 */
AppExport bool isShuttingDown();

}  // namespace App

#endif  // APP_SHUTTING_DOWN_H
