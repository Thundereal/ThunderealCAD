# SPDX-License-Identifier: LGPL-2.1-or-later

"""A fluid study's kept frames, and one of them drawn in the 3D view
(docs/FluidSimulation.md sec 11.4, 12.6): particles as points or as the
closed surface around them, a surface as triangles -- by what the frames
are, not by the backend they came from."""

import bisect
import os

import FreeCAD as App

import FluidApp  # noqa: F401 -- the study's object types are its

translate = App.Qt.translate

WATER = (0.10, 0.45, 0.80)


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
        # the parts the fluid moved; None for one that is gone
        self.bodies = [body["Object"] for body in results["Bodies"]] if self.valid else []

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

    def surface(self, index):
        """The frame as a closed surface to draw: its points, triangles,
        the points' normals and the spray. A frame of particles is wrapped,
        and the particles with next to none about them are no part of the
        surface: they are the spray, points (n, 3). A frame that is a
        surface already comes as it is, its normals and spray None."""
        import numpy

        frame = self.study.getSurface(index)
        points = numpy.frombuffer(frame["Points"], dtype=numpy.float32).reshape(-1, 3)
        triangles = numpy.frombuffer(frame["Triangles"], dtype=numpy.int32).reshape(-1, 3)
        normals = None
        spray = None
        if "Normals" in frame:
            normals = numpy.frombuffer(frame["Normals"], dtype=numpy.float32).reshape(-1, 3)
            spray = numpy.frombuffer(frame["Spray"], dtype=numpy.float32).reshape(-1, 3)
        return points, triangles, normals, spray

    def motions(self, index):
        """Where the moved parts are at a frame, as the bodies are listed:
        each the motion from where the part stood at the start."""
        return self.study.getBodies(index) if self.valid else []

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


class MovedParts:
    """The parts the fluid moved, put where a frame has them: by their own
    Placement, as an assembly's simulation shows its parts, and put back
    where they stood when the frames are done with. A run takes the parts
    where they are: put them back before one starts."""

    def __init__(self, frames):
        self.frames = frames
        self.parts = []
        for index, part in enumerate(frames.bodies):
            if part is None or not hasattr(part, "Placement"):
                continue
            own = App.Placement(part.Placement)
            try:
                whole = App.Placement(part.getGlobalPlacement())
            except Exception:
                whole = App.Placement(own)
            # what is above it: the placement its own is in
            above = whole.multiply(own.inverse())
            self.parts.append((index, part, own, whole, above.inverse()))
        self.moved = False

    def show(self, index):
        """The parts where the frame has them."""
        if not self.parts:
            return
        motions = self.frames.motions(index)
        for at, part, _own, whole, down in self.parts:
            if at < len(motions):
                part.Placement = down.multiply(motions[at].multiply(whole))
        self.moved = True

    def restore(self):
        """The parts where they stood."""
        if not self.moved:
            return
        for _at, part, own, _whole, _down in self.parts:
            try:
                part.Placement = own
            except Exception:
                pass
        self.moved = False


