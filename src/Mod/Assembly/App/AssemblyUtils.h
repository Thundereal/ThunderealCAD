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


#pragma once

#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <App/FeaturePython.h>
#include <App/Part.h>

namespace App
{
class DocumentObject;
class Link;
}  // namespace App

namespace Base
{
class Placement;
}  // namespace Base

namespace Assembly
{

/// Whether a generated link element is excluded from assembly operations.
bool isSuppressedLinkElement(const App::DocumentObject* obj);

/// The index of a link element in its link group, or -1; and the group.
int linkElementIndex(const App::DocumentObject* obj, App::Link** group = nullptr);

/// Whether obj is a link group whose elements are each a component. This
/// fork's generated arrays (App::LinkArray) are link groups too, but one
/// component each, as upstream's PartApp::LinkArray -- which is not a link
/// group -- is there.
bool isElementLinkGroup(const App::DocumentObject* obj);

// This enum has to be the same as the one in JointObject.py
enum class JointType
{
    Fixed,
    Revolute,
    Cylindrical,
    Slider,
    Ball,
    Distance,
    Parallel,
    Perpendicular,
    Angle,
    RackPinion,
    Screw,
    Gears,
    Belt,
    Cam,  // not upstream (CamJoint.h); appended, so older files keep their values
};

enum class GeometryType
{
    Point = 0,

    // Edges
    Line = 1,
    Curve = 2,
    Circle = 3,

    // Faces
    Place = 4,
    Cylinder = 5,
    Sphere = 6,
    Cone = 7,
    Torus = 8,
};

enum class DistanceType
{
    PointPoint,

    LineLine,
    LineCircle,
    CircleCircle,

    PlanePlane,
    PlaneCylinder,
    PlaneSphere,
    PlaneCone,
    PlaneTorus,
    CylinderCylinder,
    CylinderSphere,
    CylinderCone,
    CylinderTorus,
    ConeCone,
    ConeTorus,
    ConeSphere,
    TorusTorus,
    TorusSphere,
    SphereSphere,

    PointPlane,
    PointCylinder,
    PointSphere,
    PointCone,
    PointTorus,

    LinePlane,
    LineCylinder,
    LineSphere,
    LineCone,
    LineTorus,

    CurvePlane,
    CurveCylinder,
    CurveSphere,
    CurveCone,
    CurveTorus,

    PointLine,
    PointCurve,

    Other,
};

class AssemblyObject;
class JointGroup;

AssemblyExport void swapJCS(const App::DocumentObject* joint);

AssemblyExport bool isEdgeType(
    const App::DocumentObject* obj,
    const std::string& elName,
    const GeomAbs_CurveType type
);
AssemblyExport bool isFaceType(
    const App::DocumentObject* obj,
    const std::string& elName,
    const GeomAbs_SurfaceType type
);
AssemblyExport double getFaceRadius(const App::DocumentObject* obj, const std::string& elName);
AssemblyExport double getEdgeRadius(const App::DocumentObject* obj, const std::string& elName);

// The sub-type wants its sides in a set order (a line before a circle, a face
// before a vertex, ...). Upstream swaps the joint's sides in the document to
// get it; here the document is never written: *swapped, when given, says
// whether the solver's frame I is the Reference2 side.
AssemblyExport DistanceType getDistanceType(App::DocumentObject* joint, bool* swapped = nullptr);
// The enumerator's name ("PlaneCone"), for messages and Python.
AssemblyExport const char* distanceTypeName(DistanceType type);
// Audit sec 8.1 decision 7: the Distance sub-types with no proper solver
// mapping whatever the elements are. Since sec 6.11 (role mappings) only
// Other. The joint task refuses a new joint of one of these; a joint
// already in a document is still solved as Planar(d), with a warning. The
// one list; Python reads it through AssemblyApp.getRefusedDistanceTypes().
AssemblyExport const std::vector<DistanceType>& refusedDistanceTypes();
AssemblyExport bool isDistanceTypeRefused(DistanceType type);
// Why a Distance joint of this sub-type between the joint's elements is
// refused, naming the element and what it is ("the face Face3 of Loft is
// not planar, cylindrical, conical, spherical or toroidal: it is a B-spline
// surface"); empty when it is not. Refused: Other between two elements (not
// when a side is a whole object -- a datum plane, an LCS, a body -- which
// is a planar distance between the objects' frames); Curve{Cylinder,
// Sphere,Cone,Torus} on a curve that is not a circle; CurvePlane and
// PointCurve on a curve that is not planar (sec 6.11).
AssemblyExport std::string distanceJointRefusal(App::DocumentObject* joint, DistanceType type);
AssemblyExport bool isDistanceJointRefused(App::DocumentObject* joint, DistanceType type);
// What d means for the sub-types the role mappings of sec 6.11 added
// ("the point d from the cone's axis"); null for the others.
AssemblyExport const char* distanceMeaning(DistanceType type);
AssemblyExport JointGroup* getJointGroup(const App::Part* part);

AssemblyExport std::vector<App::DocumentObject*> getAssemblyComponents(const AssemblyObject* assembly);

// getters to get from properties
AssemblyExport void setJointActivated(const App::DocumentObject* joint, bool val);
AssemblyExport bool getJointActivated(const App::DocumentObject* joint);
AssemblyExport double getJointAngle(const App::DocumentObject* joint);
AssemblyExport double getJointDistance(const App::DocumentObject* joint);
AssemblyExport double getJointDistance2(const App::DocumentObject* joint);
AssemblyExport JointType getJointType(const App::DocumentObject* joint);
AssemblyExport std::string getElementFromProp(const App::DocumentObject* obj, const char* propName);
AssemblyExport std::string getElementTypeFromProp(const App::DocumentObject* obj, const char* propName);
AssemblyExport App::DocumentObject* getObjFromProp(
    const App::DocumentObject* joint,
    const char* propName
);
AssemblyExport App::DocumentObject* getObjFromRef(App::DocumentObject* obj, const std::string& sub);
AssemblyExport App::DocumentObject* getObjFromRef(const App::PropertyXLinkSub* prop);
AssemblyExport App::DocumentObject* getObjFromJointRef(
    const App::DocumentObject* joint,
    const char* propName
);
AssemblyExport App::DocumentObject* getLinkedObjFromRef(
    const App::DocumentObject* joint,
    const char* propName
);
// Get the moving part from a selection, which has the full path.
AssemblyExport App::DocumentObject* getMovingPartFromSel(
    const AssemblyObject* assemblyObject,
    App::DocumentObject* obj,
    const std::string& sub
);
AssemblyExport App::DocumentObject* getMovingPartFromRef(const App::PropertyXLinkSub* prop);
AssemblyExport App::DocumentObject* getMovingPartFromRef(App::DocumentObject* joint, const char* pName);
AssemblyExport std::vector<std::string> getSubAsList(const App::PropertyXLinkSub* prop);
AssemblyExport std::vector<std::string> getSubAsList(
    const App::DocumentObject* joint,
    const char* propName
);
AssemblyExport void syncPlacements(App::DocumentObject* src, App::DocumentObject* to);
AssemblyExport double getJointCurrentValue(App::DocumentObject* joint, bool isAngle);

}  // namespace Assembly
