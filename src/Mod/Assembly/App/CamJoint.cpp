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

#include <cstdio>
#include <sstream>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepLib_FindSurface.hxx>
#include <BRep_Tool.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Plane.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <App/DocumentObject.h>
#include <App/GeoFeature.h>
#include <App/PropertyLinks.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Mod/Part/App/PartFeature.h>

#include "AssemblyUtils.h"
#include "CamJoint.h"
#include "ElementGeometry.h"
#include "Joint.h"

using namespace Assembly;

namespace
{

gp_Trsf toTrsf(const Base::Placement& placement)
{
    double x {}, y {}, z {}, w {};
    placement.getRotation().getValue(x, y, z, w);
    const Base::Vector3d& p = placement.getPosition();
    gp_Trsf trsf;
    trsf.SetRotation(gp_Quaternion(x, y, z, w));
    trsf.SetTranslationPart(gp_Vec(p.x, p.y, p.z));
    return trsf;
}

Base::Vector3d toBase(const gp_XYZ& v)
{
    return Base::Vector3d(v.X(), v.Y(), v.Z());
}

// "%.17g", for the description.
void number(std::ostringstream& out, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    out << buf;
}

class OcctCurve: public Solver::CurveGeometry
{
public:
    OcctCurve(Handle(Geom_Curve) curve, double first, double last)
        : curve(std::move(curve))
        , u0(first)
        , u1(last)
    {
        const gp_Pnt a = this->curve->Value(u0);
        const gp_Pnt b = this->curve->Value(u1);
        isClosed = a.Distance(b) <= Precision::Confusion();
        findPlane();
        description = makeDescription();
    }

    double first() const override
    {
        return u0;
    }
    double last() const override
    {
        return u1;
    }
    bool closed() const override
    {
        return isClosed;
    }
    Solver::CurvePoint evaluate(double u) const override
    {
        gp_Pnt p;
        gp_Vec d1;
        gp_Vec d2;
        curve->D2(u, p, d1, d2);
        return {toBase(p.XYZ()), toBase(d1.XYZ()), toBase(d2.XYZ())};
    }
    Base::Vector3d planeNormal() const override
    {
        return normal;
    }
    std::string describe() const override
    {
        return description;
    }
    bool same(const CurveGeometry& other) const override
    {
        const auto* o = dynamic_cast<const OcctCurve*>(&other);
        return o && o->description == description;
    }

private:
    void findPlane()
    {
        try {
            const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(curve, u0, u1).Edge();
            BRepLib_FindSurface finder(edge, Precision::Confusion(), Standard_True);
            if (!finder.Found()) {
                return;
            }
            auto plane = Handle(Geom_Plane)::DownCast(finder.Surface());
            if (plane.IsNull()) {
                return;
            }
            gp_Dir n = plane->Axis().Direction();
            n.Transform(finder.Location().Transformation());
            normal = toBase(n.XYZ());
        }
        catch (const Standard_Failure&) {
            normal = Base::Vector3d();
        }
    }

    // One line: the kind, the range, closed or not, then the curve as the
    // B-spline GeomConvert makes of it (degree, periodic, knots with their
    // multiplicities, poles with their weights), every number to 17 digits.
    std::string makeDescription() const
    {
        std::ostringstream out;
        out << curve->DynamicType()->Name() << " range ";
        number(out, u0);
        out << ' ';
        number(out, u1);
        out << (isClosed ? " closed" : " open");
        Handle(Geom_BSplineCurve) spline;
        try {
            Handle(Geom_Curve) trimmed = curve;
            if (!curve->IsKind(STANDARD_TYPE(Geom_BSplineCurve)) || u0 != curve->FirstParameter()
                || u1 != curve->LastParameter()) {
                trimmed = new Geom_TrimmedCurve(curve, u0, u1);
            }
            spline = GeomConvert::CurveToBSplineCurve(trimmed);
        }
        catch (const Standard_Failure&) {
            spline.Nullify();
        }
        if (spline.IsNull()) {
            return out.str();
        }
        out << " bspline degree " << spline->Degree() << (spline->IsPeriodic() ? " periodic" : "")
            << " knots " << spline->NbKnots();
        for (int i = 1; i <= spline->NbKnots(); ++i) {
            out << ' ';
            number(out, spline->Knot(i));
            out << ':' << spline->Multiplicity(i);
        }
        out << " poles " << spline->NbPoles();
        for (int i = 1; i <= spline->NbPoles(); ++i) {
            const gp_Pnt p = spline->Pole(i);
            out << ' ';
            number(out, p.X());
            out << ' ';
            number(out, p.Y());
            out << ' ';
            number(out, p.Z());
            out << ' ';
            number(out, spline->Weight(i));
        }
        return out.str();
    }

