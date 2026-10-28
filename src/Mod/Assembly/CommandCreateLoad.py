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

"""Force / Torque: a load applied to a component of the active assembly in
its dynamic and static studies (docs/AssemblyDynamics.md sec 5.2's
AppliedLoad). It acts at an element's frame, picked as a joint's side is,
through an element binding the load owns; or at a free frame. The command
opens the load's task panel, which a double click on a load opens too."""

import FreeCAD as App

from PySide import QtCore, QtWidgets
from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui

import CommandCreateSpring
import UtilsAssembly

translate = App.Qt.translate


def recomputeLoad(load):
    """Recompute a load and what makes its frame, its element and element
    binding, as a joint's sides are"""
    binding = load.Binding
    if binding is not None:
        if binding.Element is not None:
            obj, subs = binding.Element
            element = obj.getSubObject(subs[0], retType=1) if subs and subs[0] else None
            if element is not None and element.isDerivedFrom("Part::Element"):
                element.recompute()
        binding.recompute()
    load.recompute()


def describeLoadEnd(load):
    """What the load acts at, for the panel"""
    component, frame = load.getEnd()
    binding = load.Binding
    if binding is not None and binding.Element is not None:
        obj, subs = binding.Element
        element = obj.getSubObject(subs[0], retType=1) if subs and subs[0] else None
        if element is not None and element.isDerivedFrom("Part::Element"):
            text = component.Label if component is not None else "?"
            if element.Target is not None and element.Target[1]:
                text += ": " + element.Target[1][0].split(".")[-1]
            if element.Vertex is not None and element.Vertex[0] is not None and element.Vertex[1]:
                text += translate("Assembly", ", at ") + element.Vertex[1][0].split(".")[-1]
            return text
    p = frame.Base
    where = "(%.3g, %.3g, %.3g)" % (p.x, p.y, p.z)
    if component is None:
        return translate("Assembly", "Not set")
    return translate("Assembly", "A point of {} ").format(component.Label) + where


def newLoad(assembly, kind="Force", magnitude=None):
    """A load acting nowhere yet in the assembly's Physics group, ticked in
    every Dynamic and Static study of it"""
    physics = assembly.getGroup("Physics", True)
    load = physics.newObject("Assembly::AppliedLoad", kind)
    load.Kind = kind
    load.Magnitude = magnitude or ("1 N*m" if kind == "Torque" else "10 N")
    sims = assembly.getGroup("Simulations")
    for sim in sims.Group if sims else []:
        if getattr(sim, "AnalysisType", None) in ("Dynamic", "Static"):
            sim.Elements = list(sim.Elements) + [load]
    return load


def makeLoad(assembly, end, kind="Force", magnitude=None, direction=None, reaction=None):
    """A load in a transaction: end is (component, Placement in its
    coordinates) for a free frame or (component, subname[, vertex]) for an
    element; the direction, in the frame's axes, defaults to its z (a face's
    normal)"""
    doc = assembly.Document
    doc.openTransaction(translate("Assembly", "Create load"))
    try:
        load = newLoad(assembly, kind, magnitude)
        component, where = end[0], end[1]
        if isinstance(where, App.Placement):
            load.Component = component
            load.Frame = where
        else:
            load.setEnd(component, where, end[2] if len(end) > 2 else "")
        if direction is not None:
            load.Direction = direction
        load.Reaction = reaction
        recomputeLoad(load)
    finally:
        doc.commitTransaction()
    return load


def editLoad(load):
    """Open the task panel of a load, as a double click does"""
    if Gui.Control.activeDialog():
        App.Console.PrintWarning(
            translate("Assembly", "Close the open task panel before editing the load.\n")
        )
        return
    Gui.Control.showDialog(TaskLoad(load))