class FrameView:
    """One frame in the scene: under the node given, or the active 3D view's
    scene graph. Particles are points of a size on the screen; a surface is
    lit triangles; a closed surface with its normals is a smooth body, and
    a body of water to a renderer that draws one (the bgfx renderer with
    its water surface on: refraction, reflection, the light in its depth).
    The spray of a body is fine pale points, half seen through: thrown
    water at a particle's size is mist to the eye, not so many balls."""

    MIST = (0.85, 0.92, 1.0)
    MIST_SIZE = 1.0
    MIST_TRANSPARENCY = 0.75

    PARTICLES = 0
    SURFACE = 1
    BODY = 2

    def __init__(self, parent=None, color=WATER, pointSize=4.0, water=True):
        self.parent = parent
        self.color = color
        self.pointSize = pointSize
        self.water = water
        self.normals = None
        self.body = None
        self.root = None
        self.coords = None
        self.points = None
        self.faces = None
        self.switch = None
        self.mistCoords = None
        self.mist = None

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
        # a closed surface with a normal a point: a smooth body, one side
        body = coin.SoSeparator()
        hints = coin.SoShapeHints()
        hints.vertexOrdering = coin.SoShapeHints.COUNTERCLOCKWISE
        hints.shapeType = coin.SoShapeHints.SOLID
        self.normals = coin.SoNormal()
        binding = coin.SoNormalBinding()
        binding.value = coin.SoNormalBinding.PER_VERTEX_INDEXED
        self.body = coin.SoIndexedFaceSet()
        for node in (hints, self.waterNode(), self.normals, binding, self.body):
            if node is not None:
                body.addChild(node)
        self.switch = coin.SoSwitch()
        self.switch.addChild(particles)
        self.switch.addChild(surface)
        self.switch.addChild(body)
        for node in (material, self.coords, self.switch):
            self.root.addChild(node)
        # the spray: unlit points of their own colour
        spray = coin.SoSeparator()
        pale = coin.SoMaterial()
        pale.diffuseColor = self.MIST
        pale.emissiveColor = tuple(0.3 * c for c in self.MIST)
        pale.transparency = self.MIST_TRANSPARENCY
        light = coin.SoLightModel()
        light.model = coin.SoLightModel.BASE_COLOR
        style = coin.SoDrawStyle()
        style.pointSize = self.MIST_SIZE
        self.mistCoords = coin.SoCoordinate3()
        self.mist = coin.SoPointSet()
        self.mist.numPoints = 0
        for node in (pale, light, style, self.mistCoords, self.mist):
            spray.addChild(node)
        self.root.addChild(spray)
        parent.addChild(self.root)
        return True

    def waterNode(self):
        """The node that says the shape after it is a body of water, to the
        renderer that knows of one; None where there is no such node (no
        GUI) or water was not asked for."""
        from pivy import coin

        if not self.water:
            return None
        kind = coin.SoType.fromName("SoFCRenderMaterial")
        if kind.isBad() or not kind.canCreateInstance():
            return None
        node = kind.createInstance()
        # by name: the node is the GUI's own and pivy knows only its base
        node.set("water TRUE waterDensity 0")
        return node

    def show(self, points, triangles=None, normals=None, spray=None):
        """Draws the points (n, 3): as a surface when there are triangles
        (m, 3), as a closed body when there are the points' normals (n, 3)
        too, and the body's spray (k, 3) as mist; what was drawn before is
        replaced. False without a view."""
        import numpy

        if self.root is None and not self._build():
            return False
        count = 0
        if spray is not None and len(spray):
            spray = numpy.ascontiguousarray(spray, dtype=numpy.float32).reshape(-1, 3)
            count = len(spray)
            self.mistCoords.point.setNum(count)
            self.mistCoords.point.setValues(0, count, spray)
        self.mist.numPoints = count
        points = numpy.ascontiguousarray(points, dtype=numpy.float32).reshape(-1, 3)
        count = len(points)
        self.coords.point.setNum(count)
        if count:
            self.coords.point.setValues(0, count, points)
        if triangles is None:
            self.points.numPoints = count
            self.switch.whichChild = self.PARTICLES
            return True
        triangles = numpy.asarray(triangles, dtype=numpy.int32).reshape(-1, 3)
        index = numpy.full((len(triangles), 4), -1, dtype=numpy.int32)
        index[:, :3] = triangles
        index = index.reshape(-1)
        faces = self.faces if normals is None else self.body
        # the other set keeps nothing of an earlier frame
        other = self.body if normals is None else self.faces
        other.coordIndex.setNum(0)
        faces.coordIndex.setNum(len(index))
        if len(index):
            faces.coordIndex.setValues(0, len(index), index)
        if normals is None:
            self.switch.whichChild = self.SURFACE
            return True
        normals = numpy.ascontiguousarray(normals, dtype=numpy.float32).reshape(-1, 3)
        self.normals.vector.setNum(len(normals))
        if len(normals):
            self.normals.vector.setValues(0, len(normals), normals)
        self.switch.whichChild = self.BODY
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


def addBody(study, part, density):
    """The part as one the fluid moves, of that density (kg/m^3)."""
    body = study.Document.addObject("Fluid::FreeBody", "Body")
    body.Label = translate("Fluid", "{} moved by the fluid").format(part.Label)
    body.Part = part
    body.Density = App.Units.Quantity(density, "kg/m^3")
    study.Group = study.Group + [body]
    return body


def mechanismsOf(doc):
    """The document's Assembly simulations, which a study can run together
    with, in the document's order."""
    return [obj for obj in doc.Objects if obj.isDerivedFrom("Assembly::Simulation")]


def linkMechanism(study, simulation):
    """The study run together with that Assembly simulation, or with none:
    the parts the fluid moves are then the mechanism's. Linked, the
    simulation ends when the study does and keeps its frames on the same
    times, the two runs being one."""
    study.Mechanism = simulation
    if simulation is not None:
        simulation.TimeEnd = study.TimeEnd
        simulation.TimeStepOutput = study.OutputInterval


def addInlet(study, part, face, speed):
    """The face (by its name, 'Face2') of the part as one the fluid comes
    in through, at that speed (mm/s), out of the part's solid."""
    inlet = study.Document.addObject("Fluid::Inlet", "Inlet")
    inlet.Label = translate("Fluid", "Inlet at {} of {}").format(face, part.Label)
    inlet.Face = (part, [face])
    inlet.Velocity = App.Units.Quantity(speed, "mm/s")
    study.Group = study.Group + [inlet]
    return inlet


def addMonitor(study, kind, point):
    """A monitor of that kind ('Height', 'Pressure') at a point."""
    monitor = study.Document.addObject("Fluid::Monitor", kind)
    monitor.Kind = kind
    monitor.Point = point
    study.Group = study.Group + [monitor]
    return monitor


def addForceMonitor(study, part, direction):
    """A monitor of the fluid's force on a wall of the study, along a
    direction."""
    monitor = study.Document.addObject("Fluid::Monitor", "Force")
    monitor.Label = translate("Fluid", "Force on {}").format(part.Label)
    monitor.Kind = "Force"
    monitor.Part = part
    monitor.Direction = direction
    study.Group = study.Group + [monitor]
    return monitor


def forceDirection(study):
    """The way a new force monitor looks: the way the study's first inlet
    sends its fluid, or with none against gravity, or with neither +Z."""
    try:
        inlets = study.buildModel()["Inlets"]
    except Exception:
        inlets = []
    if inlets:
        return App.Vector(*inlets[0]["Direction"])
    up = -study.Gravity
    if up.Length > 0:
        up.normalize()
        return up
    return App.Vector(0, 0, 1)
