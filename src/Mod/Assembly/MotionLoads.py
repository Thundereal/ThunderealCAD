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
    import Part
    import ObjectsFem

    body = loadsOn(assembly, component, frame)
    doc = assembly.Document
    joints = [l for l in body["Loads"] if l["Kind"] == "Joint"]
    if not joints:
        raise ValueError("%s is on no joint to hold it by" % component.Label)
    if hold is None:
        held = max(joints, key=lambda l: l["Force"].Length)
    else:
        found = [l for l in joints if l["Source"] == hold]
        if not found:
            raise ValueError("%s does not hold %s" % (hold.Label, component.Label))
        held = found[0]

    # Where it stands now, in the document -- at rest, or at the frame a
    # player shows: the loads are in its own coordinates
    place = component.getGlobalPlacement()
    turn = place.Rotation
    local = Part.getShape(component, "", transform=False)
    copy = doc.addObject("Part::Feature", "LoadsAt")
    copy.Shape = local
    copy.Placement = place
    copy.Label = "%s at %.6g s" % (component.Label, body["Time"])

    analysis = ObjectsFem.makeAnalysis(doc, "LoadsAnalysis")
    analysis.Label = "Loads on %s at %.6g s" % (component.Label, body["Time"])
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

    for k, load in enumerate(body["Loads"]):
        faces = []
        ref = load["Reference"]
        if ref:
            sub = ref[1][0] if ref[1] else ""
            element = Part.getShape(ref[0], sub, needSubElement=True, transform=False)
            if not element.isNull():
                faces = _facesOf(local, element)
        if not faces:
            faces = _nearestFace(local, load["Point"])
        rb = ObjectsFem.makeConstraintRigidBody(doc, "Hold" if load is held else "Load")
        rb.Label = ("Held at %s" if load is held else "%s") % load["Source"].Label
        rb.References = [(copy, tuple(faces))]
        rb.ReferenceNode = place.multVec(load["Point"])
        if load is held:
            rb.Coupling = "Rigid"
            for axis in "XYZ":
                setattr(rb, "TranslationalMode" + axis, "Constraint")
                setattr(rb, "RotationalMode" + axis, "Constraint")
        else:
            rb.Coupling = "Distributing"
            force = turn.multVec(load["Force"]) * 1e-3  # mN to N
            torque = turn.multVec(load["Torque"]) * 1e-3  # mN mm to N mm
            for axis, f, t in zip("XYZ", force, torque):
                setattr(rb, "Force" + axis, "%.12g N" % f)
                setattr(rb, "Moment" + axis, "%.12g N*mm" % t)
                setattr(rb, "TranslationalMode" + axis, "Load")
                setattr(rb, "RotationalMode" + axis, "Load")
        analysis.addObject(rb)

    motion = ObjectsFem.makeConstraintBodyMotion(doc, "LoadsMotion")
    motion.ReferencePoint = place.multVec(body["CenterOfMass"])
    motion.Gravity = turn.multVec(body["Gravity"])
    motion.LinearAcceleration = turn.multVec(body["Acceleration"])
    motion.AngularVelocity = turn.multVec(body["AngularVelocity"])
    motion.AngularAcceleration = turn.multVec(body["AngularAcceleration"])
    analysis.addObject(motion)
    doc.recompute()
    return analysis, body, held
