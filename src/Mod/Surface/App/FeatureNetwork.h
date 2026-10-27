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

#include <array>

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Mod/Part/App/FeaturePartSpline.h>
#include <Mod/Surface/SurfaceGlobal.h>

namespace Surface
{

/// A surface through a network of curves, profiles one way and guides the
/// other (docs/Surface.md sec 5.1); Geom::networkSurface builds it
class SurfaceExport Network: public Part::Spline
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::Network);

public:
    Network();

    App::PropertyLinkSubList Profiles;
    App::PropertyLinkSubList Guides;
    App::PropertyDistance Tolerance;
    App::PropertyDistance FitTolerance;
    App::PropertyDistance MaxGap;           // Output
    App::PropertyDistance MaxFitDeviation;  // Output

    /// The edge conditions of the outer curves (docs/Surface.md sec 4.2,
    /// 4.3): the first and last profile, the first and last guide
    App::PropertyEnumeration FirstProfileContinuity;
    App::PropertyFloatConstraint FirstProfileWeight;
    App::PropertyLinkSub FirstProfileSupport;
    App::PropertyEnumeration LastProfileContinuity;
    App::PropertyFloatConstraint LastProfileWeight;
    App::PropertyLinkSub LastProfileSupport;
    App::PropertyEnumeration FirstGuideContinuity;
    App::PropertyFloatConstraint FirstGuideWeight;
    App::PropertyLinkSub FirstGuideSupport;
    App::PropertyEnumeration LastGuideContinuity;
    App::PropertyFloatConstraint LastGuideWeight;
    App::PropertyLinkSub LastGuideSupport;
    /// What each condition achieved, a line each
    App::PropertyStringList Achieved;  // Output

    App::DocumentObjectExecReturn* execute() override;

private:
    struct Side
    {
        const char* name;
        App::PropertyEnumeration* continuity;
        App::PropertyFloatConstraint* weight;
        App::PropertyLinkSub* support;
    };
    std::array<Side, 4> sides();
};

}  // namespace Surface
