# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of FreeCAD.                                         *
# *                                                                         *
# *   FreeCAD is free software: you can redistribute it and/or modify it    *
# *   under the terms of the GNU Lesser General Public License as           *
# *   published by the Free Software Foundation, either version 2.1 of the  *
# *   License, or (at your option) any later version.                       *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful, but        *
# *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
# *   Lesser General Public License for more details.                       *
# *                                                                         *
# *   You should have received a copy of the GNU Lesser General Public      *
# *   License along with FreeCAD. If not, see                               *
# *   <https://www.gnu.org/licenses/>.                                      *
# *                                                                         *
# ***************************************************************************

"""Assembly3 assemblies to Assembly objects, in an open document.

docs/AssemblyObjectModel.md sec 8 and docs/AssemblyFromAsm3.md part B,
which this follows with the C++ object model in place of upstream's joints.
The Assembly3 code bundled with the module (AssemblyAsm3Bundle) restores
the document's Assembly3 objects with their proxies, so its element
references, in whatever generation the file has them, are read through
Assembly3's own API (getElementSubname, getInfo) rather than re-derived.

What each becomes:
- an Assembly3 assembly: an Assembly::AssemblyObject with the same label
  and placement, its parts moved in; one inside another's parts stays a
  direct component of the parent (a rigid one, as Assembly3's are), and a
  link to one now links to the new assembly;
- an Assembly3 element: a Part::Element of the same label in the new
  assembly's Elements group, so that a parent's references into it, by
  label, keep working (sec 6.3, "$$Elements.$Label.");
- a constraint: joints (a constraint of N elements gives N-1 of them,
  pairs as Assembly3 forms them), each side an element binding to the
  element, with the Offset that puts our frame where Assembly3 has it
  (Offset = F^-1 * G, as AssemblyMigrate does for upstream's joints), and
  the frames made to satisfy the joint at the saved poses;
- Locked: grounding of the part on a face or a curved edge; on a vertex or
  a straight edge, a ball or revolute joint to a grounded datum there, as
  Assembly3 leaves the part free to turn about it. An assembly with no
  lock grounds the first part of its first constraint, as Assembly3 does;
- an element of a link array: a component of its own (sec 7.4), the array
  shown expanded where Assembly3 had it collapsed;
- a multiplied constraint: one multiplied joint (sec 7.4), its second side
  on the edge its first side's element is paired with; the elements picked
  by hand (NoExpand) its Targets;
- AttachmentOffset: a Fixed joint, as Attachment; its "Update element
  offset" is the Fixed joint's "Update Offset".

What is not converted is reported: the constraint types with no
counterpart (the sketch and draft-wire families, Symmetric, the measures),
and anything that does not resolve.

convert(doc) works headless and is one transaction; the GUI command is
Assembly_ConvertAssembly3 (CommandConvertAsm3.py).
"""

import FreeCAD as App

# Assembly3 constraint ids (asm3/constraint.py, _id) to what they become:
# the joint type, or a handler name
CONSTRAINTS = {
    0: "Locked",
    1: "Ball",  # PointsCoincident
    3: "Distance",  # PointInPlane, 0
    4: "Distance",  # PointOnLine, 0
    5: "Distance",  # PointDistance
    7: "Distance",  # PointsPlaneDistance
    8: "Distance",  # PointLineDistance
    27: "Angle",
    28: "Perpendicular",
    35: "PlaneCoincident",
    36: "AxialAlignment",
    37: "Distance",  # PlaneAlignment: Offset, the angle lock dropped
    39: "Cylindrical",  # Colinear
    44: "Distance",  # PointsDistance
    45: "Fixed",  # Attachment
    46: "Fixed",  # AttachmentOffset
    291: "Parallel",  # MultiParallel
    2: "Parallel",  # SameOrientation: approximate, the twist is lost
}

# The ones that convert only approximately, and why
APPROXIMATE = {
    2: "SameOrientation keeps the twist; a Parallel joint does not",
    37: "PlaneAlignment's angle lock has no counterpart",
}

TOLERANCE = 1e-6


class Report:
    """What a conversion did, per assembly label: lists of lines"""

    def __init__(self):
        self.lines = {}

    def add(self, assembly, text):
        self.lines.setdefault(assembly, []).append(text)

    def __str__(self):
        out = []
        for name, lines in self.lines.items():
            out.append(name + ":")
            out += ["  " + line for line in lines]
        return "\n".join(out)


