# SPDX-License-Identifier: LGPL-2.1-or-later

"""A fluid study's kept frames, and one of them drawn in the 3D view
(docs/FluidSimulation.md sec 11.4): particles as points, a surface as
triangles -- by what the frames are, not by the backend they came from."""

import bisect
import os

import FreeCAD as App

import FluidApp  # noqa: F401 -- the study's object types are its

translate = App.Qt.translate

WATER = (0.20, 0.50, 0.90)


def iconPath(name):
    return os.path.join(os.path.dirname(__file__), "Resources", "icons", name)


class Frames:
    """The results a study keeps, a frame read when it is asked for."""

    def __init__(self, study):
        self.study = study
        results = study.getResults()
        self.valid = bool(results) and results["Frames"] > 0
        self.times = list(results["Times"]) if self.valid else []
        self.kind = results["Kind"] if self.valid else ""
        self.backend = results["Backend"] if self.valid else ""
        self.monitors = results["Monitors"] if self.valid else []
        # the monitors' own times, far more than the frames
        self.monitorTimes = list(results["MonitorTimes"]) if self.valid else []

    def __len__(self):
        return len(self.times)

    def indexAt(self, time):
        """The last frame at or before a time; the first one before them all."""
        if not self.times:
            return -1
        return max(bisect.bisect_right(self.times, time + 1e-12) - 1, 0)

    def points(self, index):
        """The frame's points, (n, 3) float32, mm."""
        import numpy

        frame = self.study.getFrame(index)
        return numpy.frombuffer(frame["Points"], dtype=numpy.float32).reshape(-1, 3)

    def triangles(self, index):
        """A surface frame's triangles, (m, 3) int32; None for particles."""
        import numpy

        frame = self.study.getFrame(index)
        if "Triangles" not in frame:
            return None
        return numpy.frombuffer(frame["Triangles"], dtype=numpy.int32).reshape(-1, 3)

    def frame(self, index):
        """Its points and triangles, read once."""
        import numpy

        frame = self.study.getFrame(index)
        points = numpy.frombuffer(frame["Points"], dtype=numpy.float32).reshape(-1, 3)
        triangles = None
        if "Triangles" in frame:
            triangles = numpy.frombuffer(frame["Triangles"], dtype=numpy.int32).reshape(-1, 3)
        return points, triangles

    def sampleAt(self, time):
        """The monitors' last sample at or before a time; -1 without one."""
        if not self.monitorTimes:
            return -1
        return max(bisect.bisect_right(self.monitorTimes, time + 1e-12) - 1, 0)

    def monitorsAt(self, index):
        """Each monitor at a frame's time: (label, kind, value, unit)."""
        sample = self.sampleAt(self.times[index])
        rows = []
        for monitor in self.monitors:
            obj = monitor["Object"]
            label = obj.Label if obj is not None else monitor["Source"]
            value = monitor["Values"][sample] if sample >= 0 else float("nan")
            rows.append((label, monitor["Kind"], value, monitor["Unit"]))
        return rows


