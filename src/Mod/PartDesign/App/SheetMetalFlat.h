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

#include <map>
#include <string>

#include <Mod/Part/App/TopoShape.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "SheetMetal/Flat.h"

namespace App
{
class DocumentObject;
}

namespace PartDesign
{

/// How a sheet lies flat: its solid's settings (docs/SheetMetal.md sec 12,
/// ruled 2026-11-03)
struct PartDesignExport FlatSettings
{
    /// The wall laid on XY, by its name; the first wall if empty
    std::string root;
    /// Its second skin on XY, not its first
    bool secondSkin = false;
    /// An edge of the folded part, by its name, the flat is turned to run
    /// along X by; none if empty, and a part lacking it is laid unturned
    std::string alignEdge;
    bool tangentLines = false;
    bool grain = false;
    /// The grain's direction from the flat's X, deg
    double grainAngle = 0;

    bool operator==(const FlatSettings&) const = default;
};

/// A part laid flat, and what a bill of materials takes of it
struct PartDesignExport SheetFlat
{
    /// The flat solid, each bend's line, the tangent lines and the grain
    /// arrow if asked for, a compound of a compound each but the first
    Part::TopoShape shape;
    /// The flat's size on XY, the longer side first, and the blank's area
    double length = 0;
    double width = 0;
    double area = 0;
    double thickness = 0;
    long bendCount = 0;
    /// deg
    double maxBendAngle = 0;
    /// Each bend, Up or Down: Up where the material comes up toward one
    /// looking down on the flat
    std::map<std::string, std::string> directions;
    /// The name of the rule's material card
    std::string material;
    /// What it was laid out from, for a DXF or an SVG
    SheetMetal::FlatResult result;
};

/** Lay a part of a sheet flat (docs/SheetMetal.md sec 4.7): a sheet metal
 * feature, or a feature after the sheet's, by the sheet's model before it
 *
 * Throws Base::ValueError saying why not: no sheet, a root that is no
 * wall's name, a face no wall or bend owns.
 */
PartDesignExport SheetFlat layFlat(const App::DocumentObject* part, const FlatSettings& settings);

/** Write the flat of \a obj, a feature of a sheet's solid or the solid (its
 * tip's), with the sheet's settings (docs/SheetMetal.md sec 12): DXF or SVG
 * by the suffix of \a path
 *
 * Throws Base::ValueError saying why not, before anything is written: a
 * suffix neither .dxf nor .svg, an object of no sheet, a flat that does not
 * lie flat; Base::FileException where the file cannot be written.
 */
PartDesignExport void exportFlat(const App::DocumentObject* obj, const char* path);

}  // namespace PartDesign
