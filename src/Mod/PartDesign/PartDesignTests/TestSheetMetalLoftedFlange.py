# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's lofted flange, PartDesign::SheetMetalLoftedFlange
# (docs/SheetMetal.md sec 5.1, M5), its first step: between two circles or
# arcs on parallel planes about one axis, a cone frustum (a cylinder where
# they are the same size), one conical bend of the sheet model, laid flat
# exactly. A circle all round leaves a seam the rule's Gap wide.

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
        turns, an outline not round, both on one plane, no section"""
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
        square = self.Body.newObject("Sketcher::SketchObject", "Square")
        square.Placement = FreeCAD.Placement(V(0, 0, 30), FreeCAD.Rotation())
        pts = [V(-5, -5, 0), V(5, -5, 0), V(5, 5, 0), V(-5, 5, 0)]
        for i in range(4):
            square.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % 4]))
        cases["not round"] = (self.round(20, name="A6"), square)
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