    Handle(Geom_Curve) curve;
    double u0;
    double u1;
    bool isClosed = false;
    Base::Vector3d normal;
    std::string description;
};

// "the edge Edge3 of Cam".
std::string elementPhrase(const App::DocumentObject* joint, const char* prop)
{
    std::string name, label;
    describeJointSide(joint, prop, name, label);
    std::string res = "the element " + name;
    const std::string type = getElementTypeFromProp(joint, prop);
    if (type == "Edge") {
        res = "the edge " + name;
    }
    else if (type == "Vertex") {
        res = "the vertex " + name;
    }
    else if (type == "Face") {
        res = "the face " + name;
    }
    if (!label.empty()) {
        res += " of ";
        res += label;
    }
    return res;
}

bool hasPick(const App::DocumentObject* joint, const char* prop)
{
    if (int side = typedJointSide(joint, prop)) {
        return static_cast<const Joint*>(joint)->getBinding(side) != nullptr;
    }
    const auto* ref = joint->getPropertyByName<App::PropertyXLinkSub>(prop);
    return ref && ref->getValue() && !ref->getSubValues().empty();
}

// The cam's edge as it is in the cam part's frame: the element through the
// reference's path from the moving part, less the part's own placement (as
// AssemblyObject::handleOneSideOfJoint takes a JCS into the part's frame).
TopoDS_Edge camEdge(const App::DocumentObject* joint, Base::Placement* partGlobal = nullptr)
{
    if (auto typed = dynamic_cast<const Joint*>(joint)) {
        // The side's edge in the assembly's coordinate system, and the cam
        // part's placement in it (docs/AssemblyObjectModel.md sec 7.1)
        const auto side = typed->getSide(1);
        if (!side.isValid()) {
            return {};
        }
        const TopoDS_Shape s = Part::singleSubShape(side.shape.getShape());
        if (s.IsNull() || s.ShapeType() != TopAbs_EDGE) {
            return {};
        }
        if (partGlobal) {
            *partGlobal = side.placement;
        }
        return TopoDS::Edge(s);
    }
    auto* ref = joint->getPropertyByName<App::PropertyXLinkSub>("Reference1");
    if (!ref || !ref->getValue() || ref->getSubValues().empty()) {
        return {};
    }
    auto* part = ref->getValue();
    try {
        const auto shape = Part::Feature::getTopoShape(
            part,
            ref->getSubValues().front().c_str(),
            true
        );
        if (shape.isNull() || shape.getShape().ShapeType() != TopAbs_EDGE) {
            return {};
        }
        if (partGlobal) {
            *partGlobal = App::GeoFeature::getGlobalPlacement(part, const_cast<App::PropertyXLinkSub*>(ref));
        }
        return TopoDS::Edge(shape.getShape());
    }
    catch (const Standard_Failure&) {
        return {};
    }
    catch (const Base::Exception&) {
        return {};
    }
}

// The roller's radius when the follower is a circular edge; 0 for a vertex,
// -1 for anything else.
double followerRadius(const App::DocumentObject* joint)
{
    const std::string type = getElementTypeFromProp(joint, "Reference2");
    if (type == "Vertex") {
        return 0.0;
    }
    if (type != "Edge") {
        return -1.0;
    }
    const auto edge = analyzeEdge(
        getLinkedObjFromRef(const_cast<App::DocumentObject*>(joint), "Reference2"),
        getElementFromProp(joint, "Reference2")
    );
    if (auto circle = Handle(Geom_Circle)::DownCast(edge.curve)) {
        return circle->Radius();
    }
    return -1.0;
}

}  // namespace

