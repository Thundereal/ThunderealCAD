# SPDX-License-Identifier: LGPL-2.1-or-later
"""The Cam joint (docs/AssemblySolverInterface.md sec 6.18).

A Cam joint holds a point of the follower on a curve of the cam: the solver's
PointOnCurve, two equations. Here: the joint type and the picks it accepts;
a disk cam -- an eccentric circle on a revolute driven by an Angular motion,
whose follower's lift is known in closed form -- with a point follower and
with a roller, each on a slider pressed onto the cam by the joint, simulated
for more than six turns with an output step of 2 rad (more than a quarter
turn), its lift checked at every frame on every backend; the same with a
dwell-rise-dwell cam profile built as a B-spline, its lift checked against
the B-spline's own evaluation; and which children of a Simulation are read
as motions (by their proxy, not by their property names).

CAM_LIFT_ERROR collects the largest lift error per case and backend.
"""

import math

import FreeCAD as App
import Part
import unittest

import JointObject

from AssemblyTests.TestJoints import JointTestBase, plc, X, Z

# {(case, backend): largest lift error in mm over the frames}
CAM_LIFT_ERROR = {}

CAM_E = 6.0  # the disk's centre from the cam's axis
CAM_R = 20.0  # the disk's radius
CAM_H = 10.0  # the disk's thickness: its top face, where the followers run, at z = 10
ROLLER = 3.0
RATE = 10.0  # the cam's motion: RATE * time rad
T_END = 4.0  # 40 rad: more than six turns
T_STEP = 0.2  # 2 rad per output step: more than a quarter turn
LIFT_TOL = 1e-9


def disk_lift(theta, roller=0.0):
    """The follower's height on the disk cam turned by theta, on the slider x = 0.

    The disk's centre is at CAM_E (cos theta, sin theta); the follower's point
    (0, y) is at CAM_R + roller from it, above the axis.
    """
    c = CAM_E * math.cos(theta)
    return CAM_E * math.sin(theta) + math.sqrt((CAM_R + roller) ** 2 - c * c)


def turn_about_z(placement):
    x, y, z, w = placement.Rotation.Q
    return 2.0 * math.atan2(z, w)


def circle_edge(obj, z):
    """The name of the circular edge of obj's shape at height z."""
    for i, e in enumerate(obj.Shape.Edges):
        if e.Curve.TypeId == "Part::GeomCircle" and abs(e.Curve.Center.z - z) < 1e-9:
            return "Edge%d" % (i + 1)
    raise AssertionError("no circle at z = %g in %s" % (z, obj.Name))


# A dwell-rise-dwell-return profile, in polar form about the cam's axis: the
# base radius, a cycloidal rise of RISE over the second sixth of the turn, a
# dwell at the top, the return over the fifth sixth.
BASE = 18.0
RISE = 6.0


def profile_radius(phi):
    phi = phi % (2 * math.pi)
    s = 2 * math.pi / 6

    def cycloid(x):
        return x - math.sin(2 * math.pi * x) / (2 * math.pi)

    if phi < s:
        return BASE
    if phi < 2 * s:
        return BASE + RISE * cycloid((phi - s) / s)
    if phi < 4 * s:
        return BASE + RISE
    if phi < 5 * s:
        return BASE + RISE * (1.0 - cycloid((phi - 4 * s) / s))
    return BASE


def profile_curve():
    """The profile as a periodic B-spline through 72 points of it, at z = CAM_H."""
    pts = []
    for i in range(72):
        phi = 2 * math.pi * i / 72
        r = profile_radius(phi)
        pts.append(App.Vector(r * math.cos(phi), r * math.sin(phi), CAM_H))
    curve = Part.BSplineCurve()
    curve.interpolate(pts, PeriodicFlag=True)
    return curve


def profile_lift(curve, theta):
    """Where the B-spline meets the follower's line, the cam turned by theta.

    In the cam's coordinates the follower's line is the ray from the axis in
    direction d = (sin theta, cos theta) (global y turned back by theta); the
    point of the curve on it is
    found by Newton on (C(u) x d).z, from the sampled point nearest the ray,
    with the curve's own value and derivative. The lift is that point's
    distance from the axis.
    """
    dx, dy = math.sin(theta), math.cos(theta)
    a, b = curve.FirstParameter, curve.LastParameter
    best = None
    for i in range(720):
        u = a + (b - a) * i / 720
        p = curve.value(u)
        along = p.x * dx + p.y * dy
        if along <= 0:
            continue
        off = abs(p.x * dy - p.y * dx)
        if best is None or off < best[0]:
            best = (off, u)
    u = best[1]
    for _ in range(50):
        p, d1 = curve.getD1(u)
        f = p.x * dy - p.y * dx
        df = d1.x * dy - d1.y * dx
        step = f / df
        u -= step
        if abs(step) < 1e-15 * (b - a):
            break
    p = curve.value(u)
    return math.hypot(p.x, p.y)


