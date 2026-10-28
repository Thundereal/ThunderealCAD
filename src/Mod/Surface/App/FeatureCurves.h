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

#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Surface/SurfaceGlobal.h>

namespace Surface
{

/// Isoparametric curves of a face, clipped to it (docs/Surface.md sec 5.3)
class SurfaceExport Isocurve: public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::Isocurve);

public:
    Isocurve();

    App::PropertyLinkSub Face;
    App::PropertyIntegerConstraint NumberU;
    App::PropertyIntegerConstraint NumberV;

    App::DocumentObjectExecReturn* execute() override;
};

/// Curves laid onto faces as curves on their surfaces, with an optional
/// ribbon along them to serve as a support (docs/Surface.md sec 5.3)
class SurfaceExport CurveOnSurface: public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::CurveOnSurface);

public:
    CurveOnSurface();

    App::PropertyLinkSubList Curves;
    App::PropertyLinkSubList Faces;
    App::PropertyVector Direction;
    App::PropertyDistance Ribbon;

    App::DocumentObjectExecReturn* execute() override;
};

/// Where two shapes meet, as edges on both (docs/Surface.md sec 5.3)
class SurfaceExport IntersectionCurve: public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::IntersectionCurve);

public:
    IntersectionCurve();

    App::PropertyLinkSubList Shape1;
    App::PropertyLinkSubList Shape2;

    App::DocumentObjectExecReturn* execute() override;
};

/// Curves joined into smooth ones, broken at corners (docs/Surface.md sec
/// 5.3)
class SurfaceExport JoinCurves: public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::JoinCurves);

public:
    JoinCurves();

    App::PropertyLinkSubList Curves;
    App::PropertyDistance Tolerance;
    App::PropertyAngle CornerAngle;
    App::PropertyDistance Gap;

    App::DocumentObjectExecReturn* execute() override;
};

/// A curve with its ends carried on, naturally or straight
/// (docs/Surface.md sec 5.3)
class SurfaceExport ExtendCurve: public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::ExtendCurve);

public:
    ExtendCurve();

    App::PropertyLinkSub Curve;
    App::PropertyDistance StartLength;
    App::PropertyDistance EndLength;
    App::PropertyEnumeration Extension;

    App::DocumentObjectExecReturn* execute() override;
};

}  // namespace Surface
