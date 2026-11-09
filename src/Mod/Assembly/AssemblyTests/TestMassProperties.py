# SPDX-License-Identifier: LGPL-2.1-or-later
"""Mass properties (docs/AssemblyDynamics.md sec 5.2, phase D0):
AssemblyObject.getMassProperties on boxes whose answers are closed form."""

import os
import tempfile
import unittest

import FreeCAD as App
import JointObject
import Materials
import Part

PARAMS = "User parameter:BaseApp/Preferences/Mod/Assembly"
ALUMINUM = 2.7e-6  # kg/mm^3


def card(name):
    """The material card of this name."""
    for material in Materials.MaterialManager().Materials.values():
        if material.Name == name:
            return material
    raise unittest.SkipTest("no material card " + name)


def boxInertia(mass, a, b, c):
    """A box's inertia about its centre along its edges a, b, c (x, y, z)."""
    return (
        mass * (b * b + c * c) / 12,
        mass * (a * a + c * c) / 12,
        mass * (a * a + b * b) / 12,
        0.0,
        0.0,
        0.0,
    )


class TestMassProperties(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestMassProperties")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.params = App.ParamGet(PARAMS)
        self._default = self.params.GetString("DefaultMaterial", "")

    def tearDown(self):
        App.closeDocument(self.doc.Name)
        if self._default:
            self.params.SetString("DefaultMaterial", self._default)
        else:
            self.params.RemString("DefaultMaterial")

    def box(self, name, size, at, parent=None):
        b = (parent or self.assembly).newObject("Part::Box", name)
        b.Length, b.Width, b.Height = size
        b.Placement.Base = App.Vector(*at)
        return b

    def props(self):
        self.doc.recompute()
        result = self.assembly.getMassProperties()
        return {c["Object"].Name: c for c in result["Components"]}, result["Total"]

    def assertTupleAlmost(self, got, want, places=9):
        self.assertEqual(len(got), len(want))
        for g, w in zip(got, want):
            self.assertAlmostEqual(g, w, places=places)

    def test_assumed(self):
        """No material: the default's, Aluminum Generic, said to be assumed."""
        self.box("Cube", (10, 10, 10), (0, 0, 0))
        components, _ = self.props()
        cube = components["Cube"]
        self.assertEqual(cube["Origin"], "Assumed")
        self.assertIn("Alumin", cube["Material"])
        self.assertAlmostEqual(cube["Density"], ALUMINUM, places=15)
        self.assertAlmostEqual(cube["Volume"], 1000.0, places=6)
        mass = 1000 * ALUMINUM
        self.assertAlmostEqual(cube["Mass"], mass, places=12)
        self.assertTupleAlmost(cube["CenterOfMass"], (5.0, 5.0, 5.0))
        self.assertTupleAlmost(cube["Inertia"], boxInertia(mass, 10, 10, 10))

    def test_moved_by_script(self):
        """A component a script moves and purges, as the player puts the parts
        back, is where it now stands: the assembly's shape cache kept it
        where it had been (D4a's free body, its centre 126 mm off)."""
        cube = self.box("Cube", (10, 10, 10), (0, 0, 0))
        components, _ = self.props()
        self.assertTupleAlmost(components["Cube"]["CenterOfMass"], (5.0, 5.0, 5.0))
        cube.Placement.Base = App.Vector(100, 0, 0)
        cube.purgeTouched()
        result = self.assembly.getMassProperties()
        self.assertTupleAlmost(result["Components"][0]["CenterOfMass"], (105.0, 5.0, 5.0))

    def test_material(self):
        """A material's density; the inertia along the assembly's axes, a box
        turned a quarter about z having its x and y swapped."""
        steel = card("Steel-Generic")
        b = self.box("Beam", (10, 20, 30), (100, 0, 0))
        b.ShapeMaterial = steel
        b.Placement.Rotation = App.Rotation(App.Vector(0, 0, 1), 90)
        components, _ = self.props()
        beam = components["Beam"]
        self.assertEqual(beam["Origin"], "Shape")
        self.assertEqual(beam["Material"], "Steel-Generic")
        density = beam["Density"]
        self.assertAlmostEqual(density, 7.9e-6, places=12)
        mass = 6000 * density
        self.assertAlmostEqual(beam["Mass"], mass, places=12)
        # Turned: its 10 along y, its 20 along -x
        self.assertTupleAlmost(beam["CenterOfMass"], (90.0, 5.0, 15.0))
        self.assertTupleAlmost(beam["Inertia"], boxInertia(mass, 20, 10, 30), places=7)

    def test_containers(self):
        """A part holding two boxes is one component; a link has what it
        links to, where it puts it."""
        holder = self.assembly.newObject("App::Part", "Holder")
        self.box("In1", (10, 10, 10), (0, 0, 0), holder)
        self.box("In2", (10, 10, 10), (20, 0, 0), holder)
        holder.Placement.Base = App.Vector(0, 100, 0)
        cube = self.box("Cube", (10, 10, 10), (0, 0, 0))
        link = self.assembly.newObject("App::Link", "Link")
        link.LinkedObject = cube
        link.Placement.Base = App.Vector(0, 0, 100)
        components, _ = self.props()
        m = 1000 * ALUMINUM
        h = components["Holder"]
        self.assertAlmostEqual(h["Mass"], 2 * m, places=12)
        self.assertTupleAlmost(h["CenterOfMass"], (15.0, 105.0, 5.0))
        # Each cube 10 from the centre along x
        cube = boxInertia(m, 10, 10, 10)
        self.assertTupleAlmost(
            h["Inertia"], (2 * cube[0], 2 * (cube[1] + m * 100), 2 * (cube[2] + m * 100), 0, 0, 0)
        )
        self.assertTupleAlmost(components["Link"]["CenterOfMass"], (5.0, 5.0, 105.0))
        self.assertAlmostEqual(components["Link"]["Mass"], m, places=12)

    def test_total(self):
        """The total by the parallel-axis theorem, products of inertia too."""
        self.box("A", (10, 10, 10), (0, 0, 0))
        self.box("B", (10, 20, 30), (40, 10, -20))
        components, total = self.props()
        mass = sum(c["Mass"] for c in components.values())
        self.assertAlmostEqual(total["Mass"], mass, places=12)
        centre = sum((c["CenterOfMass"] * c["Mass"] for c in components.values()), App.Vector())
        centre = centre * (1 / mass)
        self.assertTupleAlmost(total["CenterOfMass"], tuple(centre))
        want = [0.0] * 6
        for c in components.values():
            m = c["Mass"]
            d = c["CenterOfMass"] - centre
            moved = (
                m * (d.y * d.y + d.z * d.z),
                m * (d.x * d.x + d.z * d.z),
                m * (d.x * d.x + d.y * d.y),
                -m * d.x * d.y,
                -m * d.x * d.z,
                -m * d.y * d.z,
            )
            for i in range(6):
                want[i] += c["Inertia"][i] + moved[i]
        self.assertTupleAlmost(total["Inertia"], want)
        self.assertNotEqual(total["Inertia"][3], 0.0)
        self.assertEqual(total["Origin"], "Assumed")
        self.assertIsNone(total["Object"])

    def test_massless(self):
        """A component without a solid has no mass, and adds none."""
        self.box("Cube", (10, 10, 10), (0, 0, 0))
        face = self.assembly.newObject("Part::Feature", "Sheet")
        face.Shape = Part.makePlane(10, 10)
        components, total = self.props()
        self.assertEqual(components["Sheet"]["Origin"], "None")
        self.assertEqual(components["Sheet"]["Mass"], 0.0)
        self.assertAlmostEqual(total["Mass"], components["Cube"]["Mass"], places=12)

    def test_default_material(self):
        """The default material is a preference, by the card's UUID."""
        steel = card("Steel-Generic")
        self.params.SetString("DefaultMaterial", steel.UUID)
        self.box("Cube", (10, 10, 10), (0, 0, 0))
        components, _ = self.props()
        self.assertEqual(components["Cube"]["Origin"], "Assumed")
        self.assertEqual(components["Cube"]["Material"], "Steel-Generic")
        self.assertAlmostEqual(components["Cube"]["Mass"], 1000 * 7.9e-6, places=12)

    def override(self, component, mode):
        """A mass override of the component, made in the Physics group."""
        physics = self.assembly.getGroup("Physics", True)
        given = physics.newObject("Assembly::MassOverride", "MassOverride")
        given.Component = component
        given.Mode = mode
        return given

    def test_physics_group(self):
        """Overrides are shown in the assembly's Physics group, and are no
        components."""
        cube = self.box("Cube", (10, 10, 10), (0, 0, 0))
        given = self.override(cube, "Density")
        physics = self.assembly.getGroup("Physics")
        self.assertIsNotNone(physics)
        self.assertEqual(physics.Role, "Physics")
        self.assertIn(given, physics.Group)
        components, _ = self.props()
        self.assertEqual(sorted(components), ["Cube"])

    def test_override_density(self):
        """Density: the shape at the given density, whatever the material."""
        cube = self.box("Cube", (10, 10, 10), (0, 0, 0))
        given = self.override(cube, "Density")
        given.Density = "7900 kg/m^3"
        components, total = self.props()
        props = components["Cube"]
        self.assertEqual(props["Origin"], "Override")
        mass = 1000 * 7.9e-6
        self.assertAlmostEqual(props["Mass"], mass, places=12)
        self.assertAlmostEqual(props["Density"], 7.9e-6, places=15)
        self.assertTupleAlmost(props["Inertia"], boxInertia(mass, 10, 10, 10))
        self.assertEqual(total["Origin"], "Override")

    def test_override_mass(self):
        """Mass: the shape's centre, its inertia scaled to the given mass."""
        cube = self.box("Cube", (10, 10, 10), (0, 0, 0))
        given = self.override(cube, "Mass")
        given.Mass = "1 kg"
        components, _ = self.props()
        props = components["Cube"]
        self.assertEqual(props["Origin"], "Override")
        self.assertAlmostEqual(props["Mass"], 1.0, places=12)
        self.assertTupleAlmost(props["CenterOfMass"], (5.0, 5.0, 5.0))
        self.assertTupleAlmost(props["Inertia"], boxInertia(1.0, 10, 10, 10))

    def test_override_full(self):
        """Full: the given mass, centre and inertia, in the component's
        frame -- turned a quarter about z, its x along the assembly's y."""
        cube = self.box("Cube", (10, 10, 10), (100, 0, 0))
        cube.Placement.Rotation = App.Rotation(App.Vector(0, 0, 1), 90)
        given = self.override(cube, "Full")
        given.Mass = "2 kg"
        given.CenterOfMass = App.Vector(1, 2, 3)
        given.Inertia = [10.0, 20.0, 30.0, 4.0, 0.0, 0.0]
        components, _ = self.props()
        props = components["Cube"]
        self.assertEqual(props["Origin"], "Override")
        self.assertAlmostEqual(props["Mass"], 2.0, places=12)
        # (1, 2, 3) turned: (-2, 1, 3), at (100, 0, 0)
        self.assertTupleAlmost(props["CenterOfMass"], (98.0, 1.0, 3.0))
        # x and y swapped, the product xy's sign turned
        self.assertTupleAlmost(props["Inertia"], (20.0, 10.0, 30.0, -4.0, 0.0, 0.0))

    def test_saved(self):
        """An override and its Physics group are saved and restored."""
        cube = self.box("Cube", (10, 10, 10), (0, 0, 0))
        given = self.override(cube, "Mass")
        given.Mass = "3 kg"
        before, _ = self.props()
        path = os.path.join(tempfile.mkdtemp(), "MassOverride.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.assembly = self.doc.getObject("Assembly")
        physics = self.assembly.getGroup("Physics")
        self.assertIsNotNone(physics)
        (given,) = physics.Group
        self.assertEqual(given.TypeId, "Assembly::MassOverride")
        self.assertEqual(given.Mode, "Mass")
        self.assertEqual(given.Component, self.doc.getObject("Cube"))
        after, _ = self.props()
        self.assertAlmostEqual(after["Cube"]["Mass"], 3.0, places=12)
        self.assertTupleAlmost(after["Cube"]["Inertia"], before["Cube"]["Inertia"])

    def test_body_inertias(self):
        """The solver's bodies weigh their components: S and R, a rigid
        group on a slider, are one body, in the frame of R, the group's
        representative -- turned, so its axes are not the assembly's."""
        params = self.params
        creation = params.GetBool("SolveInJointCreation", True)
        params.SetBool("SolveInJointCreation", False)
        try:
            g = self.box("G", (10, 10, 10), (0, 0, 0))
            s = self.box("S", (10, 10, 10), (0, 0, 10))
            r = self.box("R", (4, 6, 8), (20, 0, 10))
            r.Placement.Rotation = App.Rotation(App.Vector(0, 0, 1), 30)
            joints = self.assembly.getGroup("Joints", True)
            ground = joints.newObject("Assembly::GroundedJointPython", "GroundedG")
            JointObject.GroundedJoint(ground, g)
            self.doc.recompute()
            self.assembly.makeJoint("Slider", (g, "Face6"), (s, "Face5"))
            group = joints.newObject("Assembly::RigidGroupPython", "RigidGroupJoint")
            JointObject.RigidGroupJoint(group, [s, r])
            self.doc.recompute()
            components, _ = self.props()
            bodies = self.assembly.getBodyInertias()
        finally:
            params.SetBool("SolveInJointCreation", creation)
        byObjects = {tuple(o.Name for o in b["Objects"]): b for b in bodies}
        self.assertIn(("R", "S"), byObjects)
        bundle = byObjects[("R", "S")]
        # Both, together in the assembly's frame, then in the body's
        together = [components["R"], components["S"]]
        mass = sum(c["Mass"] for c in together)
        self.assertAlmostEqual(bundle["Mass"], mass, places=12)
        self.assertEqual(bundle["Origin"], "AssumedDensity")
        frame = bundle["Placement"]
        self.assertTrue(frame.isSame(r.Placement, 1e-9))
        centre = sum((c["CenterOfMass"] * c["Mass"] for c in together), App.Vector()) * (1 / mass)
        self.assertTupleAlmost(bundle["CenterOfMass"], tuple(frame.inverse().multVec(centre)))
        # The trace and the moments about the centre do not depend on the
        # axes: the sum of xx, yy and zz is the same in any frame
        trace = 0.0
        for c in together:
            d = c["CenterOfMass"] - centre
            trace += sum(c["Inertia"][:3]) + 2 * c["Mass"] * d.Length**2
        self.assertAlmostEqual(sum(bundle["Inertia"][:3]), trace, places=9)
        # G, grounded, a body of its own
        self.assertIn(("G",), byObjects)
        self.assertAlmostEqual(byObjects[("G",)]["Mass"], components["G"]["Mass"], places=12)
