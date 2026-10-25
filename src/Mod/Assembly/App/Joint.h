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

// The joint as a C++ object (docs/AssemblyObjectModel.md sec 7.1): each side
// is a Part::ElementLink, a child of the assembly, instead of the Python
// joint's Reference1/2, Offset1/2 and Detach1/2.

#pragma once

#include <string>

#include <App/DocumentObject.h>
#include <App/FeaturePython.h>
#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <App/SuppressibleExtension.h>
#include <Mod/Part/App/TopoShape.h>

#include <Mod/Assembly/AssemblyGlobal.h>

#include "ElementGeometry.h"

namespace Part
{
class ElementLink;
}

namespace Assembly
{

class AssemblyObject;


/// What one side of a joint resolves to
struct AssemblyExport JointSide
{
    /// The element link of the side
    Part::ElementLink* link = nullptr;
    /// The component that moves: a direct child of the assembly
    App::DocumentObject* component = nullptr;
    /// The element's geometry in the assembly's coordinate system
    Part::TopoShape shape;
    /// The joint coordinate system in the assembly's coordinate system,
    /// the offsets applied
    Base::Placement frame;

    /// A whole object with no shape (an empty link, a datum, an LCS): the
    /// frame is its placement
    bool wholeObject = false;

    bool isValid() const
    {
        return link && component && (wholeObject || !shape.isNull());
    }
    /// "Vertex", "Edge" or "Face" when the element is one, else empty
    std::string elementType() const;
};

class AssemblyExport Joint: public App::DocumentObject, public App::SuppressibleExtension
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::Joint);

public:
    Joint();

    /// The same enumeration, in the same order, as JointObject.JointTypes
    App::PropertyEnumeration JointType;
    /// The element link of each side, a child of the assembly. Its Offset
    /// is the side's offset: the picked vertex and the user's offset in one
    App::PropertyLink ElementLink1;
    App::PropertyLink ElementLink2;
    /// The joint coordinate systems relative to each side's component, as
    /// the Python joint's are relative to their object; output, for display
    App::PropertyPlacement Placement1;
    App::PropertyPlacement Placement2;

    App::PropertyAngle Angle;
    App::PropertyLength Distance;
    App::PropertyLength Distance2;

    App::PropertyBool EnableLengthMin;
    App::PropertyBool EnableLengthMax;
    App::PropertyBool EnableAngleMin;
    App::PropertyBool EnableAngleMax;
    App::PropertyLength LengthMin;
    App::PropertyLength LengthMax;
    App::PropertyAngle AngleMin;
    App::PropertyAngle AngleMax;

    static const char* JointTypeEnums[];

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void unsetupObject() override;
    PyObject* getPyObject() override;
    /// One that draws nothing: the markers come with JointPython's Python
    /// view provider. A joint the assembly claims must have one.
    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }

    /// The element link of side 1 or 2
    Part::ElementLink* getElementLink(int side) const;
    /// The component side 1 or 2 moves, a direct child of the assembly,
    /// without resolving the geometry
    App::DocumentObject* getComponent(int side) const;
    /// What side 1 or 2 resolves to now: the element through its component
    /// when that resolves, the element link's stored copy when not
    JointSide getSide(int side) const;
    /// The assembly holding the joint
    AssemblyObject* getAssembly() const;

    /// Make side 1 or 2 from a pick (makeSide()), deleting its old element
    /// link unless another joint uses it
    Part::ElementLink* setSide(int side,
                               App::DocumentObject* component,
                               const char* subname,
                               const char* vertex = nullptr,
                               bool local = false);
    /// Unset side 1 or 2, deleting its element link unless another joint
    /// uses it
    void clearSide(int side);

    /** Make a side of a joint from a pick (docs/AssemblyObjectModel.md sec 7.1)
     *
     * @param assembly: the assembly the joint is in
     * @param component: a direct child of the assembly
     * @param subname: the element in the component, as a selection gives it
     * @param vertex: optionally, a vertex placing the joint's origin on the
     *                element, as upstream picks it; it becomes the element
     *                link's Offset
     *
     * The element is declared in the component when it declares elements (an
     * App::Part, a Body, an assembly, or a link to one), reusing one that
     * names the same geometry; otherwise, or with \a local, it is a
     * convenience element in the assembly's own Elements group. A migration
     * passes \a local: it does not edit the components, which may be other
     * documents.
     */
    static Part::ElementLink* makeSide(AssemblyObject* assembly,
                                       App::DocumentObject* component,
                                       const char* subname,
                                       const char* vertex = nullptr,
                                       bool local = false);

    /// Move the copies that follow a component in the assembly's coordinate
    /// system by \a delta, as a solve moved the component: its element links
    /// and the convenience elements on it
    static void relocateCopies(AssemblyObject* assembly,
                               App::DocumentObject* component,
                               const Base::Placement& delta);

    /// Make a joint in the assembly's joint group from two picks, each a
    /// component, a subname and an optional vertex subname
    static Joint* make(AssemblyObject* assembly,
                       const char* type,
                       App::DocumentObject* component1,
                       const char* subname1,
                       const char* vertex1,
                       App::DocumentObject* component2,
                       const char* subname2,
                       const char* vertex2);
};

using JointPython = App::FeaturePythonT<Joint>;

}  // namespace Assembly
