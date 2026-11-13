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

import bisect
import math
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
import SimulationPlayback

translate = App.Qt.translate

__title__ = "Assembly Command Create Simulation"
__author__ = "Ondsel"
__url__ = "https://www.freecad.org"

# Not upstream: where Save Animation's menu keeps what a video shows
VideoParams = "User parameter:BaseApp/Preferences/Mod/Assembly"

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
    # A moving part's: its centre of mass's acceleration, its turn and the
    # turn's rate, the inertia loads a stress analysis takes
    "Acceleration": (QT_TRANSLATE_NOOP("Assembly", "Acceleration"), 1e-3, "m/s^2"),
    "AngularVelocity": (
        QT_TRANSLATE_NOOP("Assembly", "Angular velocity"),
        57.29577951308232,
        "deg/s",
    ),
    "AngularAcceleration": (
        QT_TRANSLATE_NOOP("Assembly", "Angular acceleration"),
        57.29577951308232,
        "deg/s^2",
    ),
    # A part's stress analysis over frames of the run (MotionLoads.solveFrames):
    # its largest von Mises stress, at the frames solved
    "VonMises": (QT_TRANSLATE_NOOP("Assembly", "Largest von Mises stress"), 1.0, "MPa"),
    # And away from the loaded faces, about a section's depth off them
    "VonMisesAway": (
        QT_TRANSLATE_NOOP("Assembly", "Largest von Mises stress away from the loaded faces"),
        1.0,
        "MPa",
    ),
    # A flexible body's, recovered from its modes (FlexibleBodies): the largest
    # on its surface at each frame
    "FlexibleVonMises": (QT_TRANSLATE_NOOP("Assembly", "Largest von Mises stress"), 1.0, "MPa"),
    # A contact set's on each of its parts, the loads a stress analysis takes:
    # the force at the centre of its points there, that centre, and the
    # torque about it
    "PartContactForce": (QT_TRANSLATE_NOOP("Assembly", "Contact force on the part"), 1e-3, "N"),
    "PartContactPoint": (QT_TRANSLATE_NOOP("Assembly", "Contact point on the part"), 1.0, "mm"),
    "PartContactTorque": (QT_TRANSLATE_NOOP("Assembly", "Contact torque on the part"), 1e-6, "N m"),
    # An applied load's, on its component
    "AppliedForce": (QT_TRANSLATE_NOOP("Assembly", "Applied force"), 1e-3, "N"),
    "AppliedTorque": (QT_TRANSLATE_NOOP("Assembly", "Applied torque"), 1e-6, "N m"),
}


def resultKey(channel):
    """A channel's key in the Results box: (name, quantity), and the name of
    the part a value is on where its source has several (a contact set's)"""
    on = channel.get("On")
    key = (channel["Name"], channel["Quantity"])
    return key + (on.Name,) if on is not None else key


def resultName(channel):
    """What a channel is shown as of: its object's label, and the part's a
    value is on where its source has several"""
    obj = channel["Object"]
    name = obj.Label if obj is not None else channel["Name"]
    on = channel.get("On")
    return "%s (%s)" % (name, on.Label) if on is not None else name


def resultChannels(assembly):
    """The last run's results with their channels, the run's own, the
    stress series taken from it (MotionLoads.stressChannels) and its
    flexible bodies' stresses (FlexibleBodies.stressChannels): a value per
    frame, None at a frame a series did not solve. A channel of more than a
    vector's numbers a frame (a contact set's places on a part, for its
    loads; a flexible body's coordinates) is not one to plot, and left out."""
    import FlexibleBodies
    import MotionLoads

    results = assembly.getSimulationResults()
    if results["Channels"]:
        shown = [
            c
            for c in results["Channels"]
            if not (c["Values"] and isinstance(c["Values"][0], tuple))
        ]
        results["Channels"] = (
            shown
            + FlexibleBodies.stressChannels(assembly, results)
            + MotionLoads.stressChannels(assembly, results)
        )
    return results


def resultsTable(assembly):
    """Not upstream (docs/AssemblyDynamics.md sec 8.4, 8.5): the last run's values
    as a table, (header, rows), a row per frame: the time in s, then each
    channel's components in the units the panel shows (a vector's X, Y and
    Z), each column named by the object, the quantity, the component and the
    unit, and "(not unique)" for a redundant joint's reaction; a stress
    series' None at a frame it did not solve. None when the run reported no
    values."""
    results = resultChannels(assembly)
    if not results["Channels"]:
        return None
    header = [translate("Assembly", "Time") + " (s)"]
    columns = []
    for channel in results["Channels"]:
        name = resultName(channel)
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
            columns.append([v * factor if v is not None else None for v in values])
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
            writer.writerow(["%.17g" % v if v is not None else "" for v in row])
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
            if v is not None:
                sheet.set("%s%d" % (column(k), i + 2), "%.17g" % v)
    sheet.recompute()
    return sheet

