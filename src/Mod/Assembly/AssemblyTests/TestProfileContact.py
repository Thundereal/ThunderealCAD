# SPDX-License-Identifier: LGPL-2.1-or-later
"""Profile contact (docs/AssemblyDynamics.md sec 7.4's 2D path, phase D2):
the boundaries of planar faces of two parts touching in their plane, exactly
as their lines and arcs, by a contact set's law -- against the closed forms,
and a Geneva drive against its kinematics."""

import math
import unittest

import FreeCAD as App
import JointObject

G = 9806.65  # mm/s^2
DENSITY = 2.7e-6  # kg/mm^3, the default aluminium
ALLOWANCE = 0.01  # mm, a new contact's allowed penetration


def plc(x=0.0, y=0.0, z=0.0):
    return App.Placement(App.Vector(x, y, z), App.Rotation())


class TestProfileContact(unittest.TestCase):
    def setUp(self):
        import AssemblyApp

        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.skipTest("the Chrono backend is not built")
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("TestProfileContact")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)

    def face(self, obj, z):
        """The name of obj's planar face at height z"""
        self.doc.recompute()
        for i, f in enumerate(obj.Shape.Faces):
            if f.Surface.TypeId == "Part::GeomPlane" and abs(f.CenterOfMass.z - z) < 1e-6:
                return "Face%d" % (i + 1)
        self.fail("no planar face at z = %g" % z)

    def plate(self, shape=None):
        """A grounded plate 60 x 20 x 10, its top edge in the plane at y = 20,
        or the given shape"""
        if shape is None:
            plate = self.assembly.newObject("Part::Box", "Plate")
            plate.Length, plate.Width, plate.Height = 60, 20, 10
            plate.Placement.Base = App.Vector(-30, 0, 0)
        else:
            plate = self.assembly.newObject("Part::Feature", "Plate")
            plate.Shape = shape
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedPlate")
        JointObject.GroundedJoint(ground, plate)
        return plate

    def disc(self, x, y, radius=5.0):
        disc = self.assembly.newObject("Part::Cylinder", "Disc")
        disc.Radius = radius
        disc.Height = 10
        disc.Placement.Base = App.Vector(x, y, 0)
        return disc

    def contact(self, a, za, b, zb, **law):
        import CommandCreateContact

        contact = CommandCreateContact.newProfileContact(
            self.assembly, a, [self.face(a, za)], b, [self.face(b, zb)]
        )
        if "Stiffness" in law:
            contact.StiffnessFrom = "Value"
        for name, value in law.items():
            setattr(contact, name, value)
        self.doc.recompute()
        return contact

    def simulate(self, contact, end, step, analysis="Dynamic", gravity=(0, -G, 0), motion=None):
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = analysis
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        sim.Elements = [contact]
        if gravity is None:
            sim.GravityEnabled = False
        else:
            sim.Gravity = App.Vector(*gravity)
        if motion is not None:
            joint, formula = motion
            m = sim.newObject("Assembly::Motion", "Motion")
            m.Joint = joint
            m.MotionType = "Angular"
            m.Formula = formula
        self.doc.recompute()
        return self.assembly.generateSimulation(sim)

    def channel(self, quantity):
        channels = self.assembly.getSimulationResults()["Channels"]
        found = [c for c in channels if c["Quantity"] == quantity]
        self.assertEqual(len(found), 1, channels)
        return found[0]["Values"]

    def at(self, part, frame=-1):
        if frame < 0:
            frame += self.assembly.numberOfFrames()
        self.assembly.updateForFrame(frame)
        return part.Placement

    def test_command(self):
        """The contact's faces as picked, its law an allowance with no
        friction, ticked in the Dynamic study"""
        plate = self.plate()
        disc = self.disc(0, 26)
        contact = self.contact(disc, 10, plate, 10)
        self.assertEqual(contact.ProfileA[0], disc)
        self.assertEqual(contact.ProfileB[0], plate)
        self.assertEqual(contact.StiffnessFrom, "Penetration")
        self.assertEqual(contact.StaticFriction, 0.0)
        self.assertTrue(contact.isDerivedFrom("Assembly::ContactSet"))

    def test_disc_rests(self):
        """A disc dropped a millimetre onto the plate's edge, gravity in the
        plane, rests sunk by the allowance, its weight on the plate: an arc
        on a line"""
        plate = self.plate()
        disc = self.disc(0, 26)
        contact = self.contact(disc, 10, plate, 10, Restitution=0.3)
        self.assertEqual(self.simulate(contact, 0.5, 1e-3), 0)
        mass = math.pi * 25 * 10 * DENSITY
        self.assertAlmostEqual(self.at(disc).Base.y, 25 - ALLOWANCE, delta=1e-6)
        self.assertAlmostEqual(self.at(disc).Base.x, 0, delta=1e-9)
        force = self.channel("ContactForce")[-1]
        self.assertAlmostEqual(force.y, mass * G, delta=mass * G * 1e-5)
        self.assertLess(abs(force.x), 1e-6)
        self.assertAlmostEqual(self.channel("ContactPenetration")[-1], ALLOWANCE, delta=1e-6)

    def test_block_rests_flat(self):
        """A block dropped flat onto the plate's edge rests on both its
        corners, one patch, sunk by the allowance and level; the plate's
        profile its bottom face, facing the block's the other way"""
        plate = self.plate()
        block = self.assembly.newObject("Part::Box", "Block")
        block.Length = block.Width = block.Height = 10
        block.Placement.Base = App.Vector(-5, 21, 0)
        contact = self.contact(block, 10, plate, 0, Restitution=0.3)
        self.assertEqual(self.simulate(contact, 0.5, 1e-3), 0)
        placement = self.at(block)
        self.assertAlmostEqual(placement.Base.y, 20 - ALLOWANCE, delta=1e-6)
        self.assertLess(abs(placement.Rotation.Angle), 1e-6)
        self.assertAlmostEqual(self.channel("ContactPenetration")[-1], ALLOWANCE, delta=1e-6)

    def test_disc_in_notch(self):
        """A disc dropped into a round notch in the plate's edge, wider than
        it, settles at its bottom: an arc in a hollow arc. Down the middle:
        off it, with no friction, it would slide to and fro in the notch with
        nothing to stop it."""
        import Part

        box = Part.makeBox(60, 20, 10, App.Vector(-30, 0, 0))
        notch = Part.makeCylinder(6, 10, App.Vector(0, 20, 0))
        plate = self.plate(box.cut(notch).removeSplitter())
        disc = self.disc(0.0, 20.5)
        contact = self.contact(disc, 10, plate, 10, Restitution=0.3)
        self.assertEqual(self.simulate(contact, 1.0, 1e-3), 0)
        placement = self.at(disc)
        # The notch's bottom at y = 14
        self.assertAlmostEqual(placement.Base.x, 0.0, delta=1e-4)
        self.assertAlmostEqual(placement.Base.y, 19 - ALLOWANCE, delta=1e-4)

    def test_geneva(self):
        """A four-slot Geneva drive: the driver's pin, turning once a second,
        carries the wheel a quarter turn the other way while it is in a slot
        -- the pin's centre on the slot's axis, within the slot's clearance
        and the law's sink -- and the driver's locking disc holds it there
        after, in the wheel's hollow between two slots"""
        g = geneva(self.assembly, self.joints)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        import CommandCreateContact

        contact = CommandCreateContact.newProfileContact(
            self.assembly, g.driver, self.faces(g.driver, 10), g.wheel, self.faces(g.wheel, 10)
        )
        self.assertEqual(len(contact.ProfileA[1]), 2)
        contact.StiffnessFrom = "Value"
        contact.Stiffness = 1e4
        rate = 2 * math.pi
        self.assertEqual(
            self.simulate(contact, 0.5, 1e-3, gravity=None, motion=(g.crank, "%g*time" % rate)),
            0,
        )
        engaged = 0
        for k in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(k)
            # The pin's centre, and the axis of the wheel's slot that started
            # at 45 deg
            p = g.driver.Placement.multVec(App.Vector(g.a, g.r, 0))
            turn = g.wheel.Placement.Rotation.Angle * g.wheel.Placement.Rotation.Axis.z
            axis = math.radians(45) + turn
            radial = p.x * math.cos(axis) + p.y * math.sin(axis)
            across = -p.x * math.sin(axis) + p.y * math.cos(axis)
            if radial < g.outer - g.pin:
                engaged += 1
                self.assertLess(abs(across), g.half - g.pin + 2 * ALLOWANCE, msg=k)
        self.assertGreater(engaged, 100)
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        rotation = g.wheel.Placement.Rotation
        self.assertAlmostEqual(rotation.Axis.z, -1.0, delta=1e-6)
        self.assertAlmostEqual(math.degrees(rotation.Angle), 90.0, delta=0.2)

    def faces(self, obj, z):
        """The names of obj's planar faces at height z"""
        self.doc.recompute()
        return [
            "Face%d" % (i + 1)
            for i, f in enumerate(obj.Shape.Faces)
            if f.Surface.TypeId == "Part::GeomPlane" and abs(f.CenterOfMass.z - z) < 1e-6
        ]