def _asm3():
    from freecad.asm3 import assembly, utils

    return assembly, utils


def isAsm3Document(doc):
    """Whether the document has Assembly3 assemblies"""
    return bool(findAssemblies(doc))


def findAssemblies(doc):
    """The Assembly3 assemblies of a document"""
    found = []
    for obj in doc.Objects:
        proxy = getattr(obj, "Proxy", None)
        cls = type(proxy)
        if cls.__name__ == "Assembly" and cls.__module__.endswith("asm3.assembly"):
            found.append(obj)
    return found


def _childrenFirst(assemblies):
    """An assembly before the ones whose parts hold it or a link to it"""
    asm, _utils = _asm3()

    def held(a):
        res = set()
        for part in asm.flattenGroup(a.Proxy.getPartGroup()):
            target = part.getLinkedObject(True)
            if target in assemblies:
                res.add(target)
        return res

    order = []
    todo = list(assemblies)
    while todo:
        ready = [a for a in todo if not (held(a) - set(order))]
        if not ready:
            # A cycle cannot be built from; take them as they come
            ready = todo[:1]
        for a in ready:
            order.append(a)
            todo.remove(a)
    return order


def _kind(shape):
    """'Vertex', 'Line' (a straight edge), 'Edge', 'Face', or the type"""
    _asm, utils = _asm3()
    if utils.isVertex(shape):
        return "Vertex"
    if utils.isLinearEdge(shape):
        return "Line"
    return shape.ShapeType


def _read(a, report):
    """Everything the conversion needs from one Assembly3 assembly, read
    before anything is changed"""
    asm, utils = _asm3()
    data = {
        "obj": a,
        "label": a.Label,
        "placement": a.Placement,
        "parts": [],
        "elements": [],
        "constraints": [],
    }
    for part in asm.flattenGroup(a.Proxy.getPartGroup()):
        if asm.isTypeOf(part, asm.AsmPlainGroup):
            report.add(
                a.Label, "folder %s: its parts are put directly in the assembly" % part.Label
            )
            data["parts"] += [o for o in asm.flattenGroup(part)]
        else:
            data["parts"].append(part)
    for element in asm.flattenGroup(a.Proxy.getElementGroup()):
        try:
            sub = element.Proxy.getElementSubname(False)
            data["elements"].append((element, element.Label, sub))
        except Exception as e:
            report.add(a.Label, "element %s: broken (%s)" % (element.Label, e))
    for cstr in asm.flattenGroup(a.Proxy.getConstraintGroup()):
        if not asm.isTypeOf(cstr, asm.AsmConstraint):
            continue
        sides = []
        broken = False
        multiply = bool(getattr(cstr, "Multiply", False))
        links = asm.flattenGroup(cstr)
        for n, link in enumerate(links):
            try:
                info = link.Proxy.getInfo()
                linked = link.LinkedObject
                if isinstance(linked, tuple):
                    linked = linked[0].getSubObject(linked[1], 1)
                side = {
                    "element": linked,
                    "part": info.Part,
                    "frame": info.Placement.multiply(utils.getElementPlacement(info.Shape)),
                    "kind": _kind(info.Shape),
                    "shapeType": info.Shape.ShapeType,
                }
                if multiply and n == 0:
                    side["autoCount"] = getattr(link, "AutoCount", True)
                    side["count"] = getattr(link, "Count", 0)
                if multiply and n >= 1 and getattr(link, "NoExpand", False):
                    # The targets picked by hand: this element and those of
                    # the element bindings after it, on the part of the first
                    side["noExpand"] = [info]
                    if n >= 2 and len(sides) == 2 and sides[1].get("noExpand"):
                        if _partOf(info) == _partOf(sides[1]["noExpand"][0]):
                            sides[1]["noExpand"].append(info)
                        else:
                            report.add(
                                a.Label,
                                "constraint %s: target %s on another part, dropped"
                                % (cstr.Label, link.Label),
                            )
                        continue
                if multiply and n == 1:
                    side["info"] = info
                sides.append(side)
            except Exception as e:
                report.add(
                    a.Label, "constraint %s: element %s broken (%s)" % (cstr.Label, link.Label, e)
                )
                broken = True
        if multiply and len(sides) > 2:
            report.add(
                a.Label,
                "constraint %s: multiplied over its second element's edges only" % cstr.Label,
            )
            del sides[2:]
        if len(sides) >= 2 and "info" in sides[1]:
            _pairedEdge(cstr, sides[0], sides[1].pop("info"), sides[1])
        props = {"Multiply": multiply}
        for name in (
            "Offset",
            "OffsetX",
            "OffsetY",
            "Distance",
            "Angle",
            "LockAngle",
            "Cascade",
            "Supplement",
            "Disabled",
        ):
            if hasattr(cstr, name):
                value = getattr(cstr, name)
                props[name] = getattr(value, "Value", value)
        data["constraints"].append(
            {
                "obj": cstr,
                "label": cstr.Label,
                "type": int(getattr(cstr, "_ConstraintType", -1)),
                "typeName": getattr(cstr, "ConstraintType", "?"),
                "props": props,
                "sides": sides,
                "broken": broken,
                "expressions": list(cstr.ExpressionEngine),
            }
        )
    return data


