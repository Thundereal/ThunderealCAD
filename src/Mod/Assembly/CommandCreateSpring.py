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

"""Spring / Damper: a spring and damper between two ends on the active
assembly's components (docs/AssemblyDynamics.md sec 9, phase D1). Each end
is on an element, picked as a joint's side is, through an element binding
the spring owns; or a free point. The command opens the spring's task
panel, which a double click on a spring opens too."""

import FreeCAD as App

from PySide import QtCore, QtWidgets
from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui

import UtilsAssembly

translate = App.Qt.translate


def pickOf(assembly, root, sub, pos=None):
    """(component, subname, vertex) of a selected element, as a joint's side
    takes it: the vertex nearest pos, or none for the element's own frame
    (a circle's centre, a face's middle); None when it is on no component"""
    import JointObject

    component, sub = UtilsAssembly.getComponentReference(assembly, root, sub)
    if component is None or not sub:
        return None
    vertex = ""
    if pos is not None:
        ref = UtilsAssembly.addVertexToReference(
            [component, [sub]], UtilsAssembly.findElementClosestVertex([component, [sub]], pos)
        )
        vertex = ref[1][1] if len(ref[1]) > 1 else ""
        if JointObject._plainSub(vertex) == JointObject._plainSub(sub):
            vertex = ""
        elif UtilsAssembly.isFaceEdgePick(sub, vertex):
            # A face's circle nearest the cursor, numbered among the face's
            # edges: its centre on the face, the element's vertex by the
            # object's name of the circle, as a joint's side takes it
            edge = UtilsAssembly.faceEdgeToObjectEdge(
                UtilsAssembly.getObject([component, [sub]]),
                UtilsAssembly.getElementName(sub),
                UtilsAssembly.getElementName(vertex),
            )
            vertex = UtilsAssembly.swapElNameInSubname(vertex, edge) if edge else ""
    return component, sub, vertex


def hoverPoint(pick):
    """Where an end on a pick (pickOf()) is, in the document's coordinates, or
    None"""
    component, sub, vertex = pick
    try:
        if vertex and UtilsAssembly.isFaceEdgePick(sub, vertex):
            # findPlacement numbers a face's circle among the face's edges
            local = UtilsAssembly.objectEdgeToFaceEdge(
                UtilsAssembly.getObject([component, [sub]]),
                UtilsAssembly.getElementName(sub),
                UtilsAssembly.getElementName(vertex),
            )
            vertex = UtilsAssembly.swapElNameInSubname(vertex, local) if local else ""
        ref = [component, [sub, vertex or sub]]
        plc = UtilsAssembly.findPlacement(ref)
        return UtilsAssembly.getJcsGlobalPlc(plc, ref).Base
    except Exception:
        return None


def recomputeEnds(spring):
    """Recompute a spring and what makes its ends, its elements and element
    bindings, as a joint's sides are"""
    import JointObject

    JointObject.recomputeSides(spring)


def endBinding(spring, i):
    return spring.Binding1 if i == 1 else spring.Binding2


def describeEnd(spring, i):
    """What end i is on, for the panel"""
    component, point = spring.getEnd(i)
    binding = endBinding(spring, i)
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
    where = "(%.3g, %.3g, %.3g)" % (point.x, point.y, point.z)
    if component is None:
        return translate("Assembly", "A point of the ground ") + where
    return translate("Assembly", "A point of {} ").format(component.Label) + where


def newSpring(assembly, stiffness="1 N/mm"):
    """A spring of no ends yet in the assembly's Physics group, ticked in every
    Dynamic and Static study of it"""
    physics = assembly.getGroup("Physics", True)
    spring = physics.newObject("Assembly::SpringDamper", "Spring")
    spring.Stiffness = stiffness
    sims = assembly.getGroup("Simulations")
    for sim in sims.Group if sims else []:
        if getattr(sim, "AnalysisType", None) in ("Dynamic", "Static"):
            sim.Elements = list(sim.Elements) + [spring]
    return spring


def setEnd(spring, i, end):
    """End i from (component, point in its coordinates) for a free point, or
    (component, subname[, vertex]) for an element"""
    component, where = end[0], end[1]
    if isinstance(where, App.Vector):
        spring.clearEnd(i)
        if i == 1:
            spring.Component1, spring.Point1 = component, where
        else:
            spring.Component2, spring.Point2 = component, where
    else:
        spring.setEnd(i, component, where, end[2] if len(end) > 2 else "")


