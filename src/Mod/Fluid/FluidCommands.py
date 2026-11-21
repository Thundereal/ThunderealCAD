# SPDX-License-Identifier: LGPL-2.1-or-later

"""The fluid workbench's commands and its study panel: set up, run, play
back (docs/FluidSimulation.md sec 12, S1)."""

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets
from PySide.QtCore import QT_TRANSLATE_NOOP

import FluidApp
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
        self.hidden = []
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Fluid", "Fluid Study"))
        layout = QtWidgets.QVBoxLayout(self.form)

        settings = QtWidgets.QFormLayout()
        self.backend = QtWidgets.QComboBox()
        self.backends = FluidApp.getBackends()
        self.backend.addItem(translate("Fluid", "The first that can run here"), "")
        for backend in self.backends:
            self.backend.addItem(backendLabel(backend), backend["Name"])
            if not backend["Available"]:
                self.backend.setItemData(
                    self.backend.count() - 1, backend["Reason"], QtCore.Qt.ToolTipRole
                )
        self.backend.setCurrentIndex(max(self.backend.findData(study.Backend), 0))
        settings.addRow(translate("Fluid", "Solver"), self.backend)
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
        self.loadResults()

    # ------------------------------ the run ------------------------------

    def applySettings(self):
        study = self.study
        study.Document.openTransaction(translate("Fluid", "Fluid study settings"))
        study.Backend = self.backend.currentData() or ""
        study.Resolution = self.resolution.value()
        study.TimeEnd = self.timeEnd.value()
        study.OutputInterval = self.interval.value()
        study.Document.commitTransaction()

    def running(self):
        return self.study.getProgress()["Running"]

    def onRun(self):
        if self.running():
            self.study.cancel()
            self.progress.setText(translate("Fluid", "Stopping..."))
            return
        self.stopPlaying()
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
        if progress["Frames"]:
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

    def loadResults(self):
        try:
            self.frames = FluidPlayback.Frames(self.study)
            stale = self.frames.valid and self.study.isStale()
        except Exception as error:
            self.frames = None
            self.summary.setText(str(error))
            return
        valid = self.frames.valid
        self.playButton.setEnabled(valid)
        self.slider.setEnabled(valid)
        if not valid:
            self.summary.setText(translate("Fluid", "No results kept. Run the study."))
            self.table.setRowCount(0)
            self.time.setText("")
            return
        text = translate("Fluid", "{} frames to {:.4g} s, from {}").format(
            len(self.frames), self.frames.times[-1], self.frames.backend
        )
        if stale:
            text += translate("Fluid", " -- of the study as it was before a change")
        self.summary.setText(text)
        self.slider.blockSignals(True)
        self.slider.setRange(0, len(self.frames) - 1)
        self.slider.setValue(0)
        self.slider.blockSignals(False)
        self.onFrame(0)

    def hideVolumes(self):
        """The solids the fluid starts in give way to the frames."""
        if self.hidden:
            return
        for child in self.study.Group:
            volume = getattr(child, "Volume", None)
            if volume is not None and volume.ViewObject and volume.ViewObject.Visibility:
                volume.ViewObject.Visibility = False
                self.hidden.append(volume)

    def onFrame(self, index):
        if self.frames is None or not self.frames.valid:
            return
        points, triangles = self.frames.frame(index)
        if self.view.show(points, triangles):
            self.hideVolumes()
        self.time.setText("{:.4g} s".format(self.frames.times[index]))
        rows = self.frames.monitorsAt(index)
        self.table.setRowCount(len(rows))
        for row, (label, _kind, value, unit) in enumerate(rows):
            self.table.setItem(row, 0, QtWidgets.QTableWidgetItem(label))
            self.table.setItem(row, 1, QtWidgets.QTableWidgetItem("{:.6g} {}".format(value, unit)))

    def onPlay(self):
        if self.playTimer.isActive():
            self.stopPlaying()
            return
        if self.slider.value() >= self.slider.maximum():
            self.slider.setValue(0)
        # a frame each output interval, as slow as the run was
        step = self.frames.times[1] - self.frames.times[0] if len(self.frames) > 1 else 0.0
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


Gui.addCommand("Fluid_CreateStudy", CommandCreateStudy())
Gui.addCommand("Fluid_AddFluid", CommandAddFluid())
Gui.addCommand("Fluid_AddMonitor", CommandAddMonitor())
Gui.addCommand("Fluid_AddPressureMonitor", CommandAddPressureMonitor())
Gui.addCommand("Fluid_RunStudy", CommandRunStudy())
