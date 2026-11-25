# SPDX-License-Identifier: LGPL-2.1-or-later

# Parts of a mechanism in the fluid, for every backend that runs with one
# (docs/FluidSimulation.md sec 12.9): Floating.py's tank and block, the
# block a part of an assembly, held on a vertical slider and moved by the
# Assembly's simulation under the fluid's push. Where it comes to rest is
# Archimedes' still. And a bar on a hinge into that water, where its
# buoyancy's moment about the hinge is its weight's.

import math
import os

import FreeCAD as App
import Part

import FluidPlayback

from FluidTests.DamBreak import backendNamed

WATER = 998.2  # kg/m^3, the fluid's own
ALUMINUM = "9bf060e9-1663-44a2-88e2-2ff6ee858efe"  # Aluminum Generic's card
BAR = (100.0, 20.0, 20.0)  # the hinged bar: along x, across, thick
HINGE = (10.0, 60.0, 80.0)  # its hinge, 20 above the water
TANK = 120.0
DEPTH = 60.0


def below(section, level):
    """The part of a polygon in x, z below z = level."""
    kept = []
    for index, start in enumerate(section):
        end = section[(index + 1) % len(section)]
        if start[1] <= level:
            kept.append(start)
        if (start[1] <= level) != (end[1] <= level):
            along = (level - start[1]) / (end[1] - start[1])
            kept.append((start[0] + along * (end[0] - start[0]), level))
    return kept


def areaAndCentre(section):
    """A polygon's area and its centroid's x."""
    area = moment = 0.0
    for index, (x0, z0) in enumerate(section):
        x1, z1 = section[(index + 1) % len(section)]
        cross = x0 * z1 - x1 * z0
        area += cross
        moment += (x0 + x1) * cross
    area *= 0.5
    if abs(area) < 1e-12:
        return 0.0, 0.0
    return abs(area), moment / (6.0 * area)


def barAtRest(density, shrink=0.0, water=TANK * TANK * DEPTH):
    """The angle (deg) below level where the hinged bar's buoyancy has the
    moment about the hinge its weight has, with that volume of water (mm^3)
    risen by what the bar pushes aside. The buoyancy is of the bar as the
    fluid would see it 'shrink' thinner on every face; the weight is of the
    whole bar. Its section in x, z is all that counts: across, the bar is
    uniform."""
    length, width, thick = BAR

    def section(turn, inset):
        c, s = math.cos(turn), math.sin(turn)
        low, high = -thick / 2 + inset, thick / 2 - inset
        corners = [(inset, low), (length - inset, low), (length - inset, high), (inset, high)]
        return [(HINGE[0] + x * c + z * s, HINGE[2] - x * s + z * c) for x, z in corners]

    def level(turn):
        low, high = 0.0, 2.0 * DEPTH
        for _ in range(60):
            middle = 0.5 * (low + high)
            under, _ = areaAndCentre(below(section(turn, shrink), middle))
            if TANK * TANK * middle - (width - 2 * shrink) * under < water:
                low = middle
            else:
                high = middle
        return 0.5 * (low + high)

    def moment(turn):
        under, centre = areaAndCentre(below(section(turn, shrink), level(turn)))
        _, weight = areaAndCentre(section(turn, 0.0))
        pushed = WATER * (width - 2 * shrink) * under * (centre - HINGE[0])
        return pushed - density * length * width * thick * (weight - HINGE[0])

    low, high = 0.0, math.radians(80)
    for _ in range(60):
        middle = 0.5 * (low + high)
        if moment(middle) < 0:  # the weight wins: further down
            low = middle
        else:
            high = middle
    return math.degrees(0.5 * (low + high))


