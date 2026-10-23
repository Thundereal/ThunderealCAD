# SPDX-License-Identifier: LGPL-2.1-or-later
"""What each joint type does to the parts, checked on the solved placements.

Every joint here is built the way the rack-and-pinion test in TestCore builds
one: real faces as references, but the joint coordinate systems (JCS) given
explicitly (Detach), so a test does not depend on how OpenCASCADE orients a
face. The checks are then made on the JCS in global coordinates after the
solve, which is what each solver joint constrains. The moving part always
starts somewhere the joint is not satisfied, so a solve that does nothing
fails.
"""

import math

import FreeCAD as App
import Part
import unittest

import JointObject
import UtilsAssembly

from AssemblyTests.TestCore import AssemblyTestBase

TOL = 1e-6
ANGLE_TOL = 1e-6  # radians

Z = App.Vector(0, 0, 1)
X = App.Vector(1, 0, 0)


def plc(x=0.0, y=0.0, z=0.0, axis=None, angle=0.0):
    rot = App.Rotation(axis, angle) if axis is not None else App.Rotation()
    return App.Placement(App.Vector(x, y, z), rot)


def zaxis(p):
    return p.Rotation.multVec(Z)


def xaxis(p):
    return p.Rotation.multVec(X)


def dist_to_axis(point, axis_plc):
    """Distance from a point to the z axis of a placement."""
    d = point - axis_plc.Base
    z = zaxis(axis_plc)
    return (d - z * d.dot(z)).Length


def angle_about(rot_from, rot_to, axis):
    """Signed angle (degrees) of the rotation taking rot_from to rot_to, about axis."""
    delta = rot_to.multiply(rot_from.inverted())
    ang = math.degrees(delta.Angle)
    if delta.Axis.dot(axis) < 0:
        ang = -ang
    # delta.Angle is in [0, 2pi); fold into (-180, 180]
    if ang > 180:
        ang -= 360
    elif ang <= -180:
        ang += 360
    return ang


