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

/** What the assembly's Physics group shows
 *
 * An item with an end on an element (a spring's ends, a load's point) holds
 * an element binding per end, a child of the assembly, as a joint holds its
 * sides (docs/AssemblyObjectModel.md sec 7.1): the helpers here make,
 * resolve and delete them for it.
 */
class AssemblyExport PhysicsItem: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::PhysicsItem);

public:
    PhysicsItem() = default;

    /// The assembly the item is in, which holds its element bindings
    App::Part* getContainer() const;

protected:
    /// The element binding a link property holds, or null
    static Part::ElementBinding* bindingOf(const App::PropertyLink& prop);
    /** The component an end is on: its binding's, or the free end's
     *
     * @param placement: optionally, the component's placement in the
     *                   assembly's coordinate system
     */
    App::DocumentObject* endComponent(const App::PropertyLink& binding,
                                      const App::PropertyLink& component,
                                      Base::Placement* placement) const;
    /// An element binding for an end picked as a joint's side is
    /// (Joint::makeSide()), owned by the item
    Part::ElementBinding*
    makeEndBinding(App::DocumentObject* component, const char* subname, const char* vertex);
    /** The frame of a bound end relative to its component now, the offsets
     * applied; a binding that does not resolve now keeps \a last
     *
     * @return false when the binding resolves to nothing
     */
    bool resolveEnd(Part::ElementBinding* binding,
                    const Base::Placement& last,
                    Base::Placement& local) const;
    /// Delete a binding the item no longer holds, unless another holds it
    void releaseBinding(Part::ElementBinding* binding);
    /// Set a bound end's component, as its binding says, while the item
    /// is not restoring: not in execute(), which would change the links of
    /// the graph it is recomputed in
    void syncComponent(const App::PropertyLink& binding, App::PropertyLink& component);
    /// Delete the bindings the item holds, as it goes
    void releaseBindings(std::initializer_list<const App::PropertyLink*> props);
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

/** An item at a frame on a component (an applied load, a bushing): an
 * element's frame, the offsets applied, when the item has an element binding,
 * owned as a spring owns its ends -- Component and Frame are then its output,
 * following the geometry at each recompute. Without one it is the free frame
 * they give.
 */
class AssemblyExport FramedItem: public PhysicsItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::FramedItem);

public:
    FramedItem();

    /// The component the frame is on; none: the ground
    App::PropertyLink Component;
    /// In Component's coordinates (the assembly's without it)
    App::PropertyPlacement Frame;
    /// The element binding of the frame, or none for a free frame
    App::PropertyLink Binding;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;
    void unsetupObject() override;
    PyObject* getPyObject() override;

    Part::ElementBinding* getBinding() const;
    /** The component the frame is on, null for the ground, and the frame in
     * its coordinates
     *
     * @param placement: optionally, the component's placement in the
     *                   assembly's coordinate system
     */
    App::DocumentObject* getEnd(Base::Placement& frame, Base::Placement* placement = nullptr) const;
    /// The frame in the assembly's coordinate system now
    Base::Placement getEndPlacement() const;
    /// Put the frame on an element picked as a joint's side is
    Part::ElementBinding*
    setEnd(App::DocumentObject* component, const char* subname, const char* vertex = nullptr);
    /// Make the frame a free one where it is, deleting the element binding
    void clearEnd();

private:
    void updateEditable();
};

/** A force or a torque applied to a component (docs/AssemblyDynamics.md
 * sec 5.2's AppliedLoad), in dynamic and static runs: Magnitude times the
 * Profile, a formula of time (empty: 1), along Direction -- in the
 * application frame's axes, which turn with the component, or the
 * assembly's. A force acts at the frame's origin. Its opposite acts on the
 * Reaction component, at where the origin was at the start, or on the
 * ground.
 */
class AssemblyExport AppliedLoad: public FramedItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::AppliedLoad);

