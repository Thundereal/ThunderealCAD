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
#include <Mod/Part/App/FeaturePartSpline.h>
#include <Mod/Surface/SurfaceGlobal.h>

#include "EdgeConditions.h"

namespace Surface
{

/// A surface bridging two edges on two faces, G0 to G2 and a weight on
/// each side (docs/Surface.md sec 5.1); Geom::blendSurface builds it
class SurfaceExport BlendSurface: public Part::Spline
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::BlendSurface);

public:
    BlendSurface();

    App::PropertyLinkSub Edge1;
    App::PropertyLinkSub Edge2;
    App::PropertyEnumeration Side1Continuity;
    App::PropertyFloatConstraint Side1Weight;
    App::PropertyLinkSub Side1Support;
    App::PropertyEnumeration Side2Continuity;
    App::PropertyFloatConstraint Side2Weight;
    App::PropertyLinkSub Side2Support;
    App::PropertyIntegerConstraint Samples;
    App::PropertyStringList Achieved;  // Output

    App::DocumentObjectExecReturn* execute() override;
};

}  // namespace Surface
