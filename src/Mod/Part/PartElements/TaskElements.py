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

"""The task panel that edits a container's Elements group.

Elements are declared on geometry picked in the 3D view. Tree and 3D view
selection are synchronised, so dragging a pick onto an element in the tree
means scrolling the tree back and forth; this panel lists the elements
instead, and Add, Replace and Remove take the 3D selection as it is
(docs/AssemblyObjectModel.md sec 4.3).

A pick is turned into what an element stores: a direct child of the
container and the rest of the path into it. A pick of another container's
element (its Elements group in the path) is stored as "$$Elements.$Label.",
which survives renaming the group.
"""

import FreeCAD as App
import FreeCADGui as Gui
import Part
from PySide import QtCore, QtGui, QtWidgets

translate = App.Qt.translate


def getContainer(obj):
    """The container whose elements obj is, or obj itself when it declares
    elements; None when it is neither"""
    if obj is None:
        return None
    if obj.isDerivedFrom("Part::Element"):
        for parent in obj.InList:
            container = getContainer(parent)
            if container is not None and container.Elements is not None:
                if obj in container.Elements.Group:
                    return container
        return None
    if obj.isDerivedFrom("App::AuxGroup"):
        for parent in obj.InList:
            if getattr(parent, "Elements", None) == obj:
                return parent
        return None
    if hasattr(obj, "Elements") and (
        obj.isDerivedFrom("App::Part") or obj.isDerivedFrom("PartDesign::Body")
    ):
        return obj
    return None


def elementsOf(container):
    group = getattr(container, "Elements", None)
    return list(group.Group) if group is not None else []


def _elementsGroupOf(obj):
    """The Elements group an object (or what it links to) holds, or None"""
    linked = obj.getLinkedObject(True)
    return getattr(linked, "Elements", None)


def relativePick(container, root, sub):
    """(target, subname) for a pick of root.sub, as an element of container
    stores it, or None when the pick is not inside the container.

    The target is the last object of the path that is a direct child of the
    container: a body owns its features flat, so a face picked through a
    solid body is declared on its feature, as sec 5.3 has it. The element is
    stored by its plain name: a selection's mapped name is the shape's it was
    picked on (a body's, say), not the target's, and the link property makes
    its own mapped name from the plain one.
    """
    path, _mapped, element = Part.splitSubname(sub)
    objs = root.getSubObjectList(path) if path else [root]
    tokens = path.split(".")[:-1] if path else []
    # objs[k] is the object tokens[k - 1] names; objs[0] is root
    start = None
    for i, obj in enumerate(objs):
        if obj == container or obj.getLinkedObject(True) == container:
            start = i
    if start is None:
        return None
    children = set(o.Name for o in getattr(container, "Group", []))
    target = None
    for i in range(start + 1, len(objs)):
        if objs[i].Name in children and objs[i].Document == container.Document:
            target = i
    if target is None:
        tip = getattr(container, "Tip", None)
        if tip is None or not element:
            return None
        # A body's own shape is its tip's
        return tip, ".".join(tokens[start:] + [element])
    # An element of the target's Elements group is taken whole, by property
    # and label, whatever of its copy was picked
    group = _elementsGroupOf(objs[target])
    if group is not None and target + 2 < len(objs) and objs[target + 1] == group:
        return objs[target], "$$Elements.$%s." % objs[target + 2].Label
    if not element:
        return None  # a whole object, not geometry
    return objs[target], ".".join(tokens[target:] + [element])


