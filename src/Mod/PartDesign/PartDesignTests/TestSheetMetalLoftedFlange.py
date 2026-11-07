# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's lofted flange, PartDesign::SheetMetalLoftedFlange
# (docs/SheetMetal.md sec 5.1, M5), its first step: between two circles or
# arcs on parallel planes about one axis, a cone frustum (a cylinder where
# they are the same size), one conical bend of the sheet model, laid flat
# exactly. A circle all round leaves a seam the rule's Gap wide.
#
# Its second, lines to lines: each line of the profile with the section's
# line in its place, a flat facet between them where they are parallel, a
# wall of the model, two triangles either side of the shorter diagonal
# where they are not, and a bend of the rule's radius at every fold. The
# rims are cut square to the sheet, so the flat is exact: straight lines
# all round. A closed outline leaves a seam the rule's Gap wide across the
# middle of the profile's first line, or along its diagonal.
#
# Its third, mixed: round to lines (a square to a round), the round cut into
# Segments chords all round and lofted to the lines as lines are, each
# ruling the shorter of the two next -- a fan of triangles from each corner
# to its share of the round, a triangle on each line -- the seam across the
# middle of the first line. Where a fan's folds meet at a corner on the side
# the sheet turns toward (the sheet inside a square to a round, or about
# it), a thick sheet cannot come to the corner's point: a relief there, cut
# square to the sheet, so the flat stays exact.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector


