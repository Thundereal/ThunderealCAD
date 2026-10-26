// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
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
#include <cstring>
#include <iterator>

#include <BRepAdaptor_Curve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_SphericalSurface.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Sphere.hxx>
#include <gp_Pln.hxx>
#include <Standard_Failure.hxx>


#include <App/Application.h>
#include <App/Datums.h>
#include <App/Document.h>
#include <App/ElementNamingUtils.h>
#include <App/DocumentObject.h>
#include <App/PropertyStandard.h>
#include <App/Link.h>

#include <Base/Placement.h>
#include <Base/Tools.h>
#include <Base/Interpreter.h>

#include <Mod/Part/App/DatumFeature.h>
#include <App/LinkArray.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/App/Body.h>

#include "AssemblyUtils.h"
#include "ElementGeometry.h"
#include "AssemblyObject.h"
#include "AssemblyLink.h"
#include "Joint.h"
#include <Mod/Part/App/FeatureElement.h>

#include "Groups.h"


namespace PartApp = Part;

// ======================================= Utils ======================================
namespace Assembly
{

bool isElementLinkGroup(const App::DocumentObject* obj)
{
    return obj && obj->isLinkGroup() && !obj->isDerivedFrom<App::LinkArray>();
}

int linkElementIndex(const App::DocumentObject* obj, App::Link** group)
{
    auto* elt = freecad_cast<const App::LinkElement*>(obj);
    auto* link = elt ? elt->getLinkGroup() : nullptr;
    if (group) {
        *group = link;
    }
    if (!link) {
        return -1;
    }
    const auto& elements = link->ElementList.getValues();
    auto it = std::find(elements.begin(), elements.end(), obj);
    return it == elements.end() ? -1 : static_cast<int>(it - elements.begin());
}

// Upstream reads a Suppressed property on the element. Here an element is
// suppressed by hiding it in its link group's VisibilityList
// (App::LinkArray::isElementSuppressed), which also leaves it out of the shape.
bool isSuppressedLinkElement(const App::DocumentObject* obj)
{
    App::Link* group = nullptr;
    int index = linkElementIndex(obj, &group);
    return index >= 0 && group->isElementVisible(std::to_string(index).c_str()) == 0;
}

void swapJCS(const App::DocumentObject* joint)
{
    if (!joint) {
        return;
    }
    if (auto typed = dynamic_cast<const Joint*>(joint)) {
        auto* j = const_cast<Joint*>(typed);
        auto* link1 = j->ElementLink1.getValue();
        j->ElementLink1.setValue(j->ElementLink2.getValue());
        j->ElementLink2.setValue(link1);
        return;
    }

    auto pPlc1 = joint->getPropertyByName<App::PropertyPlacement>("Placement1");
    auto pPlc2 = joint->getPropertyByName<App::PropertyPlacement>("Placement2");
    if (pPlc1 && pPlc2) {
        const auto temp = pPlc1->getValue();
        pPlc1->setValue(pPlc2->getValue());
        pPlc2->setValue(temp);
    }
    auto pRef1 = joint->getPropertyByName<App::PropertyXLinkSub>("Reference1");
    auto pRef2 = joint->getPropertyByName<App::PropertyXLinkSub>("Reference2");
    if (pRef1 && pRef2) {
        auto temp = pRef1->getValue();
        auto subs1 = pRef1->getSubValues();
        auto subs2 = pRef2->getSubValues();
        // setSubValues is protected here, and going through setValue is the
        // right thing anyway -- it is what recomputes the mapped element names.
        pRef1->setValue(pRef2->getValue(), std::move(subs2));
        pRef2->setValue(temp, std::move(subs1));
    }
}

// Not upstream (docs/AssemblySolverInterface.md sec 6.11): the element's
// type as ElementGeometry recognises it, so a B-spline that is a circle or a
// cylinder within the tolerance counts as one. An analytic element reads as
// before.
bool isEdgeType(const App::DocumentObject* obj, const std::string& elName, const GeomAbs_CurveType type)
{
    return analyzeEdge(obj, elName).type == type;
}

bool isFaceType(const App::DocumentObject* obj, const std::string& elName, const GeomAbs_SurfaceType type)
{
    return analyzeFace(obj, elName).type == type;
}

double getFaceRadius(const App::DocumentObject* obj, const std::string& elt)
{
    const auto face = analyzeFace(obj, elt);
    if (auto cyl = Handle(Geom_CylindricalSurface)::DownCast(face.surface)) {
        return cyl->Radius();
    }
    if (auto sphere = Handle(Geom_SphericalSurface)::DownCast(face.surface)) {
        return sphere->Radius();
    }
    return 0.0;
}

double getEdgeRadius(const App::DocumentObject* obj, const std::string& elt)
{
    const auto edge = analyzeEdge(obj, elt);
    if (auto circle = Handle(Geom_Circle)::DownCast(edge.curve)) {
        return circle->Radius();
    }
    return 0.0;
}

DistanceType getDistanceType(App::DocumentObject* joint, bool* swapped)
{
    if (swapped) {
        *swapped = false;
    }
    if (!joint) {
        return DistanceType::Other;
    }
    // Shadows ::swapJCS: the sides trade places for the classification below
    // and in the caller's solver model, not in the document.
    auto swapJCS = [swapped](const App::DocumentObject*) {
        if (swapped) {
            *swapped = !*swapped;
        }
    };

    const auto type1 = getElementTypeFromProp(joint, "Reference1");
    const auto type2 = getElementTypeFromProp(joint, "Reference2");
    auto elt1 = getElementFromProp(joint, "Reference1");
    auto elt2 = getElementFromProp(joint, "Reference2");
    auto* obj1 = getLinkedObjFromRef(joint, "Reference1");
    auto* obj2 = getLinkedObjFromRef(joint, "Reference2");

    if (type1 == "Vertex" && type2 == "Vertex") {
        return DistanceType::PointPoint;
    }
    else if (type1 == "Edge" && type2 == "Edge") {
        if (isEdgeType(obj1, elt1, GeomAbs_Line) || isEdgeType(obj2, elt2, GeomAbs_Line)) {
            if (!isEdgeType(obj1, elt1, GeomAbs_Line)) {
                swapJCS(joint);  // make sure that line is first if not 2 lines.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isEdgeType(obj2, elt2, GeomAbs_Line)) {
                return DistanceType::LineLine;
            }
            else if (isEdgeType(obj2, elt2, GeomAbs_Circle)) {
                return DistanceType::LineCircle;
            }
            // TODO : other cases Ellipse, parabola, hyperbola...
        }

        else if (isEdgeType(obj1, elt1, GeomAbs_Circle) || isEdgeType(obj2, elt2, GeomAbs_Circle)) {
            if (!isEdgeType(obj1, elt1, GeomAbs_Circle)) {
                swapJCS(joint);  // make sure that circle is first if not 2 lines.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isEdgeType(obj2, elt2, GeomAbs_Circle)) {
                return DistanceType::CircleCircle;
            }
            // TODO : other cases Ellipse, parabola, hyperbola...
        }
    }
    else if (type1 == "Face" && type2 == "Face") {
        if (isFaceType(obj1, elt1, GeomAbs_Plane) || isFaceType(obj2, elt2, GeomAbs_Plane)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Plane)) {
                swapJCS(joint);  // make sure plane is first if its not 2 planes.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Plane)) {
                return DistanceType::PlanePlane;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Cylinder)) {
                return DistanceType::PlaneCylinder;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::PlaneSphere;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Cone)) {
                return DistanceType::PlaneCone;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Torus)) {
                return DistanceType::PlaneTorus;
            }
        }

        else if (isFaceType(obj1, elt1, GeomAbs_Cylinder) || isFaceType(obj2, elt2, GeomAbs_Cylinder)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Cylinder)) {
                swapJCS(joint);  // make sure cylinder is first if its not 2 cylinders.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Cylinder)) {
                return DistanceType::CylinderCylinder;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::CylinderSphere;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Cone)) {
                return DistanceType::CylinderCone;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Torus)) {
                return DistanceType::CylinderTorus;
            }
        }

        else if (isFaceType(obj1, elt1, GeomAbs_Cone) || isFaceType(obj2, elt2, GeomAbs_Cone)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Cone)) {
                swapJCS(joint);  // make sure cone is first if its not 2 cones.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Cone)) {
                return DistanceType::ConeCone;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Torus)) {
                return DistanceType::ConeTorus;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::ConeSphere;
            }
        }

        else if (isFaceType(obj1, elt1, GeomAbs_Torus) || isFaceType(obj2, elt2, GeomAbs_Torus)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Torus)) {
                swapJCS(joint);  // make sure torus is first if its not 2 torus.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Torus)) {
                return DistanceType::TorusTorus;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::TorusSphere;
            }
        }

        else if (isFaceType(obj1, elt1, GeomAbs_Sphere) || isFaceType(obj2, elt2, GeomAbs_Sphere)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Sphere)) {
                swapJCS(joint);  // make sure sphere is first if its not 2 spheres.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::SphereSphere;
            }
        }
    }
    else if ((type1 == "Vertex" && type2 == "Face") || (type1 == "Face" && type2 == "Vertex")) {
        if (type1 == "Vertex") {  // Make sure face is the first.
            swapJCS(joint);
            std::swap(elt1, elt2);
            std::swap(obj1, obj2);
        }
        if (isFaceType(obj1, elt1, GeomAbs_Plane)) {
            return DistanceType::PointPlane;
        }
        else if (isFaceType(obj1, elt1, GeomAbs_Cylinder)) {
            return DistanceType::PointCylinder;
        }
        else if (isFaceType(obj1, elt1, GeomAbs_Sphere)) {
            return DistanceType::PointSphere;
        }
        else if (isFaceType(obj1, elt1, GeomAbs_Cone)) {
            return DistanceType::PointCone;
        }
        else if (isFaceType(obj1, elt1, GeomAbs_Torus)) {
            return DistanceType::PointTorus;
        }
    }
    else if ((type1 == "Edge" && type2 == "Face") || (type1 == "Face" && type2 == "Edge")) {
        if (type1 == "Edge") {  // Make sure face is the first.
            swapJCS(joint);
            std::swap(elt1, elt2);
            std::swap(obj1, obj2);
        }
        if (isEdgeType(obj2, elt2, GeomAbs_Line)) {
            if (isFaceType(obj1, elt1, GeomAbs_Plane)) {
                return DistanceType::LinePlane;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Cylinder)) {
                return DistanceType::LineCylinder;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Sphere)) {
                return DistanceType::LineSphere;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Cone)) {
                return DistanceType::LineCone;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Torus)) {
                return DistanceType::LineTorus;
            }
        }
        else {
            // For other curves we consider them as planes for now. Can be refined later.
            if (isFaceType(obj1, elt1, GeomAbs_Plane)) {
                return DistanceType::CurvePlane;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Cylinder)) {
                return DistanceType::CurveCylinder;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Sphere)) {
                return DistanceType::CurveSphere;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Cone)) {
                return DistanceType::CurveCone;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Torus)) {
                return DistanceType::CurveTorus;
            }
        }
    }
    else if ((type1 == "Vertex" && type2 == "Edge") || (type1 == "Edge" && type2 == "Vertex")) {
        if (type1 == "Vertex") {  // Make sure edge is the first.
            swapJCS(joint);
            std::swap(elt1, elt2);
            std::swap(obj1, obj2);
        }
        if (isEdgeType(obj1, elt1, GeomAbs_Line)) {  // Point on line joint.
            return DistanceType::PointLine;
        }
        else {
            // For other curves we do a point in plane-of-the-curve.
            // Maybe it would be best tangent / distance to the conic? For arcs and
            // circles we could use ASMTRevSphJoint. But is it better than pointInPlane?
            return DistanceType::PointCurve;
        }
    }
    return DistanceType::Other;
}

