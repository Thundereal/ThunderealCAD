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


#pragma once

#include <string>
#include <utility>
#include <vector>

#include <App/DocumentObserver.h>
#include <App/PropertyLinks.h>

namespace Gui
{
class SelectionChanges;
}

namespace SurfaceGui
{

/** @name Geometry picked for a feature, taken as a sketch takes external
 * geometry (docs/Surface.md): through Part::SubShapeBinder::import, which
 * links what is in the feature's scope directly and anything else through a
 * binder in the feature's body or part -- made once and reused. The curves
 * a surface takes are taken as curves: a binder made for them makes no face
 * of them.
 */
//@{
/// Where `feature` is edited from: its path from the top, through the
/// groups (a body, a part) it is in
App::SubObjectT editContext(App::DocumentObject* feature);
/// A pick for `feature`, `picked` its path from the top as it was picked
App::SubObjectT importPick(App::DocumentObject* feature, const App::SubObjectT& picked);
/// The same as Python, for a command: its edit context is the variable
/// `edit` (see editContextPython)
std::string importPython(const App::SubObjectT& picked, const char* edit = "_edit");
/// The Python that sets `edit` to the edit context of `feature`
std::string editContextPython(App::DocumentObject* feature, const char* edit = "_edit");
/// The pick a selection message was made with, unresolved
App::SubObjectT pickOf(const Gui::SelectionChanges& msg);
/// Whether the link (obj, sub) is `picked`: the same, or through a binder of it
bool refersTo(App::DocumentObject* obj, const std::string& sub, const App::SubObjectT& picked);
/// The link of `list` that `picked` is, as listed; the pick resolved if none
std::pair<App::DocumentObject*, std::string> listedAs(const App::PropertyLinkSubList& list,
                                                      const App::SubObjectT& picked);
/// The index of an element name ("Edge3" -> 3); 0 for none -- the empty
/// name of a binder of one element, whole
int elementIndex(const std::string& name);
/// Whether a link is held through a binder: a pick to remove it has to be
/// let through a gate that sees only what was picked
bool anyBinder(const std::vector<App::PropertyLinkSubList::SubSet>& links);
//@}

}  // namespace SurfaceGui