class CamMechanism:
    """Builds the cam mechanisms (a JointTestBase mixin; TestSolverGateCam uses it too)."""

    def make_simulation(self, motions, t_end=T_END, t_step=T_STEP):
        """A simulation as CommandCreateSimulation makes one, without its GUI module."""
        import UtilsAssembly

        sim_group = UtilsAssembly.getSimulationGroup(self.assembly)
        sim = sim_group.newObject("App::FeaturePython", "Simulation")
        sim.addExtension("App::GroupExtensionPython")
        for prop, ptype in (
            ("aTimeStart", "App::PropertyTime"),
            ("bTimeEnd", "App::PropertyTime"),
            ("cTimeStepOutput", "App::PropertyTime"),
            ("fGlobalErrorTolerance", "App::PropertyFloat"),
        ):
            sim.addProperty(ptype, prop, "Simulation")
        sim.aTimeStart = 0.0
        sim.bTimeEnd = t_end
        sim.cTimeStepOutput = t_step
        sim.fGlobalErrorTolerance = 1.0e-6
        objs = []
        for joint, mtype, formula in motions:
            m = self.assembly.newObject("App::FeaturePython", "Motion")
            m.addProperty("App::PropertyXLinkSub", "Joint", "Motion")
            m.addProperty("App::PropertyString", "Formula", "Motion")
            m.addProperty("App::PropertyEnumeration", "MotionType", "Motion")
            m.MotionType = ["Angular", "Linear"]
            m.MotionType = mtype
            m.Joint = joint
            m.Formula = formula
            objs.append(m)
        sim.Group = objs
        return sim

    def cam_ground(self):
        g = self.box("G", plc(-60, -60, -30), size=10.0)
        self.ground(g)
        return g

    def disk_cam(self, g):
        """The eccentric disk on a revolute about the global z axis; returns (cam, revolute)."""
        cam = self.cylinder("Cam", CAM_R, CAM_H, plc(CAM_E, 0, 0))
        rev = self.joint("Revolute", (g, ["Face6"]), plc(60, 60, 30), (cam, ["Face3"]), plc(-CAM_E, 0, 0))
        return cam, rev

    def profile_cam(self, g):
        """The B-spline profile (an edge) on a revolute about the global z axis."""
        curve = profile_curve()
        cam = self.assembly.newObject("Part::Feature", "Profile")
        cam.Shape = Part.Edge(curve)
        rev = self.joint(
            "Revolute", (g, ["Face6"]), plc(60, 60, 30 + CAM_H), (cam, ["Edge1"]), plc(0, 0, CAM_H)
        )
        return cam, rev, curve

    def follower(self, g, y, roller=0.0):
        """A follower on a slider along y through the axis, at height CAM_H.

        A point follower is a box whose Vertex1 runs on the cam; a roller a
        cylinder whose bottom circle (radius roller) does. Returns the part and
        the element the cam joint takes.
        """
        if roller > 0.0:
            f = self.cylinder("Roller", roller, 4.0, plc(0, y, CAM_H))
            element = circle_edge(f, CAM_H)
        else:
            f = self.box("Follower", plc(0, y, CAM_H), size=4.0)
            element = "Vertex1"
        self.joint("Slider", (g, ["Face6"]), plc(60, 60, 30 + CAM_H, X, -90), (f, ["Face1"]), plc(0, 0, 0, X, -90))
        return f, element

    def cam_joint(self, cam, cam_edge, follower, element):
        return self.joint("Cam", (cam, [cam_edge]), plc(), (follower, [element]), plc())

    def disk_mechanism(self, roller=0.0, start_error=0.0):
        g = self.cam_ground()
        cam, rev = self.disk_cam(g)
        f, element = self.follower(g, disk_lift(0.0, roller) + start_error, roller)
        self.cam_joint(cam, circle_edge(cam, CAM_H), f, element)
        self.doc.recompute()
        return cam, rev, f

    def profile_mechanism(self, start_error=0.0):
        g = self.cam_ground()
        cam, rev, curve = self.profile_cam(g)
        f, element = self.follower(g, profile_lift(curve, 0.0) + start_error)
        self.cam_joint(cam, "Edge1", f, element)
        self.doc.recompute()
        return cam, rev, f, curve


