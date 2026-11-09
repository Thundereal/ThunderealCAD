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

// Not upstream. What an element is, geometrically, and the frame it puts its
// users on: the one place an edge's curve or a face's surface type is read
// for elements (Part::Element, Part::ElementBinding) and for the Assembly
// workbench's joints (docs/AssemblySolverInterface.md sec 6.11,
// docs/AssemblyObjectModel.md sec 7.1). It was Assembly's until the element
// link needed the frame.
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

#include <Base/Placement.h>
#include <Mod/Part/PartGlobal.h>

namespace App
{
class DocumentObject;
}

namespace Part
{

class TopoShape;

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
PartExport double recognitionTolerance(const TopoDS_Shape& shape);

PartExport ElementCurve analyzeEdge(const TopoDS_Edge& edge);
PartExport ElementSurface analyzeFace(const TopoDS_Face& face);

/// The element (EdgeN, FaceN) of a Part feature's shape, analysed; a type of
/// OtherCurve / OtherSurface when obj has no such element.
PartExport ElementCurve analyzeEdge(const App::DocumentObject* obj, const std::string& elName);
PartExport ElementSurface analyzeFace(const App::DocumentObject* obj,
                                          const std::string& elName);

/// What the geometry is, for messages: "a line", "a B-spline curve",
/// "a cone", "a B-spline surface", ...
PartExport std::string describeCurveType(GeomAbs_CurveType type);
PartExport std::string describeSurfaceType(GeomAbs_SurfaceType type);

/// Whether the analysed type is one a joint can use: a line, a circle or an
/// ellipse; a plane, a cylinder, a cone, a sphere or a torus.
PartExport bool isAnalyticCurve(GeomAbs_CurveType type);
PartExport bool isAnalyticSurface(GeomAbs_SurfaceType type);

/// Drops the cached analyses (tests).
PartExport void clearElementGeometryCache();
/// How many conversions have been attempted since the cache was cleared, for
/// tests that check the cache is used.
PartExport int elementGeometryConversions();

/// The shape itself, or the one sub-shape a compound of one holds
PartExport TopoDS_Shape singleSubShape(const TopoDS_Shape& shape);

/// The frame an element puts its users on: of a vertex, an edge or a face,
/// or a whole object's shape (its own placement). UtilsAssembly.findPlacement's
/// rules, on a shape in one coordinate system, without a second, vertex,
/// sub-element: an element binding's Vertex moves the origin instead.
PartExport Base::Placement findElementFrame(const TopoShape& shape);

}  // namespace Part
