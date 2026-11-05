# SPDX-License-Identifier: LGPL-2.1-or-later
"""The loads on one component at one frame of a run, its free body
(docs/AssemblyDynamics.md, D4a step 2): what a stress analysis of that
component takes. Not upstream.

Everything is in the component's own coordinates -- those of its Placement,
the frame's pose undone -- and the model's units: mm, kg, s, so forces in
mN and torques in mN mm."""

import math

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


def _rigidGroup(assembly, component):
    """component's rigid group as the run makes it (AssemblyObject's): the
    groups not suppressed, those sharing a part one; its Members, the
    Representative whose name the run reports its motion under (the least
    name), and the Groups it is made of. None when it is in none."""
    joints = assembly.getGroup("Joints", False)
    groups = [
        o
        for o in (joints.Group if joints else [])
        if hasattr(o, "ObjectsToRigidGroup") and getattr(o, "Suppressed", True) is False
    ]
    objects = {}  # by name
    parent = {}  # name: its parent's name

    def root(name):
        while parent[name] != name:
            name = parent[name]
        return name

    for g in groups:
        members = [m for m in g.ObjectsToRigidGroup if m is not None]
        for m in members:
            objects[m.Name] = m
            parent.setdefault(m.Name, m.Name)
        for m in members[1:]:
            a, b = root(members[0].Name), root(m.Name)
            if a != b:
                parent[b] = a
    if component.Name not in parent:
        return None
    mine = root(component.Name)
    members = [objects[n] for n in parent if root(n) == mine]
    if len(members) < 2:
        return None
    names = {m.Name for m in members}
    return {
        "Members": members,
        "Representative": min(members, key=lambda m: m.Name),
        "Groups": [g for g in groups if any(m and m.Name in names for m in g.ObjectsToRigidGroup)],
    }


def _meeting(local, others):
    """local's faces where it meets others: those sharing area with one,
    (index, the shared part); else those touching one, (index, the face).
    And whether they share area"""
    touching = []
    shared = []
    for i, face in enumerate(local.Faces, 1):
        for other in others:
            if face.distToShape(other)[0] > 1e-6:
                continue
            touching.append((i, face))
            common = face.common(other)
            if common.Area > 1e-9 * face.Area:
                shared.append((i, common))
            break
    return (shared, True) if shared else (touching, False)


def _groupFaces(component, members):
    """The faces of component where it meets the others of its rigid group,
    their centre, in its own coordinates, the round about it they take in
    and the gap to them: those sharing area with another's solid; else
    those touching one. Meeting them nowhere, where it would meet those
    nearest it -- within twice the least gap -- each moved across its gap
    to it: a part held over a clearance, on what faces it across it, not
    on a face along the edge nearest it, as near"""
    import Part

    local = Part.getShape(component, "", transform=False)
    toLocal = component.Placement.inverse()
    others = []
    for m in members:
        if m.Name == component.Name:
            continue
        shape = Part.getShape(m, "", transform=False)
        shape.transformShape((toLocal * m.Placement).toMatrix())
        others.append(shape)
    chosen, shared = _meeting(local, others)
    gap = 0.0
    if not chosen and others:
        nearest = [local.distToShape(other) for other in others]
        gap = min(d for d, _, _ in nearest)
        moved = []
        for other, (d, pairs, _) in zip(others, nearest):
            if d <= 2.0 * gap:
                a, b = pairs[0]
                moved.append(other.translated(a - b))
        chosen, shared = _meeting(local, moved)
    if not chosen:
        raise ValueError("%s has no faces" % component.Label)
    area = sum(s.Area for _, s in chosen)
    at = App.Vector()
    for _, s in chosen:
        at += s.CenterOfMass * s.Area
    at = at * (1.0 / area) if area > 0.0 else chosen[0][1].CenterOfMass
    # Where they share only part of the faces (an arm under a rod's end),
    # the round about their centre that takes it in; else 0, all of them
    radius = 0.0
    whole = sum(local.Faces[i - 1].Area for i, _ in chosen)
    if shared and area < 0.99 * whole:
        radius = max((v.Point - at).Length for _, s in chosen for v in s.Vertexes)
    return ["Face%d" % i for i, _ in chosen], at, radius, gap


def groupGap(assembly, component):
    """How far component stands from the rest of its rigid group, where
    it meets them nowhere: 0 where it does, None in no group"""
    group = _rigidGroup(assembly, component)
    if not group:
        return None
    return _groupFaces(component, group["Members"])[3]


def movingParts(assembly, results=None):
    """The parts whose motion the run reports, which loadsOn takes: each
    moving body's, and every part of a rigid group, whose motion is the
    group's"""
    results = results or assembly.getSimulationResults()
    parts = []
    for channel in results["Channels"]:
        obj = channel["Object"]
        if channel["Quantity"] != "Acceleration" or obj is None:
            continue
        group = _rigidGroup(assembly, obj)
        for part in sorted(group["Members"], key=lambda m: m.Name) if group else [obj]:
            if not any(_same(part, p) for p in parts):
                parts.append(part)
    return parts


def _runMark(assembly, results, held, component):
    """The run's channel a stress series keeps to know the run by: the held
    joint's force, or held by its rigid group, the group's acceleration;
    None when the run has neither"""
    found = _channels(results, held).get("Force")
    if found is None:
        group = _rigidGroup(assembly, component)
        body = group["Representative"] if group else component
        found = _channels(results, body).get("Acceleration")
    return found["Values"] if found else None


def _channels(results, obj):
    """The run's channels of obj, by quantity"""
    return {c["Quantity"]: c for c in results["Channels"] if c["Object"] == obj}


