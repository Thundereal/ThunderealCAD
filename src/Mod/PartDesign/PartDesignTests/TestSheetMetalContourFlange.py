# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's contour flange, PartDesign::SheetMetalContourFlange
# (docs/SheetMetal.md sec 5.1, ruled 2026-11-02): an open chain of lines
# sketched square to an edge, starting on it, swept along the edge, with a
# bend at the edge and at every corner. The sketch is the skin the edge is
# on, carried on: the material lies on the side of its lines the wall's
# does. On a plate 40 x 20, 1 thick, a radius of 1 and K 0.5.

import math
import unittest

import FreeCAD
import Part

from .SheetMetalFlatView import Flat

V = FreeCAD.Vector

# a quarter bend of radius 1 and thickness 1: its volume per unit width,
# and its developed length at K 0.5 -- the same
QUARTER = math.pi / 4 * (2**2 - 1**2)


def edgeOf(feature, a, b):
    """The edge of a feature's shape on the line from a to b, the longest"""
    best, length = None, 0
    pa, pb = V(*a), V(*b)
    along = (pb - pa).normalize()
    for i, e in enumerate(feature.Shape.Edges):
        if len(e.Vertexes) != 2 or not hasattr(e.Curve, "Direction"):
            continue
        p, q = e.Vertexes[0].Point, e.Vertexes[1].Point
        on = all(((v - pa) - along * (v - pa).dot(along)).Length < 1e-7 for v in (p, q))
        if on and e.Length > length:
            best, length = "Edge%d" % (i + 1), e.Length
    if best is None:
        raise ValueError("no edge on %s %s" % (a, b))
    return best


# the plate's top edge at its right end, and at its back
RIGHT = ((40, 0, 1), (40, 20, 1))
BACK = ((0, 20, 1), (40, 20, 1))
RIGHT_BOTTOM = ((40, 0, 0), (40, 20, 0))


