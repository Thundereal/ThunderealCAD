# SPDX-License-Identifier: LGPL-2.1-or-later

# The kept frames as playback reads them, and a frame in a scene graph
# (docs/FluidSimulation.md sec 11.4).

import unittest

import FreeCAD as App

import FluidPlayback
from FluidTests.TestStudy import FluidCase, G


class TestFrames(FluidCase):
    def test_no_results(self):
        frames = FluidPlayback.Frames(self.study)
        self.assertFalse(frames.valid)
        self.assertEqual(len(frames), 0)
        self.assertEqual(frames.indexAt(0.1), -1)

    def test_frames_by_time(self):
        self.assertEqual(self.study.run(), 0)
        frames = FluidPlayback.Frames(self.study)
        self.assertTrue(frames.valid)
        self.assertEqual(len(frames), 9)
        self.assertEqual(frames.kind, "Particles")
        self.assertEqual(frames.backend, "FreeFall")
        # the last frame at or before a time
        self.assertEqual(frames.indexAt(-1.0), 0)
        self.assertEqual(frames.indexAt(0.0), 0)
        self.assertEqual(frames.indexAt(0.049), 0)
        self.assertEqual(frames.indexAt(0.05), 1)
        self.assertEqual(frames.indexAt(0.2), 4)
        self.assertEqual(frames.indexAt(9.0), 8)

    def test_points_and_monitors(self):
        self.assertEqual(self.study.run(), 0)
        frames = FluidPlayback.Frames(self.study)
        points, triangles = frames.frame(4)
        self.assertIsNone(triangles)
        self.assertEqual(points.shape, (250, 3))
        drop = 0.5 * G * 0.2**2 * 1000.0
        self.assertAlmostEqual(float(points[:, 2].max()), 490.0 - drop, delta=2e-3)
        self.height.Label = "Level at the wall"
        rows = frames.monitorsAt(4)
        self.assertEqual(rows[0][0], "Level at the wall")
        self.assertEqual(rows[0][1], "Height")
        self.assertAlmostEqual(rows[0][2], 500.0 - drop, delta=1e-6)
        self.assertEqual(rows[0][3], "mm")
        self.assertEqual(rows[1][1], "Pressure")
        self.assertEqual(rows[1][3], "Pa")

    def test_particles_wrapped(self):
        # 250 particles 20 mm apart, a block 100 x 100 x 200: the surface
        # around them is closed and holds about the block
        import numpy

        self.assertEqual(self.study.run(), 0)
        frames = FluidPlayback.Frames(self.study)
        points, triangles, normals, spray = frames.surface(4)
        # a block at rest: none of it is spray
        self.assertEqual(spray.shape, (0, 3))
        self.assertEqual(normals.shape, points.shape)
        self.assertGreater(len(triangles), 100)
        self.assertLess(int(triangles.max()), len(points))
        lengths = numpy.linalg.norm(normals, axis=1)
        self.assertAlmostEqual(float(lengths.min()), 1.0, delta=1e-4)
        self.assertAlmostEqual(float(lengths.max()), 1.0, delta=1e-4)
        # closed: every edge once each way
        edges = numpy.concatenate(
            [triangles[:, [0, 1]], triangles[:, [1, 2]], triangles[:, [2, 0]]]
        )
        there = set(map(tuple, edges.tolist()))
        self.assertEqual(len(there), len(edges))
        self.assertTrue(all((b, a) in there for a, b in there))
        a, b, c = (points[triangles[:, k]].astype(numpy.float64) for k in range(3))
        volume = float(numpy.einsum("ij,ij->i", a, numpy.cross(b, c)).sum()) / 6.0
        # less the rounding of its edges, much on a block five particles wide
        self.assertAlmostEqual(volume / (100.0 * 100.0 * 200.0), 0.95, delta=0.07)
        # the particles are inside it by half their spacing
        particles, _none = frames.frame(4)
        self.assertAlmostEqual(float(points[:, 2].max() - particles[:, 2].max()), 10.0, delta=1.5)