def loadsOn(assembly, component, frame):
    """The free body of component at the run's frame: the loads on it, its
    inertia and the check that they balance.

    Returns a dict:
      Component, Time, Frame;
      Loads: a list of dicts, each with Source (the joint, spring,
        bushing, contact set, torsion spring, applied load or rigid group),
        Kind ("Joint", "Spring", "Bushing", "Contact", "TorsionSpring",
        "Load", "Group"), Point, Force, Torque (about Point; a joint's its reaction with
        its motor's effort, a motion driving it), Reference (a joint's or a
        torsion spring's joint's reference on this component, (object,
        [element]), else None) and Unique (False for a redundant joint's
        share). A contact set's is its force on this part at the centre of
        its points on it, weighted by their forces, and the torque about
        that centre (all none while it does not press); a torsion spring's
        a torque about its joint's axis; an applied load's at its frame's
        origin, or on its reaction the opposite, where that origin was at
        the run's start. In a rigid group, the group's ("Group") is what
        the rest of it holds the part with, which the run does not split
        among its parts: what balances the others, on the faces where it
        meets them (Faces, their names), at their centre, on a Radius about
        it where they share part of them (else 0); meeting them nowhere,
        where it would across the Gap to them (else 0);
      Mass (kg), CenterOfMass, Inertia (rows, about the centre of mass,
        kg mm^2), Gravity (mm/s^2), Acceleration (the centre's -- in a
        rigid group, the group's motion at it -- mm/s^2),
        AngularVelocity (rad/s), AngularAcceleration (rad/s^2);
      Residual: (force, torque about the centre of mass), the loads with
        gravity less m a, and their moment less I alpha + omega x I omega:
        zero when nothing is missing;
      Missing: what acts on the component that the run does not report
        loads of (a contact set, torsion spring or applied load of a run
        kept from before they were reported).
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
        start = _reactionPoints(assembly, component)
        assembly.updateForFrame(frame)
        return _freeBody(assembly, component, frame, results, sim, start)
    finally:
        UtilsAssembly.restoreAssemblyPartsPlacements(assembly, saved)


def _reactionPoints(assembly, component):
    """Where each applied load component is the reaction of acts on it, in
    its own coordinates: where the load's frame was at the run's start (its
    first frame), by load name"""
    physics = assembly.getGroup("Physics", False)
    reacting = [
        item
        for item in (physics.Group if physics else [])
        if item.isDerivedFrom("Assembly::AppliedLoad")
        and _same(item.Reaction, component)
        and item.getEnd()[0]
    ]
    if not reacting:
        return {}
    assembly.updateForFrame(0)
    toLocal = component.Placement.inverse()
    return {item.Name: toLocal.multVec(item.getEndPlacement().Base) for item in reacting}


def _freeBody(assembly, component, frame, results, sim, start):
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
        frameI = toAssembly * UtilsAssembly.getJcsGlobalPlc(joint.Placement1, joint.Reference1)
        # A motion's motor on it, which its reaction does not hold: Chrono's,
        # minus its torque about zI and its force along zI on frame I's body
        # (the force at frame J's origin, on zI)
        zI = frameI.Rotation.multVec(App.Vector(0, 0, 1))
        if "MotorTorque" in found:
            torque = torque - zI * found["MotorTorque"]["Values"][frame]
        if "MotorForce" in found:
            force = force - zI * found["MotorForce"]["Values"][frame]
        # On frame I's body, its first reference's part; the other takes
        # the opposite, about the same point
        sign = 1.0 if side == 0 else -1.0
        loads.append(
            {
                "Source": joint,
                "Kind": "Joint",
                "Point": toLocal.multVec(frameI.Base),
                "Force": turn.multVec(force * sign),
                "Torque": turn.multVec(torque * sign),
                "Reference": refs[side],
                "Unique": found["Force"]["Unique"],
                # Its frame's axes, and whether a motion drives its turn or
                # its slide: what holding the part at it leaves free
                "Axes": turn * frameI.Rotation,
                "DrivenTurn": "MotorTorque" in found,
                "DrivenSlide": "MotorForce" in found,
            }
        )

    physics = assembly.getGroup("Physics", False)
    for item in physics.Group if physics else []:
        found = _channels(results, item)
        if "SpringForce" in found:
            # Its ends as the run took them: an end on an element is on the
            # component its binding resolves to
            ends = [item.getEnd(1)[0], item.getEnd(2)[0]]
            mine = [k for k, obj in enumerate(ends) if _same(obj, component)]
            if not mine:
                continue
            points = [item.getEndPoint(1), item.getEndPoint(2)]
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
            own = _same(item.getEnd()[0], component)
            if not own and not _same(getattr(item, "Other", None), component):
                continue
            sign = 1.0 if own else -1.0
            at = item.getEndPlacement()
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
            if not any(_same(o, component) for o in list(item.GroupA) + list(item.GroupB)):
                continue
            # Its force on this part at the centre of its points there, and
            # the torque about that centre: none while it does not press
            mine = {
                c["Quantity"]: c
                for c in results["Channels"]
                if c["Object"] == item
                and c["Quantity"].startswith("PartContact")
                and _same(c.get("On"), component)
            }
            if "PartContactForce" not in mine:
                missing.append(item)
                continue
            # At each place it presses the part, a load each, with how far
            # its points reach from its centre; a run kept from before the
            # places, all as one at their centre
            places = mine.get("PartContactPlaces")
            if places is None:
                loads.append(
                    {
                        "Source": item,
                        "Kind": "Contact",
                        "Point": toLocal.multVec(mine["PartContactPoint"]["Values"][frame]),
                        "Force": turn.multVec(mine["PartContactForce"]["Values"][frame]),
                        "Torque": turn.multVec(mine["PartContactTorque"]["Values"][frame]),
                        "Reference": None,
                        "Radius": 0.0,
                        "Unique": True,
                    }
                )
                continue
            row = places["Values"][frame]
            for k in range(0, len(row), 10):
                loads.append(
                    {
                        "Source": item,
                        "Kind": "Contact",
                        "Point": toLocal.multVec(App.Vector(*row[k + 3 : k + 6])),
                        "Force": turn.multVec(App.Vector(*row[k : k + 3])),
                        "Torque": turn.multVec(App.Vector(*row[k + 6 : k + 9])),
                        "Reference": None,
                        "Radius": row[k + 9],
                        "Unique": True,
                    }
                )
        elif item.isDerivedFrom("Assembly::TorsionSpring"):
            joint = item.Joint
            refs = [getattr(joint, "Reference1", None), getattr(joint, "Reference2", None)]
            side = None
            for k, ref in enumerate(refs):
                if ref and _same(UtilsAssembly.getMovingPart(ref), component):
                    side = k
            if not joint or side is None:
                continue
            if "SpringTorque" not in found:
                missing.append(item)
                continue
            # Chrono's: minus the torque about zI on frame I's body, plus on
            # frame J's
            frameI = toAssembly * UtilsAssembly.getJcsGlobalPlc(joint.Placement1, joint.Reference1)
            zI = frameI.Rotation.multVec(App.Vector(0, 0, 1))
            sign = -1.0 if side == 0 else 1.0
            loads.append(
                {
                    "Source": item,
                    "Kind": "TorsionSpring",
                    "Point": toLocal.multVec(frameI.Base),
                    "Force": App.Vector(),
                    "Torque": turn.multVec(zI * (sign * found["SpringTorque"]["Values"][frame])),
                    "Reference": refs[side],
                    "Unique": True,
                }
            )
        elif item.isDerivedFrom("Assembly::AppliedLoad"):
            own = _same(item.getEnd()[0], component)
            if not own and not _same(item.Reaction, component):
                continue
            quantity = "AppliedTorque" if item.Kind == "Torque" else "AppliedForce"
            if quantity not in found:
                missing.append(item)
                continue
            value = found[quantity]["Values"][frame]
            if own:
                at = toLocal.multVec(item.getEndPlacement().Base)
            else:
                # On its reaction, the opposite where the frame's origin
                # was at the start, which stays on the reaction there
                at = start[item.Name]
                value = value * -1.0
            torque = quantity == "AppliedTorque"
            loads.append(
                {
                    "Source": item,
                    "Kind": "Load",
                    "Point": at,
                    "Force": App.Vector() if torque else turn.multVec(value),
                    "Torque": turn.multVec(value) if torque else App.Vector(),
                    "Reference": None,
                    "Unique": True,
                }
            )

    # The inertia, at the frame's pose
    entries = assembly.getMassProperties()["Components"]
    props = None
    for entry in entries:
        if _same(entry["Object"], component):
            props = entry
    if props is None:
        raise ValueError("%s is not a component of the assembly" % component.Label)
    mass = props["Mass"]
    centre = props["CenterOfMass"]
    inertia = _tensor(props["Inertia"])

    # In a rigid group the run reports the group's motion, its body's, at
    # the group's centre of mass and under its representative's name: this
    # part's centre's acceleration from it
    group = _rigidGroup(assembly, component)
    members = group["Members"] if group else [component]
    motion = _channels(results, group["Representative"] if group else component)
    if "Acceleration" in motion:
        accel = motion["Acceleration"]["Values"][frame]
        spin = motion["AngularVelocity"]["Values"][frame]
        rate = motion["AngularAcceleration"]["Values"][frame]
        if group:
            total = 0.0
            middle = App.Vector()
            for entry in entries:
                if any(_same(entry["Object"], m) for m in members):
                    total += entry["Mass"]
                    middle += entry["CenterOfMass"] * entry["Mass"]
            r = centre - middle * (1.0 / total)
            accel = accel + _cross(rate, r) + _cross(spin, _cross(spin, r))
    elif any(_grounded(assembly, m) for m in members):
        accel = App.Vector()
        spin = App.Vector()
        rate = App.Vector()
    else:
        raise ValueError(
            "the run reports no motion of %s: it stands still, held by nothing "
            "that the run moves" % component.Label
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

    # What the rest of its rigid group holds it with: what balances it --
    # the run does not split the group's loads among its parts -- on the
    # faces where it meets the others, at their centre
    if group:
        faces, at, radius, gap = _groupFaces(component, members)
        p = here.multVec(at)
        f = -force
        t = -moment - _cross(p - centre, f)
        loads.append(
            {
                "Source": group["Groups"][0],
                "Kind": "Group",
                "Point": at,
                "Force": turn.multVec(f),
                "Torque": turn.multVec(t),
                "Reference": None,
                "Faces": faces,
                "Radius": radius,
                "Gap": gap,
                "Unique": True,
            }
        )
        force += f
        moment += t + _cross(p - centre, f)

    return {
        "Component": component,
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


# ---------------------------------------------------------------- to FEM


def _sameFace(a, b, tol=1e-6):
    return (
        abs(a.Area - b.Area) <= tol * max(a.Area, 1.0)
        and (a.CenterOfMass - b.CenterOfMass).Length <= tol * max(a.BoundBox.DiagonalLength, 1.0)
    )


def _facesOf(shape, element):
    """The names of shape's faces an element stands for: a face, itself; an
    edge, the faces along it sharing its axis -- a hole's bore for its rim
    -- or, none sharing it, every face along it; a vertex, the faces at
    it"""
    import Part

    faces = list(enumerate(shape.Faces, 1))
    if element.ShapeType == "Face":
        return ["Face%d" % i for i, f in faces if _sameFace(f, element)]
    touching = [(i, f) for i, f in faces if f.distToShape(element)[0] < 1e-6]
    if element.ShapeType == "Edge" and isinstance(element.Curve, (Part.Circle, Part.Line)):
        if isinstance(element.Curve, Part.Circle):
            axis, centre = element.Curve.Axis, element.Curve.Center
        else:
            axis, centre = element.Curve.Direction, element.Curve.Location
        coaxial = []
        for i, f in touching:
            surface = f.Surface
            if isinstance(surface, (Part.Cylinder, Part.Cone)):
                off = surface.Center - centre
                along = off - axis * off.dot(axis)
                if abs(abs(surface.Axis.dot(axis)) - 1.0) < 1e-9 and along.Length < 1e-6:
                    coaxial.append((i, f))
        if coaxial:
            touching = coaxial
    return ["Face%d" % i for i, _ in touching]


def _nearestFace(shape, point):
    import Part

    vertex = Part.Vertex(point)
    best = min(enumerate(shape.Faces, 1), key=lambda f: f[1].distToShape(vertex)[0])
    return ["Face%d" % best[0]]


def _jointOf(load):
    """The joint a load is at: a joint's own, a torsion spring's on it;
    else None"""
    if load["Kind"] in ("Joint", "Group"):
        return load["Source"]
    if load["Kind"] == "TorsionSpring":
        return load["Source"].Joint
    return None


def _heldLoad(body, heldJoint):
    """What the hold stands in for in a free body: the held joint's load and
    a torsion spring's on it, at the joint's point. (force, torque)"""
    force = App.Vector()
    torque = App.Vector()
    for load in body["Loads"]:
        if _jointOf(load) == heldJoint:
            force += load["Force"]
            torque += load["Torque"]
    return force, torque


# What a joint leaves free between its parts, along and about its frame's
# axes: (translations, rotations)
FREES = {"Revolute": ("", "Z"), "Cylindrical": ("Z", "Z"), "Slider": ("Z", ""), "Ball": ("", "XYZ")}


def _frees(load):
    """What holding the part at the held load's joint leaves free, along and
    about the joint's axes: (translations, rotations), its turn or slide
    held where a motion drives it. A rigid group's, and a joint of another
    kind, hold it stiff: ("", "")."""
    if load["Kind"] != "Joint":
        return "", ""
    moves, turns = FREES.get(getattr(load["Source"], "JointType", ""), ("", ""))
    if load.get("DrivenTurn"):
        turns = turns.replace("Z", "")
    if load.get("DrivenSlide"):
        moves = moves.replace("Z", "")
    return moves, turns


def _freeKind(moves, turns):
    """What the hold leaves free, for StressSeries.ModesFree: "Turn",
    "Slide", "TurnSlide", "Ball", or "" -- held stiff"""
    if turns == "XYZ":
        return "Ball"
    return ("Turn" if turns else "") + ("Slide" if moves else "")


def _holdAsJoint(objects, rb, held):
    """The hold rb of the analysis _build made, free as the held load's
    joint leaves it (_frees), its axes the joint's: ([(axis, point)], kind)
    -- the motions it leaves free, as _Modes takes them, and _freeKind's"""
    moves, turns = _frees(held)
    free = []
    if moves or turns:
        place = objects["Placement"]
        axes = place.Rotation * held["Axes"]
        rb.Axes = axes
        point = tuple(place.multVec(held["Point"]))
        unit = {"X": App.Vector(1, 0, 0), "Y": App.Vector(0, 1, 0), "Z": App.Vector(0, 0, 1)}
        for axis in moves:
            setattr(rb, "TranslationalMode" + axis, "Free")
            free.append((tuple(axes.multVec(unit[axis])), None))
        for axis in turns:
            setattr(rb, "RotationalMode" + axis, "Free")
            free.append((tuple(axes.multVec(unit[axis])), point))
    return free, _freeKind(moves, turns)


