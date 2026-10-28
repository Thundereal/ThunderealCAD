# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# **************************************************************************/

"""Spring / Damper: a spring and damper between two picked points of the
active assembly's components (docs/AssemblyDynamics.md sec 9, phase D1)."""

import FreeCAD as App

from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui

import UtilsAssembly

translate = App.Qt.translate


def pickedEnd(assembly, sel, index):
    """(component, point in its coordinates) of a selected element: a
    vertex's point, a circle's centre, a face's or an edge's centre of mass,
    else where it was picked."""
    import Part

    root = sel.Object
    sub = sel.SubElementNames[index]
    component, _ = UtilsAssembly.getComponentReference(assembly, root, sub)
    if component is None:
        return None, None
    shape = Part.getShape(root, sub, needSubElement=True, transform=True)
    point = None
    if shape.ShapeType == "Vertex":
        point = shape.Point
    elif shape.ShapeType == "Edge" and hasattr(shape.Curve, "Center"):
        point = shape.Curve.Center
    elif shape.ShapeType in ("Edge", "Face"):
        point = shape.CenterOfMass
    elif index < len(sel.PickedPoints):
        point = sel.PickedPoints[index]
    if point is None:
        return None, None
    # In the component's coordinates
    return component, component.getGlobalPlacement().inverse().multVec(point)


class CommandCreateSpring:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_SpringDamper",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateSpring", "Spring / Damper"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateSpring",
                "A spring and damper between two picked points of the assembly's "
                "components,\nfor a dynamic simulation. Select the two points first.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive()

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly:
            return
        ends = []
        for sel in Gui.Selection.getSelectionEx("", 0):
            for i in range(len(sel.SubElementNames)):
                ends.append(pickedEnd(assembly, sel, i))
        ends = [e for e in ends if e[0] is not None]
        if len(ends) != 2:
            App.Console.PrintWarning(
                translate(
                    "Assembly",
                    "Spring / Damper: select two points on the assembly's components, "
                    "a vertex, an edge or a face each.\n",
                )
            )
            return
        spring = makeSpring(assembly, ends[0], ends[1])
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(spring)


def makeSpring(assembly, end1, end2, stiffness="1 N/mm"):
    """A spring between two ends, each (component, point in its coordinates),
    free at the distance between them, in a transaction; ticked in every
    Dynamic study of the assembly."""
    doc = assembly.Document
    doc.openTransaction(translate("Assembly", "Create spring"))
    try:
        physics = assembly.getGroup("Physics", True)
        spring = physics.newObject("Assembly::SpringDamper", "Spring")
        spring.Component1, spring.Point1 = end1
        spring.Component2, spring.Point2 = end2
        spring.Stiffness = stiffness
        a = end1[0].getGlobalPlacement().multVec(end1[1])
        b = end2[0].getGlobalPlacement().multVec(end2[1])
        spring.FreeLength = (b - a).Length
        sims = assembly.getGroup("Simulations")
        for sim in sims.Group if sims else []:
            if getattr(sim, "AnalysisType", None) in ("Dynamic", "Static"):
                sim.Elements = list(sim.Elements) + [spring]
    finally:
        doc.commitTransaction()
    return spring


# The joints a torsion spring turns with
TorsionJointTypes = ("Revolute", "Cylindrical")


def selectedTorsionJoint():
    """The one Revolute or Cylindrical joint selected, else None"""
    joints = [
        o
        for o in Gui.Selection.getSelection()
        if getattr(o, "JointType", None) in TorsionJointTypes
    ]
    return joints[0] if len(joints) == 1 else None


class CommandCreateTorsionSpring:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_TorsionSpring",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateTorsionSpring", "Torsion Spring"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateTorsionSpring",
                "A torsion spring and damper on a revolute or cylindrical joint, resisting "
                "its turn,\nfor a dynamic or static simulation. Select the joint first.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive() and selectedTorsionJoint() is not None

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        joint = selectedTorsionJoint()
        if not assembly or joint is None:
            return
        spring = makeTorsionSpring(assembly, joint)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(spring)


def makeTorsionSpring(assembly, joint, stiffness="1 N*m"):
    """A torsion spring on a Revolute or Cylindrical joint, 1 N m per radian
    and free where the run starts, in a transaction; ticked in every Dynamic
    and Static study of the assembly."""
    doc = assembly.Document
    doc.openTransaction(translate("Assembly", "Create torsion spring"))
    try:
        physics = assembly.getGroup("Physics", True)
        spring = physics.newObject("Assembly::TorsionSpring", "TorsionSpring")
        spring.Joint = joint
        spring.Stiffness = stiffness
        sims = assembly.getGroup("Simulations")
        for sim in sims.Group if sims else []:
            if getattr(sim, "AnalysisType", None) in ("Dynamic", "Static"):
                sim.Elements = list(sim.Elements) + [spring]
    finally:
        doc.commitTransaction()
    return spring


if App.GuiUp:
    Gui.addCommand("Assembly_CreateSpring", CommandCreateSpring())
    Gui.addCommand("Assembly_CreateTorsionSpring", CommandCreateTorsionSpring())
