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
#include <vector>

#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/Part/App/FeaturePartSpline.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Surface/SurfaceGlobal.h>

namespace Surface
{

/// Faces cut by curves laid onto them, or by surfaces, every piece kept
/// (docs/Surface.md sec 5.2)
class SurfaceExport Split: public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::Split);

public:
    Split();

    App::PropertyLinkSubList Base;
    App::PropertyLinkSubList Tools;
    App::PropertyVector Direction;
    App::PropertyBool Mutual;
    App::PropertyInteger Pieces;  // Output

    App::DocumentObjectExecReturn* execute() override;

    /// The faces of Base, and with Mutual those of Tools, as they are cut;
    /// `inputs` gets every shape the cut was made from. Returns why not, or
    /// empty
    std::string split(Part::TopoShape& pieces, std::vector<Part::TopoShape>& inputs) const;
    /// The faces Base is made of, gathered whole
    std::vector<Part::TopoShape> baseFaces() const;

protected:
    virtual const char* opCode() const;
};

/// A cut that keeps the sides picked: the piece nearest each of KeepPoints
class SurfaceExport Trim: public Split
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::Trim);

public:
    Trim();

    App::PropertyVectorList KeepPoints;
    App::PropertyStringList KeepElements;
    /// Every piece shown, for the sides to be picked from (the task panel's)
    App::PropertyBool ShowPieces;

    App::DocumentObjectExecReturn* execute() override;
    const char* getViewProviderName() const override
    {
        return "SurfaceGui::ViewProviderTrim";
    }

protected:
    const char* opCode() const override;
};

/// A face given its boundary back: its surface's own, or the one it had
/// before a trim
class SurfaceExport Untrim: public Part::Spline
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::Untrim);

public:
    Untrim();

    App::PropertyLinkSub Face;
    App::PropertyLink Before;

    App::DocumentObjectExecReturn* execute() override;
};

}  // namespace Surface
