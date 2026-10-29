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

"""Contact sets (docs/AssemblyDynamics.md sec 7, phase D2): the components
of one group touching those of another, or one another, in the Dynamic and
Static studies, by the impact law and friction of Assembly::ContactSet. The
command makes one of the components selected and opens its task panel, which
a double click on a contact set opens too."""

import FreeCAD as App

from PySide import QtCore, QtWidgets
from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui

import UtilsAssembly

translate = App.Qt.translate

# The law's plain numbers: the property, its label, the unit shown after it,
# its range and decimals. Penetration and the velocities are quantities, shown
# in their own units.
Numbers = [
    ("Stiffness", QT_TRANSLATE_NOOP("Assembly", "Stiffness"), "N/mm^e", 1e12, 4),
    ("Exponent", QT_TRANSLATE_NOOP("Assembly", "Exponent (e)"), "", 10.0, 3),
    ("MaxDamping", QT_TRANSLATE_NOOP("Assembly", "Damping"), "N s/mm", 1e12, 4),
    ("StaticFriction", QT_TRANSLATE_NOOP("Assembly", "Static friction"), "", 10.0, 3),
    ("DynamicFriction", QT_TRANSLATE_NOOP("Assembly", "Dynamic friction"), "", 10.0, 3),
]
Quantities = [
    ("Penetration", QT_TRANSLATE_NOOP("Assembly", "Full damping at")),
    ("StictionVelocity", QT_TRANSLATE_NOOP("Assembly", "Stiction velocity")),
    ("FrictionVelocity", QT_TRANSLATE_NOOP("Assembly", "Friction velocity")),
]
# Where each goes in the form, top to bottom
Order = [
    "Stiffness",
    "Exponent",
    "MaxDamping",
    "Penetration",
    "StaticFriction",
    "DynamicFriction",
    "StictionVelocity",
    "FrictionVelocity",
]


def newContactSet(assembly, groupA, groupB=()):
    """A contact set in the assembly's Physics group: the components of
    groupA touching those of groupB, or, groupB empty, one another; ticked in
    every Dynamic and Static study of the assembly"""
    physics = assembly.getGroup("Physics", True)
    contact = physics.newObject("Assembly::ContactSet", "ContactSet")
    contact.GroupA = list(groupA)
    contact.GroupB = list(groupB)
    sims = assembly.getGroup("Simulations")
    for sim in sims.Group if sims else []:
        if getattr(sim, "AnalysisType", None) in ("Dynamic", "Static"):
            sim.Elements = list(sim.Elements) + [contact]
    return contact


def selectedComponents(assembly):
    """The components of the assembly the selection is on, in its order, each
    once"""
    components = []
    for sel in Gui.Selection.getSelectionEx("", 0):
        for sub in sel.SubElementNames:
            component, _ = UtilsAssembly.getComponentReference(assembly, sel.Object, sub)
            if component is not None and component not in components:
                components.append(component)
    return components


def editContactSet(contact):
    """Open the task panel of a contact set, as a double click does"""
    if Gui.Control.activeDialog():
        App.Console.PrintWarning(
            translate("Assembly", "Close the open task panel before editing the contact set.\n")
        )
        return
    Gui.Control.showDialog(TaskContact(contact))


class CommandCreateContact:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_Contact",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateContact", "Contact Set"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateContact",
                "Components touching one another in a dynamic or static simulation: those\n"
                "selected, or picked in the panel, in group A, touching those of group B or,\n"
                "group B empty, one another.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive() and not Gui.Control.activeDialog()

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly:
            return
        components = selectedComponents(assembly)
        Gui.Selection.clearSelection()
        doc = assembly.Document
        doc.openTransaction(translate("Assembly", "Create contact set"))
        try:
            contact = newContactSet(assembly, components)
            Gui.Control.showDialog(TaskContact(contact, creating=True))
        except Exception:
            # The panel's Cancel would have undone it
            doc.abortTransaction()
            raise


class ContactSelGate:
    """While the panel picks, only what is on a component of the assembly"""

    def __init__(self, task):
        self.task = task

    def allow(self, doc, obj, sub):
        if self.task.armed is None:
            return True
        component, _ = UtilsAssembly.getComponentReference(self.task.assembly, obj, sub)
        return component is not None


