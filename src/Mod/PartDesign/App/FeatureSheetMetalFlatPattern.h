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

#include <App/PropertyLinks.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

namespace PartDesign
{

/** A sheet laid flat (docs/SheetMetal.md sec 4.7)
 *
 * In the body, not in its solid chain, as a sketch is. Its shape is a
 * compound of the flat solid, on XY from the root face with the material
 * above, and of the bend lines, each named after its bend. It recomputes
 * with the part.
 */
class PartDesignExport SheetMetalFlatPattern : public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::SheetMetalFlatPattern);

public:
    SheetMetalFlatPattern();

    /// The folded part: a feature of the body, sheet metal or one after it
    App::PropertyLink Source;
    /// The wall's skin laid on XY; the first wall's first skin if none
    App::PropertyLinkSub RootFace;

    App::DocumentObjectExecReturn* execute() override;
};

}  // namespace PartDesign
