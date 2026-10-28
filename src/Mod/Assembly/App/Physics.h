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

// The assembly's Physics items (docs/AssemblyDynamics.md sec 5.2): the
// elements of the mechanism that studies share, shown in the assembly's
// Physics group (docs/AssemblyObjectModel.md sec 3).

#pragma once

#include <App/DocumentObject.h>
#include <App/FeaturePython.h>
#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace App
{
class Part;
}

namespace Part
{
class ElementBinding;
}

namespace Assembly
{

/// What the assembly's Physics group shows
class AssemblyExport PhysicsItem: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::PhysicsItem);

public:
    PhysicsItem() = default;
};

/** A component's mass properties given, in place of those from its shape and
 * material (MassProperties): its density; its mass, the centre and the
 * inertia still from its shape; or all of them.
 */
class AssemblyExport MassOverride: public PhysicsItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::MassOverride);

public:
    MassOverride();

    /// The component it gives the mass properties of
    App::PropertyLink Component;
    /// Density: the shape at this density; Mass: the shape, scaled to this
    /// mass; Full: this mass, centre of mass and inertia
    App::PropertyEnumeration Mode;
    App::PropertyDensity Density;
    App::PropertyMass Mass;
    /// Full: the centre of mass, in the component's coordinates
    App::PropertyVectorDistance CenterOfMass;
    /// Full: the inertia about the centre of mass, along the component's
    /// axes: xx, yy, zz, xy, xz, yz, kg mm^2 (xy is minus the integral of x
    /// y dm)
    App::PropertyFloatList Inertia;

    static const char* ModeEnums[];

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderMassOverride";
    }
};

using MassOverridePython = App::FeaturePythonT<MassOverride>;

/** A translational spring and damper between two points
 * (docs/AssemblyDynamics.md sec 5.2): each on a component, or on the ground
 * (no component: the point in the assembly's coordinates). Its force along
 * the line between them, pushing them apart when positive:
 * Preload - Stiffness (length - FreeLength) - Damping d(length)/dt.
 *
 * An end is on an element when it has an element binding: the origin of the
 * element's frame, the offsets applied, as a joint's side is
 * (docs/AssemblyObjectModel.md sec 7.1). The spring owns the binding, a
 * child of the assembly, as a joint owns its sides; the element it binds to
 * may be shared. The end's Component and Point are then its output, the
 * point following the geometry at each recompute. Without a binding the end
 * is the free point they give.
 */
class AssemblyExport SpringDamper: public PhysicsItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::SpringDamper);

public:
    SpringDamper();

    App::PropertyLink Component1;
    /// In Component1's coordinates, or the assembly's without it
    App::PropertyVectorDistance Point1;
    App::PropertyLink Component2;
    App::PropertyVectorDistance Point2;
    /// The element binding of each end, or none for a free point
    App::PropertyLink Binding1;
    App::PropertyLink Binding2;
    App::PropertyStiffness Stiffness;
    /// 0: the length when the run starts
    App::PropertyLength FreeLength;
    /// Pushing the ends apart
    App::PropertyForce Preload;
    /// A force over a speed, kg/s (N s / m)
    App::PropertyQuantity Damping;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;
    void unsetupObject() override;
    PyObject* getPyObject() override;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderSpringDamper";
    }

    /// The assembly the spring is in, which holds its element bindings
    App::Part* getContainer() const;
    /// The element binding of end 1 or 2, or null for a free point
    Part::ElementBinding* getBinding(int side) const;
    /** The component end 1 or 2 is on, null for the ground, and the end in
     * its coordinates (the assembly's for the ground)
     *
     * @param placement: optionally, the component's placement in the
     *                   assembly's coordinate system
     */
    App::DocumentObject*
    getEnd(int side, Base::Vector3d& point, Base::Placement* placement = nullptr) const;
    /// End 1 or 2 in the assembly's coordinate system now
    Base::Vector3d getEndPoint(int side) const;
    /** Put end 1 or 2 on an element picked as a joint's side is
     * (Joint::makeSide()), deleting its old element binding
     *
     * @return the new element binding
     */
    Part::ElementBinding* setEnd(int side,
                                 App::DocumentObject* component,
                                 const char* subname,
                                 const char* vertex = nullptr);
    /// Make end 1 or 2 a free point where it is, deleting its element binding
    void clearEnd(int side);

private:
    /// Component and Point are an end's output while it has a binding
    void updateEditable();
};

using SpringDamperPython = App::FeaturePythonT<SpringDamper>;

/** A torsion spring and damper on a revolute or cylindrical joint
 * (docs/AssemblyDynamics.md, D1's leftovers): a torque about the joint's
 * axis from the joint's turn -- the angle its limits read, the second
 * reference's x axis about the first's z. Turning the joint the positive
 * way when positive:
 * Preload - Stiffness (turn - FreeAngle) - Damping d(turn)/dt,
 * the turn counted from where the run starts.
 */
class AssemblyExport TorsionSpring: public PhysicsItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::TorsionSpring);

public:
    TorsionSpring();

    /// A Revolute or a Cylindrical joint
    App::PropertyLink Joint;
    /// A torque over an angle, mN mm per radian (N m per radian x 1e6)
    App::PropertyQuantity Stiffness;
    /// The joint's turn, from where the run starts, at which it is free
    App::PropertyAngle FreeAngle;
    /// A torque turning the joint the positive way
    App::PropertyQuantity Preload;
    /// A torque over a turning speed, mN mm s per radian
    App::PropertyQuantity Damping;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderTorsionSpring";
    }
};

using TorsionSpringPython = App::FeaturePythonT<TorsionSpring>;

}  // namespace Assembly