# The open contact set panel, as JointObject.activeTask is the joint's
activeTask = None


class TaskContact(QtCore.QObject):
    """The task panel of a contact set: its two groups, each picked in the 3D
    view (a pick adds a component, or takes away one already in the group),
    and its law. The changes are one transaction: Cancel undoes them, and
    deletes a contact set the command made."""

    def __init__(self, contact, creating=False):
        super().__init__()
        global activeTask
        activeTask = self
        self.contact = contact
        self.creating = creating
        self.doc = contact.Document
        self.assembly = None
        for obj in contact.InListRecursive:
            if obj.isDerivedFrom("Assembly::AssemblyObject"):
                self.assembly = obj
                break
        if not creating:
            self.doc.openTransaction(translate("Assembly", "Edit contact set"))
        # The group the next pick is for: "A", "B" or None
        self.armed = "A" if creating and not contact.GroupA else None
        self.buildForm()
        self.fill()

        if self.assembly is not None:
            Gui.Selection.clearSelection()
            Gui.Selection.addSelectionGate(
                ContactSelGate(self), Gui.Selection.ResolveMode.NoResolve
            )
            Gui.Selection.addObserver(self, Gui.Selection.ResolveMode.NoResolve)

    # ------------------------------------------------------------- the form

    def buildForm(self):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Contact Set"))
        layout = QtWidgets.QVBoxLayout(self.form)

        groupsBox = QtWidgets.QGroupBox(translate("Assembly", "Components"))
        grid = QtWidgets.QGridLayout(groupsBox)
        # group -> (its list, its pick button)
        self.groups = {}
        texts = {
            "A": (
                translate("Assembly", "Group A"),
                translate("Assembly", "Pick the components of group A in the 3D view"),
            ),
            "B": (
                translate("Assembly", "Group B"),
                translate(
                    "Assembly",
                    "Pick the components group A touches in the 3D view; none: those of\n"
                    "group A touch one another",
                ),
            ),
        }
        for row, group in enumerate("AB"):
            text, tip = texts[group]
            grid.addWidget(QtWidgets.QLabel(text), 2 * row, 0)
            pickButton = QtWidgets.QPushButton(translate("Assembly", "Pick"))
            pickButton.setCheckable(True)
            pickButton.setToolTip(tip)
            pickButton.clicked.connect(
                lambda checked, group=group: self.arm(group if checked else None)
            )
            grid.addWidget(pickButton, 2 * row, 1)
            removeButton = QtWidgets.QPushButton(translate("Assembly", "Remove"))
            removeButton.setToolTip(
                translate("Assembly", "Take the components chosen in the list out")
            )
            removeButton.clicked.connect(lambda checked=False, group=group: self.onRemove(group))
            grid.addWidget(removeButton, 2 * row, 2)
            members = QtWidgets.QListWidget()
            members.setSelectionMode(QtWidgets.QAbstractItemView.ExtendedSelection)
            members.setMaximumHeight(110)
            grid.addWidget(members, 2 * row + 1, 0, 1, 3)
            self.groups[group] = (members, pickButton)
        self.hint = QtWidgets.QLabel()
        self.hint.setWordWrap(True)
        grid.addWidget(self.hint, 4, 0, 1, 3)
        layout.addWidget(groupsBox)

        lawBox = QtWidgets.QGroupBox(translate("Assembly", "Contact law"))
        form = QtWidgets.QFormLayout(lawBox)
        # property -> its box
        self.boxes = {}
        labels = {}
        for prop, label, unit, top, decimals in Numbers:
            box = QtWidgets.QDoubleSpinBox()
            box.setDecimals(decimals)
            box.setRange(1.0 if prop == "Exponent" else 0.0, top)
            if unit:
                box.setSuffix(" " + unit)
            box.valueChanged.connect(lambda value, prop=prop: self.onNumber(prop, value))
            self.boxes[prop] = box
            labels[prop] = label
        for prop, label in Quantities:
            box = Gui.UiLoader().createWidget("Gui::QuantitySpinBox")
            # Its unit with it
            box.setProperty("value", getattr(self.contact, prop))
            box.setProperty("minimum", 0.0)
            Gui.ExpressionBinding(box).bind(self.contact, prop)
            box.valueChanged.connect(lambda q, prop=prop: setattr(self.contact, prop, q))
            self.boxes[prop] = box
            labels[prop] = label
        for prop in Order:
            self.boxes[prop].setToolTip(self.contact.getDocumentationOfProperty(prop))
            form.addRow(translate("Assembly", labels[prop]), self.boxes[prop])
        layout.addWidget(lawBox)

    def fill(self):
        for group, prop in (("A", "GroupA"), ("B", "GroupB")):
            members, pickButton = self.groups[group]
            members.clear()
            for component in getattr(self.contact, prop):
                item = QtWidgets.QListWidgetItem(component.Label)
                item.setData(QtCore.Qt.UserRole, component.Name)
                members.addItem(item)
            pickButton.setChecked(self.armed == group)
        if self.armed == "A":
            self.hint.setText(
                translate(
                    "Assembly",
                    "Pick the components of group A in the 3D view; picking one already in it "
                    "takes it out",
                )
            )
        elif self.armed == "B":
            self.hint.setText(
                translate(
                    "Assembly",
                    "Pick the components group A touches in the 3D view; picking one already "
                    "in group B takes it out",
                )
            )
        elif not self.contact.GroupA:
            self.hint.setText(translate("Assembly", "Group A is empty: nothing touches."))
        elif not self.contact.GroupB:
            self.hint.setText(
                translate(
                    "Assembly", "Group B is empty: the components of group A touch one another."
                )
            )
        else:
            self.hint.setText(
                translate("Assembly", "The components of group A touch those of group B.")
            )
        for prop, *_ in Numbers:
            box = self.boxes[prop]
            box.blockSignals(True)
            box.setValue(getattr(self.contact, prop))
            box.blockSignals(False)

    def arm(self, group):
        self.armed = group
        self.fill()

    # ------------------------------------------------------------- editing

    def onNumber(self, prop, value):
        setattr(self.contact, prop, value)

    def toggle(self, component, group):
        """Put the component in the group, out of the other one, or take it out
        of the group when it is in already"""
        prop, otherProp = ("GroupA", "GroupB") if group == "A" else ("GroupB", "GroupA")
        members = list(getattr(self.contact, prop))
        if component in members:
            members.remove(component)
        else:
            members.append(component)
            others = list(getattr(self.contact, otherProp))
            if component in others:
                others.remove(component)
                setattr(self.contact, otherProp, others)
        setattr(self.contact, prop, members)
        self.fill()

    def onRemove(self, group):
        members = self.groups[group][0]
        names = {item.data(QtCore.Qt.UserRole) for item in members.selectedItems()}
        prop = "GroupA" if group == "A" else "GroupB"
        setattr(self.contact, prop, [o for o in getattr(self.contact, prop) if o.Name not in names])
        self.fill()

    # ------------------------------------------------------------- picking

    def addSelection(self, doc_name, obj_name, sub_name, mousePos):
        if self.armed is None:
            return
        root = App.getDocument(doc_name).getObject(obj_name)
        component, _ = UtilsAssembly.getComponentReference(self.assembly, root, sub_name)
        if component is not None:
            self.toggle(component, self.armed)
        # The picks are the groups', not a selection
        QtCore.QTimer.singleShot(0, Gui.Selection.clearSelection)

    # ------------------------------------------------------------- closing

    def deactivate(self):
        global activeTask
        if activeTask is self:
            activeTask = None
        if self.assembly is not None:
            Gui.Selection.removeSelectionGate()
            Gui.Selection.removeObserver(self)

    def accept(self):
        if not self.contact.GroupA:
            self.armed = "A"
            self.fill()
            App.Console.PrintWarning(
                translate("Assembly", "Contact set: pick the components of group A first.\n")
            )
            return False
        self.deactivate()
        self.doc.commitTransaction()
        Gui.Control.closeDialog()
        return True

    def reject(self):
        self.deactivate()
        self.doc.abortTransaction()
        Gui.Control.closeDialog()
        return True

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel


if App.GuiUp:
    Gui.addCommand("Assembly_CreateContact", CommandCreateContact())