class CommandCreateLoad:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_AppliedLoad",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateLoad", "Force / Torque"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateLoad",
                "A force or a torque on a component, for a dynamic or static simulation,\n"
                "acting at an element picked as for a joint, before or in the panel.",
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
        doc.openTransaction(translate("Assembly", "Create load"))
        try:
            load = newLoad(assembly)
            if pick is not None:
                load.setEnd(*pick)
                recomputeLoad(load)
            Gui.Control.showDialog(TaskLoad(load, creating=True))
        except Exception:
            # The panel's Cancel would have undone it
            doc.abortTransaction()
            raise


# The open load panel, as JointObject.activeTask is the joint's
activeTask = None


class TaskLoad(QtCore.QObject):
    """The task panel of a load: where it acts, picked on an element as a
    joint's side is (or made a free frame where it is), with an offset along
    the element's normal; Force or Torque, its magnitude and profile over
    time; its direction, in the frame's axes or the assembly's; and the
    component taking the reaction, or the ground. The changes are one
    transaction: Cancel undoes them, and deletes a load the command made."""

    def __init__(self, load, creating=False):
        super().__init__()
        global activeTask
        activeTask = self
        self.load = load
        self.creating = creating
        self.doc = load.Document
        self.assembly = None
        for obj in load.InListRecursive:
            if obj.isDerivedFrom("Assembly::AssemblyObject"):
                self.assembly = obj
                break
        if not creating:
            self.doc.openTransaction(translate("Assembly", "Edit load"))
        self.view = Gui.getDocument(self.doc).activeView()
        # What the next pick is for: "frame", "reaction" or None
        self.armed = None
        self.preview = None
        self.hovered = None
        self.callbackMove = None
        self.buildForm()
        self.arm("frame" if load.getEnd()[0] is None else None)
        self.fill()

        if self.assembly is not None:
            import JointObject

            UtilsAssembly.setJointsPickableState(self.doc, False)
            Gui.Selection.clearSelection()
            Gui.Selection.addSelectionGate(
                JointObject.MakeJointSelGate(self, self.assembly),
                Gui.Selection.ResolveMode.NoResolve,
            )
            Gui.Selection.addObserver(self, Gui.Selection.ResolveMode.NoResolve)
            if self.view is not None and hasattr(self.view, "addEventCallback"):
                self.callbackMove = self.view.addEventCallback("SoLocation2Event", self.moveMouse)

    # ------------------------------------------------------------- the form

    def quantityBox(self, prop):
        box = Gui.UiLoader().createWidget("Gui::QuantitySpinBox")
        # Its unit with it
        box.setProperty("value", getattr(self.load, prop))
        Gui.ExpressionBinding(box).bind(self.load, prop)
        box.valueChanged.connect(lambda q, prop=prop: self.onQuantity(prop, q))
        return box

    def buildForm(self):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Force / Torque"))
        layout = QtWidgets.QVBoxLayout(self.form)

        atBox = QtWidgets.QGroupBox(translate("Assembly", "Acting at"))
        grid = QtWidgets.QGridLayout(atBox)
        self.endLabel = QtWidgets.QLabel()
        self.endLabel.setWordWrap(True)
        grid.addWidget(self.endLabel, 0, 0, 1, 2)
        self.pickButton = QtWidgets.QPushButton(translate("Assembly", "Pick"))
        self.pickButton.setCheckable(True)
        self.pickButton.setToolTip(
            translate(
                "Assembly",
                "Pick an element in the 3D view: a vertex, an edge or a face, the load\n"
                "acting at its vertex nearest the cursor or its centre, in its frame",
            )
        )
        self.pickButton.clicked.connect(
            lambda checked: self.arm("frame" if checked else None)
        )
        grid.addWidget(self.pickButton, 0, 2)
        self.freeButton = QtWidgets.QPushButton(translate("Assembly", "Free"))
        self.freeButton.setToolTip(
            translate("Assembly", "Make the frame a fixed one on the component, where it is now")
        )
        self.freeButton.clicked.connect(self.onFree)
        grid.addWidget(self.freeButton, 0, 3)
        grid.addWidget(QtWidgets.QLabel(translate("Assembly", "Offset")), 1, 0)
        self.offsetBox = Gui.UiLoader().createWidget("Gui::QuantitySpinBox")
        self.offsetBox.setProperty("unit", "mm")
        self.offsetBox.setToolTip(
            translate(
                "Assembly",
                "Move the frame along the element's normal: a face's, or the axis of a\n"
                "circle; the element binding's offset",
            )
        )
        self.offsetBox.valueChanged.connect(self.onOffset)
        grid.addWidget(self.offsetBox, 1, 1, 1, 3)
        layout.addWidget(atBox)

        loadBox = QtWidgets.QGroupBox(translate("Assembly", "Load"))
        form = QtWidgets.QFormLayout(loadBox)
        self.kindCombo = QtWidgets.QComboBox()
        self.kindCombo.addItem(translate("Assembly", "Force"), "Force")
        self.kindCombo.addItem(translate("Assembly", "Torque"), "Torque")
        self.kindCombo.currentIndexChanged.connect(self.onKind)
        form.addRow(translate("Assembly", "Kind"), self.kindCombo)
        self.magnitudeBox = self.quantityBox("Magnitude")
        form.addRow(translate("Assembly", "Magnitude"), self.magnitudeBox)
        self.profileEdit = QtWidgets.QLineEdit()
        self.profileEdit.setPlaceholderText(translate("Assembly", "constant"))
        self.profileEdit.setToolTip(
            translate(
                "Assembly",
                "The magnitude's factor over time, a formula of time as a motion's,\n"
                "e.g. sin(2*pi*time); empty: 1",
            )
        )
        # Taken as typed: a refill of the panel would put back what the load
        # holds over what is not in it yet
        self.profileEdit.textChanged.connect(self.onProfile)
        form.addRow(translate("Assembly", "Profile"), self.profileEdit)

        dirRow = QtWidgets.QHBoxLayout()
        self.dirBoxes = []
        for axis in "xyz":
            box = QtWidgets.QDoubleSpinBox()
            box.setRange(-1e6, 1e6)
            box.setDecimals(4)
            box.setPrefix(axis + " ")
            box.valueChanged.connect(self.onDirection)
            dirRow.addWidget(box)
            self.dirBoxes.append(box)
        self.reverseButton = QtWidgets.QPushButton(translate("Assembly", "Reverse"))
        self.reverseButton.clicked.connect(self.onReverse)
        dirRow.addWidget(self.reverseButton)
        form.addRow(translate("Assembly", "Direction"), dirRow)
        self.axesCombo = QtWidgets.QComboBox()
        self.axesCombo.addItem(translate("Assembly", "The frame's, turning with the part"), "Frame")
        self.axesCombo.addItem(translate("Assembly", "The assembly's, fixed"), "Assembly")
        self.axesCombo.currentIndexChanged.connect(self.onAxes)
        form.addRow(translate("Assembly", "In the axes"), self.axesCombo)

        reactionRow = QtWidgets.QHBoxLayout()
        self.reactionLabel = QtWidgets.QLabel()
        reactionRow.addWidget(self.reactionLabel, 1)
        self.reactionButton = QtWidgets.QPushButton(translate("Assembly", "Pick"))
        self.reactionButton.setCheckable(True)
        self.reactionButton.setToolTip(
            translate("Assembly", "Pick the part taking the opposite load, in the 3D view")
        )
        self.reactionButton.clicked.connect(
            lambda checked: self.arm("reaction" if checked else None)
        )
        reactionRow.addWidget(self.reactionButton)
        self.groundButton = QtWidgets.QPushButton(translate("Assembly", "Ground"))
        self.groundButton.setToolTip(translate("Assembly", "The ground takes the opposite load"))
        self.groundButton.clicked.connect(self.onGround)
        reactionRow.addWidget(self.groundButton)
        form.addRow(translate("Assembly", "Reaction"), reactionRow)
        self.hint = QtWidgets.QLabel()
        self.hint.setWordWrap(True)
        form.addRow(self.hint)
        layout.addWidget(loadBox)

    def fill(self):
        load = self.load
        self.endLabel.setText(describeLoadEnd(load))
        bound = load.Binding is not None
        self.freeButton.setEnabled(bound)
        self.offsetBox.setEnabled(bound)
        self.offsetBox.blockSignals(True)
        self.offsetBox.setProperty("rawValue", load.Binding.Offset.Base.z if bound else 0.0)
        self.offsetBox.blockSignals(False)
        self.kindCombo.blockSignals(True)
        self.kindCombo.setCurrentIndex(max(0, self.kindCombo.findData(load.Kind)))
        self.kindCombo.blockSignals(False)
        self.magnitudeBox.blockSignals(True)
        self.magnitudeBox.setProperty("value", load.Magnitude)
        self.magnitudeBox.blockSignals(False)
        if self.profileEdit.text().strip() != load.Profile:
            self.profileEdit.blockSignals(True)
            self.profileEdit.setText(load.Profile)
            self.profileEdit.blockSignals(False)
        for box, value in zip(self.dirBoxes, load.Direction):
            box.blockSignals(True)
            box.setValue(value)
            box.blockSignals(False)
        self.axesCombo.blockSignals(True)
        self.axesCombo.setCurrentIndex(max(0, self.axesCombo.findData(load.DirectionAxes)))
        self.axesCombo.blockSignals(False)
        reaction = load.Reaction
        self.reactionLabel.setText(
            reaction.Label if reaction is not None else translate("Assembly", "The ground")
        )
        self.groundButton.setEnabled(reaction is not None)
        self.pickButton.setChecked(self.armed == "frame")
        self.reactionButton.setChecked(self.armed == "reaction")
        if self.armed == "frame":
            self.hint.setText(
                translate("Assembly", "Pick the element the load acts at in the 3D view")
            )
        elif self.armed == "reaction":
            self.hint.setText(
                translate("Assembly", "Pick the part taking the reaction in the 3D view")
            )
        else:
            self.hint.setText("")

    def arm(self, what):
        self.armed = what
        self.showPreview(None)
        if hasattr(self, "hint"):
            self.fill()

    # ------------------------------------------------------------- editing

    def changed(self):
        recomputeLoad(self.load)
        self.fill()

    def onQuantity(self, prop, quantity):
        setattr(self.load, prop, quantity)

    def onKind(self, index):
        kind = self.kindCombo.itemData(index)
        if kind == self.load.Kind:
            return
        self.load.Kind = kind
        # A force's number is no torque's: a fresh default
        self.load.Magnitude = "1 N*m" if kind == "Torque" else "10 N"
        self.fill()

    def onProfile(self):
        text = self.profileEdit.text().strip()
        if text != self.load.Profile:
            self.load.Profile = text

    def onDirection(self):
        self.load.Direction = App.Vector(*[box.value() for box in self.dirBoxes])

    def onReverse(self):
        self.load.Direction = self.load.Direction * -1
        self.fill()

    def onAxes(self, index):
        self.load.DirectionAxes = self.axesCombo.itemData(index)

    def onFree(self):
        self.load.clearEnd()
        self.changed()

    def onOffset(self, quantity):
        binding = self.load.Binding
        if binding is None:
            return
        plc = binding.Offset
        plc.Base.z = quantity.Value
        binding.Offset = plc
        self.changed()

    def onGround(self):
        self.load.Reaction = None
        self.fill()

    def pick(self, root, sub, pos):
        pick = CommandCreateSpring.pickOf(self.assembly, root, sub, pos)
        if pick is None:
            return False
        if self.armed == "frame":
            if self.load.Reaction is not None and self.load.Reaction == pick[0]:
                App.Console.PrintWarning(
                    translate(
                        "Assembly", "Force / Torque: the load and its reaction are on one part.\n"
                    )
                )
                return False
            self.load.setEnd(*pick)
            self.armed = None
            self.changed()
            return True
        if self.armed == "reaction":
            if pick[0] == self.load.getEnd()[0]:
                App.Console.PrintWarning(
                    translate(
                        "Assembly", "Force / Torque: the load and its reaction are on one part.\n"
                    )
                )
                return False
            self.load.Reaction = pick[0]
            self.armed = None
            self.fill()
            return True
        return False

    # ------------------------------------------------------------- picking

    def addSelection(self, doc_name, obj_name, sub_name, mousePos):
        root = App.getDocument(doc_name).getObject(obj_name)
        pos = App.Vector(*mousePos[:3]) if mousePos else None
        self.pick(root, sub_name, pos)
        # The picks are the load's, not a selection
        QtCore.QTimer.singleShot(0, Gui.Selection.clearSelection)

    def setPreselection(self, doc_name, obj_name, sub_name):
        self.hovered = None
        if self.armed != "frame" or not sub_name:
            return
        self.hovered = (App.getDocument(doc_name).getObject(obj_name), sub_name)

    def removePreselection(self, doc_name, obj_name, sub_name):
        self.hovered = None
        self.showPreview(None)

    def moveMouse(self, info):
        if self.armed != "frame" or self.hovered is None or self.view is None:
            self.showPreview(None)
            return
        cursor = self.view.getObjectInfo(self.view.getCursorPos())
        if not cursor:
            self.showPreview(None)
            return
        pos = App.Vector(cursor["x"], cursor["y"], cursor["z"])
        pick = CommandCreateSpring.pickOf(self.assembly, self.hovered[0], self.hovered[1], pos)
        self.showPreview(CommandCreateSpring.hoverPoint(pick) if pick is not None else None)

    def showPreview(self, point):
        """A dot where the hovered element would put the load; hidden with no
        point"""
        if point is None:
            if self.preview is not None:
                self.preview.whichChild = -1
            return
        from pivy import coin

        if self.preview is None and self.view is not None:
            self.preview = coin.SoSwitch()
            sep = coin.SoSeparator()
            pickStyle = coin.SoPickStyle()
            pickStyle.style = coin.SoPickStyle.UNPICKABLE
            sep.addChild(pickStyle)
            material = coin.SoMaterial()
            material.diffuseColor = (0.85, 0.15, 0.1)
            material.emissiveColor = (0.85, 0.15, 0.1)
            sep.addChild(material)
            style = coin.SoDrawStyle()
            style.pointSize = 9
            sep.addChild(style)
            self.previewCoords = coin.SoCoordinate3()
            sep.addChild(self.previewCoords)
            dot = coin.SoPointSet()
            dot.startIndex = 0
            dot.numPoints = 1
            sep.addChild(dot)
            self.preview.addChild(sep)
            self.view.getSceneGraph().addChild(self.preview)
        if self.preview is None:
            return
        self.previewCoords.point.setValues(0, 1, [tuple(point)])
        self.previewCoords.point.setNum(1)
        self.preview.whichChild = 0

    # ------------------------------------------------------------- closing

    def deactivate(self):
        global activeTask
        if activeTask is self:
            activeTask = None
        Gui.Selection.removeSelectionGate()
        Gui.Selection.removeObserver(self)
        if self.callbackMove is not None and self.view is not None:
            self.view.removeEventCallback("SoLocation2Event", self.callbackMove)
            self.callbackMove = None
        if self.preview is not None and self.view is not None:
            self.view.getSceneGraph().removeChild(self.preview)
            self.preview = None
        UtilsAssembly.setJointsPickableState(self.doc, True)

    def accept(self):
        self.onProfile()
        self.deactivate()
        recomputeLoad(self.load)
        self.doc.commitTransaction()
        Gui.Control.closeDialog()
        return True

    def reject(self):
        self.deactivate()
        self.doc.abortTransaction()
        Gui.Control.closeDialog()
        if self.assembly is not None:
            self.doc.recompute()
        return True

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel


if App.GuiUp:
    Gui.addCommand("Assembly_CreateLoad", CommandCreateLoad())