def makeSpring(assembly, end1, end2, stiffness="1 N/mm"):
    """A spring between two ends, each (component, point in its coordinates)
    for a free point or (component, subname[, vertex]) for an element, free
    at the distance between them, in a transaction; ticked in every Dynamic
    and Static study of the assembly."""
    doc = assembly.Document
    doc.openTransaction(translate("Assembly", "Create spring"))
    try:
        spring = newSpring(assembly, stiffness)
        setEnd(spring, 1, end1)
        setEnd(spring, 2, end2)
        recomputeEnds(spring)
        spring.FreeLength = (spring.getEndPoint(2) - spring.getEndPoint(1)).Length
    finally:
        doc.commitTransaction()
    return spring


def editSpring(spring):
    """Open the task panel of a spring, as a double click does"""
    if Gui.Control.activeDialog():
        App.Console.PrintWarning(
            translate("Assembly", "Close the open task panel before editing the spring.\n")
        )
        return
    Gui.Control.showDialog(TaskSpring(spring))


class CommandCreateSpring:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_SpringDamper",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateSpring", "Spring / Damper"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateSpring",
                "A spring and damper between two elements of the assembly's components,\n"
                "for a dynamic or static simulation. Pick the two elements as for a joint,\n"
                "before or in the panel.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive() and not Gui.Control.activeDialog()

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly:
            return
        picks = []
        for sel in Gui.Selection.getSelectionEx("", 0):
            for i, sub in enumerate(sel.SubElementNames):
                pos = sel.PickedPoints[i] if i < len(sel.PickedPoints) else None
                pick = pickOf(assembly, sel.Object, sub, pos)
                if pick is not None:
                    picks.append(pick)
        Gui.Selection.clearSelection()
        doc = assembly.Document
        doc.openTransaction(translate("Assembly", "Create spring"))
        try:
            spring = newSpring(assembly)
            for i, pick in enumerate(picks[:2]):
                spring.setEnd(i + 1, *pick)
            recomputeEnds(spring)
            if len(picks) >= 2:
                spring.FreeLength = (spring.getEndPoint(2) - spring.getEndPoint(1)).Length
            Gui.Control.showDialog(TaskSpring(spring, creating=True))
        except Exception:
            # The panel's Cancel would have undone it
            doc.abortTransaction()
            raise


# The open spring panel, as JointObject.activeTask is the joint's
activeTask = None