if App.GuiUp:

    class ResultPlot(QtWidgets.QWidget):
        """Not upstream: curves over the run's time, drawn by hand -- their
        range, the times shown, and the time shown as a cursor.

        It goes by the video's time (SimulationPlayback.Playback.window()):
        as much of it as the whole run takes at Normal, so that the run is
        drawn at Normal's scale and a part Auto plays slower is stretched as
        much as it is slower. Under it a timeline: filled up to the cursor,
        its ticks an even step of the run's time apart -- spreading as the
        speed eases in, closing up as it eases out -- its slowed spans
        shaded, the times each begins to slow and is back at Normal under
        it. A click or a drag on either goes to the time there (seek). Each
        pixel's column of values is drawn by its least and largest too, so
        that an impact's peak a few microseconds wide keeps its height
        (SimulationPlayback.peakSafe())."""

        COLOURS = ["#d62728", "#2ca02c", "#1f77b4", "#888888"]
        BAR = 6  # the timeline's height

        seek = QtCore.Signal(float)

        def __init__(self, parent=None):
            super().__init__(parent)
            self.setMinimumHeight(170)
            self.setToolTip(translate("Assembly", "Click or drag to go to a time"))
            self.times = []
            # (name, values), values one a frame; or (name, values, (times,
            # samples)), drawn by its samples, at their own times
            self.curves = []
            self.sampleAxes = {}  # a sampled curve's times on the axis
            self.unit = ""
            self.bridge = set()
            self.time = None
            self.playback = None
            self.axisTimes = None  # the frames' times on the axis, the video's
            self.window = None  # the video's time shown, (start, end)
            self.area = None  # where the curves were drawn last
            # In or out about the cursor: the window narrower by it
            self.zoom = 1.0
            self.zoomIn = QtWidgets.QToolButton(self)
            self.zoomIn.setText("+")
            self.zoomIn.setToolTip(translate("Assembly", "Zoom in about the time shown"))
            self.zoomIn.clicked.connect(lambda: self.zoomBy(2.0))
            self.zoomOut = QtWidgets.QToolButton(self)
            self.zoomOut.setText("-")
            self.zoomOut.setToolTip(translate("Assembly", "Zoom out about the time shown"))
            self.zoomOut.clicked.connect(lambda: self.zoomBy(0.5))
            for button in (self.zoomIn, self.zoomOut):
                button.setAutoRaise(True)
                button.setFixedSize(20, 18)

        def setCurves(self, times, curves, unit, bridge=()):
            """bridge: the frames whose missing values a line passes over
            (SimulationPlayback.peakSafe()), those through the impacts"""
            self.times = list(times)
            self.curves = curves
            self.bridge = set(bridge)
            self.unit = unit
            self.axisTimes = None
            self.sampleAxes = {}
            self.update()

        def setPlayback(self, playback):
            """The speed the run plays at, whose video's time the axis is"""
            self.playback = playback
            self.axisTimes = None
            self.sampleAxes = {}
            self.update()

        def curvePoints(self, k):
            """Curve k's times on the axis and its values: a sampled one's
            samples, else the frames'"""
            entry = self.curves[k]
            if len(entry) < 3:
                return self.axisTimes, entry[1]
            if k not in self.sampleAxes:
                self.sampleAxes[k] = [self.toAxis(t) for t in entry[2][0]]
            return self.sampleAxes[k], entry[2][1]

        def setTime(self, time):
            """The cursor at the run's time"""
            self.time = time
            self.update()

        def toAxis(self, time):
            return self.playback.videoTime(time) if self.playback else time

        def fromAxis(self, x):
            return self.playback.runTime(x) if self.playback else x

        def shownWindow(self):
            """The axis' window: the video's time shown, zoomed in or out
            about the cursor"""
            if not self.times:
                return 0.0, 0.0
            time = self.time if self.time is not None else self.times[0]
            if self.playback is not None:
                return self.playback.window(time, self.zoom)
            start, end = self.times[0], self.times[-1]
            width = (end - start) / self.zoom
            if width >= (end - start) * (1.0 - 1e-9):
                return start, end
            low = min(max(time - 0.5 * width, start), end - width)
            return low, low + width

        def zoomBy(self, factor):
            """The window narrower by factor about the cursor; no wider than
            the whole video, no narrower than a millionth of the run at
            Normal"""
            whole = 1.0
            if self.playback is not None and self.playback.normalDuration() > 0.0:
                whole = self.playback.normalDuration() / max(self.playback.duration(), 1e-12)
            self.zoom = min(max(self.zoom * factor, whole), 1e6)
            self.update()

        def wheelEvent(self, event):
            steps = event.angleDelta().y() / 120.0
            if steps:
                self.zoomBy(2.0**steps)
                event.accept()

        def resizeEvent(self, event):
            # The zoom's buttons at the top right, the legend's corner the left
            size = self.zoomIn.sizeHint()
            self.zoomIn.move(self.width() - size.width() - 2, 1)
            self.zoomOut.move(self.width() - 2 * size.width() - 4, 1)
            super().resizeEvent(event)

        def runWindow(self):
            """The run's time shown, (start, end)"""
            v0, v1 = self.shownWindow()
            return self.fromAxis(v0), self.fromAxis(v1)

        def mousePressEvent(self, event):
            self.seekAt(event.position().x())

        def mouseMoveEvent(self, event):
            if event.buttons() & Qt.LeftButton:
                self.seekAt(event.position().x())

        def seekAt(self, x):
            if self.area is None or self.window is None or not self.area.width() > 0:
                return
            v0, v1 = self.window
            u = min(max((x - self.area.left()) / self.area.width(), 0.0), 1.0)
            self.seek.emit(self.fromAxis(v0 + u * (v1 - v0)))

        def paintEvent(self, event):
            painter = QtGui.QPainter(self)
            painter.setRenderHint(QtGui.QPainter.Antialiasing)
            palette = self.palette()
            text = palette.color(QtGui.QPalette.WindowText)
            painter.fillRect(self.rect(), palette.color(QtGui.QPalette.Base))
            metrics = painter.fontMetrics()
            if self.axisTimes is None:
                self.axisTimes = [self.toAxis(t) for t in self.times]
            sources = [self.curvePoints(k) for k in range(len(self.curves))]
            # None: a frame without a value (a stress series' frame not solved)
            values = [v for _, curve in sources for v in curve if v is not None]
            if len(self.times) < 2 or not values:
                self.area = None
                painter.setPen(text)
                painter.drawText(
                    self.rect(), Qt.AlignCenter, translate("Assembly", "No values to show")
                )
                return
            v0, v1 = self.shownWindow()
            if not v1 > v0:
                v0, v1 = self.axisTimes[0], self.axisTimes[-1] or 1.0
            self.window = (v0, v1)
            span = (v1 - v0) or 1.0
            # Each curve's points to draw, by its columns' extremes; the
            # value axis by what they show in the window, the whole run's
            # where they show nothing there
            columns = max(1, int(self.width()))
            drawn = [
                SimulationPlayback.peakSafe(
                    axis, curve, v0, v1, columns, self.bridge if axis is self.axisTimes else ()
                )
                for axis, curve in sources
            ]
            shown = SimulationPlayback.shownRange([r for runs in drawn for r in runs], v0, v1)
            whole = max(values) - min(values)
            low, high = shown if shown is not None else (min(values), max(values))
            # No finer than a thousandth of the whole run's: a steady value's
            # round-off is not drawn across the plot's height
            if 0.0 < high - low < 1e-3 * whole:
                middle = 0.5 * (low + high)
                low, high = middle - 5e-4 * whole, middle + 5e-4 * whole
            if high - low <= 1e-12 * max(1.0, abs(high)):
                pad = max(abs(high) * 0.1, 1e-9)
                low, high = low - pad, high + pad
            self.valueRange = (low, high)

            def label(v):
                return f"{v:.4g}"

            left = 6 + max(metrics.horizontalAdvance(label(v)) for v in (low, high))
            area = QtCore.QRectF(
                left + 4,
                metrics.height() + 4,
                self.width() - left - 10,
                self.height() - 2 * metrics.height() - 12 - 1.5 * self.BAR,
            )
            self.area = area

            def x(v):
                return area.left() + area.width() * (v - v0) / span

            def at(v, value):
                return QtCore.QPointF(
                    x(v), area.bottom() - area.height() * (value - low) / (high - low)
                )

            grid = QtGui.QColor(text)
            grid.setAlpha(70)
            painter.setPen(QtGui.QPen(grid, 1))
            painter.drawRect(area)
            if low < 0.0 < high:
                painter.drawLine(at(v0, 0.0), at(v1, 0.0))
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
                QtCore.QRectF(0, 0, left, metrics.height()), Qt.AlignRight | Qt.AlignTop, self.unit
            )
            # The legend, along the top
            xl = area.left()
            painter.save()
            painter.setClipRect(area.adjusted(-3, -3, 3, 3))
            for k in range(len(self.curves)):
                colour = QtGui.QColor(self.COLOURS[k % len(self.COLOURS)])
                painter.setPen(QtGui.QPen(colour, 1.5))
                # A line through each run of values, a dot for one alone
                for run in drawn[k]:
                    points = [at(v, value) for v, value in run]
                    if len(points) > 1:
                        painter.drawPolyline(QtGui.QPolygonF(points))
                    else:
                        painter.drawEllipse(points[0], 2.0, 2.0)
            painter.restore()
            for k, (name, *_) in enumerate(self.curves):
                if len(self.curves) > 1:
                    painter.setPen(QtGui.QColor(self.COLOURS[k % len(self.COLOURS)]))
                    painter.drawText(QtCore.QPointF(xl, metrics.ascent()), name)
                    xl += metrics.horizontalAdvance(name) + 10
            cursor = None
            if self.time is not None:
                c = x(self.toAxis(self.time))
                if area.left() - 0.5 <= c <= area.right() + 0.5:
                    cursor = c
            if cursor is not None:
                colour = QtGui.QColor(text)
                colour.setAlpha(160)
                painter.setPen(QtGui.QPen(colour, 1, Qt.DashLine))
                painter.drawLine(QtCore.QPointF(cursor, area.top()), QtCore.QPointF(cursor, area.bottom()))
            self.paintTimeline(painter, area, x, cursor, grid, text, metrics)

        def paintTimeline(self, painter, area, x, cursor, grid, text, metrics):
            """The timeline under the curves: filled up to the cursor, its
            ticks an even step of the run's time apart, its slowed spans
            shaded, and the times shown under it"""
            palette = self.palette()
            v0, v1 = self.window
            r0, r1 = self.fromAxis(v0), self.fromAxis(v1)
            bar = QtCore.QRectF(area.left(), area.bottom() + 4, area.width(), self.BAR)
            painter.fillRect(bar, grid)
            highlight = palette.color(QtGui.QPalette.Highlight)
            if cursor is not None:
                done = QtCore.QRectF(bar.left(), bar.top(), cursor - bar.left(), bar.height())
                painter.fillRect(done, highlight)
            spans = []
            if self.playback is not None:
                spans = [s for s in self.playback.slowSpans() if s[1] >= r0 and s[0] <= r1]
            shade = QtGui.QColor(highlight)
            shade.setAlpha(70)
            for t0, t1, _ in spans:
                a, b = x(self.toAxis(t0)), x(self.toAxis(t1))
                painter.fillRect(
                    QtCore.QRectF(a, bar.top() - 3, b - a, bar.height() + 6), shade
                )
            # The ticks: the run's time at Normal's scale 8 pixels or more
            # apart; spreading where it plays slower
            normal = self.playback.normal if self.playback is not None else 1.0
            perRun = area.width() / (v1 - v0) / normal if v1 > v0 else 0.0
            if perRun > 0.0:
                step = SimulationPlayback.niceStep(8.0 / perRun)
                tick = QtGui.QColor(text)
                tick.setAlpha(140)
                painter.setPen(QtGui.QPen(tick, 1))
                first = int(math.ceil(r0 / step - 1e-9))
                last = int(math.floor(r1 / step + 1e-9))
                for m in range(first, min(last, first + 2000) + 1):
                    p = x(self.toAxis(m * step))
                    tall = 3 if m % 5 == 0 else 0
                    painter.drawLine(
                        QtCore.QPointF(p, bar.top() - tall), QtCore.QPointF(p, bar.bottom())
                    )
            # The handle, a movie player's: dragged, the run goes with it
            if cursor is not None:
                painter.setPen(QtGui.QPen(palette.color(QtGui.QPalette.Base), 1.5))
                painter.setBrush(highlight)
                painter.drawEllipse(
                    QtCore.QPointF(cursor, bar.center().y()), self.BAR, self.BAR
                )
                painter.setBrush(Qt.NoBrush)
            # The times under it: where each slowed span begins to slow and
            # is back at Normal, then the window's ends where they fit
            painter.setPen(text)
            row = bar.bottom() + self.BAR / 2 + 2
            placed = []

            def put(px, string, align):
                width = metrics.horizontalAdvance(string) + 6
                if align == "left":
                    lx = px
                elif align == "right":
                    lx = px - width
                else:
                    lx = px - width / 2
                lx = min(max(lx, 0.0), self.width() - width)
                rect = QtCore.QRectF(lx, row, width, metrics.height())
                if any(rect.intersects(r) for r in placed):
                    return False
                placed.append(rect)
                painter.drawText(rect, Qt.AlignCenter, string)
                return True

            for t0, t1, _ in spans:
                places = min(9, max(3, int(math.ceil(-math.log10(max(t1 - t0, 1e-12)))) + 1))
                for t in (t0, t1):
                    if r0 <= t <= r1:
                        p = x(self.toAxis(t))
                        painter.drawLine(
                            QtCore.QPointF(p, bar.top() - 3), QtCore.QPointF(p, bar.bottom() + 2)
                        )
                        put(p, f"{t:.{places}f} s", "centre")
            whole = self.times[-1] - self.times[0]
            if r1 - r0 < whole * (1.0 - 1e-9) and r1 > r0:
                places = max(0, int(math.ceil(-math.log10(r1 - r0))) + 2)
                ends = (f"{r0:.{places}f} s", f"{r1:.{places}f} s")
            else:
                ends = (f"{r0:g} s", f"{r1:g} s")
            put(area.left(), ends[0], "left")
            put(area.right(), ends[1], "right")



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
    # A profile contact reports a cam's readings too, its B the follower
    if obj.isDerivedFrom("Assembly::CamContact") or obj.isDerivedFrom("Assembly::ProfileContact"):
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
    repeat=False,
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
    by: it stops there, or marks that one and each after -- or, with
    repeat, every count'th"""
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
    obj.Repeat = repeat
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
        if obj.Action == "Mark" and getattr(obj, "Repeat", False):
            text = translate("Assembly", "{}, every {}").format(text, ordinal(obj.Count))
        elif obj.Action == "Mark":
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
    """The components a contact set's pairs are of, for a sensor's pair: a
    cam contact's its cam and its follower"""
    out = []
    joint = getattr(contact, "Joint", None)
    if contact.isDerivedFrom("Assembly::CamContact") and joint is not None:
        import JointObject

        out += [JointObject.sideMovingPart(joint, 1), JointObject.sideMovingPart(joint, 2)]
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
        # A mark's: every count'th trip, not each from it
        self.repeatCheck = QtWidgets.QCheckBox(
            translate("Assembly", "Every such trip"), self.dialog
        )
        self.repeatCheck.setToolTip(
            translate(
                "Assembly",
                "A sensor that marks: every such trip -- on trip 3, the 3rd, the 6th,\n"
                "the 9th and on -- rather than that one and each after it.",
            )
        )
        layout.addWidget(self.repeatCheck, 6, 2)
        self.actionCombo.currentIndexChanged.connect(self.showAction)

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
            self.repeatCheck.setChecked(getattr(obj, "Repeat", False))
            for combo, part in zip(self.pairCombos, sensorPair(obj)):
                combo.setCurrentIndex(max(0, combo.findData(part)))
        self.showQuantity()
        self.showAction()

    def showAction(self):
        """Every count'th trip is a mark's"""
        self.repeatCheck.setEnabled(self.actionCombo.currentData() == "Mark")

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
        obj.Repeat = self.repeatCheck.isChecked() and obj.Action == "Mark"
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
        self.addVideoOptions()
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
        # Not upstream: the speed, and the run's time shown -- between its
        # frames when played slower than them
        self.playback = None
        self.shownTime = None
        # Not upstream: the flexible bodies drawn bent as the run plays, by
        # their stress (docs/AssemblyDynamics.md D4b)
        import FlexibleBodies

        self.deformations = FlexibleBodies.Deformations()
        self.addSpeedWidgets()

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
        self.impactCheck = QtWidgets.QCheckBox(translate("Assembly", "Impact frames"))
        self.impactCheck.setToolTip(
            translate(
                "Assembly",
                "Dynamic: also frames through each contact's impact, between the\n"
                "output frames, as many as its force's curve needs. An impact of metal\n"
                "on metal lasts some tens of microseconds and falls between them: its\n"
                "loads, and To FEM's stresses, are seen only at these. Played at the\n"
                "Auto speed, the playback slows down through them.",
            )
        )
        row.addWidget(self.impactCheck)
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
        self.resultPlot.seek.connect(self.onPlotSeek)
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
        self.femButton = QPushButton(translate("Assembly", "To FEM..."))
        self.femButton.setToolTip(
            translate(
                "Assembly",
                "A static stress analysis of a part, from its loads, its gravity and\n"
                "motion, held at its most loaded joint: at the frame shown, for CalculiX,\n"
                "or at a range of frames, solved now and its stress plotted here",
            )
        )
        self.femButton.clicked.connect(self.loadsToFem)
        exports.addWidget(self.femButton)
        exports.addStretch(1)
        results.addLayout(exports)
        self.resultsBox.hide()
        self.form.layout().addWidget(self.resultsBox)
        self.resultCombo.currentIndexChanged.connect(self.showResult)
        self.results = None
        self.analysisCombo.currentIndexChanged.connect(self.onAnalysisChanged)
        self.gravityCheck.toggled.connect(self.onGravityChanged)
        self.reactionsCheck.toggled.connect(self.onReactionsChanged)
        self.impactCheck.toggled.connect(self.onImpactFramesChanged)
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
        self.impactCheck.setChecked(getattr(self.simFeaturePy, "ImpactFrames", False))
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
            or o.isDerivedFrom("Assembly::FlexibleBody")
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
        self.impactCheck.setVisible(
            self.analysisCombo.currentData() == "Dynamic"
            and hasattr(self.simFeaturePy, "ImpactFrames")
        )
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

    def onImpactFramesChanged(self, checked):
        if hasattr(self.simFeaturePy, "ImpactFrames"):
            self.simFeaturePy.ImpactFrames = checked

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
        self.removeDeformations()
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
        self.removeDeformations()
        # Not upstream: the flexible bodies' meshes made, or kept where they
        # are of the components as they are (docs/AssemblyDynamics.md D4b)
        import FlexibleBodies

        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        try:
            made = FlexibleBodies.prepareStudy(self.simFeaturePy)
            if made:
                App.Console.PrintLog("Assembly: meshed %s\n" % ", ".join(made))
        except Exception as e:
            App.Console.PrintError("Assembly: %s\n" % e)
            self.showRunFailed()
            return
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()
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
        try:
            progress = self.assembly.getSimulationProgress()
        except ReferenceError:
            # Its document closed under the run: nothing is left to show, and
            # the timer would raise this every 50 ms
            self.runPoll.stop()
            return
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
        self.playback = None
        self.shownTime = None
        import FlexibleBodies

        self.removeDeformations()
        self.deformations = FlexibleBodies.deformationsOf(
            self.simFeaturePy, self.assembly.getSimulationResults()
        )
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
        self.fitPlayerLabels(nFrms)
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
        when it has any, and the stress series taken from it"""
        self.results = resultChannels(self.assembly)
        current = self.resultCombo.currentData()
        self.resultCombo.blockSignals(True)
        self.resultCombo.clear()
        for k, channel in enumerate(self.results["Channels"]):
            name = resultName(channel)
            quantity = ResultQuantities.get(channel["Quantity"], (channel["Quantity"], 1.0, ""))
            key = resultKey(channel)
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
        self.resultCombo.setCurrentIndex(max(0, self.resultIndex(current)))
        self.resultCombo.blockSignals(False)
        self.resultsBox.setVisible(self.resultCombo.count() > 0)
        self.showResult()

    def resultIndex(self, key):
        """The Results box's row of a channel's key (resultKey()); -1
        when none. Not findData: it matches a key stored as a Python object
        by identity, never an equal tuple made anew -- so a channel chosen
        was lost each time the box was filled again, on a new run."""
        if key is None:
            return -1
        key = tuple(key)
        for k in range(self.resultCombo.count()):
            data = self.resultCombo.itemData(k)
            if data is not None and tuple(data) == key:
                return k
        return -1

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

    def femChoice(self, parts):
        """The part to analyse and its frames: the frame shown (an analysis
        to run from the FEM workbench) or a range of them, solved now. (part,
        [frame, ...], solve now, the damping of its own vibration or None
        without it), or None when cancelled."""
        import MotionLoads

        times = self.results["Times"]
        shownFrame = self.form.frameSlider.value()
        dialog = QDialog(self.form)
        dialog.setWindowTitle(translate("Assembly", "Loads to FEM"))
        grid = QGridLayout(dialog)
        grid.addWidget(QLabel(translate("Assembly", "Part:")), 0, 0)
        partCombo = QComboBox()
        for part in parts:
            partCombo.addItem(part.Label)
        grid.addWidget(partCombo, 0, 1, 1, 5)
        shown = QtWidgets.QRadioButton(
            translate("Assembly", "The frame shown, at {:g} s: an analysis to run").format(
                times[shownFrame]
            )
        )
        shown.setChecked(True)
        grid.addWidget(shown, 1, 0, 1, 6)
        many = QtWidgets.QRadioButton(translate("Assembly", "Frames, solved now by CalculiX:"))
        many.setToolTip(
            translate(
                "Assembly",
                "One analysis, solved at each frame in turn: each one's largest von Mises\n"
                "stress is plotted in the Results box, and the analysis is left at the\n"
                "frame of the largest, with its result",
            )
        )
        grid.addWidget(many, 2, 0, 1, 6)
        spins = []
        for k, (text, value) in enumerate(
            (
                (translate("Assembly", "from"), 0),
                (translate("Assembly", "to"), len(times) - 1),
                (translate("Assembly", "every"), 1),
            )
        ):
            grid.addWidget(QLabel(text), 3, 2 * k)
            spin = QtWidgets.QSpinBox()
            spin.setRange(1 if k == 2 else 0, max(len(times) - 1, 1))
            spin.setValue(value)
            grid.addWidget(spin, 3, 2 * k + 1)
            spins.append(spin)
        # Not upstream: the frames through the impacts, between the output
        # frames, all of them -- as many as each one's force's curve needs,
        # a handful -- so that the stress plotted follows the impact; or
        # each one's peak alone, of the part's contact forces
        step = float(self.simValues.TimeStepOutput)
        between = {k for k in range(len(times)) if not SimulationPlayback.onOutput(times, step, k)}
        peaksOnly = QtWidgets.QCheckBox(
            translate("Assembly", "Between the output frames, each impact's peak alone")
        )
        peaksOnly.setToolTip(
            translate(
                "Assembly",
                "The frames through an impact, between the output frames, are as many as\n"
                "its force's curve needs, and with them all the stress plotted follows the\n"
                "impact; its peak alone is the one of the largest of the part's contact\n"
                "forces, fewer to solve",
            )
        )
        peaksOnly.setChecked(False)
        peaksOnly.setVisible(bool(between))
        grid.addWidget(peaksOnly, 4, 0, 1, 6)
        # Not upstream: the part's own vibration, by its modes, between the
        # frames too (MotionLoads.solveFrames' dynamic)
        vibration = QtWidgets.QCheckBox(
            translate("Assembly", "With its own vibration, by its modes; damped, % of critical:")
        )
        vibration.setToolTip(
            translate(
                "Assembly",
                "Each frame is solved as it stands, and the part's held modes add how it\n"
                "rings as the loads change from frame to frame: through an impact, its own\n"
                "inertia takes the blow, and it rings after. Its stress is then sampled\n"
                "between the frames too. The frames should follow each other: every\n"
                "frame, each impact's all",
            )
        )
        grid.addWidget(vibration, 5, 0, 1, 5)
        damping = QtWidgets.QDoubleSpinBox()
        damping.setRange(0.0, 50.0)
        damping.setDecimals(2)
        damping.setValue(100.0 * MotionLoads.DAMPING)
        grid.addWidget(damping, 5, 5)
        count = QLabel()
        grid.addWidget(count, 6, 0, 1, 6)

        def peaksOf(part):
            channels = [
                c for c in self.results["Channels"] if c["Quantity"] == "PartContactForce"
            ]
            mine = [c for c in channels if c.get("On") is not None and c["On"].Name == part.Name]
            forces = [0.0] * len(times)
            for channel in mine or channels:
                for k, value in enumerate(channel["Values"][: len(times)]):
                    forces[k] += value.Length
            return set(SimulationPlayback.impactPeaks(times, step, forces))

        def chosen():
            first, last, every = (s.value() for s in spins)
            peaks = peaksOf(parts[partCombo.currentIndex()]) if peaksOnly.isChecked() else None
            frames = []
            n = 0
            for k in range(min(first, last), max(first, last) + 1):
                if k in between:
                    if peaks is None or k in peaks:
                        frames.append(k)
                    continue
                if n % every == 0:
                    frames.append(k)
                n += 1
            return frames or [min(first, last)]

        def update():
            for spin in spins:
                spin.setEnabled(many.isChecked())
            peaksOnly.setEnabled(many.isChecked())
            vibration.setEnabled(many.isChecked())
            damping.setEnabled(many.isChecked() and vibration.isChecked())
            frames = chosen()
            count.setEnabled(many.isChecked())
            count.setText(
                translate("Assembly", "{} frames, {:g} to {:g} s").format(
                    len(frames), times[frames[0]], times[frames[-1]]
                )
            )

        for spin in spins:
            spin.valueChanged.connect(update)
        many.toggled.connect(update)
        peaksOnly.toggled.connect(update)
        vibration.toggled.connect(update)
        partCombo.currentIndexChanged.connect(update)
        update()
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        grid.addWidget(buttons, 7, 0, 1, 6)
        if dialog.exec() != QDialog.Accepted:
            return None
        part = parts[partCombo.currentIndex()]
        if many.isChecked():
            dynamic = None
            if vibration.isChecked():
                dynamic = damping.value() / 100.0
            return part, chosen(), True, dynamic
        return part, [shownFrame], False, None

    def loadsToFem(self):
        """Not upstream: a part of the run as a static stress analysis
        (docs/AssemblyDynamics.md D4a): at the frame shown, to run from the
        FEM workbench (MotionLoads.makeAnalysis), or at a range of frames,
        solved now (MotionLoads.solveFrames), its stresses plotted here"""
        import MotionLoads

        if not self.results:
            return
        parts = MotionLoads.movingParts(self.assembly, self.results)
        if not parts:
            return
        choice = self.femChoice(parts)
        if choice is None:
            return
        part, frames, solveNow, damping = choice
        if solveNow:
            self.solveFemFrames(part, frames, damping)
            return
        frame = frames[0]
        doc = self.assembly.Document
        doc.openTransaction("Loads to FEM")
        try:
            analysis, body, held = MotionLoads.makeAnalysis(self.assembly, part, frame)
        except (ValueError, ImportError) as e:
            doc.abortTransaction()
            QMessageBox.warning(self.form, translate("Assembly", "Loads to FEM"), str(e))
            return
        doc.commitTransaction()
        # Where it stands, the frame it is at
        self.onFrameChanged(frame)
        notes = [
            translate("Assembly", "{} at {:g} s: {} loads, held at {}.").format(
                part.Label, body["Time"], len(body["Loads"]), held["Source"].Label
            )
        ]
        if body["Missing"]:
            notes.append(
                translate(
                    "Assembly",
                    "Not in it, whose loads this run does not report (run it again): {}.",
                ).format(
                    ", ".join(o.Label for o in body["Missing"])
                )
            )
        if not all(l["Unique"] for l in body["Loads"]):
            notes.append(
                translate(
                    "Assembly",
                    "A redundant joint's load is one split of it among the joints sharing it.",
                )
            )
        notes += self.groupGapNotes(part)
        notes.append(
            translate("Assembly", "Run it with CalculiX from the FEM workbench: {}.").format(
                analysis.Label
            )
        )
        QMessageBox.information(
            self.form, translate("Assembly", "Loads to FEM"), "\n\n".join(notes)
        )

    def solveFemFrames(self, part, frames, damping=None):
        """part at each of frames, solved by CalculiX (MotionLoads.
        solveFrames) behind a progress dialog with a Cancel; its stresses
        then in the Results box. damping: with its own vibration, its
        modes damped so; None, without"""
        import MotionLoads

        progress = QProgressDialog(
            translate("Assembly", "Solving {} with CalculiX...").format(part.Label),
            translate("Assembly", "Cancel"),
            0,
            len(frames),
            self.form,
        )
        progress.setWindowModality(Qt.WindowModal)
        progress.setMinimumDuration(0)
        progress.show()

        def step(k, n):
            progress.setValue(k)
            progress.setLabelText(
                translate("Assembly", "Solving {}: frame {} of {}").format(part.Label, k + 1, n)
            )
            QtWidgets.QApplication.processEvents()
            return not progress.wasCanceled()

        frame = self.form.frameSlider.value()
        doc = self.assembly.Document
        doc.openTransaction("Loads to FEM")
        try:
            if damping is None:
                series = MotionLoads.solveFrames(self.assembly, part, frames, progress=step)
            else:
                series = MotionLoads.solveFrames(
                    self.assembly, part, frames, progress=step, dynamic=True, damping=damping
                )
        except (ValueError, ImportError) as e:
            doc.abortTransaction()
            progress.close()
            self.onFrameChanged(frame)
            QMessageBox.warning(self.form, translate("Assembly", "Loads to FEM"), str(e))
            return
        doc.commitTransaction()
        progress.close()
        self.fillResults()
        at = self.resultIndex((series.Name, "VonMises"))
        if at >= 0:
            self.resultCombo.setCurrentIndex(at)
        self.onFrameChanged(frame)
        if not series.Frames:
            return
        peak = max(range(len(series.Frames)), key=lambda k: series.VonMises[k])
        # Against the largest reaction: where the reaction is small, what the
        # held face's own nodes take of the body load is a larger part of it
        largest = max(max(run.Length for run in series.HeldForce), 1e-12)
        worst = 0.0
        for run, fem in zip(series.HeldForce, series.FemHeldForce):
            worst = max(worst, (fem - run).Length / largest)
        notes = [
            translate("Assembly", "{} solved at {} frames, held at {}.").format(
                part.Label, len(series.Frames), series.Held.Label
            )
        ]
        # With its own vibration: its stresses between the frames too, the
        # frames' static ones beside them
        dynamic = getattr(series, "Dynamic", False) and series.SampleTimes
        times, whole, away = series.Times, series.VonMises, series.VonMisesAway
        if dynamic:
            # Held in them as its joint holds it
            free = {
                "Turn": translate("Assembly", "free to turn on {}"),
                "Slide": translate("Assembly", "free to slide along {}"),
                "TurnSlide": translate("Assembly", "free to turn on and slide along {}"),
                "Ball": translate("Assembly", "free to turn every way on {}"),
            }.get(getattr(series, "ModesFree", ""), translate("Assembly", "held stiff at {}"))
            notes.append(
                translate(
                    "Assembly",
                    "With its own vibration, by its first {} modes, {} ({:.4g} to {:.4g} "
                    "Hz), each damped {:.3g}% of critical, from rest at the first frame.",
                ).format(
                    len(series.ModeFrequencies),
                    free.format(series.Held.Label),
                    series.ModeFrequencies[0],
                    series.ModeFrequencies[-1],
                    100.0 * series.Damping,
                )
            )
            times = series.SampleTimes
            whole, away = series.SampleVonMises, series.SampleVonMisesAway
            peak = max(range(len(whole)), key=lambda k: whole[k])
        notes.append(
            translate("Assembly", "Its largest von Mises stress: {:.4g} MPa at {:g} s.").format(
                whole[peak], times[peak]
            )
        )
        # Away from the loaded faces, where the couplings and the loads'
        # concentration do not reach
        away = [(v, k) for k, v in enumerate(away) if not math.isnan(v)]
        if away:
            value, k = max(away)
            notes.append(
                translate(
                    "Assembly",
                    "Away from the loaded faces (about a section's depth off them): "
                    "{:.4g} MPa at {:g} s.",
                ).format(value, times[k])
            )
        if dynamic:
            still = [v for v in series.StaticVonMisesAway if not math.isnan(v)]
            notes.append(
                translate(
                    "Assembly",
                    "Each frame as it stands, without the vibration: {:.4g} MPa at most, "
                    "{} away from the loaded faces.",
                ).format(
                    max(series.StaticVonMises),
                    "{:.4g} MPa".format(max(still)) if still else "-",
                )
            )
        held = (
            translate(
                "Assembly",
                "What holds it in the analysis against what the rest of its rigid group holds "
                "it with, which the run does not split among its parts: within {:.2g}% of the "
                "largest, at every frame.",
            )
            if hasattr(series.Held, "ObjectsToRigidGroup")
            else translate(
                "Assembly",
                "What holds it in the analysis against the run's reaction there: within "
                "{:.2g}% of the largest reaction, at every frame.",
            )
        )
        if dynamic:
            held += " " + translate("Assembly", "(Each frame as it stands.)")
        notes += [
            held.format(100.0 * worst),
        ]
        notes += self.groupGapNotes(part)
        if len(series.Frames) < len(frames):
            notes.append(
                translate("Assembly", "Cancelled: {} of the {} frames were solved.").format(
                    len(series.Frames), len(frames)
                )
            )
        notes.append(
            translate(
                "Assembly", "The analysis is left at {:g} s with its result: {}."
            ).format(
                times[peak],
                ", ".join(o.Label for o in series.InList if o.isDerivedFrom("Fem::FemAnalysis")),
            )
        )
        QMessageBox.information(
            self.form, translate("Assembly", "Loads to FEM"), "\n\n".join(notes)
        )

    def groupGapNotes(self, part):
        """A note where part meets the rest of its rigid group nowhere: what
        holds it there is across the gap"""
        import MotionLoads

        gap = MotionLoads.groupGap(self.assembly, part)
        if not gap:
            return []
        return [
            translate(
                "Assembly",
                "It meets the rest of its rigid group nowhere, {:.3g} mm off them: held "
                "where it faces them across that.",
            ).format(gap)
        ]

    def resultCurves(self):
        """The chosen channel's curves, in the shown unit: a vector's X, Y, Z
        and its length; and the unit"""
        if not self.results:
            return [], ""
        key = self.resultCombo.currentData()
        for channel in self.results["Channels"]:
            if key is None or resultKey(channel) != tuple(key):
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
            scaled = [v * factor if v is not None else None for v in values]
            # A stress series with the part's vibration: sampled between the
            # frames too
            if channel.get("Samples"):
                samples = [v * factor if v is not None else None for v in channel["Samples"]]
                if not channel.get("Static"):
                    return [("", scaled, (channel["SampleTimes"], samples))], unit
                # Each frame as it stands, beside it
                static = [v * factor if v is not None else None for v in channel["Static"]]
                return [
                    (
                        translate("Assembly", "vibrating"),
                        scaled,
                        (channel["SampleTimes"], samples),
                    ),
                    (translate("Assembly", "as it stands"), static),
                ], unit
            return [("", scaled)], unit
        return [], ""

    def showResult(self, index=None):
        curves, unit = self.resultCurves()
        times = self.results["Times"] if self.results else []
        between = [k for k in range(len(times)) if not self.onOutputTime(k)]
        self.resultPlot.setCurves(times, curves, unit, between)
        self.showResultValue()

    def showResultValue(self):
        times = self.results["Times"] if self.results else []
        frame = self.form.frameSlider.value()
        shown = self.shownTime
        if shown is None and frame < len(times):
            shown = times[frame]
        if shown is not None:
            self.currentPlayback()
            self.resultPlot.setTime(shown)
        curves, unit = self.resultCurves()
        if not curves or frame >= len(times):
            self.resultValue.clear()
            return
        parts = []
        for name, values, *_ in curves:
            if frame < len(values):
                if values[frame] is None:
                    value = translate("Assembly", "not solved at this frame")
                else:
                    value = f"{values[frame]:.6g} {unit}"
                parts.append(f"{name} {value}" if name else value)
        self.resultValue.setText(f"{times[frame]:g} s: " + ", ".join(parts))

    def frameTimes(self):
        """Not upstream: the run's frames' times, its own -- a frame through
        an impact is between the output times -- or, from a run that reports
        none, the output times"""
        times = self.results.get("Times", []) if self.results else []
        if times:
            return times
        step = float(self.simValues.TimeStepOutput)
        return [k * step for k in range(self.assembly.numberOfFrames())]

    def playbackSpeed(self):
        """The study's PlaybackSpeed; Normal for upstream's Python one"""
        return getattr(self.simFeaturePy, "PlaybackSpeed", "Normal")

    def makePlayback(self):
        """Not upstream: the run's video frames at the study's speed and
        frames a second (SimulationPlayback.Playback)"""
        self.playback = SimulationPlayback.Playback(
            self.frameTimes(),
            float(self.simValues.TimeStepOutput),
            self.form.FramesPerSecondSpinBox.value(),
            self.playbackSpeed(),
            SimulationPlayback.contactForces(self.results),
            getattr(self.simFeaturePy, "SlowMotion", 1.0),
        )
        # The plot goes by its video's time
        self.resultPlot.setPlayback(self.playback)
        return self.playback

    def currentPlayback(self):
        return self.playback if self.playback is not None else self.makePlayback()

    def onPlotSeek(self, time):
        """Not upstream: the plot's timeline clicked or dragged, a movie
        player's: the run at that time, played on from there if playing"""
        playing = self.animationTimer.isActive()
        self.showPlayed(time, self.currentPlayback().rate(time))
        if playing:
            self.animationTimerStart()

    def nearestFrame(self, time):
        times = self.frameTimes()
        if not times:
            return 0
        k = bisect.bisect_left(times, time)
        if k >= len(times):
            return len(times) - 1
        if k > 0 and time - times[k - 1] <= times[k] - time:
            return k - 1
        return k

    def fitPlayerLabels(self, count):
        """Not upstream: the frame's, time's and speed's labels as wide as the
        widest they show, not their text now: the panel, as wide as they are,
        widened and narrowed as it played"""
        times = self.frameTimes()
        end = times[-1] if times else count * float(self.simValues.TimeStepOutput)

        def eights(text):
            return "".join("8" if c.isdigit() else c for c in text)

        # The time to timeText()'s most places
        widest = {
            self.form.FrameLabel: [
                translate("Assembly", "Frame" + " " + eights(str(max(count - 1, 0))))
            ],
            self.form.FrameTimeLabel: [eights("%.9f s" % end)],
            self.speedLabel: [
                eights(SimulationPlayback.speedText(rate))
                for rate in (1.0 / 888, 1.0 / 88.8, 1.0 / 8.88e4, 888)
            ],
        }
        # Measured as the label sizes itself, by its own hint: that is its
        # text laid out, the width rounded up, and its margin and indent --
        # a pixel more than the font's advance where the advance has a
        # fraction ("Frame 100": advance 64, hint 65, DejaVu Sans 9 pt), and
        # the label then a pixel wider at those frames than at the others.
        # The hint is never under the minimum width, so that goes first
        for label, texts in widest.items():
            shown = label.text()
            label.setMinimumWidth(0)
            width = 0
            for text in texts:
                label.setText(text)
                width = max(width, label.sizeHint().width())
            label.setText(shown)
            label.setMinimumWidth(width)

    def showTime(self, time, frame, rate):
        """The labels and the plot at the run's time, its frame nearest"""
        self.shownTime = time
        self.form.FrameLabel.setText(translate("Assembly", "Frame" + " " + str(frame)))
        self.form.FrameTimeLabel.setText(
            SimulationPlayback.timeText(time, rate, self.currentPlayback().fps)
        )
        self.speedLabel.setText(
            SimulationPlayback.speedText(rate) if self.hasPlaybackSpeed() else ""
        )
        if self.results:
            self.showResultValue()
        self.deformations.show(time)

    def removeDeformations(self):
        self.deformations.remove()

    def onFrameChanged(self, val):
        self.assembly.updateForFrame(val)
        # The frame's own time, the run's: one through an impact is between
        # the output times
        times = self.frameTimes()
        if 0 <= val < len(times):
            time = times[val]
        else:
            time = float(val * self.simValues.TimeStepOutput)
        self.showTime(time, val, self.currentPlayback().rate(time))

    def onFramesPerSecondChanged(self):
        self.simValues.FramesPerSecond = self.form.FramesPerSecondSpinBox.value()
        self.makePlayback()

    def hasPlaybackSpeed(self):
        return hasattr(self.simFeaturePy, "PlaybackSpeed")

    def addSpeedWidgets(self):
        """Not upstream: how fast the frames play and the video goes (the
        study's PlaybackSpeed), and how fast against real time they go at the
        frame shown"""
        self.speedCombo = QComboBox()
        for speed in SimulationPlayback.Speeds:
            self.speedCombo.addItem(translate("Assembly", speed), speed)
        self.speedCombo.setToolTip(
            translate(
                "Assembly",
                "How fast the frames play, and the video goes.\n"
                "Normal: an output step a frame.\n"
                "1/2 to 1/256: that much slower.\n"
                "Auto: Normal, slowed where the frames come closer -- through an impact,\n"
                "a few microseconds apart, each one shown -- and eased into and out of.\n"
                "The plot shows as much less of the run as it plays slower.",
            )
        )
        self.speedLabel = QtWidgets.QLabel()
        self.speedLabel.setToolTip(
            translate("Assembly", "How fast the run plays at the frame shown, against real time")
        )
        # Auto: how slow through the impact whose forces swing the most (the
        # study's SlowMotion), the others as much less as they swing less
        self.slowSpin = QtWidgets.QDoubleSpinBox()
        self.slowSpin.setRange(0.1, 100.0)
        self.slowSpin.setSingleStep(0.5)
        self.slowSpin.setDecimals(1)
        self.slowSpin.setPrefix(translate("Assembly", "Slow-mo") + " ")
        self.slowSpin.setToolTip(
            translate(
                "Assembly",
                "Auto: how slow where the contact forces swing the most in the run --\n"
                "video frames each of its frames takes. Where they swing less, as much\n"
                "less slow: a rattle at a twentieth of the largest swing is hardly slowed.",
            )
        )
        row = self.form.findChild(QtWidgets.QHBoxLayout, "fps_layout")
        if row is not None:
            row.addWidget(QtWidgets.QLabel(translate("Assembly", "Speed")))
            row.addWidget(self.speedCombo)
            row.addWidget(self.slowSpin)
            row.addWidget(self.speedLabel, 1)
        if not self.hasPlaybackSpeed():
            self.speedCombo.setEnabled(False)
            self.speedCombo.setCurrentIndex(self.speedCombo.findData("Normal"))
        else:
            self.speedCombo.setCurrentIndex(
                max(0, self.speedCombo.findData(self.simFeaturePy.PlaybackSpeed))
            )
        self.slowSpin.setValue(getattr(self.simFeaturePy, "SlowMotion", 1.0))
        self.slowSpin.setVisible(
            hasattr(self.simFeaturePy, "SlowMotion") and self.playbackSpeed() == "Auto"
        )
        self.speedCombo.currentIndexChanged.connect(self.onSpeedChanged)
        self.slowSpin.valueChanged.connect(self.onSlowMotionChanged)

    def onSpeedChanged(self, index):
        if self.hasPlaybackSpeed():
            self.simFeaturePy.PlaybackSpeed = self.speedCombo.currentData()
        self.slowSpin.setVisible(
            hasattr(self.simFeaturePy, "SlowMotion") and self.playbackSpeed() == "Auto"
        )
        self.replay()

    def onSlowMotionChanged(self, value):
        if hasattr(self.simFeaturePy, "SlowMotion"):
            self.simFeaturePy.SlowMotion = value
        self.replay()

    def replay(self):
        """The playback anew at the speed now: played on, or shown again"""
        self.makePlayback()
        if self.animationTimer.isActive():
            self.animationTimerStart()
        elif self.form.groupBox_player.isVisible():
            self.onFrameChanged(self.form.frameSlider.value())

    def playBackward(self):
        pass

    def animationTimerStartForward(self):
        self.direction = 1
        self.animationTimerStart()

    def animationTimerStartBackward(self):
        self.direction = -1
        self.animationTimerStart()

    def animationTimerStart(self):
        # Not upstream: played by the run's time, at the study's speed
        # (SimulationPlayback): a video frame's time between the run's frames
        # is the parts' between them (updateForTime())
        self.animationTimer.stop()
        self.makePlayback()
        self.playFrames = self.playback.frames()
        if len(self.playFrames) < 2:
            return
        now = self.shownTime
        if now is None:
            times = self.frameTimes()
            now = times[self.form.frameSlider.value()] if times else 0.0
        self.playFrom = min(
            bisect.bisect_left([t for t, _ in self.playFrames], now), len(self.playFrames) - 1
        )
        self.startTime = time.time()
        self.animationTimer.setInterval(int(1000.0 / self.playback.fps))  # ms
        self.animationTimer.start()

    def onOutputTime(self, frame):
        """Whether a frame is at one of the run's output times, not one
        through an impact between them"""
        return SimulationPlayback.onOutput(
            self.frameTimes(), float(self.simValues.TimeStepOutput), frame
        )

    def playAnimation(self):
        count = int((time.time() - self.startTime) * self.playback.fps)
        k = (self.playFrom + self.direction * count) % len(self.playFrames)
        self.showPlayed(*self.playFrames[k])

    def showPlayed(self, time, rate):
        """A video frame: the parts at the run's time, between its frames
        maybe, the slider at the frame nearest, the plot about the time"""
        self.assembly.updateForTime(time)
        frame = self.nearestFrame(time)
        self.form.frameSlider.blockSignals(True)
        self.form.frameSlider.setValue(frame)
        self.form.frameSlider.blockSignals(False)
        self.showTime(time, frame, rate)

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

    def addVideoOptions(self):
        """Not upstream: what a saved video shows with the 3D view -- the
        Results box beside it as it plays, the time and speed over it -- in
        the Save Animation button's menu, kept in the preferences"""
        params = App.ParamGet(VideoParams)
        menu = QtWidgets.QMenu(self.form.SaveAnimationButton)
        self.videoBoxAction = menu.addAction(translate("Assembly", "With the Results box"))
        self.videoBoxAction.setCheckable(True)
        self.videoBoxAction.setChecked(params.GetBool("VideoResultsBox", True))
        self.videoBoxAction.toggled.connect(lambda on: params.SetBool("VideoResultsBox", on))
        self.videoTimeAction = menu.addAction(translate("Assembly", "With the time and speed"))
        self.videoTimeAction.setCheckable(True)
        self.videoTimeAction.setChecked(params.GetBool("VideoTime", True))
        self.videoTimeAction.toggled.connect(lambda on: params.SetBool("VideoTime", on))
        # The 3D view's background in the video, as Save Picture has it, or
        # a colour of one's own
        backMenu = menu.addMenu(translate("Assembly", "Background"))
        self.videoBackgroundGroup = QtGui.QActionGroup(backMenu)
        self.videoBackgroundActions = {}
        for key, text in (
            ("Current", translate("Assembly", "As in the view")),
            ("White", translate("Assembly", "White")),
            ("Black", translate("Assembly", "Black")),
            ("Custom", translate("Assembly", "Custom color...")),
        ):
            action = backMenu.addAction(text)
            action.setCheckable(True)
            action.setData(key)
            self.videoBackgroundGroup.addAction(action)
            self.videoBackgroundActions[key] = action
        self.videoBackgroundAction().setChecked(True)
        self.showVideoBackgroundColor()
        self.videoBackgroundGroup.triggered.connect(self.onVideoBackground)
        button = self.form.SaveAnimationButton
        button.setMenu(menu)
        button.setPopupMode(QtWidgets.QToolButton.MenuButtonPopup)
        button.setToolTip(
            translate(
                "Assembly",
                "Save the run as a video, a GIF or an animated glTF.\n"
                "Its menu: the Results box beside the view as it plays, the time\n"
                "and speed over the view, and the view's background, in a video or a GIF.",
            )
        )

    def videoBackgroundAction(self):
        """Not upstream: the Background menu's action the preferences have
        chosen, the view's own background if none"""
        key = App.ParamGet(VideoParams).GetString("VideoBackground", "Current")
        return self.videoBackgroundActions.get(key, self.videoBackgroundActions["Current"])

    def videoBackgroundColor(self):
        """Not upstream: the custom background colour, white if none"""
        color = QtGui.QColor(App.ParamGet(VideoParams).GetString("VideoBackgroundColor", ""))
        return color if color.isValid() else QtGui.QColor(255, 255, 255)

    def showVideoBackgroundColor(self):
        """Not upstream: the custom colour, a swatch on its action"""
        swatch = QtGui.QPixmap(16, 16)
        swatch.fill(self.videoBackgroundColor())
        self.videoBackgroundActions["Custom"].setIcon(QtGui.QIcon(swatch))

    def onVideoBackground(self, action):
        """Not upstream: a background chosen in the menu; Custom asks for its
        colour each time, kept as it was if the dialog is cancelled"""
        params = App.ParamGet(VideoParams)
        key = action.data()
        if key == "Custom":
            color = QtWidgets.QColorDialog.getColor(
                self.videoBackgroundColor(),
                self.form,
                translate("Assembly", "Video background"),
            )
            if not color.isValid():
                self.videoBackgroundAction().setChecked(True)
                return
            params.SetString("VideoBackgroundColor", color.name())
            self.showVideoBackgroundColor()
        params.SetString("VideoBackground", key)

    def videoBackground(self):
        """Not upstream: the background a video's 3D frames are saved on,
        as View3DInventorPy.saveImage() takes it"""
        key = self.videoBackgroundAction().data()
        if key == "Custom":
            return self.videoBackgroundColor().name()
        return key

    def videoFrame(self, path, layout, time, rate):
        """Not upstream: a saved video's frame, the 3D view's image at path
        with the Results box beside it and the time over it as layout says
        (videoLayout()); written over path"""
        view = QtGui.QImage(path)
        canvas = QtGui.QImage(layout["size"], QtGui.QImage.Format_RGB32)
        canvas.fill(self.resultsBox.palette().window().color())
        painter = QtGui.QPainter(canvas)
        try:
            painter.setRenderHint(QtGui.QPainter.SmoothPixmapTransform)
            painter.setRenderHint(QtGui.QPainter.TextAntialiasing)
            painter.drawImage(0, 0, view)
            if layout.get("legend") is not None:
                import FlexibleBodies

                at = FlexibleBodies.Legend.place(layout["view"].width(), layout["view"].height())
                FlexibleBodies.Legend.paint(painter, at, *layout["legend"])
            if layout["box"] is not None:
                # At the first frame's scale, should it be taller now
                box = self.videoBoxImage()
                at = layout["box"]
                scale = layout["scale"]
                painter.drawImage(
                    QtCore.QRectF(at.x(), at.y(), box.width() * scale, box.height() * scale), box
                )
            if layout["time"]:
                text = "t = " + SimulationPlayback.timeText(
                    time, rate, self.currentPlayback().fps
                )
                if self.hasPlaybackSpeed():
                    text += "   (%s)" % SimulationPlayback.speedText(rate)
                font = QtWidgets.QApplication.font()
                font.setPixelSize(layout["text"])
                painter.setFont(font)
                metrics = QtGui.QFontMetrics(font)
                pad = layout["text"] // 3
                back = QtCore.QRect(
                    pad, pad, metrics.horizontalAdvance(text) + 2 * pad, metrics.height() + pad
                )
                painter.fillRect(back, QtGui.QColor(255, 255, 255, 200))
                painter.setPen(QtGui.QColor(30, 30, 30))
                painter.drawText(back, Qt.AlignCenter, text)
        finally:
            painter.end()
        canvas.save(path)

    def videoLayout(self, width, height):
        """Not upstream: a saved video's frames, the view width x height: the
        Results box beside it, as tall as the view at most, if it is to be
        and shows; the time over it, if it is to be; the flexible bodies'
        stress legend over its right side, as the view has it. None for the
        view alone"""
        withBox = self.videoBoxAction.isChecked() and self.resultsBox.isVisible()
        withTime = self.videoTimeAction.isChecked()
        legend = self.deformations.legendOf()
        if not withBox and not withTime and legend is None:
            return None
        size = QtCore.QSize(width, height)
        box = None
        scale = 1.0
        if withBox:
            grab = self.videoBoxImage()
            scale = min(1.0, (height - 16) / max(grab.height(), 1))
            w, h = int(grab.width() * scale), int(grab.height() * scale)
            box = QtCore.QRect(width + 8, (height - h) // 2, w, h)
            size = QtCore.QSize(width + w + 16, height)
            # Even, as the codecs want
            size.setWidth(size.width() + size.width() % 2)
        return {
            "size": size,
            "box": box,
            "scale": scale,
            "time": withTime,
            "text": max(12, height // 32),
            "legend": legend,
            "view": QtCore.QSize(width, height),
        }

    def videoBoxImage(self):
        """Not upstream: the Results box as a video shows it, down to its
        value: its buttons left out"""
        bottom = self.resultValue.geometry().bottom() + 6
        rect = QtCore.QRect(0, 0, self.resultsBox.width(), min(bottom, self.resultsBox.height()))
        return self.resultsBox.grab(rect).toImage()

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

        # Not upstream: the video goes as playback does, at the study's speed
        # (SimulationPlayback): its frames' times are the run's at that speed,
        # between the run's frames where it plays slower than them
        played = self.makePlayback().frames()
        video_times = [t for t, _ in played]
        # Not upstream: the Results box beside the view and the time over
        # it, as the menu says
        layout = self.videoLayout(width, height)
        background = self.videoBackground()
        viewSize = (width, height)
        if layout is not None:
            width, height = layout["size"].width(), layout["size"].height()

        # Setup temporary directory and progress bar
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            progress = QProgressDialog(
                translate("Assembly", "Generating Frames…"),
                translate("Assembly", "Cancel"),
                0,
                len(video_times),
                self.form,
            )
            progress.setWindowModality(Qt.WindowModal)
            progress.show()

            original_frame = self.form.frameSlider.value()

            try:
                # Generate and save all frames as temporary images
                frame_files = []
                for i, video_time in enumerate(video_times):
                    progress.setValue(i)
                    if progress.wasCanceled():
                        App.Console.PrintMessage("Animation save cancelled.\n")
                        return

                    if layout is None:
                        self.assembly.updateForTime(video_time)
                    else:
                        # The panel's labels and plot at the time too
                        self.showPlayed(video_time, played[i][1])
                    Gui.updateGui()  # Ensure the 3D view is redrawn

                    frame_filename = temp_path / f"frame_{i:05d}.png"
                    if layout is None:
                        view.saveImage(str(frame_filename), width, height, background)
                    else:
                        view.saveImage(str(frame_filename), viewSize[0], viewSize[1], background)
                        self.videoFrame(str(frame_filename), layout, video_time, played[i][1])
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
                # Not upstream: the labels and plot too, which played
                self.onFrameChanged(original_frame)

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
