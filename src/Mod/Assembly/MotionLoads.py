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
      Component, Time, Frame;
      Loads: a list of dicts, each with Source (the joint, spring,
        bushing, contact set, torsion spring or applied load), Kind
        ("Joint", "Spring", "Bushing", "Contact", "TorsionSpring", "Load"),
        Point, Force, Torque (about Point; a joint's its reaction with
        its motor's effort, a motion driving it), Reference (a joint's or a
        torsion spring's joint's reference on this component, (object,
        [element]), else None) and Unique (False for a redundant joint's
        share). A contact set's is its force on this part at the centre of
        its points on it, weighted by their forces, and the torque about
        that centre (all none while it does not press); a torsion spring's
        a torque about its joint's axis; an applied load's at its frame's
        origin, or on its reaction the opposite, where that origin was at
        the run's start;
      Mass (kg), CenterOfMass, Inertia (rows, about the centre of mass,
        kg mm^2), Gravity (mm/s^2), Acceleration (the centre's, mm/s^2),
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
            loads.append(
                {
                    "Source": item,
                    "Kind": "Contact",
                    "Point": toLocal.multVec(mine["PartContactPoint"]["Values"][frame]),
                    "Force": turn.multVec(mine["PartContactForce"]["Values"][frame]),
                    "Torque": turn.multVec(mine["PartContactTorque"]["Values"][frame]),
                    "Reference": None,
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
    if load["Kind"] == "Joint":
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


def _heldJoint(bodies, hold):
    """The joint the part is held at over its free bodies: hold, or the one
    with the largest force at any of them"""
    largest = []  # (joint, its largest force), joints in order
    for body in bodies:
        for load in body["Loads"]:
            if load["Kind"] != "Joint":
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
        faces = []
        ref = load["Reference"]
        if ref:
            sub = ref[1][0] if ref[1] else ""
            element = Part.getShape(ref[0], sub, needSubElement=True, transform=False)
            if not element.isNull():
                faces = _facesOf(local, element)
        if not faces:
            faces = _nearestFace(local, load["Point"])
        held = joint is not None and joint == heldJoint
        rb = ObjectsFem.makeConstraintRigidBody(doc, "Hold" if held else "Load")
        rb.Label = ("Held at %s" if held else "%s") % (joint or load["Source"]).Label
        rb.References = [(copy, tuple(faces))]
        rb.ReferenceNode = place.multVec(load["Point"])
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


def _setFrame(objects, body, heldJoint):
    """One free body's values into the analysis _build made: each load's
    force and torque, the body motion"""
    sources = [load["Source"] for load in body["Loads"]]
    if len(sources) != len(objects["Sources"]) or any(
        a != b for a, b in zip(sources, objects["Sources"])
    ):
        raise ValueError("the loads on %s differ between frames" % body["Component"].Label)
    turn = objects["Placement"].Rotation
    totals = {}  # coupling name: (coupling, force, torque), in the part's axes
    for load, rb in zip(body["Loads"], objects["Couplings"]):
        if _jointOf(load) == heldJoint:
            continue
        # A contact's point moves over the part: its coupling with it, on
        # the face nearest it; where it does not press, as it was
        pressing = load["Force"].Length > 0.0 or load["Torque"].Length > 0.0
        if load["Kind"] == "Contact" and pressing:
            rb.ReferenceNode = objects["Placement"].multVec(load["Point"])
            faces = tuple(_nearestFace(objects["Shape"], load["Point"]))
            if tuple(rb.References[0][1]) != faces:
                rb.References = [(objects["Copy"], faces)]
        _, force, torque = totals.get(rb.Name, (rb, App.Vector(), App.Vector()))
        totals[rb.Name] = (rb, force + load["Force"], torque + load["Torque"])
    for rb, force, torque in totals.values():
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


def _vonMises(frd):
    """The largest von Mises stress of a CalculiX result file (MPa) and the
    node it is at"""
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
    return float(vm[k]), ids[k]


def _datTotal(text, name):
    """A node set's total force in a CalculiX .dat, as a vector"""
    lines = text.splitlines()
    title = "total force (fx,fy,fz) for set %s" % name.upper()
    for k, line in enumerate(lines):
        if line.strip().startswith(title):
            return App.Vector(*[float(v) for v in lines[k + 2].split()])
    raise ValueError("CalculiX reported no %s" % title)


def solveFrames(assembly, component, frames, hold=None, maxSize=None, progress=None):
    """The stress analysis of component (makeAnalysis) at each of the run's
    frames, solved by CalculiX: one analysis, its shape, mesh and couplings
    made once -- the loads are in the part's own coordinates -- each frame's
    loads set into it and solved in turn. Held at one joint throughout:
    hold, or the one with the largest force at any of the frames.

    Each frame's largest von Mises stress, where it is, and what holds the
    part against what the run says holds it there (the check), go into a
    stress series in the analysis (StressSeries), which the Simulation
    panel's Results box plots while the run is the one it was taken from.
    The analysis is left at the frame of the largest stress, with its
    result. progress(k, n), if given, is called before each frame; it stops
    the frames by returning False (the series then has those solved).

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
    runForce = _channels(results, heldJoint)["Force"]["Values"]

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

        def solve(body):
            _setFrame(objects, body, heldJoint)
            doc.recompute()
            fea.write_inp_file()
            if fea.start_ccx() != 0:
                raise ValueError(
                    "CalculiX failed at %.6g s: %s" % (body["Time"], fea.ccx_stderr.strip())
                )
            return os.path.splitext(fea.inp_file_name)[0]

        for k, (frame, body) in enumerate(zip(frames, bodies)):
            if progress is not None and progress(k, len(frames)) is False:
                break
            base = solve(body)
            stress, node = _vonMises(base + ".frd")
            with open(base + ".dat") as f:
                text = f.read()
            heldForce, heldTorque = _heldLoad(body, heldJoint)
            rows.append(
                {
                    "Frame": frame,
                    "Time": body["Time"],
                    "Stress": stress,
                    "At": back.multVec(nodes[node]),
                    "HeldForce": heldForce * 1e-3,
                    "HeldTorque": heldTorque * 1e-3,
                    "FemForce": back.Rotation.multVec(_datTotal(text, hold_.Name + "_RefNode")),
                    "FemTorque": back.Rotation.multVec(_datTotal(text, hold_.Name + "_RotNode")),
                    "RunForce": runForce[frame],
                }
            )
        if rows:
            # The analysis left at its largest stress, with that result
            peak = max(range(len(rows)), key=lambda k: rows[k]["Stress"])
            solve(bodies[frames.index(rows[peak]["Frame"])])
            fea.load_results()
            for obj in analysis.Group:
                if obj.isDerivedFrom("Fem::FemResultObject"):
                    obj.Label = "Result at %.6g s" % rows[peak]["Time"]
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
    series.HeldForce = [r["HeldForce"] for r in rows]
    series.HeldTorque = [r["HeldTorque"] for r in rows]
    series.FemHeldForce = [r["FemForce"] for r in rows]
    series.FemHeldTorque = [r["FemTorque"] for r in rows]
    series.RunHeldForce = [r["RunForce"] for r in rows]
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
            "The held joint's force channel at each frame, as the run reported it",
        ),
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
        found = _channels(results, obj.Held).get("Force")
        if found is None or max(obj.Frames) >= len(times):
            continue
        run = found["Values"]
        scale = max(max(v.Length for v in obj.RunHeldForce), 1e-9)
        if any(
            abs(times[f] - t) > 1e-9 * max(1.0, abs(t))
            or (run[f] - v).Length > 1e-9 * scale
            for f, t, v in zip(obj.Frames, obj.Times, obj.RunHeldForce)
        ):
            continue
        values = [None] * len(times)
        for f, v in zip(obj.Frames, obj.VonMises):
            values[f] = v
        channels.append(
            {
                "Name": obj.Name,
                "Quantity": "VonMises",
                "Object": obj.Component,
                "Values": values,
                "Unique": True,
                "Series": obj,
            }
        )
    return channels
