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
over (App/AppAssembly.cpp). For now it converts upstream's joints
(JointObject.Joint, a Python feature) to Assembly::JointPython; grounding
and rigid groups stay as they are.

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


def convertDocument(doc, force=False):
    """Convert upstream's joints in every assembly of a document.

    Off when the Assembly preference ConvertJointsOnOpen is (unless force).
    Returns a list of (label, new joint or None, note).
    """
    if not force and not App.ParamGet(PARAM).GetBool("ConvertJointsOnOpen", True):
        return []
    import UtilsAssembly

    report = []
    for assembly in doc.Objects:
        if not assembly.isDerivedFrom("Assembly::AssemblyObject"):
            continue
        group = UtilsAssembly.getJointGroup(assembly)
        if group is None:
            continue
        for joint in [o for o in group.Group if isPythonJoint(o)]:
            label = joint.Label
            try:
                new, note = convertJoint(assembly, joint)
            except Exception as e:
                new, note = None, str(e)
            report.append((label, new, note))
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
    return report