std::shared_ptr<const Solver::CurveGeometry> Assembly::makeCurveGeometry(
    const Handle(Geom_Curve)& curve,
    double first,
    double last,
    const Base::Placement& placement
)
{
    if (curve.IsNull()) {
        return nullptr;
    }
    Handle(Geom_Curve) moved = curve;
    if (!placement.isIdentity()) {
        moved = Handle(Geom_Curve)::DownCast(curve->Transformed(toTrsf(placement)));
    }
    if (moved.IsNull()) {
        return nullptr;
    }
    return std::make_shared<OcctCurve>(moved, first, last);
}

std::shared_ptr<const Solver::CurveGeometry> Assembly::makeCurveGeometry(
    const TopoDS_Edge& edge,
    const Base::Placement& placement
)
{
    if (edge.IsNull()) {
        return nullptr;
    }
    TopLoc_Location location;
    double first = 0.0;
    double last = 0.0;
    Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, location, first, last);
    if (curve.IsNull()) {
        return nullptr;
    }
    if (!location.IsIdentity()) {
        curve = Handle(Geom_Curve)::DownCast(curve->Transformed(location.Transformation()));
    }
    return makeCurveGeometry(curve, first, last, placement);
}

std::string Assembly::camJointRefusal(const App::DocumentObject* joint)
{
    if (!joint) {
        return "there is no joint";
    }
    std::shared_ptr<const Solver::CurveGeometry> curve;
    if (hasPick(joint, "Reference1")) {
        if (getElementTypeFromProp(joint, "Reference1") != "Edge") {
            return elementPhrase(joint, "Reference1")
                + " is not an edge: the cam is a curve of the cam part (an edge)";
        }
        curve = makeCurveGeometry(camEdge(joint));
        if (!curve) {
            return elementPhrase(joint, "Reference1") + " has no curve";
        }
    }
    if (!hasPick(joint, "Reference2")) {
        return {};
    }
    const double radius = followerRadius(joint);
    if (radius < 0.0) {
        return elementPhrase(joint, "Reference2")
            + " is neither a vertex nor a circular edge: the follower is a point (a vertex) "
              "or a roller (a circle)";
    }
    if (!curve) {
        return {};
    }
    if (radius > 0.0 && curve->planeNormal().Length() < 0.5) {
        return elementPhrase(joint, "Reference1")
            + " does not lie in a plane: a roller follower needs a planar cam curve";
    }
    double worst = 0.0;
    Solver::curveReference(*curve, &worst);
    if (worst > 0.999) {
        return elementPhrase(joint, "Reference1")
            + " turns through every direction: no smooth normal frame along it";
    }
    return {};
}

