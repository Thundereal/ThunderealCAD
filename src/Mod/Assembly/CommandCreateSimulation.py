# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2024 Ondsel <development@ondsel.com>                     *
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

import re
import os
import time
import tempfile
from pathlib import Path

import FreeCAD as App

from pivy import coin
from Part import LineSegment, Compound

from PySide.QtCore import QT_TRANSLATE_NOOP

# Not upstream: the task panel's base class is QtCore.QObject, so the module
# imports without a GUI too (an upstream file's proxies restore from it)
from PySide import QtCore

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtCore, QtGui, QtWidgets
    from PySide.QtWidgets import (
        QPushButton,
        QMenu,
        QDialog,
        QComboBox,
        QLineEdit,
        QGridLayout,
        QLabel,
        QDialogButtonBox,
        QFileDialog,
        QProgressDialog,
    )
    from PySide.QtCore import Qt, QPoint
    from PySide.QtGui import QCursor, QIcon, QGuiApplication, QMessageBox

import UtilsAssembly
import Preferences

translate = App.Qt.translate

__title__ = "Assembly Command Create Simulation"
__author__ = "Ondsel"
__url__ = "https://www.freecad.org"


# Not upstream: what a dynamic run reports with its frames
# (docs/AssemblyDynamics.md sec 8.4), as getSimulationResults() names it: the
# label, the factor from the model's units to the shown one, and that unit
ResultQuantities = {
    "Force": (QT_TRANSLATE_NOOP("Assembly", "Reaction force"), 1e-3, "N"),
    "Torque": (QT_TRANSLATE_NOOP("Assembly", "Reaction torque"), 1e-6, "N m"),
    "MotorTorque": (QT_TRANSLATE_NOOP("Assembly", "Motor torque"), 1e-6, "N m"),
    "MotorForce": (QT_TRANSLATE_NOOP("Assembly", "Motor force"), 1e-3, "N"),
    "SpringForce": (QT_TRANSLATE_NOOP("Assembly", "Spring force"), 1e-3, "N"),
    "SpringLength": (QT_TRANSLATE_NOOP("Assembly", "Spring length"), 1.0, "mm"),
    "SpringTorque": (QT_TRANSLATE_NOOP("Assembly", "Spring torque"), 1e-6, "N m"),
    "SpringAngle": (QT_TRANSLATE_NOOP("Assembly", "Spring angle"), 57.29577951308232, "deg"),
    "BushingForce": (QT_TRANSLATE_NOOP("Assembly", "Bushing force"), 1e-3, "N"),
    "BushingTorque": (QT_TRANSLATE_NOOP("Assembly", "Bushing torque"), 1e-6, "N m"),
    "ContactForce": (QT_TRANSLATE_NOOP("Assembly", "Contact force"), 1e-3, "N"),
    "ContactNormalForce": (QT_TRANSLATE_NOOP("Assembly", "Contact normal force"), 1e-3, "N"),
    "ContactFrictionForce": (QT_TRANSLATE_NOOP("Assembly", "Contact friction force"), 1e-3, "N"),
    "ContactPenetration": (QT_TRANSLATE_NOOP("Assembly", "Contact penetration"), 1.0, "mm"),
    "FrictionForce": (QT_TRANSLATE_NOOP("Assembly", "Friction force"), 1e-3, "N"),
    "FrictionTorque": (QT_TRANSLATE_NOOP("Assembly", "Friction torque"), 1e-6, "N m"),
    # A contact set's events, which a sensor watches by their speed
    "ContactBegin": (QT_TRANSLATE_NOOP("Assembly", "Contact made"), 1.0, "mm/s"),
    "ContactEnd": (QT_TRANSLATE_NOOP("Assembly", "Contact let go"), 1.0, "mm/s"),
    # A cam contact's: the angle between the contact's normal and the way the
    # follower moves, the follower's slip along the cam, and the contact force
    # split along the follower's motion and across it
    "CamPressureAngle": (QT_TRANSLATE_NOOP("Assembly", "Pressure angle"), 57.29577951308232, "deg"),
    "CamSlip": (QT_TRANSLATE_NOOP("Assembly", "Cam slip"), 1.0, "mm/s"),
    "CamDriveForce": (QT_TRANSLATE_NOOP("Assembly", "Cam drive force"), 1e-3, "N"),
    "CamSideForce": (QT_TRANSLATE_NOOP("Assembly", "Cam side load"), 1e-3, "N"),
}


def resultsTable(assembly):
    """Not upstream (docs/AssemblyDynamics.md sec 8.4, 8.5): the last run's values
    as a table, (header, rows), a row per frame: the time in s, then each
    channel's components in the units the panel shows (a vector's X, Y and
    Z), each column named by the object, the quantity, the component and the
    unit, and "(not unique)" for a redundant joint's reaction. None when the
    run reported no values."""
    results = assembly.getSimulationResults()
    if not results["Channels"]:
        return None
    header = [translate("Assembly", "Time") + " (s)"]
    columns = []
    for channel in results["Channels"]:
        obj = channel["Object"]
        name = obj.Label if obj is not None else channel["Name"]
        label, factor, unit = ResultQuantities.get(
            channel["Quantity"], (channel["Quantity"], 1.0, "")
        )
        label = "%s: %s" % (name, translate("Assembly", label))
        note = ""
        if not channel.get("Unique", True):
            note = " " + translate("Assembly", "(not unique)")
        values = channel["Values"]
        if values and isinstance(values[0], App.Vector):
            for axis in "XYZ":
                header.append("%s %s (%s)%s" % (label, axis, unit, note))
                columns.append([getattr(v, axis.lower()) * factor for v in values])
        else:
            header.append("%s (%s)%s" % (label, unit, note))
            columns.append([v * factor for v in values])
    rows = []
    for i, time in enumerate(results["Times"]):
        rows.append([time] + [column[i] if i < len(column) else 0.0 for column in columns])
    return header, rows


def exportResultsCsv(assembly, path):
    """The last run's values (resultsTable()) as a CSV file, UTF-8, numbers
    to full precision; False when the run reported none"""
    import csv

    table = resultsTable(assembly)
    if table is None:
        return False
    header, rows = table
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        for row in rows:
            writer.writerow(["%.17g" % v for v in row])
    return True


def exportResultsSheet(assembly, doc=None, name="SimulationResults"):
    """The last run's values (resultsTable()) in a new spreadsheet of doc
    (the assembly's document by default): the names in row 1, a frame per
    row below. The sheet, or None when the run reported none."""
    table = resultsTable(assembly)
    if table is None:
        return None
    header, rows = table
    doc = doc or assembly.Document
    sheet = doc.addObject("Spreadsheet::Sheet", name)

    def column(k):
        letters = ""
        k += 1
        while k:
            k, r = divmod(k - 1, 26)
            letters = chr(ord("A") + r) + letters
        return letters

    for k, text in enumerate(header):
        # A leading quote keeps it text
        sheet.set("%s1" % column(k), "'" + text)
    for i, row in enumerate(rows):
        for k, v in enumerate(row):
            sheet.set("%s%d" % (column(k), i + 2), "%.17g" % v)
    sheet.recompute()
    return sheet

if App.GuiUp:

    class ResultPlot(QtWidgets.QWidget):
        """Not upstream: curves over the run's time, drawn by hand -- their
        range, the times at the ends, and the frame shown as a cursor."""

        COLOURS = ["#d62728", "#2ca02c", "#1f77b4", "#888888"]

        def __init__(self, parent=None):
            super().__init__(parent)
            self.setMinimumHeight(150)
            self.times = []
            self.curves = []  # (name, values)
            self.unit = ""
            self.frame = None

        def setCurves(self, times, curves, unit):
            self.times = list(times)
            self.curves = curves
            self.unit = unit
            self.update()

        def setFrame(self, frame):
            self.frame = frame
            self.update()

        def paintEvent(self, event):
            painter = QtGui.QPainter(self)
            painter.setRenderHint(QtGui.QPainter.Antialiasing)
            palette = self.palette()
            text = palette.color(QtGui.QPalette.WindowText)
            painter.fillRect(self.rect(), palette.color(QtGui.QPalette.Base))
            metrics = painter.fontMetrics()
            values = [v for _, curve in self.curves for v in curve]
            if len(self.times) < 2 or not values:
                painter.setPen(text)
                painter.drawText(
                    self.rect(), Qt.AlignCenter, translate("Assembly", "No values to show")
                )
                return
            low, high = min(values), max(values)
            if high - low <= 1e-12 * max(1.0, abs(high)):
                pad = max(abs(high) * 0.1, 1e-9)
                low, high = low - pad, high + pad

            def label(v):
                return f"{v:.4g}"

            left = 6 + max(metrics.horizontalAdvance(label(v)) for v in (low, high))
            area = QtCore.QRectF(
                left + 4,
                metrics.height() + 4,
                self.width() - left - 10,
                self.height() - 2 * metrics.height() - 10,
            )
            t0, t1 = self.times[0], self.times[-1]
            span = (t1 - t0) or 1.0

            def at(t, v):
                return QtCore.QPointF(
                    area.left() + area.width() * (t - t0) / span,
                    area.bottom() - area.height() * (v - low) / (high - low),
                )

            grid = QtGui.QColor(text)
            grid.setAlpha(70)
            painter.setPen(QtGui.QPen(grid, 1))
            painter.drawRect(area)
            if low < 0.0 < high:
                painter.drawLine(at(t0, 0.0), at(t1, 0.0))
            painter.setPen(text)
            painter.drawText(
                QtCore.QRectF(0, area.top() - metrics.height() / 2, left, metrics.height()),
                Qt.AlignRight | Qt.AlignVCenter,
                label(high),
            )
            painter.drawText(
                QtCore.QRectF(0, area.bottom() - metrics.height() / 2, left, metrics.height()),
                Qt.AlignRight | Qt.AlignVCenter,
                label(low),
            )
            painter.drawText(
                QtCore.QRectF(area.left(), area.bottom() + 2, area.width(), metrics.height()),
                Qt.AlignLeft | Qt.AlignTop,
                f"{t0:g} s",
            )
            painter.drawText(
                QtCore.QRectF(area.left(), area.bottom() + 2, area.width(), metrics.height()),
                Qt.AlignRight | Qt.AlignTop,
                f"{t1:g} s",
            )
            painter.drawText(
                QtCore.QRectF(0, 0, left, metrics.height()), Qt.AlignRight | Qt.AlignTop, self.unit
            )
            # The legend, along the top
            x = area.left()
            for k, (name, curve) in enumerate(self.curves):
                colour = QtGui.QColor(self.COLOURS[k % len(self.COLOURS)])
                painter.setPen(QtGui.QPen(colour, 1.5))
                n = min(len(self.times), len(curve))
                painter.drawPolyline(
                    QtGui.QPolygonF([at(self.times[i], curve[i]) for i in range(n)])
                )
                if len(self.curves) > 1:
                    painter.drawText(QtCore.QPointF(x, metrics.ascent()), name)
                    x += metrics.horizontalAdvance(name) + 10
            if self.frame is not None and 0 <= self.frame < len(self.times):
                cursor = QtGui.QColor(text)
                cursor.setAlpha(160)
                painter.setPen(QtGui.QPen(cursor, 1, Qt.DashLine))
                p = at(self.times[self.frame], low)
                painter.drawLine(QtCore.QPointF(p.x(), area.top()), QtCore.QPointF(p.x(), area.bottom()))