class TestSheetMetalContourFlange(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalContourFlange")
        self.Body, self.Plate = self.plate()

    def plate(self):
        """A body with the plate"""
        body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            p, q = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(p[0], p[1], 0), V(q[0], q[1], 0)))
        plate = body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        plate.Profile = sketch
        self.Doc.recompute()
        return body, plate

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def sketch(self, points, y=10, turn=0, arc=None, closed=False):
        """A chain of lines through points (x, z) on the plane y, square to
        the plate's right edge; turned by turn degrees about the line x 40
        upright; an arc (center, radius, a0, a1) in place of the lines if
        given"""
        s = self.Body.newObject("Sketcher::SketchObject", "ContourSketch")
        s.MapMode = "Deactivated"
        rot = FreeCAD.Rotation(V(1, 0, 0), V(0, 0, 1), V(0, -1, 0), "ZXY")
        about = FreeCAD.Placement(V(40, y, 0), FreeCAD.Rotation(V(0, 0, 1), turn))
        s.Placement = about.multiply(FreeCAD.Placement(V(-40, 0, 0), rot))
        if arc:
            c, r, a0, a1 = arc
            s.addGeometry(
                Part.ArcOfCircle(Part.Circle(V(c[0], c[1], 0), V(0, 0, 1), r), a0, a1)
            )
        pts = list(points) + ([points[0]] if closed else [])
        for p, q in zip(pts, pts[1:]):
            s.addGeometry(Part.LineSegment(V(p[0], p[1], 0), V(q[0], q[1], 0)))
        self.Doc.recompute()
        return s

    def contour(self, sketch, *edges, base=None, **props):
        base = base or self.Plate
        f = self.Body.newObject("PartDesign::SheetMetalContourFlange", "ContourFlange")
        f.Edges = (base, [edgeOf(base, *e) for e in edges])
        f.Profile = sketch
        for k, v in props.items():
            setattr(f, k, v)
        self.Doc.recompute()
        return f

    def flange(self, *edges, base=None, **props):
        """A flange on the plate in a body of its own, or on base in this one"""
        body = self.Body
        if base is None:
            body, base = self.plate()
        f = body.newObject("PartDesign::SheetMetalFlange", "Flange")
        f.Edges = (base, [edgeOf(base, *e) for e in edges])
        for k, v in props.items():
            setattr(f, k, v)
        self.Doc.recompute()
        self.assertNotIn("Invalid", f.State)
        return f

    def checkValid(self, feature):
        self.assertNotIn("Invalid", feature.State)
        self.assertTrue(feature.Shape.isValid())
        self.assertEqual(len(feature.Shape.Solids), 1)
        self.assertNotIn(
            None,
            [feature.sheetElementOf("Face%d" % (i + 1)) for i in range(len(feature.Shape.Faces))],
        )

    def flat(self, feature):
        self.Doc.recompute()
        flat = Flat(feature)
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(len(flat.Shape.Solids), 1)
        self.assertTrue(flat.Shape.Solids[0].isValid())
        return flat

    def checkBox(self, shape, **box):
        # optimal: a folded corner relief's faces loosen the quick one
        bb = shape.optimalBoundingBox()
        for k, v in box.items():
            self.assertAlmostEqual(getattr(bb, k), v, places=6, msg=k)

    def checkSame(self, contour, flange):
        """A contour flange the same part as a flange, and its flat"""
        self.checkValid(contour)
        self.assertAlmostEqual(contour.Shape.Volume, flange.Shape.Volume, places=6)
        a, b = contour.Shape.optimalBoundingBox(), flange.Shape.optimalBoundingBox()
        for k in ("XMin", "XMax", "YMin", "YMax", "ZMin", "ZMax"):
            self.assertAlmostEqual(getattr(a, k), getattr(b, k), places=6, msg=k)
        self.assertAlmostEqual(
            contour.Shape.common(flange.Shape).Volume, flange.Shape.Volume, places=4
        )
        fa, fb = self.flat(contour), self.flat(flange)
        self.assertEqual(fa.BendCount, fb.BendCount)
        self.assertAlmostEqual(fa.FlatArea.Value, fb.FlatArea.Value, places=5)

    def testUp(self):
        """One line up from the top edge: the leg's inner skin where the wall
        ended, its length from the inner sharp -- the flange's
        MaterialOutside, InnerSharp"""
        c = self.contour(self.sketch([(40, 1), (40, 11)]), RIGHT)
        self.checkBox(c.Shape, XMax=41, ZMax=11)
        f = self.flange(RIGHT, BendPosition="MaterialOutside", LengthType="InnerSharp", Length=10)
        self.checkSame(c, f)

    def testDown(self):
        """One line down from the top edge: the leg's outer skin where the
        wall ended, from the outer sharp -- MaterialInside, OuterSharp,
        bent the other way"""
        c = self.contour(self.sketch([(40, 1), (40, -9)]), RIGHT)
        self.checkBox(c.Shape, XMax=40, ZMin=-9)
        f = self.flange(
            RIGHT,
            Reversed=True,
            BendPosition="MaterialInside",
            LengthType="OuterSharp",
            Length=10,
        )
        self.checkSame(c, f)

    def testFromOtherSkin(self):
        """Drawn up from the bottom edge: the outer sharp there, the leg's
        outer skin where the wall ended"""
        c = self.contour(self.sketch([(40, 0), (40, 10)]), RIGHT_BOTTOM)
        self.checkBox(c.Shape, XMax=40, ZMax=10)
        f = self.flange(
            RIGHT_BOTTOM,
            Reversed=True,
            BendPosition="MaterialInside",
            LengthType="OuterSharp",
            Length=10,
        )
        self.checkSame(c, f)

    def testEitherEnd(self):
        """The chain drawn from its far end: the end on the edge is its start"""
        c = self.contour(self.sketch([(40, 11), (40, 1)]), RIGHT)
        self.checkBox(c.Shape, XMax=41, ZMax=11)
        self.checkValid(c)

    def testChain(self):
        """Up 10 then out 10: two bends, the wall cut back by the first's
        radius; the straight parts 39, 7 and 8"""
        c = self.contour(self.sketch([(40, 1), (40, 11), (50, 11)]), RIGHT)
        self.checkValid(c)
        self.checkBox(c.Shape, XMin=0, XMax=50, ZMin=0, ZMax=11)
        area = 20 * (39 + 7 + 8 + 2 * QUARTER)
        self.assertAlmostEqual(c.Shape.Volume, area, places=6)
        flat = self.flat(c)
        self.assertEqual(flat.BendCount, 2)
        self.assertAlmostEqual(flat.FlatArea.Value, area, places=5)

    def testStraightOn(self):
        """A first line straight on from the wall carries the wall on to
        the first bend -- the flange's Offset"""
        c = self.contour(self.sketch([(40, 1), (45, 1), (45, 11)]), RIGHT)
        self.checkBox(c.Shape, XMax=46, ZMax=11)
        f = self.flange(
            RIGHT, BendPosition="MaterialOutside", LengthType="InnerSharp", Length=10, Offset=5
        )
        self.checkSame(c, f)

    def testGaps(self):
        """Gaps at both ends: the wall cut back under it only, with the
        rule's relief, as the flange's"""
        c = self.contour(self.sketch([(40, 1), (40, 11)]), RIGHT, Gap1=5, Gap2=5)
        # the sketch is the profile, but the gaps are still the user's
        for name in ("Gap1", "Gap2"):
            self.assertNotIn("ReadOnly", c.getPropertyStatus(name), msg=name)
        self.checkBox(c.Shape, YMin=0, YMax=20)
        f = self.flange(
            RIGHT,
            BendPosition="MaterialOutside",
            LengthType="InnerSharp",
            Length=10,
            Gap1=5,
            Gap2=5,
        )
        self.checkSame(c, f)

    def testSeveral(self):
        """On two edges meeting at a corner: the same profile on each, mitered
        as the flange's"""
        c = self.contour(self.sketch([(40, 1), (40, 11)]), RIGHT, BACK)
        self.checkBox(c.Shape, XMax=41, YMax=21, ZMax=11)
        f = self.flange(
            RIGHT, BACK, BendPosition="MaterialOutside", LengthType="InnerSharp", Length=10
        )
        self.checkSame(c, f)

    def testSeveralChain(self):
        """A chain on two edges: each side's every bend and wall mitered"""
        c = self.contour(self.sketch([(40, 1), (40, 11), (50, 11)]), RIGHT, BACK)
        self.checkValid(c)
        self.checkBox(c.Shape, XMax=50, YMax=30, ZMax=11)
        flat = self.flat(c)
        self.assertEqual(flat.BendCount, 4)

    def testOnContour(self):
        """A contour flange on the end of another's last wall"""
        c = self.contour(self.sketch([(40, 1), (40, 11)]), RIGHT)
        self.checkValid(c)
        # the leg's outer skin is x 41, its top end at z 11: a line out
        # along +x from there, in the sketch's plane
        top = ((41, 0, 11), (41, 20, 11))
        d = self.contour(self.sketch([(41, 11), (51, 11)]), top, base=c)
        self.checkValid(d)
        self.checkBox(d.Shape, XMax=51, ZMax=12)
        self.assertEqual(self.flat(d).BendCount, 2)

    def refused(self, sketch, *edges, **props):
        c = self.contour(sketch, *(edges or (RIGHT,)), **props)
        self.assertIn("Invalid", c.State)
        self.Doc.removeObject(c.Name)
        self.Doc.recompute()

    def testRefused(self):
        """An arc; a closed sketch; a sketch not starting on the edge, or not
        square to it; a line too short for its bends; a chain running into
        the sheet"""
        self.refused(self.sketch([], arc=((40, 6), 5, -math.pi / 2, 0)))
        self.refused(self.sketch([(40, 1), (40, 11), (50, 11)], closed=True))
        self.refused(self.sketch([(35, 1), (35, 11)]))
        self.refused(self.sketch([(40, 1), (40, 11), (50, 11)], turn=30))
        self.refused(self.sketch([(40, 1), (40, 2), (50, 2)]))
        self.refused(self.sketch([(40, 1), (40, 6), (20, 6), (20, -4)]))

    def testRefusedAgainstEarlier(self):
        """Meeting an earlier flange at a corner, with no gap there"""
        f = self.flange(BACK, base=self.Plate)
        top = ((40, 0, 1), (40, 19, 1))
        c = self.contour(self.sketch([(40, 1), (40, 11)]), top, base=f)
        self.assertIn("Invalid", c.State)


if __name__ == "__main__":
    unittest.main()