class ElementsTaskPanel:
    """Lists the elements of a container; Add, Replace and Remove take the
    current 3D selection"""

    def __init__(self, container, transaction=True):
        self.container = container
        self.doc = container.Document
        self.transaction = transaction
        if transaction:
            self.doc.openTransaction(translate("PartElements", "Edit elements"))
        self.form = self.makeForm()
        self.refresh()
        Gui.Selection.addObserver(self)
        self.updateButtons()

    # ------------------------------------------------------------ the form

    def makeForm(self):
        form = QtWidgets.QWidget()
        form.setWindowTitle(translate("PartElements", "Elements of %s") % self.container.Label)
        layout = QtWidgets.QVBoxLayout(form)

        hint = QtWidgets.QLabel(
            translate(
                "PartElements",
                "Select geometry in the 3D view, then add it as new elements,\n"
                "or replace the geometry of the element selected below.",
            )
        )
        hint.setWordWrap(True)
        layout.addWidget(hint)

        self.list = QtWidgets.QTreeWidget()
        self.list.setColumnCount(2)
        self.list.setHeaderLabels(
            [translate("PartElements", "Element"), translate("PartElements", "Geometry")]
        )
        self.list.setRootIsDecorated(False)
        self.list.setSelectionMode(QtWidgets.QAbstractItemView.ExtendedSelection)
        self.list.setEditTriggers(
            QtWidgets.QAbstractItemView.DoubleClicked | QtWidgets.QAbstractItemView.EditKeyPressed
        )
        self.list.itemChanged.connect(self.onItemChanged)
        self.list.itemSelectionChanged.connect(self.onListSelection)
        layout.addWidget(self.list)

        buttons = QtWidgets.QHBoxLayout()
        self.addButton = QtWidgets.QPushButton(translate("PartElements", "Add"))
        self.addButton.setToolTip(
            translate("PartElements", "Declare an element on each selected geometry")
        )
        self.replaceButton = QtWidgets.QPushButton(translate("PartElements", "Replace"))
        self.replaceButton.setToolTip(
            translate(
                "PartElements",
                "Give the element selected in the list the selected geometry instead",
            )
        )
        self.removeButton = QtWidgets.QPushButton(translate("PartElements", "Remove"))
        self.removeButton.setToolTip(
            translate("PartElements", "Delete the elements selected in the list")
        )
        self.addButton.clicked.connect(self.add)
        self.replaceButton.clicked.connect(self.replace)
        self.removeButton.clicked.connect(self.remove)
        for button in (self.addButton, self.replaceButton, self.removeButton):
            buttons.addWidget(button)
        layout.addLayout(buttons)

        self.status = QtWidgets.QLabel()
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        return form

    def describe(self, element):
        target = element.Target
        if target is None or target[0] is None:
            return translate("PartElements", "(none)")
        subs = target[1]
        text = target[0].Label
        if subs and subs[0]:
            text += "." + subs[0]
        if element.Detach:
            text += " " + translate("PartElements", "(detached)")
        elif "Invalid" in element.State or "Error" in element.State:
            text += " " + translate("PartElements", "(broken)")
        return text

    def refresh(self):
        selected = set(e.Name for e in self.selectedElements())
        self.list.blockSignals(True)
        self.list.clear()
        for element in elementsOf(self.container):
            item = QtWidgets.QTreeWidgetItem([element.Label, self.describe(element)])
            item.setData(0, QtCore.Qt.UserRole, element.Name)
            item.setFlags(item.flags() | QtCore.Qt.ItemIsEditable)
            self.list.addTopLevelItem(item)
            if element.Name in selected:
                item.setSelected(True)
        self.list.resizeColumnToContents(0)
        self.list.blockSignals(False)
        self.updateButtons()

    def selectedElements(self):
        res = []
        for item in self.list.selectedItems():
            element = self.doc.getObject(item.data(0, QtCore.Qt.UserRole))
            if element is not None:
                res.append(element)
        return res

    def updateButtons(self):
        picks = len(self.picks())
        rows = len(self.list.selectedItems())
        self.addButton.setEnabled(picks > 0)
        self.replaceButton.setEnabled(picks == 1 and rows == 1)
        self.removeButton.setEnabled(rows > 0)

    def say(self, text):
        self.status.setText(text)

    # ------------------------------------------------------------ picks

    def picks(self):
        """The 3D selection as (target, subname) pairs of this container"""
        res = []
        for sel in Gui.Selection.getSelectionEx("", 0):
            for sub in sel.SubElementNames:
                if not sub:
                    continue
                pick = relativePick(self.container, sel.Object, sub)
                if pick is not None and not self.isOwnElement(pick[0]):
                    res.append(pick)
        return res

    def isOwnElement(self, target):
        group = getattr(self.container, "Elements", None)
        return group is not None and (target == group or target in group.Group)

    # ------------------------------------------------------------ actions

    def add(self):
        picks = self.picks()
        made = []
        for target, sub in picks:
            made.append(Part.makeElement(self.container, target, sub))
        self.doc.recompute()
        Gui.Selection.clearSelection()
        self.refresh()
        self.say(translate("PartElements", "%d element(s) added") % len(made))
        return made

    def replace(self):
        elements = self.selectedElements()
        picks = self.picks()
        if len(elements) != 1 or len(picks) != 1:
            return
        element = elements[0]
        target, sub = picks[0]
        element.Target = (target, [sub])
        element.Detach = False
        self.doc.recompute()
        Gui.Selection.clearSelection()
        self.refresh()
        self.say(translate("PartElements", "%s replaced") % element.Label)

    def remove(self):
        names = [e.Name for e in self.selectedElements()]
        for name in names:
            if self.doc.getObject(name) is not None:
                self.doc.removeObject(name)
        self.doc.recompute()
        self.refresh()
        self.say(translate("PartElements", "%d element(s) removed") % len(names))

    def onItemChanged(self, item, column):
        if column != 0:
            return
        element = self.doc.getObject(item.data(0, QtCore.Qt.UserRole))
        if element is None:
            return
        label = item.text(0).strip()
        if label and label != element.Label:
            element.Label = label
        # The label may have been made unique among the siblings
        QtCore.QTimer.singleShot(0, self.refresh)

    def onListSelection(self):
        elements = self.selectedElements()
        # Shown on top while preselected, also when auto visibility hides it
        if len(elements) == 1 and self.container.Elements is not None:
            Gui.Selection.setPreselection(
                self.container, self.container.Elements.Name + "." + elements[0].Name + "."
            )
        self.updateButtons()

    # ------------------------------------------------------------ observer

    def addSelection(self, doc, obj, sub, pnt):
        self.updateButtons()

    def removeSelection(self, doc, obj, sub):
        self.updateButtons()

    def setSelection(self, doc):
        self.updateButtons()

    def clearSelection(self, doc):
        self.updateButtons()

    # ------------------------------------------------------------ task

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel

    def close(self):
        Gui.Selection.removeObserver(self)
        Gui.Selection.clearPreselection()

    def accept(self):
        self.close()
        if self.transaction:
            self.doc.commitTransaction()
        self.doc.recompute()
        Gui.Control.closeDialog()
        return True

    def reject(self):
        self.close()
        if self.transaction:
            self.doc.abortTransaction()
        Gui.Control.closeDialog()
        return True


def edit(container):
    """Open the panel for a container, an Elements group or an element"""
    owner = getContainer(container)
    if owner is None:
        raise ValueError("%s declares no elements" % container.Label)
    if Gui.Control.activeDialog():
        Gui.Control.closeDialog()
    panel = ElementsTaskPanel(owner)
    Gui.Control.showDialog(panel)
    return panel
