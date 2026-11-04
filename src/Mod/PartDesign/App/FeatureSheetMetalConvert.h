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
#include <App/PropertyStandard.h>

#include "Feature.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** A part no sheet metal feature made, taken as sheet metal
 * (docs/SheetMetal.md sec 5.3, ruled 2026-11-04)
 *
 * Built on the feature before it -- an imported or a Part solid, as the
 * body's base. A thin part: its walls and bends found from its geometry
 * (SheetMetal::recognise()), each bend its own radius; its shape kept but for
 * faces split where a bend meets a wall. The fixed face is the wall the flat
 * lies on, Wall1.
 */
class PartDesignExport SheetMetalConvert : public PartDesign::Feature, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalConvert);

public:
    SheetMetalConvert();

    /// A face of the part to convert, a wall's skin: the wall the flat lies on
    App::PropertyLinkSub FixedFace;
    /// What the part was taken for: "Thin part"
    App::PropertyString Kind;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    bool shouldApplyPlacement() override
    {
        // the base's shape as it is, in place
        return false;
    }
};

}  // namespace PartDesign
