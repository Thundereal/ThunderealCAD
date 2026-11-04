# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's convert (docs/SheetMetal.md sec 5.3, ruled 2026-11-04):
# PartDesign::SheetMetalConvert on a base no sheet metal feature made -- an
# imported or a Part solid -- a fixed face picked, the wall the flat lies on.
# A thin part: its walls and bends found from its geometry (sec 4.2 way 3),
# each bend its own radius, the shape kept but for faces split where a bend
# meets a wall, so each lies in one element. M1's parts written to STEP and
# read back give the flat their features give. A solid or a shell: thickened
# by the rule's thickness, inside or outside, its sharp edges bends of the
# rule's radius but where ripped, a ripped corner relieved.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part
import PartDesign

from .SheetMetalFlatView import Flat

V = FreeCAD.Vector
# a 90 deg bend's allowance at R 1, T 1, K 0.5
BA = (1 + 0.5) * math.pi / 2


class TestSheetMetalConvert(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalConvert")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def body(self, shape, name="Imported"):
        """A body built on a Part feature holding shape"""
        base = self.Doc.addObject("Part::Feature", name)
        base.Shape = shape
        body = self.Doc.addObject("PartDesign::Body", "Body")
        body.BaseFeature = base
        self.Doc.recompute()
        return body

    def face(self, shape, test):
        """The name of shape's largest face passing test"""
        found = [(f.Area, "Face%d" % (i + 1)) for i, f in enumerate(shape.Faces) if test(f)]
        self.assertTrue(found)
        return max(found)[1]

    def convert(self, body, fixed):
        convert = body.newObject("PartDesign::SheetMetalConvert", "Convert")
        convert.FixedFace = (body.BaseFeature, [fixed])
        self.Doc.recompute()
        return convert

    def checkValid(self, feature):
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())
        self.assertEqual(len(feature.Shape.Solids), 1)
        owners = [
            feature.sheetElementOf("Face%d" % (i + 1)) for i in range(len(feature.Shape.Faces))
        ]
        self.assertNotIn(None, owners)

    def checkFlat(self, feature, length, width, area=None, bends=None):
        self.Doc.recompute()
        flat = Flat(feature)
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(len(flat.Shape.Solids), 1)
        self.assertTrue(flat.Shape.Solids[0].isValid())
        self.assertAlmostEqual(flat.FlatLength.Value, length, places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, width, places=6)
        if area is not None:
            self.assertAlmostEqual(flat.FlatArea.Value, area, places=4)
        if bends is not None:
            self.assertEqual(flat.BendCount, bends)
        return flat

    def stepped(self, shape):
        """shape written to STEP and read back"""
        path = os.path.join(tempfile.mkdtemp(), "part.step")
        shape.exportStep(path)
        return Part.read(path)

    def featureFlat(self):
        """M1's part made by features, in a document of its own: a plate 40 x
        20 with a hole, a flange on its far edge; its shape and its flat's
        size, area and bends"""
        doc = FreeCAD.newDocument("PartDesignTestSheetMetalConvertFeatures")
        try:
            body = doc.addObject("PartDesign::Body", "Body")
            sketch = body.newObject("Sketcher::SketchObject", "Sketch")
            pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
            for i in range(4):
                a, b = pts[i], pts[(i + 1) % 4]
                sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
            plate = body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
            plate.Profile = sketch
            doc.recompute()
            hole = body.newObject("Sketcher::SketchObject", "Hole")
            hole.MapMode = "Deactivated"
            hole.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
            hole.addGeometry(Part.Circle(V(15, 10, 0), V(0, 0, 1), 3))
            pocket = body.newObject("PartDesign::Pocket", "Pocket")
            pocket.Profile = hole
            pocket.Type = "ThroughAll"
            doc.recompute()
            edge = [
                "Edge%d" % (i + 1)
                for i, e in enumerate(pocket.Shape.Edges)
                if len(e.Vertexes) == 2
                and all(
                    abs(v.Point.y - 20) < 1e-7 and abs(v.Point.z - 1) < 1e-7 for v in e.Vertexes
                )
            ]
            flange = body.newObject("PartDesign::SheetMetalFlange", "Flange")
            flange.Edges = (pocket, edge)
            flange.Length = 10
            doc.recompute()
            self.assertNotIn("Invalid", flange.State)
            flat = Flat(flange)
            self.assertNotIn("Invalid", flat.State)
            return (
                flange.Shape.copy(),
                flat.FlatLength.Value,
                flat.FlatWidth.Value,
                flat.FlatArea.Value,
                flat.BendCount,
            )
        finally:
            FreeCAD.closeDocument(doc.Name)

    def testFromSTEP(self):
        """M1's part written to STEP and read back, converted: a thin part,
        its plate's bottom fixed; the flat its features give"""
        shape, length, width, area, bends = self.featureFlat()
        body = self.body(self.stepped(shape))
        fixed = self.face(
            body.BaseFeature.Shape,
            lambda f: isinstance(f.Surface, Part.Plane) and abs(f.BoundBox.ZMax) < 1e-7,
        )
        convert = self.convert(body, fixed)
        self.checkValid(convert)
        self.assertEqual(convert.Kind, "Thin part")
        self.assertAlmostEqual(convert.Shape.Volume, shape.Volume, places=6)
        self.assertAlmostEqual(convert.Rule.Thickness.Value, 1, places=9)
        self.checkFlat(convert, length, width, area, bends)

    def testExtrudedProfile(self):
        """An L made in Part, its caps one face each across the bend: a leg
        18 along X, a quarter bend of inner radius 1 about (18, 2), a leg up
        to y 10; 15 deep. Split where the bend meets its walls, it lies flat
        18 + BA + 8 by 15"""
        wire = Part.Wire(
            [
                Part.LineSegment(V(0, 0, 0), V(18, 0, 0)).toShape(),
                Part.ArcOfCircle(
                    Part.Circle(V(18, 2, 0), V(0, 0, 1), 2), -math.pi / 2, 0
                ).toShape(),
                Part.LineSegment(V(20, 2, 0), V(20, 10, 0)).toShape(),
                Part.LineSegment(V(20, 10, 0), V(19, 10, 0)).toShape(),
                Part.LineSegment(V(19, 10, 0), V(19, 2, 0)).toShape(),
                Part.ArcOfCircle(Part.Circle(V(18, 2, 0), V(0, 0, 1), 1), -math.pi / 2, 0)
                .toShape()
                .reversed(),
                Part.LineSegment(V(18, 1, 0), V(0, 1, 0)).toShape(),
                Part.LineSegment(V(0, 1, 0), V(0, 0, 0)).toShape(),
            ]
        )
        solid = Part.Face(wire).extrude(V(0, 0, 15))
        self.assertTrue(solid.isValid())
        caps = [f for f in solid.Faces if abs(f.BoundBox.ZLength) < 1e-9]
        self.assertEqual(len(caps), 2)
        body = self.body(solid)
        fixed = self.face(
            body.BaseFeature.Shape,
            lambda f: isinstance(f.Surface, Part.Plane) and abs(f.BoundBox.YMax) < 1e-7,
        )
        convert = self.convert(body, fixed)
        self.checkValid(convert)
        self.assertAlmostEqual(convert.Shape.Volume, solid.Volume, places=6)
        # the caps split at the bend's two tangent planes: three faces each
        self.assertEqual(len([f for f in convert.Shape.Faces if abs(f.BoundBox.ZLength) < 1e-9]), 6)
        self.checkFlat(convert, 18 + BA + 8, 15, bends=1)

    def testOwnRadius(self):
        """A bend keeps the radius it has: the L above with an inner radius
        of 2, the rule's 1 left as it is, lies flat 17 + 2.5 pi / 2 + 7"""
        wire = Part.Wire(
            [
                Part.LineSegment(V(0, 0, 0), V(17, 0, 0)).toShape(),
                Part.ArcOfCircle(
                    Part.Circle(V(17, 3, 0), V(0, 0, 1), 3), -math.pi / 2, 0
                ).toShape(),
                Part.LineSegment(V(20, 3, 0), V(20, 10, 0)).toShape(),
                Part.LineSegment(V(20, 10, 0), V(19, 10, 0)).toShape(),
                Part.LineSegment(V(19, 10, 0), V(19, 3, 0)).toShape(),
                Part.ArcOfCircle(Part.Circle(V(17, 3, 0), V(0, 0, 1), 2), -math.pi / 2, 0)
                .toShape()
                .reversed(),
                Part.LineSegment(V(17, 1, 0), V(0, 1, 0)).toShape(),
                Part.LineSegment(V(0, 1, 0), V(0, 0, 0)).toShape(),
            ]
        )
        body = self.body(Part.Face(wire).extrude(V(0, 0, 15)))
        fixed = self.face(
            body.BaseFeature.Shape,
            lambda f: isinstance(f.Surface, Part.Plane) and abs(f.BoundBox.YMax) < 1e-7,
        )
        convert = self.convert(body, fixed)
        self.checkValid(convert)
        self.assertAlmostEqual(convert.Rule.BendRadius.Value, 1, places=9)
        self.checkFlat(convert, 17 + 2.5 * math.pi / 2 + 7, 15, bends=1)
        # Check Sheet: the radius the part's own, not the rule's
        bend = PartDesign.checkSheet(convert)["Bends"][0]
        self.assertAlmostEqual(bend["Radius"], 2, places=6)
        self.assertEqual(bend["RadiusSource"], "feature")

    def testCurvedRefused(self):
        """A tube: its walls are curved, refused by name (rolled walls come
        later)"""
        tube = Part.makeCylinder(10, 20).cut(Part.makeCylinder(9, 20))
        body = self.body(tube)
        fixed = self.face(body.BaseFeature.Shape, lambda f: True)
        convert = self.convert(body, fixed)
        self.assertIn("Invalid", convert.State)
        self.assertIn("curved", convert.getStatusString())

    def testThicknessRefused(self):
        """A plate 1 thick with a step 2 thick: the thickness varies, refused
        naming the face"""
        part = Part.makeBox(40, 20, 1).fuse(Part.makeBox(10, 20, 2)).removeSplitter()
        body = self.body(part)
        fixed = self.face(
            body.BaseFeature.Shape,
            lambda f: isinstance(f.Surface, Part.Plane) and abs(f.BoundBox.ZMax) < 1e-7,
        )
        convert = self.convert(body, fixed)
        self.assertIn("Invalid", convert.State)
        self.assertIn("thick", convert.getStatusString())
        self.assertIn("Face", convert.getStatusString())

    # A solid: thickened by the rule's thickness with its open faces taken
    # away, every sharp edge a bend of the rule's radius but where ripped

    def edges(self, shape, test):
        return ["Edge%d" % (i + 1) for i, e in enumerate(shape.Edges) if test(e)]

    def tray(self, **props):
        """A box 40 x 20 x 10, its top open, its four upright edges ripped"""
        body = self.body(Part.makeBox(40, 20, 10))
        box = body.BaseFeature.Shape
        convert = body.newObject("PartDesign::SheetMetalConvert", "Convert")
        convert.FixedFace = (
            body.BaseFeature,
            [self.face(box, lambda f: abs(f.BoundBox.ZMax) < 1e-7)],
        )
        convert.OpenFaces = (
            body.BaseFeature,
            [self.face(box, lambda f: abs(f.BoundBox.ZMin - 10) < 1e-7)],
        )
        convert.RipEdges = (
            body.BaseFeature,
            self.edges(box, lambda e: abs(e.BoundBox.ZLength - 10) < 1e-7),
        )
        for k, v in props.items():
            setattr(convert, k, v)
        self.Doc.recompute()
        return convert

    def testSolidTray(self):
        """The box opened at the top and ripped at its corners, thickened
        inside: a tray of the box's size, the flange suite's mitered tray --
        the bottom cut back R + T at each bend, legs of BA and 8"""
        convert = self.tray()
        self.checkValid(convert)
        self.assertEqual(convert.Kind, "Solid")
        box = convert.Shape.optimalBoundingBox()
        for k, v in (("XLength", 40), ("YLength", 20), ("ZLength", 10)):
            self.assertAlmostEqual(getattr(box, k), v, places=6, msg=k)
        leg = BA + 8
        self.checkFlat(convert, 40 - 2 * 2 + 2 * leg, 20 - 2 * 2 + 2 * leg, bends=4)
        # Check Sheet: thickened, its bends are the rule's radius
        bends = PartDesign.checkSheet(convert)["Bends"]
        self.assertEqual([b["RadiusSource"] for b in bends], ["rule"] * 4)

    def flangeTray(self, **rule):
        """The flange suite's mitred tray -- a 40 x 20 plate, one flange on
        its four top edges -- the rule so set, in a document of its own: its
        bound box, volume, and flat's length, width and area"""
        doc = FreeCAD.newDocument("PartDesignTestSheetMetalConvertFlange")
        try:
            body = doc.addObject("PartDesign::Body", "Body")
            sketch = body.newObject("Sketcher::SketchObject", "Sketch")
            pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
            for i in range(4):
                a, b = pts[i], pts[(i + 1) % 4]
                sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
            plate = body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
            plate.Profile = sketch
            doc.recompute()
            for k, v in rule.items():
                setattr(plate.Rule, k, v)
            doc.recompute()
            names = []
            for i, e in enumerate(plate.Shape.Edges):
                if all(abs(v.Point.z - 1) < 1e-7 for v in e.Vertexes) and (
                    e.BoundBox.XLength in (0, 40) and e.BoundBox.YLength in (0, 20)
                ):
                    names.append("Edge%d" % (i + 1))
            self.assertEqual(len(names), 4)
            flange = body.newObject("PartDesign::SheetMetalFlange", "Flange")
            flange.Edges = (plate, names)
            doc.recompute()
            self.assertNotIn("Invalid", flange.State, flange.getStatusString())
            flat = Flat(flange)
            self.assertNotIn("Invalid", flat.State)
            return (
                flange.Shape.optimalBoundingBox(),
                flange.Shape.Volume,
                flat.FlatLength.Value,
                flat.FlatWidth.Value,
                flat.FlatArea.Value,
            )
        finally:
            FreeCAD.closeDocument(doc.Name)

    def checkAsFlangeTray(self, convert, **rule):
        box, volume, length, width, area = self.flangeTray(**rule)
        self.checkValid(convert)
        self.assertAlmostEqual(convert.Shape.Volume, volume, places=6)
        for k in ("XLength", "YLength", "ZLength"):
            self.assertAlmostEqual(
                getattr(convert.Shape.optimalBoundingBox(), k), getattr(box, k), places=6, msg=k
            )
        self.checkFlat(convert, length, width, area=area, bends=4)

    def testSolidTrayCornerRelief(self):
        """The rule's corner relief cut where the tray's bends meet at each
        rip, as a flange cuts it at a miter: with one larger than the
        convert's own square relief (a circle 6 across), the tray is the
        flange suite's mitred tray, its volume and its flat"""
        convert = self.tray()
        convert.Rule.CornerReliefSize = 6
        self.Doc.recompute()
        self.checkAsFlangeTray(convert, CornerReliefSize=6)

    def testSolidTrayCornerReliefSquare(self):
        """A square relief 6 on a side: the flange tray's too"""
        convert = self.tray()
        convert.Rule.CornerReliefShape = "Square"
        convert.Rule.CornerReliefSize = 6
        self.Doc.recompute()
        self.checkAsFlangeTray(convert, CornerReliefShape="Square", CornerReliefSize=6)

    def testSolidTrayCornerReliefNone(self):
        """None, and Tear, the rip's gap: the convert's own square relief
        alone, more material than the rule's circle leaves"""
        convert = self.tray()
        relieved = convert.Shape.Volume
        convert.Rule.CornerReliefShape = "None"
        self.Doc.recompute()
        self.checkValid(convert)
        plain = convert.Shape.Volume
        self.assertGreater(plain, relieved + 1e-3)
        convert.Rule.CornerReliefShape = "Tear"
        self.Doc.recompute()
        self.checkValid(convert)
        self.assertAlmostEqual(convert.Shape.Volume, plain, places=9)

    def testSolidOutside(self):
        """Thickened outside: the box's faces the inner skins, the tray a
        thickness larger every way it was thickened"""
        convert = self.tray(Side="Outside")
        self.checkValid(convert)
        box = convert.Shape.optimalBoundingBox()
        for k, v in (("XLength", 42), ("YLength", 22), ("ZLength", 11)):
            self.assertAlmostEqual(getattr(box, k), v, places=6, msg=k)
        self.checkFlat(convert, 40 - 2 * 1 + 2 * (BA + 9), 20 - 2 * 1 + 2 * (BA + 9), bends=4)

    def testSolidLoopRefused(self):
        """Not ripped, the box's sides close a loop of bends round it, which
        does not lie flat: refused, saying to rip an edge of it"""
        convert = self.tray()
        convert.RipEdges = None
        self.Doc.recompute()
        self.assertIn("Invalid", convert.State)
        self.assertIn("rip", convert.getStatusString())

    # A shell: thickened along its faces' normals, or against them, open
    # where it has no face

    def shellConvert(self, shell, rips=(), **props):
        body = self.body(shell)
        base = body.BaseFeature.Shape
        convert = body.newObject("PartDesign::SheetMetalConvert", "Convert")
        convert.FixedFace = (
            body.BaseFeature,
            [self.face(base, lambda f: abs(f.BoundBox.ZMax) < 1e-7)],
        )
        if rips:
            convert.RipEdges = (body.BaseFeature, self.edges(base, rips))
        for k, v in props.items():
            setattr(convert, k, v)
        self.Doc.recompute()
        return convert

    def ell(self):
        """An L of two faces, a 30 x 40 bottom and a 20 high side on its
        X = 0 edge, their normals out of the box they are taken from"""
        box = Part.makeBox(30, 40, 20)
        return Part.Shell(
            [f for f in box.Faces if f.BoundBox.ZMax < 1e-7 or f.BoundBox.XMax < 1e-7]
        )

    def testShellL(self):
        """The L thickened inside: the bottom cut back R + T at the bend, the
        side too, by hand 28 + 18 + BA long and 40 wide"""
        convert = self.shellConvert(self.ell())
        self.checkValid(convert)
        self.assertEqual(convert.Kind, "Shell")
        box = convert.Shape.optimalBoundingBox()
        for k, v in (("XLength", 30), ("YLength", 40), ("ZLength", 20)):
            self.assertAlmostEqual(getattr(box, k), v, places=6, msg=k)
        self.checkFlat(convert, 28 + 18 + BA, 40, area=(28 + 18 + BA) * 40, bends=1)

    def testShellLOutside(self):
        """The L thickened outside, its faces the inner skins: cut back R
        only, 29 + 19 + BA long"""
        convert = self.shellConvert(self.ell(), Side="Outside")
        self.checkValid(convert)
        box = convert.Shape.optimalBoundingBox()
        for k, v in (("XLength", 31), ("YLength", 40), ("ZLength", 21)):
            self.assertAlmostEqual(getattr(box, k), v, places=6, msg=k)
        self.checkFlat(convert, 29 + 19 + BA, 40, area=(29 + 19 + BA) * 40, bends=1)

    def testShellTray(self):
        """The box's faces but its top, a shell, its upright edges ripped:
        the tray the solid makes"""
        box = Part.makeBox(40, 20, 10)
        shell = Part.Shell([f for f in box.Faces if f.BoundBox.ZMin < 10 - 1e-7])
        convert = self.shellConvert(shell, rips=lambda e: abs(e.BoundBox.ZLength - 10) < 1e-7)
        self.checkValid(convert)
        self.assertEqual(convert.Kind, "Shell")
        leg = BA + 8
        self.checkFlat(convert, 40 - 2 * 2 + 2 * leg, 20 - 2 * 2 + 2 * leg, bends=4)

    def testShellOpenFacesRefused(self):
        """A shell is open where it has no face: an open face picked on it is
        refused"""
        shell = self.ell()
        convert = self.shellConvert(shell)
        base = convert.BaseFeature.Shape
        convert.OpenFaces = (
            convert.BaseFeature,
            [self.face(base, lambda f: f.BoundBox.XMax < 1e-7)],
        )
        self.Doc.recompute()
        self.assertIn("Invalid", convert.State)
        self.assertIn("Open Faces", convert.getStatusString())

    # A thin part ripped: a closed corner seam, as a part made elsewhere has
    # it, cut open along its edge

    def closedTray(self):
        """A thin tray made elsewhere: 40 x 20 x 10, 1 thick, its bottom's
        edges bends of radius 1, a relief at each bottom corner 2.05 square
        (the solid tray's), its upright corners closed"""

        tray = Part.makeBox(40, 20, 10).cut(Part.makeBox(38, 18, 10, V(1, 1, 1)))
        s = 2.05
        for x in (0, 40 - s):
            for y in (0, 20 - s):
                tray = tray.cut(Part.makeBox(s, s, s, V(x, y, 0)))

        def bottom(shape, inset, z):
            # the bottom's straight edges inset from the box's sides, at z
            return [
                e
                for e in shape.Edges
                if isinstance(e.Curve, Part.Line)
                and abs(e.BoundBox.ZMin - z) < 1e-7
                and e.BoundBox.ZLength < 1e-7
                and (
                    abs(e.BoundBox.YMax - inset) < 1e-7
                    and e.BoundBox.YLength < 1e-7
                    or abs(e.BoundBox.YMin - (20 - inset)) < 1e-7
                    and e.BoundBox.YLength < 1e-7
                    or abs(e.BoundBox.XMax - inset) < 1e-7
                    and e.BoundBox.XLength < 1e-7
                    or abs(e.BoundBox.XMin - (40 - inset)) < 1e-7
                    and e.BoundBox.XLength < 1e-7
                )
            ]

        tray = tray.makeFillet(2, bottom(tray, 0, 0))
        tray = tray.makeFillet(1, bottom(tray, 1, 1))
        return tray

    def testThinRipped(self):
        """The closed tray, its four upright corner seams ripped: the tray
        the solid makes, lying flat as it does"""
        tray = self.closedTray()
        self.assertTrue(tray.isValid())
        body = self.body(self.stepped(tray))
        base = body.BaseFeature.Shape
        convert = body.newObject("PartDesign::SheetMetalConvert", "Convert")
        convert.FixedFace = (body.BaseFeature, [self.face(base, lambda f: f.BoundBox.ZMax < 1e-7)])
        convert.RipEdges = (
            body.BaseFeature,
            self.edges(
                base,
                lambda e: e.BoundBox.XLength < 1e-7
                and e.BoundBox.YLength < 1e-7
                and abs(e.BoundBox.ZMax - 10) < 1e-7
                and min(abs(e.BoundBox.XMin), abs(e.BoundBox.XMin - 40)) < 1e-7
                and min(abs(e.BoundBox.YMin), abs(e.BoundBox.YMin - 20)) < 1e-7,
            ),
        )
        self.assertEqual(len(convert.RipEdges[1]), 4)
        self.Doc.recompute()
        self.checkValid(convert)
        self.assertEqual(convert.Kind, "Thin part")
        leg = BA + 8
        self.checkFlat(convert, 40 - 2 * 2 + 2 * leg, 20 - 2 * 2 + 2 * leg, bends=4)

    def testThinRipRefused(self):
        """A rip on an edge that is no corner between two plane faces -- a
        bend's arc -- is refused, naming it"""
        tray = self.closedTray()
        body = self.body(tray)
        base = body.BaseFeature.Shape
        convert = body.newObject("PartDesign::SheetMetalConvert", "Convert")
        convert.FixedFace = (body.BaseFeature, [self.face(base, lambda f: f.BoundBox.ZMax < 1e-7)])
        convert.RipEdges = (
            body.BaseFeature,
            self.edges(base, lambda e: isinstance(e.Curve, Part.Circle))[:1],
        )
        self.Doc.recompute()
        self.assertIn("Invalid", convert.State)
        self.assertIn("no straight corner", convert.getStatusString())
        self.assertIn("Edge", convert.getStatusString())

    def testSaveAndRestore(self):
        """Saved and opened again, the converted part lies flat as before"""
        shape, length, width, area, bends = self.featureFlat()
        body = self.body(self.stepped(shape))
        fixed = self.face(
            body.BaseFeature.Shape,
            lambda f: isinstance(f.Surface, Part.Plane) and abs(f.BoundBox.ZMax) < 1e-7,
        )
        convert = self.convert(body, fixed)
        self.checkValid(convert)
        path = os.path.join(tempfile.mkdtemp(), "convert.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        convert = self.Doc.getObject("Convert")
        flat = Flat(convert)
        self.assertNotIn("Invalid", flat.State)
        self.assertAlmostEqual(flat.FlatLength.Value, length, places=6)
        self.assertEqual(flat.BendCount, bends)