class JointTestBase(AssemblyTestBase):
    """Helpers shared by the joint, DoF, status and drag tests."""

    def setUp(self):
        super().setUp()
        self.prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.prefs.GetBool("SolveInJointCreation", True)
        # Joints are built property by property; solve when the test asks.
        self.prefs.SetBool("SolveInJointCreation", False)

    def tearDown(self):
        self.prefs.SetBool("SolveInJointCreation", self._solveInCreation)
        super().tearDown()

    # ------------------------------------------------------------ building

    def box(self, name, placement=None, size=10.0):
        b = self.assembly.newObject("Part::Box", name)
        b.Length = b.Width = b.Height = size
        if placement is not None:
            b.Placement = placement
        return b

    def cylinder(self, name, radius, height, placement=None):
        c = self.assembly.newObject("Part::Cylinder", name)
        c.Radius = radius
        c.Height = height
        if placement is not None:
            c.Placement = placement
        return c

    def ground(self, obj):
        gj = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(gj, obj)
        return gj

    def joint(self, jtype, ref1, plc1, ref2, plc2, **props):
        """A joint with explicit JCS: plcN is relative to the object of refN."""
        j = self.jointgroup.newObject("App::FeaturePython", jtype)
        JointObject.Joint(j, JointObject.JointTypes.index(jtype))
        j.Detach1 = True
        j.Detach2 = True
        j.Reference1 = ref1
        j.Reference2 = ref2
        j.Placement1 = plc1
        j.Placement2 = plc2
        for k, v in props.items():
            setattr(j, k, v)
        return j

    def solve(self):
        self.doc.recompute()
        r = self.assembly.solve()
        self.assertEqual(r, 0, "solve() returned %r" % r)
        return self.assembly.getSolveStatus()

    def jcs(self, j, i):
        if i == 1:
            return UtilsAssembly.getJcsGlobalPlc(j.Placement1, j.Reference1)
        return UtilsAssembly.getJcsGlobalPlc(j.Placement2, j.Reference2)

    def assertClose(self, a, b, tol=TOL, msg=None):
        self.assertLess(abs(a - b), tol, msg or "%r != %r (tol %g)" % (a, b, tol))

    def assertVecClose(self, a, b, tol=TOL, msg=None):
        self.assertLess((a - b).Length, tol, msg or "%r != %r (tol %g)" % (a, b, tol))

    def assertMoved(self, obj, start):
        self.assertFalse(
            obj.Placement.isSame(start, 1e-3),
            "%s did not move: %s" % (obj.Name, obj.Placement),
        )

    def drag(self, part, targets, mass=10.0, post=True):
        """Drag part through targets as the 3D view does; the doDragStep() results."""
        self.assembly.preDrag([part], mass)
        results = []
        for target in targets:
            part.Placement = target
            results.append(self.assembly.doDragStep())
        if post:
            self.assembly.postDrag()
        return results

    # ------------------------------------------------------------ a standard pair

    def ground_and_mover(self):
        """A grounded box G and a box B placed where no joint below is satisfied."""
        self.g = self.box("G")
        self.b_start = plc(23, -14, 31, App.Vector(1, 2, 3), 37)
        self.b = self.box("B", self.b_start)
        self.ground(self.g)
        return self.g, self.b

    # JCS on G (relative to G): on its top face, tilted so nothing lines up with
    # the global axes by accident. JCS on B: on its bottom face.
    JCS_G = plc(4, 3, 10, App.Vector(1, 1, 0), 20)
    JCS_B = plc(2, 6, 0, App.Vector(0, 1, 1), 15)

    def pair_joint(self, jtype, **props):
        g, b = self.ground_and_mover()
        return self.joint(jtype, (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B, **props)


class TestJointTypes(JointTestBase):
    """One joint between a grounded box and a free one, per joint type."""

    def test_fixed(self):
        j = self.pair_joint("Fixed")
        status = self.solve()
        self.assertMoved(self.b, self.b_start)
        self.assertTrue(self.jcs(j, 1).isSame(self.jcs(j, 2), TOL))
        self.assertEqual(status["DoF"], 0)

    def test_revolute(self):
        j = self.pair_joint("Revolute")
        status = self.solve()
        self.assertMoved(self.b, self.b_start)
        p1, p2 = self.jcs(j, 1), self.jcs(j, 2)
        self.assertVecClose(p1.Base, p2.Base)
        self.assertClose(zaxis(p1).dot(zaxis(p2)), 1.0)
        self.assertEqual(status["DoF"], 1)

    def test_cylindrical(self):
        j = self.pair_joint("Cylindrical")
        status = self.solve()
        self.assertMoved(self.b, self.b_start)
        p1, p2 = self.jcs(j, 1), self.jcs(j, 2)
        self.assertClose(zaxis(p1).dot(zaxis(p2)), 1.0)
        self.assertClose(dist_to_axis(p2.Base, p1), 0.0)
        self.assertEqual(status["DoF"], 2)

    def test_slider(self):
        j = self.pair_joint("Slider")
        status = self.solve()
        self.assertMoved(self.b, self.b_start)
        p1, p2 = self.jcs(j, 1), self.jcs(j, 2)
        self.assertClose(zaxis(p1).dot(zaxis(p2)), 1.0)
        self.assertClose(xaxis(p1).dot(xaxis(p2)), 1.0)
        self.assertClose(dist_to_axis(p2.Base, p1), 0.0)
        self.assertEqual(status["DoF"], 1)

    def test_ball(self):
        j = self.pair_joint("Ball")
        status = self.solve()
        self.assertMoved(self.b, self.b_start)
        self.assertVecClose(self.jcs(j, 1).Base, self.jcs(j, 2).Base)
        self.assertEqual(status["DoF"], 3)

    def test_parallel(self):
        j = self.pair_joint("Parallel")
        status = self.solve()
        self.assertMoved(self.b, self.b_start)
        self.assertClose(abs(zaxis(self.jcs(j, 1)).dot(zaxis(self.jcs(j, 2)))), 1.0)
        self.assertEqual(status["DoF"], 4)

    def test_perpendicular(self):
        j = self.pair_joint("Perpendicular")
        status = self.solve()
        self.assertMoved(self.b, self.b_start)
        self.assertClose(zaxis(self.jcs(j, 1)).dot(zaxis(self.jcs(j, 2))), 0.0)
        self.assertEqual(status["DoF"], 5)

    def test_angle(self):
        j = self.pair_joint("Angle", Angle=30.0)
        status = self.solve()
        self.assertMoved(self.b, self.b_start)
        cos = zaxis(self.jcs(j, 1)).dot(zaxis(self.jcs(j, 2)))
        self.assertClose(math.degrees(math.acos(max(-1.0, min(1.0, cos)))), 30.0, 1e-4)
        self.assertEqual(status["DoF"], 5)

    def test_island_without_ground_stays(self):
        """Parts joined to each other but not to anything grounded are not solved.

        The assembly's Origin always counts as grounded, so solve() does not
        return -6; the joint is dropped as unconnected and nothing moves.
        """
        g = self.box("G")
        b = self.box("B", plc(23, -14, 31))
        self.joint("Fixed", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        self.assertTrue(g.Placement.isSame(plc(), 1e-12))
        self.assertTrue(b.Placement.isSame(plc(23, -14, 31), 1e-12))


class TestDistanceJoint(JointTestBase):
    """The Distance joint and every sub-type it maps to a solver joint.

    The sub-type is picked from the referenced geometry (getDistanceType) and
    radii are folded into the distance, as docs/AssemblySolverInterface.md
    sec 5.2 tabulates. Each case here checks, on the solved JCS, the relation
    its solver joint imposes, with the distance the table gives. I is the
    Reference1 side: each case picks the side the sub-type wants first (the
    builder swaps the other order in its model, never in the document).
    """

    D = 3.0
    R_CYL = 4.0
    R_SPH = 5.0
    R_CIRCLE = 4.0  # the cylinder's circular edges

    def fixtures(self):
        g = self.box("G", size=20.0)
        cyl_g = self.cylinder("CylG", self.R_CYL, 20.0, plc(40, 0, 0))
        sph_g = self.assembly.newObject("Part::Sphere", "SphG")
        sph_g.Radius = self.R_SPH
        sph_g.Placement = plc(-40, 0, 0)
        tor_g = self.assembly.newObject("Part::Torus", "TorG")
        tor_g.Radius1 = 8.0
        tor_g.Radius2 = 2.0
        tor_g.Placement = plc(0, 40, 0)
        for obj in (g, cyl_g, sph_g, tor_g):
            self.ground(obj)
        start = plc(7, -23, 41, App.Vector(1, 2, 3), 25)
        box_m = self.box("BoxM", start, size=10.0)
        cyl_m = self.cylinder("CylM", self.R_CYL, 10.0, start)
        sph_m = self.assembly.newObject("Part::Sphere", "SphM")
        sph_m.Radius = self.R_SPH
        sph_m.Placement = start
        tor_m = self.assembly.newObject("Part::Torus", "TorM")
        tor_m.Radius1 = 8.0
        tor_m.Radius2 = 2.0
        tor_m.Placement = start
        self.doc.recompute()
        return dict(
            g=g, cyl_g=cyl_g, sph_g=sph_g, tor_g=tor_g,
            box_m=box_m, cyl_m=cyl_m, sph_m=sph_m, tor_m=tor_m,
        )  # fmt: skip

    def distance_joint(self, ref1, ref2, distance):
        j = self.jointgroup.newObject("App::FeaturePython", "Distance")
        JointObject.Joint(j, JointObject.JointTypes.index("Distance"))
        j.Distance = distance
        # setJointConnectors places each JCS on its element (a cylinder's axis,
        # a sphere's centre, a plane's normal) the way the joint task does.
        j.Proxy.setJointConnectors(
            j, [[ref1[0], [ref1[1], ref1[1]]], [ref2[0], [ref2[1], ref2[1]]]]
        )
        return j

    def check(self, name, rel, expected, ref1, ref2, d=None, start=None):
        d = self.D if d is None else d
        with self.subTest(subtype=name):
            # One fresh document per case: joints must not see each other.
            self.tearDown()
            self.setUp()
            f = self.fixtures()
            r1 = (f[ref1[0]], ref1[1])
            r2 = (f[ref2[0]], ref2[1])
            moving = r2[0]
            if start is not None:
                moving.Placement = start
                self.doc.recompute()
            start = moving.Placement
            j = self.distance_joint(r1, r2, d)
            self.solve()
            self.assertMoved(moving, start)
            pi, pj = self.jcs(j, 1), self.jcs(j, 2)
            local = pi.inverse().multVec(pj.Base)  # J's origin in I's frame
            zi, zj = zaxis(pi), zaxis(pj)
            want = expected(d)
            msg = "%s: %s, I=%s J=%s" % (name, rel, pi, pj)
            if rel == "spherical":
                self.assertVecClose(pi.Base, pj.Base, msg=msg)
            elif rel == "sphsph":
                self.assertClose(local.Length, want, msg=msg)
            elif rel == "cylsph":
                self.assertClose(math.hypot(local.x, local.y), want, msg=msg)
            elif rel == "revcyl":
                self.assertClose(abs(zi.dot(zj)), 1.0, msg=msg)
                self.assertClose(math.hypot(local.x, local.y), want, msg=msg)
            elif rel == "inplane":
                self.assertClose(local.z, want, msg=msg)
            elif rel == "planar":
                self.assertClose(abs(zi.dot(zj)), 1.0, msg=msg)
                self.assertClose(local.z, want, msg=msg)
            elif rel == "lineinplane":
                self.assertClose(zi.dot(zj), 0.0, msg=msg)
                self.assertClose(local.z, want, msg=msg)
            else:
                self.fail("unknown relation " + rel)

    def test_distance_subtypes(self):
        rc, rs, rcircle = self.R_CYL, self.R_SPH, self.R_CIRCLE
        # (name, relation, expected value of d, reference 1, reference 2)
        # Box faces: Face6 top (plane); edges: Edge1 a line. Part::Cylinder:
        # Face1 the lateral face, Edge1 its seam (a line), Edge2 and Edge3 its
        # circles. Sphere/Torus: Face1.
        cases = [
            ("PointPoint", "sphsph", lambda d: d, ("g", "Vertex8"), ("box_m", "Vertex1")),
            ("LineLine", "revcyl", lambda d: d, ("g", "Edge1"), ("box_m", "Edge1")),
            ("LineCircle", "revcyl", lambda d: d + rcircle, ("g", "Edge1"), ("cyl_m", "Edge2")),
            ("CircleCircle", "revcyl", lambda d: d + 2 * rcircle, ("cyl_g", "Edge2"), ("cyl_m", "Edge2")),
            ("PlanePlane", "planar", lambda d: d, ("g", "Face6"), ("box_m", "Face5")),
            ("PlaneCylinder", "lineinplane", lambda d: d + rc, ("g", "Face6"), ("cyl_m", "Face1")),
            ("PlaneSphere", "inplane", lambda d: d + rs, ("g", "Face6"), ("sph_m", "Face1")),
            ("PlaneTorus", "planar", lambda d: d, ("g", "Face6"), ("tor_m", "Face1")),
            ("CylinderCylinder", "revcyl", lambda d: d + 2 * rc, ("cyl_g", "Face1"), ("cyl_m", "Face1")),
            ("CylinderSphere", "cylsph", lambda d: d + rc + rs, ("cyl_g", "Face1"), ("sph_m", "Face1")),
            ("CylinderTorus", "revcyl", lambda d: d + rc, ("cyl_g", "Face1"), ("tor_m", "Face1")),
            ("TorusTorus", "planar", lambda d: d, ("tor_g", "Face1"), ("tor_m", "Face1")),
            ("TorusSphere", "cylsph", lambda d: d + rs, ("tor_g", "Face1"), ("sph_m", "Face1")),
            ("SphereSphere", "sphsph", lambda d: d + 2 * rs, ("sph_g", "Face1"), ("sph_m", "Face1")),
            ("PointPlane", "inplane", lambda d: d, ("g", "Face6"), ("box_m", "Vertex1")),
            ("PointCylinder", "cylsph", lambda d: d + rc, ("cyl_g", "Face1"), ("box_m", "Vertex1")),
            ("PointSphere", "sphsph", lambda d: d + rs, ("sph_g", "Face1"), ("box_m", "Vertex1")),
            ("LinePlane", "lineinplane", lambda d: d, ("g", "Face6"), ("box_m", "Edge1")),
            ("PointLine", "cylsph", lambda d: d, ("g", "Edge1"), ("box_m", "Vertex1")),
            ("PointCurve", "inplane", lambda d: d, ("cyl_g", "Edge2"), ("box_m", "Vertex1")),
        ]  # fmt: skip
        for name, rel, expected, ref1, ref2 in cases:
            self.check(name, rel, expected, ref1, ref2)

    def test_point_point_zero_is_a_ball(self):
        # Started near: from the far start the other cases use, OndselSolver's
        # spherical joint does not converge (the sphere-sphere joint does).
        self.check(
            "PointPoint d=0",
            "spherical",
            lambda d: 0.0,
            ("g", "Vertex8"),
            ("box_m", "Vertex1"),
            d=0.0,
            start=plc(22, 18, -9, App.Vector(1, 2, 3), 10),
        )

    def test_distance_dof(self):
        f = self.fixtures()
        self.distance_joint((f["g"], "Face6"), (f["box_m"], "Face5"), self.D)
        status = self.solve()
        # Planar removes one translation and two rotations: 3 left; the three
        # other movers are free parts, 6 each.
        self.assertEqual(status["DoF"], 3 + 3 * 6)


def line_of(obj, edge):
    """A point on the straight edge (global) and its unit direction."""
    e = obj.Shape.getElement(edge)
    p0, p1 = e.Vertexes[0].Point, e.Vertexes[-1].Point
    return p0, (p1 - p0).normalize()


def dist_to_line(point, origin, direction):
    d = point - origin
    return (d - direction * d.dot(direction)).Length


class _Warnings:
    """Console warnings that name a text."""

    def __init__(self, text):
        self.text = text
        self.messages = []

    def __call__(self, notifier, msg, level):
        if self.text in msg:
            self.messages.append(msg)


class TestDistanceMapped(JointTestBase):
    """Audit sec 8.1 decision 7, checked on the solved geometry, not on the JCS.

    LineCylinder: the line parallel to the cylinder's axis, d + r from it (d
    from the surface). LineSphere: the line d + r from the sphere's centre.
    CurvePlane: the curve's plane parallel to the plane, d from it. The 14
    refused sub-types: a joint already in a document still solves as
    Planar(d), with one warning per joint (docs/AssemblySolverInterface.md
    sec 6.8).
    """

    D = TestDistanceJoint.D
    R_CYL = TestDistanceJoint.R_CYL
    R_SPH = TestDistanceJoint.R_SPH
    fixtures = TestDistanceJoint.fixtures
    distance_joint = TestDistanceJoint.distance_joint

    def mapped(self, ref1, ref2, extra=None):
        """A fresh document, the joint between ref1 and ref2 (fixture keys), solved."""
        self.tearDown()
        self.setUp()
        f = self.fixtures()
        if extra:
            f.update(extra())
            self.doc.recompute()
        moving = f[ref2[0]]
        start = moving.Placement
        j = self.distance_joint((f[ref1[0]], ref1[1]), (moving, ref2[1]), self.D)
        self.solve()
        self.assertMoved(moving, start)
        return f, j

    def ellipse(self):
        e = self.assembly.newObject("Part::Ellipse", "EllM")
        e.MajorRadius = 6.0
        e.MinorRadius = 3.0
        e.Placement = plc(7, -23, 41, App.Vector(1, 2, 3), 25)
        return dict(ell_m=e)

    def cone(self):
        c = self.assembly.newObject("Part::Cone", "ConeM")
        c.Radius1 = 5.0
        c.Radius2 = 2.0
        c.Height = 8.0
        c.Placement = plc(7, -23, 41, App.Vector(1, 2, 3), 25)
        return dict(cone_m=c)

    def test_api(self):
        import AssemblyApp

        refused = AssemblyApp.getRefusedDistanceTypes()
        self.assertEqual(
            sorted(refused),
            sorted(
                [
                    "PlaneCone", "CylinderCone", "ConeCone", "ConeTorus", "ConeSphere",
                    "PointCone", "PointTorus", "LineCone", "LineTorus", "CurveCylinder",
                    "CurveSphere", "CurveCone", "CurveTorus", "Other",
                ]
            ),
        )  # fmt: skip
        f = self.fixtures()
        j = self.distance_joint((f["cyl_g"], "Face1"), (f["box_m"], "Edge1"), self.D)
        self.assertEqual(AssemblyApp.getDistanceType(j), "LineCylinder")
        self.assertFalse(AssemblyApp.isDistanceJointRefused(j))
        j.Reference1 = [f["tor_g"], ["Face1", "Face1"]]  # a torus and a line
        self.assertEqual(AssemblyApp.getDistanceType(j), "LineTorus")
        self.assertTrue(AssemblyApp.isDistanceJointRefused(j))

    def test_line_cylinder(self):
        f, j = self.mapped(("cyl_g", "Face1"), ("box_m", "Edge1"))
        axis = f["cyl_g"].Shape.getElement("Face1").Surface
        p, u = line_of(f["box_m"], "Edge1")
        self.assertClose(abs(u.dot(axis.Axis)), 1.0)
        self.assertClose(dist_to_line(p, axis.Center, axis.Axis), self.D + self.R_CYL)

    def test_line_sphere(self):
        f, j = self.mapped(("sph_g", "Face1"), ("box_m", "Edge1"))
        centre = f["sph_g"].Shape.getElement("Face1").Surface.Center
        p, u = line_of(f["box_m"], "Edge1")
        self.assertClose(dist_to_line(centre, p, u), self.D + self.R_SPH)

    def check_curve_plane(self, f, curve_obj, edge):
        plane = f["g"].Shape.getElement("Face6").Surface
        n = plane.Axis
        e = curve_obj.Shape.getElement(edge)
        # Every point of the curve is d above the plane, along its normal.
        for pt in e.discretize(7):
            self.assertClose((pt - plane.Position).dot(n), self.D)

    def test_curve_plane_circle(self):
        f, j = self.mapped(("g", "Face6"), ("cyl_m", "Edge2"))
        self.check_curve_plane(f, f["cyl_m"], "Edge2")

    def test_curve_plane_ellipse(self):
        """Not a circle: the JCS is the curve's plane (UtilsAssembly.findPlacement)."""
        f, j = self.mapped(("g", "Face6"), ("ell_m", "Edge1"), extra=self.ellipse)
        self.check_curve_plane(f, f["ell_m"], "Edge1")

    def test_refused_in_document_solves_planar_and_warns_once(self):
        warnings = _Warnings("PlaneCone")
        App.Console.AttachObserver(warnings)
        try:
            f, j = self.mapped(("g", "Face6"), ("cone_m", "Face1"), extra=self.cone)
            self.solve()
            self.solve()
        finally:
            App.Console.DetachObserver(warnings)
        self.assertEqual(len(warnings.messages), 1, warnings.messages)
        self.assertIn(j.Name, warnings.messages[0])
        # Planar(d): the two JCS z axes parallel, J's origin d along I's z.
        pi, pj = self.jcs(j, 1), self.jcs(j, 2)
        self.assertClose(abs(zaxis(pi).dot(zaxis(pj))), 1.0)
        self.assertClose(pi.inverse().multVec(pj.Base).z, self.D)


class TestSolveStatus(JointTestBase):
    """DoF, redundancy, conflicts and malformed joints, as getSolveStatus() reports them."""

    I = App.Placement()

    def test_free_parts_dof(self):
        self.ground(self.box("G"))
        self.box("B", plc(20, 0, 0))
        self.box("C", plc(40, 0, 0))
        status = self.solve()
        self.assertEqual(status["DoF"], 12)
        self.assertEqual(status["Redundant"], [])

    def test_chain_dof(self):
        g, b = self.ground_and_mover()
        # Near its place on top of B: from far away the solver does not converge.
        c = self.box("C", self.b_start.multiply(plc(1, -1, 12)))
        self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        self.joint("Slider", (b, ["Face6"]), plc(5, 5, 10), (c, ["Face5"]), plc(5, 5, 0))
        status = self.solve()
        self.assertEqual(status["DoF"], 2)

    def test_redundant_not_conflicting(self):
        g, b = self.ground_and_mover()
        self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        second = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        status = self.solve()
        self.assertEqual(status["Redundant"], [second.Name])
        self.assertEqual(status["Conflicting"], [])
        self.assertEqual(status["PartiallyRedundant"], [])
        # A repeated joint takes no freedom away.
        self.assertEqual(status["DoF"], 1)

    def test_partially_redundant(self):
        g, b = self.ground_and_mover()
        rev = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        # A Cylindrical on the same axis repeats four of the Revolute's five
        # equations.
        cyl = self.joint("Cylindrical", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        status = self.solve()
        self.assertEqual(status["Conflicting"], [])
        self.assertTrue(status["Redundant"])
        self.assertTrue(set(status["Redundant"]) <= {rev.Name, cyl.Name})
        # Four equations repeat, fewer than either joint has: every joint that
        # lost some lost only part of its equations.
        self.assertEqual(sorted(status["PartiallyRedundant"]), sorted(status["Redundant"]))
        self.assertEqual(status["DoF"], 1)

    def test_conflicting(self):
        g, b = self.ground_and_mover()
        rev = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        # A ball 5 mm along the revolute axis from where B's JCS must sit.
        ball = self.joint(
            "Ball", (g, ["Face6"]), self.JCS_G.multiply(plc(0, 0, 5)), (b, ["Face5"]), self.JCS_B
        )
        status = self.solve()
        self.assertEqual(len(status["Conflicting"]), 1)
        self.assertIn(status["Conflicting"][0], (rev.Name, ball.Name))
        self.assertIn(status["Conflicting"][0], status["Redundant"])
        # The solve still converged on the equations it kept: B's JCS sits on
        # one of the two points, not between them.
        pb = self.jcs(rev, 2).Base
        on_rev = (pb - self.jcs(rev, 1).Base).Length < TOL
        on_ball = (pb - self.jcs(ball, 1).Base).Length < TOL
        self.assertTrue(on_rev != on_ball, "B at %s" % pb)

    def test_malformed(self):
        g, b = self.ground_and_mover()
        good = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        same_part = self.joint("Revolute", (b, ["Face6"]), self.I, (b, ["Face5"]), self.I)
        lost_face = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face99"]), self.I)
        status = self.solve()
        self.assertEqual(sorted(status["Malformed"]), sorted([same_part.Name, lost_face.Name]))
        self.assertNotIn(good.Name, status["Malformed"])
        # The good joint still solved.
        p1, p2 = self.jcs(good, 1), self.jcs(good, 2)
        self.assertVecClose(p1.Base, p2.Base)

    def test_incomplete_is_not_malformed(self):
        g, b = self.ground_and_mover()
        self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        # A joint as the joint task leaves it before the second pick.
        j = self.jointgroup.newObject("App::FeaturePython", "Pending")
        JointObject.Joint(j, JointObject.JointTypes.index("Revolute"))
        j.Reference1 = (g, ["Face6"])
        status = self.solve()
        self.assertEqual(status["Malformed"], [])
        self.assertEqual(status["DoF"], 1)


def rotate_about(obj_plc, center, axis, angle):
    """obj_plc rotated by angle (degrees) about the line through center along axis."""
    r = App.Placement(App.Vector(), App.Rotation(axis, angle), center)
    return r.multiply(obj_plc)


class TestDrag(JointTestBase):
    """Dragging through AssemblyObject.preDrag / doDragStep / postDrag.

    The 3D view drags the same way: it writes the dragged part's Placement,
    then asks for a drag step, and the solver moves the rest.
    """

    def revolute_pair(self, **props):
        g, b = self.ground_and_mover()
        j = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B, **props)
        self.solve()
        return j

    def test_drag_revolute(self):
        j = self.revolute_pair()
        axis_plc = self.jcs(j, 1)
        start = self.b.Placement
        targets = [
            rotate_about(start, axis_plc.Base, zaxis(axis_plc), a) for a in (10, 20, 30, 40)
        ]
        ok = self.drag(self.b, targets, post=False)
        self.assertEqual(ok, [True] * 4)
        # A move the joint allows is followed exactly.
        self.assertTrue(self.b.Placement.isSame(targets[-1], 1e-6), "%s" % self.b.Placement)
        # One it does not allow (along the axis) is not: the joint holds.
        off_axis = App.Placement(zaxis(axis_plc) * 5, App.Rotation()).multiply(targets[-1])
        self.b.Placement = off_axis
        self.assertTrue(self.assembly.doDragStep())
        self.assembly.postDrag()
        self.assertVecClose(self.jcs(j, 1).Base, self.jcs(j, 2).Base)
        self.assertEqual(self.assembly.getDragStatus()["Failures"], 0)

    def test_drag_moves_fixed_neighbour(self):
        j = self.revolute_pair()
        c = self.box("C", plc(50, 50, 50))
        # C welded on top of B; the drag bundles it with B.
        self.joint("Fixed", (self.b, ["Face6"]), plc(0, 0, 10), (c, ["Face5"]), plc())
        self.solve()
        rel = self.b.Placement.inverse().multiply(c.Placement)
        axis_plc = self.jcs(j, 1)
        target = rotate_about(self.b.Placement, axis_plc.Base, zaxis(axis_plc), 60)
        self.assertEqual(self.drag(self.b, [target]), [True])
        self.assertTrue(self.b.Placement.isSame(target, 1e-6))
        self.assertTrue(c.Placement.isSame(self.b.Placement.multiply(rel), 1e-6))

    def test_drag_weighting(self):
        """The dragged part's drag mass makes it lead (audit defect 2).

        A two-link arm: A turns about the ground's z axis, B turns about a
        pin 2 mm from that axis. Dragging B to a point the pin cannot reach,
        the solve trades B's distance from the target against A's rotation;
        the heavier B is, the further A turns towards the target.
        """

        def run(mass):
            self.tearDown()
            self.setUp()
            g = self.box("G")
            a = self.box("A", plc(0, 0, 10), size=4.0)
            b = self.box("B", plc(2, 0, 14), size=4.0)
            self.ground(g)
            self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 10), (a, ["Face5"]), plc())
            self.joint("Revolute", (a, ["Face6"]), plc(2, 0, 4), (b, ["Face5"]), plc())
            self.solve()
            target = plc(2, 2, 14)
            self.assertEqual(self.drag(b, [target], mass=mass), [True])
            turned = angle_about(App.Rotation(), a.Placement.Rotation, Z)
            miss = (b.Placement.Base - target.Base).Length
            return turned, miss

        turned1, miss1 = run(1.0)
        turned10, miss10 = run(10.0)
        # The pin reaches the target's direction at 45 degrees.
        self.assertGreater(turned1, 0.0)
        self.assertLess(turned10, 45.0 + 1e-6)
        self.assertGreater(turned10, turned1 + 0.1, "mass 1: %g deg, mass 10: %g deg" % (turned1, turned10))
        self.assertLess(miss10, miss1)

    def test_drag_failure_is_reported(self):
        """A step the solver cannot take is reported, and the drag goes on (audit defect 5)."""
        j = self.revolute_pair()
        axis_plc = self.jcs(j, 1)
        start = self.b.Placement
        good = rotate_about(start, axis_plc.Base, zaxis(axis_plc), 15)
        # Far enough that the solver's squared residuals overflow: no attempt
        # of the step converges. (Not NaN: a NaN Placement sends Part::Feature's
        # Placement/Shape sync into endless recursion before the solver sees it.)
        bad = App.Placement(App.Vector(1e200, 0, 0), App.Rotation())
        self.assembly.preDrag([self.b])
        self.b.Placement = good
        self.assertTrue(self.assembly.doDragStep())
        self.b.Placement = bad
        self.assertFalse(self.assembly.doDragStep())
        status = self.assembly.getDragStatus()
        self.assertTrue(status["LastStepFailed"])
        self.assertEqual(status["Failures"], 1)
        # The document is back on the last good step, not on the bad target.
        self.assertTrue(self.b.Placement.isSame(good, 1e-6), "%s" % self.b.Placement)
        # And the drag goes on.
        further = rotate_about(start, axis_plc.Base, zaxis(axis_plc), 30)
        self.b.Placement = further
        self.assertTrue(self.assembly.doDragStep())
        self.assertFalse(self.assembly.getDragStatus()["LastStepFailed"])
        self.assembly.postDrag()
        self.assertTrue(self.b.Placement.isSame(further, 1e-6))
        self.assertEqual(self.assembly.getDragStatus(), {"Steps": 3, "Failures": 1, "LastStepFailed": False})

    def test_no_drag_in_progress(self):
        self.revolute_pair()
        self.assertFalse(self.assembly.doDragStep())


