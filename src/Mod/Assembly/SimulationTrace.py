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

"""Trace: the path a point of a part follows over a simulation's run, as a
curve (docs/AssemblyDynamics.md sec 8.5) -- a B-spline through the point's
positions at the run's frames, in the assembly's coordinates or, with
Relative, in another part's, as seen riding on it (a cam's profile from its
follower's path). A child of the Simulation, as its motions are; computed
after the run from its stored frames, so adding one runs nothing. Without a
run of its study (a file just opened, or another study run since) a trace
keeps the curve it has."""

import FreeCAD as App
import Part

from PySide.QtCore import QT_TRANSLATE_NOOP

translate = App.Qt.translate

# Consecutive positions closer than this are one: a point at rest, or a
# frame repeated, would stop the interpolation
SAME_POINT = 1e-6


class TraceCurve:
    def __init__(self, obj):
        obj.Proxy = self
        self.createProperties(obj)

    def onDocumentRestored(self, obj):
        self.createProperties(obj)

    def createProperties(self, obj):
        if not hasattr(obj, "Part"):
            obj.addProperty(
                "App::PropertyLink",
                "Part",
                "Trace",
                QT_TRANSLATE_NOOP("App::Property", "The part whose point is traced"),
            )
        if not hasattr(obj, "Point"):
            obj.addProperty(
                "App::PropertyVector",
                "Point",
                "Trace",
                QT_TRANSLATE_NOOP("App::Property", "The point traced, in the part's coordinates"),
            )
        if not hasattr(obj, "Relative"):
            obj.addProperty(
                "App::PropertyLink",
                "Relative",
                "Trace",
                QT_TRANSLATE_NOOP(
                    "App::Property",
                    "Empty: the path in the assembly's coordinates. A part: the path as seen "
                    "riding on it, in its coordinates",
                ),
            )

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def execute(self, obj):
        points = tracePoints(obj)
        if points is None:
            return
        obj.Shape = curveThrough(points)
        if obj.Relative is not None:
            obj.Placement = obj.Relative.Placement


def isTrace(obj):
    return isinstance(getattr(obj, "Proxy", None), TraceCurve)


def tracePoints(trace):
    """The traced point at each frame of the last run of the trace's study,
    in the assembly's coordinates or Relative's; None without such a run, or
    when neither the part nor Relative moves in it"""
    import CommandCreateSimulation

    sim = CommandCreateSimulation.simulationOf(trace)
    assembly = CommandCreateSimulation.assemblyOf(trace)
    if sim is None or assembly is None or trace.Part is None:
        return None
    if assembly.getSimulationResults().get("Simulation") != sim:
        return None
    frames = assembly.numberOfFrames()
    moving = assembly.getSimulationPlacements(trace.Part)
    riding = []
    if trace.Relative is not None:
        riding = assembly.getSimulationPlacements(trace.Relative)
    if not moving and not riding:
        return None
    # One that the run does not move stays where it is
    moving = moving or [trace.Part.Placement] * frames
    points = []
    for k, placement in enumerate(moving):
        point = placement.multVec(trace.Point)
        if trace.Relative is not None:
            on = riding[k] if riding else trace.Relative.Placement
            point = on.inverse().multVec(point)
        points.append(point)
    return points


def curveThrough(points):
    """A B-spline through the points in order, periodic when the path ends
    where it began; a vertex for a point that never moves"""
    kept = []
    for point in points:
        if not kept or (point - kept[-1]).Length > SAME_POINT:
            kept.append(point)
    if len(kept) < 2:
        return Part.Vertex(kept[0]) if kept else Part.Shape()
    closed = len(kept) > 3 and (kept[-1] - kept[0]).Length <= SAME_POINT * 10
    if closed:
        kept.pop()
    curve = Part.BSplineCurve()
    curve.interpolate(kept, PeriodicFlag=closed)
    return curve.toShape()


def newTrace(sim, part, point, relative=None):
    """A trace of point (in part's coordinates) in the simulation, its curve
    made at once when the assembly's last run is of that study"""
    obj = sim.Document.addObject("Part::FeaturePython", "Trace")
    TraceCurve(obj)
    if App.GuiUp:
        ViewProviderTraceCurve(obj.ViewObject)
    obj.Part = part
    obj.Point = point
    obj.Relative = relative
    obj.Label = translate("Assembly", "Trace of {}").format(part.Label)
    sim.addObject(obj)
    obj.recompute()
    return obj


def updateTraces(sim):
    """The simulation's traces after a run: their curves made again"""
    for obj in sim.Group:
        if isTrace(obj):
            obj.touch()
            obj.recompute()


class ViewProviderTraceCurve:
    def __init__(self, vp):
        vp.Proxy = self
        vp.LineColor = (0.84, 0.15, 0.16, 0.0)
        vp.PointColor = vp.LineColor
        vp.LineWidth = 2.0

    def attach(self, vp):
        self.app_obj = vp.Object

    def getIcon(self):
        return ":/icons/Assembly_CreateSimulation.svg"

    def dumps(self):
        return None

    def loads(self, state):
        return None
