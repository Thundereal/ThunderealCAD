# SPDX-License-Identifier: LGPL-2.1-or-later

# A flow through time past a cylinder (docs/FluidSimulation.md sec 14.11),
# for every backend that runs one: at a Reynolds number of 100 the flow
# behind it does not settle -- eddies leave its two sides in turn, and how
# often is a number measured many times over (the Strouhal number, the
# cylinder's width over the stream's speed and the time between two eddies
# of one side: 0.164 to 0.165 in a stream without walls, Williamson 1989).

import os

import FreeCAD as App
import Part

import FluidPlayback
from FluidTests.DamBreak import backendNamed

WIDTH = 10.0  # mm, across the cylinder
VISCOSITY = 1.0e-6  # m^2/s, the fluid's, over its density


def sheddingStudy(doc, reynolds=100.0, before=40.0, after=110.0, half=40.0, cell=1.0, off=0.0):
    """A cylinder across a channel along x, its axis along y at x 0 and z
    'off': the channel 'before' long up to the axis and 'after' behind it,
    'half' high each way, three cells deep. The fluid slides along every
    face of the channel -- the cylinder alone holds it -- and comes in
    through a lid at the first end. Returns the study and the stream's
    speed in mm/s."""
    vector = App.Vector
    wall = 2.0
    deep = 3.0 * cell
    speed = reynolds * VISCOSITY / (WIDTH * 1.0e-3) * 1000.0
    space = Part.makeBox(before + after, deep, 2.0 * half, vector(-before, 0, -half)).cut(
        Part.makeCylinder(WIDTH / 2.0, deep + 2.0, vector(0, -1, off), vector(0, 1, 0))
    )
    block = doc.addObject("Part::Feature", "Block")
    block.Shape = Part.makeBox(
        before + after, deep + 2 * wall, 2 * half + 2 * wall, vector(-before, -wall, -half - wall)
    ).cut(space)
    first = doc.addObject("Part::Feature", "First")
    first.Shape = Part.makeBox(
        wall, deep + 2 * wall, 2 * half + 2 * wall, vector(-before - wall, -wall, -half - wall)
    )
    last = doc.addObject("Part::Feature", "Last")
    last.Shape = Part.makeBox(
        wall, deep + 2 * wall, 2 * half + 2 * wall, vector(after, -wall, -half - wall)
    )

    def faceAt(obj, x):
        for i, face in enumerate(obj.Shape.Faces):
            box = face.BoundBox
            if abs(box.XMin - x) < 1e-9 and abs(box.XMax - x) < 1e-9:
                return "Face%d" % (i + 1)
        raise AssertionError("no face at x %g" % x)

    study = doc.addObject("Fluid::Study", "Study")
    study.Analysis = "Flow in time"
    study.Walls = [block, first, last]
    study.Resolution = cell
    # the channel's own four faces: the flat ones of the block that run its
    # length and are not its outside
    slid = []
    for i, face in enumerate(block.Shape.Faces):
        box = face.BoundBox
        inside = abs(box.YMin) < deep + 1e-6 and abs(box.YMax) < deep + 1e-6
        inside = inside and abs(box.ZMin) < half + 1e-6 and abs(box.ZMax) < half + 1e-6
        if face.Surface.TypeId == "Part::GeomPlane" and inside and box.XLength > before:
            slid.append("Face%d" % (i + 1))
    study.Slides = [(block, tuple(slid))]
    inlet = FluidPlayback.addInlet(study, first, faceAt(first, -before), speed)
    inlet.Density = App.Units.Quantity(1000.0, "kg/m^3")
    inlet.Viscosity = App.Units.Quantity(1000.0 * VISCOSITY, "Pa*s")
    FluidPlayback.addOutlet(study, last, faceAt(last, after))
    doc.recompute()
    return study, speed


def acrossBehind(frames, place):
    """The fluid's speed across the stream (along z, m/s) at the cell
    nearest a place, mm, at each frame that keeps the fluid inside, and
    those frames' times, s."""
    import numpy

    times, speeds = [], []
    for index in frames.inside:
        cells, flow, _pressure = frames.insideOf(index)
        near = numpy.argmin(((cells - numpy.asarray(place, dtype=numpy.float32)) ** 2).sum(axis=1))
        times.append(frames.times[index])
        speeds.append(float(flow[near, 2]))
    return numpy.array(times), numpy.array(speeds)


def risings(times, values):
    """The times at which the values go from under their mean to over it."""
    values = values - values.mean()
    found = []
    for i in range(1, len(values)):
        if values[i - 1] < 0.0 <= values[i]:
            share = -values[i - 1] / (values[i] - values[i - 1])
            found.append(float(times[i - 1] + share * (times[i] - times[i - 1])))
    return found