bool Assembly::addCamCurve(
    App::DocumentObject* joint,
    Solver::Model& model,
    Solver::Joint& mbdJoint,
    const Base::Placement& camOffset
)
{
    if (const std::string why = camJointRefusal(joint); !why.empty()) {
        Base::Console().warning(
            "Assembly: the Cam joint %s is not solved: %s.\n",
            joint->getFullName(),
            why.c_str()
        );
        return false;
    }
    if (mbdJoint.frameI >= model.frames.size() || mbdJoint.frameJ >= model.frames.size()) {
        return false;
    }
    // The cam's edge, from its part's frame into frame I's: the frame is on
    // the solver body at 'local'; the part sits in the body at camOffset.
    Base::Placement partGlobal;
    const TopoDS_Edge edge = camEdge(joint, &partGlobal);
    const Solver::Frame& frameI = model.frames[mbdJoint.frameI];
    const Base::Placement toFrame = frameI.local.inverse() * camOffset * partGlobal.inverse();
    auto curve = makeCurveGeometry(edge, toFrame);
    if (!curve) {
        return false;
    }

    // A roller's centre on the curve's offset by its radius, on the side it
    // starts on (the sign of the second equation's e . N2 there).
    const double radius = followerRadius(joint);
    double offset = 0.0;
    if (radius > 0.0) {
        auto global = [&](const Solver::Frame& f) {
            if (f.body == Solver::NoId || f.body >= model.bodies.size()) {
                return f.local;
            }
            return model.bodies[f.body].placement * f.local;
        };
        const Base::Vector3d x =
            (global(frameI).inverse() * global(model.frames[mbdJoint.frameJ])).getPosition();
        const auto start = Solver::projectOnCurveGlobally(*curve, x);
        const auto rows = Solver::pointOnCurveRows(*curve, 0.0, x, start.u);
        offset = rows.value[1] < 0.0 ? -radius : radius;
    }

    Solver::Curve record;
    record.source = model.addSource(joint->getFullName());
    record.geometry = std::move(curve);
    mbdJoint.curve = model.addCurve(std::move(record));
    mbdJoint.value = offset;
    return true;
}

double Assembly::camFollowerSide(const Solver::Model& model, const Solver::Joint& joint)
{
    if (joint.value != 0.0) {
        return joint.value > 0.0 ? 1.0 : -1.0;
    }
    if (joint.curve >= model.curves.size() || !model.curves[joint.curve].geometry
        || joint.frameI >= model.frames.size() || joint.frameJ >= model.frames.size()) {
        return 1.0;
    }
    const Solver::CurveGeometry& curve = *model.curves[joint.curve].geometry;
    const Base::Vector3d plane = curve.planeNormal();
    // Outside a closed planar curve: N2 = T x N1, N1 the plane normal, points
    // out where the curve turns about N1 counterclockwise -- its area, about
    // N1, positive
    if (curve.closed() && plane.Length() > 0.0) {
        constexpr int samples = 256;
        Base::Vector3d area;
        Base::Vector3d last = curve.evaluate(curve.first()).p;
        for (int i = 1; i <= samples; ++i) {
            const double u = curve.first() + curve.period() * i / samples;
            const Base::Vector3d p = Solver::evaluateCurve(curve, u).p;
            area += last % p;
            last = p;
        }
        return area * curve.reference() >= 0.0 ? 1.0 : -1.0;
    }
    // Else the follower body's centre of mass, in frame I's coordinates
    auto global = [&](const Solver::Frame& f) {
        if (f.body == Solver::NoId || f.body >= model.bodies.size()) {
            return f.local;
        }
        return model.bodies[f.body].placement * f.local;
    };
    const Solver::Frame& frameJ = model.frames[joint.frameJ];
    Base::Vector3d centre;
    if (frameJ.body != Solver::NoId && frameJ.body < model.bodies.size()) {
        Base::Vector3d com;
        if (model.dynamics) {
            for (const auto& inertia : model.dynamics->inertias) {
                if (inertia.body == frameJ.body) {
                    com = inertia.com;
                    break;
                }
            }
        }
        model.bodies[frameJ.body].placement.multVec(com, centre);
    }
    global(model.frames[joint.frameI]).inverse().multVec(Base::Vector3d(centre), centre);
    const auto at = Solver::projectOnCurveGlobally(curve, centre);
    Base::Vector3d tangent = at.at.d1;
    tangent.Normalize();
    Base::Vector3d n1 = curve.reference() - tangent * (curve.reference() * tangent);
    n1.Normalize();
    return (centre - at.at.p) * (tangent % n1) >= 0.0 ? 1.0 : -1.0;
}