const char* distanceTypeName(DistanceType type)
{
    // In the enum's order.
    static const char* const names[] = {
        "PointPoint",    "LineLine",      "LineCircle",       "CircleCircle",  "PlanePlane",
        "PlaneCylinder", "PlaneSphere",   "PlaneCone",        "PlaneTorus",    "CylinderCylinder",
        "CylinderSphere", "CylinderCone", "CylinderTorus",    "ConeCone",      "ConeTorus",
        "ConeSphere",    "TorusTorus",    "TorusSphere",      "SphereSphere",  "PointPlane",
        "PointCylinder", "PointSphere",   "PointCone",        "PointTorus",    "LinePlane",
        "LineCylinder",  "LineSphere",    "LineCone",         "LineTorus",     "CurvePlane",
        "CurveCylinder", "CurveSphere",   "CurveCone",        "CurveTorus",    "PointLine",
        "PointCurve",    "Other",
    };
    static_assert(std::size(names) == static_cast<std::size_t>(DistanceType::Other) + 1);
    auto index = static_cast<std::size_t>(type);
    return index < std::size(names) ? names[index] : "Other";
}

const std::vector<DistanceType>& refusedDistanceTypes()
{
    // Since sec 6.11 only Other is refused whatever the elements are (and
    // only between two elements, see distanceJointRefusal). The cone, torus
    // and curve sub-types have a role mapping; distanceJointRefusal still
    // refuses some of them on the elements' geometry.
    static const std::vector<DistanceType> refused = {
        DistanceType::Other,
    };
    return refused;
}

