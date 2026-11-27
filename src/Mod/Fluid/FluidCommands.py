# SPDX-License-Identifier: LGPL-2.1-or-later

"""The fluid workbench's commands and its study panel: set up, run, play
back (docs/FluidSimulation.md sec 12, S1)."""

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets
from PySide.QtCore import QT_TRANSLATE_NOOP

import FluidApp
import FluidOpenFoam
import FluidPlayback

translate = App.Qt.translate


def selected():
    return Gui.Selection.getSelection()


class CommandCreateStudy:
    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_CreateStudy.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_CreateStudy", "Create Study"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_CreateStudy",
                "Creates a fluid study. The solids selected are its walls: the parts the "
                "fluid cannot pass.",
            ),
        }

    def IsActive(self):
        return App.ActiveDocument is not None

    def Activated(self):
        doc = App.ActiveDocument
        doc.openTransaction(translate("Fluid", "Create fluid study"))
        study = FluidPlayback.makeStudy(doc, FluidPlayback.solidsOf(selected()))
        doc.commitTransaction()
        doc.recompute()
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(study)


class CommandAddFluid:
    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_AddFluid.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_AddFluid", "Fluid at the Start"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_AddFluid",
                "The solids selected are full of fluid when the study's run starts. "
                "Select the solids, and the study when the document has several.",
            ),
        }

    def IsActive(self):
        return (
            FluidPlayback.activeStudy(selected()) is not None
            and len(FluidPlayback.solidsOf(selected())) > 0
        )

    def Activated(self):
        study = FluidPlayback.activeStudy(selected())
        doc = study.Document
        doc.openTransaction(translate("Fluid", "Add fluid"))
        for volume in FluidPlayback.solidsOf(selected()):
            FluidPlayback.addFluid(study, volume)
            # the frames are drawn in its place; as a body of water it is
            # seen through
            if hasattr(volume.ViewObject, "Transparency"):
                volume.ViewObject.Transparency = 70
        doc.commitTransaction()
        doc.recompute()


def densityOf(part):
    """The density of the part's material, kg/m^3, or None when it has
    none: the card every shape has until one is chosen gives a density of
    1 that is no material's."""
    try:
        material = part.getLinkedObject(True).ShapeMaterial
        if material.Name in ("Default", ""):
            return None
        density = App.Units.Quantity(material.PhysicalProperties["Density"])
        value = density.getValueAs("kg/m^3").Value
        return value if value > 0.0 else None
    except Exception:
        return None


class CommandAddBody:
    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_AddBody.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_AddBody", "Part Moved by the Fluid"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_AddBody",
                "The solids selected are moved by the fluid: walls to it, held by "
                "nothing, each under the fluid's push and its own weight. A part's "
                "density is its material's, or is asked for. Select the solids, and "
                "the study when the document has several.",
            ),
        }

    def IsActive(self):
        return (
            FluidPlayback.activeStudy(selected()) is not None
            and len(FluidPlayback.solidsOf(selected())) > 0
        )

    def Activated(self):
        study = FluidPlayback.activeStudy(selected())
        doc = study.Document
        taken = [child.Part for child in study.Group if child.isDerivedFrom("Fluid::FreeBody")]
        asked = []
        for part in FluidPlayback.solidsOf(selected()):
            if part in taken:
                continue
            density = densityOf(part)
            if density is None:
                density, ok = QtWidgets.QInputDialog.getDouble(
                    Gui.getMainWindow(),
                    translate("Fluid", "Part Moved by the Fluid"),
                    translate(
                        "Fluid", "{} has no material that gives a density. Its density, kg/m^3:"
                    ).format(part.Label),
                    500.0,
                    1.0e-3,
                    1.0e6,
                    3,
                )
                if not ok:
                    continue
            asked.append((part, density))
        if not asked:
            return
        doc.openTransaction(translate("Fluid", "Add part moved by the fluid"))
        for part, density in asked:
            # held or moved, not both
            if part in study.Walls:
                study.Walls = [wall for wall in study.Walls if wall != part]
            FluidPlayback.addBody(study, part, density)
        doc.commitTransaction()
        doc.recompute()


def facesPicked():
    """The faces selected, each with the object it is of."""
    faces = []
    for picked in Gui.Selection.getSelectionEx():
        for name in picked.SubElementNames:
            if name.startswith("Face"):
                faces.append((picked.Object, name))
    return faces


