# SPDX-License-Identifier: LGPL-2.1-or-later

# A steady flow over a step down in a channel's floor (docs/FluidSimulation.md
# sec 14.9), for every backend that runs one: behind the step the fluid at
# the floor runs backwards, and where it turns forwards again is a length
# measured many times over (Armaly, Durst, Pereira and Schoenung, J. Fluid
# Mech. 127, 1983: a channel 5.2 mm high that a step of 4.9 mm makes 10.1).

import FreeCAD as App
import Part

import FluidPlayback
from FluidTests.DamBreak import backendNamed

STEP = 4.9  # mm, the step's height
AFTER = 10.1  # mm, the channel's height after the step
VISCOSITY = 1.0e-6  # m^2/s, the fluid's, over its density


def stepStudy(doc, reynolds, width=20.0, before=60.0, after=100.0, cell=1.0):
    """A channel along x in a block, 'width' across in y: 'before' long and
    5.2 high up to the step at x 0, then 10.1 high, its floor at z 0, for
    'after'. A lid 2 thick at each end, the fluid in through the first.
    The Reynolds number is Armaly's: the mean speed in, on twice the height
    before the step. Returns the study and that speed in mm/s."""
    vector = App.Vector
    wall = 2.0
    high = AFTER - STEP
    speed = reynolds * VISCOSITY / (2.0 * high * 1.0e-3) * 1000.0
    space = Part.makeBox(before, width, high, vector(-before, 0, STEP)).fuse(
        Part.makeBox(after, width, AFTER, vector(0, 0, 0))
    )
    block = doc.addObject("Part::Feature", "Block")
    block.Shape = Part.makeBox(
        before + after, width + 2 * wall, AFTER + 2 * wall, vector(-before, -wall, -wall)
    ).cut(space)
    first = doc.addObject("Part::Feature", "First")
    first.Shape = Part.makeBox(
        wall, width + 2 * wall, AFTER + 2 * wall, vector(-before - wall, -wall, -wall)
    )
    last = doc.addObject("Part::Feature", "Last")
    last.Shape = Part.makeBox(wall, width + 2 * wall, AFTER + 2 * wall, vector(after, -wall, -wall))

    def faceAt(obj, x):
        for i, face in enumerate(obj.Shape.Faces):
            box = face.BoundBox
            if abs(box.XMin - x) < 1e-9 and abs(box.XMax - x) < 1e-9:
                return "Face%d" % (i + 1)
        raise AssertionError("no face at x %g" % x)

    study = doc.addObject("Fluid::Study", "Study")
    study.Analysis = "Steady flow"
    study.Walls = [block, first, last]
    study.Resolution = cell
    inlet = FluidPlayback.addInlet(study, first, faceAt(first, -before), speed)
    inlet.Density = App.Units.Quantity(1000.0, "kg/m^3")
    inlet.Viscosity = App.Units.Quantity(1000.0 * VISCOSITY, "Pa*s")
    FluidPlayback.addOutlet(study, last, faceAt(last, after))
    doc.recompute()
    return study, speed


def alongTheFloor(cells, flow, y, cell):
    """The speed along x of the cells lying on the floor behind the step,
    within a cell of 'y' across: each place along, mm, and the speed there,
    m/s, in order along."""
    import numpy

    low = (cells[:, 2] < cell) & (numpy.abs(cells[:, 1] - y) < 0.75 * cell) & (cells[:, 0] > 0)
    x, u = cells[low, 0], flow[low, 0]
    places = numpy.unique(numpy.round(x / cell * 2.0) / 2.0 * cell)
    speeds = numpy.array([u[numpy.abs(x - place) < 0.25 * cell].mean() for place in places])
    return places, speeds


def turnsForward(places, speeds):
    """Each place along where the flow at the floor turns from backwards to
    forwards, mm."""
    found = []
    for i in range(1, len(places)):
        if speeds[i - 1] < 0.0 <= speeds[i]:
            share = -speeds[i - 1] / (speeds[i] - speeds[i - 1])
            found.append(float(places[i - 1] + share * (places[i] - places[i - 1])))
    return found


class Step:
    """A test class is this and unittest.TestCase, and sets BACKEND."""

    BACKEND = ""

    def setUp(self):
        found = backendNamed(self.BACKEND)
        if found is None or not found["Available"]:
            self.skipTest(f"no {self.BACKEND} on this machine")
        if not found["Steady"]:
            self.skipTest(f"{self.BACKEND} runs no steady flow")
        self.doc = App.newDocument("FluidStep")

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def test_the_flow_turns_back_behind_a_step_as_far_as_armaly_found(self):
        study, speed = stepStudy(self.doc, 200.0)
        study.Backend = self.BACKEND
        self.doc.recompute()
        self.assertEqual(study.run(), 0, study.Status)
        results = study.getResults()
        frames = FluidPlayback.Frames(study)
        cells, flow, _ = frames.insideOf(results["Inside"][-1])
        places, speeds = alongTheFloor(cells, flow, 10.0, 1.0)

        # backwards at the floor two step heights behind the step
        self.assertLess(speeds[abs(places - 2.0 * STEP).argmin()], 0.0)
        # and forwards again once: at a Reynolds number of 200 Armaly
        # measured 5 step heights. 4.59 on these 1 mm cells in a channel
        # 20 wide, 4.74 on 0.5 mm, 5.06 on 0.5 mm in a channel 40 wide.
        turns = turnsForward(places, speeds)
        self.assertEqual(len(turns), 1, turns)
        self.assertGreater(turns[0] / STEP, 4.2)
        self.assertLess(turns[0] / STEP, 5.4)

        # a tracer let go in the turned-back fluid is carried towards the
        # step first, against the stream above it
        import numpy

        seeds = numpy.array([[1.5 * STEP, 10.0, 0.4 * STEP]], dtype=numpy.float32)
        paths = frames.paths(results["Inside"][-1], seeds, most=200)
        self.assertLess(paths.points[:, 0].min(), 1.5 * STEP - 2.0)