public:
    AppliedLoad();

    App::PropertyEnumeration Kind;
    App::PropertyVector Direction;
    /// Frame: Direction in the frame's axes, turning with the component;
    /// Assembly: in the assembly's
    App::PropertyEnumeration DirectionAxes;
    /// A force (N) or a torque (N m), as Kind is
    App::PropertyQuantity Magnitude;
    /// The magnitude's factor over time, in the workbench's formula
    /// grammar (time, as a motion's); empty: 1
    App::PropertyString Profile;
    /// The component taking the opposite load; none: the ground
    App::PropertyLink Reaction;

    static const char* KindEnums[];
    static const char* DirectionAxesEnums[];

    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;
    PyObject* getPyObject() override;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderAppliedLoad";
    }

    bool isTorque() const
    {
        return Kind.getValue() == 1;
    }
    /// The direction in the assembly's coordinate system now, a unit vector
    /// (none when Direction is zero)
    Base::Vector3d getDirectionNow() const;

private:
    void updateUnit();
};

using AppliedLoadPython = App::FeaturePythonT<AppliedLoad>;

/** A bushing (docs/AssemblyDynamics.md sec 5.2): Component held to Other (the
 * ground without it) at the frame, by a spring and a damper along and about
 * each of the frame's axes, which turn with Component. It is free where the
 * run starts. On Component, in the frame's axes:
 * Preload - K s - C ds/dt and PreloadTorque - K_r r - C_r dr/dt, where s is
 * how far the frame has moved from where Other holds it and r its turn from
 * there, a rotation vector (small turns).
 *
 * The values are in the internal units: a stiffness in mN/mm (N/m), a
 * rotational one in mN mm per radian (1 N m per radian is 1e6), a damping in
 * mN s/mm (N s/m), a rotational one in mN mm s per radian.
 */
class AssemblyExport Bushing: public FramedItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::Bushing);

public:
    Bushing();

    /// The component holding Component; none: the ground
    App::PropertyLink Other;
    /// Along the frame's x, y and z, mN/mm (N/m)
    App::PropertyVector TranslationalStiffness;
    /// About the frame's x, y and z, mN mm per radian
    App::PropertyVector RotationalStiffness;
    /// Along the frame's x, y and z, mN s/mm (N s/m)
    App::PropertyVector TranslationalDamping;
    /// About the frame's x, y and z, mN mm s per radian
    App::PropertyVector RotationalDamping;
    /// The force on Component where the run starts, in the frame's axes, mN
    App::PropertyVector Preload;
    /// The torque on Component where the run starts, in the frame's axes,
    /// mN mm
    App::PropertyVector PreloadTorque;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderBushing";
    }
};

using BushingPython = App::FeaturePythonT<Bushing>;

/** A contact set (docs/AssemblyDynamics.md sec 5.2, 7; phase D2): the
 * components of GroupA touching those of GroupB, or, GroupB empty, those of
 * GroupA touching one another, by the impact law and friction:
 * normal  Fn = Stiffness d^Exponent + STEP(d, 0, 0, Penetration, MaxDamping)
 *         dd/dt, d the penetration, never pulling;
 * friction mu Fn against the sliding, mu rising from 0 at rest to
 *          StaticFriction at StictionVelocity and going to DynamicFriction at
 *          FrictionVelocity.
 * Each solid of a component touches as the sphere, box or cylinder it is,
 * else as its convex hull when it is convex.
 */
class AssemblyExport ContactSet: public PhysicsItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::ContactSet);

public:
    ContactSet();

    App::PropertyLinkList GroupA;
    /// Empty: the components of GroupA touch one another
    App::PropertyLinkList GroupB;
    /// N / mm^Exponent
    App::PropertyFloatConstraint Stiffness;
    App::PropertyFloatConstraint Exponent;
    /// The damping reached at Penetration, N s/mm
    App::PropertyFloatConstraint MaxDamping;
    App::PropertyLength Penetration;
    App::PropertyFloatConstraint StaticFriction;
    App::PropertyFloatConstraint DynamicFriction;
    App::PropertySpeed StictionVelocity;
    App::PropertySpeed FrictionVelocity;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderContactSet";
    }
};

using ContactSetPython = App::FeaturePythonT<ContactSet>;

}  // namespace Assembly
