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

"""Mass Properties: each component's material, density, volume, mass and
centre of mass, and the assembly's in total, with a mass override per
component (docs/AssemblyDynamics.md sec 9, phase D0)."""

import FreeCAD as App

from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtCore, QtGui, QtWidgets

import UtilsAssembly

translate = App.Qt.translate

# The warning colour of an assumed density
ASSUMED = (200, 110, 0)
COLUMNS = 7
OVERRIDE = 6


def quantity(value, unit):
    return App.Units.Quantity(value, unit).UserString


def overridesOf(assembly):
    """Each component's mass override, by the component's name."""
    found = {}
    for obj in assembly.Group:
        if obj.TypeId == "Assembly::MassOverride" and obj.Component:
            found[obj.Component.Name] = obj
    return found


class CommandMassProperties:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_MassProperties",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_MassProperties", "Mass Properties"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_MassProperties",
                "Shows each component's material, density, volume, mass and centre of mass, "
                "and the assembly's in total,\nand overrides a component's mass.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive()

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly:
            return
        Gui.Control.showDialog(TaskMassProperties(assembly))


class DocumentWatch:
    """Refreshes the panel, once, after the document changes."""

    def __init__(self, task):
        self.task = task
        self.pending = False

    def slotChangedObject(self, obj, prop):
        if obj.Document != self.task.assembly.Document or self.pending:
            return
        if prop in ("Label", "Visibility"):
            return
        self.pending = True
        QtCore.QTimer.singleShot(0, self.refresh)

    def slotDeletedObject(self, obj):
        self.slotChangedObject(obj, "")

    def refresh(self):
        self.pending = False
        if self.task.form is not None:
            self.task.fill()