def _holdStiff(rb):
    """The hold rb held stiff, along the global axes, as _build made it:
    what holds the part is read off its nodes so"""
    rb.Axes = App.Rotation()
    for axis in "XYZ":
        setattr(rb, "TranslationalMode" + axis, "Constraint")
        setattr(rb, "RotationalMode" + axis, "Constraint")


def _heldJoint(bodies, hold):
    """The joint the part is held at over its free bodies: hold, or the one
    with the largest force at any of them -- a rigid group the part is in
    holding it too, where it meets the others"""
    largest = []  # (joint, its largest force), joints in order
    for body in bodies:
        for load in body["Loads"]:
            if load["Kind"] not in ("Joint", "Group"):
                continue
            for entry in largest:
                if entry[0] == load["Source"]:
                    entry[1] = max(entry[1], load["Force"].Length)
                    break
            else:
                largest.append([load["Source"], load["Force"].Length])
    component = bodies[0]["Component"]
    if not largest:
        raise ValueError("%s is on no joint to hold it by" % component.Label)
    if hold is None:
        return max(largest, key=lambda e: e[1])[0]
    if not any(e[0] == hold for e in largest):
        raise ValueError("%s does not hold %s" % (hold.Label, component.Label))
    return hold


def _build(assembly, component, body, heldJoint, maxSize, when):
    """The analysis' objects for component's free body: the copy of its
    shape where it stands now, the analysis, its solver, material and mesh,
    a coupling per load -- the held joint's rigid and fixed -- and the body
    motion load; their values set by _setFrame. A dict of them."""
    import Part
    import ObjectsFem

    doc = assembly.Document
    # Where it stands now, in the document -- at rest, or at the frame a
    # player shows: the loads are in its own coordinates
    place = component.getGlobalPlacement()
    local = Part.getShape(component, "", transform=False)
    copy = doc.addObject("Part::Feature", "LoadsAt")
    copy.Shape = local
    copy.Placement = place
    copy.Label = "%s %s" % (component.Label, when)

    analysis = ObjectsFem.makeAnalysis(doc, "LoadsAnalysis")
    analysis.Label = "Loads on %s %s" % (component.Label, when)
    solver = ObjectsFem.makeSolverCalculiXCcxTools(doc, "LoadsSolver")
    solver.AnalysisType = "static"
    analysis.addObject(solver)

    props = [
        e for e in assembly.getMassProperties()["Components"] if _same(e["Object"], component)
    ][0]
    material = ObjectsFem.makeMaterialSolid(doc, "LoadsMaterial")
    card = material.Material
    card["Name"] = props["Material"] or "Material"
    card["YoungsModulus"] = "%.12g kPa" % props["YoungsModulus"]
    card["PoissonRatio"] = "%.12g" % props["PoissonRatio"]
    # The run's mass over the shape's volume: an override's mass too
    card["Density"] = "%.12g kg/m^3" % (props["Mass"] / props["Volume"] * 1e9)
    material.Material = card
    analysis.addObject(material)

    mesh = ObjectsFem.makeMeshNetgenLegacy(doc, "LoadsMesh")
    mesh.Shape = copy
    mesh.MaxSize = maxSize or local.BoundBox.DiagonalLength / 40.0
    mesh.SecondOrder = True
    mesh.Fineness = 2
    analysis.addObject(mesh)

    # A coupling per load, but a torsion spring's is its joint's: on the
    # same faces, about the same point -- and held with it
    couplings = []
    byJoint = {}
    for load in body["Loads"]:
        joint = _jointOf(load)
        if joint is not None and joint.Name in byJoint:
            couplings.append(byJoint[joint.Name])
            continue
        # A rigid group's, its faces where it meets the others
        faces = list(load.get("Faces") or [])
        ref = load["Reference"]
        if ref and not faces:
            sub = ref[1][0] if ref[1] else ""
            element = Part.getShape(ref[0], sub, needSubElement=True, transform=False)
            if not element.isNull():
                faces = _facesOf(local, element)
        atPoint = not faces
        if atPoint:
            faces = _nearestFace(local, load["Point"])
        held = joint is not None and joint == heldJoint
        rb = ObjectsFem.makeConstraintRigidBody(doc, "Hold" if held else "Load")
        rb.Label = ("Held at %s" if held else "%s") % (joint or load["Source"]).Label
        rb.References = [(copy, tuple(faces))]
        rb.ReferenceNode = place.multVec(load["Point"])
        # A rigid group's on the part of its faces it shares, held or not
        if load["Kind"] == "Group" and load.get("Radius"):
            rb.CouplingRadius = load["Radius"]
        if held:
            rb.Coupling = "Rigid"
            for axis in "XYZ":
                setattr(rb, "TranslationalMode" + axis, "Constraint")
                setattr(rb, "RotationalMode" + axis, "Constraint")
        else:
            rb.Coupling = "Distributing"
            for axis in "XYZ":
                setattr(rb, "TranslationalMode" + axis, "Load")
                setattr(rb, "RotationalMode" + axis, "Load")
            # A load at a point of a face (a contact's, a spring's end, an
            # applied load's) on a patch of it a mesh size round: on the
            # whole face it was spread along it, and a push at a lever's
            # middle bent it as a load along all its underside would
            if atPoint:
                rb.CouplingRadius = mesh.MaxSize
        analysis.addObject(rb)
        couplings.append(rb)
        if joint is not None:
            byJoint[joint.Name] = rb

    motion = ObjectsFem.makeConstraintBodyMotion(doc, "LoadsMotion")
    analysis.addObject(motion)
    return {
        "Analysis": analysis,
        "Copy": copy,
        "Solver": solver,
        "Mesh": mesh,
        "Couplings": couplings,
        "Motion": motion,
        "Placement": place,
        "Shape": local,
        "Sources": [load["Source"] for load in body["Loads"]],
    }