class CommandAddInlet:
    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_AddInlet.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_AddInlet", "Inlet"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_AddInlet",
                "The fluid comes in through the flat faces selected, square to each "
                "and out of its solid, at a speed that is asked for. Reversed, "
                "StartTime and EndTime can be set afterwards.",
            ),
        }

    def IsActive(self):
        return FluidPlayback.activeStudy(selected()) is not None and len(facesPicked()) > 0

    def Activated(self):
        study = FluidPlayback.activeStudy(selected())
        faces = facesPicked()
        speed, ok = QtWidgets.QInputDialog.getDouble(
            Gui.getMainWindow(),
            translate("Fluid", "Inlet"),
            translate("Fluid", "The fluid's speed through the face, m/s:"),
            1.0,
            1.0e-6,
            1.0e4,
            3,
        )
        if not ok:
            return
        doc = study.Document
        doc.openTransaction(translate("Fluid", "Add inlet"))
        for part, face in faces:
            FluidPlayback.addInlet(study, part, face, speed * 1000.0)
        doc.commitTransaction()
        doc.recompute()


class CommandAddOutlet:
    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_AddOutlet.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_AddOutlet", "Outlet"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_AddOutlet",
                "A steady flow leaves through the flat faces selected, each the face "
                "of a lid on the fluid's side, held at the surroundings' pressure. "
                "Pressure can be set afterwards. The study becomes one of a steady flow.",
            ),
        }

    def IsActive(self):
        return FluidPlayback.activeStudy(selected()) is not None and len(facesPicked()) > 0

    def Activated(self):
        study = FluidPlayback.activeStudy(selected())
        doc = study.Document
        doc.openTransaction(translate("Fluid", "Add outlet"))
        if study.Analysis != "Steady flow":
            study.Analysis = "Steady flow"
            App.Console.PrintMessage(
                translate("Fluid", "{} is now a study of a steady flow.").format(study.Label) + "\n"
            )
        for part, face in facesPicked():
            FluidPlayback.addOutlet(study, part, face)
        doc.commitTransaction()
        doc.recompute()


class CommandAddForceMonitor:
    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_AddForceMonitor.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_AddForceMonitor", "Force Monitor"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_AddForceMonitor",
                "Reports at each frame the fluid's force on the walls selected, along "
                "a direction: the way the study's first inlet sends its fluid, or "
                "against gravity. Its Direction can be set afterwards.",
            ),
        }

    def walls(self):
        study = FluidPlayback.activeStudy(selected())
        if study is None:
            return None, []
        return study, [part for part in selected() if part in study.Walls]

    def IsActive(self):
        return len(self.walls()[1]) > 0

    def Activated(self):
        study, walls = self.walls()
        doc = study.Document
        doc.openTransaction(translate("Fluid", "Add force monitor"))
        along = FluidPlayback.forceDirection(study)
        for part in walls:
            FluidPlayback.addForceMonitor(study, part, along)
        doc.commitTransaction()
        doc.recompute()


class CommandAddMonitor:
    kind = "Height"

    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_AddMonitor.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_AddMonitor", "Height Monitor"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_AddMonitor",
                "Reports at each frame how high the fluid stands above a point, against "
                "gravity. The point is where a face, an edge or a vertex was picked; its "
                "Point can be set afterwards.",
            ),
        }

    def IsActive(self):
        return FluidPlayback.activeStudy(selected()) is not None

    def Activated(self):
        study = FluidPlayback.activeStudy(selected())
        point = App.Vector()
        for picked in Gui.Selection.getSelectionEx():
            if picked.PickedPoints:
                point = picked.PickedPoints[0]
                break
        doc = study.Document
        doc.openTransaction(translate("Fluid", "Add monitor"))
        FluidPlayback.addMonitor(study, self.kind, point)
        doc.commitTransaction()
        doc.recompute()


class CommandAddPressureMonitor(CommandAddMonitor):
    kind = "Pressure"

    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_AddPressureMonitor.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_AddPressureMonitor", "Pressure Monitor"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_AddPressureMonitor",
                "Reports at each frame the fluid's pressure at a point. The point is "
                "where a face, an edge or a vertex was picked; its Point can be set "
                "afterwards.",
            ),
        }


