# SPDX-License-Identifier: LGPL-2.1-or-later

# A steady flow along a pipe (docs/FluidSimulation.md sec 14.4), for every
# backend that runs one: the pressure it loses against Hagen-Poiseuille's
# closed form, dp = 8 mu L Q / (pi R^4).

import math

import FreeCAD as App
import Part

import FluidPlayback
from FluidTests.DamBreak import backendNamed

RADIUS = 5.0  # mm
LENGTH = 100.0
SPEED = 100.0  # mm/s
DENSITY = 1000.0  # kg/m^3
VISCOSITY = 0.1  # Pa s: with the speed, a Reynolds number of 10


def pipeStudy(doc, length=LENGTH, speed=SPEED, density=DENSITY, viscosity=VISCOSITY):
    """A tube along x, 10 mm inside and 14 outside, 100 long, a lid 2 thick
    at each end: the fluid comes in through the face of the lid at x 0 and
    leaves through the face of the lid at x 100. Returns the study, its
    inlet and its outlet."""
    along = App.Vector(1, 0, 0)
    tube = doc.addObject("Part::Feature", "Tube")
    tube.Shape = Part.makeCylinder(7, length, App.Vector(0, 0, 0), along).cut(
        Part.makeCylinder(RADIUS, length, App.Vector(0, 0, 0), along)
    )
    first = doc.addObject("Part::Feature", "First")
    first.Shape = Part.makeCylinder(7, 2, App.Vector(-2, 0, 0), along)
    last = doc.addObject("Part::Feature", "Last")
    last.Shape = Part.makeCylinder(7, 2, App.Vector(length, 0, 0), along)

    def faceAt(obj, x):
        for i, face in enumerate(obj.Shape.Faces):
            box = face.BoundBox
            if abs(box.XMin - x) < 1e-9 and abs(box.XMax - x) < 1e-9:
                return "Face%d" % (i + 1)
        raise AssertionError("no face at x %g" % x)

    study = doc.addObject("Fluid::Study", "Study")
    study.Analysis = "Steady flow"
    study.Walls = [tube, first, last]
    inlet = FluidPlayback.addInlet(study, first, faceAt(first, 0), speed)
    inlet.Density = App.Units.Quantity(density, "kg/m^3")
    inlet.Viscosity = App.Units.Quantity(viscosity, "Pa*s")
    outlet = FluidPlayback.addOutlet(study, last, faceAt(last, length))
    doc.recompute()
    return study, inlet, outlet