bool isDistanceTypeRefused(DistanceType type)
{
    const auto& refused = refusedDistanceTypes();
    return std::find(refused.begin(), refused.end(), type) != refused.end();
}

namespace
{

bool isElementType(const std::string& t)
{
    return t == "Vertex" || t == "Edge" || t == "Face";
}

// "the face Face3 of Cone" (the label of the picked component).
std::string elementPhrase(App::DocumentObject* joint, const char* prop, const char* kind)
{
    std::string name, label;
    describeJointSide(joint, prop, name, label);
    std::string res = std::string("the ") + kind + " " + name;
    if (!label.empty()) {
        res += " of ";
        res += label;
    }
    return res;
}

std::string faceRefusal(App::DocumentObject* joint, const char* prop)
{
    auto* obj = getLinkedObjFromRef(joint, prop);
    const auto face = analyzeFace(obj, getElementFromProp(joint, prop));
    if (isAnalyticSurface(face.type)) {
        return {};
    }
    return elementPhrase(joint, prop, "face")
        + " is not planar, cylindrical, conical, spherical or toroidal: it is "
        + describeSurfaceType(face.type);
}

std::string edgeNotLineOrCircle(App::DocumentObject* joint, const char* prop)
{
    auto* obj = getLinkedObjFromRef(joint, prop);
    const auto edge = analyzeEdge(obj, getElementFromProp(joint, prop));
    if (edge.type == GeomAbs_Line || edge.type == GeomAbs_Circle) {
        return {};
    }
    return elementPhrase(joint, prop, "edge") + " is not a line or a circle: it is "
        + describeCurveType(edge.type);
}

std::string edgeNotPlanar(App::DocumentObject* joint, const char* prop)
{
    auto* obj = getLinkedObjFromRef(joint, prop);
    const std::string name = getElementFromProp(joint, prop);
    const auto edge = analyzeEdge(obj, name);
    if (edge.type == GeomAbs_Line || edge.type == GeomAbs_Circle || edge.type == GeomAbs_Ellipse) {
        return {};
    }
    // The plane findPlacement frames such a curve with (edge.findPlane()).
    auto* feature = dynamic_cast<PartApp::Feature*>(obj);
    if (feature) {
        try {
            PartApp::TopoShape sub(feature->Shape.getShape().getSubShape(name.c_str(), true));
            gp_Pln pln;
            if (!sub.isNull() && sub.findPlane(pln)) {
                return {};
            }
        }
        catch (const Standard_Failure&) {
        }
        catch (const Base::Exception&) {
        }
    }
    return elementPhrase(joint, prop, "edge") + " is not a line and not planar: it is "
        + describeCurveType(edge.type);
}

// The side (Reference1 or Reference2) whose element is of this type, the
// first when both are.
const char* sideOf(App::DocumentObject* joint, const char* elementType)
{
    if (getElementTypeFromProp(joint, "Reference1") == elementType) {
        return "Reference1";
    }
    if (getElementTypeFromProp(joint, "Reference2") == elementType) {
        return "Reference2";
    }
    return nullptr;
}

}  // namespace