def _partOf(info):
    """The part an Assembly3 element info is on"""
    return info.Part[2] if isinstance(info.Part, tuple) else info.Part


def _sameGeometry(a, b):
    tol = 1e-7
    if a.ShapeType == "Vertex":
        return (a.Point - b.Point).Length < tol
    if a.ShapeType == "Edge":
        size = abs(a.Length - b.Length)
    else:
        size = abs(a.Area - b.Area)
    return size < tol and (a.CenterOfMass - b.CenterOfMass).Length < tol


def _wholeName(whole, shape):
    """The name in a part's whole shape of the geometry of an Assembly3
    element (an edge, a face or a vertex), found by where it is"""
    while shape.ShapeType == "Compound" and len(shape.SubShapes) == 1:
        shape = shape.SubShapes[0]
    kind = shape.ShapeType
    pool = {"Edge": whole.Edges, "Face": whole.Faces, "Vertex": whole.Vertexes}.get(kind)
    if pool is None:
        return None
    for i, s in enumerate(pool):
        if s.isSame(shape):
            return "%s%d" % (kind, i + 1)
    for i, s in enumerate(pool):
        if _sameGeometry(s, shape):
            return "%s%d" % (kind, i + 1)
    return None


def _pairedEdge(cstr, first, info, side):
    """A multiplied constraint's second side: the element of its part the
    first side's array element is paired with, as Assembly3 pairs them by
    position, its offset taken. The candidates are the edges like the one
    picked (a circle of its radius in its plane), looked for in the part's
    whole shape whether or not Assembly3 gathered them into its element; or,
    with NoExpand, the elements picked by hand, which become the joint's
    Targets. The side goes on the element, by its name in its part"""
    import Part

    _asm, utils = _asm3()

    def value(name):
        v = getattr(cstr, name, 0)
        return getattr(v, "Value", v)

    offset = App.Vector(value("OffsetX"), value("OffsetY"), value("Offset"))
    target = first["frame"].Base - first["frame"].Rotation.multVec(offset)
    part = _partOf(info)
    whole = Part.getShape(part, "", transform=False)
    candidates = []
    if side.get("noExpand"):
        names = []
        for picked in side.pop("noExpand"):
            name = _wholeName(whole, picked.Shape)
            if name is None:
                side.setdefault("lostTargets", []).append(picked.Subname)
            elif name not in names:
                names.append(name)
                candidates.append((name, whole.getElement(name)))
        side["targets"] = names
    else:
        picked = info.Shape.Edges[0] if info.Shape.Edges else None
        if picked is None or not hasattr(picked.Curve, "Radius"):
            return
        normal = picked.Curve.Axis
        for i, edge in enumerate(whole.Edges):
            curve = edge.Curve
            if not hasattr(curve, "Radius") or not hasattr(curve, "Axis"):
                continue
            if abs(curve.Radius - picked.Curve.Radius) > 1e-7:
                continue
            if abs(abs(curve.Axis.dot(normal)) - 1) > 1e-9:
                continue
            if abs((curve.Center - picked.Curve.Center).dot(normal)) > 1e-7:
                continue
            candidates.append(("Edge%d" % (i + 1), edge))
    best = None
    for name, shape in candidates:
        origin = utils.getElementPlacement(shape).Base
        d = (info.Placement.multVec(origin) - target).Length
        if best is None or d < best[0]:
            best = (d, name, shape)
    if best is None:
        return
    _d, name, shape = best
    side["direct"] = (part, name)
    side["frame"] = info.Placement.multiply(utils.getElementPlacement(shape))
    side["kind"] = _kind(shape)
    side["shapeType"] = shape.ShapeType