class Mechanism:
    """A tank 120 x 120 inside, open above, water 60 deep; a block 60 x 60 x
    40 in the middle, a part of an assembly, on a slider along z from a
    grounded block outside the tank; or a bar on a hinge from it
    (bar_on_a_hinge); a resolution of 5 mm.

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

    def bar_on_a_hinge(self, density, start, end):
        """A bar 100 x 20 x 20 (along x, across, thick) of that density on a
        revolute about y at the middle of its end, the hinge at x 10 and 20
        above the water, turned 'start' deg down from level; the assembly's
        simulation and the study that runs with it, both 'end' long."""
        import JointObject
        import Materials

        base = self.assembly.newObject("Part::Box", "Base")
        base.Length, base.Width, base.Height = 10, 10, 10
        base.Placement.Base = App.Vector(-50, -50, 0)
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        self.bar = self.assembly.newObject("Part::Box", "Bar")
        self.bar.Length, self.bar.Width, self.bar.Height = BAR
        down = App.Rotation(App.Vector(0, 1, 0), start)
        self.bar.Placement = App.Placement(App.Vector(*HINGE), down) * App.Placement(
            App.Vector(0, -BAR[1] / 2, -BAR[2] / 2), App.Rotation()
        )
        card = Materials.MaterialManager().getMaterial(ALUMINUM)
        card.Name = "Of the density asked"
        card.setPhysicalValue("Density", "%g kg/m^3" % density)
        self.bar.ShapeMaterial = card
        self.doc.recompute()
        hinge = self.joints.newObject("App::FeaturePython", "Hinge")
        JointObject.Joint(hinge, JointObject.JointTypes.index("Revolute"))
        hinge.Detach1 = True
        hinge.Detach2 = True
        hinge.Reference1 = (base, ["Face2"])
        hinge.Reference2 = (self.bar, ["Face1"])
        # the axis along y, through the middle of the bar's end
        along = App.Rotation(App.Vector(1, 0, 0), -90)
        hinge.Placement1 = App.Placement(App.Vector(HINGE[0] + 50, 110, HINGE[2]), along)
        hinge.Placement2 = App.Placement(App.Vector(0, BAR[1] / 2, BAR[2] / 2), along)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        self.assertAlmostEqual(self.angle(), start, places=6)
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
        FluidPlayback.addBody(self.study, self.bar, density)
        self.study.Mechanism = self.simulation
        self.doc.recompute()

    def angle(self):
        """How far the bar is turned down from level (deg), by its far end's
        middle."""
        tip = self.bar.Placement.multVec(App.Vector(BAR[0], BAR[1] / 2, BAR[2] / 2))
        return math.degrees(math.atan2(HINGE[2] - tip.z, tip.x - HINGE[0]))

    def angles(self):
        """The bar's angle at each of the Assembly's frames, and the
        particles at each of the fluid's."""
        angles = []
        for index in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(index)
            angles.append(self.angle())
        counts = [
            self.study.getFrame(index)["Count"]
            for index in range(self.study.getResults()["Frames"])
        ]
        return angles, counts

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
        # a simulation of the joints' motions alone takes no push
        self.simulation.AnalysisType = "Kinematic"
        self.assertEqual(self.study.run(), -2)
        self.assertIn("Simulation is not a dynamic simulation", self.study.Status)
        self.simulation.AnalysisType = "Dynamic"
        # a part held to the ground goes nowhere
        import JointObject

        held = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBlock")
        JointObject.GroundedJoint(held, self.block)
        self.doc.recompute()
        self.assertEqual(self.study.run(), -2)
        self.assertIn("Block is grounded in Assembly", self.study.Status)
        self.doc.removeObject(held.Name)
        self.doc.recompute()
        # a part that is not the assembly's is nobody's to move
        other = self.doc.addObject("Part::Box", "Other")
        other.Length, other.Width, other.Height = 20, 20, 20
        other.Placement.Base = App.Vector(5, 5, 70)
        FluidPlayback.addBody(self.study, other, 500.0)
        self.doc.recompute()
        self.assertEqual(self.study.run(), -2)
        self.assertIn("Other is not a part of Assembly", self.study.Status)
        self.assertFalse(self.study.getProgress()["Running"])

    def links(self):
        """The study's objects' links, each by its object and property, as
        the names of what they link."""

        def named(value):
            if isinstance(value, tuple):
                return (value[0].Name, tuple(value[1]))
            if isinstance(value, list):
                return [obj.Name for obj in value]
            return value.Name if value is not None else None

        study = self.study
        found = {"Walls": named(study.Walls), "Mechanism": named(study.Mechanism)}
        for child in study.Group:
            for name in ("Volume", "Part", "Face"):
                if name in child.PropertiesList:
                    found[child.TypeId + "." + name] = named(getattr(child, name))
        return found

    def test_links_reach_into_an_assembly(self):
        # The parts the fluid moves are an assembly's, the mechanism its
        # simulation, a wall may be a body's: no link of a study is held to
        # its own group, as a plain link is (it warned at every recompute)
        self.float_on_a_slider(500.0, 40.0, 0.1)
        inlet = FluidPlayback.addInlet(self.study, self.tank, "Face1", 100.0)
        outlet = self.doc.addObject("Fluid::Outlet", "Outlet")
        outlet.Face = (self.tank, ["Face2"])
        self.study.Group = self.study.Group + [outlet]
        FluidPlayback.addForceMonitor(self.study, self.tank, App.Vector(0, 0, 1))
        self.doc.recompute()
        for obj in [self.study] + self.study.Group:
            for name in obj.PropertiesList:
                kind = obj.getTypeIdOfProperty(name)
                # Group and the _ ones: the group's own, its children
                ours = name != "Group" and not name.startswith("_")
                if kind.startswith("App::PropertyLink") and ours:
                    self.assertTrue(kind.endswith("Global"), f"{obj.Name}.{name} is {kind}")
        before = self.links()
        self.assertEqual(len(before), 7)
        # a file saved before, when they were plain links, keeps them
        import shutil
        import tempfile
        import zipfile

        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "Mechanism.FCStd")
        # every entry one zipfile reads
        self.doc.BlobCompression = "Deflate"
        self.doc.saveAs(path)
        name = self.study.Name
        App.closeDocument(self.doc.Name)
        old = os.path.join(folder, "Before.FCStd")
        with zipfile.ZipFile(path) as source, zipfile.ZipFile(old, "w") as target:
            for item in source.infolist():
                data = source.read(item.filename)
                if item.filename == "Document.xml":
                    text = data.decode("utf-8")
                    self.assertIn('type="App::PropertyLinkGlobal"', text)
                    for kind in ("PropertyLink", "PropertyLinkList", "PropertyLinkSub"):
                        text = text.replace(f'type="App::{kind}Global"', f'type="App::{kind}"')
                    data = text.encode("utf-8")
                target.writestr(item, data)
        self.doc = App.openDocument(old)
        self.study = self.doc.getObject(name)
        after = self.links()
        self.assertEqual(after, before)
        self.assertNotIn(None, before.values())

    def test_linking_a_mechanism(self):
        self.float_on_a_slider(500.0, 40.0, 0.1)
        self.study.Mechanism = None
        self.assertEqual(FluidPlayback.mechanismsOf(self.doc), [self.simulation])
        # the two run as one: one end, frames on the same times
        self.study.TimeEnd = 1.5
        self.study.OutputInterval = 0.02
        FluidPlayback.linkMechanism(self.study, self.simulation)
        self.assertEqual(self.study.Mechanism, self.simulation)
        self.assertAlmostEqual(self.simulation.TimeEnd.Value, 1.5)
        self.assertAlmostEqual(self.simulation.TimeStepOutput.Value, 0.02)
        (body,) = self.study.buildModel()["Bodies"]
        self.assertTrue(body["Driven"])
        FluidPlayback.linkMechanism(self.study, None)
        self.assertIsNone(self.study.Mechanism)
        self.assertAlmostEqual(self.simulation.TimeEnd.Value, 1.5)

    def settles(self, density, start, end=2.0):
        """The bar of that density on its hinge, let go 'start' deg down, run
        'end' long: the mean of its angle over the last 30% and how far it
        swings about it, the hinge held, the two runs of one motion, no water
        lost; and the volume of water the fluid has (mm^3)."""
        self.bar_on_a_hinge(density, start, end)
        tip = self.bar.Placement.multVec(App.Vector(BAR[0], BAR[1] / 2, BAR[2] / 2))
        modelled = App.Placement(self.bar.Placement)
        self.assertEqual(self.study.run(), 0, self.study.Status)
        self.wallPointsOnTheBar(modelled)
        angles, counts = self.angles()
        frames = round(end / 0.05) + 1
        self.assertEqual(len(angles), frames)
        self.assertEqual(len(counts), frames)
        self.assertEqual(set(counts), {counts[0]}, "no water lost")
        for index, theirs in enumerate(angles):
            self.assembly.updateForFrame(index)
            end = self.bar.Placement.multVec(App.Vector(0, BAR[1] / 2, BAR[2] / 2))
            self.assertLess((end - App.Vector(*HINGE)).Length, 1e-3, f"frame {index}")
            # the fluid's frame has the bar where the mechanism's does
            (motion,) = self.study.getFrame(index)["Bodies"]
            moved = motion.multVec(tip)
            ours = math.degrees(math.atan2(HINGE[2] - moved.z, moved.x - HINGE[0]))
            # up to a step of the fluid's ahead (2.3 ms at 5 mm): as far as
            # the bar turns in 3 ms more than the slider's 0.1 (measured
            # 0.23 deg at 0.05 s, falling at near 200 deg/s)
            around = angles[max(index - 1, 0) : index + 2]
            rate = (max(around) - min(around)) / (0.05 * (len(around) - 1))
            self.assertAlmostEqual(ours, theirs, delta=0.1 + 0.003 * rate, msg=f"frame {index}")
        late = angles[round(0.7 * (frames - 1)) :]
        mean = sum(late) / len(late)
        swing = max(abs(angle - mean) for angle in late)
        spacing = self.study.Resolution.Value
        App.Console.PrintLog(f"bar {density}: {mean:.2f} deg, swing {swing:.2f}\n")
        return mean, swing, counts[0] * spacing**3

    def wallPointsOnTheBar(self, modelled):
        """The walls the water's surface is told of at the last frame: the
        bar's points, taken back by the frame's motion, are on the bar as
        it was modelled, facing out of it."""
        import numpy

        last = self.study.getResults()["Frames"] - 1
        walls = self.study.getWallPoints(last)
        points = numpy.frombuffer(walls["Points"], dtype=numpy.float32).reshape(-1, 3)
        normals = numpy.frombuffer(walls["Normals"], dtype=numpy.float32).reshape(-1, 3)
        (motion,) = self.study.getFrame(last)["Bodies"]
        back = modelled.inverse().multiply(motion.inverse())
        matrix = numpy.array(back.toMatrix().A).reshape(4, 4)
        local = points @ matrix[:3, :3].T + matrix[:3, 3]
        turned = normals @ matrix[:3, :3].T
        size = numpy.array(BAR)
        inside = numpy.all((local > -1e-2) & (local < size + 1e-2), axis=1)
        on = local[inside]
        out = turned[inside]
        # the bar's faces: 2 x (100 x 20) twice and 2 x (20 x 20), a point
        # each half the spacing at most
        self.assertGreater(len(on), 2 * (2 * 100 * 20 + 20 * 20) / 2.5**2 * 0.9)
        # off the edges, where a point is two faces' and has its triangle's
        faces = (numpy.abs(on) < 1e-2) | (numpy.abs(on - size) < 1e-2)
        one = faces.sum(axis=1) == 1
        for face in range(3):
            at_low = one & (numpy.abs(on[:, face]) < 1e-2)
            at_high = one & (numpy.abs(on[:, face] - size[face]) < 1e-2)
            self.assertTrue(numpy.any(at_low) and numpy.any(at_high), face)
            self.assertTrue(numpy.all(out[at_low, face] < -0.99), face)
            self.assertTrue(numpy.all(out[at_high, face] > 0.99), face)
        # and every one of them on a face
        self.assertTrue(numpy.all(faces.any(axis=1)))

    def test_a_bar_on_a_hinge_let_go_level(self):
        # Where it rests is where the buoyancy's moment about the hinge is
        # the weight's: 16.45 deg down, the water risen 0.9 mm. The fluid
        # sees a part a little thinner than it is, as the block on its
        # slider shows (0.7 mm deep in the mean, a skin 0.3 mm thin); a
        # bar 20 thick feels that more: it rests as the closed form has it
        # for a bar 0.25 to 0.6 mm thinner on every face (measured 17.42
        # deg, 0.43 mm; at three tenths the water's density 0.33 mm, at
        # seven tenths 0.38). The torque's path, the centre's to the hinge's,
        # wrong would put it degrees off.
        self.assertAlmostEqual(barAtRest(500.0), 16.454, places=3)
        mean, swing, water = self.settles(500.0, 0.0)
        self.assertGreater(mean, barAtRest(500.0, 0.25, water))
        self.assertLess(mean, barAtRest(500.0, 0.6, water))
        self.assertLess(swing, 0.6)  # measured 0.25

    def test_a_bar_on_a_hinge_put_at_rest(self):
        # Put where it rests, the fluid is not seeded where the bar is: less
        # water, a lower level, a deeper rest (measured 18.13 deg, swinging
        # 0.15; 17.14 by the closed form for that water, 17.68 to 18.62 for
        # the skin)
        mean, swing, water = self.settles(500.0, barAtRest(500.0))
        self.assertLess(water, TANK * TANK * DEPTH)
        self.assertGreater(mean, barAtRest(500.0, 0.25, water))
        self.assertLess(mean, barAtRest(500.0, 0.6, water))
        self.assertLess(swing, 0.6)

    def test_a_lighter_bar_on_a_hinge(self):
        # Of the same skin: measured 13.61 deg at three tenths of the
        # water's density, 13.08 by the closed form; the least damped
        mean, swing, water = self.settles(300.0, 0.0)
        self.assertGreater(mean, barAtRest(300.0, 0.25, water))
        self.assertLess(mean, barAtRest(300.0, 0.6, water))
        self.assertLess(swing, 1.0)  # measured 0.67

    def test_a_heavier_bar_on_a_hinge(self):
        # Measured 23.80 deg at seven tenths, 21.07 by the closed form: the
        # bar mostly under, the same skin turns it further. It falls to 44
        # deg first and is still swinging 0.95 at 1.4 s: 3 s
        mean, swing, water = self.settles(700.0, 0.0, 3.0)
        self.assertGreater(mean, barAtRest(700.0, 0.25, water))
        self.assertLess(mean, barAtRest(700.0, 0.6, water))
        self.assertLess(swing, 0.8)  # measured 0.3