std::string distanceJointRefusal(App::DocumentObject* joint, DistanceType type)
{
    if (!joint) {
        return {};
    }
    switch (type) {
        case DistanceType::Other: {
            const auto t1 = getElementTypeFromProp(joint, "Reference1");
            const auto t2 = getElementTypeFromProp(joint, "Reference2");
            if (!isElementType(t1) || !isElementType(t2)) {
                // A whole object on a side (a datum plane, an LCS, a body):
                // the planar distance between the objects' frames.
                return {};
            }
            // Between two elements, Other means an element that is not one
            // getDistanceType knows: a freeform face, or (between two edges)
            // an edge that is neither a line nor a circle.
            for (const char* prop : {"Reference1", "Reference2"}) {
                if (getElementTypeFromProp(joint, prop) == "Face") {
                    auto why = faceRefusal(joint, prop);
                    if (!why.empty()) {
                        return why;
                    }
                }
            }
            for (const char* prop : {"Reference1", "Reference2"}) {
                if (getElementTypeFromProp(joint, prop) == "Edge") {
                    auto why = edgeNotLineOrCircle(joint, prop);
                    if (!why.empty()) {
                        return why;
                    }
                }
            }
            return "no distance is defined between these elements";
        }
        case DistanceType::CurveCylinder:
        case DistanceType::CurveSphere:
        case DistanceType::CurveCone:
        case DistanceType::CurveTorus: {
            // The curve stands for its axis and radius: only a circle has them.
            const char* prop = sideOf(joint, "Edge");
            return prop ? edgeNotLineOrCircle(joint, prop) : std::string();
        }
        case DistanceType::CurvePlane:
        case DistanceType::PointCurve: {
            // The curve stands for its plane.
            const char* prop = sideOf(joint, "Edge");
            return prop ? edgeNotPlanar(joint, prop) : std::string();
        }
        default:
            return {};
    }
}