class TestCamJoint(CamMechanism, JointTestBase):
    """The Cam joint type, its picks, and cam mechanisms on every backend."""

    def backends(self):
        import AssemblyApp

        return AssemblyApp.getAvailableSolvers()

    def test_type_appended(self):
        """Cam is the last joint type: the older types keep their enumeration values."""
        self.assertEqual(JointObject.JointTypes[-1], "Cam")
        self.assertEqual(JointObject.JointTypes.index("Cam"), 13)
        self.assertEqual(JointObject.JointTypes.index("Belt"), 12)
        self.assertEqual(len(JointObject.TranslatedJointTypes), len(JointObject.JointTypes))

    def test_older_joint_takes_the_cam_type(self):
        """A joint from a file of before the Cam lists twelve types; it can still become a cam."""
        j = self.jointgroup.newObject("App::FeaturePython", "Joint")
        JointObject.Joint(j, JointObject.JointTypes.index("Revolute"))
        j.JointType = JointObject.JointTypes[:-1]
        j.JointType = "Revolute"
        self.assertNotIn("Cam", j.getEnumerationsOfProperty("JointType"))
        j.Proxy.setJointType(j, "Cam")
        self.assertEqual(j.JointType, "Cam")
        self.assertEqual(j.getEnumerationsOfProperty("JointType"), JointObject.JointTypes)
        j.Proxy.setJointType(j, "Belt")
        self.assertEqual(j.JointType, "Belt")

    def test_picks(self):
        """The cam is an edge; the follower a vertex or a circle; a roller needs a planar curve."""
        import AssemblyApp
        import JointCam

        g = self.cam_ground()
        cam = self.cylinder("Cam", CAM_R, CAM_H, plc(CAM_E, 0, 0))
        f = self.box("Follower", plc(0, 30, CAM_H), size=4.0)
        roller = self.cylinder("Roller", ROLLER, 4.0, plc(0, 30, CAM_H))
        helix = self.assembly.newObject("Part::Feature", "Helix")
        helix.Shape = Part.makeHelix(4.0, 12.0, 15.0)
        self.doc.recompute()
        top = circle_edge(cam, CAM_H)
        rim = circle_edge(roller, CAM_H)
        seam = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(roller.Shape.Edges)
            if e.Curve.TypeId == "Part::GeomLine"
        ][0]
        j = self.joint("Cam", (cam, [top]), plc(), (f, ["Vertex1"]), plc())

        def refusal(ref1, ref2):
            j.Reference1 = ref1
            j.Reference2 = ref2
            return AssemblyApp.getCamRefusal(j)

        self.assertEqual(refusal((cam, [top]), (f, ["Vertex1"])), "")
        self.assertEqual(refusal((cam, [top]), (roller, [rim])), "")
        self.assertEqual(refusal((helix, ["Edge1"]), (f, ["Vertex1"])), "")
        why = refusal((cam, ["Face1"]), (f, ["Vertex1"]))
        self.assertIn("the face Face1 of Cam is not an edge", why)
        why = refusal((cam, [top]), (f, ["Face1"]))
        self.assertIn("neither a vertex nor a circular edge", why)
        why = refusal((cam, [top]), (roller, [seam]))
        self.assertIn("the edge %s of Roller" % seam, why)
        why = refusal((helix, ["Edge1"]), (roller, [rim]))
        self.assertIn("does not lie in a plane", why)
        # The task panel asks after each pick: one pick is checked alone.
        self.assertEqual(JointCam.pickRefusal(j, [[cam, [top, top]]]), "")
        self.assertIn("not an edge", JointCam.pickRefusal(j, [[cam, ["Face1", "Face1"]]]))
        self.assertIn("A cam between these elements", JointCam.refusalMessage("x"))
        # A refused joint already in a document is not solved; the others are.
        j.Reference1 = (cam, ["Face1"])
        j.Reference2 = (f, ["Vertex1"])
        self.doc.recompute()
        self.assembly.solve()
        self.assertNotIn(j.Name, self.assembly.getSolverResiduals())

    def test_static_solve_puts_the_follower_on_the_cam(self):
        """From 0.8 mm off, a solve puts the follower on the cam, on every backend.

        The cam may turn as well: the lift is checked against the cam's angle
        after the solve. A planar cam with its follower held in the plane by
        the slider has its second equation (out of the plane) dependent on the
        slider's: the cam joint is partially redundant, and the mechanism keeps one
        freedom (the cam's turn).
        """
        for backend in self.backends():
            for roller in (0.0, ROLLER):
                with self.subTest(backend=backend, roller=roller):
                    self.tearDown()
                    self.setUp()
                    self.assembly.SolverBackend = backend
                    cam, rev, f = self.disk_mechanism(roller, start_error=0.8)
                    status = self.solve()
                    theta = turn_about_z(cam.Placement)
                    self.assertAlmostEqual(f.Placement.Base.x, 0.0, delta=LIFT_TOL)
                    self.assertAlmostEqual(f.Placement.Base.z, CAM_H, delta=LIFT_TOL)
                    self.assertAlmostEqual(f.Placement.Base.y, disk_lift(theta, roller), delta=LIFT_TOL)
                    residuals = self.assembly.getSolverResiduals()
                    self.assertLess(max(residuals.values()), LIFT_TOL, residuals)
                    self.assertEqual(status["DoF"], 1)

    def check_frames(self, case, backend, cam, f, expected_lift):
        n = self.assembly.numberOfFrames()
        # OndselSolver reports the start twice (sec 6.9).
        self.assertGreaterEqual(n, round(T_END / T_STEP) + 1)
        worst = 0.0
        # The frames are the output times from the end back, one per step.
        for k in range(round(T_END / T_STEP) + 1):
            self.assembly.updateForFrame(n - 1 - k)
            t = T_END - T_STEP * k
            theta = RATE * t
            turn = math.remainder(turn_about_z(cam.Placement) - theta, 2 * math.pi)
            self.assertAlmostEqual(turn, 0.0, delta=LIFT_TOL, msg=(backend, t))
            err = abs(f.Placement.Base.y - expected_lift(theta))
            worst = max(worst, err)
            self.assertLess(err, LIFT_TOL, (backend, t, f.Placement.Base.y, expected_lift(theta)))
            self.assertAlmostEqual(f.Placement.Base.x, 0.0, delta=LIFT_TOL)
        CAM_LIFT_ERROR[(case, backend)] = worst
        App.Console.PrintMessage(
            "Cam: %s with %s: lift within %.3g mm at every frame\n" % (case, backend, worst)
        )

    def test_disk_cam_simulation(self):
        """A point follower and a roller on the disk cam, for more than six turns, on every backend."""
        for backend in self.backends():
            for roller in (0.0, ROLLER):
                with self.subTest(backend=backend, roller=roller):
                    self.tearDown()
                    self.setUp()
                    self.assembly.SolverBackend = backend
                    cam, rev, f = self.disk_mechanism(roller)
                    sim = self.make_simulation([(rev, "Angular", "%g*time" % RATE)])
                    self.doc.recompute()
                    self.assertEqual(self.assembly.generateSimulation(sim), 0)
                    case = "disk cam, " + ("roller" if roller else "point")
                    self.check_frames(case, backend, cam, f, lambda th: disk_lift(th, roller))

    def test_bspline_cam_simulation(self):
        """A point follower on a dwell-rise-dwell B-spline profile: the lift is the B-spline's."""
        for backend in self.backends():
            with self.subTest(backend=backend):
                self.tearDown()
                self.setUp()
                self.assembly.SolverBackend = backend
                cam, rev, f, curve = self.profile_mechanism()
                sim = self.make_simulation([(rev, "Angular", "%g*time" % RATE)])
                self.doc.recompute()
                self.assertEqual(self.assembly.generateSimulation(sim), 0)
                # The B-spline the cam's edge was made of, in the cam's frame.
                self.check_frames(
                    "B-spline cam", backend, cam, f, lambda th: profile_lift(curve, th)
                )
                # The profile rises and dwells: the lift spans the rise.
                lifts = [profile_lift(curve, RATE * T_STEP * k) for k in range(21)]
                self.assertGreater(max(lifts) - min(lifts), RISE - 0.1)

    def test_disk_cam_dynamics(self):
        """A Dynamic study of the disk cam driven at RATE, without gravity: the
        follower keeps the lift, and the cam pushes it along the slider by its
        mass times its acceleration there -- the cam joint's reaction on the
        cam, its first reference, the opposite -- at the start too, moving
        already there."""
        if "Chrono" not in self.backends():
            self.skipTest("the Chrono backend is not built")
        step = 0.01
        for roller in (0.0, ROLLER):
            with self.subTest(roller=roller):
                self.tearDown()
                self.setUp()
                cam, rev, f = self.disk_mechanism(roller)
                sims = self.assembly.getGroup("Simulations", True)
                sim = sims.newObject("Assembly::Simulation", "Simulation")
                sim.AnalysisType = "Dynamic"
                sim.GravityEnabled = False
                sim.TimeEnd = 1.0
                sim.TimeStepOutput = step
                motion = sim.newObject("Assembly::Motion", "Motion")
                motion.Joint = rev
                motion.MotionType = "Angular"
                motion.Formula = "%g*time" % RATE
                self.doc.recompute()
                self.assertEqual(self.assembly.generateSimulation(sim), 0)
                results = self.assembly.getSimulationResults()
                forces = [
                    c["Values"]
                    for c in results["Channels"]
                    if c["Name"].startswith("Cam") and c["Quantity"] == "Force"
                ][0]
                mass = f.Shape.Volume * 2.7e-6  # kg, the default aluminium
                e = 1e-4

                def accel(theta):
                    lift = lambda th: disk_lift(th, roller)
                    return (lift(theta + e) - 2 * lift(theta) + lift(theta - e)) / e**2 * RATE**2

                biggest = max(abs(mass * accel(RATE * step * k)) for k in range(101))
                for k in range(self.assembly.numberOfFrames()):
                    self.assembly.updateForFrame(k)
                    theta = RATE * step * k
                    self.assertAlmostEqual(
                        f.Placement.Base.y, disk_lift(theta, roller), delta=1e-6, msg=k
                    )
                    self.assertAlmostEqual(
                        forces[k].y, -mass * accel(theta), delta=biggest * 2e-3, msg=k
                    )


    def test_cam_lifts_off(self):
        """A cam contact lets the follower leave the cam (docs/AssemblyDynamics.md
        sec 7.4). The disk cam turning at w lifts the follower by about
        CAM_E sin(w t); gravity, along -y down the slider, keeps it on the cam
        while the cam falls away slower than g, CAM_E w^2 < g -- below 40.4
        rad/s. At 10 rad/s it rides the lift, sunk by the law's allowance
        (0.01 mm) and a little more, from the start: it starts moving with the
        cam, as the joint would have it, not hit by it (0.03 mm deep); at 80
        rad/s it is thrown off, millimetres above the cam, and lands on it
        again. Frames of a 400th of a turn, which see the start."""
        if "Chrono" not in self.backends():
            self.skipTest("the Chrono backend is not built")
        import CommandCreateContact

        for rate, thrown in ((10.0, False), (80.0, True)):
            for roller in (0.0, ROLLER):
                with self.subTest(rate=rate, roller=roller):
                    self.tearDown()
                    self.setUp()
                    cam, rev, f = self.disk_mechanism(roller)
                    joint = [
                        o
                        for o in self.assembly.getGroup("Joints").Group
                        if getattr(o, "JointType", None) == "Cam"
                    ][0]
                    lifted = CommandCreateContact.newCamContact(self.assembly, joint)
                    self.assertEqual(lifted.StiffnessFrom, "Penetration")
                    sims = self.assembly.getGroup("Simulations", True)
                    sim = sims.newObject("Assembly::Simulation", "Simulation")
                    sim.AnalysisType = "Dynamic"
                    sim.Gravity = App.Vector(0, -9806.65, 0)
                    turn = 2 * math.pi / rate
                    sim.TimeEnd = 2 * turn
                    step = turn / 400
                    sim.TimeStepOutput = step
                    sim.Elements = [lifted]
                    motion = sim.newObject("Assembly::Motion", "Motion")
                    motion.Joint = rev
                    motion.MotionType = "Angular"
                    motion.Formula = "%g*time" % rate
                    self.doc.recompute()
                    self.assertEqual(self.assembly.generateSimulation(sim), 0)
                    gaps = []
                    for k in range(self.assembly.numberOfFrames()):
                        self.assembly.updateForFrame(k)
                        theta = rate * k * step
                        gaps.append(f.Placement.Base.y - disk_lift(theta, roller))
                    if thrown:
                        self.assertGreater(max(gaps), 1.0, gaps)
                    else:
                        self.assertLess(max(abs(g) for g in gaps), 0.02, gaps)
                        self.assertLess(max(gaps), 1e-6, gaps)

    def test_cam_contact_on_a_rigid_group(self):
        """A cam contact whose follower is a roller fixed to a stem: the two
        one body of the run's smaller model, its joints renumbered -- the
        contact's cam joint found again there, lifted, and the follower
        riding the cam at 10 rad/s as a lone roller does (0.02 mm)"""
        if "Chrono" not in self.backends():
            self.skipTest("the Chrono backend is not built")
        import CommandCreateContact

        rate = 10.0
        cam, rev, f = self.disk_mechanism(ROLLER)
        stem = self.box("Stem", plc(-2, f.Placement.Base.y - 2, CAM_H + 4), size=4.0)
        self.joint("Fixed", (f, ["Face2"]), plc(0, 0, 4), (stem, ["Face5"]), plc(2, 2, 0))
        self.doc.recompute()
        joint = [
            o
            for o in self.assembly.getGroup("Joints").Group
            if getattr(o, "JointType", None) == "Cam"
        ][0]
        lifted = CommandCreateContact.newCamContact(self.assembly, joint)
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.Gravity = App.Vector(0, -9806.65, 0)
        turn = 2 * math.pi / rate
        sim.TimeEnd = turn
        step = turn / 200
        sim.TimeStepOutput = step
        sim.Elements = [lifted]
        motion = sim.newObject("Assembly::Motion", "Motion")
        motion.Joint = rev
        motion.MotionType = "Angular"
        motion.Formula = "%g*time" % rate
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertEqual(self.assembly.numberOfFrames(), 201)
        gaps = []
        for k in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(k)
            gaps.append(f.Placement.Base.y - disk_lift(rate * k * step, ROLLER))
            self.assertAlmostEqual(stem.Placement.Base.y - f.Placement.Base.y, -2.0, delta=1e-9)
        self.assertLess(max(abs(g) for g in gaps), 0.02, gaps)
        self.assertLess(max(gaps), 1e-6, gaps)
        channels = {c["Quantity"] for c in self.assembly.getSimulationResults()["Channels"]
                    if c["Object"] == lifted}
        self.assertIn("CamPressureAngle", channels)

    def ride_on_gravity(self, roller, rate, mark=False):
        """The disk cam turning once at rate, a cam contact holding the
        follower on it under gravity down the slider, 200 frames, with mark a
        sensor marking its side load's X crossing 0: returns the results,
        the contact's channels by quantity, and the follower"""
        import CommandCreateContact

        self.tearDown()
        self.setUp()
        cam, rev, f = self.disk_mechanism(roller)
        joint = [
            o
            for o in self.assembly.getGroup("Joints").Group
            if getattr(o, "JointType", None) == "Cam"
        ][0]
        lifted = CommandCreateContact.newCamContact(self.assembly, joint)
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.Gravity = App.Vector(0, -9806.65, 0)
        turn = 2 * math.pi / rate
        sim.TimeEnd = turn
        sim.TimeStepOutput = turn / 200
        sim.Elements = [lifted]
        motion = sim.newObject("Assembly::Motion", "Motion")
        motion.Joint = rev
        motion.MotionType = "Angular"
        motion.Formula = "%g*time" % rate
        if mark:
            import CommandCreateSimulation

            CommandCreateSimulation.newSensor(
                sim, lifted, "CamSideForce", 0.0, component="X", compare="Crosses", action="Mark"
            )
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        mine = {c["Quantity"]: c["Values"] for c in results["Channels"] if c["Object"] == lifted}
        return results, mine, f

    def test_cam_contact_force(self):
        """A cam contact's force is the one the follower's motion needs
        (docs/AssemblyDynamics.md sec 8.5): the disk cam turning at 10 rad/s,
        the follower held on it by gravity down the slider, frictionless, so
        the force along the slider, |F| cos(pressure angle), is m (g + y'')
        to 1e-3 at every frame past the first 20 ms (the follower starts
        touching unloaded, and settles). Its point found on the curve once
        a substep, fixed on the cam through it, the follower sliding under
        it, a substep at rest of 0.9 ms took it 4% deep, and the force was
        up to 6% off, in bumps every five of the contact's periods.

        The same with a sensor marking the side load's X crossing 0, at 90
        and 270 deg (0.01 deg), the force as unwatched, and the pressure
        angle the force's lean from the slider (0.01 deg): run through the
        tries locating the crossing, steps down to nanoseconds, the follower
        came out of a mark moving across the slider, its pressure angle
        1.3 deg off near 270 deg and its force ringing"""
        if "Chrono" not in self.backends():
            self.skipTest("the Chrono backend is not built")
        rate = 10.0
        g = 9806.65
        for roller in (0.0, ROLLER):
            for mark in (False, True):
                with self.subTest(roller=roller, mark=mark):
                    results, mine, f = self.ride_on_gravity(roller, rate, mark)
                    # The default material's density, kg/mm^3 (MassProperties)
                    mass = f.Shape.Volume * 2.7e-6
                    worst = [0.0, 0.0]
                    for k, t in enumerate(results["Times"]):
                        if t < 0.02:
                            continue
                        theta = rate * t
                        e = 1e-4
                        bend = (
                            disk_lift(theta + e, roller)
                            - 2 * disk_lift(theta, roller)
                            + disk_lift(theta - e, roller)
                        ) / (e * e)
                        want = mass * (g + bend * rate * rate)
                        lean = math.asin(CAM_E * math.cos(theta) / (CAM_R + roller))
                        force = mine["ContactForce"][k]
                        along = force.Length * math.cos(lean)
                        self.assertAlmostEqual(along / want, 1.0, delta=1e-3, msg=(k, theta))
                        worst[0] = max(worst[0], abs(along / want - 1.0))
                        drive = mine["CamDriveForce"][k].Length
                        side = mine["CamSideForce"][k].Length
                        read = math.degrees(math.atan2(side, drive))
                        angle = math.degrees(mine["CamPressureAngle"][k])
                        self.assertAlmostEqual(angle, read, delta=0.01, msg=(k, theta))
                        worst[1] = max(worst[1], abs(angle - read))
                    marks = [
                        math.degrees(rate * event["Time"])
                        for event in results["Events"]
                        if event["Kind"] == "Sensor"
                    ]
                    if mark:
                        self.assertEqual(len(marks), 2, marks)
                        self.assertAlmostEqual(marks[0], 90.0, delta=0.01)
                        self.assertAlmostEqual(marks[1], 270.0, delta=0.01)
                    else:
                        self.assertEqual(marks, [])
                    App.Console.PrintMessage(
                        "Cam contact force within %.3g of m (g + y''), the pressure angle "
                        "within %.3g deg of the force's lean, roller %g, marks %s\n"
                        % (worst[0], worst[1], roller, marks)
                    )

    def test_cam_pressure_angle_and_slip(self):
        """A cam contact reports its pressure angle and its slip
        (docs/AssemblyDynamics.md sec 8.5). The disk cam turning at 10 rad/s
        under gravity down the slider, the follower riding it: the normal
        runs from the disk's centre E (cos theta, sin theta) to the
        follower's point on the slider x = 0, along which it moves, so the
        pressure angle is asin(E |cos theta| / (R + r)) (5e-4 rad: the
        follower sinks some hundredths of a millimetre, and is read where
        the run's own contact point is); the slip the follower's point's
        speed from the cam's material point under it, along the curve
        (0.2 mm/s of some 200); and the contact force split along the slider
        and across it, adding up to it, the side load along x the force's
        size times E cos theta / (R + r), signed (5e-4)"""
        if "Chrono" not in self.backends():
            self.skipTest("the Chrono backend is not built")
        rate = 10.0
        for roller in (0.0, ROLLER):
            with self.subTest(roller=roller):
                results, mine, f = self.ride_on_gravity(roller, rate)
                self.assertIn("CamPressureAngle", mine)
                self.assertIn("CamSlip", mine)
                angles, slips = mine["CamPressureAngle"], mine["CamSlip"]
                forces = mine["ContactForce"]
                drives, sides = mine["CamDriveForce"], mine["CamSideForce"]
                biggest = max(f.Length for f in forces)
                self.assertGreater(biggest, 0.0)
                worst = [0.0, 0.0, 0.0]
                for k, t in enumerate(results["Times"]):
                    theta = rate * t
                    want = math.asin(CAM_E * abs(math.cos(theta)) / (CAM_R + roller))
                    self.assertAlmostEqual(angles[k], want, delta=5e-4, msg=(k, theta))
                    worst[0] = max(worst[0], abs(angles[k] - want))
                    # The slip: the follower's point (0, y) moving along y,
                    # the cam's material point under it, at R from the
                    # disk's centre along the normal, turning at the rate
                    y = disk_lift(theta, roller)
                    centre = App.Vector(CAM_E * math.cos(theta), CAM_E * math.sin(theta), 0)
                    normal = App.Vector(0, y, 0) - centre
                    normal.normalize()
                    on = centre + normal * CAM_R
                    cam_speed = App.Vector(-rate * on.y, rate * on.x, 0)
                    e = 1e-6
                    rise = (disk_lift(theta + e, roller) - disk_lift(theta - e, roller)) / (2 * e)
                    relative = App.Vector(0, rise * rate, 0) - cam_speed
                    tangent = App.Vector(-normal.y, normal.x, 0)
                    want = abs(relative.dot(tangent))
                    self.assertAlmostEqual(abs(slips[k]), want, delta=0.2, msg=(k, theta))
                    worst[1] = max(worst[1], abs(abs(slips[k]) - want))
                    # The force on the cam, -|F| along the normal with no
                    # friction, split along the slider and across it: the
                    # drive along y, the side load along x, -|F| times the
                    # normal's x, E cos theta / (R + r) -- the side the
                    # normal leans to
                    force = forces[k]
                    size = force.Length
                    drive, side = drives[k], sides[k]
                    self.assertLess((drive + side - force).Length, 1e-9 * (1 + size), (k, theta))
                    if size < 0.01 * biggest:
                        continue
                    self.assertLess(abs(drive.x) + abs(drive.z), 1e-5 * size, (k, theta))
                    self.assertLess(abs(side.y) + abs(side.z), 1e-5 * size, (k, theta))
                    self.assertAlmostEqual(side.x / size, -normal.x, delta=5e-4, msg=(k, theta))
                    worst[2] = max(worst[2], abs(side.x / size + normal.x))
                App.Console.PrintMessage(
                    "Cam pressure angle within %.3g rad, slip within %.3g mm/s, side load "
                    "within %.3g of the force, roller %g\n" % (worst[0], worst[1], worst[2], roller)
                )
                # The largest: asin(E / (R + r)) at theta 0 and pi
                self.assertAlmostEqual(
                    max(angles), math.asin(CAM_E / (CAM_R + roller)), delta=5e-4
                )


