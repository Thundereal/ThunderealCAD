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

# The water's colour: what it lets through, and what it scatters back, by
# depth (the renderer's Beer-Lambert absorption). Pale, so that a film is
# next to clear and 100 mm a light blue; (0.10, 0.45, 0.80) before, toned
# down at the user's word (2026-11-25).
WATER = (0.40, 0.70, 0.90)


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
        # of a steady flow: the frames' times are the steps it had settled
        # for; and its openings
        self.steady = bool(results.get("Steady")) if self.valid else False
        self.openings = results.get("Openings", []) if self.valid else []
        # the frames that have the fluid inside kept, to cut through
        self.inside = list(results.get("Inside", [])) if self.valid else []
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

    def field(self, index):
        """A frame of fields: its points, its triangles and the pressure at
        each point, (n,) float32, Pa."""
        import numpy

        frame = self.study.getFrame(index)
        points = numpy.frombuffer(frame["Points"], dtype=numpy.float32).reshape(-1, 3)
        triangles = numpy.frombuffer(frame["Triangles"], dtype=numpy.int32).reshape(-1, 3)
        values = numpy.frombuffer(frame.get("Values", b""), dtype=numpy.float32)
        return points, triangles, values

    def insideOf(self, index):
        """The fluid inside at a frame that has it kept: each cell's centre,
        (n, 3) float32, mm, its velocity there, (n, 3), m/s, and its
        pressure, (n,), Pa. Of a frame without, none of each."""
        import numpy

        inside = self.study.getInside(index)
        points = numpy.frombuffer(inside["Points"], dtype=numpy.float32).reshape(-1, 3)
        velocity = numpy.frombuffer(inside["Velocity"], dtype=numpy.float32).reshape(-1, 3)
        pressure = numpy.frombuffer(inside["Pressure"], dtype=numpy.float32)
        return points, velocity, pressure

    def section(self, index, base, normal, step=0.0):
        """A flat cut through the fluid inside at a frame, by the plane
        through 'base' (mm) across 'normal': a Cut, with no points where
        the plane misses the fluid or the frame has none kept."""
        import numpy

        cut = self.study.getSection(index, base, normal, step)

        def floats(name, width):
            return numpy.frombuffer(cut[name], dtype=numpy.float32).reshape(-1, width)

        return Cut(
            floats("Points", 3),
            numpy.frombuffer(cut["Triangles"], dtype=numpy.int32).reshape(-1, 3),
            floats("Velocity", 3),
            numpy.frombuffer(cut["Pressure"], dtype=numpy.float32),
            floats("Outline", 3),
            numpy.frombuffer(cut["Grid"], dtype=numpy.int32).reshape(-1, 2),
            cut["Step"],
            tuple(base),
            tuple(normal),
        )

    def paths(self, index, seeds, interval=0.0, most=2000):
        """Tracers let go at the seeds (n, 3), mm, in the fluid inside at a
        frame and followed through it: a Paths, with none of a frame that
        has no fluid inside kept."""
        import numpy

        seeds = numpy.ascontiguousarray(seeds, dtype=numpy.float32).reshape(-1, 3)
        found = self.study.getPaths(index, seeds.tobytes(), interval, most)
        return Paths(
            numpy.frombuffer(found["Points"], dtype=numpy.float32).reshape(-1, 3),
            numpy.frombuffer(found["Speeds"], dtype=numpy.float32),
            numpy.frombuffer(found["Starts"], dtype=numpy.int32),
            found["Interval"],
        )

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

    def openingsAt(self, index):
        """Each opening at a frame: (label, kind, its mean pressure Pa, what
        goes through it m^3/s)."""
        rows = []
        for opening in self.openings:
            obj = opening["Object"]
            label = obj.Label if obj is not None else opening["Source"]
            rows.append(
                (label, opening["Kind"], opening["Pressure"][index], opening["Flow"][index])
            )
        return rows

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


class Cut:
    """A flat cut through the fluid: a grid of points on the plane, those
    inside the fluid, (n, 3) mm; the triangles between them, (m, 3); the
    velocity, (n, 3) m/s, and the pressure, (n,) Pa, at each; the outline,
    (2k, 3) mm, two ends a piece of where the plane cuts the faces; where
    on the grid each point is, (n, 2); the grid's step, mm; and the plane,
    a point of it and what it lies across."""

    def __init__(self, points, triangles, velocity, pressure, outline, grid, step, base, across):
        self.base = base
        self.across = across
        self.points = points
        self.triangles = triangles
        self.velocity = velocity
        self.pressure = pressure
        self.outline = outline
        self.grid = grid
        self.step = step

    def __len__(self):
        return len(self.points)

    def speed(self):
        """How fast the fluid goes at each point, (n,) m/s."""
        return speedOf(self.velocity)

    def normal(self):
        """What the plane lies across, of length one."""
        import numpy

        across = numpy.array(self.across, dtype=numpy.float64)
        return across / numpy.sqrt((across**2).sum())

    def axis(self):
        """A direction in the plane, of length one."""
        import numpy

        normal = self.normal()
        other = numpy.zeros(3)
        other[int(numpy.argmin(abs(normal)))] = 1.0
        axis = numpy.cross(normal, other)
        return axis / numpy.sqrt((axis**2).sum())