class TestDoubts(unittest.TestCase):
    """What the panel says of a run whose account of its energy does not
    hold."""

    @staticmethod
    def results(**changed):
        energy = {
            "Judged": True,
            "Rose": False,
            "Rise": 0.0,
            "RiseLimit": 0.1,
            "Lagged": False,
            "Made": 0.02,
            "MadeLimit": 0.2,
        }
        energy.update(changed)
        return {"Energy": energy}

    def test_nothing_against_a_run_that_holds(self):
        self.assertEqual(FluidPlayback.doubts(self.results()), [])
        # nor one that kept no account, or too short a one, or no results
        self.assertEqual(FluidPlayback.doubts({}), [])
        self.assertEqual(FluidPlayback.doubts(None), [])
        self.assertEqual(FluidPlayback.doubts(self.results(Judged=False, Lagged=True)), [])

    def test_the_lag_and_the_rise_each_say_so(self):
        (lag,) = FluidPlayback.doubts(self.results(Lagged=True, Made=0.3))
        self.assertIn("30%", lag)
        self.assertIn("20%", lag)
        self.assertIn("mechanism", lag)
        (rise,) = FluidPlayback.doubts(self.results(Rose=True, Rise=0.5))
        self.assertIn("50%", rise)
        self.assertIn("10%", rise)
        both = FluidPlayback.doubts(self.results(Lagged=True, Made=0.3, Rose=True, Rise=0.5))
        self.assertEqual(both, [lag, rise])


class TestSteadyDoubts(unittest.TestCase):
    """What the panel says of a steady flow that had not settled."""

    def test_a_flow_that_settled_has_nothing_against_it(self):
        self.assertEqual(FluidPlayback.doubts({"Steady": True, "Settled": True}), [])
        # nor has a run through time, which settles to nothing
        self.assertEqual(FluidPlayback.doubts({"Steady": False, "Settled": False}), [])

    def test_one_that_did_not_says_so(self):
        (found,) = FluidPlayback.doubts({"Steady": True, "Settled": False, "Times": [0.0, 2000.0]})
        self.assertIn("2000", found)
        self.assertIn("settled", found)


class TestColors(unittest.TestCase):
    """A field's values as colours: blue the least, red the most."""

    def test_the_ends_and_between(self):
        import numpy

        values = numpy.array([0.0, 50.0, 100.0, -20.0, 300.0], dtype=numpy.float32)
        colors = FluidPlayback.colorsOf(values, 0.0, 100.0)
        self.assertEqual(colors.shape, (5, 3))
        self.assertEqual(colors.dtype, numpy.float32)
        low, middle, high, under, over = colors
        self.assertGreater(low[2], low[0])
        self.assertGreater(high[0], high[2])
        self.assertGreater(middle[1], max(middle[0], middle[2]))
        # what is past an end has the end's colour
        self.assertEqual(tuple(under), tuple(low))
        self.assertEqual(tuple(over), tuple(high))
        # all of one value: no range to spread over, and no fault
        flat = FluidPlayback.colorsOf(values, 5.0, 5.0)
        self.assertEqual(flat.shape, (5, 3))
        self.assertTrue(numpy.isfinite(flat).all())

    def test_the_range_of_a_field(self):
        import numpy

        values = numpy.array([3.0, -1.0, 7.5], dtype=numpy.float32)
        self.assertEqual(FluidPlayback.rangeOf(values), (-1.0, 7.5))
        self.assertEqual(FluidPlayback.rangeOf(values[:0]), (0.0, 0.0))

    def test_what_went_through(self):
        # 7.854e-6 m^3/s is 7.854 ml/s
        self.assertEqual(FluidPlayback.flowText(7.854e-6), "7.854 ml/s")
        self.assertEqual(FluidPlayback.flowText(2.5e-3), "2.5 l/s")
        self.assertEqual(FluidPlayback.flowText(0.0), "0 ml/s")


class TestSetUp(FluidCase):
    def test_active_study(self):
        self.assertEqual(FluidPlayback.activeStudy(), self.study)
        self.assertEqual(FluidPlayback.activeStudy([self.fluid]), self.study)
        self.assertEqual(FluidPlayback.studyOf(self.height), self.study)
        self.assertIsNone(FluidPlayback.studyOf(self.water))
        other = FluidPlayback.makeStudy(self.doc, [self.floor])
        # two studies and none selected: nobody's guess
        self.assertIsNone(FluidPlayback.activeStudy())
        self.assertEqual(FluidPlayback.activeStudy([other]), other)
        self.assertEqual(other.Walls, [self.floor])

    def test_solids_only(self):
        sheet = self.doc.addObject("Part::Plane", "Sheet")
        self.doc.recompute()
        got = FluidPlayback.solidsOf([self.floor, sheet, self.study, self.fluid, self.water])
        self.assertEqual(got, [self.floor, self.water])

    def test_add_fluid_and_monitor(self):
        study = FluidPlayback.makeStudy(self.doc, [self.floor])
        fluid = FluidPlayback.addFluid(study, self.water)
        monitor = FluidPlayback.addMonitor(study, "Pressure", App.Vector(1, 2, 3))
        self.assertEqual(study.Group, [fluid, monitor])
        self.assertEqual(fluid.Volume, self.water)
        self.assertEqual(monitor.Kind, "Pressure")
        self.assertEqual(monitor.Point, App.Vector(1, 2, 3))
        study.Backend = "FreeFall"
        study.Resolution = 20
        self.assertEqual(study.buildModel()["Monitors"], 1)
        self.assertEqual(study.run(), 0)