class TestSimulationMotions(CamMechanism, JointTestBase):
    """Which children of a Simulation are its motions (docs/AssemblyDynamics.md sec 1)."""

    def test_only_motion_objects_drive(self):
        """A child with a Motion proxy drives; one without a proxy too (as before); one
        with another proxy does not, though it has Joint, MotionType and Formula."""
        import CommandCreateSimulation

        g = self.box("G", size=20.0)
        a = self.box("A", plc(0, 0, 20))
        b = self.box("B", plc(30, 0, 20))
        c = self.box("C", plc(-30, 0, 20))
        self.ground(g)
        rev = self.joint("Revolute", (g, ["Face6"]), plc(5, 5, 20), (a, ["Face5"]), plc(5, 5, 0))
        sli = self.joint("Slider", (g, ["Face6"]), plc(15, 5, 20), (b, ["Face5"]), plc(5, 5, 0))
        sli2 = self.joint("Slider", (g, ["Face6"]), plc(2, 15, 20), (c, ["Face5"]), plc(5, 5, 0))
        self.doc.recompute()

        sim = self.make_simulation([(sli2, "Linear", "2*time")], t_end=1.0, t_step=0.1)
        # A Motion as a file restores one: its proxy is CommandCreateSimulation's
        # Motion. (Motion() itself asks for App::PropertyXLinkSubHidden, which
        # this fork does not have; the properties are made as the helper makes
        # them.)
        motion = self.assembly.newObject("App::FeaturePython", "Motion")
        motion.addProperty("App::PropertyXLinkSub", "Joint", "Motion")
        motion.addProperty("App::PropertyString", "Formula", "Motion")
        motion.addProperty("App::PropertyEnumeration", "MotionType", "Motion")
        motion.MotionType = ["Angular", "Linear"]
        motion.MotionType = "Angular"
        motion.Joint = rev
        motion.Formula = "36*time"
        motion.Proxy = CommandCreateSimulation.Motion.__new__(CommandCreateSimulation.Motion)
        self.assertEqual(type(motion.Proxy).__name__, "Motion")

        # Something else that happens to carry the same property names.
        class ForceElement:
            def __init__(self, obj):
                obj.Proxy = self

        other = self.assembly.newObject("App::FeaturePython", "Force")
        other.addProperty("App::PropertyXLinkSub", "Joint", "Force")
        other.addProperty("App::PropertyString", "Formula", "Force")
        other.addProperty("App::PropertyEnumeration", "MotionType", "Force")
        other.MotionType = ["Angular", "Linear"]
        other.MotionType = "Linear"
        other.Joint = sli
        other.Formula = "100*time"
        ForceElement(other)
        sim.Group = sim.Group + [motion, other]
        self.doc.recompute()
        b_start = App.Placement(b.Placement)

        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        n = self.assembly.numberOfFrames()
        self.assembly.updateForFrame(n - 1)
        # The proxy-less motion drove C 2 mm up the slider in 1 s.
        self.assertAlmostEqual(c.Placement.Base.z - 20.0, 2.0, delta=1e-6)
        # The Motion turned A by 36 rad.
        turn = math.remainder(turn_about_z(a.Placement) - 36.0, 2 * math.pi)
        self.assertAlmostEqual(turn, 0.0, delta=1e-6)
        # The force element did not drive B.
        self.assertTrue(b.Placement.isSame(b_start, 1e-9), b.Placement)


