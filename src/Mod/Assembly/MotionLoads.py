# SPDX-License-Identifier: LGPL-2.1-or-later
"""The loads on one component at one frame of a run, its free body
(docs/AssemblyDynamics.md, D4a step 2): what a stress analysis of that
component takes. Not upstream.

Everything is in the component's own coordinates -- those of its Placement,
the frame's pose undone -- and the model's units: mm, kg, s, so forces in
mN and torques in mN mm."""

import FreeCAD as App

import UtilsAssembly

__title__ = "Assembly Motion Loads"
__url__ = "https://www.freecad.org"


def _cross(a, b):
    return App.Vector(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)


def _tensor(inertia):
    """(xx, yy, zz, xy, xz, yz) as rows"""
    xx, yy, zz, xy, xz, yz = inertia
    return ((xx, xy, xz), (xy, yy, yz), (xz, yz, zz))


def _apply(tensor, v):
    return App.Vector(*(row[0] * v.x + row[1] * v.y + row[2] * v.z for row in tensor))


def _rotated(tensor, rotation):
    """R I R^T, I given along the axes R turns into the new ones"""
    m = rotation.toMatrix()
    r = ((m.A11, m.A12, m.A13), (m.A21, m.A22, m.A23), (m.A31, m.A32, m.A33))
    ri = [[sum(r[i][k] * tensor[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return tuple(
        tuple(sum(ri[i][k] * r[j][k] for k in range(3)) for j in range(3)) for i in range(3)
    )


def _same(a, b):
    """The same component: the object, or a link to it"""
    return a is not None and b is not None and a.getLinkedObject() == b.getLinkedObject()


def _grounded(assembly, component):
    joints = assembly.getGroup("Joints", False)
    return any(
        _same(getattr(j, "ObjectToGround", None), component)
        for j in (joints.Group if joints else [])
    )


def _channels(results, obj):
    """The run's channels of obj, by quantity"""
    return {c["Quantity"]: c for c in results["Channels"] if c["Object"] == obj}


def loadsOn(assembly, component, frame):
    """The free body of component at the run's frame: the loads on it, its
    inertia and the check that they balance.

    Returns a dict:
      Time, Frame;
      Loads: a list of dicts, each with Source (the joint, spring or
        bushing), Kind ("Joint", "Spring", "Bushing"), Point, Force,
        Torque (about Point), Reference (a joint's reference on this
        component, (object, [element]), else None) and Unique (False for a
        redundant joint's share);
      Mass (kg), CenterOfMass, Inertia (rows, about the centre of mass,
        kg mm^2), Gravity (mm/s^2), Acceleration (the centre's, mm/s^2),
        AngularVelocity (rad/s), AngularAcceleration (rad/s^2);
      Residual: (force, torque about the centre of mass), the loads with
        gravity less m a, and their moment less I alpha + omega x I omega:
        zero when nothing is missing;
      Missing: what acts on the component that the loads do not take yet
        (contact sets, torsion springs, applied loads).
    Raises ValueError where there is no run, no such frame, or no motion
    reported for the component."""
    results = assembly.getSimulationResults()
    times = results["Times"]
    if not times or not results["Channels"]:
        raise ValueError("the assembly has no run with its loads")
    if not 0 <= frame < len(times):
        raise ValueError("frame %d is not one of the run's %d" % (frame, len(times)))
    sim = results.get("Simulation")

    saved = UtilsAssembly.saveAssemblyPartsPlacements(assembly)
    try:
        assembly.updateForFrame(frame)
        return _freeBody(assembly, component, frame, results, sim)
    finally:
        UtilsAssembly.restoreAssemblyPartsPlacements(assembly, saved)


def _freeBody(assembly, component, frame, results, sim):
    here = component.Placement  # at the frame, in the assembly
    toLocal = here.inverse()
    turn = toLocal.Rotation
    toAssembly = assembly.getGlobalPlacement().inverse()

    loads = []
    missing = []
    joints = assembly.getGroup("Joints", False)
    for joint in joints.Group if joints else []:
        refs = [getattr(joint, "Reference1", None), getattr(joint, "Reference2", None)]
        side = None
        for k, ref in enumerate(refs):
            if ref and _same(UtilsAssembly.getMovingPart(ref), component):
                side = k
        if side is None:
            continue
        found = _channels(results, joint)
        if "Force" not in found:
            continue
        force = found["Force"]["Values"][frame]
        torque = found["Torque"]["Values"][frame]
        # On frame I's body, its first reference's part; the other takes
        # the opposite, about the same point
        sign = 1.0 if side == 0 else -1.0
        frameI = toAssembly * UtilsAssembly.getJcsGlobalPlc(joint.Placement1, joint.Reference1)
        loads.append(
            {
                "Source": joint,
                "Kind": "Joint",
                "Point": toLocal.multVec(frameI.Base),
                "Force": turn.multVec(force * sign),
                "Torque": turn.multVec(torque * sign),
                "Reference": refs[side],
                "Unique": found["Force"]["Unique"],
            }
        )

    physics = assembly.getGroup("Physics", False)
    for item in physics.Group if physics else []:
        found = _channels(results, item)
        if "SpringForce" in found:
            ends = [(item.Component1, item.Point1), (item.Component2, item.Point2)]
            mine = [k for k, (obj, _) in enumerate(ends) if _same(obj, component)]
            if not mine:
                continue
            points = [obj.Placement.multVec(p) if obj else p for obj, p in ends]
            k = mine[0]
            apart = points[k] - points[1 - k]
            if apart.Length > 0.0:
                apart.normalize()
            loads.append(
                {
                    "Source": item,
                    "Kind": "Spring",
                    "Point": toLocal.multVec(points[k]),
                    "Force": turn.multVec(apart * found["SpringForce"]["Values"][frame]),
                    "Torque": App.Vector(),
                    "Reference": None,
                    "Unique": True,
                }
            )
        elif "BushingForce" in found:
            own = _same(item.Component, component)
            if not own and not _same(getattr(item, "Other", None), component):
                continue
            sign = 1.0 if own else -1.0
            at = item.Component.Placement * item.Frame
            loads.append(
                {
                    "Source": item,
                    "Kind": "Bushing",
                    "Point": toLocal.multVec(at.Base),
                    "Force": turn.multVec(found["BushingForce"]["Values"][frame] * sign),
                    "Torque": turn.multVec(found["BushingTorque"]["Values"][frame] * sign),
                    "Reference": None,
                    "Unique": True,
                }
            )
        elif item.isDerivedFrom("Assembly::ContactSet"):
            if any(_same(o, component) for o in list(item.GroupA) + list(item.GroupB)):
                missing.append(item)
        elif item.isDerivedFrom("Assembly::TorsionSpring"):
            joint = item.Joint
            refs = [getattr(joint, "Reference1", None), getattr(joint, "Reference2", None)]
            if joint and any(r and _same(UtilsAssembly.getMovingPart(r), component) for r in refs):
                missing.append(item)
        elif item.isDerivedFrom("Assembly::AppliedLoad"):
            if _same(item.Component, component):
                missing.append(item)

    # The inertia, at the frame's pose
    props = None
    for entry in assembly.getMassProperties()["Components"]:
        if _same(entry["Object"], component):
            props = entry
    if props is None:
        raise ValueError("%s is not a component of the assembly" % component.Label)
    mass = props["Mass"]
    centre = props["CenterOfMass"]
    inertia = _tensor(props["Inertia"])

    motion = _channels(results, component)
    if "Acceleration" in motion:
        accel = motion["Acceleration"]["Values"][frame]
        spin = motion["AngularVelocity"]["Values"][frame]
        rate = motion["AngularAcceleration"]["Values"][frame]
    elif _grounded(assembly, component):
        accel = App.Vector()
        spin = App.Vector()
        rate = App.Vector()
    else:
        raise ValueError(
            "the run reports no motion of %s: it moves with others as one, "
            "or stands grounded" % component.Label
        )

    gravity = App.Vector()
    if sim is not None and getattr(sim, "GravityEnabled", True):
        gravity = toAssembly.Rotation.multVec(sim.Gravity)

    # The balance, in the assembly's axes at the frame
    force = gravity * mass - accel * mass
    moment = App.Vector()
    back = here.Rotation
    for load in loads:
        f = back.multVec(load["Force"])
        p = here.multVec(load["Point"])
        force += f
        moment += back.multVec(load["Torque"]) + _cross(p - centre, f)
    moment -= _apply(inertia, rate) + _cross(spin, _apply(inertia, spin))

    return {
        "Time": results["Times"][frame],
        "Frame": frame,
        "Loads": loads,
        "Mass": mass,
        "CenterOfMass": toLocal.multVec(centre),
        "Inertia": _rotated(inertia, turn),
        "Gravity": turn.multVec(gravity),
        "Acceleration": turn.multVec(accel),
        "AngularVelocity": turn.multVec(spin),
        "AngularAcceleration": turn.multVec(rate),
        "Residual": (turn.multVec(force), turn.multVec(moment)),
        "Missing": missing,
    }