def _splitSubname(sub):
    """An Assembly3 element path relative to a part group, as (the part's
    name, the path below it)"""
    name, _dot, rest = sub.partition(".")
    return name, rest


def _component(name, partGroup, converted):
    obj = partGroup.Document.getObject(name)
    return converted.get(obj, obj)


def _translateRest(component, rest):
    """The rest of an Assembly3 path below a part, in ours: through a child
    assembly, its element group ('1.') is its Elements group ('$$Elements.')"""
    asm, _utils = _asm3()
    target = component.getLinkedObject(True)
    if (
        target.isDerivedFrom("Assembly::AssemblyObject")
        or asm.isTypeOf(target, asm.Assembly)
    ) and rest.startswith("1."):
        return "$$Elements." + rest[2:]
    return rest


def _matchZ(g1, g2):
    """g2 turned half about its X when its Z is against g1's: Assembly3's
    planes and axes align in either sense, a joint's frames in one"""
    z = App.Vector(0, 0, 1)
    if g1.Rotation.multVec(z).dot(g2.Rotation.multVec(z)) < 0:
        return g2.multiply(App.Placement(App.Vector(), App.Rotation(App.Vector(1, 0, 0), 180)))
    return g2


def _frames(jtype, g1, g2):
    """The two frames, in the assembly, a joint of the type is satisfied by
    at the saved poses: side 2's made to agree with side 1's where the type
    ties them, Assembly3's offsets folded in by that"""
    if jtype == "Fixed":
        return g1, g1
    if jtype in ("Revolute", "Ball"):
        g2 = _matchZ(g1, g2)
        return g1, App.Placement(g1.Base, g2.Rotation)
    if jtype == "Cylindrical":
        return g1, _matchZ(g1, g2)
    if jtype == "Slider":
        return g1, App.Placement(g2.Base, g1.Rotation)
    return g1, g2


def _pairs(sides, cascade):
    if len(sides) < 2:
        return []
    if cascade:
        return list(zip(sides[:-1], sides[1:]))
    return [(sides[0], s) for s in sides[1:]]


def _jointType(c):
    handler = CONSTRAINTS.get(c["type"])
    if handler == "PlaneCoincident":
        return "Fixed" if c["props"].get("LockAngle") else "Revolute"
    if handler == "AxialAlignment":
        return "Slider" if c["props"].get("LockAngle") else "Cylindrical"
    return handler


def _value(c, jtype):
    """The value an Assembly3 constraint gives the joint, by our name"""
    props = c["props"]
    if jtype == "Distance":
        if c["type"] == 37:
            return "Distance", props.get("Offset", 0.0)
        return "Distance", props.get("Distance", 0.0)
    if jtype == "Angle":
        angle = props.get("Angle", 0.0)
        if props.get("Supplement"):
            angle = 180 - angle
        return "Angle", angle
    return None, None


# What the conversion made and the label each takes once the Assembly3
# objects that hold the labels are gone
_LABELS = []


def _label(obj, label):
    obj.Label = label
    _LABELS.append((obj, label))


def _makeJoint(assembly, key, jtype, label, pair, elements, report):
    import JointObject
    import Part

    group = assembly.getGroup("Joints", True)
    joint = group.newObject("Assembly::JointPython", "Joint")
    JointObject.Joint(joint, JointObject.JointTypes.index(jtype))
    if App.GuiUp and joint.ViewObject is not None:
        JointObject.ViewProviderJoint(joint.ViewObject)
    _label(joint, label)
    for i, side in enumerate(pair, 1):
        if "direct" in side and side["element"] not in elements:
            component, sub = side["direct"]
            elements[side["element"]] = Part.makeElement(assembly, component, sub, "_Element")
        element = elements[side["element"]]
        link = Part.makeElementBinding(assembly, assembly.Elements, "$%s." % element.Label)
        setattr(joint, "Binding%d" % i, link)
    JointObject.recomputeSides(joint)
    frames = []
    for i, side in enumerate(pair, 1):
        # Where Assembly3's shape is not the geometry the element names (a
        # sketch element's is a solid), its frame is not that geometry's
        # either: ours is
        shape = elements[side["element"]].Shape
        theirs = side.get("shapeType")
        if theirs and not shape.isNull() and shape.ShapeType != theirs:
            report.add(
                key,
                "%s: side %d placed on %s, not Assembly3's %s"
                % (label, i, shape.ShapeType, theirs),
            )
            frames.append(joint.getFrame(i))
        else:
            frames.append(side["frame"])
    g1, g2 = _frames(jtype, frames[0], frames[1])
    for i, g in ((1, g1), (2, g2)):
        link = getattr(joint, "Binding%d" % i)
        link.Offset = joint.getFrame(i).inverse().multiply(g)
    JointObject.recomputeSides(joint)
    return joint


