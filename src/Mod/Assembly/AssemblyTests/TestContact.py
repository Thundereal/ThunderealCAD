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

    def simulate(self, end, step, analysis="Dynamic", sensors=(), **props):
        """sensors: each a dict of newSensor()'s arguments but the study"""
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = analysis
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        sim.Elements = self.sets
        for name, value in props.items():
            setattr(sim, name, value)
        if sensors:
            import CommandCreateSimulation

            for sensor in sensors:
                CommandCreateSimulation.newSensor(sim, **sensor)
        self.doc.recompute()
        return sim, self.assembly.generateSimulation(sim)

    def test_sensor_stops_on_landing(self):
        """A sensor on a set's contact force rising above none stops the run
        where the ball lands, dropped a millimetre: at sqrt(2 h / g), the
        time located, the ball just touching (CATIA's stop on collision;
        docs/AssemblyDynamics.md sec 5.2's Sensor)."""
        plate = self.plate()
        ball = self.ball(16)
        contact = self.contact([ball], [plate])
        _, result = self.simulate(
            0.05,
            1e-3,
            sensors=[{"source": contact, "quantity": "ContactForce", "threshold": 0.0}],
        )
        self.assertEqual(result, 0)
        results = self.assembly.getSimulationResults()
        events = [e for e in results["Events"] if e["Kind"] == "Sensor"]
        self.assertEqual(len(events), 1, events)
        self.assertTrue(events[0]["Stopped"])
        landing = math.sqrt(2 * 1.0 / 9806.65)
        self.assertAlmostEqual(events[0]["Time"], landing, delta=1e-6)
        self.assertAlmostEqual(results["Times"][-1], events[0]["Time"], delta=1e-12)
        self.assertAlmostEqual(self.heights(ball)[-1], 15.0, delta=1e-5)

    def test_contact_events(self):
        """A ball dropped a millimetre bounces, restitution 0.8: its
        contact events (docs/AssemblyDynamics.md sec 8.4) alternate, made
        and let go. The first is made at sqrt(2 h / g) (1e-6 s), coming in
        at sqrt(2 g h) (1e-5 of it); each lets go at r times the speed it
        came in at (2%), and is made again 2 v / g later (1e-6 s), v the
        speed it left at (1e-4 of it), as it comes back in. Each names the
        set and the pair, the ball first, its side of the set; a sensor's
        event is among them in time order."""
        plate = self.plate()
        ball = self.ball(16)
        contact = self.contact([ball], [plate], StaticFriction=0.0, DynamicFriction=0.0)
        contact.Restitution = 0.8
        self.doc.recompute()
        _, result = self.simulate(
            0.06,
            1e-3,
            sensors=[
                {
                    "source": contact,
                    "quantity": "ContactForce",
                    "threshold": 0.0,
                    "action": "Mark",
                }
            ],
        )
        self.assertEqual(result, 0)
        events = self.assembly.getSimulationResults()["Events"]
        times = [e["Time"] for e in events]
        self.assertEqual(times, sorted(times))
        sensors = [e for e in events if e["Kind"] == "Sensor"]
        self.assertGreaterEqual(len(sensors), 1, events)
        contacts = [e for e in events if e["Kind"] != "Sensor"]
        self.assertGreaterEqual(len(contacts), 5, contacts)
        for i, event in enumerate(contacts):
            self.assertEqual(event["Kind"], "ContactEnd" if i % 2 else "ContactBegin")
            self.assertEqual(event["Object"], contact)
            self.assertEqual(event["Bodies"], (ball, plate))
            self.assertFalse(event["Stopped"])
        v0 = math.sqrt(2 * G * 1.0)
        self.assertAlmostEqual(contacts[0]["Time"], math.sqrt(2 * 1.0 / G), delta=1e-6)
        self.assertAlmostEqual(contacts[0]["Value"], v0, delta=v0 * 1e-5)
        # The sensor's mark with the first contact: the force rises from it
        self.assertAlmostEqual(sensors[0]["Time"], contacts[0]["Time"], delta=1e-6)
        for made, left, again in zip(contacts[0::2], contacts[1::2], contacts[2::2]):
            self.assertAlmostEqual(left["Value"], 0.8 * made["Value"], delta=made["Value"] * 0.02)
            self.assertAlmostEqual(again["Time"] - left["Time"], 2 * left["Value"] / G, delta=1e-6)
            self.assertAlmostEqual(again["Value"], left["Value"], delta=left["Value"] * 1e-4)

    def test_sensors_on_contact_events(self):
        """A sensor on a set's contact events: marking each contact made
        slower than 100 mm/s, the ball bouncing at restitution 0.8 -- the
        third landing, at about 90 mm/s, the first two faster -- with the
        contact's time and speed; stopping on the first let go, the run's
        last frame there (1e-8 s), the ball just leaving, at the speed the
        contact's event has; stopping on the first contact made slower than
        100 mm/s, the third landing, the ball just touching"""
        plate = self.plate()
        ball = self.ball(16)
        contact = self.contact([ball], [plate], StaticFriction=0.0, DynamicFriction=0.0)
        contact.Restitution = 0.8
        self.doc.recompute()

        start = ball.Placement

        def run(**sensor):
            # heights() leaves the ball at the last frame
            ball.Placement = start
            _, result = self.simulate(0.06, 1e-3, sensors=[dict(source=contact, **sensor)])
            self.assertEqual(result, 0)
            results = self.assembly.getSimulationResults()
            events = results["Events"]
            return (
                results,
                [e for e in events if e["Kind"] == "Sensor"],
                [e for e in events if e["Kind"] != "Sensor"],
            )

        # Marking the slow landings
        results, marks, contacts = run(
            quantity="ContactBegin", threshold=100.0, compare="Below", action="Mark"
        )
        made = [e for e in contacts if e["Kind"] == "ContactBegin"]
        self.assertEqual(len(made), 3, contacts)
        self.assertEqual(len(marks), 1, marks)
        self.assertGreater(made[1]["Value"], 100.0)
        self.assertLess(made[2]["Value"], 100.0)
        self.assertEqual(marks[0]["Time"], made[2]["Time"])
        self.assertEqual(marks[0]["Value"], made[2]["Value"])
        self.assertFalse(marks[0]["Stopped"])
        self.assertAlmostEqual(results["Times"][-1], 0.06, delta=1e-12)

        # Stopping on the first let go
        results, stops, contacts = run(quantity="ContactEnd", threshold=0.0)
        self.assertEqual([e["Kind"] for e in contacts], ["ContactBegin", "ContactEnd"])
        self.assertEqual(len(stops), 1, stops)
        self.assertTrue(stops[0]["Stopped"])
        self.assertEqual(stops[0]["Time"], contacts[1]["Time"])
        self.assertEqual(stops[0]["Value"], contacts[1]["Value"])
        self.assertAlmostEqual(results["Times"][-1], contacts[1]["Time"], delta=1e-8)
        v0 = math.sqrt(2 * G * 1.0)
        self.assertAlmostEqual(stops[0]["Value"], 0.8 * v0, delta=0.02 * v0)
        self.assertAlmostEqual(self.heights(ball)[-1], 15.0, delta=1e-5)

        # Stopping on the first slow landing
        results, stops, contacts = run(quantity="ContactBegin", threshold=100.0, compare="Below")
        self.assertEqual(len(stops), 1, stops)
        self.assertEqual(len([e for e in contacts if e["Kind"] == "ContactBegin"]), 3)
        self.assertEqual(stops[0]["Time"], contacts[-1]["Time"])
        self.assertLess(stops[0]["Value"], 100.0)
        self.assertAlmostEqual(results["Times"][-1], contacts[-1]["Time"], delta=1e-8)
        self.assertAlmostEqual(self.heights(ball)[-1], 15.0, delta=1e-5)

    def test_sensor_repeats(self):
        """A sensor that marks with Repeat acts on every Count'th trip: on
        the bouncing ball, every second contact made, the second, the
        fourth and the sixth, at their times; the contact force crossing
        none, every second crossing, each let go; one that stops does not
        look at it, stopping at the second contact made"""
        plate = self.plate()
        ball = self.ball(16)
        contact = self.contact([ball], [plate], StaticFriction=0.0, DynamicFriction=0.0)
        contact.Restitution = 0.8
        self.doc.recompute()
        start = ball.Placement

        def run(**sensor):
            ball.Placement = start
            _, result = self.simulate(0.1, 1e-3, sensors=[dict(source=contact, **sensor)])
            self.assertEqual(result, 0)
            events = self.assembly.getSimulationResults()["Events"]
            return (
                [e for e in events if e["Kind"] == "Sensor"],
                [e for e in events if e["Kind"] != "Sensor"],
            )

        marks, contacts = run(
            quantity="ContactBegin", threshold=0.0, action="Mark", count=2, repeat=True
        )
        made = [e for e in contacts if e["Kind"] == "ContactBegin"]
        self.assertGreaterEqual(len(made), 6, contacts)
        self.assertEqual(len(marks), len(made) // 2, (marks, made))
        for mark, event in zip(marks, made[1::2]):
            self.assertAlmostEqual(mark["Time"], event["Time"], delta=1e-8)
        self.assertFalse(any(m["Stopped"] for m in marks))

        marks, contacts = run(
            quantity="ContactForce",
            threshold=0.0,
            compare="Crosses",
            action="Mark",
            count=2,
            repeat=True,
        )
        let_go = [e for e in contacts if e["Kind"] == "ContactEnd"]
        self.assertGreaterEqual(len(let_go), 5, contacts)
        self.assertEqual(len(marks), len(let_go), (marks, let_go))
        for mark, event in zip(marks, let_go):
            self.assertAlmostEqual(mark["Time"], event["Time"], delta=1e-6)
            self.assertLess(mark["Value"], 1e-9)

        stops, contacts = run(quantity="ContactBegin", threshold=0.0, count=2, repeat=True)
        self.assertEqual(len(stops), 1, stops)
        self.assertTrue(stops[0]["Stopped"])
        made = [e for e in contacts if e["Kind"] == "ContactBegin"]
        self.assertEqual(len(made), 2, contacts)
        self.assertAlmostEqual(stops[0]["Time"], made[1]["Time"], delta=1e-8)

    def test_sensor_counts(self):
        """A sensor acts on the trip its Count says, those before let by:
        stopping on the third contact made, the ball just touching there;
        marking each contact made from the second; the contact force
        crossing none either way, marked at each contact made and let go,
        and stopping at its fourth crossing, the second let go"""
        plate = self.plate()
        ball = self.ball(16)
        contact = self.contact([ball], [plate], StaticFriction=0.0, DynamicFriction=0.0)
        contact.Restitution = 0.8
        self.doc.recompute()
        start = ball.Placement

        def run(**sensor):
            ball.Placement = start
            _, result = self.simulate(0.06, 1e-3, sensors=[dict(source=contact, **sensor)])
            self.assertEqual(result, 0)
            results = self.assembly.getSimulationResults()
            events = results["Events"]
            return (
                results,
                [e for e in events if e["Kind"] == "Sensor"],
                [e for e in events if e["Kind"] != "Sensor"],
            )

        # All the contacts, made and let go, for the times
        _, _, contacts = run(quantity="ContactBegin", threshold=0.0, action="Mark")
        made = [e for e in contacts if e["Kind"] == "ContactBegin"]
        self.assertEqual(len(made), 3, contacts)

        results, stops, seen = run(quantity="ContactBegin", threshold=0.0, count=3)
        self.assertEqual(len(stops), 1, stops)
        self.assertTrue(stops[0]["Stopped"])
        self.assertAlmostEqual(stops[0]["Time"], made[2]["Time"], delta=1e-8)
        self.assertEqual(len([e for e in seen if e["Kind"] == "ContactBegin"]), 3)
        self.assertAlmostEqual(results["Times"][-1], stops[0]["Time"], delta=1e-8)
        self.assertAlmostEqual(self.heights(ball)[-1], 15.0, delta=1e-5)

        _, marks, _ = run(quantity="ContactBegin", threshold=0.0, action="Mark", count=2)
        self.assertEqual([m["Time"] for m in marks], [m["Time"] for m in made[1:]])

        # The force crossing none: up at each landing, down at each let go,
        # the third landing's too, an impact over within one of the watched
        # pieces as they were cut before. Against the contacts of the same
        # run: a sensor's bisection takes steps of its own, and the third
        # landing moves by some tens of microseconds from run to run.
        _, marks, contacts = run(
            quantity="ContactForce", threshold=0.0, compare="Crosses", action="Mark"
        )
        self.assertEqual(len(contacts), 6, contacts)
        self.assertEqual(len(marks), len(contacts), (marks, contacts))
        for mark, event in zip(marks, contacts):
            self.assertAlmostEqual(mark["Time"], event["Time"], delta=1e-6)
            self.assertEqual(mark["Value"] > 0.0, event["Kind"] == "ContactBegin")
        results, stops, contacts = run(
            quantity="ContactForce", threshold=0.0, compare="Crosses", count=4
        )
        self.assertEqual(len(stops), 1, stops)
        self.assertEqual([e["Kind"] for e in contacts], ["ContactBegin", "ContactEnd"] * 2)
        self.assertAlmostEqual(stops[0]["Time"], contacts[3]["Time"], delta=1e-6)
        self.assertAlmostEqual(results["Times"][-1], stops[0]["Time"], delta=1e-8)

    def test_sensor_on_a_pair(self):
        """A sensor on a set's contacts made, given a pair: two balls on the
        plate, in one set, the second dropped from higher and landing after
        the first's first landing -- on that second ball, or on it and the
        plate, it stops where the second lands; on the two balls, which
        never touch, it never trips. The second contact made of any pair is
        the same landing."""
        plate = self.plate()
        first = self.ball(16)
        second = self.ball(18)
        first.Placement.Base.x = -15
        second.Placement.Base.x = 15
        contact = self.contact(
            [first, second], [plate], StaticFriction=0.0, DynamicFriction=0.0
        )
        contact.Restitution = 0.8
        self.doc.recompute()
        starts = first.Placement, second.Placement
        landing = math.sqrt(2 * 3.0 / G)

        def run(**sensor):
            first.Placement, second.Placement = starts
            _, result = self.simulate(0.05, 1e-3, sensors=[dict(source=contact, **sensor)])
            self.assertEqual(result, 0)
            results = self.assembly.getSimulationResults()
            stops = [e for e in results["Events"] if e["Kind"] == "Sensor"]
            made = [e for e in results["Events"] if e["Kind"] == "ContactBegin"]
            return results, stops, made

        for pair in ([second], [second, plate], [plate, second]):
            results, stops, made = run(quantity="ContactBegin", threshold=0.0, pair=pair)
            self.assertEqual(len(stops), 1, (pair, stops))
            self.assertAlmostEqual(stops[0]["Time"], landing, delta=1e-6)
            self.assertEqual(made[-1]["Bodies"], (second, plate))
            self.assertEqual(stops[0]["Time"], made[-1]["Time"])
            self.assertAlmostEqual(results["Times"][-1], landing, delta=1e-6)
        # The first's landing came before
        self.assertEqual(made[0]["Bodies"], (first, plate))

        results, stops, made = run(quantity="ContactBegin", threshold=0.0, pair=[first, second])
        self.assertEqual(stops, [])
        self.assertAlmostEqual(results["Times"][-1], 0.05, delta=1e-12)
        self.assertGreaterEqual(len(made), 3)

        results, stops, made = run(quantity="ContactBegin", threshold=0.0, count=2)
        self.assertAlmostEqual(stops[0]["Time"], landing, delta=1e-6)

    def test_events_name_the_parts_that_touched(self):
        """A rigid group's contact events name the part of it that touched,
        and a part placed on the ground the part: a ball fixed under a
        holder (the group, the holder its first part) dropped a millimetre
        onto a pad fixed to the grounded plate lands as (ball, pad), not as
        the group's first part and the ground; and a sensor on that pair
        stops the run there."""
        plate = self.plate()
        pad = self.assembly.newObject("Part::Box", "Pad")
        pad.Length, pad.Width, pad.Height = 10, 10, 2
        pad.Placement.Base = App.Vector(-5, -5, 10)
        holder = self.assembly.newObject("Part::Box", "Holder")
        holder.Length, holder.Width, holder.Height = 20, 20, 4
        holder.Placement.Base = App.Vector(-10, -10, 25)
        ball = self.ball(18)

        def fixed(a, b, at):
            j = self.joints.newObject("App::FeaturePython", "Fixed")
            JointObject.Joint(j, JointObject.JointTypes.index("Fixed"))
            j.Detach1 = True
            j.Detach2 = True
            j.Reference1 = (a, ["Face1"])
            j.Reference2 = (b, ["Face1"])
            j.Placement1 = App.Placement(at - a.Placement.Base, App.Rotation())
            j.Placement2 = App.Placement(at - b.Placement.Base, App.Rotation())

        fixed(plate, pad, App.Vector(0, 0, 10))
        fixed(holder, ball, App.Vector(0, 0, 25))
        contact = self.contact([holder, ball], [plate, pad], StaticFriction=0.0)
        contact.DynamicFriction = 0.0
        starts = holder.Placement, ball.Placement
        landing = math.sqrt(2 * 1.0 / G)

        def run(*sensors):
            holder.Placement, ball.Placement = starts
            _, result = self.simulate(0.03, 1e-3, sensors=sensors)
            self.assertEqual(result, 0)
            return self.assembly.getSimulationResults()["Events"]

        made = [e for e in run() if e["Kind"] == "ContactBegin"]
        self.assertGreaterEqual(len(made), 1)
        self.assertAlmostEqual(made[0]["Time"], landing, delta=1e-6)
        self.assertEqual(made[0]["Bodies"], (ball, pad))
        # The ball held under its holder through the landing: their joint,
        # though nothing joins them to the ground, is the run's
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        apart = ball.Placement.Base - holder.Placement.Base
        self.assertLess((apart - (starts[1].Base - starts[0].Base)).Length, 1e-6)
        sensor = dict(source=contact, quantity="ContactBegin", threshold=0.0, pair=[ball, pad])
        stops = [e for e in run(sensor) if e["Kind"] == "Sensor"]
        self.assertEqual(len(stops), 1, stops)
        self.assertAlmostEqual(stops[0]["Time"], landing, delta=1e-6)

    def test_no_events_at_rest(self):
        """A box resting on the plate from the start, sunk by the allowed
        penetration, makes no contact event: the contacts pressing at the
        start are made already"""
        plate = self.plate()
        box = self.assembly.newObject("Part::Box", "Box")
        box.Length, box.Width, box.Height = 20, 20, 10
        box.Placement.Base = App.Vector(-10, -10, 10 - 0.005)
        contact = self.contact([box], [plate], StaticFriction=0.0, DynamicFriction=0.0)
        contact.StiffnessFrom = "Penetration"
        contact.AllowedPenetration = 0.005
        self.doc.recompute()
        _, result = self.simulate(0.1, 1e-3)
        self.assertEqual(result, 0)
        self.assertLess(abs(self.heights(box)[-1] - self.heights(box)[0]), 1e-3)
        self.assertEqual(self.assembly.getSimulationResults()["Events"], [])

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

    def force(self, quantity="ContactForce"):
        """The contact set's channel: its force on group A's bodies, by
        default"""
        channels = self.assembly.getSimulationResults()["Channels"]
        found = [c for c in channels if c["Quantity"] == quantity]
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

    def test_contact_channels(self):
        """A set reports its force's normal and friction parts, and its
        deepest point: the ball at rest pressed by its weight, sunk by the
        law's closed form, with no friction; and before it lands, nothing"""
        plate = self.plate()
        ball = self.ball(16)
        self.contact([ball], [plate], **self.SOFT)
        _, result = self.simulate(1.0, 1e-3)
        self.assertEqual(result, 0)
        total = self.force()
        normal = self.force("ContactNormalForce")
        friction = self.force("ContactFrictionForce")
        depth = self.force("ContactPenetration")
        for k in range(len(total)):
            parts = normal[k] + friction[k]
            self.assertLess((parts - total[k]).Length, 1e-9 * max(1.0, total[k].Length))
        self.assertAlmostEqual(normal[-1].z, BALL * G, delta=BALL * G * 1e-5)
        self.assertLess(friction[-1].Length, 1e-9)
        self.assertAlmostEqual(depth[-1], self.sink(BALL), delta=1e-5)
        # Falling the millimetre: sqrt(2 h / g), 14 ms
        self.assertEqual(depth[5], 0.0)
        self.assertEqual(total[5].Length, 0.0)

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
        # The friction channel: mu of the normal force, against the slide
        weight = 1000 * DENSITY * G
        normal = self.force("ContactNormalForce")
        friction = self.force("ContactFrictionForce")
        for k in range(len(normal)):
            t = k * 1e-3
            if 0.005 <= t and 200 - a * t > 20:
                self.assertAlmostEqual(normal[k].z, weight, delta=weight * 1e-3, msg=t)
                self.assertAlmostEqual(
                    friction[k].x, -0.25 * normal[k].z, delta=weight * 1e-3, msg=t
                )
                self.assertLess(abs(friction[k].y) + abs(friction[k].z), weight * 1e-6, msg=t)

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
