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

"""Check Interference: which components of the active assembly overlap, and
which touch (docs/AssemblyInterference.md)."""

import time

import FreeCAD as App

from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtCore, QtGui, QtWidgets

import UtilsAssembly

translate = App.Qt.translate

PARAMS = "User parameter:BaseApp/Preferences/Mod/Assembly"


class CommandCheckInterference:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_CheckInterference",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CheckInterference", "Check Interference"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CheckInterference",
                "Finds the components of the active assembly that overlap, and, if asked, "
                "those that touch.\nExact works on the solids; Fast on meshes of them.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive()

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly:
            return
        Gui.Control.showDialog(TaskCheckInterference(assembly))


def _overlapNode(shape, placement):
    """A red, half transparent drawing of the shape, under the placement."""
    from pivy import coin

    root = coin.SoSeparator()
    transform = coin.SoTransform()
    transform.translation.setValue(tuple(placement.Base))
    q = placement.Rotation.Q
    transform.rotation.setValue(q[0], q[1], q[2], q[3])
    root.addChild(transform)
    material = coin.SoMaterial()
    material.diffuseColor.setValue(1.0, 0.1, 0.1)
    material.emissiveColor.setValue(0.4, 0.0, 0.0)
    material.transparency.setValue(0.3)
    root.addChild(material)
    style = coin.SoDrawStyle()
    root.addChild(style)
    points, triangles = shape.tessellate(max(shape.BoundBox.DiagonalLength * 1e-3, 1e-3))
    coords = coin.SoCoordinate3()
    coords.point.setValues(0, len(points), [tuple(p) for p in points])
    root.addChild(coords)
    faces = coin.SoIndexedFaceSet()
    indices = []
    for t in triangles:
        indices.extend([t[0], t[1], t[2], -1])
    faces.coordIndex.setValues(0, len(indices), indices)
    root.addChild(faces)
    return root