def _pressing(load):
    return load["Force"].Length > 0.0 or load["Torque"].Length > 0.0


def _placeContact(objects, rb, load):
    """A contact's coupling where its load presses: at its point, on the
    face nearest it"""
    rb.ReferenceNode = objects["Placement"].multVec(load["Point"])
    faces = tuple(_nearestFace(objects["Shape"], load["Point"]))
    if tuple(rb.References[0][1]) != faces:
        rb.References = [(objects["Copy"], faces)]
    # On the patch its points reach over, and a mesh size round: a box
    # resting flat on its whole underside, a ball on a dot
    size = getattr(objects["Mesh"].MaxSize, "Value", objects["Mesh"].MaxSize)
    rb.CouplingRadius = load.get("Radius", 0.0) + size


def _loadTotals(objects, body, heldJoint, about=None):
    """Each coupling's force and torque at a free body, the held joint's
    left out, in the part's axes (mN, mN mm): {name: (coupling, force,
    torque)}. With about, {name: point in the part's coordinates}, each
    torque is about its coupling's point there, not its load's own."""
    sources = [load["Source"] for load in body["Loads"]]
    if len(sources) != len(objects["Sources"]) or any(
        a != b for a, b in zip(sources, objects["Sources"])
    ):
        raise ValueError("the loads on %s differ between frames" % body["Component"].Label)
    totals = {}
    for load, rb in zip(body["Loads"], objects["Couplings"]):
        if _jointOf(load) == heldJoint:
            continue
        torque = load["Torque"]
        if about is not None:
            torque = torque + (load["Point"] - about[rb.Name]).cross(load["Force"])
        _, f, t = totals.get(rb.Name, (rb, App.Vector(), App.Vector()))
        totals[rb.Name] = (rb, f + load["Force"], t + torque)
    return totals


def _setFrame(objects, body, heldJoint):
    """One free body's values into the analysis _build made: each load's
    force and torque, the body motion; a contact's coupling where it
    presses"""
    for load, rb in zip(body["Loads"], objects["Couplings"]):
        if _jointOf(load) == heldJoint or load["Kind"] != "Contact":
            continue
        # A contact's point moves over the part: its coupling with it, on
        # the face nearest it; where it does not press, as it was. A place
        # a contact does not press at this frame is left out: its coupling
        # stood where it last was, or on the face nearest the origin -- the
        # held face's nodes too, and eight of them there at a lever's first
        # frame put its hold 0.04 N off
        rb.Suppressed = not _pressing(load)
        if _pressing(load):
            _placeContact(objects, rb, load)
    _setValues(objects, body, heldJoint)


def _setValues(objects, body, heldJoint, about=None):
    """A free body's forces and torques into the couplings where they stand
    (about: _loadTotals'), and its motion into the body motion load"""
    turn = objects["Placement"].Rotation
    for rb, force, torque in _loadTotals(objects, body, heldJoint, about).values():
        force = turn.multVec(force) * 1e-3  # mN to N
        torque = turn.multVec(torque) * 1e-3  # mN mm to N mm
        for axis, f, t in zip("XYZ", force, torque):
            setattr(rb, "Force" + axis, "%.12g N" % f)
            setattr(rb, "Moment" + axis, "%.12g N*mm" % t)
    place = objects["Placement"]
    motion = objects["Motion"]
    motion.ReferencePoint = place.multVec(body["CenterOfMass"])
    motion.Gravity = turn.multVec(body["Gravity"])
    motion.LinearAcceleration = turn.multVec(body["Acceleration"])
    motion.AngularVelocity = turn.multVec(body["AngularVelocity"])
    motion.AngularAcceleration = turn.multVec(body["AngularAcceleration"])


def makeAnalysis(assembly, component, frame, hold=None, maxSize=None):
    """A static stress analysis of component at the run's frame, from its
    free body (loadsOn): a copy of its shape where it stands now, its
    material (its density the one its run's mass is of), a Netgen mesh,
    each load through a distributing coupling on its faces, the body motion
    load, and the part held at one joint -- hold, or the one with the
    largest force -- through a rigid coupling, that joint's own load left
    out: what holds it there is the check, against the run's (sec D4a).

    Returns (analysis, free body, held load). Loads the free body says are
    missing are not in it."""
    body = loadsOn(assembly, component, frame)
    heldJoint = _heldJoint([body], hold)
    held = [l for l in body["Loads"] if l["Source"] == heldJoint][0]
    objects = _build(assembly, component, body, heldJoint, maxSize, "at %.6g s" % body["Time"])
    _setFrame(objects, body, heldJoint)
    assembly.Document.recompute()
    return objects["Analysis"], body, held


# ---------------------------------------------------------------- frames


def ccxBinary():
    """CalculiX's ccx: the FEM preferences' binary, or one on PATH; None
    when there is neither"""
    import shutil

    binary = (
        App.ParamGet("User parameter:BaseApp/Preferences/Mod/Fem/Ccx")
        .GetString("ccxBinaryPath", "")
        .strip()
    )
    return shutil.which(binary) if binary else shutil.which("ccx")


def _vonMises(frd, near=None):
    """The largest von Mises stress of a CalculiX result file (MPa) and the
    node it is at; with near, the node ids by the loaded faces, also the
    largest of the others and its node (None where there are none)"""
    import numpy as np
    from feminout import importCcxFrdResults

    results = importCcxFrdResults.read_frd_result(frd)["Results"]
    stress = results[-1].get("stress", {}) if results else {}
    if not stress:
        raise ValueError("CalculiX wrote no stresses: %s" % frd)
    ids = list(stress.keys())
    s = np.array([stress[i] for i in ids], dtype=float)
    normal = s[:, :3]
    shear = s[:, 3:]
    mean = normal.mean(axis=1, keepdims=True)
    vm = np.sqrt(1.5 * ((normal - mean) ** 2).sum(axis=1) + 3.0 * (shear**2).sum(axis=1))
    k = int(np.nanargmax(vm))
    if near is None:
        return float(vm[k]), ids[k]
    away = np.array([i not in near for i in ids])
    if not away.any():
        return float(vm[k]), ids[k], None, None
    j = int(np.nanargmax(np.where(away, vm, -np.inf)))
    return float(vm[k]), ids[k], float(vm[j]), ids[j]


def _sectionDepth(coords, axes, point, size):
    """About the part's section depth at point: the shortest of its spans
    through it along its principal axes, each from where the line first
    meets the mesh to where it last leaves it (the nodes within a mesh size
    of the line; across a bore, its wall to the far one). A load spread on
    a face there evens out into the section within about that (Saint
    Venant's). Never less than size."""
    import numpy as np

    rel = coords - np.asarray(point, dtype=float)
    best = None
    for axis in axes:
        t = rel @ axis
        off = np.linalg.norm(rel - np.outer(t, axis), axis=1)
        t = t[off <= size]
        if len(t):
            span = float(t.max() - t.min())
            best = span if best is None else min(best, span)
    return max(best or 0.0, size)