bool isDistanceJointRefused(App::DocumentObject* joint, DistanceType type)
{
    return !distanceJointRefusal(joint, type).empty();
}

const char* distanceMeaning(DistanceType type)
{
    switch (type) {
        case DistanceType::PlaneCone:
            return "the cone's axis parallel to the plane, d from it";
        case DistanceType::CylinderCone:
            return "the cone's axis parallel to the cylinder's, d from the cylinder";
        case DistanceType::ConeCone:
            return "the cones' axes parallel, d apart";
        case DistanceType::ConeTorus:
            return "the cone's and the torus's axes parallel, d apart";
        case DistanceType::ConeSphere:
            return "the sphere d from the cone's axis";
        case DistanceType::PointCone:
            return "the point d from the cone's axis";
        case DistanceType::PointTorus:
            return "the point d from the torus's axis";
        case DistanceType::LineCone:
            return "the line parallel to the cone's axis, d from it";
        case DistanceType::LineTorus:
            return "the line parallel to the torus's axis, d from it";
        case DistanceType::CurveCylinder:
            return "the circle's axis parallel to the cylinder's, the circle d from the cylinder";
        case DistanceType::CurveSphere:
            return "the sphere d from the circle's cylinder";
        case DistanceType::CurveCone:
            return "the circle's axis parallel to the cone's, the circle d from it";
        case DistanceType::CurveTorus:
            return "the circle's axis parallel to the torus's, the circle d from it";
        default:
            return nullptr;
    }
}

JointGroup* getJointGroup(const App::Part* part)
{
    if (!part) {
        return nullptr;
    }

    const auto* doc = part->getDocument();

    const auto jointGroups = doc->getObjectsOfType(JointGroup::getClassTypeId());
    if (jointGroups.empty()) {
        return nullptr;
    }
    for (auto jointGroup : jointGroups) {
        if (part->hasObject(jointGroup)) {
            return freecad_cast<JointGroup*>(jointGroup);
        }
    }
    return nullptr;
}

void setJointActivated(const App::DocumentObject* joint, bool val)
{
    if (!joint) {
        return;
    }

    if (auto propSuppressed = joint->getPropertyByName<App::PropertyBool>("Suppressed")) {
        propSuppressed->setValue(!val);
    }
}

bool getJointActivated(const App::DocumentObject* joint)
{
    if (!joint) {
        return false;
    }

    if (const auto propActivated = joint->getPropertyByName<App::PropertyBool>("Suppressed")) {
        return !propActivated->getValue();
    }
    return false;
}

double getJointDistance(const App::DocumentObject* joint, const char* propertyName)
{
    if (!joint) {
        return 0.0;
    }

    const auto* prop = joint->getPropertyByName<App::PropertyFloat>(propertyName);
    if (!prop) {
        return 0.0;
    }

    return prop->getValue();
}

double getJointAngle(const App::DocumentObject* joint)
{
    return getJointDistance(joint, "Angle");
}

double getJointDistance(const App::DocumentObject* joint)
{
    return getJointDistance(joint, "Distance");
}

double getJointDistance2(const App::DocumentObject* joint)
{
    return getJointDistance(joint, "Distance2");
}

JointType getJointType(const App::DocumentObject* joint)
{
    if (!joint) {
        return JointType::Fixed;
    }

    const auto* prop = joint->getPropertyByName<App::PropertyEnumeration>("JointType");
    if (!prop) {
        return JointType::Fixed;
    }

    return static_cast<JointType>(prop->getValue());
}

std::vector<std::string> getSubAsList(const App::PropertyXLinkSub* prop)
{
    if (!prop) {
        return {};
    }

    const auto subs = prop->getSubValues();
    if (subs.empty()) {
        return {};
    }

    return Base::Tools::splitSubName(subs[0]);
}

std::vector<std::string> getSubAsList(const App::DocumentObject* obj, const char* pName)
{
    if (!obj) {
        return {};
    }
    return getSubAsList(obj->getPropertyByName<App::PropertyXLinkSub>(pName));
}

std::string getElementFromProp(const App::DocumentObject* obj, const char* pName)
{
    if (!obj) {
        return "";
    }
    if (int side = typedJointSide(obj, pName)) {
        // The element link's copy is the element alone: its one vertex, edge
        // or face, named as the copy names it
        auto* link = static_cast<const Joint*>(obj)->getElementLink(side);
        if (!link) {
            return "";
        }
        JointSide jointSide;
        jointSide.shape = link->Shape.getShape();
        const auto type = jointSide.elementType();
        return type.empty() ? std::string() : type + "1";
    }

    const auto names = getSubAsList(obj, pName);
    if (names.empty()) {
        return "";
    }

    return names.back();
}