def _datum(assembly, label, shape):
    """A grounded datum for a lock on a vertex or a straight edge: the
    element's geometry where it is"""
    import JointObject

    datum = assembly.newObject("Part::Feature", "Datum")
    datum.Label = label
    datum.Shape = shape
    group = assembly.getGroup("Joints", True)
    ground = group.newObject("Assembly::GroundedJointPython", "GroundedJoint")
    JointObject.GroundedJoint(ground, datum)
    if App.GuiUp and ground.ViewObject is not None:
        JointObject.ViewProviderGroundedJoint(ground.ViewObject)
    return datum


def _ground(assembly, part, label):
    import JointObject

    group = assembly.getGroup("Joints", True)
    ground = group.newObject("Assembly::GroundedJointPython", "GroundedJoint")
    JointObject.GroundedJoint(ground, part)
    if App.GuiUp and ground.ViewObject is not None:
        JointObject.ViewProviderGroundedJoint(ground.ViewObject)
    _label(ground, label)
    return ground


def _build(data, converted, report):
    """One Assembly3 assembly as an AssemblyObject, its children built"""
    import Part

    asm, _utils = _asm3()
    a = data["obj"]
    doc = a.Document
    label = data["label"]
    new = doc.addObject("Assembly::AssemblyObject", "Assembly")
    _label(new, label)
    new.Placement = data["placement"]
    # Moved in as they are, not linked: they leave Assembly3's part group
    new.GroupMode = "Normal"
    partGroup = a.Proxy.getPartGroup()
    if hasattr(partGroup, "GroupMode"):
        # Assembly3's groups delete what leaves them ('Auto Delete')
        partGroup.GroupMode = 0
    parts = [converted.get(p, p) for p in data["parts"]]
    partGroup.Group = []
    new.Group = new.Group + parts
    new.getGroup("Joints", True)
    # A link array whose elements Assembly3 moves one by one: each a
    # component, its element shown
    for part in parts:
        if (
            part.isDerivedFrom("App::Link")
            and getattr(part, "ElementCount", 0)
            and not getattr(part, "ShowElement", True)
        ):
            part.ShowElement = True
            report.add(
                label, "link array %s: its elements are shown, each a component" % part.Label
            )

    # The elements, by label
    elements = {}
    for element, elabel, sub in data["elements"]:
        name, rest = _splitSubname(sub)
        component = _component(name, partGroup, converted)
        if component is None or component not in new.Group:
            report.add(label, "element %s: its part %s is not in the assembly" % (elabel, name))
            continue
        try:
            ours = Part.makeElement(new, component, _translateRest(component, rest), elabel)
            if ours.Label != elabel:
                ours.Label = elabel
            elements[element] = ours
        except Exception as e:
            report.add(label, "element %s: %s" % (elabel, e))
    # Each alone: a document's recompute would recompute the parts too, in
    # their own files
    for ours in elements.values():
        ours.recompute()

    # The constraints
    locked = False
    firstPart = None
    count = 0
    pending = []
    for c in data["constraints"]:
        tid = c["type"]
        jtype = _jointType(c)
        for s in c["sides"]:
            if "direct" in s:
                # On its own edge, not the Assembly3 element's: a key of its own
                s["element"] = ("direct",) + tuple(s["direct"])
        sides = [s for s in c["sides"] if s["element"] in elements or "direct" in s]
        if firstPart is None and sides and sides[0]["element"] in elements:
            firstPart = elements[sides[0]["element"]]
        if c["broken"] or len(sides) != len(c["sides"]):
            report.add(label, "constraint %s: not converted, an element is broken" % c["label"])
            continue
        if jtype is None:
            report.add(
                label,
                "constraint %s (%s): no counterpart, dropped" % (c["label"], c["typeName"]),
            )
            continue
        if tid in APPROXIMATE:
            report.add(label, "constraint %s: approximate: %s" % (c["label"], APPROXIMATE[tid]))
        made = []
        if jtype == "Locked":
            locked = True
            for side in sides:
                made += _lock(new, label, c, side, elements, report)
        else:
            pairs = _pairs(sides, c["props"].get("Cascade"))
            if c["props"].get("Multiply"):
                pairs = pairs[:1]
            for n, pair in enumerate(pairs):
                if pair[0]["part"] == pair[1]["part"]:
                    continue  # as Assembly3 skips them
                jlabel = c["label"] if len(pairs) == 1 else "%s_%d" % (c["label"], n + 1)
                joint = _makeJoint(new, label, jtype, jlabel, pair, elements, report)
                prop, value = _value(c, jtype)
                if prop:
                    setattr(joint, prop, value)
                    pending.append((joint, prop, value))
                if c["props"].get("Multiply"):
                    _multiply(joint, label, c, pair, report)
                made.append(joint)
        for joint in made:
            if c["props"].get("Disabled") and hasattr(joint, "Suppressed"):
                joint.Suppressed = True
        for path, expr in c["expressions"]:
            report.add(
                label,
                "constraint %s: the expression on %s is not carried over" % (c["label"], path),
            )
        count += len(made)
    if not locked and firstPart is not None:
        part = _componentOf(firstPart, new)
        if part is not None:
            _ground(new, part, "GroundedJoint")
            report.add(label, "no lock: %s grounded, as Assembly3 fixes it" % part.Label)
    report.add(label, "%d joint(s) from %d constraint(s)" % (count, len(data["constraints"])))
    new.GroupMode = "Auto"
    return new, pending