class FrameView:
    """One frame in the scene: under the node given, or the active 3D view's
    scene graph. Particles are points of a size on the screen; a surface is
    lit triangles."""

    def __init__(self, parent=None, color=WATER, pointSize=4.0):
        self.parent = parent
        self.color = color
        self.pointSize = pointSize
        self.root = None
        self.coords = None
        self.points = None
        self.faces = None
        self.switch = None

    def _parent(self):
        if self.parent is not None:
            return self.parent
        import FreeCADGui as Gui

        view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
        if view is None or not hasattr(view, "getSceneGraph"):
            return None
        self.parent = view.getSceneGraph()
        return self.parent

    def _build(self):
        from pivy import coin

        parent = self._parent()
        if parent is None:
            return False
        self.root = coin.SoSeparator()
        material = coin.SoMaterial()
        material.diffuseColor = self.color
        material.ambientColor = tuple(0.4 * c for c in self.color)
        material.specularColor = (0.3, 0.3, 0.3)
        material.shininess = 0.6
        self.coords = coin.SoCoordinate3()
        # particles: unlit points
        particles = coin.SoSeparator()
        light = coin.SoLightModel()
        light.model = coin.SoLightModel.BASE_COLOR
        style = coin.SoDrawStyle()
        style.pointSize = self.pointSize
        self.points = coin.SoPointSet()
        for node in (light, style, self.points):
            particles.addChild(node)
        # a surface: seen from both sides
        surface = coin.SoSeparator()
        hints = coin.SoShapeHints()
        # an open sheet, lit on both its sides: Coin lights both only when
        # it is told an ordering and that the shape is no solid
        hints.vertexOrdering = coin.SoShapeHints.COUNTERCLOCKWISE
        hints.shapeType = coin.SoShapeHints.UNKNOWN_SHAPE_TYPE
        hints.creaseAngle = 0.8
        self.faces = coin.SoIndexedFaceSet()
        for node in (hints, self.faces):
            surface.addChild(node)
        self.switch = coin.SoSwitch()
        self.switch.addChild(particles)
        self.switch.addChild(surface)
        for node in (material, self.coords, self.switch):
            self.root.addChild(node)
        parent.addChild(self.root)
        return True

    def show(self, points, triangles=None):
        """Draws the points (n, 3), as a surface when there are triangles
        (m, 3); what was drawn before is replaced. False without a view."""
        import numpy

        if self.root is None and not self._build():
            return False
        points = numpy.ascontiguousarray(points, dtype=numpy.float32).reshape(-1, 3)
        count = len(points)
        self.coords.point.setNum(count)
        if count:
            self.coords.point.setValues(0, count, points)
        if triangles is None:
            self.points.numPoints = count
            self.switch.whichChild = 0
        else:
            triangles = numpy.asarray(triangles, dtype=numpy.int32).reshape(-1, 3)
            index = numpy.full((len(triangles), 4), -1, dtype=numpy.int32)
            index[:, :3] = triangles
            index = index.reshape(-1)
            self.faces.coordIndex.setNum(len(index))
            if len(index):
                self.faces.coordIndex.setValues(0, len(index), index)
            self.switch.whichChild = 1
        return True

    def count(self):
        return self.coords.point.getNum() if self.coords is not None else 0

    def remove(self):
        if self.root is not None and self.parent is not None:
            self.parent.removeChild(self.root)
        self.root = None
        self.coords = None


def studyOf(obj):
    """The study an object is, or is a child of; None otherwise."""
    if obj is None:
        return None
    if obj.isDerivedFrom("Fluid::Study"):
        return obj
    for parent in obj.InList:
        if parent.isDerivedFrom("Fluid::Study") and obj in parent.Group:
            return parent
    return None


def activeStudy(selection=None):
    """The study to work on: the one selected, or a selected child's, else
    the document's only one."""
    doc = App.ActiveDocument
    if doc is None:
        return None
    for obj in selection or []:
        study = studyOf(obj)
        if study is not None:
            return study
    studies = [o for o in doc.Objects if o.isDerivedFrom("Fluid::Study")]
    return studies[0] if len(studies) == 1 else None


def solidsOf(objects):
    """Those of the objects that show a solid, in order."""
    import Part

    solids = []
    for obj in objects:
        if obj.isDerivedFrom("Fluid::Study") or studyOf(obj) is not None:
            continue
        try:
            shape = Part.getShape(obj)
        except Exception:
            continue
        if not shape.isNull() and shape.Solids:
            solids.append(obj)
    return solids


def makeStudy(doc, walls=()):
    """A new study with those parts as its walls."""
    study = doc.addObject("Fluid::Study", "FluidStudy")
    study.Label = translate("Fluid", "Fluid Study")
    study.Walls = list(walls)
    return study


def addFluid(study, volume):
    """The solid full of fluid when the study's run starts."""
    fluid = study.Document.addObject("Fluid::InitialFluid", "Fluid")
    fluid.Label = translate("Fluid", "Fluid in {}").format(volume.Label)
    fluid.Volume = volume
    study.Group = study.Group + [fluid]
    return fluid


def addMonitor(study, kind, point):
    """A monitor of that kind ('Height', 'Pressure') at a point."""
    monitor = study.Document.addObject("Fluid::Monitor", kind)
    monitor.Kind = kind
    monitor.Point = point
    study.Group = study.Group + [monitor]
    return monitor