class Shedding:
    """A test class is this and unittest.TestCase, and sets BACKEND."""

    BACKEND = ""

    def setUp(self):
        found = backendNamed(self.BACKEND)
        if found is None or not found["Available"]:
            self.skipTest(f"no {self.BACKEND} on this machine")
        if not found.get("Unsteady"):
            self.skipTest(f"{self.BACKEND} runs no flow through time")
        self.doc = App.newDocument("FluidShedding")

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def test_eddies_leave_a_cylinder_in_turn_as_often_as_measured(self):
        import numpy

        study, speed = sheddingStudy(self.doc)
        study.Backend = self.BACKEND
        study.TimeEnd = 64.0
        study.OutputInterval = 0.5
        self.doc.recompute()
        model = study.buildModel()
        self.assertEqual(model["Analysis"], "Unsteady")
        # the channel's four faces are slid along: 150 long, 80 high and 3
        # deep, less the cylinder's two ends
        slid = 2 * (150.0 * 3.0) + 2 * (150.0 * 80.0 - numpy.pi * 25.0)
        self.assertAlmostEqual(model["Domain"]["Slides"] * 1.0e6, slid, delta=1.0)
        self.assertEqual(study.run(), 0, study.Status)
        results = study.getResults()
        self.assertFalse(results["Steady"])
        self.assertEqual(results["Kind"], "Fields")
        frames = FluidPlayback.Frames(study)
        # a frame each half second, the fluid inside kept with every one
        # after the start's
        self.assertEqual(len(frames), 129)
        self.assertAlmostEqual(frames.times[-1], 64.0, places=6)
        self.assertEqual(list(frames.inside), list(range(1, 129)))

        # what comes in is the speed over the face -- the face slid along
        # is meshed to its place -- and it leaves
        (_i, _ik, _ip, came), (_o, _ok, _op, left) = frames.openingsAt(128)
        self.assertAlmostEqual(came, speed * 1.0e-3 * 240.0e-6, delta=0.01 * came)
        self.assertAlmostEqual(left, came, delta=1.0e-3 * came)

        # two widths behind the cylinder the fluid goes across the stream
        # one way, then the other: not at first, from rest, and by the
        # end as fast as two thirds of the stream
        times, across = acrossBehind(frames, (2.0 * WIDTH, 1.5, 0.0))
        self.assertLess(numpy.abs(across[times < 5.0]).max(), 0.05 * speed * 1.0e-3)
        late = times > 44.0
        self.assertGreater(across[late].max(), 0.4 * speed * 1.0e-3)
        self.assertLess(across[late].min(), -0.4 * speed * 1.0e-3)
        # and how often is the measured number: 0.164 in a stream without
        # walls, more in a channel 8 widths high. 0.178 on these cells, 10
        # across the cylinder.
        ups = risings(times[late], across[late])
        self.assertGreaterEqual(len(ups), 3, ups)
        period = float(numpy.diff(ups).mean())
        strouhal = WIDTH * 1.0e-3 / (period * speed * 1.0e-3)
        self.assertGreater(strouhal, 0.16)
        self.assertLess(strouhal, 0.20)

        # the solver's steps are kept with the frames: as long as cells of
        # 1 mm and the fastest fluid allow, or nearly, and nothing is said
        steps = results["Steps"]
        self.assertEqual(len(steps), 129)
        self.assertEqual(steps[0], (0.0, 0.0))
        taken, even = FluidPlayback.stepHeld(results)
        self.assertGreater(taken, 0.5 * even)
        self.assertLessEqual(taken, 0.5 + 1.0e-9)
        self.assertEqual(FluidPlayback.doubts(results), [])
        # the cells' centres are in the file once, not with every frame:
        # four numbers a cell and frame, where seven were 28 bytes
        cells = len(frames.insideOf(128)[0])
        self.assertLess(os.path.getsize(study.Results), 20 * cells * 128)

        # tracers let go before the cylinder and carried from frame to
        # frame are swept to both sides of its wake
        seeds = [[-2.0 * WIDTH, 1.5, z] for z in (-3.0, -1.0, 1.0, 3.0)]
        streaks = FluidPlayback.Streaks(frames, seeds, 1.0)
        points, _speeds = streaks.at(128)
        behind = points[points[:, 0] > 3.0 * WIDTH]
        self.assertGreater(len(behind), 40)
        self.assertGreater(behind[:, 2].max(), 0.5 * WIDTH)
        self.assertLess(behind[:, 2].min(), -0.5 * WIDTH)

        # kept with the document
        import tempfile

        cells = len(frames.insideOf(64)[0])
        path = os.path.join(tempfile.mkdtemp(), "shedding.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        again = FluidPlayback.Frames(self.doc.getObject("Study"))
        self.assertEqual(len(again), 129)
        self.assertEqual(len(again.insideOf(64)[0]), cells)
        self.assertFalse(again.steady)