class CommandMechanism:
    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_Mechanism.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_Mechanism", "Run with a Mechanism"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_Mechanism",
                "Runs the study together with an Assembly's dynamic simulation: the "
                "parts the fluid moves are then the assembly's, moved with its joints "
                "under the fluid's push. Select the simulation, or its assembly, when "
                "the document has several. The simulation ends with the study.",
            ),
        }

    def IsActive(self):
        study = FluidPlayback.activeStudy(selected())
        return study is not None and len(FluidPlayback.mechanismsOf(study.Document)) > 0

    def chosen(self, study):
        """The simulation selected, or the one of the assembly selected, or
        the document's only one; else the user picks one."""
        mechanisms = FluidPlayback.mechanismsOf(study.Document)
        for obj in selected():
            if obj in mechanisms:
                return obj
        for obj in selected():
            if obj.isDerivedFrom("Assembly::AssemblyObject"):
                own = [sim for sim in mechanisms if obj in sim.InListRecursive]
                dynamic = [sim for sim in own if sim.AnalysisType == "Dynamic"]
                if len(dynamic or own) == 1:
                    return (dynamic or own)[0]
                if own:
                    mechanisms = own
                break
        if len(mechanisms) == 1:
            return mechanisms[0]
        labels = [sim.Label for sim in mechanisms]
        label, ok = QtWidgets.QInputDialog.getItem(
            Gui.getMainWindow(),
            translate("Fluid", "Run with a Mechanism"),
            translate("Fluid", "The Assembly simulation to run with:"),
            labels,
            0,
            False,
        )
        return mechanisms[labels.index(label)] if ok else None

    def Activated(self):
        study = FluidPlayback.activeStudy(selected())
        simulation = self.chosen(study)
        if simulation is None:
            return
        doc = study.Document
        doc.openTransaction(translate("Fluid", "Run with a mechanism"))
        if simulation.AnalysisType != "Dynamic":
            asked = QtWidgets.QMessageBox.question(
                Gui.getMainWindow(),
                translate("Fluid", "Run with a Mechanism"),
                translate(
                    "Fluid",
                    "{} is a {} simulation: its parts move by the joints' motions "
                    "alone, and the fluid's push would move nothing. Make it dynamic?",
                ).format(simulation.Label, simulation.AnalysisType),
            )
            if asked != QtWidgets.QMessageBox.Yes:
                doc.abortTransaction()
                return
            simulation.AnalysisType = "Dynamic"
        FluidPlayback.linkMechanism(study, simulation)
        doc.commitTransaction()
        doc.recompute()


class CommandRunStudy:
    def GetResources(self):
        return {
            "Pixmap": FluidPlayback.iconPath("Fluid_RunStudy.svg"),
            "MenuText": QT_TRANSLATE_NOOP("Fluid_RunStudy", "Run and Play Back"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Fluid_RunStudy",
                "Opens the study's panel: choose the solver, run the study, and play "
                "its kept frames back in the 3D view.",
            ),
        }

    def IsActive(self):
        return FluidPlayback.activeStudy(selected()) is not None and not Gui.Control.activeDialog()

    def Activated(self):
        Gui.Control.showDialog(TaskFluidStudy(FluidPlayback.activeStudy(selected())))


def rampStyle():
    """The colours a field is drawn in, from the least at the left to the
    most at the right, as a widget's background."""
    ramp = FluidPlayback.RAMP
    stops = ", ".join(
        "stop:{:.3f} rgb({:.0f}, {:.0f}, {:.0f})".format(
            k / (len(ramp) - 1), 255 * r, 255 * g, 255 * b
        )
        for k, (r, g, b) in enumerate(ramp)
    )
    return (
        "QFrame#FluidLegend {{ border: 1px solid #555; background: "
        "qlineargradient(x1:0, y1:0, x2:1, y2:0, {}); }}".format(stops)
    )


def backendLabel(backend):
    """A backend as the panel lists it."""
    label = backend["Label"]
    if not backend["Available"]:
        label += translate("Fluid", " -- not found")
    return label


