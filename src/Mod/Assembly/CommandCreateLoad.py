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
opens the load's task panel, which a double click on a load opens too.

TaskFramed is the panel of any item at a frame (Assembly::FramedItem): the
frame's pick, its offset, the other component, and the transaction; a
bushing's panel (CommandCreateBushing.py) is one too."""

import sys

import FreeCAD as App

from PySide import QtCore, QtWidgets
from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui

import CommandCreateSpring
import UtilsAssembly

translate = App.Qt.translate


def recomputeFramed(item):
    """Recompute an item at a frame (a load, a bushing) and what makes its
    frame, its element and element binding, as a joint's sides are"""
    binding = item.Binding
    if binding is not None:
        if binding.Element is not None:
            obj, subs = binding.Element
            element = obj.getSubObject(subs[0], retType=1) if subs and subs[0] else None
            if element is not None and element.isDerivedFrom("Part::Element"):
                element.recompute()
        binding.recompute()
    item.recompute()


recomputeLoad = recomputeFramed


def describeFrame(item):
    """Where an item at a frame is, for its panel"""
    component, frame = item.getEnd()
    binding = item.Binding
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
        recomputeFramed(load)
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
                recomputeFramed(load)
            Gui.Control.showDialog(TaskLoad(load, creating=True))
        except Exception:
            # The panel's Cancel would have undone it
            doc.abortTransaction()
            raise


# The open load panel, as JointObject.activeTask is the joint's
activeTask = None