class TestLimits(JointTestBase):
    """Joint limits act while dragging (OndselSolver applies them in drag steps only)."""

    def test_slider_length_limits(self):
        g = self.box("G")
        b = self.box("B", plc(0, 0, 10))  # travel 0: limits act in drags only
        self.ground(g)
        j = self.joint(
            "Slider",
            (g, ["Face6"]),
            plc(5, 5, 10),
            (b, ["Face5"]),
            plc(5, 5, 0),
            EnableLengthMin=True,
            LengthMin=0.0,
            EnableLengthMax=True,
            LengthMax=20.0,
        )
        self.solve()

        def travel():
            return self.jcs(j, 1).inverse().multVec(self.jcs(j, 2).Base).z

        self.assertClose(travel(), 0.0)
        start = b.Placement
        up = [App.Placement(Z * dz, App.Rotation()).multiply(start) for dz in (10, 20, 30, 50)]
        ok = self.drag(b, up)
        self.assertEqual(ok, [True] * 4)
        self.assertClose(travel(), 20.0, 1e-5)
        down = [App.Placement(Z * dz, App.Rotation()).multiply(start) for dz in (10, 0, -10, -30)]
        ok = self.drag(b, down)
        self.assertEqual(ok, [True] * 4)
        self.assertClose(travel(), 0.0, 1e-5)

    def test_revolute_angle_limit(self):
        g = self.box("G")
        b = self.box("B", plc(0, 0, 10))  # angle 0: limits act in drags only
        self.ground(g)
        j = self.joint(
            "Revolute",
            (g, ["Face6"]),
            plc(5, 5, 10),
            (b, ["Face5"]),
            plc(5, 5, 0),
            EnableAngleMax=True,
            AngleMax=30.0,
        )
        self.solve()

        def turned():
            return angle_about(self.jcs(j, 1).Rotation, self.jcs(j, 2).Rotation, zaxis(self.jcs(j, 1)))

        self.assertClose(turned(), 0.0, 1e-6)
        center = self.jcs(j, 1).Base
        start = b.Placement
        steps = [rotate_about(start, center, Z, a) for a in (10, 20, 30, 40, 60)]
        ok = self.drag(b, steps)
        self.assertEqual(ok, [True] * 5)
        self.assertClose(turned(), 30.0, 1e-4)


