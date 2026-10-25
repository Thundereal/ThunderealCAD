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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <type_traits>
#include <unordered_map>

#include <BRepAdaptor_Curve.hxx>
#include <gp_Trsf.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Ax3.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopExp.hxx>
#include <Geom_ElementarySurface.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomConvert_SurfToAnaSurf.hxx>
#include <Geom_Circle.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Ellipse.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_CanonicalRecognition.hxx>
#include <Standard_Failure.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_TShape.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Elips.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

#include <App/DocumentObject.h>
#include <Base/Exception.h>
#include <Mod/Part/App/PartFeature.h>

#include "ElementGeometry.h"
#include "TopoShape.h"

namespace Part
{

namespace
{

// The analyses of non-analytic elements, by TShape, in the TShape's own
// coordinates (the element with its location cleared): the same TShape under
// another location is the same geometry moved. The handle keeps the TShape,
// and so its address, alive while it is a key.
struct CachedCurve
{
    Handle(TopoDS_TShape) tshape;
    ElementCurve value;
};
struct CachedSurface
{
    Handle(TopoDS_TShape) tshape;
    ElementSurface value;
};

constexpr std::size_t CacheLimit = 4096;

std::mutex& cacheMutex()
{
    static std::mutex m;
    return m;
}
std::unordered_map<const TopoDS_TShape*, CachedCurve>& curveCache()
{
    static std::unordered_map<const TopoDS_TShape*, CachedCurve> cache;
    return cache;
}
std::unordered_map<const TopoDS_TShape*, CachedSurface>& surfaceCache()
{
    static std::unordered_map<const TopoDS_TShape*, CachedSurface> cache;
    return cache;
}
std::atomic<int> conversions {0};

bool analyticCurve(GeomAbs_CurveType type)
{
    return type == GeomAbs_Line || type == GeomAbs_Circle || type == GeomAbs_Ellipse;
}

bool analyticSurface(GeomAbs_SurfaceType type)
{
    return type == GeomAbs_Plane || type == GeomAbs_Cylinder || type == GeomAbs_Cone
        || type == GeomAbs_Sphere || type == GeomAbs_Torus;
}

// The adaptor's analytic curve, as Geom (the way TopoShapeEdgePy::getCurve
// builds edge.Curve, so the two agree).
Handle(Geom_Curve) curveOf(const BRepAdaptor_Curve& adapt)
{
    switch (adapt.GetType()) {
        case GeomAbs_Line:
            return new Geom_Line(adapt.Line());
        case GeomAbs_Circle:
            return new Geom_Circle(adapt.Circle());
        case GeomAbs_Ellipse:
            return new Geom_Ellipse(adapt.Ellipse());
        default:
            return {};
    }
}

Handle(Geom_Surface) surfaceOf(const BRepAdaptor_Surface& adapt)
{
    switch (adapt.GetType()) {
        case GeomAbs_Plane:
            return new Geom_Plane(adapt.Plane());
        case GeomAbs_Cylinder:
            return new Geom_CylindricalSurface(adapt.Cylinder());
        case GeomAbs_Cone:
            return new Geom_ConicalSurface(adapt.Cone());
        case GeomAbs_Sphere:
            return new Geom_SphericalSurface(adapt.Sphere());
        case GeomAbs_Torus:
            return new Geom_ToroidalSurface(adapt.Torus());
        default:
            return {};
    }
}

// Canonical recognition of an edge with no location; its own type otherwise.
ElementCurve recognizeCurve(const TopoDS_Edge& edge, GeomAbs_CurveType ownType)
{
    ElementCurve res;
    res.type = ownType;
    if (BRep_Tool::Degenerated(edge)) {
        return res;
    }
    ++conversions;
    const double tol = recognitionTolerance(edge);
    try {
        ShapeAnalysis_CanonicalRecognition rec(edge);
        gp_Lin lin;
        if (rec.IsLine(tol, lin)) {
            res.type = GeomAbs_Line;
            res.curve = new Geom_Line(lin);
        }
        else {
            rec.ClearStatus();
            gp_Circ circ;
            if (rec.IsCircle(tol, circ)) {
                res.type = GeomAbs_Circle;
                res.curve = new Geom_Circle(circ);
            }
            else {
                rec.ClearStatus();
                gp_Elips elips;
                if (rec.IsEllipse(tol, elips)) {
                    res.type = GeomAbs_Ellipse;
                    res.curve = new Geom_Ellipse(elips);
                }
            }
        }
        if (!res.curve.IsNull()) {
            res.recognized = true;
            res.gap = rec.GetGap();
        }
    }
    catch (const Standard_Failure&) {
        res = ElementCurve();
        res.type = ownType;
    }
    return res;
}

ElementSurface recognizeSurface(const TopoDS_Face& face, GeomAbs_SurfaceType ownType)
{
    ElementSurface res;
    res.type = ownType;
    ++conversions;
    const double tol = recognitionTolerance(face);
    try {
        ShapeAnalysis_CanonicalRecognition rec(face);
        gp_Pln pln;
        gp_Cylinder cyl;
        gp_Cone cone;
        gp_Sphere sphere;
        // In the order of increasing freedom: a plane is also a cylinder of
        // infinite radius, and a cylinder a cone of zero angle.
        if (rec.IsPlane(tol, pln)) {
            res.type = GeomAbs_Plane;
            res.surface = new Geom_Plane(pln);
        }
        else if (rec.ClearStatus(), rec.IsCylinder(tol, cyl)) {
            res.type = GeomAbs_Cylinder;
            res.surface = new Geom_CylindricalSurface(cyl);
        }
        else if (rec.ClearStatus(), rec.IsCone(tol, cone)) {
            res.type = GeomAbs_Cone;
            res.surface = new Geom_ConicalSurface(cone);
        }
        else if (rec.ClearStatus(), rec.IsSphere(tol, sphere)) {
            res.type = GeomAbs_Sphere;
            res.surface = new Geom_SphericalSurface(sphere);
        }
        if (!res.surface.IsNull()) {
            res.recognized = true;
            res.gap = rec.GetGap();
            return res;
        }

        // ShapeAnalysis_CanonicalRecognition has no torus; the converter it
        // drives does (the same call, aimed at a torus).
        TopLoc_Location loc;
        const Handle(Geom_Surface)& surf = BRep_Tool::Surface(face, loc);
        if (!surf.IsNull()) {
            GeomConvert_SurfToAnaSurf conv(surf);
            conv.SetConvType(GeomConvert_Target);
            conv.SetTarget(GeomAbs_Torus);
            Handle(Geom_Surface) torus = conv.ConvertToAnalytical(tol);
            if (!torus.IsNull() && torus->IsKind(STANDARD_TYPE(Geom_ToroidalSurface))) {
                if (!loc.IsIdentity()) {
                    torus->Transform(loc.Transformation());
                }
                res.type = GeomAbs_Torus;
                res.surface = torus;
                res.recognized = true;
                res.gap = conv.Gap();
            }
        }
    }
    catch (const Standard_Failure&) {
        res = ElementSurface();
        res.type = ownType;
    }
    return res;
}

template<class T>
T moved(const T& value, const TopLoc_Location& loc)
{
    T res = value;
    if (loc.IsIdentity()) {
        return res;
    }
    if constexpr (std::is_same_v<T, ElementCurve>) {
        if (!res.curve.IsNull()) {
            res.curve = Handle(Geom_Curve)::DownCast(res.curve->Transformed(loc.Transformation()));
        }
    }
    else {
        if (!res.surface.IsNull()) {
            res.surface =
                Handle(Geom_Surface)::DownCast(res.surface->Transformed(loc.Transformation()));
        }
    }
    return res;
}

const Part::Feature* partFeature(const App::DocumentObject* obj)
{
    return dynamic_cast<const Part::Feature*>(obj);
}

}  // namespace

double recognitionTolerance(const TopoDS_Shape& shape)
{
    Bnd_Box box;
    try {
        BRepBndLib::Add(shape, box, false);
    }
    catch (const Standard_Failure&) {
        return Precision::Confusion();
    }
    if (box.IsVoid()) {
        return Precision::Confusion();
    }
    return std::max(1e-6 * std::sqrt(box.SquareExtent()), Precision::Confusion());
}

ElementCurve analyzeEdge(const TopoDS_Edge& edge)
{
    ElementCurve res;
    if (edge.IsNull()) {
        return res;
    }
    BRepAdaptor_Curve adapt(edge);
    res.type = adapt.GetType();
    if (analyticCurve(res.type)) {
        res.curve = curveOf(adapt);
        return res;
    }

    const auto& tshape = edge.TShape();
    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        auto it = curveCache().find(tshape.get());
        if (it != curveCache().end()) {
            return moved(it->second.value, edge.Location());
        }
    }
    TopoDS_Edge bare = TopoDS::Edge(edge.Located(TopLoc_Location()));
    ElementCurve value = recognizeCurve(bare, res.type);
    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        if (curveCache().size() >= CacheLimit) {
            curveCache().clear();
        }
        curveCache()[tshape.get()] = CachedCurve {tshape, value};
    }
    return moved(value, edge.Location());
}

