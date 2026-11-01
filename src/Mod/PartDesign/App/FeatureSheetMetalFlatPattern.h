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
#include <App/PropertyUnits.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "SheetMetal/Flat.h"

namespace PartDesign
{

class SheetMetalExtension;

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

    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalFlatPattern";
    }

    /// The folded part: a feature of the body, sheet metal or one after it
    App::PropertyLink Source;
    /// Each bend's tangent lines in the shape and the DXF
    App::PropertyBool TangentLines;
    /// An edge of the folded part whose image the flat is turned to run along X
    App::PropertyLinkSub AlignEdge;
    /// The grain arrow in the shape and the DXF
    App::PropertyBool Grain;
    /// The grain's direction, from the flat's X
    App::PropertyAngle GrainAngle;
    /// The wall's skin laid on XY; the first wall's first skin if none
    App::PropertyLinkSub RootFace;

    /** @name What a bill of materials takes (docs/SheetMetal.md sec 5.4) */
    //@{
    /// The flat's size on XY, the longer side first
    App::PropertyLength FlatLength;
    App::PropertyLength FlatWidth;
    /// The blank's area
    App::PropertyArea FlatArea;
    App::PropertyLength Thickness;
    /// The name of the rule's material card
    App::PropertyString MaterialName;
    App::PropertyInteger BendCount;
    App::PropertyAngle MaxBendAngle;
    /// Each bend, Up or Down: Up where the material comes up toward one
    /// looking down on the flat
    App::PropertyMap BendDirections;
    //@}

    App::DocumentObjectExecReturn* execute() override;
    PyObject* getPyObject() override;

    /// Write the flat as DXF (SheetMetal::writeDXF()); throws on failure
    void exportDXF(const char* path) const;
    /// Write the flat as SVG (SheetMetal::writeSVG()); throws on failure
    void exportSVG(const char* path) const;

private:
    /// The flat of Source from RootFace, the folded part in folded if given;
    /// throws Base::ValueError saying why not
    SheetMetal::FlatResult buildFlat(SheetMetalExtension*& sheet,
                                     Part::TopoShape* folded = nullptr) const;
};

}  // namespace PartDesign