def _nearLoads(objects, size, cache):
    """The mesh's node ids by the loaded faces, where a coupling's own
    stiffness or the load's concentration is what the stress shows (Saint
    Venant's): within about a section's depth (_sectionDepth, at least
    size) of the hold's faces and of a whole-face coupling's, and within
    its radius and the depth there of a coupling on a patch round its
    point. cache keeps each face's by its name, and the depths."""
    import numpy as np

    if "Ids" not in cache:
        nodes = objects["Mesh"].FemMesh.Nodes
        cache["Ids"] = list(nodes.keys())
        coords = np.array([[v.x, v.y, v.z] for v in nodes.values()])
        cache["Coords"] = coords
        # The part's principal axes, by its nodes' spread
        centred = coords - coords.mean(axis=0)
        cache["Axes"] = np.linalg.eigh(centred.T @ centred)[1].T
    ids = cache["Ids"]
    coords = cache["Coords"]
    axes = cache["Axes"]
    shape = objects["Copy"].Shape
    depths = cache.setdefault("Depths", {})

    def depth(point):
        # A patch moves with its contact: its depth by where it is, to a
        # half mesh size
        key = tuple(int(round(2.0 * c / size)) for c in point)
        if key not in depths:
            depths[key] = _sectionDepth(coords, axes, point, size)
        return depths[key]

    near = set()
    for rb in objects["Couplings"]:
        if getattr(rb, "Suppressed", False):
            continue
        radius = rb.CouplingRadius.Value if hasattr(rb, "CouplingRadius") else 0.0
        if radius > 0.0:
            ref = rb.ReferenceNode
            ref = (ref.x, ref.y, ref.z)
            d = np.linalg.norm(coords - np.array(ref), axis=1)
            near.update(ids[k] for k in np.nonzero(d <= radius + depth(ref))[0])
            continue
        for name in rb.References[0][1]:
            if name not in cache:
                import Part

                face = shape.getElement(name)
                at = face.CenterOfMass
                margin = depth((at.x, at.y, at.z))
                box = face.BoundBox
                box.enlarge(margin)
                found = set()
                for k, (x, y, z) in enumerate(coords):
                    if box.isInside(App.Vector(x, y, z)):
                        if face.distToShape(Part.Vertex(App.Vector(x, y, z)))[0] <= margin:
                            found.add(ids[k])
                cache[name] = found
            near |= cache[name]
    return near


def _datTotal(text, name):
    """A node set's total force in a CalculiX .dat, as a vector"""
    lines = text.splitlines()
    title = "total force (fx,fy,fz) for set %s" % name.upper()
    for k, line in enumerate(lines):
        if line.strip().startswith(title):
            return App.Vector(*[float(v) for v in lines[k + 2].split()])
    raise ValueError("CalculiX reported no %s" % title)


# ---------------------------------------------------------------- dynamics
#
# A part's own vibration through the run (sec D4a), by its held modes and
# the mode-acceleration method: the loads between two solved frames are
# linear in time, so the static response u_s(t) is the line between the
# frames' static solutions, exactly; the rest, u - u_s = sum y_i phi_i over
# the modes kept, each y_i'' + 2 z w y_i' + w^2 y_i = -c_i'' - 2 z w c_i',
# c_i = phi_i^T M u_s the static solution's share of the mode (its modal
# coordinate). c is linear between frames, so y is a damped sine from each
# frame on, in closed form: no steps. Started at rest at the first frame's
# static state. The modes the static part leaves to the dynamic one are
# those that see the loads change; the rest respond statically, which u_s
# already has.

# Damping ratio of each mode, by default
DAMPING = 0.02
# Modes kept, by default: the places lever's landing peaked at 51.29 MPa
# with 20, 51.36 with 40
MODES = 20
# Between two frames, samples enough for the highest mode kept, this many a
# period, at most SAMPLES
PER_PERIOD = 8
SAMPLES = 200


def _frdFields(path):
    """The result blocks of a CalculiX .frd, in order: [(name, ids, values
    n x columns)] -- DISP's three, STRESS's six"""
    import numpy as np

    blocks = []
    name = ids = vals = None
    with open(path) as f:
        for line in f:
            if line.startswith(" -4"):
                name = line[5:13].strip()
                ids, vals = [], []
            elif name is not None and line.startswith(" -1"):
                ids.append(int(line[3:13]))
                text = line.rstrip("\n")
                vals.append([float(text[k : k + 12]) for k in range(13, len(text) - 11, 12)])
            elif name is not None and line.startswith(" -3"):
                blocks.append((name, ids, np.array(vals)))
                name = None
    return blocks


def _vonMisesOf(s):
    """Each row's von Mises stress of a stress array n x 6"""
    import numpy as np

    normal = s[..., :3]
    shear = s[..., 3:6]
    mean = normal.mean(axis=-1, keepdims=True)
    return np.sqrt(1.5 * ((normal - mean) ** 2).sum(axis=-1) + 3.0 * (shear**2).sum(axis=-1))


def _largest(s, near, ids):
    """The largest von Mises stress of a stress field at the nodes ids
    (n x 6) and its node; and the largest of those not near (a mask) and
    its, None where there are none: as _vonMises"""
    import numpy as np

    vm = _vonMisesOf(s)
    k = int(np.nanargmax(vm))
    if not (~near).any():
        return float(vm[k]), ids[k], None, None
    j = int(np.nanargmax(np.where(near, -np.inf, vm)))
    return float(vm[k]), ids[k], float(vm[j]), ids[j]


_TET10 = []


def _tet10Mass():
    """The integral of N_a N_b over a straight-sided 10-node tetrahedron,
    over its volume: its corners first, then the nodes of its edges (0 1)
    (1 2) (2 0) (0 3) (1 3) (2 3) -- exact, by the integrals of the
    barycentric monomials, 6 a! b! c! d! / (a + b + c + d + 3)!"""
    import numpy as np

    if _TET10:
        return _TET10[0]
    edges = [(0, 1), (1, 2), (2, 0), (0, 3), (1, 3), (2, 3)]

    def mono(*at):
        e = [0, 0, 0, 0]
        for i in at:
            e[i] += 1
        return tuple(e)

    # Each shape function, {its monomial's exponents: coefficient}: a
    # corner's L (2 L - 1), an edge's 4 L_i L_j
    shapes = [{mono(i, i): 2.0, mono(i): -1.0} for i in range(4)]
    shapes += [{mono(i, j): 4.0} for i, j in edges]
    f = math.factorial
    m = np.zeros((10, 10))
    for a in range(10):
        for b in range(10):
            for ea, ca in shapes[a].items():
                for eb, cb in shapes[b].items():
                    e = [x + y for x, y in zip(ea, eb)]
                    m[a, b] += ca * cb * 6.0 * f(e[0]) * f(e[1]) * f(e[2]) * f(e[3]) / f(sum(e) + 3)
    _TET10.append((m, edges))
    return _TET10[0]


def _tet10Nodes(femmesh):
    """Each 10-node tetrahedron's nodes in _tet10Mass' order -- the corners,
    then each edge's node the one midway along it, found by where they
    are, not by the mesh's numbering -- and its volume"""
    import numpy as np

    _, edges = _tet10Mass()
    nodes = femmesh.Nodes
    rows = []
    volumes = []
    for e in femmesh.Volumes:
        ids = list(femmesh.getElementNodes(e))
        if len(ids) != 10:
            raise ValueError("the part's own vibration needs a second order tetrahedral mesh")
        p = [np.array(tuple(nodes[n])) for n in ids[:4]]
        rest = {n: np.array(tuple(nodes[n])) for n in ids[4:]}
        row = ids[:4]
        for i, j in edges:
            mid = 0.5 * (p[i] + p[j])
            row.append(min(rest, key=lambda n: float(np.linalg.norm(rest[n] - mid))))
        rows.append(row)
        volumes.append(abs(np.linalg.det(np.array([p[1] - p[0], p[2] - p[0], p[3] - p[0]]))) / 6)
    return np.array(rows), np.array(volumes)