class TestSheetMetalLoftedFlange(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalLoftedFlange")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def round(self, radius, z=0, span=None, centre=(0, 0), tilt=0, name="Sketch"):
        """A sketch at height z of a circle, or an arc over span (degrees,
        from, to) counterclockwise; tilt turns its plane about X"""
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        sketch.Placement = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation(V(1, 0, 0), tilt))
        circle = Part.Circle(V(centre[0], centre[1], 0), V(0, 0, 1), radius)
        if span is None:
            sketch.addGeometry(circle)
        else:
            sketch.addGeometry(
                Part.ArcOfCircle(circle, math.radians(span[0]), math.radians(span[1]))
            )
        return sketch

    def loft(self, profile, section, name="LoftedFlange", **props):
        loft = self.Body.newObject("PartDesign::SheetMetalLoftedFlange", name)
        loft.Profile = profile
        loft.Section = section
        for k, v in props.items():
            setattr(loft, k, v)
        self.Doc.recompute()
        return loft

    def owners(self, feature):
        count = {}
        for i in range(len(feature.Shape.Faces)):
            name = feature.sheetElementOf("Face%d" % (i + 1))
            count[name] = count.get(name, 0) + 1
        return count

    def cone(self, feature):
        cones = [e for e in feature.SheetModel["Elements"] if e["Kind"] == "Cone"]
        self.assertEqual(len(cones), 1)
        return cones[0]

    def frustum(self, side="Outside"):
        """Round to round: 20 at the bottom, 10 at 30 up"""
        return self.loft(self.round(20), self.round(10, 30, name="Top"), MaterialSide=side)

    # the frustum's slant length and the cosine and sine of its half angle
    SLANT = math.hypot(10, 30)
    COS = 30 / SLANT
    SIN = 10 / SLANT

    def checkFrustum(self, loft, middle, zmin, zmax):
        """Valid, one cone owning its six faces, as much as its section
        turned through its angle; the sketches on the side asked"""
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        self.assertTrue(loft.Shape.isValid())
        self.assertEqual(self.owners(loft), {"Bend1": 6})
        cone = self.cone(loft)
        self.assertEqual(cone["Name"], "Bend1")
        # the axis from the narrow end to the wide, the half angle above 0
        self.assertAlmostEqual(cone["SemiAngle"], math.degrees(math.atan2(10, 30)))
        self.assertTrue(cone["Axis"].isEqual(V(0, 0, -1), 1e-9))
        # all round less the seam: the rule's Gap on the narrow sketched circle
        sweep = 2 * math.pi - 0.1 / 10
        self.assertAlmostEqual(cone["Angle"], math.degrees(sweep))
        T = 1
        self.assertAlmostEqual(loft.Shape.Volume, sweep * self.SLANT * T * middle, places=6)
        box = loft.Shape.BoundBox
        self.assertAlmostEqual(box.ZMin, zmin, places=6)
        self.assertAlmostEqual(box.ZMax, zmax, places=6)
        return cone

    def testFrustumOutside(self):
        """The sketches on the inner skin, the one facing the axis"""
        loft = self.frustum("Outside")
        cone = self.checkFrustum(loft, 15 + self.COS / 2, 0, 30 + self.SIN)
        self.assertAlmostEqual(cone["Radius"], 10)

    def testFrustumInside(self):
        """On the outer skin: the inner one a thickness in"""
        loft = self.frustum("Inside")
        cone = self.checkFrustum(loft, 15 - self.COS / 2, -self.SIN, 30)
        # where the inner skin meets the narrow end
        self.assertAlmostEqual(cone["Radius"], 10 - self.COS)

    def testFrustumMiddle(self):
        loft = self.frustum("Middle")
        self.checkFrustum(loft, 15, -self.SIN / 2, 30 + self.SIN / 2)

    def checkFlat(self, loft, k):
        """Laid flat exactly: an annular sector the thickness through, its
        arcs about one point at the neutral cone's lengths from its apex, its
        sides the slant length, apart by the angle the cone's turn becomes"""
        flat = loft.getSubObject("Flat")
        self.assertIsNotNone(flat)
        self.assertTrue(flat.isValid())
        T = 1
        sweep = 2 * math.pi - 0.1 / 10
        box = flat.BoundBox
        self.assertAlmostEqual(box.ZLength, T, places=6)
        top = [f for f in flat.Faces if f.Surface.TypeId == "Part::GeomPlane"
               and abs(f.CenterOfMass.z - box.ZMax) < 1e-6]
        self.assertEqual(len(top), 1)
        arcs = sorted(
            (e for e in top[0].Edges if e.Curve.TypeId == "Part::GeomCircle"),
            key=lambda e: e.Curve.Radius,
        )
        lines = [e for e in top[0].Edges if e.Curve.TypeId == "Part::GeomLine"]
        self.assertEqual(len(arcs), 2)
        self.assertEqual(len(lines), 2)
        neutral = k * T * self.COS
        for arc, r in zip(arcs, (10, 20)):
            self.assertAlmostEqual(arc.Curve.Radius, (r + neutral) / self.SIN, places=6)
        self.assertTrue(arcs[0].Curve.Center.isEqual(arcs[1].Curve.Center, 1e-6))
        for line in lines:
            self.assertAlmostEqual(line.Length, self.SLANT, places=6)
        centre = arcs[0].Curve.Center
        a, b = (l.CenterOfMass - centre for l in lines)
        self.assertAlmostEqual(a.getAngle(b), sweep * self.SIN, places=6)
        # the neutral surface keeps its area: as much flat as folded at K 0.5
        middle = 15 + k * T * self.COS
        self.assertAlmostEqual(flat.Volume, sweep * self.SLANT * T * middle, places=5)
        return flat

    def testFrustumFlat(self):
        loft = self.frustum()
        flat = self.checkFlat(loft, 0.5)
        self.assertAlmostEqual(flat.Volume, loft.Shape.Volume, places=5)

    def testFrustumFlatK(self):
        """The rule's K factor puts the neutral cone, which the flat keeps"""
        loft = self.frustum()
        loft.Rule.KFactor = 0.3
        self.Doc.recompute()
        self.assertAlmostEqual(self.cone(loft)["KFactor"], 0.3)
        self.checkFlat(loft, 0.3)

    def testCylinder(self):
        """The same size at both ends: a cylinder, laid flat a rectangle as
        long as it is high and as wide as its neutral fibre's turn"""
        loft = self.loft(self.round(15), self.round(15, 20, name="Top"))
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        cone = self.cone(loft)
        self.assertAlmostEqual(cone["SemiAngle"], 0)
        self.assertAlmostEqual(cone["Radius"], 15)
        sweep = 2 * math.pi - 0.1 / 15
        self.assertAlmostEqual(loft.Shape.Volume, sweep * 20 * (16**2 - 15**2) / 2, places=6)
        self.assertEqual(self.owners(loft), {"Bend1": 6})
        flat = loft.getSubObject("Flat")
        self.assertIsNotNone(flat)
        box = flat.BoundBox
        self.assertAlmostEqual(box.ZLength, 1, places=6)
        self.assertAlmostEqual(min(box.XLength, box.YLength), 20, places=6)
        self.assertAlmostEqual(max(box.XLength, box.YLength), sweep * 15.5, places=6)

    def testArcs(self):
        """Two arcs over the same half turn: no seam, the sheet over just
        that, its sketches on the outer skin (Inside)"""
        loft = self.loft(
            self.round(20, span=(0, 180)),
            self.round(10, 30, span=(0, 180), name="Top"),
            MaterialSide="Inside",
        )
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        self.assertAlmostEqual(self.cone(loft)["Angle"], 180)
        self.assertEqual(self.owners(loft), {"Bend1": 6})
        middle = 15 - self.COS / 2
        self.assertAlmostEqual(loft.Shape.Volume, math.pi * self.SLANT * middle, places=6)
        box = loft.Shape.BoundBox
        self.assertAlmostEqual(box.YMin, 0, places=6)
        self.assertAlmostEqual(box.YMax, 20, places=6)
        self.assertAlmostEqual(box.XMin, -20, places=6)
        self.assertAlmostEqual(box.XMax, 20, places=6)
        # the outer skin through the sketched arc
        self.assertAlmostEqual(loft.Shape.distToShape(Part.Vertex(V(0, 20, 0)))[0], 0, places=6)
        flat = loft.getSubObject("Flat")
        self.assertIsNotNone(flat)
        self.assertAlmostEqual(flat.Volume, loft.Shape.Volume, places=5)

    def testRefused(self):
        """Not about one axis, planes not parallel, arcs over different
        turns, both on one plane, no section (round to lines is the mixed
        step's)"""
        cases = {
            "off the axis": (self.round(20, name="A1"), self.round(10, 30, centre=(3, 0), name="B1")),
            "tilted": (self.round(20, name="A2"), self.round(10, 30, tilt=10, name="B2")),
            "other turns": (
                self.round(20, span=(0, 180), name="A3"),
                self.round(10, 30, span=(0, 90), name="B3"),
            ),
            "one plane": (self.round(20, name="A4"), self.round(10, 0, name="B4")),
            "no section": (self.round(20, name="A5"), None),
        }
        for what, (a, b) in cases.items():
            with self.subTest(what=what):
                loft = self.loft(a, b, name="L")
                self.assertIn("Invalid", loft.State)
                self.Body.removeObject(loft)
                self.Doc.removeObject(loft.Name)

    def testNoGapNoSeam(self):
        """A circle all round needs the rule's Gap for its seam"""
        loft = self.frustum()
        loft.Rule.Gap = 0
        self.Doc.recompute()
        self.assertIn("Invalid", loft.State)
        loft.Rule.Gap = 0.2
        self.Doc.recompute()
        self.assertNotIn("Invalid", loft.State)
        self.assertAlmostEqual(self.cone(loft)["Angle"], math.degrees(2 * math.pi - 0.02))

    def testSaveAndRestore(self):
        """The cone comes back with the file, and a recompute after makes the
        same"""
        loft = self.frustum()
        model = loft.SheetModel
        owners = self.owners(loft)
        volume = loft.Shape.Volume
        path = os.path.join(tempfile.mkdtemp(), "loft.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        self.Body = self.Doc.getObject("Body")
        loft = self.Doc.getObject("LoftedFlange")
        self.assertEqual(loft.SheetModel, model)
        self.assertEqual(self.owners(loft), owners)
        self.assertIsNotNone(loft.getSubObject("Flat"))
        loft.touch()
        self.Doc.recompute()
        self.assertAlmostEqual(loft.Shape.Volume, volume)
        self.assertEqual(loft.SheetModel, model)

    # -- lines to lines --

    def lines(self, points, z=0, name="Sketch", closed=True, start=0, reverse=False):
        """A sketch at height z of lines through points (x, y), closed
        unless asked, drawn from points[start], the other way round if asked"""
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        sketch.Placement = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
        pts = [V(x, y, 0) for x, y in points]
        if reverse:
            pts.reverse()
        if closed:
            pts = pts[start:] + pts[:start]
            pairs = [(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]
        else:
            pairs = [(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
        for a, b in pairs:
            sketch.addGeometry(Part.LineSegment(a, b))
        return sketch

    @staticmethod
    def square(half, cx=0, cy=0):
        """A square's corners counterclockwise, the bottom line first"""
        return [(cx - half, cy - half), (cx + half, cy - half), (cx + half, cy + half),
                (cx - half, cy + half)]

    def kinds(self, feature):
        count = {}
        for e in feature.SheetModel["Elements"]:
            count[e["Kind"]] = count.get(e["Kind"], 0) + 1
        return count

    def bends(self, feature):
        return [e for e in feature.SheetModel["Elements"] if e["Kind"] == "Bend"]

    def duct(self, side="Outside", **props):
        """A square of 40 to the same 30 up: four upright facets"""
        return self.loft(self.lines(self.square(20)),
                         self.lines(self.square(20), 30, name="Top"),
                         MaterialSide=side, **props)

    def flatOf(self, loft):
        flat = loft.getSubObject("Flat")
        self.assertIsNotNone(flat, loft.getStatusString())
        self.assertTrue(flat.isValid())
        return flat

    def checkStraight(self, flat):
        """Exact: every edge of the flat a straight line, whatever curve
        carries it"""
        for e in flat.Edges:
            a, b = e.Vertexes[0].Point, e.Vertexes[-1].Point
            chord = Part.LineSegment(a, b).toShape()
            for t in (0.25, 0.5, 0.75):
                p = e.valueAt(e.FirstParameter + t * (e.LastParameter - e.FirstParameter))
                self.assertLess(chord.distToShape(Part.Vertex(p))[0], 1e-7,
                                (e.Curve.TypeId, a, b))

    def testDuct(self):
        """A facet a wall along each line, a quarter bend of the rule's
        radius at each corner, the first wall split by the seam"""
        loft = self.duct()
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        self.assertTrue(loft.Shape.isValid())
        self.assertEqual(len(loft.Shape.Solids), 1)
        self.assertEqual(self.kinds(loft), {"Wall": 5, "Bend": 4})
        for b in self.bends(loft):
            self.assertAlmostEqual(b["Angle"], 90)
            self.assertAlmostEqual(b["Radius"], 1)
        self.assertEqual(self.owners(loft),
                         {"Wall1": 5, "Wall2": 4, "Wall3": 4, "Wall4": 4, "Wall5": 5,
                          "Bend1": 4, "Bend2": 4, "Bend3": 4, "Bend4": 4})
        # the sketches on the inner skin: 38 straight along each line, a
        # quarter annulus from 1 to 2 at each corner, 30 high, less the seam
        self.assertAlmostEqual(loft.Shape.Volume, 30 * (4 * 38 - 0.1 + 3 * math.pi), places=5)
        box = loft.Shape.BoundBox
        self.assertAlmostEqual(box.XMax, 21, places=6)
        self.assertAlmostEqual(box.ZMin, 0, places=6)
        self.assertAlmostEqual(box.ZMax, 30, places=6)
        # the seam across the middle of the first line, the Gap wide
        self.assertGreater(loft.Shape.distToShape(Part.Vertex(V(0, -20.5, 15)))[0], 0.04)
        self.assertAlmostEqual(loft.Shape.distToShape(Part.Vertex(V(0.1, -20.5, 15)))[0], 0)
        self.assertAlmostEqual(loft.Shape.distToShape(Part.Vertex(V(-0.1, -20.5, 15)))[0], 0)

    def testDuctInside(self):
        """The sketches on the outer skin"""
        loft = self.duct("Inside")
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        self.assertAlmostEqual(loft.Shape.Volume, 30 * (4 * 36 - 0.1 + 3 * math.pi), places=5)
        self.assertAlmostEqual(loft.Shape.BoundBox.XMax, 20, places=6)

    def checkDuctFlat(self, loft, k):
        """A rectangle as high as the duct, as long as its straight runs and
        its bends' neutral fibres"""
        flat = self.flatOf(loft)
        self.checkStraight(flat)
        box = flat.BoundBox
        self.assertAlmostEqual(box.ZLength, 1, places=6)
        self.assertAlmostEqual(min(box.XLength, box.YLength), 30, places=6)
        length = 4 * 38 - 0.1 + 4 * math.pi / 2 * (1 + k)
        self.assertAlmostEqual(max(box.XLength, box.YLength), length, places=6)
        self.assertAlmostEqual(flat.Volume, 30 * length, places=5)
        return flat

    def testDuctFlat(self):
        loft = self.duct()
        flat = self.checkDuctFlat(loft, 0.5)
        self.assertAlmostEqual(flat.Volume, loft.Shape.Volume, places=5)

    def testDuctFlatK(self):
        loft = self.duct()
        loft.Rule.KFactor = 0.3
        self.Doc.recompute()
        for b in self.bends(loft):
            self.assertAlmostEqual(b["KFactor"], 0.3)
        self.checkDuctFlat(loft, 0.3)

    def testOwnRadius(self):
        """The bends at the feature's own radius"""
        loft = self.duct(UseRuleRadius=False)
        loft.BendRadius = 3
        self.Doc.recompute()
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        for b in self.bends(loft):
            self.assertAlmostEqual(b["Radius"], 3)
        self.assertAlmostEqual(loft.Shape.Volume, 30 * (4 * 34 - 0.1 + 7 * math.pi), places=5)
        # too large for the lines
        loft.BendRadius = 25
        self.Doc.recompute()
        self.assertIn("Invalid", loft.State)

    def hopper(self, side="Outside", **kw):
        """A square of 60 to a rectangle of 30 by 20, 50 up"""
        top = [(-15, -10), (15, -10), (15, 10), (-15, 10)]
        return self.loft(self.lines(self.square(30)), self.lines(top, 50, name="Top", **kw),
                         MaterialSide=side)

    # each facet's normal out of the hopper, by the profile's line
    NORMALS = {
        (0, -30): V(0, -50, 20),
        (30, 0): V(50, 0, 15),
        (0, 30): V(0, 50, 20),
        (-30, 0): V(-50, 0, 15),
    }

    def checkHopper(self, loft, side):
        """Valid, its sketches on the skin asked, the material out of the
        hopper from them (Outside), into it (Inside), about them (Middle)"""
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        self.assertTrue(loft.Shape.isValid())
        self.assertEqual(len(loft.Shape.Solids), 1)
        self.assertEqual(self.kinds(loft), {"Wall": 5, "Bend": 4})
        out = {"Outside": 0.5, "Inside": -0.5, "Middle": 0}[side]
        shape = loft.Shape
        for (x, y), n in self.NORMALS.items():
            n = V(n).normalize()
            # a quarter along the line from its middle, off the seam
            e = V(-y, x, 0).normalize()
            rim = V(x, y, 0) + e * 7.5
            # up the facet, square to the line
            up = n.cross(e)
            at = rim + up * 5
            with self.subTest(line=(x, y)):
                self.assertTrue(shape.isInside(at + n * out, 1e-7, True))
                self.assertFalse(shape.isInside(at + n * (out + 0.6), 1e-7, True))
                self.assertFalse(shape.isInside(at + n * (out - 0.6), 1e-7, True))
                # the rim square to the sheet: through the sketched line
                # along the normal, the sheet on the facet's side of it only
                for s in (-0.45, 0, 0.45):
                    p = rim + n * (out + s)
                    self.assertTrue(shape.isInside(p + up * 1e-3, 1e-7, True))
                    self.assertFalse(shape.isInside(p - up * 1e-3, 1e-7, True))

    def testHopperOutside(self):
        self.checkHopper(self.hopper("Outside"), "Outside")

    def testHopperInside(self):
        self.checkHopper(self.hopper("Inside"), "Inside")

    def testHopperMiddle(self):
        self.checkHopper(self.hopper("Middle"), "Middle")

    def testHopperFlat(self):
        """Exact, straight all round, keeping the volume at K 0.5"""
        loft = self.hopper()
        flat = self.flatOf(loft)
        self.checkStraight(flat)
        self.assertAlmostEqual(flat.BoundBox.ZLength, 1, places=6)
        self.assertAlmostEqual(flat.Volume, loft.Shape.Volume, places=5)

    def testMatchedByDirection(self):
        """Each line of the profile with the section's line running its way:
        the section drawn from another line, or the other way round, makes
        the same"""
        volume = self.hopper().Shape.Volume
        for kw in ({"start": 2}, {"reverse": True}, {"start": 1, "reverse": True}):
            with self.subTest(**kw):
                loft = self.hopper(**kw)
                self.assertNotIn("Invalid", loft.State, loft.getStatusString())
                self.assertAlmostEqual(loft.Shape.Volume, volume, places=6)

    def testConcave(self):
        """An L to a smaller L: the bend at the inner corner turns the other
        way, its inner skin the sheet's outer one"""
        a = [(0, 0), (40, 0), (40, 20), (20, 20), (20, 40), (0, 40)]
        b = [(5, 5), (35, 5), (35, 15), (15, 15), (15, 35), (5, 35)]
        loft = self.loft(self.lines(a), self.lines(b, 30, name="Top"))
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        self.assertTrue(loft.Shape.isValid())
        self.assertEqual(len(loft.Shape.Solids), 1)
        self.assertEqual(self.kinds(loft), {"Wall": 7, "Bend": 6})
        flat = self.flatOf(loft)
        self.checkStraight(flat)
        self.assertAlmostEqual(flat.Volume, loft.Shape.Volume, places=5)

    def testOpen(self):
        """Open lines to open lines: no seam, a wall each, a bend between"""
        a = [(-20, 20), (-20, -20), (20, -20), (20, 20)]
        b = [(-10, 20), (-10, -10), (10, -10), (10, 20)]
        loft = self.loft(self.lines(a, closed=False),
                         self.lines(b, 30, name="Top", closed=False))
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        self.assertTrue(loft.Shape.isValid())
        self.assertEqual(self.kinds(loft), {"Wall": 3, "Bend": 2})
        self.assertEqual(self.owners(loft),
                         {"Wall1": 5, "Wall2": 4, "Wall3": 5, "Bend1": 4, "Bend2": 4})
        # the sheet outside the U (Outside): on the far side of its bottom
        self.assertTrue(loft.Shape.isInside(V(0, -20.5, 1), 1e-7, True))
        self.assertFalse(loft.Shape.isInside(V(0, -19.5, 1), 1e-7, True))
        flat = self.flatOf(loft)
        self.checkStraight(flat)
        self.assertAlmostEqual(flat.Volume, loft.Shape.Volume, places=5)

    def testRefusedLines(self):
        """Lines and an arc in one sketch, a different count of lines,
        closed lines to an arc"""
        tri = [(-20, -20), (20, -20), (0, 20)]
        mixed = self.Body.newObject("Sketcher::SketchObject", "Mixed")
        mixed.Placement = FreeCAD.Placement(V(0, 0, 30), FreeCAD.Rotation())
        mixed.addGeometry(Part.LineSegment(V(-10, 0, 0), V(10, 0, 0)))
        mixed.addGeometry(
            Part.ArcOfCircle(Part.Circle(V(0, 0, 0), V(0, 0, 1), 10), 0, math.pi))
        cases = {
            "mixed": (self.lines(self.square(20), name="A1"), mixed),
            "count": (self.lines(self.square(20), name="A2"), self.lines(tri, 30, name="B2")),
            "closed to open": (
                self.lines(self.square(20), name="A4"),
                self.round(10, 30, span=(0, 180), name="B4"),
            ),
        }
        for what, (a, b) in cases.items():
            with self.subTest(what=what):
                loft = self.loft(a, b, name="L")
                self.assertIn("Invalid", loft.State)
                self.Body.removeObject(loft)
                self.Doc.removeObject(loft.Name)

    # -- lines that do not run each other's way: triangles --

    @staticmethod
    def turned(radius, angle):
        """A square's corners on a circle, counterclockwise, turned by angle
        (degrees) from the bottom line along X"""
        return [(radius * math.cos(math.radians(a + angle)),
                 radius * math.sin(math.radians(a + angle)))
                for a in (-135, -45, 45, 135)]

    def checkSketchOnShape(self, loft, sketch):
        """Each sketched line on the sheet, but near its ends, where the
        bends round the corners off, and at its middle, where a seam may
        be"""
        for e in sketch.Shape.Edges:
            for t in (0.3, 0.7):
                p = e.valueAt(e.FirstParameter + t * (e.LastParameter - e.FirstParameter))
                with self.subTest(sketch=sketch.Name, at=p):
                    self.assertAlmostEqual(loft.Shape.distToShape(Part.Vertex(p))[0], 0,
                                           places=6)

    def checkExact(self, loft):
        self.assertNotIn("Invalid", loft.State, loft.getStatusString())
        self.assertTrue(loft.Shape.isValid())
        self.assertEqual(len(loft.Shape.Solids), 1)
        flat = self.flatOf(loft)
        self.checkStraight(flat)
        self.assertAlmostEqual(flat.BoundBox.ZLength, 1, places=6)
        self.assertAlmostEqual(flat.Volume, loft.Shape.Volume, places=5)

    def testTurned(self):
        """A square to a smaller one turned 30 deg: each line's facet two
        triangles and a shallow bend along the shorter diagonal, the seam
        along the first line's"""
        a = self.lines(self.turned(20 * math.sqrt(2), 0))
        b = self.lines(self.turned(10 * math.sqrt(2), 30), 30, name="Top")
        loft = self.loft(a, b)
        self.checkExact(loft)
        self.assertEqual(self.kinds(loft), {"Wall": 8, "Bend": 7})
        angles = sorted(e["Angle"] for e in self.bends(loft))
        # three diagonals (the fourth is the seam), four corners, their
        # turns together all round
        self.assertTrue(all(x < 40 for x in angles[:3]), angles)
        self.assertTrue(all(x > 50 for x in angles[3:]), angles)
        self.assertAlmostEqual(angles[0], angles[2], places=6)
        self.assertAlmostEqual(angles[3], angles[6], places=6)
        self.checkSketchOnShape(loft, a)
        self.checkSketchOnShape(loft, b)

    def testTurnedInside(self):
        a = self.lines(self.turned(20 * math.sqrt(2), 0))
        b = self.lines(self.turned(10 * math.sqrt(2), 30), 30, name="Top")
        loft = self.loft(a, b, MaterialSide="Inside")
        self.checkExact(loft)
        self.checkSketchOnShape(loft, a)
        self.checkSketchOnShape(loft, b)

    def testSomeParallel(self):
        """A square to a trapezoid: its bottom and top lines run the
        square's way, a facet each, the bottom one the seam's; its sides do
        not, two triangles each"""
        a = self.lines(self.square(20))
        b = self.lines([(-10, -10), (10, -10), (5, 10), (-5, 10)], 30, name="Top")
        loft = self.loft(a, b)
        self.checkExact(loft)
        self.assertEqual(self.kinds(loft), {"Wall": 7, "Bend": 6})
        self.checkSketchOnShape(loft, a)
        self.checkSketchOnShape(loft, b)

    def testOpenTwisted(self):
        """One line to one line across it: two triangles and their bend"""
        a = self.lines([(-20, 0), (20, 0)], closed=False)
        b = self.lines([(-10, 10), (10, -10)], 30, name="Top", closed=False)
        loft = self.loft(a, b)
        self.checkExact(loft)
        self.assertEqual(self.kinds(loft), {"Wall": 2, "Bend": 1})

    def testSaveAndRestoreTurned(self):
        """The bends' rims come back with the file: each bend still owns its
        own faces, and the flat closes"""
        a = self.lines(self.turned(20 * math.sqrt(2), 0))
        b = self.lines(self.turned(10 * math.sqrt(2), 30), 30, name="Top")
        loft = self.loft(a, b, MaterialSide="Inside")
        model = loft.SheetModel
        self.assertTrue(all("Rims" in e for e in self.bends(loft)))
        owners = self.owners(loft)
        path = os.path.join(tempfile.mkdtemp(), "turned.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        self.Body = self.Doc.getObject("Body")
        loft = self.Doc.getObject("LoftedFlange")
        self.assertEqual(loft.SheetModel, model)
        self.assertEqual(self.owners(loft), owners)
        self.assertIsNotNone(loft.getSubObject("Flat"))

    # -- mixed: round to lines --

    def squareToRound(self, side="Outside", **props):
        """A square of 60 to a circle of 20, 50 up"""
        self.squareSketch = self.lines(self.square(30))
        self.roundSketch = self.round(20, 50, name="Top")
        return self.loft(self.squareSketch, self.roundSketch, MaterialSide=side, **props)

    def checkRound(self, loft, radius, z, segments, start=0, notches=4):
        """The round's corners (Segments of them all round from its X) on
        the sheet but for the bends rounding them -- but where a line's
        triangle meets the round, two folds meeting there, a notch -- the
        chords between within their sagitta of the round"""
        sagitta = radius * (1 - math.cos(math.pi / segments))
        off = []
        for k in range(segments):
            a = start + 2 * math.pi * k / segments
            p = V(radius * math.cos(a), radius * math.sin(a), z)
            if loft.Shape.distToShape(Part.Vertex(p))[0] > 0.02:
                off.append(k)
            a += math.pi / segments
            p = V(radius * math.cos(a), radius * math.sin(a), z)
            with self.subTest(chord=k):
                self.assertLess(loft.Shape.distToShape(Part.Vertex(p))[0], sagitta + 0.02)
        self.assertLessEqual(len(off), notches, off)

    def testSquareToRound(self):
        """A fan of six triangles from each corner to its quarter of the
        round, a triangle on each line, a bend at every fold, the first
        line's triangle split by the seam; exact flat"""
        loft = self.squareToRound()
        self.checkExact(loft)
        kinds = self.kinds(loft)
        self.assertEqual(kinds["Bend"], 24 + 4)
        self.assertEqual(kinds["Wall"], kinds["Bend"] + 1)
        self.checkSketchOnShape(loft, self.squareSketch)
        self.checkRound(loft, 20, 50, 24)

    def checkRelieved(self, loft, sketch, clear):
        """A relief at each corner of the lines: the sheet clear of the
        corner by more than clear, on its lines still a tenth of their
        length from it"""
        for e in sketch.Shape.Edges:
            for v in e.Vertexes:
                with self.subTest(corner=v.Point):
                    self.assertGreater(loft.Shape.distToShape(v)[0], clear)
            for t in (0.1, 0.9):
                p = e.valueAt(e.FirstParameter + t * (e.LastParameter - e.FirstParameter))
                with self.subTest(at=p):
                    self.assertAlmostEqual(loft.Shape.distToShape(Part.Vertex(p))[0], 0,
                                           places=6)

    def testSquareToRoundInside(self):
        """The sheet inside the square: at each corner, where a fan of folds
        meets, a thick sheet cannot come to the point the square has -- a
        relief there, cut square to the sheet, so the flat is exact"""
        loft = self.squareToRound("Inside")
        self.checkExact(loft)
        self.checkRelieved(loft, self.squareSketch, 1)
        self.checkSketchOnShape(loft, self.squareSketch)
        self.checkRound(loft, 20, 50, 24)

    def testSquareToRoundMiddle(self):
        """The sheet about the square: a relief at each corner as well"""
        loft = self.squareToRound("Middle")
        self.checkExact(loft)
        self.checkRelieved(loft, self.squareSketch, 0.5)
        self.checkRound(loft, 20, 50, 24)

    def testRoundToSquareInside(self):
        """The lines the section: the reliefs at its corners"""
        top = self.lines(self.square(30), 50, name="Top")
        loft = self.loft(self.round(20), top, MaterialSide="Inside")
        self.checkExact(loft)
        self.checkRelieved(loft, top, 1)

    def testFewSegmentsInside(self):
        """Fewer chords, fewer folds at each corner, a relief still where
        they run into each other"""
        loft = self.squareToRound("Inside", Segments=8)
        self.checkExact(loft)
        self.checkSketchOnShape(loft, self.squareSketch)

    def testFewSegmentsMiddle(self):
        """Few chords about the square: the sheet runs into itself at the
        corners by a little only -- a relief all the same, or the flat does
        not close"""
        loft = self.squareToRound("Middle", Segments=8)
        self.checkExact(loft)
        self.checkRelieved(loft, self.squareSketch, 0.5)

    def testSmallRadiusInside(self):
        """A small radius: the triangle on each line comes to a point on the
        round short of it, a relief there too"""
        loft = self.squareToRound("Inside", UseRuleRadius=False, BendRadius=0.3)
        self.checkExact(loft)
        self.checkRelieved(loft, self.squareSketch, 1)

    def testSegments(self):
        """Fewer chords, fewer triangles"""
        loft = self.squareToRound(Segments=12)
        self.checkExact(loft)
        self.assertEqual(self.kinds(loft)["Bend"], 12 + 4)
        self.checkRound(loft, 20, 50, 12)

    def testRoundToSquare(self):
        """The round the profile, the lines the section"""
        loft = self.loft(self.round(20), self.lines(self.square(30), 50, name="Top"))
        self.checkExact(loft)
        self.assertEqual(self.kinds(loft)["Bend"], 24 + 4)

    def testArcToLines(self):
        """A half round to two lines, open: no seam, the arc in chords as
        many as its share of Segments"""
        arc = self.round(20, span=(0, 180))
        top = self.lines([(25, 0), (0, 25), (-25, 0)], 30, name="Top", closed=False)
        loft = self.loft(arc, top)
        self.checkExact(loft)
        kinds = self.kinds(loft)
        self.assertEqual(kinds["Bend"], 12 + 2 - 1)
        self.assertEqual(kinds["Wall"], kinds["Bend"] + 1)
