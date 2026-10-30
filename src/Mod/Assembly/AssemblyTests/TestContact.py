# SPDX-License-Identifier: LGPL-2.1-or-later
"""Contact (docs/AssemblyDynamics.md sec 7, phase D2): contact sets between
the components' solids, as the primitives they are, their convex hulls or
their closed meshes, by the impact law and friction, against the closed
forms."""

import math
import unittest

import FreeCAD as App
import JointObject

G = 9806.65  # mm/s^2
DENSITY = 2.7e-6  # kg/mm^3, the default aluminium
BALL = 4.0 / 3.0 * math.pi * 125 * DENSITY  # a ball of radius 5, kg
ALUMINUM = "9bf060e9-1663-44a2-88e2-2ff6ee858efe"  # Aluminum Generic's card
# The soft card's Young's modulus (MPa) and Poisson's ratio: the sinks of
# Hertz's law hundredths of a millimetre
SOFT_E, SOFT_NU = 10.0, 0.3


class TestContact(unittest.TestCase):
    def setUp(self):
        import AssemblyApp

        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.skipTest("the Chrono backend is not built")
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("TestContact")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)
        self.sets = []

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)

    def plate(self):
        """A grounded plate 50 x 50 x 10 under the origin's column, its top
        at z = 10"""
        plate = self.assembly.newObject("Part::Box", "Plate")
        plate.Length, plate.Width, plate.Height = 50, 50, 10
        plate.Placement.Base = App.Vector(-25, -25, 0)
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedPlate")
        JointObject.GroundedJoint(ground, plate)
        return plate

    def ball(self, z):
        ball = self.assembly.newObject("Part::Sphere", "Ball")
        ball.Radius = 5
        ball.Placement.Base = App.Vector(0, 0, z)
        return ball

    def contact(self, a, b, **law):
        import CommandCreateContact

        self.doc.recompute()
        contact = CommandCreateContact.newContactSet(self.assembly, a, b)
        # A stiffness or a damping given is the one to run with: not the
        # materials' or the restitution's
        if "Stiffness" in law:
            contact.StiffnessFrom = "Value"
        if "MaxDamping" in law:
            contact.DampingFrom = "Value"
        for name, value in law.items():
            setattr(contact, name, value)
        self.doc.recompute()
        self.sets.append(contact)
        return contact

    def simulate(self, end, step, analysis="Dynamic", **props):
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = analysis
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        sim.Elements = self.sets
        for name, value in props.items():
            setattr(sim, name, value)
        self.doc.recompute()
        return sim, self.assembly.generateSimulation(sim)

    def heights(self, part):
        out = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            out.append(part.Placement.Base.z)
        return out

    def tilt(self, part):
        """How far the part's own z is turned off the vertical at the last
        frame, rad"""
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        up = App.Vector(0, 0, 1)
        return part.Placement.Rotation.multVec(up).getAngle(up)

    def cylinder(self, height, placement):
        cylinder = self.assembly.newObject("Part::Cylinder", "Cylinder")
        cylinder.Radius = 5
        cylinder.Height = height
        cylinder.Placement = placement
        return cylinder

    def force(self):
        """The contact set's ContactForce channel: on group A's bodies"""
        channels = self.assembly.getSimulationResults()["Channels"]
        found = [c for c in channels if c["Quantity"] == "ContactForce"]
        self.assertEqual(len(found), 1, channels)
        return found[0]["Values"]

    # The law for the closed forms: soft enough to sink measurably, a little
    # damping to settle, no friction
    SOFT = dict(Stiffness=1.0, MaxDamping=1e-4, StaticFriction=0.0, DynamicFriction=0.0)

    def sink(self, mass, stiffness=1.0, exponent=1.5):
        """How far a mass sinks into the impact law's spring, mm"""
        return (mass * G * 1e-3 / stiffness) ** (1.0 / exponent)

    def test_ball_rests(self):
        """A ball dropped on the plate comes to rest sunk by (m g / k)^(1/e),
        held by its weight; its contact force is the weight, up"""
        plate = self.plate()
        ball = self.ball(16)
        contact = self.contact([ball], [plate], **self.SOFT)
        self.assertEqual(contact.GroupA, [ball])
        _, result = self.simulate(1.0, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(ball)[-1], 15 - self.sink(BALL), delta=1e-5)
        force = self.force()[-1]
        self.assertAlmostEqual(force.z, BALL * G, delta=BALL * G * 1e-5)
        self.assertLess(abs(force.x) + abs(force.y), 1e-9)

    def test_ball_rests_static(self):
        """A Static study finds the same rest"""
        plate = self.plate()
        ball = self.ball(16)
        contact = self.contact([ball], [plate], **self.SOFT)
        _, result = self.simulate(0.0, 1e-2, "Static", Elements=[contact])
        self.assertEqual(result, 0)
        self.assembly.updateForFrame(0)
        self.assertAlmostEqual(ball.Placement.Base.z, 15 - self.sink(BALL), delta=1e-5)

    def test_elastic_bounce(self):
        """Without damping the ball bounces back to the height it fell
        from, the energy kept to 1e-4"""
        plate = self.plate()
        ball = self.ball(20)
        self.contact([ball], [plate], Stiffness=1.0, MaxDamping=0.0, StaticFriction=0.0)
        _, result = self.simulate(0.12, 1e-4)
        self.assertEqual(result, 0)
        z = self.heights(ball)
        low = z.index(min(z))
        self.assertLess(min(z), 15)
        self.assertAlmostEqual(max(z[low:]), 20, delta=5 * 1e-4)

    def test_sliding_friction(self):
        """A block sent sliding along the plate slows at mu g, mu the
        dynamic friction while it slides faster than the friction velocity"""
        plate = self.plate()
        block = self.assembly.newObject("Part::Box", "Block")
        block.Length = block.Width = block.Height = 10
        block.Placement.Base = App.Vector(-15, -5, 10)
        self.contact([block], [plate])
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        import CommandCreateSimulation

        CommandCreateSimulation.newInitialCondition(
            sim, component=block, linear=App.Vector(200, 0, 0)
        )
        sim.AnalysisType = "Dynamic"
        sim.Elements = self.sets
        sim.TimeEnd = 0.05
        sim.TimeStepOutput = 1e-3
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        a = 0.25 * G
        for k in range(self.assembly.numberOfFrames()):
            t = k * 1e-3
            self.assembly.updateForFrame(k)
            # While it slides fast: v0 - a t above 10.16 mm/s, after the
            # settling of the first steps
            if 0.005 <= t and 200 - a * t > 20:
                x = block.Placement.Base.x
                self.assertAlmostEqual(x, -15 + 200 * t - a * t * t / 2, delta=0.05, msg=t)
        # It stops, not past where the dynamic friction alone stops it by
        # more than the slow end's slip
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        self.assertLess(block.Placement.Base.x, -15 + 200 * 200 / (2 * a) + 0.5)

    def test_hull_rests(self):
        """A hexagonal prism, no primitive but convex, rests on its hull"""
        plate = self.plate()
        prism = self.assembly.newObject("Part::Prism", "Prism")
        prism.Polygon = 6
        prism.Circumradius = 5
        prism.Height = 5
        prism.Placement.Base = App.Vector(0, 0, 11)
        self.contact([prism], [plate], **self.SOFT)
        _, result = self.simulate(1.0, 1e-3)
        self.assertEqual(result, 0)
        mass = 6 * (math.sqrt(3) / 4 * 25) * 5 * DENSITY
        # On its whole base, its six corners one patch: the law's sink, as
        # the ball's one point
        self.assertAlmostEqual(self.heights(prism)[-1], 10 - self.sink(mass), delta=1e-5)

    def test_hull_lands_flat(self):
        """The prism landing flat under the default, stiff law rests level:
        its base is clipped against the plate face to face, all its corners
        at once, not one a query"""
        plate = self.plate()
        prism = self.assembly.newObject("Part::Prism", "Prism")
        prism.Polygon = 6
        prism.Circumradius = 5
        prism.Height = 5
        prism.Placement.Base = App.Vector(0, 0, 11)
        self.contact([prism], [plate])
        _, result = self.simulate(0.2, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(prism)[-1], 10, delta=1e-4)
        self.assertLess(self.tilt(prism), 1e-4)

    def test_hull_on_hull(self):
        """A hexagonal prism landing flat on a grounded, larger one rests
        level on its top face: two hulls, the separating axis by the
        backend's own search"""
        under = self.assembly.newObject("Part::Prism", "Under")
        under.Polygon = 6
        under.Circumradius = 10
        under.Height = 10
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedUnder")
        JointObject.GroundedJoint(ground, under)
        prism = self.assembly.newObject("Part::Prism", "Prism")
        prism.Polygon = 6
        prism.Circumradius = 5
        prism.Height = 5
        prism.Placement.Base = App.Vector(0, 0, 11)
        self.contact([prism], [under])
        _, result = self.simulate(0.2, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(prism)[-1], 10, delta=1e-4)
        self.assertLess(self.tilt(prism), 1e-4)

    def test_curved_convex(self):
        """A convex solid with curved faces is its hull, and comes to rest on
        its lowest point: an ellipsoid on the plate. Its mesh's long thin
        triangles at the poles once had it refused as not convex"""
        plate = self.plate()
        egg = self.assembly.newObject("Part::Ellipsoid", "Egg")
        egg.Radius1, egg.Radius2, egg.Radius3 = 3, 6, 6
        egg.Placement.Base = App.Vector(0, 0, 14)
        self.contact([egg], [plate])
        _, result = self.simulate(0.3, 1e-3)
        self.assertEqual(result, 0)
        # Resting on its pole, within the hull's deflection
        self.assertAlmostEqual(self.heights(egg)[-1], 13, delta=0.01)
        self.assertLess(self.tilt(egg), 1e-2)

    def test_torus_rests(self):
        """A torus, curved and not convex, touches as its mesh: lying flat
        it comes to rest on its tube, its centre the tube's radius above the
        plate, within the mesh's deflection. Its ring of points is one
        patch, a line contact 62.8 mm long. Of the soft material: of
        aluminium it bounces some twenty-five times before it rests, and
        0.1 s takes half a minute"""
        plate = self.plate()
        plate.ShapeMaterial = self.soft()
        torus = self.assembly.newObject("Part::Torus", "Torus")
        torus.Radius1, torus.Radius2 = 10, 2
        torus.Placement.Base = App.Vector(0, 0, 13)
        torus.ShapeMaterial = self.soft()
        self.contact([torus], [plate])
        _, result = self.simulate(0.1, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(torus)[-1], 12, delta=0.01)
        self.assertLess(self.tilt(torus), 1e-3)

    def test_cylinder_on_its_cap(self):
        """A cylinder landing on its cap rests level on it: the cap's rim
        clipped against the plate"""
        plate = self.plate()
        cylinder = self.cylinder(5, App.Placement(App.Vector(0, 0, 11), App.Rotation()))
        self.contact([cylinder], [plate])
        _, result = self.simulate(0.2, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(cylinder)[-1], 10, delta=1e-4)
        self.assertLess(self.tilt(cylinder), 1e-4)

    def test_cylinder_tips_onto_its_cap(self):
        """A tall cylinder landing tilted on its rim tips onto its cap and
        rests there, upright"""
        plate = self.plate()
        cylinder = self.cylinder(
            20, App.Placement(App.Vector(0, 10, 16), App.Rotation(App.Vector(1, 0, 0), 20))
        )
        self.contact([cylinder], [plate])
        _, result = self.simulate(0.3, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(cylinder)[-1], 10, delta=1e-4)
        self.assertLess(self.tilt(cylinder), 1e-4)

    def test_cylinder_on_its_side(self):
        """A cylinder landing on its side rests on the line along it, its
        axis a radius above the plate, and stays where it landed"""
        plate = self.plate()
        cylinder = self.cylinder(
            20, App.Placement(App.Vector(0, 10, 16), App.Rotation(App.Vector(1, 0, 0), 90))
        )
        self.contact([cylinder], [plate])
        _, result = self.simulate(0.3, 1e-3)
        self.assertEqual(result, 0)
        # Its placement is on its axis, at a cap's centre
        self.assertAlmostEqual(self.heights(cylinder)[-1], 15, delta=1e-4)
        self.assertAlmostEqual(self.tilt(cylinder), math.pi / 2, delta=1e-4)
        self.assertLess(abs(cylinder.Placement.Base.x), 0.01)

    def notch(self, z):
        """A cube of 10 with a cube of 6 cut from its top corner: no
        primitive and not convex, its base whole"""
        import Part

        notch = self.assembly.newObject("Part::Feature", "Notch")
        notch.Shape = Part.makeBox(10, 10, 10).cut(Part.makeBox(6, 6, 6, App.Vector(4, 4, 4)))
        notch.Placement.Base = App.Vector(-5, -5, z)
        return notch

    def channel(self):
        """A grounded block 30 x 30 x 10 with a slot 10 wide and 5 deep along
        y through its top: its top at z = 10, the slot's floor at 5, between
        x = -5 and 5"""
        import Part

        channel = self.assembly.newObject("Part::Feature", "Channel")
        channel.Shape = Part.makeBox(30, 30, 10).cut(
            Part.makeBox(10, 30, 5, App.Vector(10, 0, 5))
        )
        channel.Placement.Base = App.Vector(-15, -15, 0)
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedChannel")
        JointObject.GroundedJoint(ground, channel)
        return channel

    def test_mesh_lands_flat(self):
        """A solid neither a primitive nor convex touches as its mesh: the
        notched cube landing on its base rests level on it, its triangles
        clipped against the plate face to face"""
        plate = self.plate()
        notch = self.notch(12)
        self.contact([notch], [plate])
        _, result = self.simulate(0.2, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(notch)[-1], 10, delta=1e-4)
        self.assertLess(self.tilt(notch), 1e-4)

    def test_concave_contact(self):
        """A box dropped into the channel's slot rests on the slot's floor,
        not on the channel's hull 5 mm above it"""
        channel = self.channel()
        box = self.assembly.newObject("Part::Box", "Box")
        box.Length, box.Width, box.Height = 6, 6, 4
        box.Placement.Base = App.Vector(-3, -3, 7)
        self.contact([box], [channel])
        _, result = self.simulate(0.2, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(box)[-1], 5, delta=1e-4)
        self.assertLess(self.tilt(box), 1e-4)

    def test_mesh_on_mesh(self):
        """The notched cube landing on the channel's top, beside the slot,
        rests level there: mesh against mesh, triangle against triangle"""
        channel = self.channel()
        notch = self.notch(12)
        notch.Placement.Base = App.Vector(5, -5, 12)
        self.contact([notch], [channel])
        _, result = self.simulate(0.2, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(notch)[-1], 10, delta=1e-4)
        self.assertLess(self.tilt(notch), 1e-4)

    def test_patch_law(self):
        """The law is a patch's: a cube with a hole through it, its base a
        fan of triangles around the hole, sinks the law's sink for its
        weight as the ball does on its one point; the channel upside down,
        on its two legs, sinks each leg's for half its weight. A soft law,
        so that the sinks are tenths of a millimetre"""
        import Part

        plate = self.plate()
        holed = self.assembly.newObject("Part::Feature", "Holed")
        holed.Shape = Part.makeBox(10, 10, 10).cut(Part.makeCylinder(3, 10, App.Vector(5, 5, 0)))
        holed.Placement.Base = App.Vector(-5, 35, 11)
        bridge = self.assembly.newObject("Part::Feature", "Bridge")
        bridge.Shape = Part.makeBox(30, 30, 10).cut(Part.makeBox(10, 30, 5, App.Vector(10, 0, 0)))
        bridge.Placement.Base = App.Vector(-15, -15, 11)
        plate.Length, plate.Width = 100, 80
        law = dict(self.SOFT, MaxDamping=1e-3)
        self.contact([holed, bridge], [plate], **law)
        _, result = self.simulate(1.0, 1e-3)
        self.assertEqual(result, 0)
        mass = holed.Shape.Volume * DENSITY
        self.assertAlmostEqual(self.heights(holed)[-1], 10 - self.sink(mass), delta=1e-5)
        self.assertLess(self.tilt(holed), 1e-4)
        mass = (9000 - 1500) * DENSITY
        self.assertAlmostEqual(self.heights(bridge)[-1], 10 - self.sink(mass / 2), delta=1e-5)
        self.assertLess(self.tilt(bridge), 1e-4)

    def soft(self):
        """A material card of aluminium's density, as soft as a foam"""
        import Materials

        material = Materials.MaterialManager().getMaterial(ALUMINUM)
        material.Name = "Soft aluminium"
        material.setPhysicalValue("YoungsModulus", "%g MPa" % SOFT_E)
        material.setPhysicalValue("PoissonRatio", str(SOFT_NU))
        return material

    def hertz(self, radius):
        """Hertz's stiffness of two soft parts touching at the radius,
        N/mm^1.5: 4/3 E* R^0.5"""
        modulus = SOFT_E / (1 - SOFT_NU**2) / 2
        return 4.0 / 3.0 * modulus * math.sqrt(radius)

    def johnson(self, length, radius, mass, modulus=None):
        """Johnson's line contact of two soft parts along a length, the
        contact's radius across it (johnsonLine()): the sink under the
        mass's weight, mm, and the power law tangent to it there, its
        stiffness and exponent. Explicit for a load: d = P / (pi E*)
        (2 ln(4R/a) - 1), a = sqrt(4 P R / (pi E*)), P the load a mm"""
        if modulus is None:
            modulus = SOFT_E / (1 - SOFT_NU**2) / 2
        load = mass * G * 1e-3 / length
        half = math.sqrt(4 * load * radius / (math.pi * modulus))
        log = math.log(4 * radius / half)
        sink = load / (math.pi * modulus) * (2 * log - 1)
        exponent = (2 * log - 1) / (2 * log - 2)
        return sink, length * load / sink**exponent, exponent

    def sizeOf(self, part):
        """The radius of the ball of the part's volume"""
        return (3 * part.Shape.Volume / (4 * math.pi)) ** (1.0 / 3.0)

    def test_hertz_ball(self):
        """The stiffness from the materials: Hertz's law. A ball on the plate,
        both soft, sinks by Hertz's closed form, its contact radius its own
        where they touch, or by the parts' sizes, 1/R = 1/RA + 1/RB"""
        plate = self.plate()
        plate.ShapeMaterial = self.soft()
        ball = self.ball(16)
        ball.ShapeMaterial = self.soft()
        contact = self.contact(
            [ball], [plate], MaxDamping=1e-3, StaticFriction=0.0, DynamicFriction=0.0
        )
        self.assertEqual(contact.StiffnessFrom, "Materials")
        self.assertEqual(contact.RadiusFrom, "Patch")
        sim, result = self.simulate(0.3, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(
            self.heights(ball)[-1], 15 - self.sink(BALL, self.hertz(5.0)), delta=1e-6
        )
        contact.RadiusFrom = "Part"
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        radius = 1.0 / (1.0 / self.sizeOf(ball) + 1.0 / self.sizeOf(plate))
        self.assertAlmostEqual(
            self.heights(ball)[-1], 15 - self.sink(BALL, self.hertz(radius)), delta=1e-6
        )

    def test_hertz_flat(self):
        """Two flat faces have no curvature: the prism's base on the plate
        takes the patch's extent, its corners' distance from their centre, for
        its radius -- 5 mm, its corners grown by the envelope 5.02 -- or the
        parts' sizes"""
        plate = self.plate()
        plate.ShapeMaterial = self.soft()
        prism = self.assembly.newObject("Part::Prism", "Prism")
        prism.Polygon = 6
        prism.Circumradius = 5
        prism.Height = 5
        prism.Placement.Base = App.Vector(0, 0, 11)
        prism.ShapeMaterial = self.soft()
        contact = self.contact(
            [prism], [plate], MaxDamping=1e-3, StaticFriction=0.0, DynamicFriction=0.0
        )
        sim, result = self.simulate(0.5, 1e-3)
        self.assertEqual(result, 0)
        mass = prism.Shape.Volume * DENSITY
        self.assertAlmostEqual(
            self.heights(prism)[-1], 10 - self.sink(mass, self.hertz(5.0)), delta=2e-5
        )
        contact.RadiusFrom = "Part"
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        radius = 1.0 / (1.0 / self.sizeOf(prism) + 1.0 / self.sizeOf(plate))
        self.assertAlmostEqual(
            self.heights(prism)[-1], 10 - self.sink(mass, self.hertz(radius)), delta=1e-6
        )

    def test_hertz_line(self):
        """A line contact: the cylinder lying on its side, one curvature
        across it and none along, sinks by Johnson's line contact, L its
        length, R its radius. Fully damped from 1e-4 mm: its sink is a
        hundredth of the default's"""
        plate = self.plate()
        plate.ShapeMaterial = self.soft()
        turned = App.Rotation(App.Vector(0, 1, 0), 90)
        cylinder = self.cylinder(20, App.Placement(App.Vector(-10, 0, 16), turned))
        cylinder.ShapeMaterial = self.soft()
        self.contact(
            [cylinder],
            [plate],
            MaxDamping=0.01,
            Penetration=1e-4,
            StaticFriction=0.0,
            DynamicFriction=0.0,
        )
        _, result = self.simulate(0.3, 1e-3)
        self.assertEqual(result, 0)
        mass = cylinder.Shape.Volume * DENSITY
        sink, _, _ = self.johnson(20.0, 5.0, mass)
        self.assertAlmostEqual(self.heights(cylinder)[-1], 15 - sink, delta=1e-6)

    def test_metal_line(self):
        """Of aluminium, the default, a line contact is stiff: the cylinder on
        its side sinks Johnson's 3.3e-7 mm, resting on it in steps longer
        than its period of 36 us, where a fiftieth of it took 68 s for
        0.3 s"""
        plate = self.plate()
        turned = App.Rotation(App.Vector(0, 1, 0), 90)
        cylinder = self.cylinder(20, App.Placement(App.Vector(-10, 0, 16), turned))
        self.contact([cylinder], [plate], StaticFriction=0.0, DynamicFriction=0.0)
        _, result = self.simulate(0.3, 1e-3)
        self.assertEqual(result, 0)
        mass = cylinder.Shape.Volume * DENSITY
        sink, _, _ = self.johnson(20.0, 5.0, mass, modulus=70000.0 / (1 - 0.35**2) / 2)
        self.assertAlmostEqual(self.heights(cylinder)[-1], 15 - sink, delta=1e-8)

    def test_hertz_curvature(self):
        """A mesh's and a hull's curvature, read off their meshes where they
        touch: the torus lying flat touches along its ring, a line contact
        2 pi 10 mm long across its tube of R 2, and a hemisphere on its
        dome, R 5; each rests as it does on the stiffness Johnson's law, at
        its sink, or Hertz's gives those -- what their meshes' facets do to
        the rest, both runs share"""
        import Part

        plate = self.plate()
        plate.ShapeMaterial = self.soft()
        torus = self.assembly.newObject("Part::Torus", "Torus")
        torus.Radius1, torus.Radius2 = 10, 2
        torus.Placement.Base = App.Vector(0, 0, 13)
        torus.ShapeMaterial = self.soft()
        dome = self.assembly.newObject("Part::Feature", "Dome")
        dome.Shape = Part.makeSphere(5).common(Part.makeBox(20, 20, 10, App.Vector(-10, -10, -10)))
        dome.Placement.Base = App.Vector(18, 0, 16)
        dome.ShapeMaterial = self.soft()
        law = dict(MaxDamping=0.01, Penetration=1e-4, StaticFriction=0.0, DynamicFriction=0.0)
        _, stiffness, exponent = self.johnson(
            2 * math.pi * 10, 2.0, 2 * math.pi**2 * 10 * 2**2 * DENSITY
        )
        sets = {
            torus: (self.contact([torus], [plate], **law), stiffness, exponent),
            dome: (self.contact([dome], [plate], **law), self.hertz(5.0), 1.5),
        }
        start = {part: part.Placement for part in sets}
        sim, result = self.simulate(0.3, 1e-3)
        self.assertEqual(result, 0)
        fromMaterials = {part: self.heights(part)[-1] for part in sets}
        # From where they started: the hemisphere still rocks at 0.3 s
        for part, placement in start.items():
            part.Placement = placement
        for contact, stiffness, exponent in sets.values():
            contact.StiffnessFrom = "Value"
            contact.Stiffness = stiffness
            contact.Exponent = exponent
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        for part in sets:
            self.assertAlmostEqual(fromMaterials[part], self.heights(part)[-1], delta=5e-5)

    def test_restitution(self):
        """Damped by its coefficient of restitution, a ball dropped a
        millimetre onto the plate rebounds to r^2 of it, within 2% at r 0.8
        and 0.3 -- Hunt and Crossley's damping, its factor r's exactly; the
        damping scaling with the stiffness, from the materials here.
        Flores et al.'s factor, the first try, rebounded 0.065 for 0.09"""
        plate = self.plate()
        ball = self.ball(16)
        start = ball.Placement
        contact = self.contact([ball], [plate], StaticFriction=0.0, DynamicFriction=0.0)
        self.assertEqual(contact.DampingFrom, "Restitution")
        sim = None
        for r in (0.8, 0.3):
            ball.Placement = start
            contact.Restitution = r
            self.doc.recompute()
            if sim is None:
                sim, result = self.simulate(0.06, 1e-4)
            else:
                result = self.assembly.generateSimulation(sim)
            self.assertEqual(result, 0)
            z = self.heights(ball)
            landed = next(i for i, h in enumerate(z) if h < 15.0)
            left = next(i for i in range(landed, len(z)) if z[i] > 15.0)
            again = next((i for i in range(left, len(z)) if z[i] < 15.0), len(z))
            self.assertAlmostEqual(max(z[left:again]) - 15.0, r * r, delta=r * r * 0.02)

    def test_allowed_penetration(self):
        """The stiffness by an allowed penetration (Drake's allowance): each
        pair's lighter part sinks it under its weight, whatever its mass --
        the ball of 1.4 g and the box of 10.8 g alike, 5e-3 mm (1e-6)"""
        plate = self.plate()
        ball = self.ball(16)
        box = self.assembly.newObject("Part::Box", "Box")
        box.Length, box.Width, box.Height = 20, 20, 10
        box.Placement.Base = App.Vector(5, 5, 11)
        contact = self.contact([ball, box], [plate], StaticFriction=0.0, DynamicFriction=0.0)
        contact.StiffnessFrom = "Penetration"
        contact.AllowedPenetration = 0.005
        self.doc.recompute()
        _, result = self.simulate(0.3, 1e-3)
        self.assertEqual(result, 0)
        self.assertAlmostEqual(self.heights(ball)[-1], 15 - 0.005, delta=1e-6)
        self.assertAlmostEqual(self.heights(box)[-1], 10 - 0.005, delta=1e-6)
        self.assertLess(self.tilt(box), 1e-4)

    def test_old_set_keeps_its_stiffness(self):
        """A contact set saved before StiffnessFrom and DampingFrom ran with
        its Stiffness and its MaxDamping: restored, it still does"""
        import io
        import re
        import zipfile

        plate = self.plate()
        ball = self.ball(16)
        contact = self.contact([ball], [plate])
        self.assertEqual(contact.StiffnessFrom, "Materials")
        self.assertEqual(contact.DampingFrom, "Restitution")
        data = bytes(contact.dumpContent(Compression=0))
        with zipfile.ZipFile(io.BytesIO(data)) as zipped:
            name = zipped.namelist()[0]
            xml = zipped.read(name).decode("utf-8")
        older, removed = re.subn(
            r'<Property name="(StiffnessFrom|DampingFrom)".*?</Property>\s*', "", xml, flags=re.S
        )
        self.assertEqual(removed, 2)
        count = int(re.search(r'<Properties Count="(\d+)"', older).group(1))
        older = older.replace(
            '<Properties Count="%d"' % count, '<Properties Count="%d"' % (count - 2)
        )
        out = io.BytesIO()
        with zipfile.ZipFile(out, "w") as zipped:
            zipped.writestr(name, older)
        contact.restoreContent(out.getvalue())
        self.assertEqual(contact.StiffnessFrom, "Value")
        self.assertEqual(contact.DampingFrom, "Value")

    def test_mesh_in_pieces_refused(self):
        """A solid whose mesh is in pieces -- a cube with a hollow in it, two
        shells -- is refused, by name"""
        import Part

        plate = self.plate()
        hollow = self.assembly.newObject("Part::Feature", "Hollow")
        hollow.Shape = Part.makeBox(10, 10, 10).cut(Part.makeSphere(2, App.Vector(5, 5, 5)))
        hollow.Placement.Base = App.Vector(-5, -5, 12)
        self.contact([hollow], [plate])
        _, result = self.simulate(0.01, 1e-3)
        self.assertEqual(result, -1)

    def test_outside_the_set(self):
        """A set touches only its own pairs: a ball whose set pairs it with
        another ball, far off, falls through the plate"""
        plate = self.plate()
        ball = self.ball(16)
        other = self.ball(16)
        other.Placement.Base.x = 100
        self.contact([ball, other], [], **self.SOFT)
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        self.assertLess(self.heights(ball)[-1], 0)
