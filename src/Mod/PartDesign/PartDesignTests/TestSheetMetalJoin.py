# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's bend between walls, PartDesign::SheetMetalJoin (docs/SheetMetal.md
# sec 5.1, ruled 2026-11-03): two edges, one on each of two walls not joined
# yet, joined by a bend of the rule's radius, its inner skin touching both
# walls' inner skins; each wall extended or trimmed to where the bend starts,
# over the length the two edges share. A plate 40 x 20, 1 thick, and a second
# base wall in the same body; a radius of 1 and K 0.5, so the blank's area is
# the part's volume.

import math
import unittest

import FreeCAD
import Part

from . import SheetMetalNames
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


# the plate's edges at its right end, top and bottom, and at its left
RIGHT = ((40, 0, 1), (40, 20, 1))
RIGHT_BOTTOM = ((40, 0, 0), (40, 20, 0))
LEFT = ((0, 0, 1), (0, 20, 1))


class TestSheetMetalJoin(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalJoin")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.Plate = self.wall(V(0, 0, 0), V(1, 0, 0), V(0, 1, 0), 40, 20)

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def wall(self, origin, xdir, ydir, w, h):
        """A base wall w x h on the sketch at origin along xdir and ydir, its
        material on the side of xdir x ydir"""
        s = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        s.MapMode = "Deactivated"
        s.Placement = FreeCAD.Placement(
            origin, FreeCAD.Rotation(xdir, ydir, xdir.cross(ydir), "ZXY")
        )
        pts = [(0, 0), (w, 0), (w, h), (0, h)]
        for i in range(4):
            p, q = pts[i], pts[(i + 1) % 4]
            s.addGeometry(Part.LineSegment(V(p[0], p[1], 0), V(q[0], q[1], 0)))
        f = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Wall")
        f.Profile = s
        self.Doc.recompute()
        self.assertNotIn("Invalid", f.State)
        return f

    def upright(self, x, z, y=0, w=20, h=20):
        """A wall standing on the plane x, its material from x to x + 1, its
        bottom end at z, from y across w"""
        return self.wall(V(x, y, z), V(0, 1, 0), V(0, 0, 1), w, h)

    def join(self, *edges, base=None, **props):
        base = base or self.Body.Tip
        j = self.Body.newObject("PartDesign::SheetMetalJoin", "Join")
        j.Edges = (base, [edgeOf(base, *e) for e in edges])
        for k, v in props.items():
            setattr(j, k, v)
        self.Doc.recompute()
        return j

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
        bb = shape.optimalBoundingBox()
        for k, v in box.items():
            self.assertAlmostEqual(getattr(bb, k), v, places=6, msg=k)

    def checkJoined(self, join, volume):
        """Valid, one solid of the volume, and its blank the same area, K
        being 0.5"""
        self.checkValid(join)
        self.assertAlmostEqual(join.Shape.Volume, volume, places=6)
        flat = self.flat(join)
        self.assertEqual(flat.BendCount, 1)
        self.assertAlmostEqual(flat.FlatArea.Value, volume, places=5)

    def testUpright(self):
        """A wall standing at x 49 to 50, its end at z 10: the bend's centre
        at x 48, z 2; the plate extended 8 to x 48, the wall 8 down to z 2"""
        self.upright(49, 10)
        j = self.join(RIGHT, ((49, 0, 10), (49, 20, 10)))
        self.checkJoined(j, 800 + 400 + 8 * 20 + 8 * 20 + QUARTER * 20)
        self.checkBox(j.Shape, XMin=0, XMax=50, ZMin=0, ZMax=30)
        model = j.SheetModel["Elements"]
        self.assertEqual([e["Kind"] for e in model], ["Wall", "Wall", "Bend"])
        bend = model[2]
        self.assertEqual((bend["Before"], bend["After"]), (model[0]["Name"], model[1]["Name"]))
        self.assertAlmostEqual(bend["Angle"], 90, places=9)
        self.assertAlmostEqual(bend["Radius"], 1, places=9)

    def testOwnRadius(self):
        """The join's own radius, 3: the bend's centre at x 46, z 4; the
        plate extended 6 to x 46, the wall 6 down to z 4, a quarter ring of
        3 to 4, 7 pi / 4 in section and as long flat"""
        self.upright(49, 10)
        tip = self.Body.Tip
        j = self.Body.newObject("PartDesign::SheetMetalJoin", "Join")
        j.Edges = (tip, [edgeOf(tip, *RIGHT), edgeOf(tip, (49, 0, 10), (49, 20, 10))])
        j.UseRuleRadius = False
        j.BendRadius = 3
        self.Doc.recompute()
        self.checkJoined(j, 800 + 400 + 6 * 20 + 6 * 20 + math.pi / 4 * 7 * 20)
        self.checkBox(j.Shape, XMin=0, XMax=50, ZMin=0, ZMax=30)
        self.assertAlmostEqual(j.SheetModel["Elements"][2]["Radius"], 3, places=9)

    def testEitherSkin(self):
        """Either skin's edge at a wall's end picks the same end"""
        self.upright(49, 10)
        j = self.join(RIGHT_BOTTOM, ((50, 0, 10), (50, 20, 10)))
        self.checkJoined(j, 800 + 400 + 8 * 20 + 8 * 20 + QUARTER * 20)
        self.checkBox(j.Shape, XMax=50, ZMax=30)

    def testOrder(self):
        """The first edge's wall is the bend's before"""
        self.upright(49, 10)
        j = self.join(((49, 0, 10), (49, 20, 10)), RIGHT)
        self.checkJoined(j, 800 + 400 + 8 * 20 + 8 * 20 + QUARTER * 20)
        model = j.SheetModel["Elements"]
        self.assertEqual(
            (model[2]["Before"], model[2]["After"]), (model[1]["Name"], model[0]["Name"])
        )

    def testTrimmed(self):
        """A wall standing at x 39 to 40 over the plate's end, its end at
        z 5: the plate trimmed back 2 to x 38, the wall extended 3 to z 2"""
        self.upright(39, 5)
        j = self.join(RIGHT, ((39, 0, 5), (39, 20, 5)))
        self.checkJoined(j, 800 - 2 * 20 + 400 + 3 * 20 + QUARTER * 20)
        self.checkBox(j.Shape, XMin=0, XMax=40, ZMin=0, ZMax=25)
        self.assertFalse(j.Shape.isInside(V(39.5, 10, 0.5), 1e-7, True))
        self.assertTrue(j.Shape.isInside(V(37.5, 10, 0.5), 1e-7, True))

    def testAngled(self):
        """A wall rising at 45 deg, its inner skin touching the bend 5 short
        of its end, the bend's centre at x 45, z 2: a 45 deg bend, the plate
        extended 5"""
        r = math.sqrt(0.5)
        e = V(r, 0, r)
        end = V(45 + r, 0, 2 - r) + e * 5
        self.wall(end, V(0, 1, 0), e, 20, 15)
        j = self.join(RIGHT, ((end.x, 0, end.z), (end.x, 20, end.z)))
        self.checkJoined(j, 800 + 5 * 20 + QUARTER / 2 * 20 + 5 * 20 + 300)
        self.assertAlmostEqual(j.SheetModel["Elements"][2]["Angle"], 45, places=9)

    def testNarrower(self):
        """A wall 10 wide over y 5 to 15: the bend and both extensions that
        wide, the plate's end beside them as it was"""
        self.upright(49, 10, y=5, w=10)
        j = self.join(RIGHT, ((49, 5, 10), (49, 15, 10)))
        self.checkJoined(j, 800 + 200 + 8 * 10 + 8 * 10 + QUARTER * 10)
        self.assertFalse(j.Shape.isInside(V(44, 2, 0.5), 1e-7, True))
        self.assertTrue(j.Shape.isInside(V(44, 10, 0.5), 1e-7, True))

    def testNarrowerTrimmed(self):
        """Trimmed under a narrower wall: the plate's end beside it kept,
        the bend relief between"""
        self.upright(39, 5, y=5, w=10)
        j = self.join(RIGHT, ((39, 5, 5), (39, 15, 5)))
        self.checkValid(j)
        self.assertEqual(self.flat(j).BendCount, 1)
        self.assertFalse(j.Shape.isInside(V(39.5, 10, 0.5), 1e-7, True))
        self.assertTrue(j.Shape.isInside(V(39.5, 0.5, 0.5), 1e-7, True))
        self.assertTrue(j.Shape.isInside(V(39.5, 19.5, 0.5), 1e-7, True))

    def testWiderTrimmed(self):
        """A wall 30 wide over y -5 to 25, its end at z 1.5: both trimmed,
        the plate 2, the wall 0.5 over the 20 they share; the wall's part
        beside the bend kept, and the rule's relief there, 1 wide and 1 past
        where the bend starts"""
        self.upright(39, 1.5, y=-5, w=30)
        j = self.join(RIGHT, ((39, -5, 1.5), (39, 25, 1.5)))
        self.checkJoined(j, 800 - 2 * 20 + 600 - 0.5 * 20 - 2 * 1.5 + QUARTER * 20)
        self.assertTrue(j.Shape.isInside(V(39.5, -3, 1.75), 1e-7, True))
        self.assertTrue(j.Shape.isInside(V(39.5, 23, 1.75), 1e-7, True))

    def refused(self, *edges):
        j = self.join(*edges)
        self.assertIn("Invalid", j.State)
        self.Doc.removeObject(j.Name)
        self.Doc.recompute()

    # Parallel walls (built 2026-11-11): two bends of 90 deg at the rule's
    # radius and a run between them, across from the one wall to the other.
    # The first edge's wall stays as it is, its bend starting at its end; the
    # second is extended or trimmed to where its bend starts. The walls'
    # facing skins are the bends' inner ones, 2 R apart at least: at just
    # that, the one bend of a half turn.

    def above(self, x, z, w=30):
        """A plate parallel to the first, from x across w, its bottom skin at z"""
        return self.wall(V(x, 0, z), V(1, 0, 0), V(0, 1, 0), w, 20)

    def checkParallel(self, join, volume, bends=2):
        self.checkValid(join)
        self.assertAlmostEqual(join.Shape.Volume, volume, places=6)
        flat = self.flat(join)
        self.assertEqual(flat.BendCount, bends)
        self.assertAlmostEqual(flat.FlatArea.Value, volume, places=5)

    def testParallelFacing(self):
        """A plate above, its end facing the first's across a gap of 10: the
        run stands at x 41 to 42 from z 2 to 9, the upper plate extended 7 to
        x 43"""
        self.above(50, 10)
        j = self.join(RIGHT, ((50, 0, 10), (50, 20, 10)))
        self.checkParallel(j, 800 + 600 + 7 * 20 + 7 * 20 + 2 * QUARTER * 20)
        self.checkBox(j.Shape, XMin=0, XMax=80, ZMin=0, ZMax=11)
        shape = j.Shape
        self.assertTrue(shape.isInside(V(41.5, 10, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(46, 10, 10.5), 1e-9, True))
        self.assertFalse(shape.isInside(V(45, 10, 0.5), 1e-9, True))
        model = j.SheetModel["Elements"]
        self.assertEqual(sorted(e["Kind"] for e in model), ["Bend", "Bend", "Wall", "Wall", "Wall"])
        bends = [e for e in model if e["Kind"] == "Bend"]
        for bend in bends:
            self.assertAlmostEqual(bend["Angle"], 90, places=9)
            self.assertAlmostEqual(bend["Radius"], 1, places=9)
        run = [e["Name"] for e in model if e["Kind"] == "Wall"][2]
        self.assertEqual(
            [(b["Before"], b["After"]) for b in bends],
            [(model[0]["Name"], run), (run, model[1]["Name"])],
        )

    def testParallelFacingEitherSkin(self):
        """The edges on the walls' far skins: the same"""
        self.above(50, 10)
        j = self.join(RIGHT_BOTTOM, ((50, 0, 11), (50, 20, 11)))
        self.checkParallel(j, 800 + 600 + 7 * 20 + 7 * 20 + 2 * QUARTER * 20)

    def testParallelFacingOrder(self):
        """The upper plate's edge first: it stays, the run at x 48 to 49, the
        first plate extended 7 to x 47"""
        self.above(50, 10)
        j = self.join(((50, 0, 10), (50, 20, 10)), RIGHT)
        self.checkParallel(j, 800 + 600 + 7 * 20 + 7 * 20 + 2 * QUARTER * 20)
        shape = j.Shape
        self.assertTrue(shape.isInside(V(48.5, 10, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(45, 10, 0.5), 1e-9, True))
        self.assertFalse(shape.isInside(V(46, 10, 10.5), 1e-9, True))

    def testParallelFacingTrimmed(self):
        """The upper plate from x 42, past where its bend starts at 43:
        trimmed 1"""
        self.above(42, 10)
        j = self.join(RIGHT, ((42, 0, 10), (42, 20, 10)))
        self.checkParallel(j, 800 + 600 - 1 * 20 + 7 * 20 + 2 * QUARTER * 20)

    def testParallelStacked(self):
        """A plate above ending where the first does, both ends the same
        way: a U, its run at x 41 to 42"""
        self.above(10, 10)
        j = self.join(RIGHT, ((40, 0, 10), (40, 20, 10)))
        self.checkParallel(j, 800 + 600 + 7 * 20 + 2 * QUARTER * 20)
        self.checkBox(j.Shape, XMin=0, XMax=42, ZMin=0, ZMax=11)

    def testParallelStackedShorter(self):
        """The upper plate ending 5 short of the first: extended 5"""
        self.above(5, 10)
        j = self.join(RIGHT, ((35, 0, 10), (35, 20, 10)))
        self.checkParallel(j, 800 + 600 + 5 * 20 + 7 * 20 + 2 * QUARTER * 20)
        self.checkBox(j.Shape, XMax=42)

    def testParallelHalfTurn(self):
        """The facing skins 2 apart, twice the radius: one bend of a half
        turn, no run"""
        self.above(10, 3)
        j = self.join(RIGHT, ((40, 0, 3), (40, 20, 3)))
        self.checkParallel(j, 800 + 600 + 2 * QUARTER * 20, bends=1)
        model = j.SheetModel["Elements"]
        self.assertEqual([e["Kind"] for e in model], ["Wall", "Wall", "Bend"])
        self.assertAlmostEqual(model[2]["Angle"], 180, places=9)
        self.checkBox(j.Shape, XMax=42, ZMax=4)

    def testParallelNarrower(self):
        """The upper plate half as wide, at y 5 to 15: the bends and the run
        10 wide, the first plate's end as it was beside them"""
        self.wall(V(50, 5, 10), V(1, 0, 0), V(0, 1, 0), 30, 10)
        j = self.join(RIGHT, ((50, 5, 10), (50, 15, 10)))
        self.checkParallel(j, 800 + 300 + 7 * 10 + 7 * 10 + 2 * QUARTER * 10)
        self.assertFalse(j.Shape.isInside(V(41.5, 2, 5), 1e-9, True))

    def testRefused(self):
        """One edge; walls in one plane; parallel walls nearer than two
        bends take; walls already joined; a trim taking all of the wall;
        edges sharing no length"""
        self.upright(49, 10)
        self.refused(RIGHT)
        self.refused(RIGHT, LEFT)
        self.refused(((49, 0, 10), (49, 20, 10)), ((50, 0, 30), (50, 20, 30)))
        # a plate beside the first, in its plane
        self.wall(V(55, 0, 0), V(1, 0, 0), V(0, 1, 0), 20, 20)
        self.refused(RIGHT, ((55, 0, 1), (55, 20, 1)))
        # a plate above, its bottom skin 1.5 above the first's top
        self.above(55, 2.5, w=20)
        self.refused(RIGHT, ((55, 0, 2.5), (55, 20, 2.5)))
        # beyond the plate's left end: the plate trimmed back 47
        self.upright(-6, 10)
        self.refused(RIGHT, ((-6, 0, 10), (-6, 20, 10)))
        # beside the plate, along y
        self.upright(49, 12, y=30)
        self.refused(RIGHT, ((49, 30, 12), (49, 50, 12)))

    def testRefusedJoinedByFlange(self):
        """A flange's leg is the plate's sheet already: a loop needs a rip"""
        f = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        f.Edges = (self.Plate, [edgeOf(self.Plate, *RIGHT)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", f.State)
        # the leg's end, at its top
        bb = f.Shape.BoundBox
        self.refused(LEFT, ((bb.XMax, 0, bb.ZMax), (bb.XMax, 20, bb.ZMax)))


if __name__ == "__main__":
    unittest.main()