class Geneva:
    pass


def geneva(assembly, joints):
    """A four-slot Geneva drive, 10 mm thick, on a grounded base: the wheel
    about the origin, its slots at 45 deg and every quarter turn from it,
    hollows between them; the driver about (a, 0), its pin at (a, r) and its
    locking disc cut away behind the pin, so that the wheel's corners pass
    while the pin turns it. The wheel's and the driver's revolute joints; the
    driver's is 'crank'."""
    import Part

    g = Geneva()
    g.a = a = 40.0  # the centres apart
    g.r = r = a * math.sin(math.pi / 4)  # the crank
    g.pin = pin = 3.0
    g.half = half = 3.05  # the slot's half width
    g.outer = outer = math.hypot(r, pin)
    clearance = 0.05
    wheelShape = Part.makeCylinder(outer, 10)
    for k in range(4):
        slot = Part.makeBox(outer, 2 * half, 10, App.Vector(10, -half, 0))
        slot = slot.fuse(Part.makeCylinder(half, 10, App.Vector(10, 0, 0)))
        slot.rotate(App.Vector(), App.Vector(0, 0, 1), 45 + 90 * k)
        wheelShape = wheelShape.cut(slot)
    # The hollows: about where the driver's centre is to the wheel at rest,
    # through the corners of the slots either side
    corner = App.Rotation(App.Vector(0, 0, 1), 45).multVec(
        App.Vector(math.sqrt(outer**2 - half**2), -half, 0)
    )
    lock = (corner - App.Vector(a, 0, 0)).Length
    for k in range(4):
        hollow = Part.makeCylinder(lock, 10, App.Vector(a, 0, 0))
        hollow.rotate(App.Vector(), App.Vector(0, 0, 1), 90 * k)
        wheelShape = wheelShape.cut(hollow)
    disc = Part.makeCylinder(lock - clearance, 10, App.Vector(a, 0, 0))
    disc = disc.cut(Part.makeCylinder(outer + 2 * clearance, 10, App.Vector(a, a, 0)))
    driverShape = disc.fuse(Part.makeCylinder(pin, 10, App.Vector(a, r, 0)))
    base = assembly.newObject("Part::Box", "Base")
    base.Length, base.Width, base.Height = 120, 80, 5
    base.Placement.Base = App.Vector(-40, -40, -5)
    ground = joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
    JointObject.GroundedJoint(ground, base)
    g.wheel = assembly.newObject("Part::Feature", "Wheel")
    g.wheel.Shape = wheelShape.removeSplitter()
    g.driver = assembly.newObject("Part::Feature", "Driver")
    g.driver.Shape = driverShape.removeSplitter()
    assembly.Document.recompute()

    def revolute(name, part, x):
        j = joints.newObject("App::FeaturePython", name)
        JointObject.Joint(j, JointObject.JointTypes.index("Revolute"))
        j.Detach1 = True
        j.Detach2 = True
        j.Reference1 = (base, ["Face6"])
        j.Reference2 = (part, ["Face1"])
        j.Placement1 = plc(x + 40, 40, 5)
        j.Placement2 = plc(x, 0, 0)
        return j

    g.axle = revolute("WheelAxle", g.wheel, 0.0)
    g.crank = revolute("Crank", g.driver, a)
    return g