class Paths:
    """Tracers followed through a steady flow: where each is at even times,
    points (n, 3) float32 in mm, one path after another; how fast the fluid
    goes there, speeds (n,), m/s; where each path starts in them and after
    the last where they end, starts (paths + 1,); and the time from a point
    to the next, interval, s. Played, a tracer is let go at each seed every
    so many points, and each goes a point a tick: fast where the flow is."""

    def __init__(self, points, speeds, starts, interval):
        import numpy

        self.points = points
        self.speeds = speeds
        self.starts = starts
        self.interval = interval
        # how far along its own path each point is
        self.along = numpy.zeros(len(speeds), dtype=numpy.int64)
        # and how many points the path it is of has
        self.length = numpy.ones(len(speeds), dtype=numpy.int64)
        for path in range(len(self)):
            first, after = int(starts[path]), int(starts[path + 1])
            self.along[first:after] = numpy.arange(after - first)
            self.length[first:after] = after - first

    def __len__(self):
        return max(len(self.starts) - 1, 0)

    def longest(self):
        """The most points a path has."""
        return int(self.along.max()) + 1 if len(self.along) else 0

    def at(self, tick, every, most=0):
        """The tracers there are at a tick, one let go at each seed each
        'every' ticks since ever: their places (m, 3) and the fluid's speed
        at each (m,). With 'most', no more than so many on a path at once:
        on a path longer than 'most' times 'every' points -- one through
        slow fluid, or round and round -- they are let go less often, so
        that they do not run into a line."""
        import numpy

        every = max(int(every), 1)
        if most > 0:
            every = numpy.maximum(every, -(-self.length // int(most)))
        there = (self.along - tick) % every == 0
        return self.points[there], self.speeds[there]

    def lines(self):
        """The paths as lines to draw: the points, and how many each line
        has -- the paths of one point left out."""
        import numpy

        counts = numpy.diff(self.starts) if len(self) else numpy.zeros(0, dtype=numpy.int32)
        keep = numpy.repeat(counts > 1, counts)
        return self.points[keep], counts[counts > 1].astype(numpy.int32)


class Streaks:
    """Tracers carried by a flow through time: one let go at each seed at
    every frame that has the fluid inside kept, and each carried from a
    frame to the next by the flow of the frame it leaves -- what smoke let
    out at the seeds would draw. 'frames' is a Frames (its inside, times,
    insideOf and paths are what is asked of it), the seeds (n, 3) in mm,
    'cell' the solver's cell, mm. A frame's tracers are found when first
    asked for, from the nearest frame before that has them."""

    def __init__(self, frames, seeds, cell):
        import numpy

        self.frames = frames
        self.seeds = numpy.ascontiguousarray(seeds, dtype=numpy.float32).reshape(-1, 3)
        self.cell = float(cell)
        self.order = sorted(frames.inside)
        self.held = {}

    def _placed(self, index, points):
        """The points that are in the fluid at a frame, and its speed at
        each."""
        found = self.frames.paths(index, points, 1.0, 1)
        return found.points, found.speeds

    def _carried(self, index, points, time):
        """Where the frame's flow has the points after a time, s: those
        that left the fluid, or stand still, are gone."""
        import numpy

        if len(points) == 0 or not time > 0.0:
            return points
        _cells, velocity, _pressure = self.frames.insideOf(index)
        fastest = float(speedOf(velocity).max()) if len(velocity) else 0.0
        # half a cell a step at the fastest
        steps = int(numpy.ceil(time * fastest * 1000.0 / (0.5 * self.cell))) if fastest else 1
        steps = min(max(steps, 1), 400)
        found = self.frames.paths(index, points, time / steps, steps + 1)
        ends = found.starts[1:]
        whole = numpy.diff(found.starts) == steps + 1
        return found.points[ends[whole] - 1]

    def at(self, index):
        """The tracers there are at a frame: their places (m, 3), mm, and
        the fluid's speed at each (m,), m/s. None of a frame without the
        fluid inside."""
        import numpy

        none = numpy.zeros((0, 3), numpy.float32), numpy.zeros(0, numpy.float32)
        if index not in self.order or len(self.seeds) == 0:
            return none
        place = self.order.index(index)
        start = place
        while start > 0 and self.order[start] not in self.held:
            start -= 1
        if self.order[start] not in self.held:
            self.held[self.order[start]] = self._placed(self.order[start], self.seeds)
        for k in range(start, place):
            here, there = self.order[k], self.order[k + 1]
            moved = self._carried(
                here, self.held[here][0], self.frames.times[there] - self.frames.times[here]
            )
            self.held[there] = self._placed(there, numpy.concatenate([moved, self.seeds]))
        return self.held[index]


class MovedParts:
    """The parts the fluid moved, put where a frame has them: by their own
    Placement, as an assembly's simulation shows its parts, and put back
    where they stood when the frames are done with. A run takes the parts
    where they are: put them back before one starts.

    A study run with a mechanism (its Mechanism, an Assembly's simulation)
    moves more than the parts the fluid pushes: the whole assembly is put
    where the Assembly's run has it at each frame's time -- the parts the
    fluid does not touch with the ones it does -- when the simulation keeps
    the run that ends where the fluid's does."""

    def __init__(self, frames):
        self.frames = frames
        self.parts = []
        self.assembly = None
        self.placed = {}
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
        self._mechanism()
        self.moved = False

    def _mechanism(self):
        """The assembly whose run this is, its parts' placements kept."""
        simulation = getattr(self.frames.study, "Mechanism", None)
        if simulation is None or not self.frames.valid:
            return
        assembly = None
        for obj in simulation.InListRecursive:
            if obj.isDerivedFrom("Assembly::AssemblyObject"):
                assembly = obj
                break
        if assembly is None:
            return
        try:
            import UtilsAssembly

            if not assembly.loadSimulationResults(simulation):
                return
            times = list(assembly.getSimulationResults().get("Times", []))
        except Exception:
            return
        # the same run: it ends where the fluid's does, to a frame
        step = self.frames.times[1] - self.frames.times[0] if len(self.frames) > 1 else 0.0
        if not times or abs(times[-1] - self.frames.times[-1]) > max(step, 1e-9):
            return
        self.assembly = assembly
        self.placed = UtilsAssembly.saveAssemblyPartsPlacements(assembly)

    def show(self, index):
        """The parts where the frame has them."""
        if self.assembly is not None:
            self.assembly.updateForTime(self.frames.times[index])
            self.moved = True
            return
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
        if self.assembly is not None:
            import UtilsAssembly

            try:
                UtilsAssembly.restoreAssemblyPartsPlacements(self.assembly, self.placed)
            except Exception:
                pass
            self.moved = False
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
    FIELD = 3

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
        self.colors = None
        self.field = None

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
        # a field: the faces in the colours of the values at their corners
        field = coin.SoSeparator()
        hints = coin.SoShapeHints()
        hints.vertexOrdering = coin.SoShapeHints.COUNTERCLOCKWISE
        hints.shapeType = coin.SoShapeHints.UNKNOWN_SHAPE_TYPE
        hints.creaseAngle = 0.8
        self.colors = coin.SoMaterial()
        self.colors.specularColor = (0.1, 0.1, 0.1)
        binding = coin.SoMaterialBinding()
        binding.value = coin.SoMaterialBinding.PER_VERTEX_INDEXED
        self.field = coin.SoIndexedFaceSet()
        for node in (hints, self.colors, binding, self.field):
            field.addChild(node)
        self.switch = coin.SoSwitch()
        self.switch.addChild(particles)
        self.switch.addChild(surface)
        self.switch.addChild(body)
        self.switch.addChild(field)
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
        # the other sets keep nothing of an earlier frame
        other = self.body if normals is None else self.faces
        other.coordIndex.setNum(0)
        self.field.coordIndex.setNum(0)
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

    def showField(self, points, triangles, values, low, high):
        """Draws the triangles (m, 3) of the points (n, 3) in the colours of
        the values at the points (n,), 'low' the ramp's first colour and
        'high' its last; what was drawn before is replaced. False without a
        view."""
        import numpy

        if self.root is None and not self._build():
            return False
        self.mist.numPoints = 0
        points = numpy.ascontiguousarray(points, dtype=numpy.float32).reshape(-1, 3)
        count = len(points)
        self.coords.point.setNum(count)
        colors = colorsOf(values, low, high)
        self.colors.diffuseColor.setNum(len(colors))
        if count:
            self.coords.point.setValues(0, count, points)
            self.colors.diffuseColor.setValues(0, len(colors), colors)
        triangles = numpy.asarray(triangles, dtype=numpy.int32).reshape(-1, 3)
        index = numpy.full((len(triangles), 4), -1, dtype=numpy.int32)
        index[:, :3] = triangles
        index = index.reshape(-1)
        for other in (self.faces, self.body):
            other.coordIndex.setNum(0)
        self.field.coordIndex.setNum(len(index))
        if len(index):
            self.field.coordIndex.setValues(0, len(index), index)
        self.switch.whichChild = self.FIELD
        return True

    def count(self):
        return self.coords.point.getNum() if self.coords is not None else 0

    def remove(self):
        if self.root is not None and self.parent is not None:
            self.parent.removeChild(self.root)
        self.root = None
        self.coords = None


class CutView:
    """A cut through the fluid in the scene: its triangles in the colours
    of a value at their corners, its outline, and arrows of the flow. Under
    the node given, or the active 3D view's scene graph."""

    INK = (0.05, 0.05, 0.05)

    def __init__(self, parent=None):
        self.parent = parent
        self.root = None
        self.coords = None
        self.colors = None
        self.faces = None
        self.edgeCoords = None
        self.edges = None
        self.arrowCoords = None
        self.arrows = None

    def _build(self):
        from pivy import coin

        if self.parent is None:
            import FreeCADGui as Gui

            view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
            if view is None or not hasattr(view, "getSceneGraph"):
                return False
            self.parent = view.getSceneGraph()
        self.root = coin.SoSeparator()
        # the values are read off the colours: no light changes them, and
        # the lines over the faces are not to fight them for the depth
        light = coin.SoLightModel()
        light.model = coin.SoLightModel.BASE_COLOR
        faces = coin.SoSeparator()
        offset = coin.SoPolygonOffset()
        hints = coin.SoShapeHints()
        hints.vertexOrdering = coin.SoShapeHints.COUNTERCLOCKWISE
        hints.shapeType = coin.SoShapeHints.UNKNOWN_SHAPE_TYPE
        self.coords = coin.SoCoordinate3()
        self.colors = coin.SoMaterial()
        binding = coin.SoMaterialBinding()
        binding.value = coin.SoMaterialBinding.PER_VERTEX_INDEXED
        self.faces = coin.SoIndexedFaceSet()
        for node in (offset, hints, self.coords, self.colors, binding, self.faces):
            faces.addChild(node)
        lines = coin.SoSeparator()
        ink = coin.SoMaterial()
        ink.diffuseColor = self.INK
        style = coin.SoDrawStyle()
        style.lineWidth = 1.5
        self.edgeCoords = coin.SoCoordinate3()
        self.edges = coin.SoLineSet()
        self.arrowCoords = coin.SoCoordinate3()
        self.arrows = coin.SoIndexedLineSet()
        for node in (ink, style, self.edgeCoords, self.edges, self.arrowCoords, self.arrows):
            lines.addChild(node)
        for node in (light, faces, lines):
            self.root.addChild(node)
        self.parent.addChild(self.root)
        return True

    def show(self, cut, values, low, high, arrows=None):
        """Draws the cut in the colours of the values at its points (n,),
        'low' the ramp's first colour and 'high' its last, and the arrows
        given as arrowsOf() makes them; what was drawn before is replaced.
        False without a view."""
        import numpy

        if self.root is None and not self._build():
            return False
        points = numpy.ascontiguousarray(cut.points, dtype=numpy.float32).reshape(-1, 3)
        colors = colorsOf(values, low, high)
        self.coords.point.setNum(len(points))
        self.colors.diffuseColor.setNum(len(colors))
        if len(points):
            self.coords.point.setValues(0, len(points), points)
            self.colors.diffuseColor.setValues(0, len(colors), colors)
        index = numpy.full((len(cut.triangles), 4), -1, dtype=numpy.int32)
        index[:, :3] = cut.triangles
        index = index.reshape(-1)
        self.faces.coordIndex.setNum(len(index))
        if len(index):
            self.faces.coordIndex.setValues(0, len(index), index)
        outline = numpy.ascontiguousarray(cut.outline, dtype=numpy.float32).reshape(-1, 3)
        self.edgeCoords.point.setNum(len(outline))
        self.edges.numVertices.setNum(len(outline) // 2)
        if len(outline):
            self.edgeCoords.point.setValues(0, len(outline), outline)
            self.edges.numVertices.setValues(
                0, len(outline) // 2, numpy.full(len(outline) // 2, 2, dtype=numpy.int32)
            )
        tips, strokes = arrows if arrows is not None else arrowsOf(cut, 1, 0.0)
        self.arrowCoords.point.setNum(len(tips))
        self.arrows.coordIndex.setNum(len(strokes))
        if len(strokes):
            self.arrowCoords.point.setValues(0, len(tips), tips)
            self.arrows.coordIndex.setValues(0, len(strokes), strokes)
        return True

    def count(self):
        return self.coords.point.getNum() if self.coords is not None else 0

    def arrowCount(self):
        """How many arrows are drawn: each is seven places of its lines."""
        return self.arrows.coordIndex.getNum() // 7 if self.arrows is not None else 0

    def remove(self):
        if self.root is not None and self.parent is not None:
            self.parent.removeChild(self.root)
        self.root = None
        self.coords = None
        self.arrows = None


class TracerView:
    """Tracers in the scene: points in the colours of how fast the fluid
    carries them, and the paths they go by as faint lines. Under the node
    given, or the active 3D view's scene graph."""

    FAINT = (0.45, 0.45, 0.45)

    def __init__(self, parent=None, pointSize=4.0):
        self.parent = parent
        self.pointSize = pointSize
        self.root = None
        self.coords = None
        self.colors = None
        self.points = None
        self.lineCoords = None
        self.lines = None

    def _build(self):
        from pivy import coin

        if self.parent is None:
            import FreeCADGui as Gui

            view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
            if view is None or not hasattr(view, "getSceneGraph"):
                return False
            self.parent = view.getSceneGraph()
        self.root = coin.SoSeparator()
        light = coin.SoLightModel()
        light.model = coin.SoLightModel.BASE_COLOR
        lines = coin.SoSeparator()
        faint = coin.SoMaterial()
        faint.diffuseColor = self.FAINT
        faint.transparency = 0.6
        self.lineCoords = coin.SoCoordinate3()
        self.lines = coin.SoLineSet()
        for node in (faint, self.lineCoords, self.lines):
            lines.addChild(node)
        dots = coin.SoSeparator()
        style = coin.SoDrawStyle()
        style.pointSize = self.pointSize
        self.coords = coin.SoCoordinate3()
        self.colors = coin.SoMaterial()
        binding = coin.SoMaterialBinding()
        binding.value = coin.SoMaterialBinding.PER_VERTEX
        self.points = coin.SoPointSet()
        for node in (style, self.coords, self.colors, binding, self.points):
            dots.addChild(node)
        for node in (light, lines, dots):
            self.root.addChild(node)
        self.parent.addChild(self.root)
        return True

    def showPaths(self, paths):
        """Draws the lines the tracers go by; None or no paths: none. False
        without a view."""
        import numpy

        if self.root is None and not self._build():
            return False
        points, counts = paths.lines() if paths is not None else ((), ())
        points = numpy.ascontiguousarray(points, dtype=numpy.float32).reshape(-1, 3)
        self.lineCoords.point.setNum(len(points))
        self.lines.numVertices.setNum(len(counts))
        if len(points):
            self.lineCoords.point.setValues(0, len(points), points)
            self.lines.numVertices.setValues(0, len(counts), counts)
        return True

    def show(self, points, speeds, low, high):
        """Draws the tracers at the points (n, 3) in the colours of the
        speeds (n,), 'low' the ramp's first colour and 'high' its last;
        those drawn before are replaced. False without a view."""
        import numpy

        if self.root is None and not self._build():
            return False
        points = numpy.ascontiguousarray(points, dtype=numpy.float32).reshape(-1, 3)
        colors = colorsOf(speeds, low, high)
        self.coords.point.setNum(len(points))
        self.colors.diffuseColor.setNum(max(len(colors), 1))
        if len(points):
            self.coords.point.setValues(0, len(points), points)
            self.colors.diffuseColor.setValues(0, len(colors), colors)
        self.points.numPoints = len(points)
        return True

    def count(self):
        return self.points.numPoints.getValue() if self.points is not None else 0

    def lineCount(self):
        return self.lines.numVertices.getNum() if self.lines is not None else 0

    def remove(self):
        if self.root is not None and self.parent is not None:
            self.parent.removeChild(self.root)
        self.root = None
        self.points = None
        self.lines = None


# From the least to the most: Google's Turbo, a rainbow whose brightness
# goes evenly, at eleven places of it.
RAMP = (
    (0.190, 0.072, 0.232),
    (0.271, 0.345, 0.796),
    (0.259, 0.596, 0.997),
    (0.118, 0.812, 0.839),
    (0.165, 0.949, 0.624),
    (0.467, 0.996, 0.341),
    (0.741, 0.957, 0.204),
    (0.953, 0.796, 0.200),
    (0.992, 0.561, 0.141),
    (0.906, 0.290, 0.051),
    (0.647, 0.086, 0.004),
)


def colorsOf(values, low, high):
    """A colour for each value, (n, 3) float32: the ramp's first at 'low'
    and under, its last at 'high' and over."""
    import numpy

    values = numpy.asarray(values, dtype=numpy.float64)
    span = high - low
    share = (values - low) / span if span > 0.0 else numpy.zeros(len(values))
    share = numpy.clip(numpy.nan_to_num(share), 0.0, 1.0)
    ramp = numpy.array(RAMP)
    at = numpy.linspace(0.0, 1.0, len(ramp))
    colors = numpy.stack([numpy.interp(share, at, ramp[:, k]) for k in range(3)], axis=1)
    return numpy.ascontiguousarray(colors, dtype=numpy.float32)


def rangeOf(values):
    """The least and the most of a field's values; (0, 0) of none."""
    if len(values) == 0:
        return 0.0, 0.0
    return float(values.min()), float(values.max())


def speedOf(velocity):
    """The lengths of the velocities (n, 3): (n,)."""
    import numpy

    velocity = numpy.asarray(velocity, dtype=numpy.float64).reshape(-1, 3)
    return numpy.sqrt((velocity**2).sum(axis=1))


def behind(points, triangles, base, normal):
    """The triangles (m, 3) with no corner of the points (n, 3) past the
    plane through 'base' on the side its 'normal' points to: what is left
    of the faces when the fluid is cut open there."""
    import numpy

    points = numpy.asarray(points, dtype=numpy.float64).reshape(-1, 3)
    triangles = numpy.asarray(triangles, dtype=numpy.int32).reshape(-1, 3)
    off = (points - numpy.array(tuple(base))) @ numpy.array(tuple(normal))
    return triangles[(off[triangles] <= 1e-6).all(axis=1)]


def seedsAt(points, velocity, middle, direction, cell, count):
    """Where to let tracers go at an inlet: of the cells' centres, points
    (n, 3) in mm with the velocity there (n, 3), those within a cell of the
    plane through 'middle' (mm) across 'direction' where the fluid goes the
    inlet's way -- the first cells inside the opening -- thinned evenly to
    some 'count' of them. (m, 3) float32; none when no cell is there."""
    import numpy

    points = numpy.asarray(points, dtype=numpy.float32).reshape(-1, 3)
    way = numpy.asarray(direction, dtype=numpy.float64)
    way = way / max(float(numpy.linalg.norm(way)), 1.0e-30)
    past = (points - numpy.asarray(middle, dtype=numpy.float64)) @ way
    going = numpy.asarray(velocity, dtype=numpy.float64).reshape(-1, 3) @ way
    first = points[(past > 0.0) & (past < cell) & (going > 0.0)]
    if len(first) <= count or count <= 0:
        return numpy.ascontiguousarray(first)
    return numpy.ascontiguousarray(
        first[numpy.linspace(0, len(first) - 1, count).round().astype(int)]
    )


def seedsAlong(points, origin, direction, cell, count):
    """Where to let tracers go along a line of sight, from 'origin' (mm)
    along 'direction', through the fluid whose cells' centres are points
    (n, 3) in mm, 'cell' their side: places on the line at the depths of
    the cells it passes through, thinned evenly to 'count' of them, the
    first met first. (m, 3) float32; none where the line misses the fluid."""
    import numpy

    none = numpy.zeros((0, 3), dtype=numpy.float32)
    points = numpy.asarray(points, dtype=numpy.float64).reshape(-1, 3)
    start = numpy.asarray(tuple(origin), dtype=numpy.float64)
    way = numpy.asarray(tuple(direction), dtype=numpy.float64)
    length = float(numpy.linalg.norm(way))
    if not len(points) or not length > 0.0 or not cell > 0.0:
        return none
    way = way / length
    off = points - start
    depth = off @ way
    aside = numpy.sqrt(numpy.maximum((off * off).sum(axis=1) - depth * depth, 0.0))
    # the cells whose centres the line passes within half a side of, or
    # with none so near the nearest within the cell's half diagonal
    passed = aside <= 0.5 * cell
    if not passed.any():
        passed = aside <= 0.87 * cell
    if not passed.any():
        return none
    layer = numpy.floor(depth[passed] / cell).astype(numpy.int64)
    exact = numpy.array([depth[passed][layer == d].mean() for d in numpy.unique(layer)])
    if 0 < count < len(exact):
        exact = exact[numpy.linspace(0, len(exact) - 1, count).round().astype(int)]
    return numpy.ascontiguousarray((start + exact[:, None] * way).astype(numpy.float32))


def sightOnPlane(origin, direction, base, normal):
    """Where a line of sight from 'origin' along 'direction' meets the
    plane through 'base' across 'normal', an App.Vector; None when it goes
    along the plane or the plane is behind."""
    origin, direction = App.Vector(*tuple(origin)), App.Vector(*tuple(direction))
    base, normal = App.Vector(*tuple(base)), App.Vector(*tuple(normal))
    closing = direction.dot(normal)
    if abs(closing) < 1.0e-12 * max(direction.Length * normal.Length, 1.0e-30):
        return None
    along = (base - origin).dot(normal) / closing
    if along < 0.0:
        return None
    return origin + direction * along


def seedsAbout(points, place, normal, cell):
    """Where to let tracers go at a place (mm) on a plane across 'normal':
    the place and one a cell's side from it each way in the plane, those
    of them in the fluid -- within a cell's half diagonal of one of the
    cells' centres, points (n, 3). (m, 3) float32, the place first."""
    import numpy

    none = numpy.zeros((0, 3), dtype=numpy.float32)
    points = numpy.asarray(points, dtype=numpy.float64).reshape(-1, 3)
    across = numpy.asarray(tuple(normal), dtype=numpy.float64)
    length = float(numpy.linalg.norm(across))
    if not len(points) or not length > 0.0:
        return none
    across = across / length
    other = numpy.eye(3)[int(numpy.abs(across).argmin())]
    one = numpy.cross(across, other)
    one = one / numpy.linalg.norm(one)
    two = numpy.cross(across, one)
    middle = numpy.asarray(tuple(place), dtype=numpy.float64)
    asked = [middle] + [middle + cell * step for step in (one, -one, two, -two)]
    kept = [
        at for at in asked if float(((points - at) ** 2).sum(axis=1).min()) <= (0.87 * cell) ** 2
    ]
    if not kept:
        return none
    return numpy.ascontiguousarray(numpy.array(kept, dtype=numpy.float32))


def facingAway(points, triangles, looking):
    """Of the triangles (m, 3) of a closed surface wound outwards, those
    that face away from one who looks along 'looking': the walls on the
    other side of the fluid, with what is inside it seen before them."""
    import numpy

    points = numpy.asarray(points, dtype=numpy.float64).reshape(-1, 3)
    triangles = numpy.asarray(triangles, dtype=numpy.int32).reshape(-1, 3)
    if not len(triangles):
        return triangles
    a, b, c = (points[triangles[:, k]] for k in range(3))
    out = numpy.cross(b - a, c - a)
    return triangles[out @ numpy.asarray(tuple(looking), dtype=numpy.float64) > 0.0]


def arrowsOf(cut, every, fastest):
    """Arrows of the cut's velocities at every 'every'th point of its grid
    each way, the fastest 'fastest' long, mm: their points (k, 3) float32
    and, for a set of lines, the places of each line's points with -1
    after it. An arrow is its shaft and a head of two strokes; where the
    fluid stands still there is none."""
    import numpy

    none = numpy.zeros((0, 3), dtype=numpy.float32), numpy.zeros(0, dtype=numpy.int32)
    if len(cut) == 0:
        return none
    speed = cut.speed()
    most = float(speed.max())
    every = max(int(every), 1)
    taken = ((cut.grid % every) == 0).all(axis=1) & (speed > 1e-3 * most)
    if most <= 0.0 or not fastest > 0.0 or not taken.any():
        return none
    start = cut.points[taken].astype(numpy.float64)
    shaft = cut.velocity[taken].astype(numpy.float64) * (fastest / most)
    tip = start + shaft
    # the head's strokes lie across the shaft: in the plane where the flow
    # is in it, and for a flow straight through the plane, along the grid
    lengths = numpy.sqrt((shaft**2).sum(axis=1))[:, None]
    along = shaft / lengths
    side = numpy.cross(along, cut.normal())
    flat = numpy.sqrt((side**2).sum(axis=1)) < 0.2
    side[flat] = numpy.cross(along[flat], cut.axis())
    side /= numpy.sqrt((side**2).sum(axis=1))[:, None]
    back = tip - 0.3 * shaft
    left = back + 0.12 * lengths * side
    right = back - 0.12 * lengths * side
    count = len(start)
    points = numpy.concatenate([start, tip, left, right]).astype(numpy.float32)
    at = numpy.arange(count, dtype=numpy.int32)
    lines = numpy.stack(
        [
            at,
            at + count,
            numpy.full(count, -1, dtype=numpy.int32),
            at + 2 * count,
            at + count,
            at + 3 * count,
            numpy.full(count, -1, dtype=numpy.int32),
        ],
        axis=1,
    ).reshape(-1)
    return points, lines


def cutPlane(low, high, axis, share):
    """The plane across an axis (0 x, 1 y, 2 z) of the box from 'low' to
    'high', a 'share' of the way along it from 0 to 1: its base and what it
    lies across. Kept off the box's very ends, where it would cut nothing."""
    share = min(max(share, 0.002), 0.998)
    base = [(low[k] + high[k]) / 2.0 for k in range(3)]
    base[axis] = low[axis] + share * (high[axis] - low[axis])
    normal = [0.0, 0.0, 0.0]
    normal[axis] = 1.0
    return App.Vector(*base), App.Vector(*normal)


def flowText(flow):
    """What goes through an opening, m^3/s, in a unit that reads."""
    litres = flow * 1000.0
    if abs(litres) >= 1.0:
        return "{:.4g} l/s".format(litres)
    return "{:.4g} ml/s".format(litres * 1000.0)


# A pipe's flow stays in layers under a Reynolds number of some 2300.
LAYERED_UNDER = 2300.0
# The wall model of a turbulent flow is in its range when the first cell at
# a wall is some 30 wall units from it or more; under that, in the pipes
# measured (docs/FluidSimulation.md sec 14.6), the friction was 10% off.
WALL_UNITS_FROM = 30.0
# and no more than some 1000: at 1370 the one pipe measured was 6% off.
WALL_UNITS_TO = 1000.0


def flowDoubts(results):
    """What a steady flow's Reynolds number and its cells at the walls have
    against the way its eddies were had, each a sentence for the user."""
    found = []
    if not results.get("Steady"):
        return found
    reynolds = results.get("Reynolds") or 0.0
    turbulent = results.get("Turbulence", "Laminar") != "Laminar"
    if not turbulent and reynolds > LAYERED_UNDER:
        found.append(
            translate(
                "Fluid",
                "The study took the flow as in layers, and its Reynolds number is "
                "{:.0f}: over some {:.0f} a flow does not stay in layers, and the "
                "pressures shown here are not its own. Set the study's flow to turbulent.",
            ).format(reynolds, LAYERED_UNDER)
        )
    if turbulent and 0.0 < reynolds < LAYERED_UNDER:
        found.append(
            translate(
                "Fluid",
                "The study took the flow as turbulent, and its Reynolds number is "
                "{:.0f}: under some {:.0f} a flow stays in layers, and the pressures "
                "shown here are not its own. Set the study's flow to laminar.",
            ).format(reynolds, LAYERED_UNDER)
        )
    units = results.get("WallUnits")
    if turbulent and units and units[2] < WALL_UNITS_FROM:
        found.append(
            translate(
                "Fluid",
                "The cells at the walls are too fine for the wall model of a "
                "turbulent flow: they are {:.0f} wall units from the wall in the "
                "mean ({:.0f} to {:.0f}), and under {:.0f} the friction can be a "
                "tenth off. A resolution some {:.1f} times coarser is in its range.",
            ).format(
                units[2],
                units[0],
                units[1],
                WALL_UNITS_FROM,
                max(2.0 * WALL_UNITS_FROM / max(units[2], 1.0e-9), 1.1),
            )
        )
    if turbulent and units and units[2] > WALL_UNITS_TO:
        found.append(
            translate(
                "Fluid",
                "The cells at the walls are too coarse for the wall model of a "
                "turbulent flow: they are {:.0f} wall units from the wall in the "
                "mean ({:.0f} to {:.0f}), and over {:.0f} the friction can be more "
                "than a twentieth off. A resolution some {:.1f} times finer is in "
                "its range.",
            ).format(units[2], units[0], units[1], WALL_UNITS_TO, units[2] / (0.5 * WALL_UNITS_TO))
        )
    return found


# A flow through time is slowed by its smallest cell: said when the solver's
# steps are under a quarter of what cells all of the study's resolution would
# let it take. In the one case measured (docs/FluidSimulation.md sec 14.12)
# a cell 14 times longer than thick left the steps a tenth as long.
STEP_HELD_UNDER = 0.25


def stepHeld(results):
    """Of a flow through time: the step the solver took and the step cells
    all of the study's resolution would have let it take, s, each the
    middle one of the run's later half. None when the solver did not say."""
    steps = [pair for pair in (results.get("Steps") or []) if pair[0] > 0.0 and pair[1] > 0.0]
    steps = steps[len(steps) // 2 :]
    if not steps:
        return None
    taken = sorted(pair[0] for pair in steps)[len(steps) // 2]
    even = sorted(pair[1] for pair in steps)[len(steps) // 2]
    return taken, even


def stepDoubts(results):
    """What the steps of a flow through time say of its mesh: a sentence
    for the user when some cell held them far under the rest's."""
    held = stepHeld(results)
    if held is None or held[0] >= STEP_HELD_UNDER * held[1]:
        return []
    return [
        translate(
            "Fluid",
            "The solver took steps of {:.2g} s, where cells all of the study's "
            "resolution would let it take {:.2g} s: some cell of the mesh is far "
            "smaller or thinner than the rest, and the run took some {:.0f} times "
            "as long for it. The results are not worse for it.",
        ).format(held[0], held[1], held[1] / held[0])
    ]


def doubts(results):
    """What a run's own account of its energy has against its results, each
    a sentence for the user; none when it has nothing, or kept no account.
    Nothing found is not the results proved: a run on particles too coarse
    for the question keeps its account in order."""
    results = results or {}
    found = []
    if results.get("Steady") and not results.get("Settled"):
        times = results.get("Times") or [0.0]
        found.append(
            translate(
                "Fluid",
                "The flow had not settled in the {:.0f} steps it was given: the "
                "pressures and what goes through the openings may still be on their "
                "way. Give it more steps; a flow that never settles may be one that "
                "does not stay steady.",
            ).format(times[-1])
        )
    found.extend(flowDoubts(results))
    found.extend(stepDoubts(results))
    energy = results.get("Energy")
    if not energy or not energy["Judged"]:
        return found
    if energy["Lagged"]:
        found.append(
            translate(
                "Fluid",
                "The mechanism's parts and the fluid did not keep step: {:.0%} of the "
                "energy that passed between them came from the coupling itself (over "
                "{:.0%} is not to be trusted). The parts may swing wider than they "
                "should, or never come to rest. Heavier parts or a coarser resolution "
                "keep step more easily.",
            ).format(energy["Made"], energy["MadeLimit"])
        )
    if energy["Rose"]:
        found.append(
            translate(
                "Fluid",
                "The fluid gained energy from nowhere: by {:.0%} of the most it ever "
                "moved with (over {:.0%} is not to be trusted). The run was coming "
                "apart. Try a finer resolution, or a shorter time between frames.",
            ).format(energy["Rise"], energy["RiseLimit"])
        )
    return found


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


def addOutlet(study, part, face):
    """The face (by its name, 'Face2') of the part as one a steady flow
    leaves through, at the surroundings' pressure."""
    outlet = study.Document.addObject("Fluid::Outlet", "Outlet")
    outlet.Label = translate("Fluid", "Outlet at {} of {}").format(face, part.Label)
    outlet.Face = (part, [face])
    study.Group = study.Group + [outlet]
    return outlet


def toggleSlides(study, faces):
    """Each face, (part, 'Face2'), among those the study's fluid slides
    along (its Slides) when it was not, and out of them when it was."""
    held = []
    for part, names in study.Slides:
        held += [(part, name) for name in names]
    for face in faces:
        if face in held:
            held.remove(face)
        else:
            held.append(face)
    parts = []
    for part, _name in held:
        if part not in parts:
            parts.append(part)
    study.Slides = [(part, tuple(n for p, n in held if p is part)) for part in parts]


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