std::string getElementTypeFromProp(const App::DocumentObject* obj, const char* propName)
{
    // The prop is going to be something like 'Edge14' or 'Face7'. We need 'Edge' or 'Face'
    std::string elementType;
    for (const char ch : getElementFromProp(obj, propName)) {
        if (std::isalpha(ch)) {
            elementType += ch;
        }
    }
    return elementType;
}

App::DocumentObject* getObjFromProp(const App::DocumentObject* joint, const char* pName)
{
    if (!joint) {
        return {};
    }

    const auto* propObj = joint->getPropertyByName<App::PropertyLink>(pName);
    if (!propObj) {
        return {};
    }

    return propObj->getValue();
}

App::DocumentObject* getObjFromRef(App::DocumentObject* comp, const std::string& sub)
{
    if (!comp) {
        return nullptr;
    }

    const auto* doc = comp->getDocument();
    auto names = Base::Tools::splitSubName(sub);
    names.insert(names.begin(), comp->getNameInDocument());

    if (names.size() <= 2) {
        return comp;
    }

    // Lambda function to check if the typeId is a BodySubObject
    const auto isBodySubObject = [](App::DocumentObject* obj) -> bool {
        // PartDesign::Point + Line + Plane + CoordinateSystem
        // getViewProviderName instead of isDerivedFrom to avoid dependency on sketcher
        const auto isDerivedFromVpSketch
            = strcmp(obj->getViewProviderName(), "SketcherGui::ViewProviderSketch") == 0;
        return isDerivedFromVpSketch || obj->isDerivedFrom<PartApp::Datum>()
            || obj->isDerivedFrom<App::DatumElement>()
            || obj->isDerivedFrom<App::LocalCoordinateSystem>();
    };

    // Helper function to handle PartDesign::Body objects
    const auto handlePartDesignBody =
        [&](App::DocumentObject* obj,
            std::vector<std::string>::const_iterator it) -> App::DocumentObject* {
        auto nextIt = std::next(it);
        if (nextIt != names.end()) {
            for (auto* obji : obj->getOutList()) {
                if (*nextIt == obji->getNameInDocument() && isBodySubObject(obji)) {
                    // if obji is a LCS then perhaps we need to resolve one more level
                    if (auto* lcs = freecad_cast<App::LocalCoordinateSystem*>(obji)) {
                        nextIt = std::next(nextIt);
                        if (nextIt != names.end()) {
                            for (auto* objj : lcs->baseObjects()) {
                                if (*nextIt == objj->getNameInDocument()
                                    && objj->isDerivedFrom<App::DatumElement>()) {
                                    return objj;
                                }
                            }
                        }
                    }
                    return obji;
                }
            }
        }
        return obj;
    };


    for (auto it = names.begin(); it != names.end(); ++it) {
        App::DocumentObject* obj = doc->getObject(it->c_str());
        if (!obj) {
            return nullptr;
        }

        if (obj->isDerivedFrom<App::DocumentObjectGroup>()) {
            continue;
        }

        // The last but one name should be the selected
        if (std::next(it) == std::prev(names.end())) {
            return obj;
        }

        if (obj->isDerivedFrom<App::Part>() || isElementLinkGroup(obj)) {
            continue;
        }
        else if (obj->isDerivedFrom<PartDesign::Body>()) {
            return handlePartDesignBody(obj, it);
        }
        else if (obj->isDerivedFrom<App::LinkArray>() || obj->isDerivedFrom<PartApp::Feature>()) {
            // Primitive, fastener, gear, etc.
            return obj;
        }
        else if (obj->isLink()) {
            App::DocumentObject* linked_obj = obj->getLinkedObject();
            if (linked_obj->isDerivedFrom<PartDesign::Body>()) {
                auto* retObj = handlePartDesignBody(linked_obj, it);
                return retObj == linked_obj ? obj : retObj;
            }
            else if (linked_obj->isDerivedFrom<App::LinkArray>()) {
                return obj;
            }
            else if (linked_obj->isDerivedFrom<PartApp::Feature>()) {
                return obj;
            }
            else {
                doc = linked_obj->getDocument();
                continue;
            }
        }
    }

    return nullptr;
}

App::DocumentObject* getObjFromRef(const App::PropertyXLinkSub* prop)
{
    if (!prop) {
        return nullptr;
    }

    App::DocumentObject* obj = prop->getValue();
    if (!obj) {
        return nullptr;
    }

    const std::vector<std::string> subs = prop->getSubValues();
    if (subs.empty()) {
        return nullptr;
    }

    return getObjFromRef(obj, subs[0]);
}

