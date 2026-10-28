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