class TaskFluidStudy:
    """The study's panel: the solver and the run's settings, the run with
    its progress, and the kept frames played back."""

    def __init__(self, study):
        self.study = study
        self.frames = None
        self.view = FluidPlayback.FrameView()
        self.moved = None
        self.hidden = []
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Fluid", "Fluid Study"))
        layout = QtWidgets.QVBoxLayout(self.form)

        settings = QtWidgets.QFormLayout()
        self.settings = settings
        self.analysis = QtWidgets.QComboBox()
        self.analysis.setToolTip(
            translate(
                "Fluid",
                "A free surface: a liquid under the air, from how it stands at the "
                "start, through time. A steady flow: one fluid filling the closed "
                "space inside the walls, in at its inlets and out at its outlets, as "
                "it settles to.",
            )
        )
        self.analysis.addItem(translate("Fluid", "Free surface"), "Free surface")
        self.analysis.addItem(translate("Fluid", "Steady flow"), "Steady flow")
        self.analysis.setCurrentIndex(max(self.analysis.findData(study.Analysis), 0))
        self.analysis.currentIndexChanged.connect(self.showKind)
        settings.addRow(translate("Fluid", "Study of"), self.analysis)
        self.backend = QtWidgets.QComboBox()
        self.fillBackends(study.Backend)
        settings.addRow(translate("Fluid", "Solver"), self.backend)
        self.missing = QtWidgets.QLabel("")
        self.missing.setWordWrap(True)
        # a folder's path has nowhere to break: the label keeps to the
        # panel's width and the path is cut, the whole of it selectable
        self.missing.setSizePolicy(QtWidgets.QSizePolicy.Ignored, QtWidgets.QSizePolicy.Preferred)
        self.missing.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self.download = QtWidgets.QPushButton(translate("Fluid", "Download OpenFOAM..."))
        self.download.clicked.connect(self.onDownload)
        settings.addRow(self.missing)
        settings.addRow(self.download)
        self.backend.currentIndexChanged.connect(self.showMissing)
        self.resolution = QtWidgets.QDoubleSpinBox()
        self.resolution.setRange(0.0, 1.0e6)
        self.resolution.setDecimals(3)
        self.resolution.setSuffix(" mm")
        self.resolution.setSpecialValueText(translate("Fluid", "Automatic"))
        self.resolution.setToolTip(
            translate(
                "Fluid",
                "The size of what is to be resolved: the spacing of particles, or the "
                "base size of cells. Automatic: a twentieth of the fluid's smallest side.",
            )
        )
        self.resolution.setValue(study.Resolution.Value)
        settings.addRow(translate("Fluid", "Resolution"), self.resolution)
        self.timeEnd = QtWidgets.QDoubleSpinBox()
        self.timeEnd.setRange(1.0e-6, 1.0e6)
        self.timeEnd.setDecimals(4)
        self.timeEnd.setSuffix(" s")
        self.timeEnd.setValue(study.TimeEnd.Value)
        settings.addRow(translate("Fluid", "End time"), self.timeEnd)
        self.interval = QtWidgets.QDoubleSpinBox()
        self.interval.setRange(1.0e-6, 1.0e6)
        self.interval.setDecimals(4)
        self.interval.setSuffix(" s")
        self.interval.setValue(study.OutputInterval.Value)
        settings.addRow(translate("Fluid", "Frame every"), self.interval)
        self.iterations = QtWidgets.QSpinBox()
        self.iterations.setRange(1, 1000000)
        self.iterations.setSingleStep(500)
        self.iterations.setToolTip(
            translate(
                "Fluid",
                "How many steps a steady flow is given to settle in, at most. It "
                "stops sooner when it has settled.",
            )
        )
        self.iterations.setValue(study.Iterations)
        settings.addRow(translate("Fluid", "Steps at most"), self.iterations)
        self.mechanism = QtWidgets.QComboBox()
        self.mechanism.setToolTip(
            translate(
                "Fluid",
                "An Assembly's dynamic simulation the study runs together with: the "
                "parts the fluid moves are then that mechanism's, moved with its joints. "
                "It ends with the study and keeps its frames on the same times.",
            )
        )
        self.mechanism.addItem(translate("Fluid", "None"), None)
        for simulation in FluidPlayback.mechanismsOf(study.Document):
            self.mechanism.addItem(simulation.Label, simulation.Name)
        self.mechanism.setCurrentIndex(
            max(self.mechanism.findData(study.Mechanism.Name if study.Mechanism else None), 0)
        )
        settings.addRow(translate("Fluid", "Mechanism"), self.mechanism)
        layout.addLayout(settings)

        row = QtWidgets.QHBoxLayout()
        self.runButton = QtWidgets.QPushButton(translate("Fluid", "Run"))
        self.runButton.clicked.connect(self.onRun)
        row.addWidget(self.runButton)
        self.progress = QtWidgets.QLabel("")
        row.addWidget(self.progress, 1)
        layout.addLayout(row)

        self.summary = QtWidgets.QLabel("")
        self.summary.setWordWrap(True)
        layout.addWidget(self.summary)
        # what the run's own account has against its results
        self.doubt = QtWidgets.QLabel("")
        self.doubt.setObjectName("FluidDoubt")
        self.doubt.setWordWrap(True)
        self.doubt.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self.doubt.setStyleSheet("color: #c05000;")
        # wrapped to the panel as it is, never widening it
        self.doubt.setSizePolicy(QtWidgets.QSizePolicy.Ignored, QtWidgets.QSizePolicy.Preferred)
        self.doubt.setVisible(False)
        layout.addWidget(self.doubt)
        row = QtWidgets.QHBoxLayout()
        self.playButton = QtWidgets.QPushButton(translate("Fluid", "Play"))
        self.playButton.clicked.connect(self.onPlay)
        row.addWidget(self.playButton)
        self.slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
        self.slider.valueChanged.connect(self.onFrame)
        row.addWidget(self.slider, 1)
        self.time = QtWidgets.QLabel("")
        self.time.setMinimumWidth(70)
        row.addWidget(self.time)
        layout.addLayout(row)
        # a field's scale: the ramp, and the values at its ends
        self.legend = QtWidgets.QWidget()
        scale = QtWidgets.QHBoxLayout(self.legend)
        scale.setContentsMargins(0, 0, 0, 0)
        self.legendLow = QtWidgets.QLabel("")
        self.legendBar = QtWidgets.QFrame()
        self.legendBar.setObjectName("FluidLegend")
        self.legendBar.setFixedHeight(14)
        self.legendBar.setStyleSheet(rampStyle())
        self.legendHigh = QtWidgets.QLabel("")
        scale.addWidget(self.legendLow)
        scale.addWidget(self.legendBar, 1)
        scale.addWidget(self.legendHigh)
        self.legend.setVisible(False)
        layout.addWidget(self.legend)
        self.range = (0.0, 0.0)
        self.asSurface = QtWidgets.QCheckBox(translate("Fluid", "Particles as a surface"))
        self.asSurface.setToolTip(
            translate(
                "Fluid",
                "Draws a frame of particles as the closed surface around them, a body "
                "of water to a renderer that draws one. Off: the particles as points.",
            )
        )
        self.asSurface.setChecked(
            App.ParamGet("User parameter:BaseApp/Preferences/Mod/Fluid").GetBool(
                "ParticlesAsSurface", True
            )
        )
        self.asSurface.toggled.connect(self.onAsSurface)
        layout.addWidget(self.asSurface)
        self.table = QtWidgets.QTableWidget(0, 2)
        self.table.setHorizontalHeaderLabels(
            [translate("Fluid", "Monitor"), translate("Fluid", "At this frame")]
        )
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.verticalHeader().hide()
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        layout.addWidget(self.table)

        self.runPoll = QtCore.QTimer(self.form)
        self.runPoll.setInterval(100)
        self.runPoll.timeout.connect(self.pollRun)
        self.playTimer = QtCore.QTimer(self.form)
        self.playTimer.timeout.connect(self.playStep)
        self.showMissing()
        self.showKind()
        self.loadResults()

    # ----------------------------- the solver -----------------------------

    def steady(self):
        return self.analysis.currentData() == "Steady flow"

    def showKind(self, *_args):
        """The settings of the kind of study chosen, and no others."""
        steady = self.steady()
        self.settings.setRowVisible(self.timeEnd, not steady)
        self.settings.setRowVisible(self.interval, not steady)
        self.settings.setRowVisible(self.mechanism, not steady and self.mechanism.count() > 1)
        self.settings.setRowVisible(self.iterations, steady)

    def fillBackends(self, chosen):
        """The solvers as they are on this machine now, that one chosen."""
        self.backends = FluidApp.getBackends()
        self.backend.blockSignals(True)
        self.backend.clear()
        self.backend.addItem(translate("Fluid", "The first that can run here"), "")
        for backend in self.backends:
            self.backend.addItem(backendLabel(backend), backend["Name"])
            if not backend["Available"]:
                self.backend.setItemData(
                    self.backend.count() - 1, backend["Reason"], QtCore.Qt.ToolTipRole
                )
        self.backend.setCurrentIndex(max(self.backend.findData(chosen), 0))
        self.backend.blockSignals(False)

    def missingBackend(self):
        """The backend chosen that cannot run here -- or, with none chosen,
        the first real one when no real one can; None when the choice runs."""
        name = self.backend.currentData() or ""
        real = [b for b in self.backends if not b["TestData"]]
        if name:
            for backend in self.backends:
                if backend["Name"] == name and not backend["Available"]:
                    return backend
            return None
        if real and not any(b["Available"] for b in real):
            return real[0]
        return None

    def showMissing(self, *_args):
        missing = self.missingBackend()
        self.missing.setVisible(missing is not None)
        self.missing.setText(missing["Reason"] if missing else "")
        offered = missing is not None and missing["Name"] == "OpenFOAM"
        self.download.setVisible(offered)

    def onDownload(self):
        dialog = DownloadOpenFoam(self.form)
        dialog.exec_()
        FluidApp.refreshBackends()
        self.fillBackends(self.backend.currentData() or "")
        self.showMissing()

    # ------------------------------ the run ------------------------------

    def applySettings(self):
        study = self.study
        study.Document.openTransaction(translate("Fluid", "Fluid study settings"))
        if study.Analysis != self.analysis.currentData():
            study.Analysis = self.analysis.currentData()
        study.Iterations = self.iterations.value()
        study.Backend = self.backend.currentData() or ""
        study.Resolution = self.resolution.value()
        study.TimeEnd = self.timeEnd.value()
        study.OutputInterval = self.interval.value()
        name = self.mechanism.currentData()
        chosen = study.Document.getObject(name) if name else None
        if chosen is not None or study.Mechanism is not None:
            FluidPlayback.linkMechanism(study, chosen)
        study.Document.commitTransaction()

    def running(self):
        return self.study.getProgress()["Running"]

    def onRun(self):
        if self.running():
            self.study.cancel()
            self.progress.setText(translate("Fluid", "Stopping..."))
            return
        self.stopPlaying()
        self.putBack()
        self.applySettings()
        try:
            started = self.study.start()
        except Exception as error:
            self.progress.setText(str(error))
            return
        if started != 0:
            self.progress.setText(self.study.Status)
            return
        self.runButton.setText(translate("Fluid", "Cancel"))
        self.progress.setText(translate("Fluid", "Starting..."))
        self.runPoll.start()

    def pollRun(self):
        progress = self.study.getProgress()
        if progress["Frames"] and self.steady():
            self.progress.setText(
                translate("Fluid", "{} frames, step {:.0f} of {} at most").format(
                    progress["Frames"], progress["Time"], self.study.Iterations
                )
            )
        elif progress["Frames"]:
            self.progress.setText(
                translate("Fluid", "{} frames, {:.4g} s of {:.4g}").format(
                    progress["Frames"], progress["Time"], self.study.TimeEnd.Value
                )
            )
        if progress["Done"]:
            self.finishRun()

    def finishRun(self):
        """The run is through: its results kept, and shown from the start."""
        self.runPoll.stop()
        self.study.finish()
        self.runButton.setText(translate("Fluid", "Run"))
        self.progress.setText(self.study.Status)
        self.loadResults()

    # ----------------------------- playback ------------------------------

    def putBack(self):
        """The parts the frames moved, where they stood: a run and the
        question whether the results are still the study's take the parts
        where they are."""
        if self.moved is not None:
            self.moved.restore()
            self.moved = None

    def loadResults(self):
        self.putBack()
        try:
            self.frames = FluidPlayback.Frames(self.study)
            stale = self.frames.valid and self.study.isStale()
        except Exception as error:
            self.frames = None
            self.summary.setText(str(error))
            self.doubt.setVisible(False)
            return
        valid = self.frames.valid
        found = FluidPlayback.doubts(self.study.getResults()) if valid else []
        self.doubt.setText("\n\n".join(found))
        self.doubt.setVisible(bool(found))
        for sentence in found:
            App.Console.PrintWarning("{}: {}\n".format(self.study.Label, sentence))
        self.playButton.setEnabled(valid)
        self.slider.setEnabled(valid)
        self.asSurface.setVisible(valid and self.frames.kind == "Particles")
        self.legend.setVisible(valid and self.frames.kind == "Fields")
        if not valid:
            self.summary.setText(translate("Fluid", "No results kept. Run the study."))
            self.table.setRowCount(0)
            self.time.setText("")
            return
        text = translate("Fluid", "{} frames to {:.4g} s, from {}").format(
            len(self.frames), self.frames.times[-1], self.frames.backend
        )
        if self.frames.steady:
            text = translate("Fluid", "{} frames to step {:.0f}, from {}").format(
                len(self.frames), self.frames.times[-1], self.frames.backend
            )
        if self.frames.kind == "Fields":
            # one scale for every frame: the last one's, the flow as it settled
            self.range = FluidPlayback.rangeOf(self.frames.field(len(self.frames) - 1)[2])
            self.legendLow.setText("{:.4g} Pa".format(self.range[0]))
            self.legendHigh.setText("{:.4g} Pa".format(self.range[1]))
        if stale:
            text += translate("Fluid", " -- of the study as it was before a change")
        self.summary.setText(text)
        self.slider.blockSignals(True)
        self.slider.setRange(0, len(self.frames) - 1)
        # a steady flow is shown as it settled, a run through time from
        # its start
        first = len(self.frames) - 1 if self.frames.steady else 0
        self.slider.setValue(first)
        self.slider.blockSignals(False)
        self.onFrame(first)

    def hideVolumes(self):
        """The solids the fluid starts in give way to the frames; and to a
        field the walls, which is drawn on their faces from the inside."""
        if self.hidden:
            return
        given = [getattr(child, "Volume", None) for child in self.study.Group]
        if self.frames is not None and self.frames.kind == "Fields":
            given += list(self.study.Walls)
        for volume in given:
            if volume is not None and volume.ViewObject and volume.ViewObject.Visibility:
                volume.ViewObject.Visibility = False
                self.hidden.append(volume)

    def onFrame(self, index):
        if self.frames is None or not self.frames.valid:
            return
        if self.frames.kind == "Fields":
            drawn = self.view.showField(*self.frames.field(index), *self.range)
        elif self.frames.kind == "Particles" and self.asSurface.isChecked():
            drawn = self.view.show(*self.frames.surface(index))
        else:
            drawn = self.view.show(*self.frames.frame(index))
        if drawn:
            self.hideVolumes()
        if self.moved is None:
            self.moved = FluidPlayback.MovedParts(self.frames)
        self.moved.show(index)
        if self.frames.steady:
            self.time.setText(translate("Fluid", "step {:.0f}").format(self.frames.times[index]))
        else:
            self.time.setText("{:.4g} s".format(self.frames.times[index]))
        rows = [
            (label, "{:.6g} {}".format(value, unit))
            for label, _kind, value, unit in self.frames.monitorsAt(index)
        ]
        # an opening: its mean pressure, and what goes through it
        rows += [
            (label, "{:.6g} Pa, {}".format(pressure, FluidPlayback.flowText(flow)))
            for label, _kind, pressure, flow in self.frames.openingsAt(index)
        ]
        self.table.setRowCount(len(rows))
        for row, (label, text) in enumerate(rows):
            self.table.setItem(row, 0, QtWidgets.QTableWidgetItem(label))
            self.table.setItem(row, 1, QtWidgets.QTableWidgetItem(text))

    def onAsSurface(self, on):
        App.ParamGet("User parameter:BaseApp/Preferences/Mod/Fluid").SetBool(
            "ParticlesAsSurface", on
        )
        if self.frames is not None and self.frames.valid:
            self.onFrame(self.slider.value())

    def onPlay(self):
        if self.playTimer.isActive():
            self.stopPlaying()
            return
        if self.slider.value() >= self.slider.maximum():
            self.slider.setValue(0)
        # a frame each output interval, as slow as the run was
        step = self.frames.times[1] - self.frames.times[0] if len(self.frames) > 1 else 0.0
        if self.frames.steady:
            # steps, not seconds: five frames a second
            step = 0.2
        self.playTimer.setInterval(max(int(step * 1000.0), 15))
        self.playTimer.start()
        self.playButton.setText(translate("Fluid", "Pause"))

    def playStep(self):
        if self.slider.value() >= self.slider.maximum():
            self.stopPlaying()
            return
        self.slider.setValue(self.slider.value() + 1)

    def stopPlaying(self):
        self.playTimer.stop()
        self.playButton.setText(translate("Fluid", "Play"))

    # ------------------------------ the task ------------------------------

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Close

    def close(self):
        self.stopPlaying()
        self.runPoll.stop()
        if self.running():
            self.study.cancel()
            self.study.finish()
        self.view.remove()
        self.putBack()
        for volume in self.hidden:
            try:
                volume.ViewObject.Visibility = True
            except Exception:
                pass
        self.hidden = []
        Gui.Control.closeDialog()

    def accept(self):
        self.close()
        return True

    def reject(self):
        self.close()
        return True


