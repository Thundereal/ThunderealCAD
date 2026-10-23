// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
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

// Not upstream. What a joint's element is, geometrically: the one place the
// Assembly workbench reads an edge's curve or a face's surface type
// (docs/AssemblySolverInterface.md sec 6.11).
//
// An element whose geometry is already analytic (a line, a circle, an
// ellipse; a plane, a cylinder, a cone, a sphere, a torus) is taken as it is.
// Any other -- a B-spline, a Bezier, a surface of revolution or extrusion,
// an offset -- is handed to OCCT's canonical recognition
// (ShapeAnalysis_CanonicalRecognition; GeomConvert_SurfToAnaSurf for the
// torus, which the former does not try), so a STEP file's B-spline circle or
// cylinder is classified and framed as the circle or cylinder it is. What is
// still not recognised is freeform.
//
// Tolerance: 1e-6 of the element's bounding-box diagonal (Assembly3's
// normalised 1e-6), never below Precision::Confusion(). The analysis is
// cached by the element's TShape.

#pragma once

#include <string>

#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace App
{
class DocumentObject;
}

namespace Assembly
{

/// An edge's curve as analytic geometry.
struct ElementCurve
{
    /// GeomAbs_Line, GeomAbs_Circle or GeomAbs_Ellipse when the edge is one
    /// (exactly or within the tolerance); else the adaptor's own type.
    GeomAbs_CurveType type {GeomAbs_OtherCurve};
    /// The analytic curve (Geom_Line, Geom_Circle, Geom_Ellipse) in the
    /// shape's coordinates; null when the edge is none of those.
    Handle(Geom_Curve) curve;
    /// The geometry was converted from another form (B-spline, ...).
    bool recognized {false};
    /// Largest deviation of the converted curve from the edge's.
    double gap {0.0};
};

/// A face's surface as analytic geometry.
struct ElementSurface
{
    /// GeomAbs_Plane, _Cylinder, _Cone, _Sphere or _Torus when the face is one;
    /// else the adaptor's own type.
    GeomAbs_SurfaceType type {GeomAbs_OtherSurface};
    /// The analytic surface (a Geom_ElementarySurface) in the shape's
    /// coordinates; null when the face is none of those.
    Handle(Geom_Surface) surface;
    bool recognized {false};
    double gap {0.0};
};

/// The recognition tolerance for a shape: 1e-6 of its bounding-box diagonal,
/// at least Precision::Confusion().
AssemblyExport double recognitionTolerance(const TopoDS_Shape& shape);

AssemblyExport ElementCurve analyzeEdge(const TopoDS_Edge& edge);
AssemblyExport ElementSurface analyzeFace(const TopoDS_Face& face);

/// The element (EdgeN, FaceN) of a Part feature's shape, analysed; a type of
/// OtherCurve / OtherSurface when obj has no such element.
AssemblyExport ElementCurve analyzeEdge(const App::DocumentObject* obj, const std::string& elName);
AssemblyExport ElementSurface analyzeFace(const App::DocumentObject* obj,
                                          const std::string& elName);

/// What the geometry is, for messages: "a line", "a B-spline curve",
/// "a cone", "a B-spline surface", ...
AssemblyExport std::string describeCurveType(GeomAbs_CurveType type);
AssemblyExport std::string describeSurfaceType(GeomAbs_SurfaceType type);

/// Whether the analysed type is one a joint can use: a line, a circle or an
/// ellipse; a plane, a cylinder, a cone, a sphere or a torus.
AssemblyExport bool isAnalyticCurve(GeomAbs_CurveType type);
AssemblyExport bool isAnalyticSurface(GeomAbs_SurfaceType type);

/// Drops the cached analyses (tests).
AssemblyExport void clearElementGeometryCache();
/// How many conversions have been attempted since the cache was cleared, for
/// tests that check the cache is used.
AssemblyExport int elementGeometryConversions();

}  // namespace Assembly
