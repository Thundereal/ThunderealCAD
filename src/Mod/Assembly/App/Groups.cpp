// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2024 Ondsel <development@ondsel.com>                     *
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


#include <App/Application.h>
#include <App/FeaturePythonPyImp.h>
#include <App/PropertyPythonObject.h>
#include <Base/Console.h>
#include <Base/Tools.h>

#include <algorithm>

#include <App/DocumentObjectExtension.h>

#include "Groups.h"
#include "Joint.h"
#include "BomGroupPy.h"
#include "JointGroupPy.h"
#include "SimulationGroupPy.h"
#include "SnapshotGroupPy.h"
#include "ViewGroupPy.h"

using namespace Assembly;


PROPERTY_SOURCE(Assembly::BomGroup, App::DocumentObjectGroup)
PROPERTY_SOURCE(Assembly::JointGroup, App::DocumentObjectGroup)
PROPERTY_SOURCE(Assembly::SimulationGroup, App::DocumentObjectGroup)
PROPERTY_SOURCE(Assembly::SnapshotGroup, App::DocumentObjectGroup)
PROPERTY_SOURCE(Assembly::ViewGroup, App::DocumentObjectGroup)

namespace
{
/// An assembly's group owns what it holds (docs/TransactionLogJournal.md sec 37.9)
std::vector<App::DocumentObject*> heldObjects(const App::DocumentObjectGroup& group)
{
    auto owned = group.App::DocumentObject::getOwnedObjects();
    for (auto obj : group.Group.getValues()) {
        if (obj)
            owned.push_back(obj);
    }
    return owned;
}

/// Pasted onto a live group of its type: the member of the UID it was
/// brought with, which follows the group
App::DocumentObject* heldCounterpart(const App::DocumentObjectGroup& group,
                                     const App::DocumentObject* owned,
                                     const App::DocumentObject* liveOwner,
                                     App::OwnedPaste& how)
{
    auto live = Base::freecad_dynamic_cast<const App::DocumentObjectGroup>(liveOwner);
    const auto& held = group.Group.getValues();
    if (!owned || !live || live->getTypeId() != group.getTypeId()
            || std::find(held.begin(), held.end(), owned) == held.end())
        return nullptr;
    how = App::OwnedPaste::FollowOwner;
    for (auto obj : live->Group.getValues()) {
        if (obj && obj->getObjectUid() == owned->getObjectUid())
            return obj;
    }
    return nullptr;
}
}  // namespace

std::vector<App::DocumentObject*> BomGroup::getOwnedObjects() const
{
    return heldObjects(*this);
}

App::DocumentObject* BomGroup::getOwnedCounterpart(const App::DocumentObject* owned,
                                                const App::DocumentObject* liveOwner,
                                                App::OwnedPaste& how) const
{
    if (auto there = heldCounterpart(*this, owned, liveOwner, how))
        return there;
    return App::DocumentObjectGroup::getOwnedCounterpart(owned, liveOwner, how);
}

std::vector<App::DocumentObject*> JointGroup::getOwnedObjects() const
{
    return heldObjects(*this);
}

App::DocumentObject* JointGroup::getOwnedCounterpart(const App::DocumentObject* owned,
                                                const App::DocumentObject* liveOwner,
                                                App::OwnedPaste& how) const
{
    if (auto there = heldCounterpart(*this, owned, liveOwner, how))
        return there;
    return App::DocumentObjectGroup::getOwnedCounterpart(owned, liveOwner, how);
}

std::vector<App::DocumentObject*> ViewGroup::getOwnedObjects() const
{
    return heldObjects(*this);
}

App::DocumentObject* ViewGroup::getOwnedCounterpart(const App::DocumentObject* owned,
                                                const App::DocumentObject* liveOwner,
                                                App::OwnedPaste& how) const
{
    if (auto there = heldCounterpart(*this, owned, liveOwner, how))
        return there;
    return App::DocumentObjectGroup::getOwnedCounterpart(owned, liveOwner, how);
}



PyObject* BomGroup::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new BomGroupPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}


PyObject* JointGroup::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new JointGroupPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

std::vector<App::DocumentObject*> JointGroup::getJoints()
{
    std::vector<App::DocumentObject*> joints = {};

    Base::PyGILStateLocker lock;
    for (auto joint : getObjects()) {
        if (!joint) {
            continue;
        }

        auto* prop = dynamic_cast<App::PropertyBool*>(joint->getPropertyByName("Suppressed"));
        if (!prop || prop->getValue()) {
            // Filter grounded joints and deactivated joints.
            continue;
        }

        if (joint->isDerivedFrom<Joint>()) {
            joints.push_back(joint);
            continue;
        }
        auto proxy = dynamic_cast<App::PropertyPythonObject*>(joint->getPropertyByName("Proxy"));
        if (proxy) {
            if (proxy->getValue().hasAttr("setJointConnectors")) {
                joints.push_back(joint);
            }
        }
    }

    return joints;
}


PyObject* SimulationGroup::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new SimulationGroupPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

PyObject* SnapshotGroup::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new SnapshotGroupPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}


PyObject* ViewGroup::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new ViewGroupPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