class CommandCreateSimulation:
    def __init__(self):
        pass

    def GetResources(self):
        return {
            "Pixmap": "Assembly_CreateSimulation",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateSimulation", "Simulation"),
            "Accel": "V",
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateSimulation",
                "Creates a new simulation of the current assembly",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        if not UtilsAssembly.isAssemblyCommandActive():
            return False

        assembly = UtilsAssembly.activeAssembly()
        joint_types = ["Revolute", "Slider", "Cylindrical"]
        joints = UtilsAssembly.getJointsOfType(assembly, joint_types)
        if len(joints) > 0:
            return True
        # Not upstream: a dynamic or static run needs no joint to drive --
        # parts falling onto one another by a contact set, a spring's pull
        return any(o.isDerivedFrom("Assembly::PhysicsItem") for o in assembly.Group)

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly:
            return

        self.panel = TaskAssemblyCreateSimulation()
        dialog = Gui.Control.showDialog(self.panel)
        if dialog is not None:
            dialog.setAutoCloseOnDeletedDocument(True)
            dialog.setDocumentName(App.ActiveDocument.Name)


# Not upstream: the C++ simulation's property names, and upstream's
# (letter prefixes ordered them in its property editor, which sorts by name)
SIMULATION_NAMES = {
    "TimeStart": "aTimeStart",
    "TimeEnd": "bTimeEnd",
    "TimeStepOutput": "cTimeStepOutput",
    "GlobalErrorTolerance": "fGlobalErrorTolerance",
    "FramesPerSecond": "jFramesPerSecond",
}


class SimulationValues:
    """Not upstream: a simulation's values by the C++ simulation's names, on
    it or on upstream's Python simulation, not converted"""

    def __init__(self, sim):
        object.__setattr__(self, "sim", sim)

    def _name(self, name):
        if self.sim.isDerivedFrom("Assembly::Simulation"):
            return name
        return SIMULATION_NAMES[name]

    def __getattr__(self, name):
        return getattr(self.sim, self._name(name))

    def __setattr__(self, name, value):
        setattr(self.sim, self._name(name), value)


######### Simulation Object ###########
class Simulation:
    def __init__(self, feaPy):
        feaPy.Proxy = self
        self.motionsChangedCallback = None
        # Not upstream: the C++ simulation (Assembly::SimulationPython) has
        # its group and its properties, under SIMULATION_NAMES' names, with
        # these defaults
        if feaPy.isDerivedFrom("Assembly::Simulation"):
            return
        feaPy.addExtension("App::GroupExtensionPython")

        if not hasattr(feaPy, "aTimeStart"):
            feaPy.addProperty(
                "App::PropertyTime",
                "aTimeStart",
                "Simulation",
                QT_TRANSLATE_NOOP(
                    "App::Property",
                    "Simulation start time.",
                ),
                locked=True,
            )

        if not hasattr(feaPy, "bTimeEnd"):
            feaPy.addProperty(
                "App::PropertyTime",
                "bTimeEnd",
                "Simulation",
                QT_TRANSLATE_NOOP(
                    "App::Property",
                    "Simulation end time.",
                ),
                locked=True,
            )

        if not hasattr(feaPy, "cTimeStepOutput"):
            feaPy.addProperty(
                "App::PropertyTime",
                "cTimeStepOutput",
                "Simulation",
                QT_TRANSLATE_NOOP(
                    "App::Property",
                    "Simulation time step for output.",
                ),
                locked=True,
            )

        if not hasattr(feaPy, "fGlobalErrorTolerance"):
            feaPy.addProperty(
                "App::PropertyFloat",
                "fGlobalErrorTolerance",
                "Simulation",
                QT_TRANSLATE_NOOP(
                    "App::Property",
                    "Integration global error tolerance.",
                ),
                locked=True,
            )

        if not hasattr(feaPy, "jFramesPerSecond"):
            feaPy.addProperty(
                "App::PropertyInteger",
                "jFramesPerSecond",
                "Simulation",
                QT_TRANSLATE_NOOP(
                    "App::Property",
                    "Frames Per Second.",
                ),
                locked=True,
            )

        feaPy.aTimeStart = 0.0
        feaPy.bTimeEnd = 1.0
        feaPy.cTimeStepOutput = 1.0e-2
        feaPy.fGlobalErrorTolerance = 1.0e-6
        feaPy.jFramesPerSecond = 30

        self.motionsChangedCallback = None

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def onChanged(self, feaPy, prop):
        if prop == "Group" and hasattr(self, "motionsChangedCallback"):
            if self.motionsChangedCallback is not None:
                self.motionsChangedCallback()
        # Not upstream: the open panel says when the run it shows is stale
        if prop == "ResultsStale" and getattr(self, "resultsStaleCallback", None):
            self.resultsStaleCallback()

    def setMotionsChangedCallback(self, callback):
        self.motionsChangedCallback = callback

    def execute(self, feaPy):
        """Do something when doing a recomputation, this method is mandatory"""
        pass

    def getAssembly(self, feaPy):
        assert feaPy.isDerivedFrom("App::FeaturePython") or feaPy.isDerivedFrom(
            "Assembly::Simulation"
        ), "Type error"
        for obj in feaPy.InList:
            if obj.isDerivedFrom("Assembly::AssemblyObject"):
                return obj
        return None


class ViewProviderSimulation:
    def __init__(self, vpDoc):
        vpDoc.Proxy = self
        self.Object = vpDoc.Object
        self.setProperties(vpDoc)

    def setProperties(self, vpDoc):
        if not hasattr(vpDoc, "Decimals"):
            vpDoc.addProperty(
                "App::PropertyInteger",
                "Decimals",
                "Space",
                QT_TRANSLATE_NOOP(
                    "App::Property", "The number of decimals to use for calculated texts"
                ),
                locked=True,
            )
            vpDoc.Decimals = 9

    def attach(self, vpDoc):
        """Setup the scene sub-graph of the view provider, this method is mandatory"""
        self.app_obj = vpDoc.Object

        self.display_mode = coin.SoType.fromName("SoFCSelection").createInstance()

        vpDoc.addDisplayMode(self.display_mode, "Wireframe")

    def updateData(self, feaPy, prop):
        """If a property of the handled feature has changed we have the chance to handle this here"""
        # Not upstream: the stale flag (docs/AssemblyDynamics.md sec 8.4)
        if prop == "ResultsStale":
            feaPy.ViewObject.signalChangeIcon()

    def getDisplayModes(self, vpDoc):
        """Return a list of display modes."""
        return ["Wireframe"]

    def getDefaultDisplayMode(self):
        """Return the name of the default display mode. It must be defined in getDisplayModes."""
        return "Wireframe"

    def onChanged(self, vpDoc, prop):
        """Here we can do something when a single property got changed"""
        pass

    def getIcon(self):
        # Not upstream: a stale run's study carries the tree's recompute mark
        if getattr(getattr(self, "app_obj", None), "ResultsStale", False):
            pixmap = QtGui.QIcon(":/icons/Assembly_CreateSimulation.svg").pixmap(64, 64)
            painter = QtGui.QPainter(pixmap)
            painter.drawPixmap(32, 0, QtGui.QIcon(":/icons/TreeRecompute.svg").pixmap(32, 32))
            painter.end()
            return QtGui.QIcon(pixmap)
        return ":/icons/Assembly_CreateSimulation.svg"

    def dumps(self):
        """When saving the document this object gets stored using Python's json module.\
                Since we have some un-serializable parts here -- the Coin stuff -- we must define this method\
                to return a tuple of all serializable objects or None."""
        return None

    def loads(self, state):
        """When restoring the serialized object from document we have the chance to set some internals here.\
                Since no data were serialized nothing needs to be done here."""
        return None

    def claimChildren(self):
        return self.app_obj.Group

    def doubleClicked(self, vpDoc):
        task = Gui.Control.activeTaskDialog()
        if task:
            task.reject()

        assembly = vpDoc.Object.Proxy.getAssembly(vpDoc.Object)

        if assembly is None:
            return False

        if UtilsAssembly.activeAssembly() != assembly:
            Gui.ActiveDocument.setEdit(assembly)

        panel = TaskAssemblyCreateSimulation(vpDoc.Object)
        dialog = Gui.Control.showDialog(panel)
        if dialog is not None:
            dialog.setAutoCloseOnDeletedDocument(True)
            dialog.setDocumentName(App.ActiveDocument.Name)

        return True

    def onDelete(self, vobj, subelements):
        for obj in self.claimChildren():
            obj.Document.removeObject(obj.Name)
        return True


########### Motion Object #############
MotionTypes = [
    "Angular",
    "Linear",
]


def motionJoint(motion):
    """Not upstream: the joint a motion drives, or None. The C++ motion's
    Joint is a plain link, upstream's a link with subnames"""
    joint = motion.Joint
    if isinstance(joint, tuple):
        return joint[0] if joint else None
    return joint


class Motion:
    def __init__(self, feaPy, motionType=MotionTypes[0], joint=None, formula=""):
        feaPy.Proxy = self

        self.createProperties(feaPy)

        # Not upstream: the C++ motion (Assembly::MotionPython) has the list
        if not feaPy.isDerivedFrom("Assembly::Motion"):
            feaPy.MotionType = MotionTypes  # sets the list
        feaPy.MotionType = motionType  # set the initial value
        feaPy.Joint = joint
        feaPy.Formula = formula

    def onDocumentRestored(self, feaPy):
        self.createProperties(feaPy)

    def createProperties(self, feaPy):
        if not hasattr(feaPy, "Joint"):
            feaPy.addProperty(
                "App::PropertyXLinkSubHidden",
                "Joint",
                "Motion",
                QT_TRANSLATE_NOOP("App::Property", "The joint that is moved by the motion"),
                locked=True,
            )

        if not hasattr(feaPy, "Formula"):
            feaPy.addProperty(
                "App::PropertyString",
                "Formula",
                "Motion",
                QT_TRANSLATE_NOOP(
                    "App::Property",
                    "This is the formula of the motion. For example '1.0*time'.",
                ),
                locked=True,
            )

        if not hasattr(feaPy, "MotionType"):
            feaPy.addProperty(
                "App::PropertyEnumeration",
                "MotionType",
                "Motion",
                QT_TRANSLATE_NOOP("App::Property", "The type of the motion"),
                locked=True,
            )

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def onChanged(self, feaPy, prop):
        pass

    def execute(self, feaPy):
        """Do something when doing a recomputation, this method is mandatory"""
        pass

    def getSimulation(self, feaPy):
        for obj in feaPy.InList:
            if hasattr(obj, "Proxy"):
                if hasattr(obj.Proxy, "setMotionsChangedCallback"):
                    return obj
        return None

    def getAssembly(self, feaPy):
        simulation = self.getSimulation(feaPy)
        if simulation is not None:
            return simulation.Proxy.getAssembly(simulation)
        return None


class ViewProviderMotion:
    def __init__(self, vp):
        vp.Proxy = self
        self.updateLabel()

    def attach(self, vpDoc):
        """Setup the scene sub-graph of the view provider, this method is mandatory"""
        self.app_obj = vpDoc.Object

        self.display_mode = coin.SoType.fromName("SoFCSelection").createInstance()

        vpDoc.addDisplayMode(self.display_mode, "Wireframe")

    def updateData(self, feaPy, prop):
        """If a property of the handled feature has changed we have the chance to handle this here"""
        pass

    def getDisplayModes(self, vpDoc):
        """Return a list of display modes."""
        return ["Wireframe"]

    def getDefaultDisplayMode(self):
        """Return the name of the default display mode. It must be defined in getDisplayModes."""
        return "Wireframe"

    def onChanged(self, vpDoc, prop):
        """Here we can do something when a single property got changed"""
        # App.Console.PrintMessage("Change property: " + str(prop) + "\n")
        pass

    def getIcon(self):
        if self.app_obj.MotionType == "Angular":
            return ":/icons/button_rotate.svg"

        return ":/icons/button_right.svg"

    def dumps(self):
        """When saving the document this object gets stored using Python's json module.\
                Since we have some un-serializable parts here -- the Coin stuff -- we must define this method\
                to return a tuple of all serializable objects or None."""
        return None

    def loads(self, state):
        """When restoring the serialized object from document we have the chance to set some internals here.\
                Since no data were serialized nothing needs to be done here."""
        return None

    def doubleClicked(self, vpDoc):
        self.openEditDialog()

    def openEditDialog(self):
        assembly = self.getAssembly()

        if assembly is None:
            return False

        joint = motionJoint(self.app_obj)

        control = getattr(self.app_obj, "Control", MotionControls[0])
        dialog = MotionEditDialog(
            assembly, self.app_obj.MotionType, joint, self.app_obj.Formula, control
        )
        if dialog.exec_():
            self.app_obj.MotionType = dialog.motionType
            self.app_obj.Joint = dialog.joint
            self.app_obj.Formula = dialog.formula
            if hasattr(self.app_obj, "Control"):
                self.app_obj.Control = dialog.control

            self.updateLabel()

    def updateLabel(self):
        joint = motionJoint(self.app_obj)
        if joint is None:
            return

        typeStr = "Linear" if self.app_obj.MotionType == "Linear" else "Angular"

        self.app_obj.Label = "{label} ({type_})".format(
            label=joint.Label, type_=translate("Assembly", typeStr)
        )

    def getAssembly(self):
        assembly = self.app_obj.Proxy.getAssembly(self.app_obj)

        if assembly is None:
            return None

        if UtilsAssembly.activeAssembly() != assembly:
            Gui.ActiveDocument.setEdit(assembly)

        return assembly


# Not upstream: what a motion's formula gives (docs/AssemblyDynamics.md sec 5.2)
MotionControls = ["Position", "Speed", "Effort"]


class MotionEditDialog:
    def __init__(
        self,
        assembly,
        motionType=MotionTypes[0],
        joint=None,
        formula="initialValue + 5*time",
        control=MotionControls[0],
    ):
        self.assembly = assembly
        self.motionType = motionType
        self.joint = joint
        self.formula = formula
        self.control = control

        # Create a non-modal, frameless dialog
        self.dialog = QDialog()
        self.dialog.setWindowFlags(Qt.Popup)
        self.initialPos = QCursor.pos()
        self.dialog.setMinimumSize(500, 200)  # Set a reasonable minimum size

        # Create the joints combobox
        self.joint_combo = QComboBox(self.dialog)
        self.setup_joint_combo()

        # Create the motion type combobox
        self.motion_type_combo = QComboBox(self.dialog)
        self.setup_motiontype_combo()

        def on_motion_type_changed(text):
            self.motionType = text

        self.motion_type_combo.currentTextChanged.connect(on_motion_type_changed)

        # Not upstream: Position, Speed or Effort
        self.control_combo = QComboBox(self.dialog)
        for name in MotionControls:
            self.control_combo.addItem(translate("Assembly", name), name)
        self.control_combo.setCurrentIndex(max(0, self.control_combo.findData(self.control)))
        self.control_combo.setToolTip(
            translate(
                "Assembly",
                "What the formula gives. Position: the angle (rad) or the distance (mm).\n"
                "Speed: their rate (rad/s, mm/s). Effort: the torque (N m) or the force (N).\n"
                "Speed and Effort need a Dynamic simulation.",
            )
        )

        def on_control_changed(index):
            self.control = self.control_combo.itemData(index)

        self.control_combo.currentIndexChanged.connect(on_control_changed)

        def on_joint_changed(index):
            self.joint = self.joint_combo.itemData(index)
            self.setup_motiontype_combo()  # Refresh the motion combo box based on the new joint type

        self.joint_combo.currentIndexChanged.connect(on_joint_changed)

        # Create the line edit for the formula
        formula_edit = QLineEdit(self.dialog)
        formula_edit.setText(self.formula)
        formula_edit.setPlaceholderText(translate("Assembly", "Enter your formula…"))

        # Connect the line edit to update the Formula property
        def on_formula_changed(text):
            self.formula = text

        formula_edit.textChanged.connect(on_formula_changed)

        self.setupHelpSection()

        # Create Ok and Cancel buttons
        button_box = QDialogButtonBox(
            QDialogButtonBox.Ok | QDialogButtonBox.Cancel, Qt.Horizontal, self.dialog
        )
        button_box.accepted.connect(self.dialog.accept)
        button_box.rejected.connect(self.dialog.reject)

        # Set up the layout of the dialog
        layout = QGridLayout(self.dialog)

        # Add labels and widgets to the layout
        layout.addWidget(QLabel("Joint"), 0, 0)
        layout.addWidget(self.joint_combo, 0, 1)

        layout.addWidget(QLabel("Motion Type"), 1, 0)
        layout.addWidget(self.motion_type_combo, 1, 1)

        layout.addWidget(QLabel(translate("Assembly", "Control")), 2, 0)
        layout.addWidget(self.control_combo, 2, 1)

        layout.addWidget(QLabel("Formula"), 3, 0)
        layout.addWidget(formula_edit, 3, 1)

        # Add the help label above the buttons
        layout.addWidget(self.help_label0, 4, 0, 1, 2)
        layout.addWidget(self.help_label1, 5, 0, 1, 2)
        layout.addWidget(self.help_label2, 6, 0, 1, 2)
        layout.addWidget(self.help_label3, 7, 0, 1, 2)
        layout.addWidget(self.help_label4, 8, 0, 1, 2)
        layout.addWidget(self.help_label5, 9, 0, 1, 2)
        layout.addWidget(self.help_label6, 10, 0, 1, 2)
        layout.addWidget(self.help_label7, 11, 0, 1, 2)
        # Add the help button and button box in the next row
        layout.addWidget(self.help_button, 12, 0)

        layout.addWidget(button_box, 12, 1)

        self.positionDialog()

    def setupHelpSection(self):

        # Create the help QLabels and set them to be initially hidden
        self.help_label0 = QLabel(
            translate(
                "Assembly",
                "In capital are variables that you need to replace with actual values. 'initialValue' is dynamically replaced by the current angle or distance. More details about each example in its tooltip.",
            ),
            self.dialog,
        )
        self.help_label1 = QLabel(translate("Assembly", " - Linear: C + VEL*time"), self.dialog)
        self.help_label2 = QLabel(
            translate("Assembly", " - Quadratic: C + VEL*time + ACC*time^2"), self.dialog
        )
        self.help_label3 = QLabel(
            translate("Assembly", " - Harmonic: C + AMP*sin(VEL*time - PHASE)"), self.dialog
        )
        self.help_label4 = QLabel(
            translate("Assembly", " - Exponential: C*exp(time/TIMEC)"), self.dialog
        )
        self.help_label5 = QLabel(
            translate(
                "Assembly",
                " - Smooth Step: L1 + (L2 - L1)*((1/2) + (1/pi)*arctan(SLOPE*(time - T0)))",
            ),
            self.dialog,
        )
        self.help_label6 = QLabel(
            translate(
                "Assembly",
                " - Smooth Square Impulse: (H/pi)*(arctan(SLOPE*(time - T1)) - arctan(SLOPE*(time - T2)))",
            ),
            self.dialog,
        )
        self.help_label7 = QLabel(
            translate(
                "Assembly",
                " - Smooth Ramp Top Impulse: ((1/pi)*(arctan(1000*(time - T1)) - arctan(1000*(time - T2))))*(((H2 - H1)/(T2 - T1))*(time - T1) + H1)",
            ),
            self.dialog,
        )

        self.help_label1.setToolTip(
            translate(
                "Assembly",
                """C is a constant offset.
VEL is a velocity or slope or gradient of the straight line.""",
            )
        )
        self.help_label2.setToolTip(
            translate(
                "Assembly",
                """C is a constant offset.
VEL is the velocity or slope or gradient of the straight line.
ACC is the acceleration or coefficient of the second order. The function is a parabola.""",
            )
        )
        self.help_label3.setToolTip(
            translate(
                "Assembly",
                """C is a constant offset.
AMP is the amplitude of the sine wave.
VEL is the angular velocity in radians per second.
PHASE is the phase of the sine wave.""",
            )
        )
        self.help_label4.setToolTip(
            translate(
                "Assembly",
                """C is a constant.
TIMEC is the time constant of the exponential function.""",
            )
        )
        self.help_label5.setToolTip(
            translate(
                "Assembly",
                """L1 is step level before time = T0.
L2 is step level after time = T0.
SLOPE defines the steepness of the transition between L1 and L2 about time = T0. Higher values gives sharper cornered steps. SLOPE = 1000 or greater are suitable.""",
            )
        )
        self.help_label6.setToolTip(
            translate(
                "Assembly",
                """H is the height of the impulse.
T1 is the start of the impulse.
T2 is the end of the impulse.
SLOPE defines the steepness of the transition between 0 and H about time = T1 and T2. Higher values gives sharper cornered impulses. SLOPE = 1000 or greater are suitable.""",
            )
        )
        self.help_label7.setToolTip(
            translate(
                "Assembly",
                """This is similar to the square impulse but the top has a sloping ramp. It is good for building a smooth piecewise linear function by adding a series of these.
T1 is the start of the impulse.
T2 is the end of the impulse.
H1 is the height at T1 at the beginning of the ramp.
H2 is the height at T2 at the end of the ramp.
SLOPE defines the steepness of the transition between 0 and H1 and H2 to 0 about time = T1 and T2 respectively. Higher values gives sharper cornered impulses. SLOPE = 1000 or greater are suitable.""",
            )
        )

        self.help_label0.setWordWrap(True)
        self.help_label1.setWordWrap(True)
        self.help_label2.setWordWrap(True)
        self.help_label3.setWordWrap(True)
        self.help_label4.setWordWrap(True)
        self.help_label5.setWordWrap(True)
        self.help_label6.setWordWrap(True)
        self.help_label7.setWordWrap(True)

        width = 1000
        self.help_label0.setFixedWidth(width)
        self.help_label1.setFixedWidth(width)
        self.help_label2.setFixedWidth(width)
        self.help_label3.setFixedWidth(width)
        self.help_label4.setFixedWidth(width)
        self.help_label5.setFixedWidth(width)
        self.help_label6.setFixedWidth(width)
        self.help_label7.setFixedWidth(width)

        self.help_label0.setVisible(False)
        self.help_label1.setVisible(False)
        self.help_label2.setVisible(False)
        self.help_label3.setVisible(False)
        self.help_label4.setVisible(False)
        self.help_label5.setVisible(False)
        self.help_label6.setVisible(False)
        self.help_label7.setVisible(False)

        self.help_label1.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.help_label2.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.help_label3.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.help_label4.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.help_label5.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.help_label6.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.help_label7.setTextInteractionFlags(Qt.TextSelectableByMouse)
        # Create the Help button
        self.help_button = QPushButton(translate("Assembly", "Help"), self.dialog)

        # Slot to toggle help visibility and button text
        def toggle_help():
            show = not self.help_label1.isVisible()
            self.help_label0.setVisible(show)
            self.help_label1.setVisible(show)
            self.help_label2.setVisible(show)
            self.help_label3.setVisible(show)
            self.help_label4.setVisible(show)
            self.help_label5.setVisible(show)
            self.help_label6.setVisible(show)
            self.help_label7.setVisible(show)

            if show:
                self.help_button.setText(translate("Assembly", "Hide help"))
            else:
                self.help_button.setText(translate("Assembly", "Help"))

            self.positionDialog()

        self.help_button.clicked.connect(toggle_help)

    def positionDialog(self):
        self.dialog.adjustSize()

        # Get the screen where the mouse is located
        screen = QGuiApplication.screenAt(self.initialPos)
        screen_geometry = (
            screen.availableGeometry()
            if screen
            else QApplication.primaryScreen().availableGeometry()
        )

        # Calculate the position of the dialog to ensure it stays within the screen
        dialog_position = self.initialPos

        # Adjust position to keep the dialog within the screen bounds
        if dialog_position.x() + self.dialog.width() > screen_geometry.right():
            dialog_position.setX(screen_geometry.right() - self.dialog.width())
        if dialog_position.y() + self.dialog.height() > screen_geometry.bottom():
            dialog_position.setY(screen_geometry.bottom() - self.dialog.height())

        # Ensure the dialog does not go above or to the left of the screen
        if dialog_position.x() < screen_geometry.left():
            dialog_position.setX(screen_geometry.left())
        if dialog_position.y() < screen_geometry.top():
            dialog_position.setY(screen_geometry.top())

        # Move the dialog to the final position
        self.dialog.move(dialog_position)

    def setup_joint_combo(self):
        # Function to set up the joint combo box based on the selected motion type

        self.joint_combo.clear()  # Clear existing items

        jointTypes = ["Revolute", "Slider", "Cylindrical"]

        joints = UtilsAssembly.getJointsOfType(self.assembly, jointTypes)

        # Add joints to the combo box with labels and icons
        for joint in joints:
            joint_label = joint.Label
            joint_icon = QIcon(joint.ViewObject.Icon)
            self.joint_combo.addItem(joint_icon, joint_label, userData=joint)

        # Set the current value based on the object's Joint property
        if self.joint in joints:
            self.joint_combo.setCurrentText(self.joint.Label)
        elif len(joints) > 0:
            self.joint = joints[0]

    def setup_motiontype_combo(self):
        self.motion_type_combo.clear()  # Clear existing items

        if self.joint is None:
            return

        if self.joint.JointType == "Revolute":
            types = ["Angular"]
        elif self.joint.JointType == "Slider":
            types = ["Linear"]
        else:
            types = ["Angular", "Linear"]

        self.motion_type_combo.addItems(types)

        # Set current value based on the object's MotionType
        if self.motionType in types:
            self.motion_type_combo.setCurrentText(self.motionType)
        else:
            # self.motionType is no longer available, so we reset it to first entry
            self.motionType = types[0]

    def exec_(self):
        return self.dialog.exec()


def selectedPoint(assembly):
    """Not upstream: the component of the assembly the selection picks, and
    the point picked in the component's coordinates -- a vertex's own point,
    else where the pick hit; None without one"""
    if not App.GuiUp:
        return None
    for sel in Gui.Selection.getSelectionEx("", 0):
        picks = list(sel.PickedPoints)
        shapes = list(sel.SubObjects)
        for k, sub in enumerate(sel.SubElementNames):
            part, element = UtilsAssembly.getComponentReference(assembly, sel.Object, sub)
            if part is None:
                continue
            name = element.split(".")[-1]
            if name.startswith("Vertex") and k < len(shapes):
                where = shapes[k].Point
            elif k < len(picks):
                where = picks[k]
            else:
                continue
            # Global, through the assembly into the component's coordinates
            toPart = assembly.getGlobalPlacement().multiply(part.Placement)
            return part, toPart.inverse().multVec(where)
    return None


########### Initial velocity #############
# Not upstream (docs/AssemblyDynamics.md sec 5.2's InitialCondition): a
# Dynamic study's start velocity, a child of the Simulation as a motion is


def simulationOf(obj):
    """The Simulation a motion or an initial velocity is a child of"""
    for parent in obj.InList:
        if parent.isDerivedFrom("Assembly::Simulation") or hasattr(
            getattr(parent, "Proxy", None), "setMotionsChangedCallback"
        ):
            return parent
    return None


def assemblyOf(obj):
    sim = simulationOf(obj)
    if sim is None:
        return None
    for parent in sim.InListRecursive:
        if parent.isDerivedFrom("Assembly::AssemblyObject"):
            return parent
    return None


def newInitialCondition(sim, joint=None, rate=None, component=None, linear=None, angular=None):
    """An initial velocity in the simulation: a joint's rate (in deg/s for
    an angular one, a number or a quantity) when joint is given, else the
    component's linear (mm/s) and angular (deg/s) velocity"""
    assembly = None
    for parent in sim.InListRecursive:
        if parent.isDerivedFrom("Assembly::AssemblyObject"):
            assembly = parent
            break
    obj = (assembly or sim).newObject("Assembly::InitialConditionPython", "InitialVelocity")
    if App.GuiUp:
        ViewProviderInitialCondition(obj.ViewObject)
    if joint is not None:
        obj.Mode = "Joint"
        obj.Joint = joint
        obj.MotionType = "Linear" if joint.JointType == "Slider" else "Angular"
        if rate is not None:
            obj.Rate = rate
    else:
        obj.Mode = "Component"
        obj.Component = component
        obj.LinearVelocity = linear or App.Vector()
        obj.AngularVelocity = angular or App.Vector()
    group = list(sim.Group)
    if obj not in group:
        sim.Group = group + [obj]
    describeInitialCondition(obj)
    return obj


def describeInitialCondition(obj):
    """The label: what it starts moving"""
    if obj.Mode == "Joint":
        target = obj.Joint
    else:
        target = obj.Component
    if target is not None:
        obj.Label = translate("Assembly", "{} (initial velocity)").format(target.Label)


class ViewProviderInitialCondition:
    def __init__(self, vp):
        vp.Proxy = self

    def attach(self, vpDoc):
        self.app_obj = vpDoc.Object

    def getDisplayModes(self, vpDoc):
        return []

    def getDefaultDisplayMode(self):
        return ""

    def getIcon(self):
        return ":/icons/button_right.svg"

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def doubleClicked(self, vpDoc):
        self.openEditDialog()
        return True

    def openEditDialog(self):
        assembly = assemblyOf(self.app_obj)
        if assembly is None:
            return False
        dialog = InitialConditionDialog(assembly, self.app_obj)
        if dialog.exec_():
            dialog.apply(self.app_obj)
        return True


class InitialConditionDialog:
    """An initial velocity's values: a joint and its rate, or a component and
    its velocity. apply() writes them to the object."""

    def __init__(self, assembly, obj=None):
        self.assembly = assembly
        self.dialog = QDialog()
        self.dialog.setWindowTitle(translate("Assembly", "Initial velocity"))
        layout = QGridLayout(self.dialog)

        self.modeCombo = QComboBox(self.dialog)
        self.modeCombo.addItem(translate("Assembly", "A joint's rate"), "Joint")
        self.modeCombo.addItem(translate("Assembly", "A component's velocity"), "Component")
        layout.addWidget(self.modeCombo, 0, 0, 1, 4)

        self.jointCombo = QComboBox(self.dialog)
        for joint in UtilsAssembly.getJointsOfType(assembly, ["Revolute", "Slider", "Cylindrical"]):
            self.jointCombo.addItem(QIcon(joint.ViewObject.Icon), joint.Label, userData=joint)
        self.typeCombo = QComboBox(self.dialog)
        self.rateBox = QtWidgets.QDoubleSpinBox(self.dialog)
        self.rateBox.setRange(-1e9, 1e9)
        self.rateBox.setDecimals(4)
        self.jointLabel = QLabel(translate("Assembly", "Joint"))
        layout.addWidget(self.jointLabel, 1, 0)
        layout.addWidget(self.jointCombo, 1, 1, 1, 3)
        self.typeLabel = QLabel(translate("Assembly", "Motion Type"))
        layout.addWidget(self.typeLabel, 2, 0)
        layout.addWidget(self.typeCombo, 2, 1, 1, 3)
        self.rateLabel = QLabel(translate("Assembly", "Rate"))
        self.rateBox.setToolTip(
            translate(
                "Assembly",
                "The joint's rate at the start, the way a Speed motion of it turns or slides it",
            )
        )
        layout.addWidget(self.rateLabel, 3, 0)
        layout.addWidget(self.rateBox, 3, 1, 1, 3)

        self.componentCombo = QComboBox(self.dialog)
        for part in UtilsAssembly.getMovablePartsWithin(assembly, True):
            self.componentCombo.addItem(part.Label, userData=part)
        self.componentLabel = QLabel(translate("Assembly", "Component"))
        layout.addWidget(self.componentLabel, 4, 0)
        layout.addWidget(self.componentCombo, 4, 1, 1, 3)

        def vectorRow(row, text, suffix, tip):
            label = QLabel(text)
            label.setToolTip(tip)
            layout.addWidget(label, row, 0)
            boxes = []
            for col, axis in enumerate("xyz"):
                box = QtWidgets.QDoubleSpinBox(self.dialog)
                box.setRange(-1e9, 1e9)
                box.setDecimals(4)
                box.setPrefix(axis + " ")
                box.setSuffix(suffix)
                layout.addWidget(box, row, col + 1)
                boxes.append(box)
            return label, boxes

        self.linearLabel, self.linearBoxes = vectorRow(
            5,
            translate("Assembly", "Linear"),
            " mm/s",
            translate("Assembly", "The velocity of the component's origin, in the assembly's axes"),
        )
        self.angularLabel, self.angularBoxes = vectorRow(
            6,
            translate("Assembly", "Angular"),
            " deg/s",
            translate("Assembly", "The component's angular velocity, in the assembly's axes"),
        )
        note = QLabel(
            translate(
                "Assembly",
                "A Dynamic study starts with these velocities; what the joints do not allow\n"
                "is taken out.",
            )
        )
        layout.addWidget(note, 7, 0, 1, 4)
        buttons = QDialogButtonBox(
            QDialogButtonBox.Ok | QDialogButtonBox.Cancel, Qt.Horizontal, self.dialog
        )
        buttons.accepted.connect(self.dialog.accept)
        buttons.rejected.connect(self.dialog.reject)
        layout.addWidget(buttons, 8, 0, 1, 4)

        self.modeCombo.currentIndexChanged.connect(self.showMode)
        self.jointCombo.currentIndexChanged.connect(self.fillTypes)
        self.typeCombo.currentIndexChanged.connect(self.showUnit)
        self.fill(obj)

    def fill(self, obj):
        mode = obj.Mode if obj is not None else "Joint"
        self.modeCombo.setCurrentIndex(max(0, self.modeCombo.findData(mode)))
        if obj is not None and obj.Joint is not None:
            self.jointCombo.setCurrentIndex(max(0, self.jointCombo.findData(obj.Joint)))
        self.fillTypes()
        if obj is not None:
            self.typeCombo.setCurrentIndex(max(0, self.typeCombo.findData(obj.MotionType)))
            self.rateBox.setValue(obj.Rate.Value)
            if obj.Component is not None:
                self.componentCombo.setCurrentIndex(
                    max(0, self.componentCombo.findData(obj.Component))
                )
            for box, value in zip(self.linearBoxes, obj.LinearVelocity):
                box.setValue(value)
            for box, value in zip(self.angularBoxes, obj.AngularVelocity):
                box.setValue(value)
        self.showMode()

    def fillTypes(self):
        joint = self.jointCombo.currentData()
        current = self.typeCombo.currentData()
        self.typeCombo.blockSignals(True)
        self.typeCombo.clear()
        if joint is not None:
            if joint.JointType == "Revolute":
                types = ["Angular"]
            elif joint.JointType == "Slider":
                types = ["Linear"]
            else:
                types = ["Angular", "Linear"]
            for name in types:
                self.typeCombo.addItem(translate("Assembly", name), name)
            self.typeCombo.setCurrentIndex(max(0, self.typeCombo.findData(current)))
        self.typeCombo.blockSignals(False)
        self.showUnit()

    def showUnit(self):
        self.rateBox.setSuffix(" mm/s" if self.typeCombo.currentData() == "Linear" else " deg/s")

    def showMode(self):
        joint = self.modeCombo.currentData() == "Joint"
        for widget in (
            self.jointLabel,
            self.jointCombo,
            self.typeLabel,
            self.typeCombo,
            self.rateLabel,
            self.rateBox,
        ):
            widget.setVisible(joint)
        for widget in [self.componentLabel, self.componentCombo, self.linearLabel]:
            widget.setVisible(not joint)
        for widget in [self.angularLabel] + self.linearBoxes + self.angularBoxes:
            widget.setVisible(not joint)

    def apply(self, obj):
        obj.Mode = self.modeCombo.currentData()
        if obj.Mode == "Joint":
            obj.Joint = self.jointCombo.currentData()
            obj.MotionType = self.typeCombo.currentData() or "Angular"
            obj.Rate = self.rateBox.value()
        else:
            obj.Component = self.componentCombo.currentData()
            obj.LinearVelocity = App.Vector(*[box.value() for box in self.linearBoxes])
            obj.AngularVelocity = App.Vector(*[box.value() for box in self.angularBoxes])
        describeInitialCondition(obj)

    def exec_(self):
        return self.dialog.exec()


########### Sensor #############
# Not upstream (docs/AssemblyDynamics.md sec 5.2's Sensor, P15): a value the
# run reports, watched after each step, which stops the run where it passes
# a threshold or marks the time; a child of the Simulation as a motion is

# What each kind of source reports, the quantities a sensor on it can watch
SensorQuantities = {
    "Joint": ("Force", "Torque", "MotorTorque", "MotorForce"),
    "Spring": ("SpringForce", "SpringLength"),
    "TorsionSpring": ("SpringTorque", "SpringAngle"),
    "Bushing": ("BushingForce", "BushingTorque"),
    "Contact": (
        "ContactForce",
        "ContactNormalForce",
        "ContactFrictionForce",
        "ContactPenetration",
        "ContactBegin",
        "ContactEnd",
    ),
    "CamContact": (
        "ContactForce",
        "ContactNormalForce",
        "ContactFrictionForce",
        "ContactPenetration",
        "CamPressureAngle",
        "CamSlip",
        "CamDriveForce",
        "CamSideForce",
        "ContactBegin",
        "ContactEnd",
    ),
    "Friction": ("FrictionForce", "FrictionTorque"),
}

# The contact events a sensor watches, which trip at a speed at or above
# the threshold, or below it
SensorEvents = ("ContactBegin", "ContactEnd")


def sensorSourceKind(obj):
    """Which of SensorQuantities' kinds obj is, or None"""
    if obj is None:
        return None
    if obj.isDerivedFrom("Assembly::CamContact"):
        return "CamContact"
    if obj.isDerivedFrom("Assembly::ContactSet"):
        return "Contact"
    if obj.isDerivedFrom("Assembly::SpringDamper"):
        return "Spring"
    if obj.isDerivedFrom("Assembly::TorsionSpring"):
        return "TorsionSpring"
    if obj.isDerivedFrom("Assembly::Bushing"):
        return "Bushing"
    if obj.isDerivedFrom("Assembly::JointFriction"):
        return "Friction"
    if hasattr(obj, "JointType") or (hasattr(obj, "Joint") and hasattr(obj, "Formula")):
        return "Joint"
    return None


# A vector's quantities, whose component or magnitude a sensor takes
SensorVectors = (
    "Force",
    "Torque",
    "BushingForce",
    "BushingTorque",
    "ContactForce",
    "ContactNormalForce",
    "ContactFrictionForce",
    "CamDriveForce",
    "CamSideForce",
)


def sensorSources(assembly):
    """What a sensor can watch in the assembly: its joints (but the
    grounding ones), and its springs, torsion springs, bushings and contact
    sets"""
    out = []
    joints = assembly.getGroup("Joints")
    for joint in joints.Group if joints else []:
        if hasattr(joint, "JointType"):
            out.append(joint)
    physics = assembly.getGroup("Physics")
    for item in physics.Group if physics else []:
        if sensorSourceKind(item) not in (None, "Joint"):
            out.append(item)
    return out


def thresholdFactor(quantity):
    """From a threshold's internal unit (mN, mN mm, mm, deg) to the one
    the panel shows; ResultQuantities' factors are from the run's values,
    which take an angle in radians"""
    if quantity in ("SpringAngle", "CamPressureAngle"):
        return 1.0
    return ResultQuantities.get(quantity, ("", 1.0, ""))[1]


def newSensor(
    sim,
    source,
    quantity,
    threshold,
    component="Magnitude",
    compare="Above",
    action="Stop",
    count=1,
    pair=(),
):
    """A sensor in the simulation: source's quantity (one of
    SensorQuantities' for its kind; a vector's component or its magnitude)
    passing threshold (a number in the quantity's internal unit -- mN, mN mm,
    mm, deg -- or a quantity) rising above it, or falling below it with
    compare "Below", or either way with "Crosses"; action "Stop" ends the
    run there, "Mark" notes the time. A contact set's ContactBegin or
    ContactEnd trips at each contact made or let go at a speed (mm/s) at or
    above threshold, or below it, by a pair with pair's components (one or
    two; none, any pair). It acts on its count'th trip, those before let
    by: it stops there, or marks that one and each after"""
    assembly = None
    for parent in sim.InListRecursive:
        if parent.isDerivedFrom("Assembly::AssemblyObject"):
            assembly = parent
            break
    obj = (assembly or sim).newObject("Assembly::SensorPython", "Sensor")
    if App.GuiUp:
        ViewProviderSensor(obj.ViewObject)
    obj.Source = source
    obj.Quantity = quantity
    obj.Component = component
    obj.Compare = compare
    obj.Threshold = threshold
    obj.Action = action
    obj.Count = count
    obj.Pair = list(pair)
    group = list(sim.Group)
    if obj not in group:
        sim.Group = group + [obj]
    describeSensor(obj)
    return obj


def describeSensor(obj):
    """The label: what it watches, and where it trips, as the panel shows
    the quantity, as Spring: Spring length above 5.5 mm"""
    if obj.Source is None:
        return
    label, _, unit = ResultQuantities.get(obj.Quantity, (obj.Quantity, 1.0, ""))
    value = "%.6g %s" % (obj.Threshold.Value * thresholdFactor(obj.Quantity), unit)
    if obj.Quantity in SensorVectors and obj.Component != "Magnitude":
        label = "%s %s" % (translate("Assembly", label), obj.Component)
    else:
        label = translate("Assembly", label)
    pair = [o.Label for o in sensorPair(obj)]
    if len(pair) == 2:
        label = translate("Assembly", "{} by {} and {}").format(label, *pair)
    elif pair:
        label = translate("Assembly", "{} by {}").format(label, pair[0])
    if obj.Quantity in SensorEvents:
        # As Contact: Contact made below 1 mm/s; every one, Contact: Contact made
        if obj.Compare == "Below":
            text = translate("Assembly", "{}: {} below {}")
        elif obj.Threshold.Value > 0:
            text = translate("Assembly", "{}: {} at {} or more")
        else:
            text = "{}: {}"
    elif obj.Compare == "Below":
        text = translate("Assembly", "{}: {} below {}")
    elif obj.Compare == "Crosses":
        text = translate("Assembly", "{}: {} crossing {}")
    else:
        text = translate("Assembly", "{}: {} above {}")
    text = text.format(obj.Source.Label, label, value.strip())
    # The trip it acts on, the first unsaid: Contact: ..., at the 3rd
    if obj.Count > 1:
        if obj.Action == "Mark":
            text = translate("Assembly", "{}, from the {}").format(text, ordinal(obj.Count))
        else:
            text = translate("Assembly", "{}, at the {}").format(text, ordinal(obj.Count))
    obj.Label = text


def ordinal(n):
    """1st, 2nd, 3rd, 4th, ..., 11th, 12th, 13th, 21st"""
    if n % 100 in (11, 12, 13):
        return "%dth" % n
    return "%d%s" % (n, {1: "st", 2: "nd", 3: "rd"}.get(n % 10, "th"))


def sensorPair(obj):
    """The components of the pair a sensor on contact events watches, those
    the run uses: two at most; none for another quantity"""
    if obj.Quantity not in SensorEvents:
        return []
    return [o for o in getattr(obj, "Pair", []) if o is not None][:2]


def contactComponents(contact):
    """The components a contact set's pairs are of, for a sensor's pair"""
    out = []
    for name in ("GroupA", "GroupB"):
        out += list(getattr(contact, name, None) or [])
    for name in ("ProfileA", "ProfileB"):
        link = getattr(contact, name, None)
        if link:
            out.append(link[0])
    seen = []
    for o in out:
        if o is not None and o not in seen:
            seen.append(o)
    return seen


class ViewProviderSensor:
    def __init__(self, vp):
        vp.Proxy = self

    def attach(self, vpDoc):
        self.app_obj = vpDoc.Object

    def getDisplayModes(self, vpDoc):
        return []

    def getDefaultDisplayMode(self):
        return ""

    def getIcon(self):
        return ":/icons/Assembly_Sensor.svg"

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def doubleClicked(self, vpDoc):
        self.openEditDialog()
        return True

    def openEditDialog(self):
        assembly = assemblyOf(self.app_obj)
        if assembly is None:
            return False
        dialog = SensorDialog(assembly, self.app_obj)
        if dialog.exec_():
            dialog.apply(self.app_obj)
        return True


class SensorDialog:
    """A sensor's values: what it watches, its threshold and what it does
    there. apply() writes them to the object."""

    def __init__(self, assembly, obj=None):
        self.assembly = assembly
        self.dialog = QDialog()
        self.dialog.setWindowTitle(translate("Assembly", "Sensor"))
        layout = QGridLayout(self.dialog)

        self.sourceCombo = QComboBox(self.dialog)
        for source in sensorSources(assembly):
            self.sourceCombo.addItem(QIcon(source.ViewObject.Icon), source.Label, userData=source)
        self.sourceCombo.setToolTip(
            translate(
                "Assembly",
                "What reports the value: a joint (its reaction, or the effort of the motion\n"
                "driving it), a spring, a torsion spring, a bushing or a contact set",
            )
        )
        layout.addWidget(QLabel(translate("Assembly", "Watch")), 0, 0)
        layout.addWidget(self.sourceCombo, 0, 1, 1, 2)

        self.quantityCombo = QComboBox(self.dialog)
        layout.addWidget(QLabel(translate("Assembly", "Value")), 1, 0)
        layout.addWidget(self.quantityCombo, 1, 1, 1, 2)

        self.componentCombo = QComboBox(self.dialog)
        for name, text in (
            ("Magnitude", translate("Assembly", "Magnitude")),
            ("X", "X"),
            ("Y", "Y"),
            ("Z", "Z"),
        ):
            self.componentCombo.addItem(text, name)
        self.componentCombo.setToolTip(
            translate("Assembly", "A vector's magnitude, or its component in the assembly's axes")
        )
        self.componentLabel = QLabel(translate("Assembly", "Component"))
        layout.addWidget(self.componentLabel, 2, 0)
        layout.addWidget(self.componentCombo, 2, 1, 1, 2)

        self.compareCombo = QComboBox(self.dialog)
        self.compareCombo.addItem(translate("Assembly", "Rises above"), "Above")
        self.compareCombo.addItem(translate("Assembly", "Falls below"), "Below")
        self.thresholdBox = QtWidgets.QDoubleSpinBox(self.dialog)
        self.thresholdBox.setRange(-1e12, 1e12)
        self.thresholdBox.setDecimals(6)
        layout.addWidget(QLabel(translate("Assembly", "When it")), 3, 0)
        layout.addWidget(self.compareCombo, 3, 1)
        layout.addWidget(self.thresholdBox, 3, 2)

        # A contact's events: of a pair with these components
        self.pairCombos = [QComboBox(self.dialog), QComboBox(self.dialog)]
        for combo in self.pairCombos:
            combo.setToolTip(
                translate(
                    "Assembly",
                    "The pair whose contacts count: one component, any pair with it;\n"
                    "two, that pair; any, every pair of the set",
                )
            )
        self.pairLabel = QLabel(translate("Assembly", "Between"))
        layout.addWidget(self.pairLabel, 4, 0)
        layout.addWidget(self.pairCombos[0], 4, 1)
        layout.addWidget(self.pairCombos[1], 4, 2)

        self.actionCombo = QComboBox(self.dialog)
        self.actionCombo.addItem(translate("Assembly", "Stop the run there"), "Stop")
        self.actionCombo.addItem(translate("Assembly", "Mark the time and go on"), "Mark")
        layout.addWidget(QLabel(translate("Assembly", "Then")), 5, 0)
        layout.addWidget(self.actionCombo, 5, 1, 1, 2)

        self.countBox = QtWidgets.QSpinBox(self.dialog)
        self.countBox.setRange(1, 1000000)
        self.countBox.setToolTip(
            translate(
                "Assembly",
                "The trip it acts on, 1 the first: those before are let by.\n"
                "A sensor that marks marks that one and each after it.",
            )
        )
        layout.addWidget(QLabel(translate("Assembly", "On trip")), 6, 0)
        layout.addWidget(self.countBox, 6, 1)

        note = QLabel(
            translate(
                "Assembly",
                "A Dynamic study watches it after each step and finds the time it trips to\n"
                "a small fraction of a step; a Static study at each output time. A contact\n"
                "made or let go, each pair's, trips it by its speed in or out (Dynamic only).",
            )
        )
        layout.addWidget(note, 7, 0, 1, 3)
        buttons = QDialogButtonBox(
            QDialogButtonBox.Ok | QDialogButtonBox.Cancel, Qt.Horizontal, self.dialog
        )
        buttons.accepted.connect(self.dialog.accept)
        buttons.rejected.connect(self.dialog.reject)
        self.okButton = buttons.button(QDialogButtonBox.Ok)
        layout.addWidget(buttons, 8, 0, 1, 3)

        self.sourceCombo.currentIndexChanged.connect(self.fillQuantities)
        self.quantityCombo.currentIndexChanged.connect(self.showQuantity)
        self.fill(obj)

    def fill(self, obj):
        if obj is not None and obj.Source is not None:
            self.sourceCombo.setCurrentIndex(max(0, self.sourceCombo.findData(obj.Source)))
        self.fillQuantities()
        if obj is not None:
            self.quantityCombo.setCurrentIndex(max(0, self.quantityCombo.findData(obj.Quantity)))
            self.componentCombo.setCurrentIndex(
                max(0, self.componentCombo.findData(obj.Component))
            )
            self.showQuantity()
            self.compareCombo.setCurrentIndex(max(0, self.compareCombo.findData(obj.Compare)))
            self.actionCombo.setCurrentIndex(max(0, self.actionCombo.findData(obj.Action)))
            self.thresholdBox.setValue(obj.Threshold.Value * thresholdFactor(obj.Quantity))
            self.countBox.setValue(obj.Count)
            for combo, part in zip(self.pairCombos, sensorPair(obj)):
                combo.setCurrentIndex(max(0, combo.findData(part)))
        self.showQuantity()

    def fillPair(self):
        """The components the source's pairs are of; any, first"""
        source = self.sourceCombo.currentData()
        parts = []
        if sensorSourceKind(source) in ("Contact", "CamContact"):
            parts = contactComponents(source)
            if not parts:
                parts = UtilsAssembly.getMovablePartsWithin(self.assembly)
        for combo in self.pairCombos:
            current = combo.currentData()
            combo.clear()
            combo.addItem(translate("Assembly", "Any component"), None)
            for part in parts:
                combo.addItem(part.Label, part)
            combo.setCurrentIndex(max(0, combo.findData(current)))

    def fillQuantities(self):
        self.fillPair()
        current = self.quantityCombo.currentData()
        self.quantityCombo.blockSignals(True)
        self.quantityCombo.clear()
        kind = sensorSourceKind(self.sourceCombo.currentData())
        for quantity in SensorQuantities.get(kind, ()):
            label = ResultQuantities.get(quantity, (quantity, 1.0, ""))[0]
            self.quantityCombo.addItem(translate("Assembly", label), quantity)
        self.quantityCombo.setCurrentIndex(max(0, self.quantityCombo.findData(current)))
        self.quantityCombo.blockSignals(False)
        self.showQuantity()

    def showQuantity(self):
        quantity = self.quantityCombo.currentData()
        vector = quantity in SensorVectors
        self.componentLabel.setEnabled(vector)
        self.componentCombo.setEnabled(vector)
        # An event's speed is at or above the threshold, or below it; a
        # value's can cross it either way
        events = quantity in SensorEvents
        if events:
            self.compareCombo.setItemText(0, translate("Assembly", "Speed at least"))
            self.compareCombo.setItemText(1, translate("Assembly", "Speed below"))
            if self.compareCombo.count() > 2:
                crossing = self.compareCombo.currentIndex() == 2
                self.compareCombo.removeItem(2)
                if crossing:
                    self.compareCombo.setCurrentIndex(0)
        else:
            self.compareCombo.setItemText(0, translate("Assembly", "Rises above"))
            self.compareCombo.setItemText(1, translate("Assembly", "Falls below"))
            if self.compareCombo.count() < 3:
                self.compareCombo.addItem(translate("Assembly", "Crosses"), "Crosses")
        self.pairLabel.setEnabled(events)
        for combo in self.pairCombos:
            combo.setEnabled(events)
        unit = ResultQuantities.get(quantity, ("", 1.0, ""))[2]
        self.thresholdBox.setSuffix(" " + unit if unit else "")
        self.okButton.setEnabled(quantity is not None)

    def apply(self, obj):
        quantity = self.quantityCombo.currentData()
        obj.Source = self.sourceCombo.currentData()
        obj.Quantity = quantity
        obj.Component = self.componentCombo.currentData()
        obj.Compare = self.compareCombo.currentData()
        obj.Action = self.actionCombo.currentData()
        obj.Threshold = self.thresholdBox.value() / thresholdFactor(quantity)
        obj.Count = self.countBox.value()
        pair = []
        if quantity in SensorEvents:
            for combo in self.pairCombos:
                part = combo.currentData()
                if part is not None and part not in pair:
                    pair.append(part)
        obj.Pair = pair
        describeSensor(obj)

    def exec_(self):
        return self.dialog.exec()


######### Create Simulation Task ###########
class TaskAssemblyCreateSimulation(QtCore.QObject):
    def __init__(self, simFeaturePy=None):
        super().__init__()
        Gui.Selection.clearSelection()

        self.assembly = UtilsAssembly.activeAssembly()

        self.initialPlcs = UtilsAssembly.saveAssemblyPartsPlacements(self.assembly)

        self.doc = self.assembly.Document
        self.gui_doc = Gui.getDocument(self.doc)

        self.view = self.gui_doc.activeView()

        if not self.assembly or not self.view or not self.doc:
            return

        self.runKinematicsTimer = QtCore.QTimer()
        self.runKinematicsTimer.setSingleShot(True)
        self.runKinematicsTimer.timeout.connect(self.displayLastFrame)

        self.animationTimer = QtCore.QTimer()
        self.animationTimer.setInterval(50)  # ms
        self.animationTimer.timeout.connect(self.playAnimation)

        self.form = Gui.PySideUic.loadUi(":/panels/TaskAssemblyCreateSimulation.ui")
        self.form.motionList.installEventFilter(self)
        self.setSpinboxPrecision(self.form.TimeStartSpinBox, 9)
        self.setSpinboxPrecision(self.form.TimeEndSpinBox, 9)
        self.setSpinboxPrecision(self.form.TimeStepOutputSpinBox, 9)
        self.setSpinboxPrecision(self.form.GlobalErrorToleranceSpinBox, 9, App.Units.Length)
        self.form.motionList.itemDoubleClicked.connect(self.onItemDoubleClicked)
        self.form.TimeStartSpinBox.valueChanged.connect(self.onTimeStartChanged)
        self.form.TimeEndSpinBox.valueChanged.connect(self.onTimeEndChanged)
        self.form.TimeStepOutputSpinBox.valueChanged.connect(self.onTimeStepOutputChanged)
        self.form.GlobalErrorToleranceSpinBox.valueChanged.connect(
            self.onGlobalErrorToleranceChanged
        )
        self.form.RunKinematicsButton.clicked.connect(self.runKinematics)
        self.form.frameSlider.valueChanged.connect(self.onFrameChanged)
        self.form.FramesPerSecondSpinBox.valueChanged.connect(self.onFramesPerSecondChanged)
        self.form.PlayBackwardButton.clicked.connect(self.animationTimerStartBackward)
        self.form.PlayForwardButton.clicked.connect(self.animationTimerStartForward)
        self.form.StepBackwardButton.clicked.connect(self.stepBackward)
        self.form.StepForwardButton.clicked.connect(self.stepForward)
        self.form.StopButton.clicked.connect(self.stopAnimation)
        self.form.AddButton.clicked.connect(self.addMotionClicked)
        # Not upstream: a Dynamic study's initial velocities, listed with the
        # motions
        self.addInitialButton = QtWidgets.QToolButton()
        self.addInitialButton.setText(translate("Assembly", "v0"))
        self.addInitialButton.setToolTip(
            translate("Assembly", "Add an initial velocity: a joint's rate or a component's")
        )
        self.addInitialButton.clicked.connect(self.addInitialClicked)
        # Not upstream: a point's path over the run (SimulationTrace)
        self.addTraceButton = QtWidgets.QToolButton()
        self.addTraceButton.setText(translate("Assembly", "Trace"))
        self.addTraceButton.setToolTip(
            translate(
                "Assembly",
                "Trace the path of a point over the run: select a vertex of a part first",
            )
        )
        self.addTraceButton.clicked.connect(self.addTraceClicked)
        # Not upstream: a value watched, which stops the run or marks the time
        self.addSensorButton = QtWidgets.QToolButton()
        self.addSensorButton.setIcon(QIcon(":/icons/Assembly_Sensor.svg"))
        self.addSensorButton.setToolTip(
            translate(
                "Assembly",
                "Add a sensor: a reaction, effort, spring or contact force watched over a "
                "Dynamic or Static run, which stops it or marks the time where it passes a "
                "threshold, or a contact made or let go",
            )
        )
        self.addSensorButton.clicked.connect(self.addSensorClicked)
        buttons = self.form.findChild(QtWidgets.QVBoxLayout, "motion_buttonsLayout")
        if buttons is not None:
            buttons.insertWidget(1, self.addInitialButton)
            buttons.insertWidget(2, self.addTraceButton)
            buttons.insertWidget(3, self.addSensorButton)
        self.form.RemoveButton.clicked.connect(self.deleteSelectedMotions)
        self.form.groupBox_player.hide()
        self.form.SaveAnimationButton.clicked.connect(self.saveAnimation)
        self.form.SaveAnimationButton.hide()
        self.addAnalysisWidgets()

        if simFeaturePy:
            self.simFeaturePy = simFeaturePy
            Gui.ActiveDocument.openCommand("Edit " + simFeaturePy.Label + " Simulation")
            self.onMotionsChanged()
            self.creating = False
        else:
            Gui.ActiveDocument.openCommand("Create Simulation")
            self.createSimulationObject()
            self.creating = True

        self.setUiInitialValues()
        self.setAnalysisInitialValues()

        self.simFeaturePy.Proxy.setMotionsChangedCallback(self.onMotionsChanged)
        self.simFeaturePy.Proxy.resultsStaleCallback = self.showStale
        self.showStale()

        self.currentFrm = 1
        self.startFrm = 1
        self.endFrm = 100
        self.fps = 30
        self.deltaTime = 1.0 / self.fps
        self.startTime = time.time()
        self.index = 0

        # Not upstream: the study's results, kept by the assembly or in the
        # document (docs/AssemblyDynamics.md sec 8.4), shown again
        if not self.creating and self.assembly.loadSimulationResults(self.simFeaturePy):
            self.showRun(fresh=False)

    @property
    def simValues(self):
        """Not upstream: the simulation's values by the C++ one's names"""
        return SimulationValues(self.simFeaturePy)

    def setUiInitialValues(self):
        self.form.TimeStartSpinBox.setProperty("rawValue", self.simValues.TimeStart.Value)
        self.form.TimeEndSpinBox.setProperty("rawValue", self.simValues.TimeEnd.Value)
        self.form.TimeStepOutputSpinBox.setProperty(
            "rawValue", self.simValues.TimeStepOutput.Value
        )
        self.form.GlobalErrorToleranceSpinBox.setProperty(
            "rawValue", self.simValues.GlobalErrorTolerance
        )
        self.form.FramesPerSecondSpinBox.setValue(self.simValues.FramesPerSecond)

    def addAnalysisWidgets(self):
        """Not upstream: the analysis, Kinematic or Dynamic, and gravity
        (docs/AssemblyDynamics.md sec 9), above the motions; and a line saying
        why a run failed, under Generate."""
        box = QtWidgets.QGroupBox(translate("Assembly", "Analysis"))
        row = QtWidgets.QHBoxLayout(box)
        self.analysisCombo = QtWidgets.QComboBox()
        self.analysisCombo.addItem(translate("Assembly", "Kinematic"), "Kinematic")
        self.analysisCombo.addItem(translate("Assembly", "Dynamic"), "Dynamic")
        self.analysisCombo.addItem(translate("Assembly", "Static"), "Static")
        self.analysisCombo.setToolTip(
            translate(
                "Assembly",
                "Kinematic: the motions drive the joints.\n"
                "Dynamic: the parts move under gravity by their masses (Mass Properties).\n"
                "Static: where the parts settle under those loads, at each output time.",
            )
        )
        row.addWidget(self.analysisCombo, 1)
        self.reactionsCheck = QtWidgets.QCheckBox(translate("Assembly", "Reactions"))
        self.reactionsCheck.setToolTip(
            translate(
                "Assembly",
                "Kinematic: also the joints' reactions and the motions' efforts the motion\n"
                "takes, from the masses, gravity and the elements ticked. The motions must\n"
                "drive every freedom.",
            )
        )
        row.addWidget(self.reactionsCheck)
        self.gravityCheck = QtWidgets.QCheckBox(translate("Assembly", "Gravity"))
        self.gravityCheck.setToolTip(
            translate("Assembly", "Dynamic: gravity acts, along the simulation's Gravity")
        )
        row.addWidget(self.gravityCheck)
        self.restitutionSpin = QtWidgets.QDoubleSpinBox()
        self.restitutionSpin.setRange(0.0, 1.0)
        self.restitutionSpin.setSingleStep(0.05)
        self.restitutionSpin.setDecimals(2)
        self.restitutionSpin.setPrefix(translate("Assembly", "Restitution") + " ")
        self.restitutionSpin.setToolTip(
            translate(
                "Assembly",
                "Dynamic: the joints' limits are rigid stops. 0: a part stays at the stop;\n"
                "1: it bounces back as fast as it came.",
            )
        )
        row.addWidget(self.restitutionSpin)
        self.form.layout().insertWidget(0, box)
        # The Physics group's force elements, those ticked used by the study
        self.elementsBox = QtWidgets.QGroupBox(translate("Assembly", "Elements"))
        elements = QtWidgets.QVBoxLayout(self.elementsBox)
        self.elementsList = QtWidgets.QListWidget()
        self.elementsList.setMaximumHeight(90)
        self.elementsList.setToolTip(
            translate(
                "Assembly",
                "The springs and dampers of the assembly's Physics group; a dynamic study "
                "uses those ticked",
            )
        )
        elements.addWidget(self.elementsList)
        self.form.layout().insertWidget(1, self.elementsBox)
        self.elementsList.itemChanged.connect(self.onElementTicked)
        # What a run keeps in the document (docs/AssemblyDynamics.md sec 8.4)
        self.keepRow = QtWidgets.QWidget()
        keep = QtWidgets.QHBoxLayout(self.keepRow)
        keep.setContentsMargins(0, 0, 0, 0)
        keep.addWidget(QtWidgets.QLabel(translate("Assembly", "Keep in the document")))
        self.keepCombo = QComboBox()
        for name, text in (
            ("None", translate("Assembly", "Nothing")),
            ("Motion", translate("Assembly", "The motion and the events")),
            ("All", translate("Assembly", "Everything, the plots' values too")),
        ):
            self.keepCombo.addItem(text, name)
        self.keepCombo.setToolTip(
            translate(
                "Assembly",
                "What a run keeps in the document, saved with it and shown again when the\n"
                "study is opened. Nothing: the results last until another study runs.",
            )
        )
        keep.addWidget(self.keepCombo, 1)
        self.form.layout().insertWidget(2, self.keepRow)
        self.keepCombo.currentIndexChanged.connect(self.onKeepChanged)
        self.runMessage = QtWidgets.QLabel()
        self.runMessage.setWordWrap(True)
        self.runMessage.setStyleSheet("color: #c80000")
        self.runMessage.hide()
        self.form.layout().addWidget(self.runMessage)
        # The sensors that tripped in the run shown
        self.eventMessage = QtWidgets.QLabel()
        self.eventMessage.setWordWrap(True)
        self.eventMessage.hide()
        self.form.layout().addWidget(self.eventMessage)
        # A run going: how far it got, and a Cancel
        self.runRow = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(self.runRow)
        row.setContentsMargins(0, 0, 0, 0)
        self.runProgress = QtWidgets.QProgressBar()
        self.runProgress.setRange(0, 1000)
        self.runProgress.setTextVisible(False)
        row.addWidget(self.runProgress, 1)
        self.runCancelButton = QPushButton(translate("Assembly", "Cancel"))
        self.runCancelButton.setToolTip(
            translate("Assembly", "Stop the simulation; it keeps no frames")
        )
        self.runCancelButton.clicked.connect(self.cancelRun)
        row.addWidget(self.runCancelButton)
        self.runRow.hide()
        self.form.layout().addWidget(self.runRow)
        self.runPoll = QtCore.QTimer()
        self.runPoll.setInterval(50)
        self.runPoll.timeout.connect(self.pollRun)
        # The stale flag (docs/AssemblyDynamics.md sec 8.4)
        self.staleMessage = QtWidgets.QLabel(
            translate(
                "Assembly",
                "The results are of an earlier version of the assembly or of this study: "
                "generate them again.",
            )
        )
        self.staleMessage.setWordWrap(True)
        self.staleMessage.setStyleSheet("color: #c86400")
        self.staleMessage.hide()
        self.form.layout().addWidget(self.staleMessage)
        # What a dynamic run reports with its frames (sec 8.4)
        self.resultsBox = QtWidgets.QGroupBox(translate("Assembly", "Results"))
        results = QtWidgets.QVBoxLayout(self.resultsBox)
        self.resultCombo = QtWidgets.QComboBox()
        self.resultCombo.setToolTip(
            translate(
                "Assembly",
                "What the run reports with each frame: a joint's reaction on the part of its\n"
                "first reference (in the assembly's axes, the torque about the joint's\n"
                "coordinate system on that part), a motion's motor effort, a spring's force\n"
                "(positive pushing its ends apart) and length",
            )
        )
        results.addWidget(self.resultCombo)
        self.resultPlot = ResultPlot()
        results.addWidget(self.resultPlot)
        self.resultValue = QtWidgets.QLabel()
        self.resultValue.setWordWrap(True)
        self.resultValue.setTextInteractionFlags(Qt.TextSelectableByMouse)
        results.addWidget(self.resultValue)
        exports = QtWidgets.QHBoxLayout()
        self.csvButton = QPushButton(translate("Assembly", "Export CSV..."))
        self.csvButton.setToolTip(
            translate("Assembly", "Every value of the run, a row per frame, to a CSV file")
        )
        self.csvButton.clicked.connect(self.exportCsv)
        exports.addWidget(self.csvButton)
        self.sheetButton = QPushButton(translate("Assembly", "To Spreadsheet"))
        self.sheetButton.setToolTip(
            translate("Assembly", "Every value of the run, a row per frame, in a new spreadsheet")
        )
        self.sheetButton.clicked.connect(self.exportSheet)
        exports.addWidget(self.sheetButton)
        exports.addStretch(1)
        results.addLayout(exports)
        self.resultsBox.hide()
        self.form.layout().addWidget(self.resultsBox)
        self.resultCombo.currentIndexChanged.connect(self.showResult)
        self.results = None
        self.analysisCombo.currentIndexChanged.connect(self.onAnalysisChanged)
        self.gravityCheck.toggled.connect(self.onGravityChanged)
        self.reactionsCheck.toggled.connect(self.onReactionsChanged)
        self.restitutionSpin.valueChanged.connect(self.onRestitutionChanged)

    def showStale(self):
        """Not upstream: whether the run kept for this study is stale"""
        self.staleMessage.setVisible(getattr(self.simFeaturePy, "ResultsStale", False))

    def hasAnalysis(self):
        """Upstream's Python simulation, not converted, has none"""
        return hasattr(self.simFeaturePy, "AnalysisType")

    def setAnalysisInitialValues(self):
        if not self.hasAnalysis():
            self.analysisCombo.setEnabled(False)
            self.gravityCheck.setEnabled(False)
            self.reactionsCheck.hide()
            self.restitutionSpin.setEnabled(False)
            self.elementsBox.hide()
            self.keepRow.hide()
            return
        self.keepCombo.blockSignals(True)
        self.keepCombo.setCurrentIndex(
            max(0, self.keepCombo.findData(getattr(self.simFeaturePy, "KeepResults", "Motion")))
        )
        self.keepCombo.blockSignals(False)
        self.analysisCombo.blockSignals(True)
        self.analysisCombo.setCurrentIndex(
            max(0, self.analysisCombo.findData(self.simFeaturePy.AnalysisType))
        )
        self.analysisCombo.blockSignals(False)
        self.gravityCheck.blockSignals(True)
        self.gravityCheck.setChecked(self.simFeaturePy.GravityEnabled)
        self.gravityCheck.blockSignals(False)
        self.reactionsCheck.blockSignals(True)
        self.reactionsCheck.setChecked(getattr(self.simFeaturePy, "Reactions", False))
        self.reactionsCheck.blockSignals(False)
        self.restitutionSpin.blockSignals(True)
        self.restitutionSpin.setValue(getattr(self.simFeaturePy, "Restitution", 0.0))
        self.restitutionSpin.blockSignals(False)
        self.fillElements()
        self.showAnalysis()

    def forceElements(self):
        """The force elements of the assembly's Physics group"""
        return [
            o
            for o in self.assembly.Group
            if o.isDerivedFrom("Assembly::SpringDamper")
            or o.isDerivedFrom("Assembly::TorsionSpring")
            or o.isDerivedFrom("Assembly::JointStop")
            or o.isDerivedFrom("Assembly::JointFriction")
            or o.isDerivedFrom("Assembly::FramedItem")
            or o.isDerivedFrom("Assembly::ContactSet")
        ]

    def fillElements(self):
        used = list(self.simFeaturePy.Elements)
        # A new study starts with every element ticked (sec 5.1)
        if self.creating and not used:
            used = self.forceElements()
            self.simFeaturePy.Elements = used
        self.elementsList.blockSignals(True)
        self.elementsList.clear()
        for element in self.forceElements():
            item = QtWidgets.QListWidgetItem(element.Label)
            item.setData(QtCore.Qt.UserRole, element.Name)
            item.setFlags(item.flags() | QtCore.Qt.ItemIsUserCheckable)
            item.setCheckState(QtCore.Qt.Checked if element in used else QtCore.Qt.Unchecked)
            self.elementsList.addItem(item)
        self.elementsList.blockSignals(False)

    def onElementTicked(self, item):
        used = [
            self.assembly.Document.getObject(self.elementsList.item(i).data(QtCore.Qt.UserRole))
            for i in range(self.elementsList.count())
            if self.elementsList.item(i).checkState() == QtCore.Qt.Checked
        ]
        self.simFeaturePy.Elements = [o for o in used if o is not None]

    def showAnalysis(self):
        # Static takes the masses, gravity and the elements as Dynamic does,
        # and so does a kinematic run with its reactions
        kinematic = self.analysisCombo.currentData() == "Kinematic"
        self.reactionsCheck.setVisible(kinematic and hasattr(self.simFeaturePy, "Reactions"))
        dynamic = not kinematic or (
            hasattr(self.simFeaturePy, "Reactions") and self.reactionsCheck.isChecked()
        )
        self.gravityCheck.setEnabled(dynamic)
        # A static run settles on its stops, never bouncing
        self.restitutionSpin.setEnabled(
            self.analysisCombo.currentData() == "Dynamic"
            and hasattr(self.simFeaturePy, "Restitution")
        )
        self.elementsBox.setVisible(dynamic and self.hasAnalysis())
        self.addInitialButton.setVisible(
            self.analysisCombo.currentData() == "Dynamic" and self.hasAnalysis()
        )

    def onAnalysisChanged(self, index):
        if self.hasAnalysis():
            self.simFeaturePy.AnalysisType = self.analysisCombo.currentData()
        self.showAnalysis()

    def onReactionsChanged(self, checked):
        if hasattr(self.simFeaturePy, "Reactions"):
            self.simFeaturePy.Reactions = checked
        self.showAnalysis()

    def onGravityChanged(self, checked):
        if self.hasAnalysis():
            self.simFeaturePy.GravityEnabled = checked

    def onRestitutionChanged(self, value):
        if hasattr(self.simFeaturePy, "Restitution"):
            self.simFeaturePy.Restitution = value

    def onKeepChanged(self):
        """Not upstream: what the next run keeps"""
        if hasattr(self.simFeaturePy, "KeepResults"):
            self.simFeaturePy.KeepResults = self.keepCombo.currentData()

    def setSpinboxPrecision(self, spinbox, precision, unit=App.Units.TimeSpan):
        q = App.Units.Quantity()
        q.Unit = unit
        q.Format = {"Precision": precision}
        spinbox.setProperty("value", q)

    def accept(self):
        self.deactivate()
        UtilsAssembly.restoreAssemblyPartsPlacements(self.assembly, self.initialPlcs)
        Gui.ActiveDocument.commitCommand()
        return True

    def reject(self):
        self.deactivate()
        Gui.ActiveDocument.abortCommand()
        return True

    def deactivate(self):
        self.animationTimer.stop()
        # A run going is let go of
        if self.runPoll.isActive():
            self.endRun()
            self.assembly.cancelSimulation()
            self.assembly.finishSimulation()
        self.simFeaturePy.Proxy.setMotionsChangedCallback(None)
        self.simFeaturePy.Proxy.resultsStaleCallback = None
        if Gui.Control.activeDialog():
            Gui.Control.closeDialog()

    def onTimeStartChanged(self, quantity):
        self.simValues.TimeStart = self.form.TimeStartSpinBox.property("rawValue")

    def onTimeEndChanged(self, quantity):
        self.simValues.TimeEnd = self.form.TimeEndSpinBox.property("rawValue")

    def onTimeStepOutputChanged(self, quantity):
        self.simValues.TimeStepOutput = self.form.TimeStepOutputSpinBox.property("rawValue")

    def onGlobalErrorToleranceChanged(self, quantity):
        self.simValues.GlobalErrorTolerance = self.form.GlobalErrorToleranceSpinBox.property(
            "rawValue"
        )

    def onItemDoubleClicked(self, item):
        row = self.form.motionList.row(item)
        if row < len(self.simFeaturePy.Group):
            motion = self.simFeaturePy.Group[row]
            # Not upstream: a trace has no dialog
            proxy = getattr(motion.ViewObject, "Proxy", None)
            if hasattr(proxy, "openEditDialog"):
                proxy.openEditDialog()
            self.onMotionsChanged()

    def createSimulationObject(self):
        sim_group = UtilsAssembly.getSimulationGroup(self.assembly)
        # Not upstream: the C++ simulation (docs/AssemblyObjectModel.md sec 7.2)
        self.simFeaturePy = sim_group.newObject("Assembly::SimulationPython", "Simulation")
        Simulation(self.simFeaturePy)
        ViewProviderSimulation(self.simFeaturePy.ViewObject)

    def createMotionObject(self, motionType, joint, formula, control=MotionControls[0]):
        motion = self.assembly.newObject("Assembly::MotionPython", "Motion")
        Motion(motion, motionType, joint, formula)
        motion.Control = control
        ViewProviderMotion(motion.ViewObject)

        listOfMotions = self.simFeaturePy.Group
        listOfMotions.append(motion)
        self.simFeaturePy.Group = listOfMotions

    def onMotionsChanged(self):
        self.form.motionList.clear()
        for motion in self.simFeaturePy.Group:
            self.form.motionList.addItem(motion.Label)

    def runKinematics(self):
        # Not upstream: a run that fails says so, here as in the report; it
        # runs on a worker thread (docs/AssemblyDynamics.md sec 4), the panel
        # showing how far it got, with a Cancel
        self.runMessage.hide()
        self.eventMessage.hide()
        self.results = None
        self.resultsBox.hide()
        if self.runPoll.isActive():
            return
        if self.assembly.startSimulation(self.simFeaturePy) != 0:
            self.showRunFailed()
            return
        self.form.RunKinematicsButton.setEnabled(False)
        self.runProgress.setValue(0)
        self.runRow.show()
        self.runPoll.start()

    def showRunFailed(self, cancelled=False):
        if cancelled:
            text = translate("Assembly", "The simulation was cancelled.")
        else:
            text = translate(
                "Assembly", "The simulation was not generated: the report view says why."
            )
        self.runMessage.setText(text)
        self.runMessage.show()

    def pollRun(self):
        """Not upstream: the worker's progress, and its frames once it is done"""
        progress = self.assembly.getSimulationProgress()
        span = self.simValues.TimeEnd - self.simValues.TimeStart
        done = (progress["Time"] - self.simValues.TimeStart) / span if span > 0 else 1.0
        self.runProgress.setValue(int(1000 * min(max(done, 0.0), 1.0)))
        if not progress["Done"]:
            return
        self.endRun()
        code = self.assembly.finishSimulation()
        if code != 0:
            self.showRunFailed(cancelled=code == -7)
            return
        self.showRun()

    def endRun(self):
        self.runPoll.stop()
        self.runRow.hide()
        self.form.RunKinematicsButton.setEnabled(True)

    def cancelRun(self):
        self.assembly.cancelSimulation()

    def showRun(self, fresh=True):
        """The frames of the run just taken in: the traces, the results and
        the player, at the last frame. Not fresh, the results the study kept,
        shown again when it is opened: the traces as they are, the player at
        the first frame"""
        nFrms = self.assembly.numberOfFrames()
        if fresh:
            # Not upstream: the study's traces follow its run
            import SimulationTrace

            SimulationTrace.updateTraces(self.simFeaturePy)
        self.fillResults()
        self.showEvents()
        if not fresh and not self.results["Channels"]:
            # Kept without the plots' values
            if (
                getattr(self.simFeaturePy, "KeepResults", "") == "Motion"
                and getattr(self.simFeaturePy, "AnalysisType", "Kinematic") != "Kinematic"
            ):
                self.runMessage.setText(
                    translate(
                        "Assembly",
                        "The plots' values were not kept, only the motion: generate the "
                        "results again for them, or keep everything.",
                    )
                )
                self.runMessage.show()
        self.form.frameSlider.setMaximum(nFrms - 1)
        if fresh:
            self.setFrameValue(nFrms - 1)
        else:
            # The start, which setFrameValue() does not go to
            self.form.frameSlider.setValue(0)
        # Not upstream: shown even when the slider stays where it was -- a
        # single frame (a Static study at one time) is frame 0, as before
        self.onFrameChanged(self.form.frameSlider.value())
        self.form.groupBox_player.show()
        self.form.SaveAnimationButton.show()

    def fillResults(self):
        """Not upstream: the run's channels in the Results box, which shows
        when it has any"""
        self.results = self.assembly.getSimulationResults()
        current = self.resultCombo.currentData()
        self.resultCombo.blockSignals(True)
        self.resultCombo.clear()
        for k, channel in enumerate(self.results["Channels"]):
            obj = channel["Object"]
            name = obj.Label if obj is not None else channel["Name"]
            quantity = ResultQuantities.get(channel["Quantity"], (channel["Quantity"], 1.0, ""))
            key = (channel["Name"], channel["Quantity"])
            text = "%s: %s" % (name, translate("Assembly", quantity[0]))
            if not channel.get("Unique", True):
                # A redundant joint's: one split of the load among the
                # joints that share it
                text += " " + translate("Assembly", "(not unique)")
            self.resultCombo.addItem(text, key)
            if not channel.get("Unique", True):
                self.resultCombo.setItemData(
                    self.resultCombo.count() - 1,
                    translate(
                        "Assembly",
                        "The joint is redundant: other joints hold what some of its "
                        "equations hold, and the run took those out. This is one split "
                        "of the load among them, not the only one.",
                    ),
                    QtCore.Qt.ToolTipRole,
                )
        at = self.resultCombo.findData(current) if current is not None else -1
        self.resultCombo.setCurrentIndex(max(0, at))
        self.resultCombo.blockSignals(False)
        self.resultsBox.setVisible(self.resultCombo.count() > 0)
        self.showResult()

    def exportCsv(self):
        path, _ = QFileDialog.getSaveFileName(
            self.form,
            translate("Assembly", "Export the results"),
            "",
            translate("Assembly", "CSV files") + " (*.csv)",
        )
        if not path:
            return
        if not path.lower().endswith(".csv"):
            path += ".csv"
        exportResultsCsv(self.assembly, path)

    def exportSheet(self):
        exportResultsSheet(self.assembly)

    def resultCurves(self):
        """The chosen channel's curves, in the shown unit: a vector's X, Y, Z
        and its length; and the unit"""
        if not self.results:
            return [], ""
        key = self.resultCombo.currentData()
        for channel in self.results["Channels"]:
            if (channel["Name"], channel["Quantity"]) != key:
                continue
            _, factor, unit = ResultQuantities.get(channel["Quantity"], ("", 1.0, ""))
            values = channel["Values"]
            if values and isinstance(values[0], App.Vector):
                return [
                    ("X", [v.x * factor for v in values]),
                    ("Y", [v.y * factor for v in values]),
                    ("Z", [v.z * factor for v in values]),
                    ("|%s|" % translate("Assembly", "total"), [v.Length * factor for v in values]),
                ], unit
            return [("", [v * factor for v in values])], unit
        return [], ""

    def showResult(self, index=None):
        curves, unit = self.resultCurves()
        times = self.results["Times"] if self.results else []
        self.resultPlot.setCurves(times, curves, unit)
        self.showResultValue()

    def showResultValue(self):
        frame = self.form.frameSlider.value()
        self.resultPlot.setFrame(frame)
        curves, unit = self.resultCurves()
        times = self.results["Times"] if self.results else []
        if not curves or frame >= len(times):
            self.resultValue.clear()
            return
        parts = []
        for name, values in curves:
            if frame < len(values):
                value = f"{values[frame]:.6g} {unit}"
                parts.append(f"{name} {value}" if name else value)
        self.resultValue.setText(f"{times[frame]:g} s: " + ", ".join(parts))

    def onFrameChanged(self, val):
        self.assembly.updateForFrame(val)
        self.form.FrameLabel.setText(translate("Assembly", "Frame" + " " + str(val)))
        time = float(val * self.simValues.TimeStepOutput)
        self.form.FrameTimeLabel.setText(f"{time:.2f} s")
        if self.results:
            self.showResultValue()

    def onFramesPerSecondChanged(self):
        self.simValues.FramesPerSecond = self.form.FramesPerSecondSpinBox.value()

    def playBackward(self):
        pass

    def animationTimerStartForward(self):
        self.direction = 1
        self.animationTimerStart()

    def animationTimerStartBackward(self):
        self.direction = -1
        self.animationTimerStart()

    def animationTimerStart(self):
        self.animationTimer.stop()
        self.currentFrm = self.form.frameSlider.value()
        self.startFrm = 1
        self.endFrm = self.form.frameSlider.maximum()
        if self.startFrm >= self.endFrm:
            return

        self.fps = self.simValues.FramesPerSecond
        self.deltaTime = 1.0 / self.fps
        self.startTime = time.time()
        self.index = self.currentFrm
        self.animationTimer.setInterval(self.deltaTime * 1000)  # ms
        self.animationTimer.start()

    def playAnimation(self):
        range_ = self.endFrm - self.startFrm
        offset = self.currentFrm - self.startFrm
        count = int((time.time() - self.startTime) / self.deltaTime)
        self.index = ((self.direction * count + offset) % range_) + self.startFrm
        self.setFrameValue(self.index)

    def displayLastFrame(self):
        nFrms = self.assembly.numberOfFrames()
        self.setFrameValue(nFrms - 1)

    def stepBackward(self):
        self.animationTimer.stop()

        nextFrm = self.form.frameSlider.value() - 1
        if nextFrm < 1:
            nextFrm = self.form.frameSlider.maximum()  # wraparound
        self.setFrameValue(nextFrm)

    def stepForward(self):
        self.animationTimer.stop()

        nextFrm = self.form.frameSlider.value() + 1
        if nextFrm > self.form.frameSlider.maximum():
            nextFrm = 1  # wraparound
        self.setFrameValue(nextFrm)

    def setFrameValue(self, val):
        if val < 1:
            val = 1
        if val > self.form.frameSlider.maximum():
            val = self.form.frameSlider.maximum()

        self.form.frameSlider.setValue(val)

    def stopAnimation(self):
        self.animationTimer.stop()

    def addTraceClicked(self):
        """A trace of the selected point: a vertex of a component, or where
        a pick hit one"""
        import SimulationTrace

        picked = selectedPoint(self.assembly)
        if picked is None:
            QMessageBox.information(
                self.form,
                translate("Assembly", "Trace"),
                translate("Assembly", "Select a vertex of a part to trace its path."),
            )
            return
        part, point = picked
        SimulationTrace.newTrace(self.simFeaturePy, part, point)
        self.onMotionsChanged()

    def showEvents(self):
        """Not upstream: the sensors that tripped and when, by their labels,
        which say what they watch and where they trip; the one that stopped
        the run last. And the contacts made and let go, each pair's
        components and its speed in or out, from the first, in the lines the
        sensors leave"""
        events = (self.results or {}).get("Events", [])
        if not events:
            self.eventMessage.hide()
            return
        lines = []
        shown = 8
        # The sensors' first, the contacts' from the start in what lines are
        # left: a stop late in a run of bounces is still shown
        sensors = [i for i, e in enumerate(events) if e.get("Kind", "Sensor") == "Sensor"]
        contacts = [i for i, e in enumerate(events) if e.get("Kind", "Sensor") != "Sensor"]
        picked = sorted(sensors[:shown] + contacts[: max(0, shown - len(sensors))])
        for i in picked:
            event = events[i]
            obj = event["Object"]
            name = obj.Label if obj is not None else event["Name"]
            time = "%.6g" % event["Time"]
            kind = event.get("Kind", "Sensor")
            if kind != "Sensor":
                a, b = (
                    o.Label if o is not None else translate("Assembly", "the ground")
                    for o in event["Bodies"]
                )
                speed = App.Units.Quantity(event["Value"], App.Units.Velocity).UserString
                if kind == "ContactBegin":
                    text = translate("Assembly", "Contact at {} s: {} and {}, in at {} ({})")
                else:
                    text = translate("Assembly", "Let go at {} s: {} and {}, out at {} ({})")
                lines.append(text.format(time, a, b, speed, name))
                continue
            if event["Stopped"]:
                text = translate("Assembly", "Stopped at {} s by {}")
            else:
                text = translate("Assembly", "Marked at {} s by {}")
            lines.append(text.format(time, name))
        if len(events) > len(picked):
            lines.append(translate("Assembly", "and {} more").format(len(events) - len(picked)))
        self.eventMessage.setText("\n".join(lines))
        self.eventMessage.show()

    def addSensorClicked(self):
        dialog = SensorDialog(self.assembly)
        if dialog.exec_():
            source = dialog.sourceCombo.currentData()
            quantity = dialog.quantityCombo.currentData()
            if source is None or quantity is None:
                return
            obj = newSensor(self.simFeaturePy, source, quantity, 0.0)
            dialog.apply(obj)
            self.onMotionsChanged()

    def addInitialClicked(self):
        dialog = InitialConditionDialog(self.assembly)
        if dialog.exec_():
            obj = newInitialCondition(self.simFeaturePy)
            dialog.apply(obj)
            self.onMotionsChanged()

    def addMotionClicked(self):
        dialog = MotionEditDialog(self.assembly)
        if dialog.exec_():
            self.createMotionObject(
                dialog.motionType, dialog.joint, dialog.formula, dialog.control
            )

    # Taskbox keyboard event handler
    def eventFilter(self, watched, event):
        if self.form is not None and watched == self.form.motionList:
            if event.type() == QtCore.QEvent.ShortcutOverride:
                if event.key() == QtCore.Qt.Key_Delete:
                    event.accept()
                    return True  # Indicate that the event has been handled
                return False

            elif event.type() == QtCore.QEvent.KeyPress:
                if event.key() == QtCore.Qt.Key_Delete:
                    self.deleteSelectedMotions()
                    return True  # Consume the event

        return super().eventFilter(watched, event)

    def deleteSelectedMotions(self):
        selected_indexes = self.form.motionList.selectedIndexes()
        sorted_indexes = sorted(selected_indexes, key=lambda x: x.row(), reverse=True)
        for index in sorted_indexes:
            row = index.row()
            if row < len(self.simFeaturePy.Group):
                motion = self.simFeaturePy.Group[row]
                # First remove the link from the viewObj
                self.simFeaturePy.Group.remove(motion)
                # Delete the object
                motion.Document.removeObject(motion.Name)

    def saveAnimation(self):
        num_frames = self.assembly.numberOfFrames()
        if num_frames <= 1:
            QMessageBox.warning(
                self.form,
                translate("Assembly", "Animation"),
                translate("Assembly", "Not enough frames to create an animation."),
            )
            return

        formats = {
            "MP4 Video": ".mp4",
            "Animated GIF": ".gif",
            "AVI Video": ".avi",
        }

        try:
            import av

            # 05/26 libvpx has no Windows conda package
            if "libvpx-vp9" in av.codecs_available:
                formats["WebM Video"] = ".webm"
        except ImportError:
            pass  # Error out later
        # Not upstream: the parts and their motion, for Blender and web viewers
        formats["Animated glTF, binary"] = ".glb"
        formats["Animated glTF"] = ".gltf"

        # Prompt user for file location and type
        file_path, selected_filter = QFileDialog.getSaveFileName(
            self.form,
            translate("Assembly", "Save Animation"),
            "",
            ";;".join(f"{k} (*{v})" for k, v in formats.items()),
        )

        if not file_path:
            return  # User cancelled

        if not Path(file_path).suffix:
            file_path += [v for v in formats.values() if v in selected_filter][0]
        if Path(file_path).suffix.lower() in (".glb", ".gltf"):
            self.saveGltf(file_path)
            return

        # Get parameters
        view = Gui.ActiveDocument.ActiveView
        width, height = view.getSize()

        # Ensure dimensions are even, as required by many video codecs
        width -= width % 2
        height -= height % 2

        fps = self.form.FramesPerSecondSpinBox.value()

        # Fail early when PIL is not installed
        try:
            from PIL import Image
        except ImportError:
            errMsg = translate(
                "Assembly", "Pillow (PIL) is not installed. It is required for video export."
            )
            QMessageBox.critical(self.form, "Error", errMsg)
            return

        # Setup temporary directory and progress bar
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            progress = QProgressDialog(
                translate("Assembly", "Generating Frames…"),
                translate("Assembly", "Cancel"),
                0,
                num_frames,
                self.form,
            )
            progress.setWindowModality(Qt.WindowModal)
            progress.show()

            original_frame = self.form.frameSlider.value()

            try:
                # Generate and save all frames as temporary images
                frame_files = []
                for i in range(num_frames):
                    progress.setValue(i)
                    if progress.wasCanceled():
                        App.Console.PrintMessage("Animation save cancelled.\n")
                        return

                    self.assembly.updateForFrame(i)
                    Gui.updateGui()  # Ensure the 3D view is redrawn

                    frame_filename = temp_path / f"frame_{i:05d}.png"
                    view.saveImage(str(frame_filename), width, height, "Current")
                    frame_files.append(str(frame_filename))

                # Assemble the final animation file
                progress.setLabelText(translate("Assembly", "Assembling animation…"))
                progress.setMaximum(0)  # Indeterminate progress

                success = False
                file_extension = Path(file_path).suffix.lower()
                if not file_extension:
                    file_extension = [
                        filter for filter in formats.values() if filter in selected_filter
                    ][0]
                    file_path += file_extension

                if file_extension == ".gif":
                    success = self.create_gif(file_path, frame_files, fps)
                elif file_extension in [".mp4", ".avi", ".webm"]:
                    success = self.create_video(file_path, frame_files, fps, (width, height))

                if success:
                    App.Console.PrintMessage(f"Animation successfully saved to {file_path}\n")

            except Exception as e:
                errMsg = (
                    translate("Assembly", "An error occurred while saving the animation")
                    + ": "
                    + str(e)
                )
                QMessageBox.critical(self.form, "Error", errMsg)
            finally:
                progress.close()
                # Restore original state
                self.assembly.updateForFrame(original_frame)
                self.form.frameSlider.setValue(original_frame)

    def saveGltf(self, file_path):
        """The frames as an animated glTF (SimulationGltf), the parts as
        the view shows them"""
        import SimulationGltf

        QGuiApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            done = SimulationGltf.exportGltf(self.assembly, file_path)
        except Exception as e:
            QGuiApplication.restoreOverrideCursor()
            QMessageBox.critical(
                self.form,
                "Error",
                translate("Assembly", "An error occurred while saving the animation")
                + ": "
                + str(e),
            )
            return
        QGuiApplication.restoreOverrideCursor()
        App.Console.PrintMessage(
            f"Animation saved to {file_path}: {done['Animated']} moving of "
            f"{done['Nodes']} nodes, {done['Frames']} frames\n"
        )

    def create_gif(self, output_path, frame_files, fps):
        """Creates an animated GIF from a list of image files using Pillow."""
        from PIL import Image

        pil_images = [Image.open(f) for f in frame_files]
        duration_ms = int(1000 / fps)
        pil_images[0].save(
            output_path,
            save_all=True,
            append_images=pil_images[1:],
            optimize=True,
            duration=duration_ms,
            loop=0,  # 0 means loop forever
        )
        return True

    def create_video(
        self, output_path: str, frame_files: list[str], fps: int, size: tuple[int, int]
    ):
        """Creates a video file from a list of image files using PyAV."""
        try:
            import av
        except ImportError:
            errMsg = translate(
                "Assembly", "PyAv is not installed. It is required for video export."
            )
            QMessageBox.critical(self.form, "Error", errMsg)
            return False

        file_extension = Path(output_path).suffix.lower()

        # Select codec based on file type
        with av.open(output_path, "w") as output:
            stream = None
            if file_extension == ".mp4":
                try:
                    stream = output.add_stream("libx264", fps)
                except av.codec.codec.UnknownCodecError:
                    stream = output.add_stream("mpeg4", fps)
            elif file_extension == ".avi":
                stream = output.add_stream("mpeg4", fps)
            elif file_extension == ".webm":
                stream = output.add_stream("libvpx-vp9", fps)
            else:
                errMsg = translate("Assembly", "Unknown video export format")
                QMessageBox.critical(self.form, "Error", errMsg)
                return False

            stream.width, stream.height = size

            from PIL import Image
            import numpy as np

            for file in frame_files:
                img = Image.open(file).convert("RGB")
                frame = av.VideoFrame.from_ndarray(np.array(img), format="rgb24")
                packet = stream.encode(frame)
                output.mux(packet)

            # Flush & write
            packet = stream.encode(None)
            output.mux(packet)

        return True


if App.GuiUp:
    Gui.addCommand("Assembly_CreateSimulation", CommandCreateSimulation())