class DownloadOpenFoam(QtWidgets.QDialog):
    """Says what the download is and makes it when the user starts it,
    showing what the installer prints (FluidOpenFoam.py)."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle(translate("Fluid", "Download OpenFOAM"))
        self.resize(640, 460)
        layout = QtWidgets.QVBoxLayout(self)
        text = QtWidgets.QLabel(FluidOpenFoam.explanation())
        text.setWordWrap(True)
        text.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(text)
        self.log = QtWidgets.QPlainTextEdit()
        self.log.setReadOnly(True)
        layout.addWidget(self.log, 1)
        self.state = QtWidgets.QLabel("")
        self.state.setWordWrap(True)
        layout.addWidget(self.state)
        row = QtWidgets.QHBoxLayout()
        row.addStretch(1)
        self.startButton = QtWidgets.QPushButton(translate("Fluid", "Download"))
        self.startButton.clicked.connect(self.start)
        row.addWidget(self.startButton)
        self.closeButton = QtWidgets.QPushButton(translate("Fluid", "Close"))
        self.closeButton.clicked.connect(self.reject)
        row.addWidget(self.closeButton)
        layout.addLayout(row)
        self.process = None
        self.succeeded = False
        can, why = FluidOpenFoam.supported()
        if not can:
            self.startButton.setEnabled(False)
            self.state.setText(why)

    def start(self):
        self.startButton.setEnabled(False)
        self.closeButton.setText(translate("Fluid", "Cancel"))
        self.state.setText(translate("Fluid", "Downloading..."))
        self.process = QtCore.QProcess(self)
        self.process.setProcessChannelMode(QtCore.QProcess.MergedChannels)
        self.process.readyReadStandardOutput.connect(self.onOutput)
        self.process.finished.connect(self.onFinished)
        script = FluidOpenFoam.installScript(tool=FluidOpenFoam.condaTool())
        self.process.start("bash", ["-c", script])

    def onOutput(self):
        text = bytes(self.process.readAllStandardOutput()).decode("utf-8", "replace")
        if "DONE" in text.splitlines():
            self.succeeded = True
        # the installer redraws its progress with carriage returns
        for line in text.replace("\r", "\n").splitlines():
            if line.strip():
                self.log.appendPlainText(line.rstrip())

    def onFinished(self, *_args):
        self.onOutput()
        self.closeButton.setText(translate("Fluid", "Close"))
        if self.succeeded:
            if FluidOpenFoam.fixParallel():
                self.log.appendPlainText(
                    translate("Fluid", "Mended the package's parallel layer (lib/sys-mpich).")
                )
            FluidApp.refreshBackends()
            found = [b for b in FluidApp.getBackends() if b["Name"] == "OpenFOAM"]
            if found and found[0]["Available"]:
                self.state.setText(
                    translate("Fluid", "OpenFOAM {} is installed and found.").format(
                        found[0]["Version"]
                    )
                )
            else:
                self.state.setText(
                    translate("Fluid", "Installed, but not found: {}").format(
                        found[0]["Reason"] if found else ""
                    )
                )
        else:
            self.state.setText(
                translate("Fluid", "The download did not finish. What it printed is above.")
            )
            self.startButton.setEnabled(True)
        self.process = None

    def reject(self):
        if self.process is not None:
            self.process.finished.disconnect(self.onFinished)
            self.process.kill()
            self.process.waitForFinished(3000)
            self.process = None
        super().reject()


Gui.addCommand("Fluid_CreateStudy", CommandCreateStudy())
Gui.addCommand("Fluid_AddFluid", CommandAddFluid())
Gui.addCommand("Fluid_AddBody", CommandAddBody())
Gui.addCommand("Fluid_AddInlet", CommandAddInlet())
Gui.addCommand("Fluid_AddOutlet", CommandAddOutlet())
Gui.addCommand("Fluid_AddForceMonitor", CommandAddForceMonitor())
Gui.addCommand("Fluid_AddMonitor", CommandAddMonitor())
Gui.addCommand("Fluid_AddPressureMonitor", CommandAddPressureMonitor())
Gui.addCommand("Fluid_Mechanism", CommandMechanism())
Gui.addCommand("Fluid_RunStudy", CommandRunStudy())