class TaskFramed(QtCore.QObject):
    """The task panel of an item at a frame: where it is, picked on an
    element as a joint's side is (or made a free frame where it is), with an
    offset along the element's normal, and the other component (its property
    otherProp), picked or the ground; a subclass adds its own group of
    fields (buildItem, fillItem). The changes are one transaction: Cancel
    undoes them, and deletes an item the command made. The open panel is its
    module's activeTask."""

    title = ""
    editText = ""
    otherProp = ""
    otherLabel = ""
    # The item's name in the messages, the panel's title by default
    name = ""
    texts = {}

    def __init__(self, item, creating=False):
        super().__init__()
        self.module = sys.modules[type(self).__module__]
        self.module.activeTask = self
        self.item = item
        self.creating = creating
        self.doc = item.Document
        self.assembly = None
        for obj in item.InListRecursive:
            if obj.isDerivedFrom("Assembly::AssemblyObject"):
                self.assembly = obj
                break
        if not creating:
            self.doc.openTransaction(self.editText)
        self.view = Gui.getDocument(self.doc).activeView()
        # What the next pick is for: "frame", "other" or None
        self.armed = None
        self.preview = None
        self.hovered = None
        self.callbackMove = None
        self.buildForm()
        self.arm("frame" if item.getEnd()[0] is None else None)
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
        box.setProperty("value", getattr(self.item, prop))
        Gui.ExpressionBinding(box).bind(self.item, prop)
        box.valueChanged.connect(lambda q, prop=prop: self.onQuantity(prop, q))
        return box

    def buildForm(self):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(self.title)
        layout = QtWidgets.QVBoxLayout(self.form)

        atBox = QtWidgets.QGroupBox(self.texts["at"])
        grid = QtWidgets.QGridLayout(atBox)
        self.endLabel = QtWidgets.QLabel()
        self.endLabel.setWordWrap(True)
        grid.addWidget(self.endLabel, 0, 0, 1, 2)
        self.pickButton = QtWidgets.QPushButton(translate("Assembly", "Pick"))
        self.pickButton.setCheckable(True)
        self.pickButton.setToolTip(self.texts["pickTip"])
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

        itemBox, form = self.buildItem()

        otherRow = QtWidgets.QHBoxLayout()
        self.otherLabel = QtWidgets.QLabel()
        otherRow.addWidget(self.otherLabel, 1)
        self.otherButton = QtWidgets.QPushButton(translate("Assembly", "Pick"))
        self.otherButton.setCheckable(True)
        self.otherButton.setToolTip(self.texts["otherTip"])
        self.otherButton.clicked.connect(lambda checked: self.arm("other" if checked else None))
        otherRow.addWidget(self.otherButton)
        self.groundButton = QtWidgets.QPushButton(translate("Assembly", "Ground"))
        self.groundButton.setToolTip(self.texts["groundTip"])
        self.groundButton.clicked.connect(self.onGround)
        otherRow.addWidget(self.groundButton)
        form.addRow(self.texts["other"], otherRow)
        self.hint = QtWidgets.QLabel()
        self.hint.setWordWrap(True)
        form.addRow(self.hint)
        layout.addWidget(itemBox)

    def buildItem(self):
        """The item's own group box and its form layout, filled; the other
        component's row goes at its end"""
        box = QtWidgets.QGroupBox()
        return box, QtWidgets.QFormLayout(box)

    def fillItem(self):
        """Put the item's own fields in the panel"""

    def fill(self):
        item = self.item
        self.endLabel.setText(describeFrame(item))
        bound = item.Binding is not None
        self.freeButton.setEnabled(bound)
        self.offsetBox.setEnabled(bound)
        self.offsetBox.blockSignals(True)
        self.offsetBox.setProperty("rawValue", item.Binding.Offset.Base.z if bound else 0.0)
        self.offsetBox.blockSignals(False)
        self.fillItem()
        other = getattr(item, self.otherProp)
        self.otherLabel.setText(
            other.Label if other is not None else translate("Assembly", "The ground")
        )
        self.groundButton.setEnabled(other is not None)
        self.pickButton.setChecked(self.armed == "frame")
        self.otherButton.setChecked(self.armed == "other")
        if self.armed == "frame":
            self.hint.setText(self.texts["pickHint"])
        elif self.armed == "other":
            self.hint.setText(self.texts["otherHint"])
        else:
            self.hint.setText("")
    def arm(self, what):
        self.armed = what
        self.showPreview(None)
        if hasattr(self, "hint"):
            self.fill()

    # ------------------------------------------------------------- editing

    def changed(self):
        recomputeFramed(self.item)
        self.fill()

    def onQuantity(self, prop, quantity):
        setattr(self.item, prop, quantity)

    def onFree(self):
        self.item.clearEnd()
        self.changed()

    def onOffset(self, quantity):
        binding = self.item.Binding
        if binding is None:
            return
        plc = binding.Offset
        plc.Base.z = quantity.Value
        binding.Offset = plc
        self.changed()

    def onGround(self):
        setattr(self.item, self.otherProp, None)
        self.fill()

    def pick(self, root, sub, pos):
        pick = CommandCreateSpring.pickOf(self.assembly, root, sub, pos)
        if pick is None:
            return False
        other = getattr(self.item, self.otherProp)
        if self.armed == "frame":
            if other is not None and other == pick[0]:
                App.Console.PrintWarning(self.texts["onePart"] + "\n")
                return False
            self.item.setEnd(*pick)
            self.armed = None
            self.changed()
            return True
        if self.armed == "other":
            if pick[0] == self.item.getEnd()[0]:
                App.Console.PrintWarning(self.texts["onePart"] + "\n")
                return False
            setattr(self.item, self.otherProp, pick[0])
            self.armed = None
            self.fill()
            return True
        return False

    # ------------------------------------------------------------- picking

    def addSelection(self, doc_name, obj_name, sub_name, mousePos):
        root = App.getDocument(doc_name).getObject(obj_name)
        pos = App.Vector(*mousePos[:3]) if mousePos else None
        self.pick(root, sub_name, pos)
        # The picks are the item's, not a selection
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
        """A dot where the hovered element would put the frame; hidden with no
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
        if self.module.activeTask is self:
            self.module.activeTask = None
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
        self.deactivate()
        recomputeFramed(self.item)
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


class TaskLoad(TaskFramed):
    """The task panel of a load: where it acts; Force or Torque, its
    magnitude and profile over time; its direction, in the frame's axes or
    the assembly's; and the component taking the reaction, or the ground."""

    title = translate("Assembly", "Force / Torque")
    editText = translate("Assembly", "Edit load")
    otherProp = "Reaction"
    texts = {
        "at": translate("Assembly", "Acting at"),
        "pickTip": translate(
            "Assembly",
            "Pick an element in the 3D view: a vertex, an edge or a face, the load\n"
            "acting at its vertex nearest the cursor or its centre, in its frame",
        ),
        "pickHint": translate("Assembly", "Pick the element the load acts at in the 3D view"),
        "other": translate("Assembly", "Reaction"),
        "otherTip": translate("Assembly", "Pick the part taking the opposite load, in the 3D view"),
        "otherHint": translate("Assembly", "Pick the part taking the reaction in the 3D view"),
        "groundTip": translate("Assembly", "The ground takes the opposite load"),
        "onePart": translate(
            "Assembly", "Force / Torque: the load and its reaction are on one part."
        ),
    }

    @property
    def load(self):
        return self.item

    def buildItem(self):
        group = QtWidgets.QGroupBox(translate("Assembly", "Load"))
        form = QtWidgets.QFormLayout(group)
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
        return group, form

    def fillItem(self):
        self.kindCombo.blockSignals(True)
        self.kindCombo.setCurrentIndex(max(0, self.kindCombo.findData(self.item.Kind)))
        self.kindCombo.blockSignals(False)
        self.magnitudeBox.blockSignals(True)
        self.magnitudeBox.setProperty("value", self.item.Magnitude)
        self.magnitudeBox.blockSignals(False)
        if self.profileEdit.text().strip() != self.item.Profile:
            self.profileEdit.blockSignals(True)
            self.profileEdit.setText(self.item.Profile)
            self.profileEdit.blockSignals(False)
        for box, value in zip(self.dirBoxes, self.item.Direction):
            box.blockSignals(True)
            box.setValue(value)
            box.blockSignals(False)
        self.axesCombo.blockSignals(True)
        self.axesCombo.setCurrentIndex(max(0, self.axesCombo.findData(self.item.DirectionAxes)))
        self.axesCombo.blockSignals(False)

    def onKind(self, index):
        kind = self.kindCombo.itemData(index)
        if kind == self.item.Kind:
            return
        self.item.Kind = kind
        # A force's number is no torque's: a fresh default
        self.item.Magnitude = "1 N*m" if kind == "Torque" else "10 N"
        self.fill()

    def onProfile(self):
        text = self.profileEdit.text().strip()
        if text != self.item.Profile:
            self.item.Profile = text

    def onDirection(self):
        self.item.Direction = App.Vector(*[box.value() for box in self.dirBoxes])

    def onReverse(self):
        self.item.Direction = self.item.Direction * -1
        self.fill()

    def onAxes(self, index):
        self.item.DirectionAxes = self.axesCombo.itemData(index)

    def accept(self):
        self.onProfile()
        return super().accept()


if App.GuiUp:
    Gui.addCommand("Assembly_CreateLoad", CommandCreateLoad())