class TestMovedParts(FluidCase):
    """A block the fluid moves, put where a frame has it and back
    (docs/FluidSimulation.md sec 12, S4): on the test data it falls 110 to
    the space's floor."""

    def addBlock(self):
        self.block = self.doc.addObject("Part::Box", "Block")
        self.block.Length, self.block.Width, self.block.Height = 50, 50, 50
        self.block.Placement.Base = App.Vector(200, 75, 100)
        return self.block

    def run_with(self, block):
        self.body = FluidPlayback.addBody(self.study, block, 600.0)
        self.assertEqual(self.body.Part, block)
        self.assertAlmostEqual(self.body.Density.getValueAs("kg/m^3").Value, 600.0, delta=1e-9)
        self.assertIn(self.body, self.study.Group)
        self.doc.recompute()
        self.assertEqual(self.study.run(), 0, self.study.Status)
        return FluidPlayback.Frames(self.study)

    def test_moved_and_put_back(self):
        block = self.addBlock()
        frames = self.run_with(block)
        self.assertEqual(frames.bodies, [block])
        fall = 0.5 * G * 0.1**2 * 1000.0
        self.assertAlmostEqual(frames.motions(2)[0].Base.z, -fall, delta=1e-9)
        moved = FluidPlayback.MovedParts(frames)
        moved.show(2)
        self.assertAlmostEqual(block.Placement.Base.z, 100.0 - fall, delta=1e-9)
        self.assertAlmostEqual(block.Placement.Base.x, 200.0, delta=1e-9)
        # from where it stood, not from where the last frame put it
        moved.show(8)
        self.assertAlmostEqual(block.Placement.Base.z, -10.0, delta=1e-9)
        moved.show(0)
        self.assertAlmostEqual(block.Placement.Base.z, 100.0, delta=1e-9)
        moved.show(8)
        moved.restore()
        self.assertEqual(block.Placement.Base, App.Vector(200, 75, 100))
        # and the study is of the model it ran
        self.assertFalse(self.study.isStale())
        # put back, it stays: nothing is kept of the frames
        moved.restore()
        self.assertEqual(block.Placement.Base, App.Vector(200, 75, 100))

    def test_a_part_inside_a_turned_group(self):
        # the block in a group that is turned a quarter about x and set
        # aside: the model's down is not the block's own
        block = self.addBlock()
        group = self.doc.addObject("App::Part", "Group")
        group.Placement = App.Placement(
            App.Vector(10, 0, 300), App.Rotation(App.Vector(1, 0, 0), 90)
        )
        group.addObject(block)
        self.doc.recompute()
        start = block.getGlobalPlacement()
        own = block.Placement
        self.assertNotEqual(start.Base, own.Base)
        frames = self.run_with(block)
        moved = FluidPlayback.MovedParts(frames)
        moved.show(2)
        fall = frames.motions(2)[0].Base.z
        self.assertLess(fall, -10.0)
        now = block.getGlobalPlacement()
        self.assertAlmostEqual(now.Base.z, start.Base.z + fall, delta=1e-9)
        self.assertAlmostEqual(now.Base.x, start.Base.x, delta=1e-9)
        self.assertAlmostEqual(now.Base.y, start.Base.y, delta=1e-9)
        self.assertTrue(now.Rotation.isSame(start.Rotation, 1e-12))
        moved.restore()
        self.assertTrue(block.Placement.isSame(own, 1e-12))

    def test_nothing_moved(self):
        self.assertEqual(self.study.run(), 0)
        frames = FluidPlayback.Frames(self.study)
        self.assertEqual(frames.bodies, [])
        self.assertEqual(frames.motions(1), [])
        moved = FluidPlayback.MovedParts(frames)
        moved.show(1)
        moved.restore()