def _multiply(joint, key, c, pair, report):
    """A multiplied constraint as one multiplied joint (sec 7.4): its pairs
    as Assembly3 has them, the solved pose putting each element by its edge.
    Assembly3's NoExpand targets are the joint's Targets"""
    first, second = pair
    for lost in second.get("lostTargets", []):
        report.add(key, "constraint %s: target %s not found, dropped" % (c["label"], lost))
    joint.AutoCount = bool(first.get("autoCount", True))
    if second.get("targets"):
        joint.Targets = second["targets"]
    joint.Multiply = True
    joint.recompute()
    count = len(joint._Instances)
    if not count:
        report.add(key, "constraint %s: not multiplied: %s" % (c["label"], joint.State))
    elif first.get("count") and count != first["count"]:
        report.add(
            key,
            "constraint %s: %d pairs, where Assembly3 has %d" % (c["label"], count, first["count"]),
        )


def _componentOf(element, assembly):
    """The component an element is on: a direct child of the assembly, or the
    element of a link array its path steps into, as a joint takes it"""
    target, sub = element.Target, ""
    if isinstance(target, tuple):
        target, subs = target
        sub = subs[0] if subs else ""
    if target not in assembly.Group:
        return None
    if target.isDerivedFrom("App::Link") and getattr(target, "ElementCount", 0) and "." in sub:
        child = target.getSubObject(sub.split(".")[0] + ".", retType=1)
        if child is not None and child in target.ElementList:
            return child
    return target


def _lock(assembly, key, c, side, elements, report):
    """Locked on one element: the part grounded on a face or curved edge, a
    ball or revolute to a grounded datum on a vertex or straight edge"""
    import Part

    element = elements[side["element"]]
    part = _componentOf(element, assembly)
    if part is None:
        report.add(key, "constraint %s: no part to lock" % c["label"])
        return []
    if side["kind"] not in ("Vertex", "Line"):
        return [_ground(assembly, part, c["label"])]
    g = side["frame"]
    if side["kind"] == "Vertex":
        shape = Part.Vertex(g.Base)
        jtype = "Ball"
    else:
        z = g.Rotation.multVec(App.Vector(0, 0, 1))
        shape = Part.LineSegment(g.Base - z * 5, g.Base + z * 5).toShape()
        jtype = "Revolute"
    datum = _datum(assembly, c["label"] + "_Datum", shape)
    datum.recompute()
    sub = "Vertex1" if side["kind"] == "Vertex" else "Edge1"
    datumElement = Part.makeElement(assembly, datum, sub, "_" + c["label"])
    elements[datum] = datumElement
    pair = ({"element": datum, "frame": g}, side)
    return [_makeJoint(assembly, key, jtype, c["label"], pair, elements, report)]