class TestCoupledJoints(JointTestBase):
    """Gears, Belt, Screw and RackPinion couple two motions; a drag shows the ratio.

    Each holds whatever relation it starts in (the solver records the
    initial value), so a static solve moves nothing: the drag is the test.
    """

    def gear_pair(self, jtype):
        g = self.box("G")
        a = self.cylinder("A", 10.0, 5.0, plc(0, 0, 10))
        b = self.cylinder("B", 20.0, 5.0, plc(30, 0, 10))
        self.ground(g)
        self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 10), (a, ["Face3"]), plc())
        self.joint("Revolute", (g, ["Face6"]), plc(30, 0, 10), (b, ["Face3"]), plc())
        self.joint(jtype, (a, ["Face1"]), plc(), (b, ["Face1"]), plc(), Distance=10.0, Distance2=20.0)
        self.solve()
        start_a = a.Placement.Rotation
        start_b = b.Placement.Rotation
        target = rotate_about(a.Placement, App.Vector(0, 0, 10), Z, 30)
        self.assertEqual(self.drag(a, [target]), [True])
        # The drag is a weighted compromise: A leads but need not reach the
        # target, since turning B costs too. The coupling itself is exact.
        turned_a = angle_about(start_a, a.Placement.Rotation, Z)
        turned_b = angle_about(start_b, b.Placement.Rotation, Z)
        self.assertGreater(turned_a, 5.0)
        # Both turn about their own axes only.
        self.assertVecClose(a.Placement.Base, App.Vector(0, 0, 10))
        self.assertVecClose(b.Placement.Base, App.Vector(30, 0, 10))
        return turned_b / turned_a

    def test_gears(self):
        # Meshing gears turn the other way, by the radius ratio 10/20.
        self.assertClose(self.gear_pair("Gears"), -0.5, 1e-6)

    def test_belt(self):
        # A belt keeps the direction.
        self.assertClose(self.gear_pair("Belt"), 0.5, 1e-6)

    def test_screw(self):
        g = self.box("G")
        shaft = self.cylinder("Shaft", 3.0, 40.0, plc(0, 0, 10))
        nut = self.box("Nut", plc(10, 10, 10), size=6.0)
        self.ground(g)
        self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 10), (shaft, ["Face3"]), plc())
        self.joint("Slider", (g, ["Face6"]), plc(10, 10, 10), (nut, ["Face5"]), plc())
        self.joint("Screw", (nut, ["Face5"]), plc(), (shaft, ["Face1"]), plc(), Distance=8.0)
        self.solve()
        nut_start = nut.Placement
        shaft_start = shaft.Placement.Rotation
        target = rotate_about(shaft.Placement, App.Vector(0, 0, 10), Z, 90)
        self.assertEqual(self.drag(shaft, [target]), [True])
        turned = angle_about(shaft_start, shaft.Placement.Rotation, Z)
        self.assertGreater(abs(turned), 5.0)
        moved = nut.Placement.Base - nut_start.Base
        # An 8 mm pitch: the nut travels 8 mm per turn of the shaft.
        self.assertClose(abs(moved.z), 8.0 * abs(turned) / 360.0, 1e-6)
        self.assertClose(math.hypot(moved.x, moved.y), 0.0)
        self.assertTrue(nut.Placement.Rotation.isSame(nut_start.Rotation, 1e-9))

    def test_rack_pinion(self):
        g = self.box("G", size=10.0)
        rack = self.box("Rack", plc(0, 0, 10), size=6.0)
        pinion = self.cylinder("Pinion", 10.0, 5.0, plc(0, 20, 10))
        self.ground(g)
        along_x = App.Rotation(App.Vector(0, 1, 0), 90)  # local z -> global x
        self.joint(
            "Slider",
            (g, ["Face6"]),
            App.Placement(App.Vector(0, 0, 10), along_x),
            (rack, ["Face5"]),
            App.Placement(App.Vector(), along_x),
        )
        self.joint("Revolute", (g, ["Face6"]), plc(0, 20, 10), (pinion, ["Face3"]), plc())
        self.joint(
            "RackPinion",
            (rack, ["Face5"]),
            App.Placement(App.Vector(), along_x),
            (pinion, ["Face1"]),
            plc(),
            Distance=10.0,
        )
        self.solve()
        rack_start = rack.Placement
        pinion_start = pinion.Placement.Rotation
        target = rotate_about(pinion.Placement, App.Vector(0, 20, 10), Z, 30)
        self.assertEqual(self.drag(pinion, [target]), [True])
        turned = angle_about(pinion_start, pinion.Placement.Rotation, Z)
        # Moving the rack costs more than turning the pinion saves, so the
        # pinion gets well short of 30 degrees; it still turns.
        self.assertGreater(abs(turned), 0.1)
        moved = rack.Placement.Base - rack_start.Base
        # The 10 mm pitch circle rolls along the rack: 10 mm per radian.
        self.assertClose(abs(moved.x), 10.0 * math.radians(abs(turned)), 1e-6)
        self.assertClose(math.hypot(moved.y, moved.z), 0.0)
