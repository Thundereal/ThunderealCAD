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

"""Upstream's assembly objects to this fork's C++ ones, when a file is opened.

docs/AssemblyObjectModel.md sec 8. The Assembly module calls
convertDocument() for each document a file open restored, once the open is
over (App/AppAssembly.cpp). It converts upstream's joints
(JointObject.Joint, a Python feature) to Assembly::JointPython, and its
simulations, motions, exploded views, steps and snapshots to their C++
twins (TWINS); grounding and rigid groups stay as they are.

A side converts to one element link, with Offset = F^-1 * G: G upstream's
frame as it is (its PlacementN on the referenced object, which carries its
OffsetN, or the user's own when DetachN), F the element's frame. So a joint
keeps its frames exactly and a re-solve moves nothing. The vertex upstream
picks with the element is the element link's Vertex, kept live, with
VertexPosition where it is in F -- except where upstream does not take the
frame from it: a Distance or Cam joint, a detached side, a vertex that is
the element itself.

A joint that cannot be converted stays a Python joint, which still solves,
and is reported: a side on a component that is not a direct child of the
assembly (one inside a sub-assembly), or a side not set.
"""

import FreeCAD as App

# The properties of upstream's joint that the C++ one has under the same
# name and meaning; their expressions go with them
COPIED = (
    "Distance",
    "Distance2",
    "Angle",
    "EnableLengthMin",
    "EnableLengthMax",
    "EnableAngleMin",
    "EnableAngleMax",
    "LengthMin",
    "LengthMax",
    "AngleMin",
    "AngleMax",
    "Suppressed",
    "Label2",
)

PARAM = "User parameter:BaseApp/Preferences/Mod/Assembly"


def isPythonJoint(obj):
    """Whether obj is upstream's joint: a Python feature with two references"""
    return (
        obj.isDerivedFrom("App::FeaturePython")
        and not obj.isDerivedFrom("Assembly::Joint")
        and all(hasattr(obj, p) for p in ("JointType", "Reference1", "Reference2", "Placement1"))
    )


def _plain(sub):
    import Part

    path, _mapped, element = Part.splitSubname(sub or "")
    return path + element


def _sides(assembly, joint):
    """Upstream's sides as (component, element, vertex, global frame, detached),
    or a reason they cannot be converted"""
    import UtilsAssembly

    children = set(o.Name for o in assembly.Group)
    sides = []
    for i in (1, 2):
        ref = getattr(joint, "Reference%d" % i)
        if not UtilsAssembly.isRefValid(ref, 1):
            return None, "side %d is not set" % i
        comp = ref[0]
        if comp.Document != assembly.Document or comp.Name not in children:
            return None, "side %d is on %s, which is not a component of the assembly" % (
                i,
                comp.Label,
            )
        subs = list(ref[1])
        element = subs[0] if subs else ""
        vertex = subs[1] if len(subs) > 1 else element
        frame = UtilsAssembly.getJcsGlobalPlc(getattr(joint, "Placement%d" % i), ref)
        sides.append((comp, element, vertex, frame, bool(getattr(joint, "Detach%d" % i, False))))
    return sides, ""


def convertJoint(assembly, joint):
    """Convert one of upstream's joints; returns (new joint, "") or (None, reason)"""
    import JointObject
    import Part
    import UtilsAssembly

    sides, reason = _sides(assembly, joint)
    if sides is None:
        return None, reason
    group = UtilsAssembly.getJointGroup(assembly)
    if group is None:
        return None, "the assembly has no joint group"

    jtype = joint.JointType
    if jtype not in JointObject.JointTypes:
        return None, "the joint type %s is not known" % jtype
    doc = joint.Document
    new = group.newObject("Assembly::JointPython", joint.Name)
    JointObject.Joint(new, JointObject.JointTypes.index(jtype))
    if App.GuiUp:
        JointObject.ViewProviderJoint(new.ViewObject)

    ignoreVertex = jtype in ("Distance", "Cam")
    toAssembly = assembly.getGlobalPlacement().inverse()
    for i, (comp, element, vertex, frame, detached) in enumerate(sides, 1):
        try:
            # Local: a migration does not edit the components, which may be
            # other documents
            link = new.setSide(i, comp, element, "", True)
        except Exception as e:
            doc.removeObject(new.Name)
            return None, "side %d: %s" % (i, e)
        JointObject.recomputeSides(new)
        F = new.getFrame(i)
        G = toAssembly * frame
        if not (ignoreVertex or detached or _plain(vertex) == _plain(element)):
            try:
                point = Part.getShape(comp, vertex, needSubElement=True)
                if point.ShapeType == "Vertex":
                    link.Vertex = (comp, [vertex])
                    link.VertexPosition = F.inverse().multVec(point.Point)
            except Exception:
                pass
        link.Offset = F.inverse().multiply(G)

    for prop in COPIED:
        if hasattr(joint, prop) and hasattr(new, prop):
            try:
                setattr(new, prop, getattr(joint, prop))
            except Exception:
                pass
    lost = []
    for path, expr in joint.ExpressionEngine:
        if path.split(".")[0] in COPIED:
            new.setExpression(path, expr)
        else:
            lost.append(path)
    new.Visibility = joint.Visibility

    import AssemblyApp

    AssemblyApp.replaceObjectLinks(joint, new)
    JointObject.recomputeSides(new)

    # In its place among the joints, and by its name for whoever reads labels
    label = joint.Label
    members = list(group.Group)
    index = members.index(joint) if joint in members else -1
    doc.removeObject(joint.Name)
    new.Label = label
    if index >= 0:
        members = [o for o in group.Group if o != new]
        members.insert(min(index, len(members)), new)
        group.Group = members
    if lost:
        return new, "the expressions on %s are not carried over" % ", ".join(lost)
    return new, ""


