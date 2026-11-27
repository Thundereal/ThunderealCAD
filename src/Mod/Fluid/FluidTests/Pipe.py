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


def pipeStudy(doc):
    """A tube along x, 10 mm inside and 14 outside, 100 long, a lid 2 thick
    at each end: the fluid comes in through the face of the lid at x 0 and
    leaves through the face of the lid at x 100. Returns the study, its
    inlet and its outlet."""
    along = App.Vector(1, 0, 0)
    tube = doc.addObject("Part::Feature", "Tube")
    tube.Shape = Part.makeCylinder(7, LENGTH, App.Vector(0, 0, 0), along).cut(
        Part.makeCylinder(RADIUS, LENGTH, App.Vector(0, 0, 0), along)
    )
    first = doc.addObject("Part::Feature", "First")
    first.Shape = Part.makeCylinder(7, 2, App.Vector(-2, 0, 0), along)
    last = doc.addObject("Part::Feature", "Last")
    last.Shape = Part.makeCylinder(7, 2, App.Vector(LENGTH, 0, 0), along)

    def faceAt(obj, x):
        for i, face in enumerate(obj.Shape.Faces):
            box = face.BoundBox
            if abs(box.XMin - x) < 1e-9 and abs(box.XMax - x) < 1e-9:
                return "Face%d" % (i + 1)
        raise AssertionError("no face at x %g" % x)

    study = doc.addObject("Fluid::Study", "Study")
    study.Analysis = "Steady flow"
    study.Walls = [tube, first, last]
    inlet = FluidPlayback.addInlet(study, first, faceAt(first, 0), SPEED)
    inlet.Density = App.Units.Quantity(DENSITY, "kg/m^3")
    inlet.Viscosity = App.Units.Quantity(VISCOSITY, "Pa*s")
    outlet = FluidPlayback.addOutlet(study, last, faceAt(last, LENGTH))
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

    def test_the_results_are_kept_with_the_document(self):
        self.study.Iterations = 40
        # no solver named: the first that runs a steady flow
        self.study.Backend = ""
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        self.assertEqual(results["Backend"], self.BACKEND)
        self.assertEqual(results["Times"][-1], 40.0)
        self.assertFalse(results["Settled"])
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