ElementSurface analyzeFace(const TopoDS_Face& face)
{
    ElementSurface res;
    if (face.IsNull()) {
        return res;
    }
    BRepAdaptor_Surface adapt(face);
    res.type = adapt.GetType();
    if (analyticSurface(res.type)) {
        res.surface = surfaceOf(adapt);
        return res;
    }

    const auto& tshape = face.TShape();
    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        auto it = surfaceCache().find(tshape.get());
        if (it != surfaceCache().end()) {
            return moved(it->second.value, face.Location());
        }
    }
    TopoDS_Face bare = TopoDS::Face(face.Located(TopLoc_Location()));
    ElementSurface value = recognizeSurface(bare, res.type);
    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        if (surfaceCache().size() >= CacheLimit) {
            surfaceCache().clear();
        }
        surfaceCache()[tshape.get()] = CachedSurface {tshape, value};
    }
    return moved(value, face.Location());
}

ElementCurve analyzeEdge(const App::DocumentObject* obj, const std::string& elName)
{
    auto* feature = partFeature(obj);
    if (!feature) {
        return {};
    }
    try {
        TopoDS_Shape sub = feature->Shape.getShape().getSubShape(elName.c_str(), true);
        if (sub.IsNull() || sub.ShapeType() != TopAbs_EDGE) {
            return {};
        }
        return analyzeEdge(TopoDS::Edge(sub));
    }
    catch (const Standard_Failure&) {
        return {};
    }
    catch (const Base::Exception&) {
        return {};
    }
}