class Pipe:
    """A test class is this and unittest.TestCase, and sets BACKEND."""

    BACKEND = ""

    def setUp(self):
        found = backendNamed(self.BACKEND)
        if found is None or not found["Available"]:
            self.skipTest(f"no {self.BACKEND} on this machine")
        if not found["Steady"]:
            self.skipTest(f"{self.BACKEND} runs no steady flow")
        self.doc = App.newDocument("FluidPipe")
        self.study, self.inlet, self.outlet = pipeStudy(self.doc)
        # on the pipe's axis, 50 mm apart, the first well past where the
        # flow has taken its shape (some 6 mm at this Reynolds number)
        self.before = FluidPlayback.addMonitor(self.study, "Pressure", App.Vector(40, 0, 0))
        self.after = FluidPlayback.addMonitor(self.study, "Pressure", App.Vector(90, 0, 0))
        self.study.Backend = self.BACKEND
        self.doc.recompute()

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def test_the_pipe_loses_what_hagen_poiseuille_says(self):
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        self.assertEqual(results["Backend"], self.BACKEND)
        self.assertEqual(results["Kind"], "Fields")
        self.assertTrue(results["Steady"])
        # it settled before the steps it was given were up, and the frames
        # are of the steps on the way and of the last
        self.assertGreater(results["Frames"], 1)
        self.assertEqual(results["Times"][0], 0.0)
        self.assertLess(results["Times"][-1], self.study.Iterations)
        self.assertTrue(results["Settled"])
        self.assertEqual(results["MonitorTimes"][-1], results["Times"][-1])

        into, out = results["Openings"]
        self.assertEqual((into["Kind"], out["Kind"]), ("Inlet", "Outlet"))
        self.assertEqual(into["Object"], self.inlet)
        self.assertEqual(out["Object"], self.outlet)
        # what comes in goes out, and it is the speed over the face
        flow = out["Flow"][-1]
        self.assertAlmostEqual(into["Flow"][-1] / flow, 1.0, delta=1e-4)
        given = SPEED * 1e-3 * math.pi * (RADIUS * 1e-3) ** 2
        self.assertAlmostEqual(flow / given, 1.0, delta=0.01)

        # the pressure lost between the monitors, at the flow there was
        before, after = (monitor["Values"][-1] for monitor in results["Monitors"])
        lost = before - after
        closed = 8.0 * VISCOSITY * 0.05 * flow / (math.pi * (RADIUS * 1e-3) ** 4)
        self.assertAlmostEqual(lost / closed, 1.0, delta=0.01)
        # the outlet is held at the surroundings' pressure, and the inlet
        # is above the first monitor
        self.assertAlmostEqual(out["Pressure"][-1], 0.0, delta=0.01 * closed)
        self.assertGreater(into["Pressure"][-1], before)

        # the pressure on the faces the fluid touches: a value a point,
        # falling along the pipe, and at a monitor's place along it what
        # the monitor has -- across a pipe the pressure is one
        frames = FluidPlayback.Frames(self.study)
        points, triangles, values = frames.field(len(frames) - 1)
        self.assertEqual(len(values), len(points))
        self.assertGreater(len(triangles), 0)
        self.assertLess(int(triangles.max()), len(points))
        self.assertAlmostEqual(float(points[:, 0].min()), 0.0, delta=0.01)
        self.assertAlmostEqual(float(points[:, 0].max()), LENGTH, delta=0.01)
        radius = (points[:, 1] ** 2 + points[:, 2] ** 2) ** 0.5
        # the mesh's points are snapped to the round to a part of a cell
        self.assertLess(float(radius.max()), RADIUS + 0.05)
        for x, monitor in ((40.0, before), (90.0, after)):
            ring = values[(abs(points[:, 0] - x) < 1.0) & (radius > RADIUS - 0.5)]
            self.assertGreater(len(ring), 0)
            self.assertAlmostEqual(float(ring.mean()) / monitor, 1.0, delta=0.05)
        self.assertGreater(float(values[points[:, 0] < 1.0].mean()), before)
        # the frame at the start is the space with no pressure on it
        points, triangles, values = frames.field(0)
        self.assertEqual(float(abs(values).max()), 0.0)

    def test_the_flow_inside_is_poiseuilles(self):
        import numpy

        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        frames = FluidPlayback.Frames(self.study)
        last = len(frames) - 1
        # the fluid inside is kept of the flow as it settled, and no other
        self.assertEqual(results["Inside"], [last])
        self.assertEqual(frames.inside, [last])
        self.assertEqual(self.study.getInside(0)["Count"], 0)
        points, velocity, pressure = frames.insideOf(last)
        self.assertGreater(len(points), 10000)
        self.assertEqual(len(velocity), len(points))
        self.assertEqual(len(pressure), len(points))
        radius = (points[:, 1] ** 2 + points[:, 2] ** 2) ** 0.5
        self.assertLess(float(radius.max()), RADIUS)
        self.assertGreater(float(points[:, 0].min()), 0.0)
        self.assertLess(float(points[:, 0].max()), LENGTH)

        # past where it has taken its shape the flow is along the pipe, a
        # parabola across it: twice the mean speed on the axis and none at
        # the wall. The mean is of the flow there was
        mean = results["Openings"][1]["Flow"][-1] / (math.pi * (RADIUS * 1e-3) ** 2)
        shaped = (points[:, 0] > 40.0) & (points[:, 0] < 90.0)
        closed = 2.0 * mean * (1.0 - (radius[shaped] / RADIUS) ** 2)
        along = velocity[shaped][:, 0]
        self.assertLess(float(abs(along - closed).max()), 0.03 * 2.0 * mean)
        self.assertLess(float(abs(velocity[shaped][:, 1:]).max()), 0.01 * mean)
        # and the pressure is one across the pipe, falling along it
        before, after = (monitor["Values"][-1] for monitor in results["Monitors"])
        for x, monitor in ((40.0, before), (90.0, after)):
            ring = pressure[abs(points[:, 0] - x) < 0.3]
            self.assertGreater(len(ring), 50)
            self.assertAlmostEqual(float(ring.mean()) / monitor, 1.0, delta=0.02)
            self.assertLess(float(ring.max() - ring.min()), 0.02 * before)

        # a cut across the pipe, half way: a disc of the pipe's bore
        cut = frames.section(last, App.Vector(60, 0, 0), App.Vector(1, 0, 0))
        self.assertGreater(len(cut), 1000)
        # half the solver's cell, which is a twentieth of the bore
        self.assertAlmostEqual(cut.step, 2.0 * RADIUS / 40.0, places=6)
        self.assertLess(float(abs(cut.points[:, 0] - 60.0).max()), 1e-4)
        across = (cut.points[:, 1] ** 2 + cut.points[:, 2] ** 2) ** 0.5
        self.assertLess(float(across.max()), RADIUS + 0.05)
        self.assertGreater(float(across.max()), RADIUS - cut.step)
        area = len(cut.triangles) * cut.step**2 / 2.0
        self.assertAlmostEqual(area / (math.pi * RADIUS**2), 1.0, delta=0.08)
        # the speed on it: the parabola again, as near as values between
        # the cells' centres come to it
        speed = cut.speed()
        centre = int(numpy.argmin(across))
        self.assertAlmostEqual(float(speed[centre]) / (2.0 * mean), 1.0, delta=0.03)
        inner = across < RADIUS - 1.0
        closed = 2.0 * mean * (1.0 - (across[inner] / RADIUS) ** 2)
        self.assertLess(float(abs(speed[inner] - closed).max()), 0.06 * 2.0 * mean)
        self.assertLess(float(cut.pressure.max() - cut.pressure.min()), 0.02 * before)
        # the outline is the bore's round
        ring = (cut.outline[:, 1] ** 2 + cut.outline[:, 2] ** 2) ** 0.5
        self.assertGreater(len(ring), 20)
        self.assertLess(float(abs(ring - RADIUS).max()), 0.05)

        # a cut along the pipe: all its length, and the flow in the plane
        cut = frames.section(last, App.Vector(0, 0, 0), App.Vector(0, 0, 1))
        self.assertGreater(float(cut.points[:, 0].max() - cut.points[:, 0].min()), LENGTH - 1.0)
        self.assertLess(float(abs(cut.points[:, 2]).max()), 1e-4)
        points, lines = FluidPlayback.arrowsOf(cut, 4, 2.0)
        self.assertGreater(len(lines), 7 * 100)
        # a plane past the pipe finds nothing, and a frame with none kept
        self.assertEqual(len(frames.section(last, App.Vector(0, 0, 9), App.Vector(0, 0, 1))), 0)
        self.assertEqual(len(frames.section(0, App.Vector(60, 0, 0), App.Vector(1, 0, 0))), 0)

        # tracers let go in the first cells inside the inlet are carried to
        # the outlet, the one nearest the axis at twice the mean speed once
        # the flow has its shape: 100 mm in a little over half a second
        cell = self.study.buildModel()["Resolution"] * 1000.0
        cells, flow, _pressure = frames.insideOf(last)
        seeds = FluidPlayback.seedsAt(cells, flow, (0, 0, 0), (1, 0, 0), cell, 40)
        self.assertEqual(len(seeds), 40)
        self.assertLess(float(seeds[:, 0].max()), cell)
        paths = frames.paths(last, seeds, 0.0, 20000)
        self.assertEqual(len(paths), 40)
        # the fastest of the fluid goes half a cell a point
        fastest = float(((flow**2).sum(axis=1) ** 0.5).max())
        self.assertAlmostEqual(paths.interval, 0.5 * cell * 1e-3 / fastest, delta=1e-9)
        nearest = int(((seeds[:, 1] ** 2 + seeds[:, 2] ** 2)).argmin())
        first, after = int(paths.starts[nearest]), int(paths.starts[nearest + 1])
        along = paths.points[first:after]
        self.assertGreater(float(along[-1, 0]), LENGTH - cell)
        self.assertLess(float(abs(along[:, 1:]).max()), 2.0 * cell)
        took = (after - first) * paths.interval
        self.assertAlmostEqual(took, 0.53, delta=0.04)
        # each goes on along the pipe, none back, and none through the wall
        for path in range(len(paths)):
            went = paths.points[int(paths.starts[path]) : int(paths.starts[path + 1])]
            self.assertTrue((went[1:, 0] >= went[:-1, 0]).all())
            self.assertLess(float((went[:, 1] ** 2 + went[:, 2] ** 2).max()) ** 0.5, RADIUS + cell)
        # of a frame with no fluid inside kept: no paths
        self.assertEqual(len(frames.paths(0, seeds)), 0)

    def test_a_turbulent_pipe_loses_what_haaland_says(self):
        """Water at 3 m/s along the pipe made 300 long, a Reynolds number of
        30000 (docs/FluidSimulation.md sec 14.6): the friction factor between
        15 and 27 widths in against Haaland's for a smooth pipe,
        1 / sqrt(f) = -1.8 log10(6.9 / Re), to 5%."""
        doc = App.newDocument("FluidTurbulentPipe")
        try:
            speed = 3.0  # m/s
            study, _inlet, _outlet = pipeStudy(doc, 300.0, speed * 1000.0, 1000.0, 1.0e-3)
            study.Turbulence = "k-omega SST"
            study.Resolution = 1.0
            FluidPlayback.addMonitor(study, "Pressure", App.Vector(150, 0, 0))
            FluidPlayback.addMonitor(study, "Pressure", App.Vector(270, 0, 0))
            study.Backend = self.BACKEND
            doc.recompute()
            model = study.buildModel()
            self.assertEqual(model["Turbulence"], "KOmegaSST")
            self.assertAlmostEqual(model["Inlets"][0]["Reynolds"] / 30000.0, 1.0, delta=0.01)

            self.assertEqual(study.run(), 0, study.Status)
            results = study.getResults()
            self.assertTrue(results["Settled"])
            self.assertEqual(results["Turbulence"], "KOmegaSST")
            self.assertAlmostEqual(results["Reynolds"] / 30000.0, 1.0, delta=0.01)
            into, out = results["Openings"]
            flow = out["Flow"][-1]
            self.assertAlmostEqual(into["Flow"][-1] / flow, 1.0, delta=1e-3)
            # at the flow there was: the mean speed along the pipe
            mean = flow / (math.pi * (RADIUS * 1e-3) ** 2)
            self.assertAlmostEqual(mean / speed, 1.0, delta=0.02)
            before, after = (monitor["Values"][-1] for monitor in results["Monitors"])
            width = 2.0 * RADIUS * 1e-3
            factor = (before - after) * width / 0.12 * 2.0 / (1000.0 * mean**2)
            reynolds = 1000.0 * mean * width / 1.0e-3
            haaland = (-1.8 * math.log10(6.9 / reynolds)) ** -2
            self.assertAlmostEqual(factor / haaland, 1.0, delta=0.05)
            # the wall's cells are in the wall model's range, and the study
            # has nothing against its own results
            least, most, middling = results["WallUnits"]
            self.assertLess(least, middling)
            self.assertLess(middling, most)
            self.assertGreater(middling, FluidPlayback.WALL_UNITS_FROM)
            self.assertEqual(FluidPlayback.doubts(results), [])
            # in layers it would have lost 64 / Re, a third of it
            self.assertGreater(factor, 2.5 * 64.0 / reynolds)
        finally:
            App.closeDocument(doc.Name)

    def test_the_results_are_kept_with_the_document(self):
        self.study.Iterations = 40
        # no solver named: the first that runs a steady flow
        self.study.Backend = ""
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        self.assertEqual(results["Backend"], self.BACKEND)
        self.assertEqual(results["Times"][-1], 40.0)
        self.assertFalse(results["Settled"])
        self.count = self.study.getInside(results["Frames"] - 1)["Count"]
        self.assertGreater(self.count, 10000)
        import os
        import tempfile

        path = os.path.join(tempfile.mkdtemp(), "pipe.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        study = self.doc.getObject("Study")
        again = study.getResults()
        self.assertTrue(again["Steady"])
        self.assertEqual(again["Times"], results["Times"])
        self.assertEqual(again["Openings"][1]["Flow"], results["Openings"][1]["Flow"])
        frames = FluidPlayback.Frames(study)
        points, _triangles, values = frames.field(len(frames) - 1)
        self.assertEqual(len(values), len(points))
        # the fluid inside with them, though the flow had not settled
        last = len(frames) - 1
        self.assertEqual(again["Inside"], [last])
        self.assertEqual(study.getInside(last)["Count"], self.count)
        self.assertGreater(len(frames.section(last, App.Vector(60, 0, 0), App.Vector(1, 0, 0))), 0)
