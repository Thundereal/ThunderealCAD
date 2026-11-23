# SPDX-License-Identifier: LGPL-2.1-or-later

# Parts of a mechanism in the fluid, for every backend that runs with one
# (docs/FluidSimulation.md sec 12.9): Floating.py's tank and block, the
# block a part of an assembly, held on a vertical slider and moved by the
# Assembly's simulation under the fluid's push. Where it comes to rest is
# Archimedes' still.

import math

import FreeCAD as App
import Part

import FluidPlayback

from FluidTests.DamBreak import backendNamed

WATER = 998.2  # kg/m^3, the fluid's own
ALUMINUM = "9bf060e9-1663-44a2-88e2-2ff6ee858efe"  # Aluminum Generic's card


class Mechanism:
    """A tank 120 x 120 inside, open above, water 60 deep; a block 60 x 60 x
    40 in the middle, a part of an assembly, on a slider along z from a
    grounded block outside the tank; a resolution of 5 mm.

    A test class is this and unittest.TestCase, and sets BACKEND."""

    BACKEND = ""

    def setUp(self):
        found = backendNamed(self.BACKEND)
        if found is None or not found["Available"]:
            self.skipTest(f"no {self.BACKEND} on this machine")
        try:
            import AssemblyApp
        except ImportError:
            self.skipTest("no Assembly module in this build")
        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.skipTest("the Assembly's Chrono backend is not built")
        self.assertTrue(found["Mechanisms"])
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self.solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("FluidMechanism")
        self.tank = self.doc.addObject("Part::Feature", "Tank")
        outer = Part.makeBox(140, 140, 140, App.Vector(-10, -10, -10))
        self.tank.Shape = outer.cut(Part.makeBox(120, 120, 131))
        self.water = self.doc.addObject("Part::Box", "Water")
        self.water.Length, self.water.Width, self.water.Height = 120, 120, 60
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)

    def tearDown(self):
        if hasattr(self, "params"):
            self.params.SetBool("SolveInJointCreation", self.solveInCreation)
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def float_on_a_slider(self, density, bottom, end):
        """The block of that density (kg/m^3), its underside that high above
        the tank's floor, on its slider; the assembly's simulation and the
        study that runs with it, both 'end' long."""
        import JointObject
        import Materials

        base = self.assembly.newObject("Part::Box", "Base")
        base.Length, base.Width, base.Height = 10, 10, 10
        base.Placement.Base = App.Vector(-50, -50, 0)
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        self.block = self.assembly.newObject("Part::Box", "Block")
        self.block.Length, self.block.Width, self.block.Height = 60, 60, 40
        self.block.Placement.Base = App.Vector(30, 30, bottom)
        card = Materials.MaterialManager().getMaterial(ALUMINUM)
        card.Name = "Of the density asked"
        card.setPhysicalValue("Density", "%g kg/m^3" % density)
        self.block.ShapeMaterial = card
        self.doc.recompute()
        slider = self.joints.newObject("App::FeaturePython", "Slider")
        JointObject.Joint(slider, JointObject.JointTypes.index("Slider"))
        slider.Detach1 = True
        slider.Detach2 = True
        slider.Reference1 = (base, ["Face6"])
        slider.Reference2 = (self.block, ["Face5"])
        # one line along z, through both
        slider.Placement1 = App.Placement(App.Vector(5, 5, 10), App.Rotation())
        slider.Placement2 = App.Placement(App.Vector(-75, -75, 0), App.Rotation())
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        self.assertAlmostEqual(self.block.Placement.Base.z, bottom, places=9)
        simulations = self.assembly.getGroup("Simulations", True)
        self.simulation = simulations.newObject("Assembly::Simulation", "Simulation")
        self.simulation.AnalysisType = "Dynamic"
        self.simulation.TimeEnd = end
        self.simulation.TimeStepOutput = 0.05
        self.study = FluidPlayback.makeStudy(self.doc, [self.tank])
        FluidPlayback.addFluid(self.study, self.water)
        self.study.Backend = self.BACKEND
        self.study.Resolution = 5
        self.study.OutputInterval = 0.05
        self.study.TimeEnd = end
        self.body = FluidPlayback.addBody(self.study, self.block, density)
        self.study.Mechanism = self.simulation
        self.doc.recompute()

    def heights(self, bottom):
        """Where the block's underside is at each of the fluid's frames, how
        far it leans (deg), how many particles there are; and where the
        Assembly's frames have it."""
        results = self.study.getResults()
        self.assertEqual([body["Object"] for body in results["Bodies"]], [self.block])
        rows = []
        for index in range(results["Frames"]):
            frame = self.study.getFrame(index)
            (motion,) = frame["Bodies"]
            under = motion.multVec(App.Vector(60, 60, bottom))
            rows.append(
                (frame["Time"], under.z, math.degrees(motion.Rotation.Angle), frame["Count"])
            )
        mechanism = []
        for index in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(index)
            mechanism.append(self.block.Placement.Base.z)
        return rows, mechanism

    def test_the_parts_are_the_mechanisms(self):
        self.float_on_a_slider(500.0, 40.0, 0.1)
        (body,) = self.study.buildModel()["Bodies"]
        self.assertTrue(body["Driven"])
        signature = self.study.buildModel()["Signature"]
        # without the mechanism it is the fluid's own to move, and another
        # model
        self.study.Mechanism = None
        (body,) = self.study.buildModel()["Bodies"]
        self.assertFalse(body["Driven"])
        self.assertNotEqual(self.study.buildModel()["Signature"], signature)

    def test_a_float_on_a_slider_rests_at_its_draught(self):
        draught = 40.0 * 500.0 / WATER
        rest = 60.0 - draught
        self.float_on_a_slider(500.0, rest, 1.0)
        self.assertEqual(self.study.run(), 0, self.study.Status)
        rows, mechanism = self.heights(rest)
        self.assertEqual(len(rows), 21)
        self.assertEqual(len(mechanism), 21)
        for (time, under, lean, count), theirs in zip(rows, mechanism):
            # It bobs about its draught while the water settles against it
            # (measured: 2.1 down at 0.15 s, 0.3 up at 0.3 s, half as far by
            # 1 s), level on its slider, no water lost
            self.assertAlmostEqual(under, rest, delta=3.0, msg=f"at {time}")
            self.assertLess(lean, 1e-6, f"at {time}")
            self.assertEqual(count, rows[0][3], f"at {time}")
            # and the two runs are of one motion: the fluid's frame is the
            # first step at or past its time, the mechanism's on it
            self.assertAlmostEqual(under, theirs, delta=0.1, msg=f"at {time}")
        late = [under for time, under, _, _ in rows if time >= 0.5]
        # measured: 0.7 mm deep in the mean, as the block let go alone is
        self.assertAlmostEqual(sum(late) / len(late), rest, delta=1.5)

    def test_a_float_a_tenth_as_dense_as_the_water(self):
        # The force is a step behind the motion; a part lighter than the
        # water it moves with is where that would show
        draught = 40.0 * 100.0 / WATER
        rest = 60.0 - draught
        self.float_on_a_slider(100.0, rest, 1.0)
        self.assertEqual(self.study.run(), 0, self.study.Status)
        rows, _ = self.heights(rest)
        self.assertEqual(len(rows), 21)
        for time, under, lean, count in rows:
            # measured: within 1.1 mm
            self.assertAlmostEqual(under, rest, delta=2.5, msg=f"at {time}")
            self.assertEqual(count, rows[0][3], f"at {time}")

    def test_let_go_above_the_water_it_comes_to_float(self):
        # Floating.py's dropped block, on the slider: under its draught and
        # back up
        draught = 40.0 * 500.0 / WATER
        rest = 60.0 + draught * 60.0 * 60.0 / (120.0 * 120.0) - draught
        self.float_on_a_slider(500.0, 70.0, 1.5)
        self.assertEqual(self.study.run(), 0, self.study.Status)
        rows, _ = self.heights(70.0)
        self.assertLess(min(under for _, under, _, _ in rows), rest - 5.0)
        for time, under, lean, count in rows:
            self.assertEqual(count, rows[0][3], f"at {time}")
        late = [under for time, under, _, _ in rows if time >= 1.0]
        self.assertAlmostEqual(sum(late) / len(late), rest, delta=3.0)

    def test_what_cannot_run_with_a_mechanism(self):
        self.float_on_a_slider(500.0, 40.0, 0.1)
        # a backend that runs with none says so
        self.study.Backend = "FreeFall"
        self.assertEqual(self.study.run(), -2)
        self.assertIn("cannot run with a mechanism", self.study.Status)
        self.study.Backend = self.BACKEND
        # a part that is not the assembly's is nobody's to move
        other = self.doc.addObject("Part::Box", "Other")
        other.Length, other.Width, other.Height = 20, 20, 20
        other.Placement.Base = App.Vector(5, 5, 70)
        FluidPlayback.addBody(self.study, other, 500.0)
        self.doc.recompute()
        self.assertEqual(self.study.run(), -1)
        self.assertIn("could not be started", self.study.Status)
        self.assertFalse(self.study.getProgress()["Running"])