class _Modes:
    """The held part's modes from a CalculiX frequency run: their
    frequencies, and their shapes and stresses at the mesh's nodes (ids, in
    order; align() puts a result block so); each one's share of a
    displacement (coordinate()). free: the rigid motions the hold leaves
    free, [(axis, point)] -- a slide along axis where point is None, else a
    turn about it through point -- whose modes are not the part's own
    vibration but its motion in the run, and are dropped; count: the modes
    kept, at most."""

    def __init__(self, base, femmesh, free=(), count=None):
        import numpy as np

        with open(base + ".dat") as f:
            lines = f.read().splitlines()
        k = next(i for i, l in enumerate(lines) if "E I G E N V A L U E   O U T P U T" in l)
        freqs = []
        for line in lines[k + 1 :]:
            parts = line.split()
            if len(parts) >= 4 and parts[0].isdigit():
                freqs.append(float(parts[3]))
            elif freqs and not line.strip():
                break
        blocks = _frdFields(base + ".frd")
        shapes = [b for b in blocks if b[0] == "DISP"][: len(freqs)]
        stresses = [b for b in blocks if b[0] == "STRESS"][: len(freqs)]
        if not freqs or len(shapes) < len(freqs) or len(stresses) < len(freqs):
            raise ValueError("CalculiX gave no modes of the part")
        self.mass, _ = _tet10Mass()
        rows, self.volumes = _tet10Nodes(femmesh)
        # The mesh's own nodes: a coupling's reference node is not one, and
        # one left out at a frame is not in its results
        self.ids = sorted(femmesh.Nodes.keys())
        where = {n: k for k, n in enumerate(self.ids)}
        self.index = np.vectorize(where.__getitem__)(rows)
        nodal = np.array([self.align(b)[:, :3] for b in shapes])
        stresses = np.array([self.align(b)[:, :6] for b in stresses])
        # The free motions, orthonormal by the mass; a mode that is more of
        # them than not is one. CalculiX leaves a free motion out when its
        # eigenvalue comes out below 0, keeps it when just above.
        at = np.array([tuple(femmesh.Nodes[n]) for n in self.ids])
        rigid = []
        for axis, point in free:
            a = np.array(axis)
            r = np.tile(a, (len(at), 1)) if point is None else np.cross(a, at - np.array(point))
            r = r[self.index]
            for q in rigid:
                r = r - self.inner(q, r) * q
            rigid.append(r / math.sqrt(self.inner(r, r)))
        keep = []
        for k, phi in enumerate(nodal[:, self.index]):
            share = sum(self.inner(q, phi) ** 2 for q in rigid) / self.inner(phi, phi)
            if share < 0.5:
                keep.append(k)
        keep = keep[:count]
        if not keep:
            raise ValueError("CalculiX gave no modes of the part")
        freqs = [freqs[k] for k in keep]
        self.omega = 2.0 * math.pi * np.array(freqs)
        self.frequencies = freqs
        self.nodal = nodal[keep]
        self.stresses = stresses[keep]
        self.shapes = self.nodal[:, self.index]
        self.norms = np.array([self.inner(s, s) for s in self.shapes])

    def align(self, block):
        """A result block's values at the mesh's nodes, in ids' order"""
        _, ids, values = block
        at = {n: k for k, n in enumerate(ids)}
        return values[[at[n] for n in self.ids]]

    def inner(self, a, b):
        """a^T M b over the part, a and b at the elements' nodes (E x 10 x
        3), the density aside: it cancels"""
        import numpy as np

        return float(np.einsum("e,ab,eai,ebi->", self.volumes, self.mass, a, b))

    def coordinate(self, u):
        """Each mode's share of a displacement at the mesh's nodes"""
        import numpy as np

        g = u[self.index]
        return np.array([self.inner(s, g) for s in self.shapes]) / self.norms


def _modalResponse(times, c, omega, zeta, grid):
    """The modes' dynamic parts y (modes x grid, grid within times) for
    their static coordinates c (modes x frames) at times, linear between
    them; at rest at the first, its static state"""
    import numpy as np

    times = np.asarray(times, dtype=float)
    grid = np.asarray(grid, dtype=float)
    wd = omega * math.sqrt(max(1.0 - zeta * zeta, 1e-12))
    y = np.zeros((len(omega), len(grid)))
    y0 = np.zeros(len(omega))
    v0 = np.zeros(len(omega))
    slope = np.zeros(len(omega))
    for k in range(len(times) - 1):
        span = times[k + 1] - times[k]
        new = (c[:, k + 1] - c[:, k]) / span if span > 0.0 else slope
        # q' goes on through a frame, c' turns: y' = q' - c' by as much
        v0 = v0 - (new - slope)
        slope = new
        # y'' + 2 z w y' + w^2 y = -2 z w c': a constant's share, and a
        # damped sine about it
        p = -2.0 * zeta * slope / omega
        a = y0 - p
        b = (v0 + zeta * omega * a) / wd
        last = k == len(times) - 2
        on = (grid >= times[k]) & ((grid <= times[k + 1]) if last else (grid < times[k + 1]))
        tau = grid[on] - times[k]
        decay = np.exp(-np.outer(zeta * omega, tau))
        y[:, on] = p[:, None] + decay * (
            a[:, None] * np.cos(np.outer(wd, tau)) + b[:, None] * np.sin(np.outer(wd, tau))
        )
        # The state at the next frame
        decay = np.exp(-zeta * omega * span)
        cs, sn = np.cos(wd * span), np.sin(wd * span)
        y1 = p + decay * (a * cs + b * sn)
        v1 = decay * ((-zeta * omega) * (a * cs + b * sn) + wd * (-a * sn + b * cs))
        y0, v0 = y1, v1
    return y


def _vibration(rows, modal, damping, back, nodes):
    """The frames' stresses with the part's own vibration (see "dynamics"
    above), into rows -- each frame's static ones kept as Static and
    StaticAway -- and sampled between them: a dict of the samples' times
    and their largest von Mises stress, overall and away from the loaded
    faces, and the largest sample's index, the frame nearest it, and the
    displacements and stresses there"""
    import numpy as np

    times = np.array([r["Time"] for r in rows])
    c = np.array([r["C"] for r in rows]).T
    top = modal.frequencies[-1]
    # The samples: each frame, and between it and the next enough for the
    # highest mode, at most SAMPLES
    grid = []
    spans = []  # each interval's (first frame, its samples)
    for a in range(len(times) - 1):
        span = times[a + 1] - times[a]
        n = min(SAMPLES, max(1, int(math.ceil(span * top * PER_PERIOD))))
        spans.append((a, list(range(len(grid), len(grid) + n))))
        grid += [times[a] + span * j / n for j in range(n)]
    grid.append(times[-1])
    if spans:
        spans[-1][1].append(len(grid) - 1)
    else:
        spans.append((0, [0]))
    grid = np.array(grid)
    y = _modalResponse(times, c, modal.omega, damping, grid)
    shapes = modal.stresses.reshape(len(modal.omega), -1)
    out = {"Times": list(grid), "Stress": [], "Away": []}
    best = (-1.0, 0, 0, 0.0)  # (stress, sample, interval's first frame, its share)
    for a, on in spans:
        on = np.array(on)
        b = min(a + 1, len(times) - 1)
        span = times[b] - times[a]
        w = (grid[on] - times[a]) / span if span > 0.0 else np.zeros(len(on))
        Sa, Sb = rows[a]["S"], rows[b]["S"]
        field = (
            Sa[None]
            + w[:, None, None] * (Sb - Sa)[None]
            + (y[:, on].T @ shapes).reshape(len(on), *Sa.shape)
        )
        vm = _vonMisesOf(field)
        near = rows[a]["Near"] | rows[b]["Near"]
        whole = vm.max(axis=1)
        away = np.where(near[None], -np.inf, vm).max(axis=1) if (~near).any() else None
        out["Stress"] += [float(v) for v in whole]
        out["Away"] += [float(v) for v in away] if away is not None else [math.nan] * len(on)
        j = int(np.argmax(whole))
        if whole[j] > best[0]:
            best = (float(whole[j]), int(on[j]), a, float(w[j]))
        # The frames' own: each interval's first sample, the last frame the
        # last; away from its own loaded faces, as its static stress
        own = [(a, 0)] + ([(b, len(on) - 1)] if b != a and b == len(times) - 1 else [])
        for frame, sample in own:
            node = int(np.argmax(vm[sample]))
            row = rows[frame]
            row["Static"], row["StaticAway"] = row["Stress"], row["Away"]
            row["Stress"] = float(vm[sample][node])
            row["At"] = back.multVec(nodes[modal.ids[node]])
            if (~row["Near"]).any():
                far = int(np.argmax(np.where(row["Near"], -np.inf, vm[sample])))
                row["Away"] = float(vm[sample][far])
                row["AwayAt"] = back.multVec(nodes[modal.ids[far]])
    _, sample, a, w = best
    b = min(a + 1, len(rows) - 1)
    mix = y[:, sample]
    out["Peak"] = sample
    out["PeakFrame"] = a if w <= 0.5 else b
    out["PeakDisp"] = (
        (1.0 - w) * rows[a]["U"] + w * rows[b]["U"] + np.tensordot(mix, modal.nodal, axes=1)
    )
    out["PeakStress"] = (
        (1.0 - w) * rows[a]["S"]
        + w * rows[b]["S"]
        + np.tensordot(mix, modal.stresses, axes=1)
    )
    return out


def _writeField(frd, modal, disp, stress):
    """A CalculiX .frd's displacements and stresses at the mesh's nodes put
    as given (modal.ids' order), its other results taken out: a field of
    the part's vibration, read as a frame's result"""
    with open(frd) as f:
        lines = f.readlines()
    where = {n: k for k, n in enumerate(modal.ids)}
    out = []
    block = None
    for line in lines:
        if block is None and line.startswith("  100C"):
            block = [line]
            continue
        if block is None:
            out.append(line)
            continue
        block.append(line)
        if not line.startswith(" -3"):
            continue
        name = next((l[5:13].strip() for l in block if l.startswith(" -4")), "")
        values = {"DISP": disp, "STRESS": stress}.get(name)
        if values is not None:
            for l in block:
                if l.startswith(" -1") and int(l[3:13]) in where:
                    count = (len(l.rstrip("\n")) - 13) // 12
                    row = values[where[int(l[3:13])]]
                    l = l[:13] + "".join("%12.5E" % row[i] for i in range(count)) + "\n"
                out.append(l)
        block = None
    with open(frd, "w") as f:
        f.writelines(out)


