// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

// Not upstream. The Cam joint (docs/AssemblySolverInterface.md sec 6.18): a
// point of the follower held on a curve of the cam -- JointKind::PointOnCurve
// in the solver's model. Here: the OCCT side of the neutral curve
// (Solver/Curve.h), what a Cam joint may be picked between, and the builder's
// part of it (AssemblyObject::makeMbdJoint calls addCamCurve()).
//
// The joint's Reference1 is the cam: an edge, any curve (a line, a circle, an
// ellipse, a B-spline; planar or not). Reference2 is the follower: a vertex
// (a point follower), or a circular edge -- a roller, whose centre is held on
// the cam curve offset by the roller's radius, in the cam curve's plane, on
// the side the centre starts on. A roller needs a planar cam curve. Flat-faced
// followers are not done.

#pragma once

#include <memory>
#include <string>

#include <Geom_Curve.hxx>
#include <TopoDS_Edge.hxx>

#include <Base/Placement.h>
#include <Mod/Assembly/AssemblyGlobal.h>

#include "Solver/Model.h"

namespace App
{
class DocumentObject;
}

namespace Assembly
{

/// An OCCT curve over [first, last] as the solver's neutral curve, moved by
/// 'placement' (into the coordinates of the joint's frame I). Evaluated with
/// the curve's own D2; closed when its ends meet (Precision::Confusion()), so
/// its parameter repeats with the period last - first; the plane normal from
/// BRepLib_FindSurface; described as the B-spline GeomConvert makes of it.
AssemblyExport std::shared_ptr<const Solver::CurveGeometry> makeCurveGeometry(
    const Handle(Geom_Curve)& curve,
    double first,
    double last,
    const Base::Placement& placement = Base::Placement()
);

/// Of an edge: its 3D curve, its range and its location; null when the edge
/// has no 3D curve.
AssemblyExport std::shared_ptr<const Solver::CurveGeometry> makeCurveGeometry(
    const TopoDS_Edge& edge,
    const Base::Placement& placement = Base::Placement()
);

/// Why the joint's picks cannot make a cam: Reference1 must be an edge,
/// Reference2 a vertex or a circular edge (a roller), and a roller needs a
/// cam curve that lies in a plane. Empty when they can. A pick not made yet
/// is not checked (the task panel asks after each pick).
AssemblyExport std::string camJointRefusal(const App::DocumentObject* joint);

/// Completes the PointOnCurve joint the builder made for a Cam joint, whose
/// frames are set: adds the cam's curve to the model, in frame I's
/// coordinates, and gives the joint the curve and the roller's offset. False,
/// with a warning, when the joint cannot be a cam. 'camOffset' is the cam
/// part's offset in the solver body it belongs to (a bundle's member).
bool addCamCurve(
    App::DocumentObject* joint,
    Solver::Model& model,
    Solver::Joint& mbdJoint,
    const Base::Placement& camOffset
);

}  // namespace Assembly
