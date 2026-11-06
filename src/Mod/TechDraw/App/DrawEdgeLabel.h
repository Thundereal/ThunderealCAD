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

#include <App/PropertyStandard.h>
#include <Mod/TechDraw/TechDrawGlobal.h>

#include "DrawViewBalloon.h"

namespace TechDraw
{

/** A label of an edge of a view's source, taken from the source's view
 * (Gui::ViewProvider::getEdgeInfo(), docs/SheetMetal.md sec 5.6): a sheet's
 * flat gives each bend line its note. A balloon, edited as one -- its text,
 * its shape, where it stands -- that remembers the edge it is of and the text
 * the source gave it, so an import again moves it with its edge and gives it
 * the source's new text unless the user wrote their own.
 */
class TechDrawExport DrawEdgeLabel: public DrawViewBalloon
{
    PROPERTY_HEADER_WITH_OVERRIDE(TechDraw::DrawEdgeLabel);

public:
    DrawEdgeLabel();

    /// The source edge's name, as the view's geometry keeps it
    App::PropertyString SourceEdge;
    /// The text the source gave at the last import; Text other than it is
    /// the user's own
    App::PropertyString SourceText;

    /// Whether the user wrote their own text
    bool isCustomized() const;
};

}  // namespace TechDraw
