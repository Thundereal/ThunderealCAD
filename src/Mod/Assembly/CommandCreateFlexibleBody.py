# SPDX-License-Identifier: LGPL-2.1-or-later
"""The command making a component a flexible body (docs/AssemblyDynamics.md,
D4b): an Assembly::FlexibleBody of the component selected, used by every
Dynamic study of the assembly; and its task panel, which the command opens
and a double click on a flexible body opens too. Not upstream."""

import FreeCAD as App

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtCore, QtWidgets

import UtilsAssembly
import FlexibleBodies

__title__ = "Assembly Command Create Flexible Body"
__url__ = "https://www.freecad.org"

translate = App.Qt.translate
QT_TRANSLATE_NOOP = App.Qt.QT_TRANSLATE_NOOP


def selectedComponent(assembly):
    """The one component of the assembly selected, in the tree or in the 3D
    view, else None"""
    found = []
    for sel in Gui.Selection.getSelectionEx("", 0):
        for sub in sel.SubElementNames or [""]:
            part, _ = UtilsAssembly.getComponentReference(assembly, sel.Object, sub)
            if part is not None and part not in found:
                found.append(part)
    return found[0] if len(found) == 1 else None


def editFlexibleBody(flexible):
    """Open the task panel of a flexible body, as a double click does"""
    if Gui.Control.activeDialog():
        App.Console.PrintWarning(
            translate("Assembly", "Close the open task panel before editing the flexible body.\n")
        )
        return
    Gui.Control.showDialog(TaskFlexibleBody(flexible))


class CommandCreateFlexibleBody:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_FlexibleBody",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateFlexibleBody", "Flexible Body"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateFlexibleBody",
                "A component that bends in a dynamic simulation: meshed, and reduced to its "
                "modes\nheld where its joints hold it. Select a component first.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        if not UtilsAssembly.isAssemblyCommandActive() or Gui.Control.activeDialog():
            return False
        assembly = UtilsAssembly.activeAssembly()
        return assembly is not None and selectedComponent(assembly) is not None

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        component = selectedComponent(assembly) if assembly else None
        if component is None:
            return
        doc = assembly.Document
        doc.openTransaction(translate("Assembly", "Create flexible body"))
        try:
            flexible = FlexibleBodies.newFlexibleBody(assembly, component)
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(flexible)
            Gui.Control.showDialog(TaskFlexibleBody(flexible, creating=True))
        except Exception:
            # The panel's Cancel would have undone it
            doc.abortTransaction()
            raise


# The colours of the interfaces' nodes in the mesh shown: a joint's, a
# force element's, the contact faces'
_HELD = (0.95, 0.55, 0.1)
_CONTACT = (0.15, 0.7, 0.3)


def interfaceName(doc, label):
    """An interface's frame label (InterfaceFrames) as the panel shows it:
    the joint's or the force element's label, its end, or the contact faces"""
    if label == FlexibleBodies.CONTACT:
        return translate("Assembly", "Contact faces")
    end = ""
    name = label
    if label.endswith("-1") or label.endswith("-2"):
        name, end = label[:-2], label[-1]
    obj = None
    if "#" in name:
        docName, objName = name.split("#", 1)
        owner = App.getDocument(docName) if docName in App.listDocuments() else doc
        obj = owner.getObject(objName)
    text = obj.Label if obj is not None else name
    if end:
        text = translate("Assembly", "{}, end {}").format(text, end)
    return text


# The open flexible body panel, as JointObject.activeTask is the joint's
activeTask = None


class FaceGate:
    """Only the faces of the flexible body's component, while its contact
    faces are being picked"""

    def __init__(self, task):
        self.task = task

    def allow(self, doc, obj, sub):
        return self.task.faceOf(obj, sub) is not None


