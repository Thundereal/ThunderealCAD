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
