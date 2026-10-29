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

#include <TopoDS_Edge.hxx>

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/Surface/SurfaceGlobal.h>

#include "Geom/EdgeCondition.h"

namespace Surface
{

/// The properties of one edge condition on a feature (docs/Surface.md sec
/// 4.2, 4.3): its continuity, its weight and its support face
struct ConditionProperties
{
    const char* name;  // "first profile", for messages and Achieved
    App::PropertyEnumeration* continuity;
    App::PropertyFloatConstraint* weight;
    App::PropertyLinkSub* support;
};

SurfaceExport extern const char* ContinuityEnums[];
SurfaceExport extern const App::PropertyFloatConstraint::Constraints WeightRange;

/** The condition the properties ask for, on the curve `edge`, which is
 * `sub` of `source`: an explicit support, or the one face of the source's
 * shape the edge lies on. Returns why it cannot be had, or empty.
 */
SurfaceExport std::string conditionOf(const ConditionProperties& props,
                                      App::DocumentObject* source,
                                      const std::string& sub,
                                      const TopoDS_Edge& edge,
                                      Geom::EdgeCondition& condition);

/// A line of Achieved: what the condition achieved
SurfaceExport std::string describe(const ConditionProperties& props,
                                   const Geom::EdgeCondition& condition,
                                   const Geom::MatchReport& report);

}  // namespace Surface

/// Adds the three properties of a condition named _name (FirstProfile ->
/// FirstProfileContinuity, FirstProfileWeight, FirstProfileSupport) to the
/// group "Conditions"; _doc is what it is the condition of
#define SURFACE_ADD_CONDITION(_name, _doc)                                                         \
    ADD_PROPERTY_TYPE(_name##Continuity,                                                           \
                      ((long)0),                                                                   \
                      "Conditions",                                                                \
                      App::Prop_None,                                                              \
                      "How the surface meets its neighbour along " _doc                           \
                      ": G0 position, G1 tangent, G2 curvature");                                  \
    _name##Continuity.setEnums(Surface::ContinuityEnums);                                          \
    ADD_PROPERTY_TYPE(_name##Weight,                                                               \
                      (1.0),                                                                       \
                      "Conditions",                                                                \
                      App::Prop_None,                                                              \
                      "Scales the surface's derivative across " _doc ": 1 keeps its own");        \
    _name##Weight.setConstraints(&Surface::WeightRange);                                           \
    ADD_PROPERTY_TYPE(_name##Support,                                                              \
                      (nullptr),                                                                   \
                      "Conditions",                                                                \
                      App::Prop_None,                                                              \
                      "The face to meet along " _doc                                               \
                      "; empty takes the face the curve is an edge of");                          \
    _name##Support.setScope(App::LinkScope::Global)