def _placements(assemblies):
    res = {}
    for a in assemblies:
        for part in a.Group:
            if (
                hasattr(part, "Placement")
                and not part.isDerivedFrom("Part::ElementBinding")
                and not part.isDerivedFrom("App::AuxGroup")
            ):
                res[part] = part.Placement
    return res


def _moved(before):
    worst = 0.0
    names = []
    for part, plc in before.items():
        now = part.Placement
        d = max((now.Base - plc.Base).Length, abs(now.Rotation.Angle - plc.Rotation.Angle))
        if not now.isSame(plc, TOLERANCE):
            names.append(part.Label)
        worst = max(worst, d)
    return worst, names


def _settle(assembly, key, pending, report):
    """Solve once; where it moves the parts, a distance or an angle read in
    the other sense or as its supplement may hold them, as the sign
    conventions differ: keep what moves the least"""
    before = _placements([assembly])

    def trial():
        for part, plc in before.items():
            part.Placement = plc
        # The joints resolve their sides live: no recompute of the parts
        assembly.solve()
        worst, _names = _moved(before)
        return worst

    worst = trial()
    for joint, prop, value in pending:
        if worst <= TOLERANCE:
            break
        candidates = [-value]
        if prop == "Angle":
            candidates += [180 - value, value - 180]
        best = value
        for candidate in candidates:
            setattr(joint, prop, candidate)
            w = trial()
            if w < worst - TOLERANCE:
                worst, best = w, candidate
        setattr(joint, prop, best)
    trial()
    worst, names = _moved(before)
    if names:
        report.add(key, "a solve moves %s (up to %.3g)" % (", ".join(names), worst))
    return worst


def _removeAsm3(datas):
    """The Assembly3 objects, the parts having left them"""
    doc = None
    names = []
    for data in datas:
        a = data["obj"]
        doc = a.Document
        for group in (
            a.Proxy.getConstraintGroup(),
            a.Proxy.getElementGroup(),
            getattr(a.Proxy, "getRelationGroup", lambda: None)(),
            a.Proxy.getPartGroup(),
        ):
            if group is None:
                continue
            for obj in group.OutListRecursive:
                if _isAsm3(obj):
                    names.append(obj.Name)
            names.append(group.Name)
        names.append(a.Name)
    for name in names:
        obj = doc.getObject(name)
        if obj is not None:
            try:
                doc.removeObject(name)
            except Exception:
                pass


def _isAsm3(obj):
    cls = type(getattr(obj, "Proxy", None))
    return cls.__module__.startswith("freecad.asm3")


def convert(doc, report=None):
    """Convert every Assembly3 assembly of the document, in place, as one
    transaction. Returns the Report"""
    if report is None:
        report = Report()
    assemblies = findAssemblies(doc)
    if not assemblies:
        return report
    asm, _utils = _asm3()
    try:
        from freecad.asm3 import gui as asmGui

        autoRecompute = asmGui.AsmCmdManager.AutoRecompute
        asmGui.AsmCmdManager.AutoRecompute = False
    except Exception:
        asmGui = None
    params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
    solveInCreation = params.GetBool("SolveInJointCreation", True)
    params.SetBool("SolveInJointCreation", False)
    doc.openTransaction("Convert Assembly3")
    del _LABELS[:]
    try:
        order = _childrenFirst(assemblies)
        datas = [_read(a, report) for a in order]
        converted = {}
        built = []
        for data in datas:
            new, pending = _build(data, converted, report)
            old = data["obj"]
            converted[old] = new
            # A link to the old assembly is a link to the new one (its value
            # may come with a subname)
            for obj in doc.Objects:
                if not obj.isDerivedFrom("App::Link"):
                    continue
                linked = obj.LinkedObject
                if isinstance(linked, tuple):
                    linked = linked[0]
                if linked == old:
                    obj.LinkedObject = new
            built.append((new, pending, data["label"]))
        _removeAsm3(datas)
        for obj, label in _LABELS:
            obj.Label = label
        for new, pending, label in built:
            _settle(new, label, pending, report)
        doc.commitTransaction()
    except Exception:
        doc.abortTransaction()
        raise
    finally:
        del _LABELS[:]
        params.SetBool("SolveInJointCreation", solveInCreation)
        if asmGui is not None:
            asmGui.AsmCmdManager.AutoRecompute = autoRecompute
    return report