App::DocumentObject* getObjFromJointRef(const App::DocumentObject* joint, const char* pName)
{
    if (!joint) {
        return nullptr;
    }
    if (int side = typedJointSide(joint, pName)) {
        // The object holding the element's geometry
        return static_cast<const Joint*>(joint)->getElementLink(side);
    }

    const auto* prop = joint->getPropertyByName<App::PropertyXLinkSub>(pName);
    return getObjFromRef(prop);
}

App::DocumentObject* getLinkedObjFromRef(const App::DocumentObject* joint, const char* pObj)
{
    if (!joint) {
        return nullptr;
    }

    if (const auto* obj = getObjFromJointRef(joint, pObj)) {
        return obj->getLinkedObject(true);
    }
    return nullptr;
}

App::DocumentObject* getMovingPartFromSel(
    const AssemblyObject* assemblyObject,
    App::DocumentObject* obj,
    const std::string& sub
)
{
    if (!obj) {
        return nullptr;
    }

    auto* doc = obj->getDocument();

    auto names = Base::Tools::splitSubName(sub);
    names.insert(names.begin(), obj->getNameInDocument());

    bool assemblyPassed = false;

    for (const auto& objName : names) {
        obj = doc->getObject(objName.c_str());
        if (!obj) {
            continue;
        }

        if (obj->isLink()) {  // update the document if necessary for next object
            doc = obj->getLinkedObject()->getDocument();
        }

        if (obj == assemblyObject) {
            // We make sure we pass the assembly for cases like part.assembly.part.body
            assemblyPassed = true;
            continue;
        }
        if (!assemblyPassed) {
            continue;
        }

        if (obj->isDerivedFrom<App::DocumentObjectGroup>()) {
            continue;  // we ignore groups.
        }

        if (isElementLinkGroup(obj)) {
            continue;
        }

        // We ignore dynamic sub-assemblies.
        if (obj->isDerivedFrom<Assembly::AssemblyLink>()) {
            const auto* pRigid = obj->getPropertyByName<App::PropertyBool>("Rigid");
            if (pRigid && !pRigid->getValue()) {
                continue;
            }
        }

        return obj;
    }
    return nullptr;
}

App::DocumentObject* getMovingPartFromRef(App::PropertyXLinkSub* prop)
{
    if (!prop) {
        return nullptr;
    }

    return prop->getValue();
}

App::DocumentObject* getMovingPartFromRef(App::DocumentObject* joint, const char* pName)
{
    if (!joint) {
        return nullptr;
    }
    if (int side = typedJointSide(joint, pName)) {
        return static_cast<const Joint*>(joint)->getComponent(side);
    }

    auto* prop = joint->getPropertyByName<App::PropertyXLinkSub>(pName);
    return getMovingPartFromRef(prop);
}

void describeJointSide(
    const App::DocumentObject* joint,
    const char* refName,
    std::string& element,
    std::string& label
)
{
    element = getElementFromProp(joint, refName);
    label.clear();
    int side = typedJointSide(joint, refName);
    if (!side) {
        if (auto* obj = getObjFromJointRef(joint, refName)) {
            label = obj->Label.getValue();
        }
        return;
    }
    auto* link = static_cast<const Joint*>(joint)->getElementLink(side);
    if (!link) {
        return;
    }
    // Down the elements to the geometry they name
    App::DocumentObject* obj = link->Element.getValue();
    const auto& subs = link->Element.getSubValues();
    std::string sub = subs.empty() ? std::string() : subs.front();
    for (int depth = 0; obj && depth < 16; ++depth) {
        auto* target = Base::freecad_dynamic_cast<Part::Element>(obj->getSubObject(sub.c_str()));
        if (!target || !target->Target.getValue()) {
            break;
        }
        obj = target->Target.getValue();
        const auto& tsubs = target->Target.getSubValues();
        sub = tsubs.empty() ? std::string() : tsubs.front();
    }
    if (!obj) {
        return;
    }
    // The object the element is of, at the end of the path
    std::string path = sub.substr(0, Data::findElementName(sub.c_str()) - sub.c_str());
    auto* owner = path.empty() ? obj : obj->getSubObject(path.c_str());
    label = (owner ? owner : obj)->Label.getValue();
    const std::string name = Data::oldElementName(Data::findElementName(sub.c_str()));
    if (!name.empty()) {
        element = name;
    }
}

int typedJointSide(const App::DocumentObject* joint, const char* refName)
{
    if (!refName || !dynamic_cast<const Joint*>(joint)) {
        return 0;
    }
    if (std::strcmp(refName, "Reference1") == 0) {
        return 1;
    }
    if (std::strcmp(refName, "Reference2") == 0) {
        return 2;
    }
    return 0;
}