class TaskMassProperties:
    def __init__(self, assembly):
        self.assembly = assembly
        self.rows = []
        self.filling = False

        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Mass Properties"))
        self.form.setWindowIcon(QtGui.QIcon(":/icons/Assembly_MassProperties.svg"))
        layout = QtWidgets.QVBoxLayout(self.form)

        self.defaultMaterial = QtWidgets.QLabel()
        self.defaultMaterial.setWordWrap(True)
        layout.addWidget(self.defaultMaterial)

        self.table = QtWidgets.QTreeWidget()
        self.table.setRootIsDecorated(False)
        self.table.setUniformRowHeights(True)
        self.table.setHeaderLabels(
            [
                translate("Assembly", "Component"),
                translate("Assembly", "Material"),
                translate("Assembly", "Density"),
                translate("Assembly", "Volume"),
                translate("Assembly", "Mass"),
                translate("Assembly", "Center of mass"),
                translate("Assembly", "Override"),
            ]
        )
        self.table.headerItem().setToolTip(
            OVERRIDE,
            translate(
                "Assembly",
                "A mass override of the component, in the assembly's Physics group: its "
                "density, its mass, or its mass, centre of mass and inertia.\nTicked, it "
                "starts at the mass shown; edit it in the property view.",
            ),
        )
        layout.addWidget(self.table)

        self.summary = QtWidgets.QLabel()
        self.summary.setWordWrap(True)
        self.summary.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self.summary)

        self.table.itemChanged.connect(self.onItemChanged)
        self.table.itemSelectionChanged.connect(self.onSelected)
        self.watch = DocumentWatch(self)
        App.addDocumentObserver(self.watch)
        self.fill()

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Close

    def reject(self):
        self.finish()
        return True

    def accept(self):
        self.finish()
        return True

    def finish(self):
        App.removeDocumentObserver(self.watch)
        self.form = None
        Gui.Control.closeDialog()

    def fill(self):
        """The table from the assembly's mass properties as they are now."""
        if self.form is None:
            return
        props = self.assembly.getMassProperties()
        overrides = overridesOf(self.assembly)
        defaultName = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly").GetString(
            "DefaultMaterial", ""
        )
        assumed = [c for c in props["Components"] if c["Origin"] == "Assumed"]
        material = assumed[0]["Material"] if assumed else ""
        self.defaultMaterial.setText(
            translate(
                "Assembly",
                "A part with no material, or one without a density, is taken to be of the "
                "default material{name} (Preferences: Mod/Assembly/DefaultMaterial{set}); "
                "those are in orange.",
            ).format(
                name=(" (" + material + ")") if material else "",
                set="" if defaultName else translate("Assembly", ", not set"),
            )
        )

        self.filling = True
        self.table.clear()
        self.rows = []
        warn = QtGui.QBrush(QtGui.QColor(*ASSUMED))
        for c in props["Components"]:
            obj = c["Object"]
            origin = c["Origin"]
            if c["Material"]:
                material = c["Material"]
            elif origin == "None":
                material = "-"
            elif origin == "Override":
                material = translate("Assembly", "overridden")
            else:
                material = translate("Assembly", "several")
            columns = [
                obj.Label,
                material,
                quantity(c["Density"], App.Units.Density) if c["Density"] > 0 else "",
                quantity(c["Volume"], App.Units.Volume) if c["Volume"] > 0 else "",
                quantity(c["Mass"], App.Units.Mass),
                self.point(c["CenterOfMass"]) if c["Mass"] > 0 else "",
                "",
            ]
            item = QtWidgets.QTreeWidgetItem(columns)
            item.setData(0, QtCore.Qt.UserRole, len(self.rows))
            item.setFlags(item.flags() | QtCore.Qt.ItemIsUserCheckable)
            item.setCheckState(
                OVERRIDE, QtCore.Qt.Checked if obj.Name in overrides else QtCore.Qt.Unchecked
            )
            if origin == "Assumed":
                tip = translate(
                    "Assembly",
                    "Assumed: its material gives no density, so the default material's is used",
                )
                for column in (1, 2, 4):
                    item.setForeground(column, warn)
                    item.setToolTip(column, tip)
            elif origin == "Override":
                tip = translate("Assembly", "Given by its mass override")
                for column in (2, 4, 5):
                    item.setToolTip(column, tip)
            elif origin == "None":
                item.setToolTip(4, translate("Assembly", "No solid: no mass"))
            self.table.addTopLevelItem(item)
            self.rows.append(c)

        total = props["Total"]
        totalItem = QtWidgets.QTreeWidgetItem(
            [
                translate("Assembly", "Total"),
                total["Material"] if total["Material"] else "",
                "",
                quantity(total["Volume"], App.Units.Volume) if total["Volume"] > 0 else "",
                quantity(total["Mass"], App.Units.Mass),
                self.point(total["CenterOfMass"]) if total["Mass"] > 0 else "",
                "",
            ]
        )
        font = totalItem.font(0)
        font.setBold(True)
        for column in range(COLUMNS):
            totalItem.setFont(column, font)
        totalItem.setFlags(QtCore.Qt.ItemIsEnabled)
        self.table.addTopLevelItem(totalItem)
        for column in range(COLUMNS):
            self.table.resizeColumnToContents(column)
        self.filling = False

        inertia = total["Inertia"]
        unit = translate("Assembly", "kg mm^2")
        self.summary.setText(
            translate(
                "Assembly",
                "The whole: {mass}, the centre of mass at {centre}; the inertia about it, "
                "along the assembly's axes, in {unit}: Ixx {xx:.6g}, Iyy {yy:.6g}, "
                "Izz {zz:.6g}, Ixy {xy:.6g}, Ixz {xz:.6g}, Iyz {yz:.6g}.{assumed}",
            ).format(
                mass=quantity(total["Mass"], App.Units.Mass),
                centre=self.point(total["CenterOfMass"]),
                unit=unit,
                xx=inertia[0],
                yy=inertia[1],
                zz=inertia[2],
                xy=inertia[3],
                xz=inertia[4],
                yz=inertia[5],
                assumed=(
                    " "
                    + translate(
                        "Assembly", "{n} of the components have the default material's density."
                    ).format(n=len(assumed))
                    if assumed
                    else ""
                ),
            )
        )

    @staticmethod
    def point(v):
        return "(%s, %s, %s)" % tuple(quantity(x, App.Units.Length) for x in (v.x, v.y, v.z))

    def onItemChanged(self, item, column):
        if self.filling or column != OVERRIDE:
            return
        index = item.data(0, QtCore.Qt.UserRole)
        if index is None:
            return
        self.setOverride(self.rows[index], item.checkState(OVERRIDE) == QtCore.Qt.Checked)

    def setOverride(self, props, on):
        """Give the component a mass override, at the mass it has now, or take
        it away; in a transaction."""
        obj = props["Object"]
        doc = self.assembly.Document
        existing = overridesOf(self.assembly).get(obj.Name)
        if on == (existing is not None):
            return
        if on:
            doc.openTransaction(translate("Assembly", "Override mass"))
            try:
                physics = self.assembly.getGroup("Physics", True)
                given = physics.newObject("Assembly::MassOverride", "MassOverride")
                given.Label = translate("Assembly", "Mass of {label}").format(label=obj.Label)
                given.Component = obj
                given.Mode = "Mass"
                given.Mass = props["Mass"]
                given.Density = props["Density"] if props["Density"] > 0 else given.Density
            finally:
                doc.commitTransaction()
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(given)
        else:
            doc.openTransaction(translate("Assembly", "Remove mass override"))
            try:
                doc.removeObject(existing.Name)
            finally:
                doc.commitTransaction()
        # Later: this runs from the item's own itemChanged, inside its
        # setData, and filling clears the table -- the item with it, which
        # Qt is still in (an access violation in QTreeWidgetItem::setData)
        QtCore.QTimer.singleShot(0, self.fill)

    def onSelected(self):
        items = self.table.selectedItems()
        if not items:
            return
        index = items[0].data(0, QtCore.Qt.UserRole)
        if index is None:
            return
        obj = self.rows[index]["Object"]
        given = overridesOf(self.assembly).get(obj.Name)
        Gui.Selection.clearSelection()
        # The override, to edit in the property view; else the component
        Gui.Selection.addSelection(given if given is not None else obj)


if App.GuiUp:
    Gui.addCommand("Assembly_MassProperties", CommandMassProperties())