def solveFrames(
    assembly,
    component,
    frames,
    hold=None,
    maxSize=None,
    progress=None,
    jobs=None,
    dynamic=False,
    damping=DAMPING,
    modes=MODES,
):
    """The stress analysis of component (makeAnalysis) at each of the run's
    frames, solved by CalculiX: one analysis, its shape, mesh and couplings
    made once -- the loads are in the part's own coordinates -- each frame's
    loads set into it and solved, jobs of the frames' CalculiX runs at
    once (up to 4, half the CPUs, when not given). Held at one joint
    throughout: hold, or the one with the largest force at any of the
    frames.

    Each frame's largest von Mises stress, where it is, and what holds the
    part against what the run says holds it there (the check), go into a
    stress series in the analysis (StressSeries), which the Simulation
    panel's Results box plots while the run is the one it was taken from.
    The analysis is left at the frame of the largest stress, with its
    result. progress(k, n), if given, is called before each frame; it stops
    the frames by returning False (the series then has those started).

    dynamic: the part's own vibration too, by its modes held as it is
    (modes of them, each damped by damping of critical): the frames' static
    solutions in turn as the loads go, linear between frames, and the modes
    the change sets ringing (see "dynamics" above) -- from rest at the first
    frame. The stresses at the frames are then those, the static ones kept
    beside them (StaticVonMises), and the series has the stress between the
    frames too, sampled for the highest mode (SampleTimes); the analysis is
    left at the largest of the samples, its result the field there. What
    holds the part is still checked by the frames' static solutions.

    Returns the series. Raises ValueError where makeAnalysis would, without
    CalculiX, or when a solve fails."""
    import os
    import shutil
    import tempfile

    from femtools import ccxtools

    frames = sorted(set(frames))
    if not frames:
        raise ValueError("no frames to solve")
    binary = ccxBinary()
    if not binary:
        raise ValueError(
            "CalculiX (ccx) is not found: set it in the FEM preferences, or put it on PATH"
        )
    results = assembly.getSimulationResults()
    bodies = [loadsOn(assembly, component, f) for f in frames]
    heldJoint = _heldJoint(bodies, hold)
    times = [b["Time"] for b in bodies]
    when = "at %.6g s" % times[0]
    if len(frames) > 1:
        when = "from %.6g to %.6g s" % (times[0], times[-1])
    doc = assembly.Document
    objects = _build(assembly, component, bodies[0], heldJoint, maxSize, when)
    analysis = objects["Analysis"]
    hold_ = [rb for rb, s in zip(objects["Couplings"], objects["Sources"]) if s == heldJoint][0]
    _setFrame(objects, bodies[0], heldJoint)
    doc.recompute()
    back = objects["Placement"].inverse()
    nodes = objects["Mesh"].FemMesh.Nodes
    # Away from the loaded faces: about a section's depth off them, at
    # least a mesh size
    size = getattr(objects["Mesh"].MaxSize, "Value", objects["Mesh"].MaxSize)
    cache = {}
    runForce = _runMark(assembly, results, heldJoint, component)

    work = tempfile.mkdtemp(prefix="loadsfem_")
    rows = []
    try:
        fea = ccxtools.FemToolsCcx(analysis, objects["Solver"], test_mode=True)
        fea.update_objects()
        fea.setup_working_dir(work)
        problems = fea.check_prerequisites()
        if problems:
            raise ValueError(problems)
        fea.setup_ccx(binary)
        if jobs is None:
            jobs = max(1, min(4, (os.cpu_count() or 1) // 2))
        modal = None
        freeKind = ""
        if dynamic:
            # Its modes first, held as its joint holds it: free to turn on a
            # hinge, the hinge's axis one of the hold's axes. A frame's
            # static state holds it stiff there, but what holds it takes
            # next to nothing, the frame balancing: the state is the one
            # held free but for a free motion, and no mode has a share of
            # that. Each frame's share of the modes is taken as it is
            # collected.
            held = [l for l in bodies[0]["Loads"] if l["Source"] == heldJoint][0]
            free, freeKind = _holdAsJoint(objects, hold_, held)
            solver = objects["Solver"]
            solver.AnalysisType = "frequency"
            solver.EigenmodesCount = modes + len(free)
            doc.recompute()
            fea.write_inp_file()
            folder = os.path.join(work, "modes")
            os.mkdir(folder)
            inp = os.path.join(folder, os.path.basename(fea.inp_file_name))
            os.replace(fea.inp_file_name, inp)
            base = os.path.splitext(inp)[0]
            with open(base + ".out", "wb") as out:
                code = fea.ccx_process(inp, threads=jobs, output=out).wait()
            if code != 0:
                with open(base + ".out", errors="replace") as f:
                    said = f.read().strip().splitlines()[-3:]
                raise ValueError("CalculiX failed for the modes: %s" % " ".join(said))
            modal = _Modes(base, objects["Mesh"].FemMesh, free, modes)
            _holdStiff(hold_)
            solver.AnalysisType = "static"
            doc.recompute()

        def solve(body):
            _setFrame(objects, body, heldJoint)
            doc.recompute()
            fea.write_inp_file()
            if fea.start_ccx() != 0:
                raise ValueError(
                    "CalculiX failed at %.6g s: %s" % (body["Time"], fea.ccx_stderr.strip())
                )
            return os.path.splitext(fea.inp_file_name)[0]

        def collect(job):
            k, frame, body, base, near, proc, out = job
            code = proc.wait()
            out.close()
            if code != 0:
                with open(base + ".out", errors="replace") as f:
                    said = f.read().strip().splitlines()[-3:]
                raise ValueError("CalculiX failed at %.6g s: %s" % (body["Time"], " ".join(said)))
            extra = {}
            if modal is None:
                stress, node, away, awayNode = _vonMises(base + ".frd", near)
            else:
                # Its static state kept, and its share of each mode
                import numpy as np

                blocks = _frdFields(base + ".frd")
                u = modal.align(next(b for b in blocks if b[0] == "DISP"))[:, :3]
                s = modal.align(next(b for b in blocks if b[0] == "STRESS"))[:, :6]
                extra = {
                    "U": u,
                    "S": s,
                    "C": modal.coordinate(u),
                    "Near": np.array([n in near for n in modal.ids]),
                }
                stress, node, away, awayNode = _largest(s, extra["Near"], modal.ids)
            with open(base + ".dat") as f:
                text = f.read()
            heldForce, heldTorque = _heldLoad(body, heldJoint)
            rows.append(
                {
                    **extra,
                    "Frame": frame,
                    "Time": body["Time"],
                    "Stress": stress,
                    "At": back.multVec(nodes[node]),
                    "Away": away if away is not None else math.nan,
                    "AwayAt": back.multVec(nodes[awayNode]) if away is not None else App.Vector(),
                    "HeldForce": heldForce * 1e-3,
                    "HeldTorque": heldTorque * 1e-3,
                    "FemForce": back.Rotation.multVec(_datTotal(text, hold_.Name + "_RefNode")),
                    "FemTorque": back.Rotation.multVec(_datTotal(text, hold_.Name + "_RotNode")),
                    "RunForce": runForce[frame],
                }
            )

        # Each frame's input written in turn and its CalculiX run started,
        # in a folder of its own, jobs of them at once, one thread each;
        # the oldest collected when that many run. ccx's solver takes one
        # CPU, and a frame's 0.4 s of it was the most of its time.
        running = []
        try:
            for k, (frame, body) in enumerate(zip(frames, bodies)):
                if progress is not None and progress(k, len(frames)) is False:
                    break
                _setFrame(objects, body, heldJoint)
                doc.recompute()
                fea.write_inp_file()
                # Where the couplings are now, before the next frame moves them
                near = _nearLoads(objects, size, cache)
                folder = os.path.join(work, "frame%05d" % k)
                os.mkdir(folder)
                inp = os.path.join(folder, os.path.basename(fea.inp_file_name))
                os.replace(fea.inp_file_name, inp)
                base = os.path.splitext(inp)[0]
                out = open(base + ".out", "wb")
                proc = fea.ccx_process(inp, threads=1, output=out)
                running.append((k, frame, body, base, near, proc, out))
                if len(running) >= jobs:
                    collect(running.pop(0))
            while running:
                collect(running.pop(0))
        finally:
            for job in running:
                job[5].kill()
                job[5].wait()
                job[6].close()
        samples = None
        if rows and modal is not None:
            samples = _vibration(rows, modal, damping, back, nodes)
        if rows:
            # The analysis left at its largest stress, with that result: of
            # the vibration, the field at its largest sample, written over
            # the nearest frame's static one
            peak = max(range(len(rows)), key=lambda k: rows[k]["Stress"])
            when = rows[peak]["Time"]
            if samples is not None:
                when = samples["Times"][samples["Peak"]]
                peak = samples["PeakFrame"]
            solve(bodies[frames.index(rows[peak]["Frame"])])
            if samples is not None:
                _writeField(
                    os.path.splitext(fea.inp_file_name)[0] + ".frd",
                    modal,
                    samples["PeakDisp"],
                    samples["PeakStress"],
                )
            fea.load_results()
            for obj in analysis.Group:
                if obj.isDerivedFrom("Fem::FemResultObject"):
                    obj.Label = "Result at %.6g s" % when
    finally:
        shutil.rmtree(work, ignore_errors=True)

    series = doc.addObject("App::FeaturePython", "StressSeries")
    StressSeries(series)
    series.Label = "Stress in %s" % component.Label
    series.Assembly = assembly
    series.Simulation = results.get("Simulation")
    series.Component = component
    series.Held = heldJoint
    series.Frames = [r["Frame"] for r in rows]
    series.Times = [r["Time"] for r in rows]
    series.VonMises = [r["Stress"] for r in rows]
    series.VonMisesAt = [r["At"] for r in rows]
    series.VonMisesAway = [r["Away"] for r in rows]
    series.VonMisesAwayAt = [r["AwayAt"] for r in rows]
    series.HeldForce = [r["HeldForce"] for r in rows]
    series.HeldTorque = [r["HeldTorque"] for r in rows]
    series.FemHeldForce = [r["FemForce"] for r in rows]
    series.FemHeldTorque = [r["FemTorque"] for r in rows]
    series.RunHeldForce = [r["RunForce"] for r in rows]
    if samples is not None:
        series.Dynamic = True
        series.Damping = damping
        series.ModeFrequencies = list(modal.frequencies)
        series.ModesFree = freeKind
        series.StaticVonMises = [r["Static"] for r in rows]
        series.StaticVonMisesAway = [r["StaticAway"] for r in rows]
        series.SampleTimes = samples["Times"]
        series.SampleVonMises = samples["Stress"]
        series.SampleVonMisesAway = samples["Away"]
    analysis.addObject(series)
    doc.recompute()
    return series


class StressSeries:
    """A part's stresses over frames of a run (solveFrames): each frame's
    largest von Mises stress and where it is, and the check -- what holds
    the part in the analysis against what the run says holds it there. The
    held joint's force in the run at each frame (RunHeldForce, the run's
    own values) tells whether a run is the one the series was taken from."""

    PROPERTIES = (
        ("App::PropertyLinkHidden", "Assembly", "The assembly whose run it is of"),
        ("App::PropertyLinkHidden", "Simulation", "The study whose run it is of"),
        ("App::PropertyLink", "Component", "The part analysed"),
        ("App::PropertyLink", "Held", "The joint it is held at"),
        ("App::PropertyIntegerList", "Frames", "The run's frames solved"),
        ("App::PropertyFloatList", "Times", "Their times, s"),
        ("App::PropertyFloatList", "VonMises", "The largest von Mises stress at each, MPa"),
        ("App::PropertyVectorList", "VonMisesAt", "Where it is, in the part's coordinates, mm"),
        (
            "App::PropertyFloatList",
            "VonMisesAway",
            "The largest away from the loaded faces, about a section's depth off them and off "
            "a load's patch: what the couplings' own stiffness and the loads' concentration leave "
            "out, MPa; NaN where nothing is that far",
        ),
        ("App::PropertyVectorList", "VonMisesAwayAt", "Where it is, mm"),
        ("App::PropertyVectorList", "HeldForce", "The run's load at the held joint, N"),
        (
            "App::PropertyVectorList",
            "HeldTorque",
            "The run's torque at the held joint, a torsion spring's on it with it, N mm",
        ),
        ("App::PropertyVectorList", "FemHeldForce", "What holds the part in the analysis, N"),
        ("App::PropertyVectorList", "FemHeldTorque", "Its torque, N mm"),
        (
            "App::PropertyVectorList",
            "RunHeldForce",
            "The held joint's force channel at each frame, as the run reported it (held by "
            "its rigid group, the group's acceleration): the run it is of",
        ),
        (
            "App::PropertyBool",
            "Dynamic",
            "With the part's own vibration, by its modes held as it is: the stresses at the "
            "frames are those, the static ones beside them",
        ),
        ("App::PropertyFloat", "Damping", "Each mode's damping, of critical"),
        ("App::PropertyFloatList", "ModeFrequencies", "The modes', Hz"),
        (
            "App::PropertyString",
            "ModesFree",
            "What holding the part at its joint leaves free in its modes: Turn, Slide, "
            "TurnSlide, Ball; empty, held stiff",
        ),
        ("App::PropertyFloatList", "StaticVonMises", "The frames' static largest, MPa"),
        ("App::PropertyFloatList", "StaticVonMisesAway", "And away from the loaded faces, MPa"),
        (
            "App::PropertyFloatList",
            "SampleTimes",
            "The vibration sampled between the frames, for the highest mode: the times, s",
        ),
        ("App::PropertyFloatList", "SampleVonMises", "The largest at each, MPa"),
        ("App::PropertyFloatList", "SampleVonMisesAway", "And away from the loaded faces, MPa"),
    )

    def __init__(self, obj):
        obj.Proxy = self
        self.createProperties(obj)

    def onDocumentRestored(self, obj):
        self.createProperties(obj)

    def createProperties(self, obj):
        from PySide.QtCore import QT_TRANSLATE_NOOP

        for kind, name, doc in self.PROPERTIES:
            if not hasattr(obj, name):
                obj.addProperty(kind, name, "Stress", QT_TRANSLATE_NOOP("App::Property", doc))
                obj.setEditorMode(name, 1)

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def execute(self, obj):
        pass


def isStressSeries(obj):
    return isinstance(getattr(obj, "Proxy", None), StressSeries)


def stressChannels(assembly, results=None):
    """The stress series taken from the assembly's last run, as channels of
    the run's results: Quantity "VonMises", Object the part, a value per
    frame of the run (MPa), None at a frame not solved. A series of another
    run, or of this study before it was run again, is left out."""
    results = results or assembly.getSimulationResults()
    times = results["Times"]
    channels = []
    for obj in assembly.Document.Objects:
        if not isStressSeries(obj) or obj.Assembly != assembly or not obj.Frames:
            continue
        if obj.Simulation != results.get("Simulation") or obj.Held is None:
            continue
        run = _runMark(assembly, results, obj.Held, obj.Component)
        if run is None or max(obj.Frames) >= len(times):
            continue
        scale = max(max(v.Length for v in obj.RunHeldForce), 1e-9)
        if any(
            abs(times[f] - t) > 1e-9 * max(1.0, abs(t))
            or (run[f] - v).Length > 1e-9 * scale
            for f, t, v in zip(obj.Frames, obj.Times, obj.RunHeldForce)
        ):
            continue
        # The largest, and the largest away from the loaded faces (a series
        # before it has none)
        # A series with the part's vibration has it sampled between the
        # frames too: Samples, at SampleTimes
        dynamic = getattr(obj, "Dynamic", False) and obj.SampleTimes
        for quantity, stresses, sampled, still in (
            ("VonMises", obj.VonMises, "SampleVonMises", "StaticVonMises"),
            ("VonMisesAway", obj.VonMisesAway, "SampleVonMisesAway", "StaticVonMisesAway"),
        ):
            if len(stresses) != len(obj.Frames):
                continue
            values = [None] * len(times)
            for f, v in zip(obj.Frames, stresses):
                values[f] = None if math.isnan(v) else v
            channel = {
                "Name": obj.Name,
                "Quantity": quantity,
                "Object": obj.Component,
                "Values": values,
                "Unique": True,
                "Series": obj,
            }
            samples = getattr(obj, sampled, []) if dynamic else []
            if len(samples) == len(obj.SampleTimes) and samples:
                channel["SampleTimes"] = list(obj.SampleTimes)
                channel["Samples"] = [None if math.isnan(v) else v for v in samples]
                # Each frame as it stands, beside it
                static = getattr(obj, still, [])
                if len(static) == len(obj.Frames):
                    channel["Static"] = [None] * len(times)
                    for f, v in zip(obj.Frames, static):
                        channel["Static"][f] = None if math.isnan(v) else v
            channels.append(channel)
    return channels