ElementSurface analyzeFace(const App::DocumentObject* obj, const std::string& elName)
{
    auto* feature = partFeature(obj);
    if (!feature) {
        return {};
    }
    try {
        TopoDS_Shape sub = feature->Shape.getShape().getSubShape(elName.c_str(), true);
        if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE) {
            return {};
        }
        return analyzeFace(TopoDS::Face(sub));
    }
    catch (const Standard_Failure&) {
        return {};
    }
    catch (const Base::Exception&) {
        return {};
    }
}

std::string describeCurveType(GeomAbs_CurveType type)
{
    switch (type) {
        case GeomAbs_Line:
            return "a line";
        case GeomAbs_Circle:
            return "a circle";
        case GeomAbs_Ellipse:
            return "an ellipse";
        case GeomAbs_Hyperbola:
            return "a hyperbola";
        case GeomAbs_Parabola:
            return "a parabola";
        case GeomAbs_BezierCurve:
            return "a Bezier curve";
        case GeomAbs_BSplineCurve:
            return "a B-spline curve";
        case GeomAbs_OffsetCurve:
            return "an offset curve";
        default:
            return "a freeform curve";
    }
}

std::string describeSurfaceType(GeomAbs_SurfaceType type)
{
    switch (type) {
        case GeomAbs_Plane:
            return "a plane";
        case GeomAbs_Cylinder:
            return "a cylinder";
        case GeomAbs_Cone:
            return "a cone";
        case GeomAbs_Sphere:
            return "a sphere";
        case GeomAbs_Torus:
            return "a torus";
        case GeomAbs_BezierSurface:
            return "a Bezier surface";
        case GeomAbs_BSplineSurface:
            return "a B-spline surface";
        case GeomAbs_SurfaceOfRevolution:
            return "a surface of revolution";
        case GeomAbs_SurfaceOfExtrusion:
            return "a surface of extrusion";
        case GeomAbs_OffsetSurface:
            return "an offset surface";
        default:
            return "a freeform surface";
    }
}

bool isAnalyticCurve(GeomAbs_CurveType type)
{
    return analyticCurve(type);
}

bool isAnalyticSurface(GeomAbs_SurfaceType type)
{
    return analyticSurface(type);
}

void clearElementGeometryCache()
{
    std::lock_guard<std::mutex> lock(cacheMutex());
    curveCache().clear();
    surfaceCache().clear();
    conversions = 0;
}

int elementGeometryConversions()
{
    return conversions.load();
}

// ------------------------------------------------------------------------
// The element frame

