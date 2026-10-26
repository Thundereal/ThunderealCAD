# SPDX-License-Identifier: LGPL-2.1-or-later

# The 3D sketch, phase S1 (docs/Sketch3D.md section 10): the object, its solver facade,
# external references, the output shape, the Python API and save / restore.

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part
import Sketcher

V = App.Vector


def addPath(sk):
    """A pipe route from the origin: 10 along X, 20 along Z, then 15 perpendicular to
    both. Fully constrained; returns the constraint indices of the three lengths."""
    l0 = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(9, 0.5, 0.3)))
    l1 = sk.addGeometry(Part.LineSegment(V(9, 0.5, 0.3), V(9.4, 0.2, 18)))
    l2 = sk.addGeometry(Part.LineSegment(V(9.4, 0.2, 18), V(10, 14, 19)))
    cons = [
        Sketcher.Constraint("Coincident", -1, 1, l0, 1),
        Sketcher.Constraint("Coincident", l0, 2, l1, 1),
        Sketcher.Constraint("Coincident", l1, 2, l2, 1),
        Sketcher.Constraint("AlongX", l0),
        Sketcher.Constraint("AlongZ", l1),
        Sketcher.Constraint("Perpendicular", l1, l2),
        Sketcher.Constraint("Perpendicular", l0, l2),
    ]
    sk.addConstraint(cons)
    return sk.addConstraint(
        [
            Sketcher.Constraint("Distance", l0, 10.0),
            Sketcher.Constraint("Distance", l1, 20.0),
            Sketcher.Constraint("Distance", l2, 15.0),
        ]
    )