class TaskCheckInterference:
    def __init__(self, assembly):
        self.assembly = assembly
        self.results = []
        self.overlap = None
        self.params = App.ParamGet(PARAMS)

        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Check Interference"))
        self.form.setWindowIcon(QtGui.QIcon(":/icons/Assembly_CheckInterference.svg"))
        layout = QtWidgets.QVBoxLayout(self.form)

        options = QtWidgets.QFormLayout()
        self.mode = QtWidgets.QComboBox()
        self.mode.addItem(translate("Assembly", "Exact"), "Exact")
        self.mode.addItem(translate("Assembly", "Fast"), "Fast")
        self.mode.setToolTip(
            translate(
                "Assembly",
                "Exact: on the solids -- the volume in common, the exact distance; "
                "as slow as a boolean.\n"
                "Fast: on meshes of the solids -- their surfaces crossing, confirmed "
                "on the solids; within the meshes' error.",
            )
        )
        options.addRow(translate("Assembly", "Mode"), self.mode)

        self.tolerance = QtWidgets.QDoubleSpinBox()
        self.tolerance.setDecimals(4)
        self.tolerance.setRange(0.0, 1000.0)
        self.tolerance.setSingleStep(0.01)
        self.tolerance.setSuffix(" mm")
        self.tolerance.setToolTip(
            translate(
                "Assembly",
                "Components closer than this touch, and interfere only when they overlap.\n"
                "Fast takes it at least twice the meshes' deflection.",
            )
        )
        options.addRow(translate("Assembly", "Tolerance"), self.tolerance)

        self.deflection = QtWidgets.QDoubleSpinBox()
        self.deflection.setDecimals(4)
        self.deflection.setRange(0.0, 100.0)
        self.deflection.setSingleStep(0.01)
        self.deflection.setSuffix(" mm")
        self.deflection.setSpecialValueText(translate("Assembly", "Automatic"))
        self.deflection.setToolTip(
            translate(
                "Assembly",
                "Fast only: the meshes' chord error. Automatic is 1/10000 of each "
                "component's size.",
            )
        )
        self.deflectionLabel = QtWidgets.QLabel(translate("Assembly", "Mesh deflection"))
        options.addRow(self.deflectionLabel, self.deflection)

        self.contacts = QtWidgets.QCheckBox(translate("Assembly", "Report touching components"))
        options.addRow(self.contacts)
        layout.addLayout(options)

        self.check = QtWidgets.QPushButton(translate("Assembly", "Check"))
        layout.addWidget(self.check)

        self.summary = QtWidgets.QLabel()
        self.summary.setWordWrap(True)
        layout.addWidget(self.summary)

        self.table = QtWidgets.QTreeWidget()
        self.table.setRootIsDecorated(False)
        self.table.setHeaderLabels(
            [
                translate("Assembly", "Component"),
                translate("Assembly", "Component"),
                translate("Assembly", "Kind"),
                translate("Assembly", "Amount"),
            ]
        )
        layout.addWidget(self.table)

        mode = self.params.GetString("InterferenceMode", "Exact")
        self.mode.setCurrentIndex(max(0, self.mode.findData(mode)))
        self.tolerance.setValue(self.params.GetFloat("InterferenceTolerance", 0.0))
        self.deflection.setValue(self.params.GetFloat("InterferenceDeflection", 0.0))
        self.contacts.setChecked(self.params.GetBool("InterferenceContacts", False))
        self.onModeChanged()

        self.mode.currentIndexChanged.connect(self.onModeChanged)
        self.check.clicked.connect(self.run)
        self.table.itemSelectionChanged.connect(self.onSelected)

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Close

    def reject(self):
        self.finish()
        return True

    def accept(self):
        self.finish()
        return True

    def finish(self):
        self.showOverlap(None)
        Gui.Control.closeDialog()

    def onModeChanged(self, *_):
        fast = self.mode.currentData() == "Fast"
        self.deflection.setVisible(fast)
        self.deflectionLabel.setVisible(fast)

    def run(self):
        mode = self.mode.currentData()
        tolerance = self.tolerance.value()
        contacts = self.contacts.isChecked()
        deflection = self.deflection.value()
        self.params.SetString("InterferenceMode", mode)
        self.params.SetFloat("InterferenceTolerance", tolerance)
        self.params.SetFloat("InterferenceDeflection", deflection)
        self.params.SetBool("InterferenceContacts", contacts)

        self.showOverlap(None)
        self.table.clear()
        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        try:
            start = time.perf_counter()
            self.results = self.assembly.findInterferences(
                mode=mode, tolerance=tolerance, contacts=contacts, deflection=deflection
            )
            seconds = time.perf_counter() - start
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()

        interferences = 0
        for result in self.results:
            if result["Kind"] == "Interference":
                interferences += 1
                kind = translate("Assembly", "Interference")
                if mode == "Exact":
                    amount = "%.6g mm^3" % result["Volume"]
                else:
                    amount = translate("Assembly", "overlap")
            else:
                kind = translate("Assembly", "Contact")
                if mode == "Exact":
                    amount = "%.4g mm" % result["Distance"]
                else:
                    amount = translate("Assembly", "within %.4g mm") % result["Tolerance"]
            item = QtWidgets.QTreeWidgetItem(
                [result["Object1"].Label, result["Object2"].Label, kind, amount]
            )
            if result["Kind"] == "Interference":
                item.setForeground(2, QtGui.QBrush(QtGui.QColor(200, 0, 0)))
            self.table.addTopLevelItem(item)
        for column in range(4):
            self.table.resizeColumnToContents(column)

        tolerances = [r["Tolerance"] for r in self.results]
        used = max(tolerances) if tolerances else tolerance
        self.summary.setText(
            translate(
                "Assembly",
                "{interferences} interfering, {contacts} touching -- {mode}, "
                "tolerance {tolerance:.4g} mm, {seconds:.2f} s",
            ).format(
                interferences=interferences,
                contacts=len(self.results) - interferences,
                mode=self.mode.currentText(),
                tolerance=used,
                seconds=seconds,
            )
        )

    def onSelected(self):
        items = self.table.selectedItems()
        if not items:
            self.showOverlap(None)
            return
        result = self.results[self.table.indexOfTopLevelItem(items[0])]
        Gui.Selection.clearSelection()
        for obj in (result["Object1"], result["Object2"]):
            self.select(obj)
        self.showOverlap(result["Overlap"])

    def select(self, obj):
        """Select the component through the assembly, from the document's root."""
        path = []
        current = obj
        while current is not None and current != self.assembly:
            path.insert(0, current.Name)
            current = current.getParentGeoFeatureGroup()
        if current != self.assembly:
            Gui.Selection.addSelection(obj)
            return
        root = self.assembly
        prefix = ""
        parents = self.assembly.Parents
        if parents:
            root, prefix = parents[0]
        sub = prefix + ".".join(path) + "."
        Gui.Selection.addSelection(root.Document.Name, root.Name, sub)

    def showOverlap(self, shape):
        view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
        if self.overlap is not None and view is not None:
            try:
                view.getSceneGraph().removeChild(self.overlap)
            except Exception:
                pass
        self.overlap = None
        if shape is None or view is None:
            return
        self.overlap = _overlapNode(shape, self.assembly.getGlobalPlacement())
        view.getSceneGraph().addChild(self.overlap)


if App.GuiUp:
    Gui.addCommand("Assembly_CheckInterference", CommandCheckInterference())