namespace
{

Base::Rotation rotationOf(const gp_Ax2& ax)
{
    gp_Trsf trsf;
    trsf.SetTransformation(gp_Ax3(ax), gp_Ax3());
    const auto q = trsf.GetRotation();
    return Base::Rotation(q.X(), q.Y(), q.Z(), q.W());
}

Base::Vector3d toVector(const gp_XYZ& p)
{
    return Base::Vector3d(p.X(), p.Y(), p.Z());
}

double round10(double v)
{
    return std::round(v * 1e10) / 1e10;
}

/// The rotation Part.Plane(origin, normal) has: the one OCCT picks for an
/// axis with only its direction given. The direction is rounded first, as
/// UtilsAssembly.round_vector does, so that a direction a hair off an axis
/// gets the axis' frame.
Base::Rotation rotationOfNormal(const gp_Dir& dir)
{
    gp_Dir rounded(round10(dir.X()), round10(dir.Y()), round10(dir.Z()));
    return rotationOf(gp_Ax3(gp_Pnt(), rounded).Ax2());
}

/// A shape of one sub-shape is that sub-shape
TopoDS_Shape unwrap(TopoDS_Shape shape)
{
    while (!shape.IsNull() && shape.ShapeType() == TopAbs_COMPOUND) {
        TopoDS_Iterator it(shape);
        if (!it.More()) {
            break;
        }
        TopoDS_Shape child = it.Value();
        it.Next();
        if (it.More()) {
            break;
        }
        child.Move(shape.Location());
        shape = child;
    }
    return shape;
}

Base::Placement edgeFrame(const TopoDS_Edge& edge)
{
    Base::Placement plc;
    const auto curve = analyzeEdge(edge);

    // The origin
    if (curve.type == GeomAbs_Circle && !curve.curve.IsNull()) {
        auto circle = Handle(Geom_Circle)::DownCast(curve.curve);
        plc.setPosition(toVector(circle->Location().XYZ()));
    }
    else if (curve.type == GeomAbs_Line) {
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(edge, v1, v2);
        if (!v1.IsNull() && !v2.IsNull()) {
            plc.setPosition(
                (toVector(BRep_Tool::Pnt(v1).XYZ()) + toVector(BRep_Tool::Pnt(v2).XYZ())) * 0.5
            );
        }
    }
    else {
        gp_Pln pln;
        if (Part::TopoShape(edge).findPlane(pln)) {
            // Another planar curve, at its centre of mass, which is in its plane
            GProp_GProps props;
            BRepGProp::LinearProperties(edge, props);
            plc.setPosition(toVector(props.CentreOfMass().XYZ()));
        }
    }

    // The rotation
    if (curve.type == GeomAbs_Circle && !curve.curve.IsNull()) {
        auto circle = Handle(Geom_Circle)::DownCast(curve.curve);
        plc.setRotation(rotationOf(circle->Position()));
    }
    else if (curve.type == GeomAbs_Line) {
        BRepAdaptor_Curve adaptor(edge);
        plc.setRotation(rotationOfNormal(adaptor.Line().Direction()));
    }
    else {
        gp_Pln pln;
        if (Part::TopoShape(edge).findPlane(pln)) {
            plc.setRotation(rotationOf(pln.Position().Ax2()));
        }
    }
    return plc;
}

Base::Placement faceFrame(const TopoDS_Face& face)
{
    Base::Placement plc;
    const auto analyzed = analyzeFace(face);
    auto surface = Handle(Geom_ElementarySurface)::DownCast(analyzed.surface);

    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    const Base::Vector3d cog = toVector(props.CentreOfMass().XYZ());

    // The origin
    if (analyzed.type == GeomAbs_Cylinder && !surface.IsNull()) {
        // The point of the axis level with the centre of gravity
        const auto& pos = surface->Position();
        const Base::Vector3d center = toVector(pos.Location().XYZ());
        const Base::Vector3d axis = toVector(pos.Direction().XYZ());
        const Base::Vector3d rel = cog - center;
        plc.setPosition(center + axis * ((rel * axis) / axis.Sqr()));
    }
    else if ((analyzed.type == GeomAbs_Sphere || analyzed.type == GeomAbs_Torus)
             && !surface.IsNull()) {
        plc.setPosition(toVector(surface->Position().Location().XYZ()));
    }
    else if (analyzed.type == GeomAbs_Cone && !surface.IsNull()) {
        auto cone = Handle(Geom_ConicalSurface)::DownCast(surface);
        if (!cone.IsNull()) {
            plc.setPosition(toVector(cone->Apex().XYZ()));
        }
    }
    else {
        plc.setPosition(cog);
    }

    // The rotation: the surface's own frame, when it has one
    if (!surface.IsNull()) {
        plc.setRotation(rotationOf(surface->Position().Ax2()));
    }
    return plc;
}

}  // namespace

TopoDS_Shape singleSubShape(const TopoDS_Shape& shape)
{
    return unwrap(shape);
}

Base::Placement findElementFrame(const TopoShape& shape)
{
    if (shape.isNull()) {
        return {};
    }
    const TopoDS_Shape s = unwrap(shape.getShape());
    switch (s.ShapeType()) {
        case TopAbs_VERTEX:
            return Base::Placement(toVector(BRep_Tool::Pnt(TopoDS::Vertex(s)).XYZ()),
                                   Base::Rotation());
        case TopAbs_EDGE:
            return edgeFrame(TopoDS::Edge(s));
        case TopAbs_FACE:
            return faceFrame(TopoDS::Face(s));
        default:
            // A whole object (a datum, a body): its own frame
            return shape.getPlacement();
    }
}

}  // namespace Part
