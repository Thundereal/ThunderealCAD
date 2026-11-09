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

"""Bushing: a component held to another, or to the ground, at a frame by a
spring and a damper along and about each of the frame's axes, in the active
assembly's dynamic and static studies (docs/AssemblyDynamics.md sec 5.2's
Bushing). The frame is an element's, picked as a joint's side is, through an
element binding the bushing owns; or a free one. The command opens the
bushing's task panel, which a double click on a bushing opens too."""

import FreeCAD as App

from PySide import QtWidgets
from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui

import CommandCreateLoad
import CommandCreateSpring
import UtilsAssembly

translate = App.Qt.translate

# The bushing's values: its properties, the unit each is shown in, the
# factor from that unit to the internal one, and a new bushing's value -- a
# rubber mount of a few kilograms, ringing at some tens of hertz, damped by
# a fraction of the critical. Shown in plain spin boxes: a quantity box
# shows a value in the unit schema's choice, a torque stiffness as joules
# and a zero torque as electron volts.
Values = {
    "TranslationalStiffness": ("N/mm", 1e3, 1e3),
    "RotationalStiffness": ("N m/rad", 1e6, 1e2),
    "TranslationalDamping": ("N s/mm", 1e3, 0.5),
    "RotationalDamping": ("N m s/rad", 1e6, 0.05),
    "Preload": ("N", 1e3, 0.0),
    "PreloadTorque": ("N m", 1e6, 0.0),
}


def newBushing(assembly):
    """A bushing on nothing yet in the assembly's Physics group, ticked in
    every Dynamic and Static study of it"""
    physics = assembly.getGroup("Physics", True)
    bushing = physics.newObject("Assembly::Bushing", "Bushing")
    for prop, (_, factor, value) in Values.items():
        setattr(bushing, prop, App.Vector(value, value, value) * factor)
    sims = assembly.getGroup("Simulations")
    for sim in sims.Group if sims else []:
        if getattr(sim, "AnalysisType", None) in ("Dynamic", "Static"):
            sim.Elements = list(sim.Elements) + [bushing]
    return bushing


def makeBushing(assembly, end, other=None):
    """A bushing in a transaction: end is (component, Placement in its
    coordinates) for a free frame or (component, subname[, vertex]) for an
    element; other the component holding it, None the ground"""
    doc = assembly.Document
    doc.openTransaction(translate("Assembly", "Create bushing"))
    try:
        bushing = newBushing(assembly)
        component, where = end[0], end[1]
        if isinstance(where, App.Placement):
            bushing.Component = component
            bushing.Frame = where
        else:
            bushing.setEnd(component, where, end[2] if len(end) > 2 else "")
        bushing.Other = other
        CommandCreateLoad.recomputeFramed(bushing)
    finally:
        doc.commitTransaction()
    return bushing


def editBushing(bushing):
    """Open the task panel of a bushing, as a double click does"""
    if Gui.Control.activeDialog():
        App.Console.PrintWarning(
            translate("Assembly", "Close the open task panel before editing the bushing.\n")
        )
        return
    Gui.Control.showDialog(TaskBushing(bushing))


class CommandCreateBushing:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_Bushing",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateBushing", "Bushing"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateBushing",
                "A component held to another or to the ground by springs and dampers along\n"
                "and about three axes, for a dynamic or static simulation, at an element\n"
                "picked as for a joint, before or in the panel.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive() and not Gui.Control.activeDialog()

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly:
            return
        pick = None
        for sel in Gui.Selection.getSelectionEx("", 0):
            for i, sub in enumerate(sel.SubElementNames):
                pos = sel.PickedPoints[i] if i < len(sel.PickedPoints) else None
                pick = pick or CommandCreateSpring.pickOf(assembly, sel.Object, sub, pos)
        Gui.Selection.clearSelection()
        doc = assembly.Document
        doc.openTransaction(translate("Assembly", "Create bushing"))
        try:
            bushing = newBushing(assembly)
            if pick is not None:
                bushing.setEnd(*pick)
                CommandCreateLoad.recomputeFramed(bushing)
            Gui.Control.showDialog(TaskBushing(bushing, creating=True))
        except Exception:
            # The panel's Cancel would have undone it
            doc.abortTransaction()
            raise


# The open bushing panel, as JointObject.activeTask is the joint's
activeTask = None


class TaskBushing(CommandCreateLoad.TaskFramed):
    """The task panel of a bushing: where it is; its stiffness, damping and
    preload along and about each of the frame's axes; and the component
    holding it, or the ground."""

    title = translate("Assembly", "Bushing")
    editText = translate("Assembly", "Edit bushing")
    otherProp = "Other"
    texts = {
        "at": translate("Assembly", "At"),
        "pickTip": translate(
            "Assembly",
            "Pick an element in the 3D view: a vertex, an edge or a face, the bushing\n"
            "at its vertex nearest the cursor or its centre, in its frame",
        ),
        "pickHint": translate("Assembly", "Pick the element the bushing is at in the 3D view"),
        "other": translate("Assembly", "Held by"),
        "otherTip": translate("Assembly", "Pick the part holding the bushing, in the 3D view"),
        "otherHint": translate("Assembly", "Pick the part holding the bushing in the 3D view"),
        "groundTip": translate("Assembly", "The ground holds the bushing"),
        "onePart": translate("Assembly", "Bushing: both its ends are on one part."),
    }

    # The grid: a row per axis, a column per property pair (along, about)
    Columns = [
        ("TranslationalStiffness", "RotationalStiffness"),
        ("TranslationalDamping", "RotationalDamping"),
        ("Preload", "PreloadTorque"),
    ]

    def buildItem(self):
        group = QtWidgets.QGroupBox(translate("Assembly", "Bushing"))
        form = QtWidgets.QFormLayout(group)
        grid = QtWidgets.QGridLayout()
        heads = [
            translate("Assembly", "Stiffness"),
            translate("Assembly", "Damping"),
            translate("Assembly", "Preload"),
        ]
        for col, text in enumerate(heads):
            grid.addWidget(QtWidgets.QLabel(text), 0, col + 1)
        rows = [
            translate("Assembly", "Along x"),
            translate("Assembly", "Along y"),
            translate("Assembly", "Along z"),
            translate("Assembly", "About x"),
            translate("Assembly", "About y"),
            translate("Assembly", "About z"),
        ]
        # (property, component) -> its box
        self.valueBoxes = {}
        for row, text in enumerate(rows):
            grid.addWidget(QtWidgets.QLabel(text), row + 1, 0)
            for col, props in enumerate(self.Columns):
                prop = props[row // 3]
                box = QtWidgets.QDoubleSpinBox()
                box.setDecimals(4)
                box.setRange(-1e12 if col == 2 else 0.0, 1e12)
                box.setSuffix(" " + Values[prop][0])
                box.valueChanged.connect(
                    lambda value, prop=prop, k=row % 3: self.onValue(prop, k, value)
                )
                grid.addWidget(box, row + 1, col + 1)
                self.valueBoxes[(prop, row % 3)] = box
        form.addRow(grid)
        return group, form

    def fillItem(self):
        for (prop, k), box in self.valueBoxes.items():
            box.blockSignals(True)
            box.setValue(getattr(self.item, prop)[k] / Values[prop][1])
            box.blockSignals(False)

    def onValue(self, prop, k, value):
        vector = getattr(self.item, prop)
        values = [vector.x, vector.y, vector.z]
        values[k] = value * Values[prop][1]
        setattr(self.item, prop, App.Vector(*values))


if App.GuiUp:
    Gui.addCommand("Assembly_CreateBushing", CommandCreateBushing())