# Upstream's other Python objects that have a C++ twin (sec 7.2), by the
# class of their proxy: (module, C++ type, the properties copied, a name or
# (upstream's name, the twin's name)). Containers come last, so their
# members are converted when their group is made
TWINS = (
    (
        "GroundedJoint",
        "JointObject",
        "Assembly::GroundedJointPython",
        ("ObjectToGround", "Label2"),
    ),
    (
        "RigidGroupJoint",
        "JointObject",
        "Assembly::RigidGroupPython",
        ("ObjectsToRigidGroup", "RigidPlacements", "Suppressed", "Label2"),
    ),
    (
        "Motion",
        "CommandCreateSimulation",
        "Assembly::MotionPython",
        ("MotionType", "Formula", "Label2"),
    ),
    (
        "ExplodedViewStep",
        "CommandCreateView",
        "Assembly::ExplodedViewStepPython",
        ("References", "MovementTransform", "MoveType", "Label2"),
    ),
    (
        "Snapshot",
        "CommandCreateSnapshot",
        "Assembly::SnapshotPython",
        ("Components", "Placements", "Visibilities", "SolveOnActivation", "Label2"),
    ),
    (
        "Simulation",
        "CommandCreateSimulation",
        "Assembly::SimulationPython",
        (
            ("aTimeStart", "TimeStart"),
            ("bTimeEnd", "TimeEnd"),
            ("cTimeStepOutput", "TimeStepOutput"),
            ("fGlobalErrorTolerance", "GlobalErrorTolerance"),
            ("jFramesPerSecond", "FramesPerSecond"),
            "Label2",
        ),
    ),
    ("ExplodedView", "CommandCreateView", "Assembly::ExplodedViewPython", ("Label2",)),
)

# The view provider class of each, in the same module
VIEW_PROVIDERS = {
    "GroundedJoint": "ViewProviderGroundedJoint",
    "RigidGroupJoint": "ViewProviderRigidGroupJoint",
    "Motion": "ViewProviderMotion",
    "ExplodedViewStep": "ViewProviderExplodedViewStep",
    "Snapshot": "ViewProviderSnapshot",
    "Simulation": "ViewProviderSimulation",
    "ExplodedView": "ViewProviderExplodedView",
}


def twinOf(obj):
    """The TWINS entry of one of upstream's Python objects, or None"""
    if obj.TypeId != "App::FeaturePython":
        return None
    proxy = getattr(obj, "Proxy", None)
    cls = type(proxy)
    for entry in TWINS:
        if cls.__name__ == entry[0] and cls.__module__ == entry[1]:
            return entry
    return None


def _members(assembly):
    """Every object the assembly holds, its groups' members included"""
    seen = []
    todo = list(assembly.Group)
    while todo:
        obj = todo.pop(0)
        if obj in seen:
            continue
        seen.append(obj)
        if not obj.isDerivedFrom("Assembly::AssemblyObject") and hasattr(obj, "Group"):
            todo += [o for o in obj.Group if o not in seen]
    return seen


def _plainGroupOf(obj, assembly):
    """The group that lists obj, other than the assembly itself, or None"""
    for parent in obj.InList:
        if parent != assembly and obj in getattr(parent, "Group", []):
            if not parent.hasExtension("App::GeoFeatureGroupExtension"):
                return parent
    return None