class TestCamJointTyped(CamMechanism, JointTestBase):
    """The Cam joint as a C++ joint (docs/AssemblyObjectModel.md sec 7.1): its
    sides are element bindings, the cam's edge and the follower are read through
    them. The mechanisms are built as Python joints and converted in place,
    which keeps every frame exactly."""

    def convert(self):
        import AssemblyMigrate

        report = AssemblyMigrate.convertDocument(self.doc, force=True)
        self.doc.recompute()
        self.assertTrue(all(new is not None for _, new, _ in report), report)
        cams = [
            o
            for o in self.doc.Objects
            if o.isDerivedFrom("Assembly::Joint") and o.JointType == "Cam"
        ]
        self.assertEqual(len(cams), 1)
        return cams[0]

    def test_picks(self):
        """A C++ cam from picks: an edge and a vertex; a face is refused, by its name."""
        import AssemblyApp

        g = self.cam_ground()
        cam = self.cylinder("Cam", CAM_R, CAM_H, plc(CAM_E, 0, 0))
        f = self.box("Follower", plc(0, 30, CAM_H), size=4.0)
        self.doc.recompute()
        top = circle_edge(cam, CAM_H)
        # The refusal reads the element bindings' copies, made on recompute (the
        # task recomputes the sides before it asks)
        j = self.assembly.makeJoint("Cam", (cam, top), (f, "Vertex1"))
        self.doc.recompute()
        self.assertEqual(AssemblyApp.getCamRefusal(j), "")
        j = self.assembly.makeJoint("Cam", (cam, "Face1"), (f, "Vertex1"))
        self.doc.recompute()
        self.assertIn("the face Face1 of Cam is not an edge", AssemblyApp.getCamRefusal(j))

    def test_static_solve_puts_the_follower_on_the_cam(self):
        """From 0.8 mm off, a solve puts the follower on the cam, a point and a roller."""
        import AssemblyApp

        for roller in (0.0, ROLLER):
            with self.subTest(roller=roller):
                self.tearDown()
                self.setUp()
                cam, rev, f = self.disk_mechanism(roller, start_error=0.8)
                j = self.convert()
                self.assertEqual(AssemblyApp.getCamRefusal(j), "")
                status = self.solve()
                theta = turn_about_z(cam.Placement)
                self.assertAlmostEqual(f.Placement.Base.y, disk_lift(theta, roller), delta=LIFT_TOL)
                self.assertAlmostEqual(f.Placement.Base.z, CAM_H, delta=LIFT_TOL)
                self.assertEqual(status["DoF"], 1)

    def test_disk_cam_simulation(self):
        """The disk cam for more than six turns: the lift at every frame."""
        for roller in (0.0, ROLLER):
            with self.subTest(roller=roller):
                self.tearDown()
                self.setUp()
                cam, rev, f = self.disk_mechanism(roller)
                sim = self.make_simulation([(rev, "Angular", "%g*time" % RATE)])
                self.convert()
                self.assertEqual(self.assembly.generateSimulation(sim), 0)
                n = self.assembly.numberOfFrames()
                for k in range(round(T_END / T_STEP) + 1):
                    self.assembly.updateForFrame(n - 1 - k)
                    theta = RATE * (T_END - T_STEP * k)
                    self.assertAlmostEqual(
                        f.Placement.Base.y, disk_lift(theta, roller), delta=LIFT_TOL
                    )