class TaskSpring(QtCore.QObject):
    """The task panel of a spring: its two ends, each picked on an element as
    a joint's side is (an element of the active assembly's components) or
    made a free point where it is, with an offset along the element's normal;
    and its stiffness, free length, preload and damping. While an end is
    being picked, the element under the cursor shows where it would go.

    The changes are one transaction: Cancel undoes them, and deletes a
    spring the command made."""

    def __init__(self, spring, creating=False):
        super().__init__()
        global activeTask
        activeTask = self
        self.spring = spring
        self.creating = creating
        self.doc = spring.Document
        self.assembly = None
        for obj in spring.InListRecursive:
            if obj.isDerivedFrom("Assembly::AssemblyObject"):
                self.assembly = obj
                break
        if not creating:
            self.doc.openTransaction(translate("Assembly", "Edit spring"))
        self.view = Gui.getDocument(self.doc).activeView()
        self.freeLengthEdited = bool(spring.FreeLength.Value) and not creating
        self.hovered = None
        self.armed = None
        self.preview = None
        self.callbackMove = None
        self.buildForm()
        self.arm(self.firstUnset())
        self.fill()

        if self.assembly is not None:
            UtilsAssembly.setJointsPickableState(self.doc, False)
            Gui.Selection.clearSelection()
            Gui.Selection.addObserver(self, Gui.Selection.ResolveMode.NoResolve)
            if self.view is not None and hasattr(self.view, "addEventCallback"):
                self.callbackMove = self.view.addEventCallback("SoLocation2Event", self.moveMouse)

    def open(self):
        """Shown: the gate, on the selection the dialog's view has of its own
        now (docs/TaskPanelPerView.md sec 12). Set as the panel was made --
        edited by a double click, no assembly in edit -- it went to the
        shared one, which the view does not take it from, and deactivate()
        removed nothing: it gated the shared selection after the panel was
        gone, and Python's end, under it, crashed the exit."""
        if self.assembly is not None:
            import JointObject

            Gui.Selection.addSelectionGate(
                JointObject.MakeJointSelGate(self, self.assembly),
                Gui.Selection.ResolveMode.NoResolve,
            )

    # ------------------------------------------------------------- the form

    def quantityBox(self, prop):
        box = Gui.UiLoader().createWidget("Gui::QuantitySpinBox")
        # Its unit with it
        box.setProperty("value", getattr(self.spring, prop))
        Gui.ExpressionBinding(box).bind(self.spring, prop)
        box.valueChanged.connect(lambda q, prop=prop: self.onQuantity(prop, q))
        return box

    def buildForm(self):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Spring / Damper"))
        layout = QtWidgets.QVBoxLayout(self.form)

        endsBox = QtWidgets.QGroupBox(translate("Assembly", "Ends"))
        grid = QtWidgets.QGridLayout(endsBox)
        self.endLabels = {}
        self.pickButtons = {}
        self.freeButtons = {}
        self.offsetBoxes = {}
        for i in (1, 2):
            row = (i - 1) * 2
            grid.addWidget(QtWidgets.QLabel(translate("Assembly", "End {}").format(i)), row, 0)
            label = QtWidgets.QLabel()
            label.setWordWrap(True)
            grid.addWidget(label, row, 1)
            pick = QtWidgets.QPushButton(translate("Assembly", "Pick"))
            pick.setCheckable(True)
            pick.setToolTip(
                translate(
                    "Assembly",
                    "Pick an element for this end in the 3D view: a vertex, an edge or a face,\n"
                    "the end going to its vertex nearest the cursor or its centre",
                )
            )
            pick.clicked.connect(lambda checked, i=i: self.arm(i if checked else None))
            grid.addWidget(pick, row, 2)
            free = QtWidgets.QPushButton(translate("Assembly", "Free"))
            free.setToolTip(
                translate(
                    "Assembly",
                    "Make this end a point where it is now, no longer following an element",
                )
            )
            free.clicked.connect(lambda checked=False, i=i: self.onFree(i))
            grid.addWidget(free, row, 3)
            grid.addWidget(QtWidgets.QLabel(translate("Assembly", "Offset")), row + 1, 0)
            offset = Gui.UiLoader().createWidget("Gui::QuantitySpinBox")
            offset.setProperty("unit", "mm")
            offset.setToolTip(
                translate(
                    "Assembly",
                    "Move the end along the element's normal: a face's, or the axis of a\n"
                    "circle; the element binding's offset",
                )
            )
            offset.valueChanged.connect(lambda q, i=i: self.onOffset(i, q))
            grid.addWidget(offset, row + 1, 1, 1, 3)
            self.endLabels[i] = label
            self.pickButtons[i] = pick
            self.freeButtons[i] = free
            self.offsetBoxes[i] = offset
        self.hint = QtWidgets.QLabel()
        self.hint.setWordWrap(True)
        grid.addWidget(self.hint, 4, 0, 1, 4)
        layout.addWidget(endsBox)

        springBox = QtWidgets.QGroupBox(translate("Assembly", "Spring"))
        form = QtWidgets.QFormLayout(springBox)
        self.stiffnessBox = self.quantityBox("Stiffness")
        form.addRow(translate("Assembly", "Stiffness"), self.stiffnessBox)
        lengthRow = QtWidgets.QHBoxLayout()
        self.freeLengthBox = self.quantityBox("FreeLength")
        self.freeLengthBox.setToolTip(
            translate(
                "Assembly",
                "The length at which the spring neither pushes nor pulls; 0: the length\n"
                "when the run starts",
            )
        )
        lengthRow.addWidget(self.freeLengthBox)
        self.currentButton = QtWidgets.QPushButton(translate("Assembly", "Current length"))
        self.currentButton.setToolTip(
            translate("Assembly", "Make the free length the distance between the ends now")
        )
        self.currentButton.clicked.connect(self.onCurrentLength)
        lengthRow.addWidget(self.currentButton)
        form.addRow(translate("Assembly", "Free length"), lengthRow)
        self.lengthLabel = QtWidgets.QLabel()
        form.addRow(translate("Assembly", "Length now"), self.lengthLabel)
        self.preloadBox = self.quantityBox("Preload")
        self.preloadBox.setToolTip(
            translate("Assembly", "A constant force pushing the ends apart (negative: pulling)")
        )
        form.addRow(translate("Assembly", "Preload"), self.preloadBox)
        self.dampingBox = self.quantityBox("Damping")
        self.dampingBox.setToolTip(
            translate("Assembly", "The damper's force over the ends' speed apart, kg/s (N s / m)")
        )
        form.addRow(translate("Assembly", "Damping"), self.dampingBox)
        layout.addWidget(springBox)

    def firstUnset(self):
        for i in (1, 2):
            if endBinding(self.spring, i) is None and self.spring.getEnd(i)[0] is None:
                return i
        return None

    def length(self):
        return (self.spring.getEndPoint(2) - self.spring.getEndPoint(1)).Length

    def fill(self):
        for i in (1, 2):
            self.endLabels[i].setText(describeEnd(self.spring, i))
            binding = endBinding(self.spring, i)
            self.freeButtons[i].setEnabled(binding is not None)
            self.offsetBoxes[i].setEnabled(binding is not None)
            self.offsetBoxes[i].blockSignals(True)
            self.offsetBoxes[i].setProperty(
                "rawValue", binding.Offset.Base.z if binding is not None else 0.0
            )
            self.offsetBoxes[i].blockSignals(False)
            self.pickButtons[i].setChecked(self.armed == i)
        if self.armed is None:
            self.hint.setText(
                translate("Assembly", "Press Pick to put an end on an element in the 3D view")
            )
        else:
            self.hint.setText(
                translate("Assembly", "Pick an element for end {} in the 3D view").format(
                    self.armed
                )
            )
        self.lengthLabel.setText("%.4g mm" % self.length())

    def arm(self, i):
        self.armed = i
        for j, button in self.pickButtons.items():
            button.setChecked(j == i)
        self.showPreview(None)
        if hasattr(self, "hint"):
            self.fill()

    # ------------------------------------------------------------- editing

    def changed(self):
        recomputeEnds(self.spring)
        if (
            not self.freeLengthEdited
            and endBinding(self.spring, 1) is not None
            and endBinding(self.spring, 2) is not None
        ):
            # Free at the distance between the ends until the user says
            # otherwise, as the command makes it
            self.setFreeLength(self.length())
        self.fill()

    def setFreeLength(self, value):
        self.freeLengthBox.blockSignals(True)
        self.spring.FreeLength = value
        self.freeLengthBox.setProperty("rawValue", value)
        self.freeLengthBox.blockSignals(False)

    def onQuantity(self, prop, quantity):
        setattr(self.spring, prop, quantity)
        if prop == "FreeLength":
            self.freeLengthEdited = True

    def onCurrentLength(self):
        self.setFreeLength(self.length())
        self.freeLengthEdited = True
        self.fill()

    def onFree(self, i):
        self.spring.clearEnd(i)
        self.changed()

    def onOffset(self, i, quantity):
        binding = endBinding(self.spring, i)
        if binding is None:
            return
        plc = binding.Offset
        plc.Base.z = quantity.Value
        binding.Offset = plc
        self.changed()

    def pickEnd(self, pick):
        i = self.armed
        if i is None:
            return False
        other = self.spring.getEnd(3 - i)[0]
        if other is not None and other == pick[0]:
            App.Console.PrintWarning(
                translate("Assembly", "Spring / Damper: the two ends are on one part.\n")
            )
            return False
        self.spring.setEnd(i, *pick)
        # The other end next, while it has none
        unset = self.firstUnset()
        self.armed = unset if unset != i else None
        self.changed()
        return True

    # ------------------------------------------------------------- picking

    def addSelection(self, doc_name, obj_name, sub_name, mousePos):
        root = App.getDocument(doc_name).getObject(obj_name)
        pos = App.Vector(*mousePos[:3]) if mousePos else None
        pick = pickOf(self.assembly, root, sub_name, pos)
        if pick is not None:
            self.pickEnd(pick)
        # The picks are the spring's ends, not a selection
        QtCore.QTimer.singleShot(0, Gui.Selection.clearSelection)

    def setPreselection(self, doc_name, obj_name, sub_name):
        self.hovered = None
        if self.armed is None or not sub_name:
            return
        root = App.getDocument(doc_name).getObject(obj_name)
        self.hovered = (root, sub_name)

    def removePreselection(self, doc_name, obj_name, sub_name):
        self.hovered = None
        self.showPreview(None)

    def moveMouse(self, info):
        if self.armed is None or self.hovered is None or self.view is None:
            self.showPreview(None)
            return
        cursor = self.view.getObjectInfo(self.view.getCursorPos())
        if not cursor:
            self.showPreview(None)
            return
        pos = App.Vector(cursor["x"], cursor["y"], cursor["z"])
        pick = pickOf(self.assembly, self.hovered[0], self.hovered[1], pos)
        self.showPreview(hoverPoint(pick) if pick is not None else None)

    def showPreview(self, point):
        """A dashed line from the other end to where the picked end would go,
        and a dot there; hidden with no point"""
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
            material.diffuseColor = (0.2, 0.45, 0.85)
            material.emissiveColor = (0.2, 0.45, 0.85)
            sep.addChild(material)
            style = coin.SoDrawStyle()
            style.lineWidth = 2
            style.linePattern = 0xF0F0
            style.pointSize = 7
            sep.addChild(style)
            self.previewCoords = coin.SoCoordinate3()
            sep.addChild(self.previewCoords)
            self.previewLine = coin.SoLineSet()
            sep.addChild(self.previewLine)
            self.previewDot = coin.SoPointSet()
            self.previewDot.startIndex = 0
            self.previewDot.numPoints = 1
            sep.addChild(self.previewDot)
            self.preview.addChild(sep)
            self.view.getSceneGraph().addChild(self.preview)
        if self.preview is None:
            return
        points = [point]
        other = 3 - self.armed
        if endBinding(self.spring, other) is not None or self.spring.getEnd(other)[0] is not None:
            points.append(self.spring.getEndPoint(other))
        self.previewCoords.point.setValues(0, len(points), [tuple(p) for p in points])
        self.previewCoords.point.setNum(len(points))
        self.previewLine.numVertices.setValue(len(points) if len(points) > 1 else 0)
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
        self.deactivate()
        recomputeEnds(self.spring)
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