class TestSketch3D(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestSketch3D")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def assertVector(self, a, b, tol=1e-7):
        self.assertLess((a - b).Length, tol, f"{a} != {b}")

    def testPathSolves(self):
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "Path")
        addPath(sk)
        self.assertEqual(sk.solve(), 0)
        self.assertEqual(sk.DoF, 0)
        self.assertEqual((sk.Conflicting, sk.Redundant, sk.Malformed), ([], [], []))
        g = sk.Geometry
        self.assertVector(g[0].EndPoint, V(10, 0, 0))
        self.assertVector(g[1].EndPoint, V(10, 0, 20))
        self.assertVector(g[2].EndPoint, V(10, 15, 20))
        self.doc.recompute()
        self.assertEqual(sk.Shape.ShapeType, "Wire")
        self.assertEqual(len(sk.Shape.Edges), 3)
        self.assertAlmostEqual(sk.Shape.Length, 45, 7)

    def testDiagnosis(self):
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        line = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(10, 1, 2)))
        self.assertEqual(sk.DoF, 6)
        sk.addConstraint(Sketcher.Constraint("AlongX", line))
        self.assertEqual(sk.DoF, 4)
        sk.addConstraint(Sketcher.Constraint("DistanceX", line, 10.0))
        self.assertEqual(sk.DoF, 3)
        # The same length along X again: redundant.
        sk.addConstraint(Sketcher.Constraint("Distance", line, 10.0))
        self.assertEqual(sk.Redundant, [2])
        # A different one: conflicting, and the solve refuses.
        sk.delConstraint(2)
        sk.addConstraint(Sketcher.Constraint("Distance", line, 12.0))
        self.assertEqual(sorted(sk.Conflicting), [1, 2])
        self.assertEqual(sk.solve(), -3)
        # A 2D-only constraint is reported, not solved.
        sk.delConstraint(2)
        sk.addConstraint(Sketcher.Constraint("Horizontal", line))
        self.assertEqual(sk.Malformed, [2])

    def testWorkplaneRectangle(self):
        # A rectangle on a fixed workplane tilted 30 deg: 5 DOF; a fourth right angle
        # is implied and so redundant (docs/Sketch3D.md section 6). Fixed: blocked, as
        # Keep Plane makes it; a workplane without is solved for.
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        pln = Part.Plane(V(0, 0, 40), V(0, -math.sin(math.pi / 6), math.cos(math.pi / 6)))
        wp = sk.addGeometry(pln)
        sk.addConstraint(Sketcher.Constraint("Block", wp))
        pts = [V(0, 0, 40), V(20, 0, 40), V(20, 8.7, 45), V(0, 8.7, 45)]
        lines = [sk.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % 4])) for i in range(4)]
        for i in range(4):
            sk.addConstraint(Sketcher.Constraint("Coincident", lines[i], 2, lines[(i + 1) % 4], 1))
            sk.addConstraint(Sketcher.Constraint("PointOnObject", lines[i], 1, wp))
        for i in range(3):
            sk.addConstraint(Sketcher.Constraint("Perpendicular", lines[i], lines[i + 1]))
        self.assertEqual(sk.DoF, 5)
        self.assertEqual(sk.Redundant, [])
        sk.addConstraint(Sketcher.Constraint("Perpendicular", lines[3], lines[0]))
        self.assertEqual(len(sk.Redundant), 1)
        # The workplane is construction: not in the shape.
        sk.delConstraint(len(sk.Constraints) - 1)
        self.doc.recompute()
        self.assertEqual(len(sk.Shape.Edges), 4)

    def testArcTangentBend(self):
        # Line - arc - line, tangent at both ends, radius 5: the routing bend.
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        a = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(20, 0, 0)))
        circ = Part.Circle(V(20, 5, 0), V(0, 0, 1), 5)
        arc = sk.addGeometry(Part.ArcOfCircle(circ, -math.pi / 2, 0))
        # Drawn roughly up Z, as a line gets AlongZ: AlongZ equates the ends' X and Y,
        # so a line drawn along Y would start the solve at zero length.
        b = sk.addGeometry(Part.LineSegment(V(25, 5, 0), V(26, 6, 25)))
        # One at a time, solving after each, as the editor adds them. All at once, the
        # diagnosis runs at the drawn shape -- the arc a quarter turn from where it ends
        # up -- where the rank it measures is not the solution's, and it reports a
        # conflict; the 2D sketch diagnoses before it solves in the same way.
        for c in [
            Sketcher.Constraint("Coincident", -1, 1, a, 1),
            Sketcher.Constraint("AlongX", a),
            Sketcher.Constraint("Tangent", a, 2, arc, 1),
            Sketcher.Constraint("Tangent", arc, 2, b, 1),
            Sketcher.Constraint("Radius", arc, 5.0),
            Sketcher.Constraint("AlongZ", b),
            Sketcher.Constraint("Distance", a, 20.0),
            Sketcher.Constraint("Distance", b, 25.0),
        ]:
            sk.addConstraint(c)
            self.assertEqual(sk.Conflicting, [])
        self.assertEqual(sk.solve(), 0)
        arcGeo = sk.Geometry[arc]
        self.assertAlmostEqual(arcGeo.Radius, 5, 7)
        # The second line is along Z, so the arc turned out of the XY plane into XZ.
        d = sk.Geometry[b].EndPoint - sk.Geometry[b].StartPoint
        self.assertAlmostEqual(abs(d.z), 25, 7)
        self.assertAlmostEqual(abs(d.x) + abs(d.y), 0, 7)
        self.assertVector(arcGeo.StartPoint, V(20, 0, 0))
        self.assertAlmostEqual(arcGeo.Axis.z, 0, 7)
        self.assertEqual(sk.DoF, 0)

    def testSweepAlongPath(self):
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "Path")
        addPath(sk)
        circle = self.doc.addObject("Part::Circle", "Profile")
        circle.Radius = 1
        circle.Placement = App.Placement(V(0, 0, 0), App.Rotation(V(0, 1, 0), 90))
        sweep = self.doc.addObject("Part::Sweep", "Sweep")
        sweep.Sections = [circle]
        sweep.Spine = (sk, ["Edge1", "Edge2", "Edge3"])
        sweep.Solid = True
        sweep.Transition = "Right corner"
        self.doc.recompute()
        self.assertTrue(sweep.Shape.isValid())
        self.assertEqual(len(sweep.Shape.Solids), 1)

    def testPipeInBody(self):
        body = self.doc.addObject("PartDesign::Body", "Body")
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "Path")
        body.addObject(sk)
        self.assertIn(sk, body.Group)
        addPath(sk)
        prof = self.doc.addObject("Sketcher::SketchObject", "Profile")
        body.addObject(prof)
        prof.AttachmentSupport = (self.doc.getObject("YZ_Plane"), [""])
        prof.MapMode = "FlatFace"
        prof.addGeometry(Part.Circle(V(0, 0, 0), V(0, 0, 1), 1))
        pipe = self.doc.addObject("PartDesign::AdditivePipe", "Pipe")
        body.addObject(pipe)
        pipe.Profile = prof
        pipe.Spine = (sk, ["Edge1", "Edge2", "Edge3"])
        pipe.Transition = "Right corner"
        self.doc.recompute()
        self.assertTrue(pipe.isValid(), pipe.getStatusString())
        self.assertGreater(pipe.Shape.Volume, 0)

    def testEdgeNamesFollowGeometry(self):
        # Output edges are named from the geometry id, so a reference to the last leg
        # follows it through a dimension change and through a new edge that comes
        # first in the wire.
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "Path")
        lengths = addPath(sk)
        self.doc.recompute()
        names = [sk.Shape.getElementMappedName("Edge%d" % (i + 1)) for i in range(3)]
        self.assertEqual(names, ["g1;SKT", "g2;SKT", "g3;SKT"])
        link = self.doc.addObject("App::FeaturePython", "Link")
        link.addProperty("App::PropertyLinkSub", "Leg")
        link.Leg = (sk, ["Edge3"])

        sk.setDatum(lengths[2], 30.0)
        self.doc.recompute()
        self.assertEqual(sk.Shape.getElementMappedName("Edge3"), "g3;SKT")
        self.assertAlmostEqual(sk.Shape.getElement(link.Leg[1][0]).Length, 30, 7)

        # A lead-in before the start: the wire now begins with it.
        lead = sk.addGeometry(Part.LineSegment(V(-5, 0, 0), V(0, 0, 0)))
        sk.addConstraint(Sketcher.Constraint("Coincident", lead, 2, 0, 1))
        self.doc.recompute()
        self.assertAlmostEqual(sk.Shape.getElement(link.Leg[1][0]).Length, 30, 7)
        self.assertEqual(sk.Shape.getElementMappedName(link.Leg[1][0]), "g3;SKT")

    def testExpressionDrivesDimension(self):
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "Path")
        lengths = addPath(sk)
        cons = sk.Constraints
        cons[lengths[1]].Name = "rise"
        sk.Constraints = cons
        sk.setExpression("Constraints.rise", "2 * 15 mm")
        self.doc.recompute()
        self.assertAlmostEqual(sk.getDatum("rise").Value, 30, 7)
        self.assertVector(sk.Geometry[1].EndPoint, V(10, 0, 30))

    def testReferenceDimension(self):
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "Path")
        addPath(sk)
        # The diagonal from start to end, measured, not driving.
        i = sk.addConstraint(Sketcher.Constraint("Distance", 0, 1, 2, 2, 1.0))
        sk.setDriving(i, False)
        self.assertEqual(sk.DoF, 0)
        self.assertAlmostEqual(sk.getDatum(i).Value, math.sqrt(10**2 + 15**2 + 20**2), 7)
        with self.assertRaises(ValueError):
            sk.setDatum(i, 5.0)

    def testSaveRestore(self):
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "Path")
        addPath(sk)
        box = self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()
        sk.addExternal(box, "Vertex8")
        self.doc.recompute()
        path = os.path.join(tempfile.mkdtemp(), "sketch3d.FCStd")
        self.doc.saveAs(path)
        name = self.doc.Name
        App.closeDocument(name)
        self.doc = App.openDocument(path)
        sk = self.doc.getObject("Path")
        self.assertEqual(sk.TypeId, "Sketcher::Sketch3DObject")
        self.assertEqual(len(sk.Geometry), 3)
        self.assertEqual(len(sk.Constraints), 10)
        self.assertEqual(sk.Constraints[3].Type, "AlongX")
        self.assertEqual(len(sk.ExternalGeo), 1)
        self.assertEqual(sk.DoF, 0)
        self.assertVector(sk.Geometry[2].EndPoint, V(10, 15, 20))
        self.doc.recompute()
        self.assertTrue(sk.isValid())
        self.assertEqual(len(sk.Shape.Edges), 3)

    def testWorkplaneSaveRestore(self):
        # A kept workplane is a Part.Plane in the geometry list: it is written and
        # read back with its frame and its construction flag (GeomPlane had no
        # persistence: "Not yet implemented: GeomPlane::Save").
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        pl = Part.Plane()
        plm = App.Placement(V(1, 2, 3), App.Rotation(V(1, 1, 0), 40))
        pl.transform(plm.toMatrix())
        wp = sk.addGeometry(pl)
        sk.addConstraint(Sketcher.Constraint("Block", wp))  # kept where it is
        line = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(5, 5, 5)))
        sk.addConstraint(Sketcher.Constraint("PointOnObject", line, 2, wp))
        self.assertEqual(sk.solve(), 0)
        self.doc.recompute()
        path = os.path.join(tempfile.mkdtemp(), "workplane.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        sk = self.doc.getObject("S")
        plane = sk.Geometry[wp]
        self.assertEqual(type(plane).__name__, "Plane")
        self.assertVector(plane.Position, V(1, 2, 3))
        self.assertVector(plane.Axis, plm.Rotation.multVec(V(0, 0, 1)))
        self.assertVector(plane.value(1, 0) - plane.value(0, 0), plm.Rotation.multVec(V(1, 0, 0)))
        self.assertEqual(sk.DoF, 5)  # the line, one end held on the plane
        self.doc.recompute()
        self.assertTrue(sk.isValid())
        # construction: not in the shape
        self.assertEqual(len(sk.Shape.Edges), 1)

    def testExternalEdgeFollowedThroughNamingChange(self):
        # A line endpoint on a model edge whose index changes when a cut adds edges.
        box = self.doc.addObject("Part::Box", "Box")
        box.Length, box.Width, box.Height = 20, 10, 10
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius, cyl.Height = 2, 30
        cyl.Placement.Base = V(50, 5, -5)
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base, cut.Tool = box, cyl
        self.doc.recompute()

        def topFrontEdge(shape):
            for i, e in enumerate(shape.Edges):
                a, b = e.Vertexes[0].Point, e.Vertexes[-1].Point
                if max(abs(a.y), abs(b.y), abs(a.z - 10), abs(b.z - 10)) < 1e-7:
                    return "Edge%d" % (i + 1)

        before = topFrontEdge(cut.Shape)
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        ext = sk.addExternal(cut, before)
        self.assertEqual(ext, -7)
        line = sk.addGeometry(Part.LineSegment(V(3, 1, 9), V(3, 20, 30)))
        sk.addConstraint(Sketcher.Constraint("PointOnObject", line, 1, ext))
        sk.addConstraint(Sketcher.Constraint("DistanceX", line, 1, 4.0))

        cyl.Placement.Base = V(5, 5, -5)  # now cuts the box: edges renumbered
        box.Length = 30
        self.doc.recompute()
        self.assertNotEqual(topFrontEdge(cut.Shape), before)
        self.assertEqual(sk.ExternalGeometry[0][1][0], topFrontEdge(cut.Shape))
        self.assertTrue(sk.isValid(), sk.getStatusString())
        self.assertVector(sk.Geometry[line].StartPoint, V(4, 0, 10))
        e = sk.ExternalGeo[0]
        self.assertVector(e.EndPoint - e.StartPoint, V(30, 0, 0))

    def testExternalPlaneAndFrame(self):
        # A point on the frame's XY plane and a datum plane; the sketch frame attached
        # and moved: geometry stays in the frame, references come into it.
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        sk.Placement = App.Placement(V(0, 0, 5), App.Rotation())
        plane = self.doc.addObject("App::Plane", "Plane")
        plane.Placement = App.Placement(V(0, 0, 25), App.Rotation())
        line = sk.addGeometry(Part.LineSegment(V(1, 2, 3), V(4, 6, 9)))
        ext = sk.addExternal(plane, "")
        sk.addConstraint(Sketcher.Constraint("PointOnObject", line, 1, -4))  # frame XY
        sk.addConstraint(Sketcher.Constraint("PointOnObject", line, 2, ext))
        self.assertEqual(sk.solve(), 0)
        self.assertAlmostEqual(sk.Geometry[line].StartPoint.z, 0, 7)
        # The datum is 20 above the sketch's frame.
        self.assertAlmostEqual(sk.Geometry[line].EndPoint.z, 20, 7)

    def testMovePoint(self):
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        line = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(10, 0, 0)))
        sk.addConstraint(Sketcher.Constraint("Coincident", -1, 1, line, 1))
        sk.addConstraint(Sketcher.Constraint("Distance", line, 10.0))
        self.assertEqual(sk.movePoint(line, 2, V(0, 0, 20)), 0)
        # The end goes as near as the length allows: straight up.
        self.assertVector(sk.Geometry[line].EndPoint, V(0, 0, 10), 1e-6)
        self.assertVector(sk.Geometry[line].StartPoint, V(0, 0, 0))

    def testSplineThroughPoints(self):
        # A spline whose points at its knots are sketch points, the points placed.
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        pts = [V(0, 0, 0), V(10, 5, 2), V(20, 0, 8), V(30, -5, 4)]
        bs = Part.BSplineCurve()
        bs.interpolate(pts)
        spline = sk.addGeometry(bs)
        knots = bs.getKnots()
        ids = [sk.addGeometry(Part.Point(p), True) for p in pts]
        for k, pid in enumerate(ids):
            c = Sketcher.Constraint(
                "InternalAlignment:Sketcher::BSplineKnotPoint", pid, 1, spline, k
            )
            sk.addConstraint(c)
        sk.addConstraint(Sketcher.Constraint("DistanceZ", ids[2], 1, 12.0))
        self.assertEqual(sk.solve(), 0)
        self.assertEqual(sk.Malformed, [])
        curve = sk.Geometry[spline]
        self.assertVector(curve.value(knots[2]), sk.Geometry[ids[2]].toShape().Point, 1e-7)
        self.assertAlmostEqual(sk.Geometry[ids[2]].Z, 12, 7)

    def test2DSketchRefuses3DTypes(self):
        sk = self.doc.addObject("Sketcher::SketchObject", "S2")
        sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(10, 1, 0)))
        sk.addGeometry(Part.LineSegment(V(0, 5, 0), V(10, 6, 0)))
        with self.assertRaises(ValueError):
            sk.addConstraint(Sketcher.Constraint("AlongX", 0))
        # Horizontal in its form on a 3D sketch's workplane: a line and a plane
        with self.assertRaises(ValueError):
            sk.addConstraint(Sketcher.Constraint("Horizontal", 0, 1))

    # -- S3b: workplanes solved for (docs/Sketch3D.md section 6, v2) -------------------

    def planeOf(self, sk, wp):
        g = sk.Geometry[wp]
        return g.Position, g.Axis

    def testFreeWorkplaneFollowsItsConstraints(self):
        # A workplane without a Block is a frame of 6 DOF: put through a point whose
        # height is a dimension (3), its normal along a guide line (2), its X along the
        # frame's X (Horizontal of that axis on it, 1). It moves with the height and
        # carries a line held on it.
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        wp = sk.addGeometry(Part.Plane())
        p = sk.addGeometry(Part.Point(V(0, 0, 10)))
        guide = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(0, -1, 1)))
        line = sk.addGeometry(Part.LineSegment(V(2, 1, 8), V(9, 3, 6)))
        sk.addConstraint(Sketcher.Constraint("Block", guide))
        sk.addConstraint(Sketcher.Constraint("DistanceX", p, 1, 0.0))
        sk.addConstraint(Sketcher.Constraint("DistanceY", p, 1, 0.0))
        h = sk.addConstraint(Sketcher.Constraint("DistanceZ", p, 1, 10.0))
        for c in [
            Sketcher.Constraint("Coincident", wp, 3, p, 1),
            Sketcher.Constraint("Perpendicular", guide, wp),
            Sketcher.Constraint("Horizontal", -1, wp),
            Sketcher.Constraint("PointOnObject", line, 1, wp),
            Sketcher.Constraint("PointOnObject", line, 2, wp),
        ]:
            sk.addConstraint(c)
            self.assertEqual(sk.Conflicting, [])
        self.assertEqual(sk.Redundant, [])
        self.assertEqual(sk.Malformed, [])
        self.assertEqual(sk.DoF, 4)  # the line: its ends slide on the plane
        o, n = self.planeOf(sk, wp)
        self.assertVector(o, V(0, 0, 10))
        self.assertLess(n.cross(V(0, -1, 1)).Length, 1e-7)
        g = sk.Geometry[line]
        for q in (g.StartPoint, g.EndPoint):
            self.assertAlmostEqual((q - o).dot(n), 0, 7)
        # the height moves the plane, and the line with it
        sk.setDatum(h, 25.0)
        o, n = self.planeOf(sk, wp)
        self.assertVector(o, V(0, 0, 25))
        g = sk.Geometry[line]
        for q in (g.StartPoint, g.EndPoint):
            self.assertAlmostEqual((q - o).dot(n), 0, 7)
        self.assertLess(abs(sk.Geometry[wp].value(1, 0).sub(o).dot(V(0, 1, 0))), 1e-7)

    def testSymmetricAboutAPlane(self):
        # Mirrored in a plane: the midpoint on it and the two along its normal, 3 DOF.
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        wp = sk.addGeometry(Part.Plane(V(0, 0, 5), V(0, 0, 1)))
        sk.addConstraint(Sketcher.Constraint("Block", wp))
        a = sk.addGeometry(Part.Point(V(1, 2, 9)))
        b = sk.addGeometry(Part.Point(V(4, 1, 0)))
        sk.addConstraint(Sketcher.Constraint("DistanceX", a, 1, 1.0))
        sk.addConstraint(Sketcher.Constraint("DistanceY", a, 1, 2.0))
        sk.addConstraint(Sketcher.Constraint("DistanceZ", a, 1, 9.0))
        self.assertEqual(sk.DoF, 3)
        sk.addConstraint(Sketcher.Constraint("Symmetric", a, 1, b, 1, wp))
        self.assertEqual(sk.DoF, 0)
        self.assertEqual(sk.Redundant, [])
        g = sk.Geometry[b]
        self.assertVector(V(g.X, g.Y, g.Z), V(1, 2, 1))

    def testHorizontalVerticalOnAWorkplane(self):
        # A rectangle on a tilted kept plane, its sides horizontal and vertical as seen
        # on the plane: one equation each, so with its corners held on the plane
        # nothing is redundant, and 4 DOF are left, as for a 2D rectangle.
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        plm = App.Placement(V(0, 0, 10), App.Rotation(V(1, 0, 0), 30))
        pl = Part.Plane()
        pl.transform(plm.toMatrix())
        wp = sk.addGeometry(pl)
        sk.addConstraint(Sketcher.Constraint("Block", wp))
        corners = [plm.multVec(V(x, y, 0)) for x, y in ((0, 0), (20, 1), (21, 9), (1, 8))]
        lines = [
            sk.addGeometry(Part.LineSegment(corners[i], corners[(i + 1) % 4])) for i in range(4)
        ]
        for i in range(4):
            sk.addConstraint(Sketcher.Constraint("Coincident", lines[i], 2, lines[(i + 1) % 4], 1))
            sk.addConstraint(Sketcher.Constraint("PointOnObject", lines[i], 1, wp))
        for i, kind in enumerate(("Horizontal", "Vertical", "Horizontal", "Vertical")):
            sk.addConstraint(Sketcher.Constraint(kind, lines[i], wp))
            self.assertEqual(sk.Redundant, [])
        self.assertEqual(sk.Malformed, [])
        self.assertEqual(sk.DoF, 4)
        x, y = plm.Rotation.multVec(V(1, 0, 0)), plm.Rotation.multVec(V(0, 1, 0))
        for i, axis in enumerate((x, y, x, y)):
            g = sk.Geometry[lines[i]]
            d = g.EndPoint - g.StartPoint
            self.assertLess(d.cross(axis).Length, 1e-7 * d.Length, (i, d))

    def testAngleBetweenWorkplanes(self):
        # A plane at 30 deg to a kept one, hinged on the frame's X: the angle of the
        # normals, and X along the plane (Parallel) and across none of it (Horizontal).
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "S")
        base = sk.addGeometry(Part.Plane())
        sk.addConstraint(Sketcher.Constraint("Block", base))
        # Started near, its X along the hinge: the dot products these use have no
        # gradient where the plane would have to turn a quarter round its normal.
        pl = Part.Plane()
        pl.transform(App.Placement(V(0, 0, 0), App.Rotation(V(1, 0, 0), 10)).toMatrix())
        wp = sk.addGeometry(pl)
        for c in [
            Sketcher.Constraint("Coincident", wp, 3, -1, 1),
            Sketcher.Constraint("Parallel", -1, wp),
            Sketcher.Constraint("Horizontal", -1, wp),
            Sketcher.Constraint("Angle", base, wp, math.radians(30)),
        ]:
            sk.addConstraint(c)
            self.assertEqual(sk.Conflicting, [])
        self.assertEqual(sk.Redundant, [])
        self.assertEqual(sk.DoF, 0)
        o, n = self.planeOf(sk, wp)
        self.assertAlmostEqual(math.degrees(n.getAngle(V(0, 0, 1))), 30, 6)
        self.assertAlmostEqual(n.dot(V(1, 0, 0)), 0, 7)


if __name__ == "__main__":
    unittest.main()