class TestFrameView(unittest.TestCase):
    def setUp(self):
        try:
            from pivy import coin
        except ImportError:
            self.skipTest("pivy is not installed")
        self.coin = coin
        self.parent = coin.SoSeparator()
        self.parent.ref()

    def tearDown(self):
        self.parent.unref()

    def test_particles_then_fewer(self):
        import numpy

        view = FluidPlayback.FrameView(self.parent)
        points = numpy.arange(30, dtype=numpy.float32).reshape(10, 3)
        self.assertTrue(view.show(points))
        self.assertEqual(self.parent.getNumChildren(), 1)
        self.assertEqual(view.count(), 10)
        self.assertEqual(view.points.numPoints.getValue(), 10)
        self.assertEqual(view.switch.whichChild.getValue(), 0)
        self.assertEqual(tuple(view.coords.point[7].getValue()), (21.0, 22.0, 23.0))
        # a frame with fewer: none of the last one's left over
        self.assertTrue(view.show(points[:4]))
        self.assertEqual(self.parent.getNumChildren(), 1)
        self.assertEqual(view.count(), 4)
        self.assertEqual(view.points.numPoints.getValue(), 4)
        self.assertTrue(view.show(numpy.zeros((0, 3), dtype=numpy.float32)))
        self.assertEqual(view.count(), 0)
        view.remove()
        self.assertEqual(self.parent.getNumChildren(), 0)

    def test_surface(self):
        import numpy

        view = FluidPlayback.FrameView(self.parent)
        points = numpy.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], dtype=numpy.float32)
        triangles = numpy.array([[0, 1, 2], [0, 2, 3]], dtype=numpy.int32)
        self.assertTrue(view.show(points, triangles))
        self.assertEqual(view.switch.whichChild.getValue(), 1)
        index = [view.faces.coordIndex[i] for i in range(view.faces.coordIndex.getNum())]
        self.assertEqual(index, [0, 1, 2, -1, 0, 2, 3, -1])
        # and back to particles
        self.assertTrue(view.show(points))
        self.assertEqual(view.switch.whichChild.getValue(), 0)
        view.remove()

    def test_a_field_is_coloured_by_its_values(self):
        import numpy

        view = FluidPlayback.FrameView(self.parent)
        points = numpy.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], dtype=numpy.float32)
        triangles = numpy.array([[0, 1, 2], [0, 2, 3]], dtype=numpy.int32)
        values = numpy.array([0.0, 10.0, 20.0, 5.0], dtype=numpy.float32)
        self.assertTrue(view.showField(points, triangles, values, 0.0, 20.0))
        self.assertEqual(view.switch.whichChild.getValue(), view.FIELD)
        self.assertEqual(view.field.coordIndex.getNum(), 8)
        self.assertEqual(view.colors.diffuseColor.getNum(), 4)
        want = FluidPlayback.colorsOf(values, 0.0, 20.0)
        for got, expected in zip(view.colors.diffuseColor[2].getValue(), want[2]):
            self.assertAlmostEqual(got, float(expected), places=5)
        # the other shapes keep nothing, and a surface after it none of it
        self.assertEqual(view.faces.coordIndex.getNum(), 0)
        self.assertTrue(view.show(points, triangles))
        self.assertEqual(view.switch.whichChild.getValue(), view.SURFACE)
        self.assertEqual(view.field.coordIndex.getNum(), 0)
        view.remove()

    def test_body(self):
        import numpy

        view = FluidPlayback.FrameView(self.parent)
        points = numpy.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=numpy.float32)
        triangles = numpy.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [0, 3, 2]], dtype=numpy.int32)
        normals = numpy.array([[-1, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=numpy.float32)
        self.assertTrue(view.show(points, triangles, normals))
        self.assertEqual(view.switch.whichChild.getValue(), view.BODY)
        self.assertEqual(view.body.coordIndex.getNum(), 16)
        self.assertEqual(view.normals.vector.getNum(), 4)
        self.assertEqual(tuple(view.normals.vector[3].getValue()), (0.0, 0.0, 1.0))
        self.assertEqual(view.mist.numPoints.getValue(), 0)
        # with its spray: so many points of mist
        spray = numpy.array([[5, 5, 5], [6, 5, 5], [7, 5, 5]], dtype=numpy.float32)
        self.assertTrue(view.show(points, triangles, normals, spray))
        self.assertEqual(view.mist.numPoints.getValue(), 3)
        self.assertEqual(tuple(view.mistCoords.point[2].getValue()), (7.0, 5.0, 5.0))
        # a surface with no normals after it: the body keeps nothing, and
        # no spray is left
        self.assertTrue(view.show(points, triangles[:2]))
        self.assertEqual(view.switch.whichChild.getValue(), view.SURFACE)
        self.assertEqual(view.body.coordIndex.getNum(), 0)
        self.assertEqual(view.mist.numPoints.getValue(), 0)
        self.assertEqual(view.faces.coordIndex.getNum(), 8)
        view.remove()
