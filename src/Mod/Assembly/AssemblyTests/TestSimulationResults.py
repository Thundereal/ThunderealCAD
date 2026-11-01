# SPDX-License-Identifier: LGPL-2.1-or-later
"""Results kept in the document (docs/AssemblyDynamics.md sec 8.4): a study's
run saved with the file as its KeepResults says, read back when the study is
opened again, and marked stale when the mechanism changes after."""

import io
import os
import shutil
import tempfile
import unittest
import zipfile

import FreeCAD as App
import JointObject


def plc(x=0.0, y=0.0, z=0.0, axis=None, angle=0.0):
    rot = App.Rotation(axis, angle) if axis is not None else App.Rotation()
    return App.Placement(App.Vector(x, y, z), rot)


class TestSimulationResults(unittest.TestCase):
    def setUp(self):
        import AssemblyApp

        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.skipTest("the Chrono backend is not built")
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.folder = tempfile.mkdtemp(prefix="SimulationResults")
        self.doc = App.newDocument("TestSimulationResults")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)
        shutil.rmtree(self.folder, ignore_errors=True)

    def pendulum(self):
        """A rod 100 x 10 x 10 hinged at one end to a grounded block, lying
        horizontal along x"""
        base = self.assembly.newObject("Part::Box", "Base")
        base.Length, base.Width, base.Height = 10, 10, 10
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        rod = self.assembly.newObject("Part::Box", "Rod")
        rod.Length, rod.Width, rod.Height = 100, 10, 10
        rod.Placement.Base = App.Vector(10, 0, 0)
        self.doc.recompute()
        j = self.joints.newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(j, JointObject.JointTypes.index("Revolute"))
        j.Detach1 = True
        j.Detach2 = True
        j.Reference1 = (base, ["Face2"])
        j.Reference2 = (rod, ["Face1"])
        j.Placement1 = plc(10, 5, 5, App.Vector(1, 0, 0), -90)
        j.Placement2 = plc(0, 5, 5, App.Vector(1, 0, 0), -90)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        # Upstream's Python joint is converted to a C++ one, the GUI's, when
        # the file is opened -- under another name, another mechanism: done
        # before any run
        self.reopen()
        self.joint = next(o for o in self.joints.Group if o.isDerivedFrom("Assembly::Joint"))
        return self.doc.getObject("Rod")

    def simulate(self, end=0.05, step=1e-3, **props):
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        for name, value in props.items():
            setattr(sim, name, value)
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        return sim

    def placements(self, part):
        out = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            out.append(part.Placement)
        return out

    def reopen(self):
        """The document saved, closed and opened again; its assembly"""
        path = os.path.join(self.folder, "Results.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.assembly = self.doc.getObject("Assembly")
        self.joints = self.assembly.getGroup("Joints")
        return self.doc

    def assertSameResults(self, before, after, values=True):
        self.assertEqual(after["Times"], before["Times"])
        if values:
            self.assertEqual(len(after["Channels"]), len(before["Channels"]))
            for a, b in zip(after["Channels"], before["Channels"]):
                for key in ("Name", "Quantity", "Unique", "Values"):
                    self.assertEqual(a[key], b[key], key)
                # The document's own: the one before is closed
                self.assertEqual(a["Object"].Name, b["Name"])
        else:
            self.assertEqual(after["Channels"], [])
        self.assertEqual(len(after["Events"]), len(before["Events"]))
        for a, b in zip(after["Events"], before["Events"]):
            for key in ("Time", "Name", "Value", "Stopped", "Kind"):
                self.assertEqual(a[key], b[key], key)

    def assertSamePlacements(self, after, before):
        self.assertEqual(len(after), len(before))
        for a, b in zip(after, before):
            self.assertTrue(a.isSame(b, 1e-12), (a, b))

    def test_kept_in_the_file(self):
        """A run kept whole (All) is saved with the document: opened again,
        the study's results are read back -- the same times, values and
        events, bit for bit, and the same placement at each frame (1e-12) --
        and its archive is a zip numpy reads as it is"""
        rod = self.pendulum()
        sim = self.simulate(KeepResults="All")
        before = self.assembly.getSimulationResults()
        self.assertTrue(before["Channels"])
        frames = self.placements(rod)
        self.assertEqual(len(frames), 51)
        self.assertTrue(sim.Results)

        # numpy reads the blocks as they are
        import numpy

        with zipfile.ZipFile(sim.Results) as archive:
            names = set(archive.namelist())
            self.assertEqual(names, {"index.json", "times.npy", "poses.npy", "values.npy"})
            times = numpy.load(io.BytesIO(archive.read("times.npy")))
            poses = numpy.load(io.BytesIO(archive.read("poses.npy")))
            values = numpy.load(io.BytesIO(archive.read("values.npy")))
        self.assertEqual(list(times), before["Times"])
        self.assertEqual(poses.shape[0], 51)
        self.assertEqual(poses.shape[2], 7)
        self.assertEqual(values.shape[0], 51)

        name = sim.Name
        self.reopen()
        sim = self.doc.getObject(name)
        rod = self.doc.getObject("Rod")
        self.assertEqual(self.assembly.numberOfFrames(), 0)
        self.assertTrue(self.assembly.loadSimulationResults(sim))
        after = self.assembly.getSimulationResults()
        self.assertEqual(after["Simulation"], sim)
        self.assertSameResults(before, after)
        self.assertSamePlacements(self.placements(rod), frames)
        self.assertFalse(sim.ResultsStale)

    def test_what_is_kept(self):
        """Motion keeps the frames and the events, not the values; None
        keeps nothing, and a study opened again has no results"""
        rod = self.pendulum()
        motion = self.simulate(KeepResults="Motion")
        before = self.assembly.getSimulationResults()
        frames = self.placements(rod)
        with zipfile.ZipFile(motion.Results) as archive:
            self.assertNotIn("values.npy", archive.namelist())
        nothing = self.simulate(end=0.02, KeepResults="None")
        self.assertEqual(nothing.Results, "")

        names = motion.Name, nothing.Name
        self.reopen()
        motion, nothing = (self.doc.getObject(n) for n in names)
        rod = self.doc.getObject("Rod")
        self.assertFalse(self.assembly.loadSimulationResults(nothing))
        self.assertTrue(self.assembly.loadSimulationResults(motion))
        self.assertSameResults(before, self.assembly.getSimulationResults(), values=False)
        self.assertSamePlacements(self.placements(rod), frames)

    def test_each_study_keeps_its_own(self):
        """Two studies run one after the other each keep their run: the
        first's read back after the second's, and the second's again"""
        self.pendulum()
        first = self.simulate(end=0.05)
        second = self.simulate(end=0.02, step=1e-2)
        self.assertEqual(self.assembly.numberOfFrames(), 3)
        self.assertTrue(self.assembly.loadSimulationResults(first))
        self.assertEqual(self.assembly.numberOfFrames(), 51)
        self.assertEqual(self.assembly.getSimulationResults()["Simulation"], first)
        self.assertTrue(self.assembly.loadSimulationResults(second))
        self.assertEqual(self.assembly.numberOfFrames(), 3)
        self.assertEqual(self.assembly.getSimulationResults()["Simulation"], second)

    def test_stale_after_opening(self):
        """Opened again, a study's results are stale once the study or a
        joint changes, fresh once changed back, whether read back or not; a
        stale flag is saved with them, and stale results still play"""
        self.pendulum()
        sim = self.simulate()
        name = sim.Name
        self.reopen()
        sim = self.doc.getObject(name)
        joint = next(o for o in self.joints.Group if o.isDerivedFrom("Assembly::Joint"))

        def stale():
            self.doc.recompute()
            self.assertEqual(sim.ResultsStale, self.assembly.isSimulationStale(sim))
            return sim.ResultsStale

        # Not read back yet: the study's own change counts
        sim.TimeEnd = 0.04
        self.assertTrue(stale())
        sim.TimeEnd = 0.05
        self.assertFalse(stale())
        self.assertTrue(self.assembly.loadSimulationResults(sim))
        self.assembly.updateForFrame(10)
        self.assertFalse(stale())
        joint.Placement2 = plc(0, 5, 4, App.Vector(1, 0, 0), -90)
        self.assertTrue(stale())
        joint.Placement2 = plc(0, 5, 5, App.Vector(1, 0, 0), -90)
        self.assertFalse(stale())

        # Saved stale, opened stale, and played
        sim.TimeEnd = 0.04
        self.assertTrue(stale())
        self.reopen()
        sim = self.doc.getObject(name)
        self.assertTrue(sim.ResultsStale)
        self.assertTrue(self.assembly.loadSimulationResults(sim))
        self.assertEqual(self.assembly.numberOfFrames(), 51)