# The joints whose limits a stop stops
StopJointTypes = ("Slider", "Cylindrical", "Revolute")


def selectedStopJoint():
    """The one Slider, Cylindrical or Revolute joint selected, else None"""
    joints = [
        o for o in Gui.Selection.getSelection() if getattr(o, "JointType", None) in StopJointTypes
    ]
    return joints[0] if len(joints) == 1 else None


class CommandCreateJointStop:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_JointStop",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateJointStop", "Joint Stop"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateJointStop",
                "How a dynamic or static simulation stops a joint at its limits: rigid "
                "with its own\nrestitution, a spring and a damper (penalty), or not at all. "
                "Select a slider,\ncylindrical or revolute joint first.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive() and selectedStopJoint() is not None

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        joint = selectedStopJoint()
        if not assembly or joint is None:
            return
        stop = makeJointStop(assembly, joint)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(stop)


def makeJointStop(assembly, joint, mode="Penalty"):
    """A stop for a joint's limits, a penalty one by default (a joint without
    one stops rigidly already), in a transaction; ticked in every Dynamic and
    Static study of the assembly."""
    doc = assembly.Document
    doc.openTransaction(translate("Assembly", "Create joint stop"))
    try:
        physics = assembly.getGroup("Physics", True)
        stop = physics.newObject("Assembly::JointStop", "JointStop")
        stop.Joint = joint
        stop.Mode = mode
        sims = assembly.getGroup("Simulations")
        for sim in sims.Group if sims else []:
            if getattr(sim, "AnalysisType", None) in ("Dynamic", "Static"):
                sim.Elements = list(sim.Elements) + [stop]
    finally:
        doc.commitTransaction()
    return stop


if App.GuiUp:
    Gui.addCommand("Assembly_CreateSpring", CommandCreateSpring())
    Gui.addCommand("Assembly_CreateTorsionSpring", CommandCreateTorsionSpring())
    Gui.addCommand("Assembly_CreateJointStop", CommandCreateJointStop())
