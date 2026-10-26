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

#include <cstring>

#include <App/Document.h>
#include <App/ElementNamingUtils.h>
#include <App/GroupExtension.h>
#include <App/PropertyPythonObject.h>
#include <Base/Exception.h>
#include <Mod/Part/App/PartPyCXX.h>

#include "AssemblyGroup.h"
#include "AssemblyGroupPy.h"

using namespace Assembly;


PROPERTY_SOURCE(Assembly::AssemblyGroup, App::PartAuxGroup)

const char* AssemblyGroup::JointsRole = "Joints";
const char* AssemblyGroup::SimulationsRole = "Simulations";
const char* AssemblyGroup::ViewsRole = "Views";

AssemblyGroup::AssemblyGroup() = default;

PyObject* AssemblyGroup::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new AssemblyGroupPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

App::DocumentObject* AssemblyGroup::newObject(const char* type, const char* name)
{
    auto group = getOwnerGroup();
    if (!group) {
        THROWM(Base::RuntimeError, "The group has no owner");
    }
    return group->addObject(type, name);
}

void AssemblyGroup::addObject(App::DocumentObject* obj)
{
    auto group = getOwnerGroup();
    if (!group) {
        THROWM(Base::RuntimeError, "The group has no owner");
    }
    if (obj && !group->hasObject(obj)) {
        group->addObject(obj);
    }
}

void AssemblyGroup::onSettingDocument()
{
    PartAuxGroup::onSettingDocument();
    auto doc = getDocument();
    if (!doc) {
        return;
    }
    connProxy = doc->signalChangedObject.connect(
        [this](const App::DocumentObject& obj, const App::Property& prop) {
            // A proxy set decides the role of what has it. Not any Python
            // object property: each view provider property change touches
            // the object's ViewObject mirror, which is one too, and an
            // element link's style alone refreshed every group of the
            // assembly a dozen times per joint made
            if (!prop.isDerivedFrom<App::PropertyPythonObject>() || !prop.getName()
                || std::strcmp(prop.getName(), "Proxy") != 0) {
                return;
            }
            auto group = getOwnerGroup();
            if (group && group->hasObject(&obj)) {
                refresh();
            }
        }
    );
}


PROPERTY_SOURCE(Assembly::PartsGroup, App::PartAuxGroup)

PartsGroup::PartsGroup()
{
    ADD_PROPERTY_TYPE(
        Shape,
        (Part::TopoShape()),
        "Base",
        App::Prop_Output,
        "The components at their placements, kept while the assembly is frozen"
    );
}

App::DocumentObject* PartsGroup::getSubObject(
    const char* subname,
    PyObject** pyObj,
    Base::Matrix4D* mat,
    bool transform,
    int depth
) const
{
    while (subname && *subname == '.') {
        ++subname;
    }
    auto shape = Shape.getShape();
    // A name through a component goes to it
    if (shape.isNull()
        || (subname && !Data::isMappedElement(subname) && std::strchr(subname, '.'))) {
        return PartAuxGroup::getSubObject(subname, pyObj, mat, transform, depth);
    }
    if (pyObj) {
        if (subname && *subname) {
            shape = shape.getSubTopoShape(subname, true);
        }
        if (mat && !shape.isNull()) {
            shape.transformShape(*mat, false, true);
        }
        *pyObj = Py::new_reference_to(Part::shape2pyshape(shape));
    }
    return const_cast<PartsGroup*>(this);
}