class TaskFlexibleBody(QtCore.QObject):
    """The task panel of a flexible body: its component and material; its
    modes -- how many, the damping, and what they are (Find Modes reduces
    it as the first Dynamic study using it would, and lists the
    frequencies); its mesh -- the size, whether it is made and of what the
    part is now, Mesh Now, and the mesh shown over the part with each
    interface's nodes; and its contact faces, picked on the part.

    The changes are one transaction: Cancel undoes them, and deletes a
    flexible body the command made."""

    def __init__(self, flexible, creating=False):
        super().__init__()
        global activeTask
        activeTask = self
        self.flexible = flexible
        self.creating = creating
        self.doc = flexible.Document
        self.assembly = FlexibleBodies._assemblyOf(flexible)
        # Where Cancel undoes to: the command's transaction is open already
        self.undoBase = self.doc.UndoCount
        self.transaction = (
            translate("Assembly", "Create flexible body")
            if creating
            else translate("Assembly", "Edit flexible body")
        )
        if not creating:
            self.doc.openTransaction(self.transaction)
        self.view = Gui.getDocument(self.doc).activeView()
        self.picking = False
        self.gated = False
        self.meshRoot = None
        # The last Find Modes' frequencies, Hz
        self.frequencies = []
        self.buildForm()
        self.fill()
        Gui.Selection.addObserver(self, Gui.Selection.ResolveMode.NoResolve)

    # ------------------------------------------------------------- the form

    def buildForm(self):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Flexible Body"))
        layout = QtWidgets.QVBoxLayout(self.form)

        partBox = QtWidgets.QGroupBox(translate("Assembly", "Part"))
        form = QtWidgets.QFormLayout(partBox)
        self.componentLabel = QtWidgets.QLabel()
        form.addRow(translate("Assembly", "Component"), self.componentLabel)
        self.materialLabel = QtWidgets.QLabel()
        self.materialLabel.setWordWrap(True)
        form.addRow(translate("Assembly", "Material"), self.materialLabel)
        layout.addWidget(partBox)

        modesBox = QtWidgets.QGroupBox(translate("Assembly", "Modes"))
        form = QtWidgets.QFormLayout(modesBox)
        self.modesBox = QtWidgets.QSpinBox()
        self.modesBox.setRange(1, 200)
        self.modesBox.setToolTip(
            translate(
                "Assembly",
                "How many of its modes it bends in, held where its joints hold it: the\n"
                "lowest, the higher ones costing more of the run's steps",
            )
        )
        self.modesBox.valueChanged.connect(self.onModes)
        form.addRow(translate("Assembly", "Modes"), self.modesBox)
        self.dampingBox = QtWidgets.QDoubleSpinBox()
        self.dampingBox.setRange(0.0, 100.0)
        self.dampingBox.setDecimals(2)
        self.dampingBox.setSingleStep(0.5)
        self.dampingBox.setSuffix(" %")
        self.dampingBox.setToolTip(
            translate(
                "Assembly",
                "Its damping, a ratio of critical at the damping frequency, in proportion\n"
                "to its stiffness: more above it",
            )
        )
        self.dampingBox.valueChanged.connect(self.onDamping)
        form.addRow(translate("Assembly", "Damping"), self.dampingBox)
        self.frequencyBox = Gui.UiLoader().createWidget("Gui::QuantitySpinBox")
        self.frequencyBox.setProperty("value", self.flexible.DampingFrequency)
        Gui.ExpressionBinding(self.frequencyBox).bind(self.flexible, "DampingFrequency")
        self.frequencyBox.setToolTip(
            translate("Assembly", "Where its damping is the ratio given; 0: its first mode")
        )
        self.frequencyBox.valueChanged.connect(self.onFrequency)
        form.addRow(translate("Assembly", "Damping frequency"), self.frequencyBox)
        findRow = QtWidgets.QHBoxLayout()
        self.findButton = QtWidgets.QPushButton(translate("Assembly", "Find Modes"))
        self.findButton.clicked.connect(self.onFindModes)
        findRow.addWidget(self.findButton)
        findRow.addStretch(1)
        form.addRow(findRow)
        self.modesLabel = QtWidgets.QLabel()
        self.modesLabel.setWordWrap(True)
        self.modesLabel.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        form.addRow(self.modesLabel)
        layout.addWidget(modesBox)

        meshBox = QtWidgets.QGroupBox(translate("Assembly", "Mesh"))
        form = QtWidgets.QFormLayout(meshBox)
        self.sizeBox = Gui.UiLoader().createWidget("Gui::QuantitySpinBox")
        self.sizeBox.setProperty("value", self.flexible.MeshSize)
        Gui.ExpressionBinding(self.sizeBox).bind(self.flexible, "MeshSize")
        self.sizeBox.setToolTip(
            translate(
                "Assembly",
                "The largest element of its mesh (Netgen, second order); 0: a fortieth of\n"
                "the part's size",
            )
        )
        self.sizeBox.valueChanged.connect(self.onSize)
        form.addRow(translate("Assembly", "Mesh size"), self.sizeBox)
        self.stateLabel = QtWidgets.QLabel()
        self.stateLabel.setWordWrap(True)
        form.addRow(self.stateLabel)
        meshRow = QtWidgets.QHBoxLayout()
        self.meshButton = QtWidgets.QPushButton(translate("Assembly", "Mesh Now"))
        self.meshButton.setToolTip(
            translate(
                "Assembly",
                "Mesh it now, as the next run would: kept while the part, the size, what\n"
                "holds it and its contact faces stay as they are",
            )
        )
        self.meshButton.clicked.connect(self.onMesh)
        meshRow.addWidget(self.meshButton)
        self.showMeshBox = QtWidgets.QCheckBox(translate("Assembly", "Show the mesh"))
        self.showMeshBox.setToolTip(
            translate(
                "Assembly",
                "Its mesh's surface over the part, and the nodes each interface holds:\n"
                "orange a joint's or a force element's, green the contact faces'",
            )
        )
        self.showMeshBox.toggled.connect(self.onShowMesh)
        meshRow.addWidget(self.showMeshBox)
        meshRow.addStretch(1)
        form.addRow(meshRow)
        self.interfacesLabel = QtWidgets.QLabel()
        self.interfacesLabel.setWordWrap(True)
        form.addRow(translate("Assembly", "Held at"), self.interfacesLabel)
        layout.addWidget(meshBox)

        contactBox = QtWidgets.QGroupBox(translate("Assembly", "Contact faces"))
        box = QtWidgets.QVBoxLayout(contactBox)
        self.facesList = QtWidgets.QListWidget()
        self.facesList.setSelectionMode(QtWidgets.QAbstractItemView.ExtendedSelection)
        self.facesList.setToolTip(
            translate(
                "Assembly",
                "The faces of its component a contact touches it on: their nodes move\n"
                "together, as one interface its contacts press it through. A contact on\n"
                "it needs them",
            )
        )
        self.facesList.setMaximumHeight(110)
        box.addWidget(self.facesList)
        buttons = QtWidgets.QHBoxLayout()
        self.addButton = QtWidgets.QPushButton(translate("Assembly", "Add"))
        self.addButton.setCheckable(True)
        self.addButton.setToolTip(
            translate("Assembly", "Pick faces of the part in the 3D view; press again to stop")
        )
        self.addButton.toggled.connect(self.onPick)
        buttons.addWidget(self.addButton)
        self.removeButton = QtWidgets.QPushButton(translate("Assembly", "Remove"))
        self.removeButton.clicked.connect(self.onRemoveFaces)
        buttons.addWidget(self.removeButton)
        self.clearButton = QtWidgets.QPushButton(translate("Assembly", "Clear"))
        self.clearButton.clicked.connect(self.onClearFaces)
        buttons.addWidget(self.clearButton)
        buttons.addStretch(1)
        box.addLayout(buttons)
        self.pickHint = QtWidgets.QLabel()
        self.pickHint.setWordWrap(True)
        box.addWidget(self.pickHint)
        layout.addWidget(contactBox)
        layout.addStretch(1)

    def studies(self):
        """The Dynamic studies that use it, the first the one Find Modes takes"""
        sims = self.assembly.getGroup("Simulations") if self.assembly else None
        return [
            sim
            for sim in (sims.Group if sims else [])
            if getattr(sim, "AnalysisType", None) == "Dynamic"
            and any(e.FullName == self.flexible.FullName for e in getattr(sim, "Elements", []))
        ]

    def fill(self):
        flexible = self.flexible
        component = flexible.Component
        self.componentLabel.setText(component.Label if component else "-")
        self.materialLabel.setText(self.materialText())
        for box, value in (
            (self.modesBox, flexible.Modes),
            (self.dampingBox, 100.0 * flexible.Damping),
        ):
            box.blockSignals(True)
            box.setValue(value)
            box.blockSignals(False)
        self.fillMesh()
        self.fillFaces()
        studies = self.studies()
        self.findButton.setEnabled(bool(studies))
        if studies:
            self.findButton.setToolTip(
                translate(
                    "Assembly",
                    "Reduce it to its modes as the study {} would, meshing it first if need\n"
                    "be, and list their frequencies; the run takes the reduction",
                ).format(studies[0].Label)
            )
        else:
            self.findButton.setToolTip(
                translate("Assembly", "No Dynamic study uses it: tick it in one's Elements")
            )

    def materialText(self):
        component = self.flexible.Component
        if component is None or self.assembly is None:
            return "-"
        try:
            entries = self.assembly.getMassProperties()["Components"]
        except Exception:
            return "-"
        for entry in entries:
            obj = entry.get("Object")
            if obj is None or obj.FullName != component.FullName:
                continue
            name = entry.get("ElasticMaterial") or entry.get("Material") or "?"
            text = translate(
                "Assembly", "{}: E {:.4g} GPa, Poisson's ratio {:.3g}, {:.4g} kg/m^3"
            ).format(
                name,
                entry.get("YoungsModulus", 0.0) * 1e-6,
                entry.get("PoissonRatio", 0.0),
                entry.get("Density", 0.0) * 1e9,
            )
            if entry.get("Origin") == "Assumed":
                text += translate("Assembly", " -- assumed: no material chosen for it")
            return text
        return "-"

    def fillMesh(self):
        flexible = self.flexible
        state = FlexibleBodies.meshState(flexible)
        auto = FlexibleBodies.defaultMeshSize(flexible)
        if flexible.MeshSize.Value <= 0 and auto > 0:
            size = translate("Assembly", "the size {:.3g} mm, a fortieth of the part's").format(
                auto
            )
        else:
            size = ""
        if state == "none":
            text = translate("Assembly", "Not meshed yet: the first run meshes it")
        else:
            text = translate("Assembly", "{} nodes, {} tetrahedra").format(
                len(flexible.Nodes), len(flexible.Tetrahedra) // 10
            )
            if state == "stale":
                text += translate(
                    "Assembly",
                    " -- of the part as it was: the part, the size, what holds it or its\n"
                    "contact faces have changed since, and the next run meshes it again",
                )
        if size:
            text += "\n" + translate("Assembly", "At {}").format(size)
        self.stateLabel.setText(text)
        held = []
        nodes = self.interfaceNodes()
        for label, ids in nodes:
            held.append(
                translate("Assembly", "{}: {} nodes").format(
                    interfaceName(self.doc, label), len(ids)
                )
            )
        self.interfacesLabel.setText(
            "\n".join(held) if held else translate("Assembly", "nothing yet")
        )
        self.showMeshBox.setEnabled(state != "none")
        if self.meshRoot is not None:
            self.showMesh(False)
            self.showMesh(True)

    def interfaceNodes(self):
        """(frame label, node ids) of each interface of the mesh made"""
        result = []
        frames = list(self.flexible.InterfaceFrames)
        current = []
        for node in self.flexible.InterfaceNodes:
            if node < 0:
                if len(result) < len(frames):
                    result.append((frames[len(result)], current))
                current = []
            else:
                current.append(node)
        return result

    def fillFaces(self):
        self.facesList.clear()
        for name in FlexibleBodies.contactFacesOf(self.flexible):
            self.facesList.addItem(name)
        self.removeButton.setEnabled(self.facesList.count() > 0)
        self.clearButton.setEnabled(self.facesList.count() > 0)
        if self.picking:
            self.pickHint.setText(
                translate("Assembly", "Pick faces of {} in the 3D view").format(
                    self.flexible.Component.Label
                )
            )
        else:
            self.pickHint.setText("")

    # ------------------------------------------------------------- editing

    def keepTransaction(self):
        """The panel's transaction open again after what closed it: meshing,
        in a document of its own, closes the application's active one, which
        commits the panel's. Cancel undoes the steps so made (undoBase)"""
        if not self.doc.HasPendingTransaction:
            self.doc.openTransaction(self.transaction)

    def onModes(self, value):
        self.flexible.Modes = value
        self.frequencies = []
        self.modesLabel.setText("")

    def onDamping(self, value):
        self.flexible.Damping = value / 100.0

    def onFrequency(self, quantity):
        self.flexible.DampingFrequency = quantity

    def onSize(self, quantity):
        self.flexible.MeshSize = quantity
        self.fillMesh()

    def busy(self, work):
        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        try:
            return work()
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()

    def onMesh(self):
        try:
            self.busy(lambda: FlexibleBodies.prepare(self.flexible, force=True))
        except Exception as e:
            App.Console.PrintError(
                translate("Assembly", "The flexible body could not be meshed: {}\n").format(e)
            )
        self.keepTransaction()
        self.fillMesh()

    def onFindModes(self):
        studies = self.studies()
        if not studies:
            return

        def find():
            FlexibleBodies.prepare(self.flexible)
            return self.assembly.getFlexibleFrequencies(studies[0], self.flexible)

        try:
            hz = self.busy(find)
        except Exception as e:
            self.keepTransaction()
            self.modesLabel.setText(translate("Assembly", "No modes: {}").format(e))
            self.fillMesh()
            return
        self.keepTransaction()
        self.fillMesh()
        self.frequencies = list(hz)
        self.modesLabel.setText(
            translate("Assembly", "{} modes, Hz: {}").format(
                len(hz), ", ".join("%.4g" % f for f in hz)
            )
        )

    def faceOf(self, obj, sub):
        """The face name of a pick on the flexible body's component, else None"""
        component = self.flexible.Component
        if self.assembly is None or component is None or not sub:
            return None
        part, element = UtilsAssembly.getComponentReference(self.assembly, obj, sub)
        if part is None or part.FullName != component.FullName:
            return None
        name = UtilsAssembly.getElementName(element)
        return name if name.startswith("Face") else None

    def setFaces(self, names):
        component = self.flexible.Component
        self.flexible.ContactFaces = [(component, list(names))] if names else []
        self.fillFaces()
        self.fillMesh()

    def onPick(self, checked):
        self.picking = checked
        if checked and not self.gated:
            Gui.Selection.clearSelection()
            Gui.Selection.addSelectionGate(FaceGate(self), Gui.Selection.ResolveMode.NoResolve)
            self.gated = True
        elif not checked and self.gated:
            Gui.Selection.removeSelectionGate()
            self.gated = False
        self.fillFaces()

    def onRemoveFaces(self):
        gone = {item.text() for item in self.facesList.selectedItems()}
        if not gone and self.facesList.currentItem() is not None:
            gone = {self.facesList.currentItem().text()}
        self.setFaces([n for n in FlexibleBodies.contactFacesOf(self.flexible) if n not in gone])

    def onClearFaces(self):
        self.setFaces([])

    def addSelection(self, doc_name, obj_name, sub_name, mousePos):
        if not self.picking:
            return
        doc = App.getDocument(doc_name)
        obj = doc.getObject(obj_name) if doc else None
        name = self.faceOf(obj, sub_name) if obj is not None else None
        if name is None:
            return
        names = FlexibleBodies.contactFacesOf(self.flexible)
        if name not in names:
            self.setFaces(names + [name])
        QtCore.QTimer.singleShot(0, Gui.Selection.clearSelection)

    # ------------------------------------------------------------- the mesh

    def onShowMesh(self, checked):
        self.showMesh(checked)

    def showMesh(self, on):
        """The mesh's surface edges over the part, in the active view, and the
        interfaces' nodes"""
        from pivy import coin

        if not on:
            if self.meshRoot is not None and self.view is not None:
                self.view.getSceneGraph().removeChild(self.meshRoot)
            self.meshRoot = None
            return
        flexible = self.flexible
        nodes = flexible.Nodes
        component = flexible.Component
        if self.meshRoot is not None or not nodes or component is None or self.view is None:
            return
        tetrahedra = list(flexible.Tetrahedra)
        triangles = FlexibleBodies.surfaceOf(tetrahedra, nodes)
        # The elements' edges: each face's four triangles but for its middle
        # one's, which join two mid-side nodes
        middle = {tetrahedra[i] for i in range(len(tetrahedra)) if i % 10 >= 4}
        edges = set()
        for a, b, c in triangles:
            for i, j in ((a, b), (b, c), (c, a)):
                if i not in middle or j not in middle:
                    edges.add((i, j) if i < j else (j, i))
        root = coin.SoSeparator()
        pick = coin.SoPickStyle()
        pick.style = coin.SoPickStyle.UNPICKABLE
        root.addChild(pick)
        transform = coin.SoTransform()
        at = component.getGlobalPlacement()
        transform.translation = tuple(at.Base)
        transform.rotation = at.Rotation.Q
        root.addChild(transform)
        coords = coin.SoCoordinate3()
        coords.point.setValues(0, len(nodes), [tuple(p) for p in nodes])
        root.addChild(coords)
        lines = coin.SoSeparator()
        material = coin.SoMaterial()
        material.diffuseColor = (0.1, 0.1, 0.1)
        material.emissiveColor = (0.1, 0.1, 0.1)
        lines.addChild(material)
        style = coin.SoDrawStyle()
        style.lineWidth = 1.0
        lines.addChild(style)
        index = []
        for i, j in edges:
            index += [i, j, -1]
        lineSet = coin.SoIndexedLineSet()
        lineSet.coordIndex.setValues(0, len(index), index)
        lines.addChild(lineSet)
        root.addChild(lines)
        for label, ids in self.interfaceNodes():
            if not ids:
                continue
            points = coin.SoSeparator()
            material = coin.SoMaterial()
            color = _CONTACT if label == FlexibleBodies.CONTACT else _HELD
            material.diffuseColor = color
            material.emissiveColor = color
            points.addChild(material)
            style = coin.SoDrawStyle()
            style.pointSize = 5.0
            points.addChild(style)
            held = coin.SoCoordinate3()
            held.point.setValues(0, len(ids), [tuple(nodes[i]) for i in ids if i < len(nodes)])
            points.addChild(held)
            points.addChild(coin.SoPointSet())
            root.addChild(points)
        self.view.getSceneGraph().addChild(root)
        self.meshRoot = root

    # ------------------------------------------------------------- closing

    def deactivate(self):
        global activeTask
        if activeTask is self:
            activeTask = None
        if self.gated:
            Gui.Selection.removeSelectionGate()
            self.gated = False
        Gui.Selection.removeObserver(self)
        self.showMesh(False)

    def accept(self):
        self.deactivate()
        self.doc.commitTransaction()
        Gui.Control.closeDialog()
        return True

    def reject(self):
        self.deactivate()
        self.doc.abortTransaction()
        # And what meshing committed of it (keepTransaction())
        while self.doc.UndoCount > self.undoBase:
            self.doc.undo()
        Gui.Control.closeDialog()
        return True

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel


if App.GuiUp:
    Gui.addCommand("Assembly_CreateFlexibleBody", CommandCreateFlexibleBody())