def convertTwin(assembly, old, entry, converted, members=None):
    """Convert one of upstream's objects in TWINS to its C++ twin; converted
    maps the names of the members already converted to their twins, and
    members is a container's Group by name as it was before them (deleting
    one takes it out). Returns the new object"""
    import importlib

    name, moduleName, typeName, props = entry
    module = importlib.import_module(moduleName)
    doc = old.Document

    parent = _plainGroupOf(old, assembly)
    siblings = list(parent.Group) if parent is not None else []
    index = siblings.index(old) if old in siblings else -1
    if parent is not None and twinOf(parent) is None:
        new = parent.newObject(typeName, old.Name)
    else:
        # In a container that is converted after it, which lists it then
        new = assembly.newObject(typeName, old.Name)

    renames = dict(p if isinstance(p, tuple) else (p, p) for p in props)
    for prop, newProp in renames.items():
        if hasattr(old, prop) and hasattr(new, newProp):
            _copyValue(old, prop, new, newProp)
    # As a file restores a proxy: the class, not its __init__, which would
    # set defaults (and a snapshot capture the state now). After the values:
    # a rigid group's proxy stores its members' positions when they are set,
    # and moves them and solves when it is unsuppressed
    cls = getattr(module, name)
    new.Proxy = cls.__new__(cls)
    if name == "Motion":
        new.Joint = _motionJoint(old)
    for path, expr in old.ExpressionEngine:
        prop, dot, rest = path.partition(".")
        if prop in renames:
            new.setExpression(renames[prop] + dot + rest, expr)
    if hasattr(new, "Group"):
        if members is None:
            members = [o.Name for o in old.Group]
        group = [converted.get(n) or doc.getObject(n) for n in members]
        group = [o for o in group if o is not None]
        old.Group = []
        new.Group = group
    new.Visibility = old.Visibility
    if App.GuiUp and new.ViewObject is not None:
        vp = getattr(module, VIEW_PROVIDERS[name], None)
        if vp is not None:
            vp(new.ViewObject)

    import AssemblyApp

    AssemblyApp.replaceObjectLinks(old, new)
    # What named a property renamed: the other objects' expressions, and the
    # new object's own, which say old's name when they named its properties
    renamed = {p: n for p, n in renames.items() if p != n}
    if renamed:
        AssemblyApp.renamePropertyReferences(new, renamed)
    label = old.Label
    doc.removeObject(old.Name)
    new.Label = label
    # And what a restore runs once the proxy is in, after the old object is
    # gone: deleting a grounding frees its part's placement, which the new
    # one locks again (else syncGroundedJoints() takes it for released)
    if hasattr(new.Proxy, "onDocumentRestored"):
        new.Proxy.onDocumentRestored(new)
    if parent is not None and index >= 0 and twinOf(parent) is None:
        siblings = [o for o in parent.Group if o != new]
        siblings.insert(min(index, len(siblings)), new)
        parent.Group = siblings
    return new


def _copyValue(old, prop, new, newProp):
    """A value from one object to the other, read-only as it may be (a rigid
    group's RigidPlacements)"""
    readOnly = "ReadOnly" in new.getPropertyStatus(newProp)
    if readOnly:
        new.setPropertyStatus(newProp, "-ReadOnly")
    try:
        setattr(new, newProp, getattr(old, prop))
    except Exception:
        pass
    finally:
        if readOnly:
            new.setPropertyStatus(newProp, "ReadOnly")


def _motionJoint(motion):
    joint = motion.Joint
    if isinstance(joint, tuple):
        return joint[0] if joint else None
    return joint


def convertTwins(assembly):
    """Convert upstream's simulations, motions, exploded views, steps and
    snapshots in an assembly. Returns a list of (label, new or None, note)"""
    report = []
    converted = {}
    # Sorted out before any is converted: a converted one is deleted
    todo = [(o, twinOf(o)) for o in _members(assembly)]
    # By name: an object deleted is no key to look up
    groups = {
        o.Name: [m.Name for m in o.Group] for o, e in todo if e is not None and hasattr(o, "Group")
    }
    for entry in TWINS:
        for old in [o for o, e in todo if e is entry]:
            label, name = old.Label, old.Name
            try:
                new = convertTwin(assembly, old, entry, converted, groups.get(name))
                converted[name] = new
                report.append((label, new, ""))
            except Exception as e:
                report.append((label, None, str(e)))
    return report


def convertDocument(doc, force=False):
    """Convert upstream's joints, simulations, motions, exploded views, steps
    and snapshots in every assembly of a document.

    Off when the Assembly preference ConvertJointsOnOpen is (unless force).
    Returns a list of (label, new object or None, note).
    """
    if not force and not App.ParamGet(PARAM).GetBool("ConvertJointsOnOpen", True):
        return []
    import UtilsAssembly

    report = []
    others = []
    for assembly in [o for o in doc.Objects if o.isDerivedFrom("Assembly::AssemblyObject")]:
        group = UtilsAssembly.getJointGroup(assembly)
        if group is not None:
            for joint in [o for o in group.Group if isPythonJoint(o)]:
                label = joint.Label
                try:
                    new, note = convertJoint(assembly, joint)
                except Exception as e:
                    new, note = None, str(e)
                report.append((label, new, note))
        # After the joints: a motion's joint is the converted one by then
        others += convertTwins(assembly)
    converted = [r for r in report if r[1] is not None]
    if converted:
        App.Console.PrintMessage(
            "%s: %d joint(s) converted to the C++ joint\n" % (doc.Label, len(converted))
        )
    for label, new, note in report:
        if new is None:
            App.Console.PrintWarning(
                "%s: joint %s kept as it is: %s\n" % (doc.Label, label, note)
            )
        elif note:
            App.Console.PrintWarning("%s: joint %s: %s\n" % (doc.Label, label, note))
    converted = [r for r in others if r[1] is not None]
    if converted:
        App.Console.PrintMessage(
            "%s: %d simulation, motion, view or snapshot object(s) converted to C++\n"
            % (doc.Label, len(converted))
        )
    for label, new, note in others:
        if new is None:
            App.Console.PrintWarning("%s: %s kept as it is: %s\n" % (doc.Label, label, note))
    return report + others