void syncPlacements(App::DocumentObject* src, App::DocumentObject* to)
{
    auto* plcPropSource = dynamic_cast<App::PropertyPlacement*>(src->getPropertyByName("Placement"));
    auto* plcPropLink = dynamic_cast<App::PropertyPlacement*>(to->getPropertyByName("Placement"));

    if (plcPropSource && plcPropLink) {
        if (!plcPropSource->getValue().isSame(plcPropLink->getValue())) {
            plcPropLink->setValue(plcPropSource->getValue());
        }
    }
}
namespace
{
// Helper function to perform the recursive traversal. Kept in an anonymous
// namespace as it's an implementation detail of getAssemblyComponents.
void collectComponentsRecursively(
    const std::vector<App::DocumentObject*>& objects,
    std::vector<App::DocumentObject*>& results
)
{
    for (auto* obj : objects) {
        if (!obj || isSuppressedLinkElement(obj)) {
            continue;
        }

        if (auto* asmLink = freecad_cast<Assembly::AssemblyLink*>(obj)) {
            // If the sub-assembly is rigid, treat it as a single movable part.
            // If it's flexible, we need to check its individual components.
            if (asmLink->isRigid()) {
                results.push_back(asmLink);
            }
            else {
                collectComponentsRecursively(asmLink->Group.getValues(), results);
            }
            continue;
        }
        // A generated array is one component. It must come before the
        // link-group branch: here App::LinkArray is a link group, where
        // upstream's PartApp::LinkArray is not.
        else if (obj->isDerivedFrom<App::LinkArray>()) {
            results.push_back(obj);
            continue;
        }
        else if (obj->isLinkGroup()) {
            auto* linkGroup = static_cast<App::Link*>(obj);
            for (auto* elt : linkGroup->ElementList.getValues()) {
                if (!elt || isSuppressedLinkElement(elt)) {
                    continue;
                }
                results.push_back(elt);
            }
            continue;
        }
        else if (auto* group = freecad_cast<App::DocumentObjectGroup*>(obj)) {
            collectComponentsRecursively(group->Group.getValues(), results);
            continue;
        }
        else if (auto* link = freecad_cast<App::Link*>(obj)) {
            // A link to what is not loaded now (a closed or missing document)
            // keeps its place, and is solved on its element links' copies
            if (Part::Element::isUnloaded(link)) {
                results.push_back(link);
                continue;
            }
            obj = link->getLinkedObject();
            if (!obj) {
                continue;
            }
            if (obj->isDerivedFrom<App::LinkArray>()
                || (obj->isDerivedFrom<App::GeoFeature>()
                    && !obj->isDerivedFrom<App::LocalCoordinateSystem>())) {
                results.push_back(link);
            }
        }

        else if (
            obj->isDerivedFrom<App::GeoFeature>() && !obj->isDerivedFrom<App::LocalCoordinateSystem>()
        ) {
            results.push_back(obj);
        }
    }
}
}  // namespace

std::vector<App::DocumentObject*> getAssemblyComponents(const AssemblyObject* assembly)
{
    if (!assembly) {
        return {};
    }

    std::vector<App::DocumentObject*> components;
    collectComponentsRecursively(assembly->Group.getValues(), components);
    return components;
}

double getJointCurrentValue(App::DocumentObject* joint, bool isAngle)
{
    Base::Placement plc1 = App::GeoFeature::getPlacementFromProp(joint, "Placement1");
    Base::Placement plc2 = App::GeoFeature::getPlacementFromProp(joint, "Placement2");

    if (auto typed = dynamic_cast<const Joint*>(joint)) {
        // Both frames in the assembly's coordinate system
        plc1 = typed->getSide(1).frame;
        plc2 = typed->getSide(2).frame;
    }
    else {
        auto* ref1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
        auto* ref2 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
        if (!ref1 || !ref2) {
            return 0.0;
        }
        Base::Placement obj_global_plc1 = App::GeoFeature::getGlobalPlacement(nullptr, ref1);
        Base::Placement obj_global_plc2 = App::GeoFeature::getGlobalPlacement(nullptr, ref2);

        plc1 = obj_global_plc1 * plc1;
        plc2 = obj_global_plc2 * plc2;
    }

    Base::Placement plc3 = plc1.inverse() * plc2;

    if (isAngle) {
        Base::Vector3d x_axis = plc3.getRotation().multVec(Base::Vector3d(1, 0, 0));
        return std::atan2(x_axis.y, x_axis.x);
    }
    return (plc1.getPosition() - plc2.getPosition()).Length()
        * (plc3.getPosition().z < 0 ? -1.0 : 1.0);
}
}  // namespace Assembly
