# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's names across an edit before the feature (docs/SheetMetal.md
# sec 12, the audit of 2026-11-10 and the ruling of 2026-11-11).
#
# A feature names the faces it makes after its walls and bends, by its own
# count of them: a reference to one is then found by its name, not by where it
# lies, and holds when the edit moves it too.
#
# The sheet's own names -- its walls and bends as the model, the bend table
# and the flat's lines have them -- hold as well: a wall or a bend takes the
# next number of its body when its feature first makes it, and keeps it
# (WallNumbers, BendNumbers). A number is not handed out again while a feature
# of the body has it, suppressed or not.
#
# Each test: a plate 40 x 20, 1 thick; a flange on its left edge; the feature
# on the result, clear of that flange. The faces the feature made keep their
# names with the flange suppressed, and with it back, and so do its walls and
# bends. (What each feature's highlight and names must be at all is
# SheetMetalNames.py, asked of every sheet metal test's document.)

import os
import tempfile
import unittest

import FreeCAD
import Part
import PartDesign

from . import SheetMetalNames
from .SheetMetalFlatView import Flat
from .TestSheetMetalContourFlange import edgeOf

V = FreeCAD.Vector
RIGHT = ((40, 0, 1), (40, 20, 1))


class TestSheetMetalNames(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalNames")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.Early = self.Body.newObject("PartDesign::SheetMetalFlange", "Early")
        self.Early.Edges = (self.Plate, [edgeOf(self.Plate, (0, 0, 1), (0, 20, 1))])
        self.Doc.recompute()
        self.assertNotIn("Invalid", self.Early.State)

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def line(self, x=30):
        """A sketch on the plate's top, a line across it at x"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "Line")
        sketch.MapMode = "Deactivated"
        sketch.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        sketch.addGeometry(Part.LineSegment(V(x, -5, 0), V(x, 25, 0)))
        self.Doc.recompute()
        return sketch

    def made(self, feature):
        """The names of the faces feature made, by where each lies"""
        shape = feature.Shape
        res = {}
        for name in feature.getGeneratedElements():
            if not name.startswith("Face"):
                continue
            face = shape.getElement(name)
            c = face.CenterOfMass
            key = (round(face.Area, 5), round(c.x, 5), round(c.y, 5), round(c.z, 5))
            res[key] = shape.getElementName(name, 1)
        return res

    def checkNamesHold(self, feature, least):
        """feature's faces, least of them at least, keep their names with
        the flange before it suppressed and with it back"""
        self.Doc.recompute()
        self.assertNotIn("Invalid", feature.State)
        before = self.made(feature)
        self.assertGreaterEqual(len(before), least)
        for suppress in (True, False):
            self.Early.Suppress = suppress
            self.Doc.recompute()
            self.assertNotIn("Invalid", feature.State)
            after = self.made(feature)
            # every face the feature made is where it was, the flange being
            # clear of it
            self.assertEqual(sorted(after), sorted(before), suppress)
            renamed = {
                key: (before[key], after[key]) for key in before if before[key] != after[key]
            }
            self.assertEqual(renamed, {}, suppress)

    def testFlange(self):
        """A flange's bend, leg and ends"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        self.checkNamesHold(flange, 8)

    def testFlangeOnFlange(self):
        """A flange on the leg of a flange after the first: its own names,
        and its reference to that leg's edge"""
        first = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        first.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        self.Doc.recompute()
        top = first.Shape.BoundBox.ZMax
        second = self.Body.newObject("PartDesign::SheetMetalFlange", "Second")
        second.Edges = (first, [edgeOf(first, (40, 0, top), (40, 20, top))])
        second.Angle = 45
        self.checkNamesHold(second, 8)

    def testHem(self):
        """A hem's bend and leg"""
        hem = self.Body.newObject("PartDesign::SheetMetalHem", "Hem")
        hem.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        hem.Length = 6
        self.checkNamesHold(hem, 6)

    def testContourFlange(self):
        """A contour flange's two bends and two walls"""
        s = self.Body.newObject("Sketcher::SketchObject", "ContourSketch")
        s.MapMode = "Deactivated"
        # x along the plate's x, y up, on the plane y 10
        s.Placement = FreeCAD.Placement(
            V(0, 10, 0), FreeCAD.Rotation(V(1, 0, 0), V(0, 0, 1), V(0, -1, 0), "ZXY")
        )
        pts = [(40, 1), (40, 11), (50, 11)]
        for p, q in zip(pts, pts[1:]):
            s.addGeometry(Part.LineSegment(V(p[0], p[1], 0), V(q[0], q[1], 0)))
        self.Doc.recompute()
        contour = self.Body.newObject("PartDesign::SheetMetalContourFlange", "ContourFlange")
        contour.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        contour.Profile = s
        self.checkNamesHold(contour, 12)

    def testFold(self):
        """A fold's bend"""
        fold = self.Body.newObject("PartDesign::SheetMetalFold", "Fold")
        fold.Profile = self.line()
        self.checkNamesHold(fold, 4)

    def testJog(self):
        """A jog's two bends and its run"""
        jog = self.Body.newObject("PartDesign::SheetMetalJog", "Jog")
        jog.Profile = self.line()
        jog.Offset = 5
        self.checkNamesHold(jog, 12)

    # the sheet's own names

    def sheet(self, feature):
        """feature's walls and bends: the name of each, by where it lies"""
        res = {}
        for e in feature.SheetModel["Elements"]:
            p = e["Origin"] if e["Kind"] == "Wall" else e["Center"]
            res[(e["Kind"], round(p.x, 5), round(p.y, 5), round(p.z, 5))] = e["Name"]
        return res

    def rightFlange(self):
        on = self.Early
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (on, [edgeOf(on, *RIGHT)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        return flange

    def checkSheetNamesHold(self, feature, least):
        """The walls and bends of feature's sheet, least of them at least
        beside the first flange's, keep their names with that flange
        suppressed and with it back"""
        self.Doc.recompute()
        self.assertNotIn("Invalid", feature.State)
        early = set(self.sheet(self.Early)) - set(self.sheet(self.Plate))
        before = {k: v for k, v in self.sheet(feature).items() if k not in early}
        self.assertGreaterEqual(len(before), least)
        self.assertEqual(len(set(before.values())), len(before))
        for suppress in (True, False):
            self.Early.Suppress = suppress
            self.Doc.recompute()
            self.assertNotIn("Invalid", feature.State)
            after = {k: v for k, v in self.sheet(feature).items() if k not in early}
            self.assertEqual(after, before, suppress)

    def testSheetFlange(self):
        """A flange's bend and wall"""
        self.checkSheetNamesHold(self.rightFlange(), 3)

    def testSheetFlangeOnFlange(self):
        first = self.rightFlange()
        top = first.Shape.BoundBox.ZMax
        second = self.Body.newObject("PartDesign::SheetMetalFlange", "Second")
        second.Edges = (first, [edgeOf(first, (40, 0, top), (40, 20, top))])
        second.Angle = 45
        self.checkSheetNamesHold(second, 5)

    def testSheetHem(self):
        hem = self.Body.newObject("PartDesign::SheetMetalHem", "Hem")
        hem.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        hem.Length = 6
        self.checkSheetNamesHold(hem, 3)

    def testSheetContourFlange(self):
        s = self.Body.newObject("Sketcher::SketchObject", "ContourSketch")
        s.MapMode = "Deactivated"
        s.Placement = FreeCAD.Placement(
            V(0, 10, 0), FreeCAD.Rotation(V(1, 0, 0), V(0, 0, 1), V(0, -1, 0), "ZXY")
        )
        pts = [(40, 1), (40, 11), (50, 11)]
        for p, q in zip(pts, pts[1:]):
            s.addGeometry(Part.LineSegment(V(p[0], p[1], 0), V(q[0], q[1], 0)))
        self.Doc.recompute()
        contour = self.Body.newObject("PartDesign::SheetMetalContourFlange", "ContourFlange")
        contour.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        contour.Profile = s
        self.checkSheetNamesHold(contour, 5)

    def testSheetFold(self):
        fold = self.Body.newObject("PartDesign::SheetMetalFold", "Fold")
        fold.Profile = self.line()
        self.checkSheetNamesHold(fold, 3)

    def testSheetJog(self):
        jog = self.Body.newObject("PartDesign::SheetMetalJog", "Jog")
        jog.Profile = self.line()
        jog.Offset = 5
        self.checkSheetNamesHold(jog, 5)

    def testBendTable(self):
        """The bend table's row of a flange's bend keeps its name"""
        flange = self.rightFlange()

        def rows():
            return [r["Name"] for r in PartDesign.sheetBends(flange) if r["Feature"] == flange.Name]

        before = rows()
        self.assertEqual(len(before), 1)
        for suppress in (True, False):
            self.Early.Suppress = suppress
            self.Doc.recompute()
            self.assertEqual(rows(), before, suppress)

    def testFlatLine(self):
        """The flat's line of a flange's bend keeps its name: a drawing's
        dimension to it finds it by that"""
        flange = self.rightFlange()

        def lines():
            shape = Flat(flange).Shape
            res = set()
            for i, edge in enumerate(shape.SubShapes[1].Edges):
                if edge.CenterOfMass.x > 20:
                    for j, other in enumerate(shape.Edges):
                        if other.isSame(edge):
                            res.add(shape.getElementName("Edge%d" % (j + 1), 1).split(";")[0])
            return res

        before = lines()
        self.assertEqual(len(before), 1)
        self.assertTrue(min(before).startswith("Bend"), before)
        for suppress in (True, False):
            self.Early.Suppress = suppress
            self.Doc.recompute()
            self.assertEqual(lines(), before, suppress)

    def testOverrideByName(self):
        """A bend's own K is asked for by the bend's name, which holds"""
        flange = self.rightFlange()
        (bend,) = [
            r["Name"] for r in PartDesign.sheetBends(flange) if r["Feature"] == flange.Name
        ]
        flange.setBendKFactor(bend, 0.3)
        self.Doc.recompute()
        for suppress in (True, False):
            self.Early.Suppress = suppress
            self.Doc.recompute()
            self.assertAlmostEqual(flange.bendKFactor(bend), 0.3, places=12, msg=suppress)

    def testAddedWhileSuppressed(self):
        """A flange made while the one before it is suppressed takes numbers
        that flange does not hold: with it back, neither is renamed"""
        early = self.sheet(self.Early)
        self.Early.Suppress = True
        self.Doc.recompute()
        flange = self.rightFlange()
        made = self.sheet(flange)
        self.Early.Suppress = False
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        both = self.sheet(flange)
        self.assertEqual(len(set(both.values())), len(both))
        for had in (early, made):
            self.assertEqual({k: both.get(k) for k in had}, had)

    def testInsertedBefore(self):
        """A flange put into the body before another takes the next numbers
        of the body, the one after it keeping its own"""

        def own(feature):
            return sorted(
                set(self.sheet(feature).values()) - set(self.sheet(feature.BaseFeature).values())
            )

        flange = self.rightFlange()
        before = own(flange)
        self.assertEqual(len(before), 2)
        mid = self.Doc.addObject("PartDesign::SheetMetalFlange", "Mid")
        self.Body.insertObject(mid, self.Early, True)
        mid.Edges = (self.Early, [edgeOf(self.Early, (0, 0, 1), (40, 0, 1))])
        self.Doc.recompute()
        self.assertNotIn("Invalid", mid.State)
        self.assertEqual(flange.BaseFeature, mid)
        # its edge is one of the feature it now builds on
        flange.Edges = (mid, [edgeOf(mid, *RIGHT)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        self.assertEqual(own(flange), before)
        self.assertEqual(len(own(mid)), 2)
        names = list(self.sheet(flange).values())
        self.assertEqual(len(names), 7)
        self.assertEqual(len(set(names)), 7)

    def testRemovedBefore(self):
        """The flange before another deleted, the other keeps its names"""
        flange = self.rightFlange()
        early = set(self.sheet(self.Early)) - set(self.sheet(self.Plate))
        before = {k: v for k, v in self.sheet(flange).items() if k not in early}
        self.Body.removeObject(self.Early)
        self.Doc.removeObject(self.Early.Name)
        self.Early = None
        # its edge is one of the feature it now builds on
        flange.Edges = (self.Plate, [edgeOf(self.Plate, *RIGHT)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        self.assertEqual(self.sheet(flange), before)

    def testNumberTaken(self):
        """A feature holding a number another before it has -- a copy of
        that one -- takes a new one: no name is twice in a sheet"""
        flange = self.rightFlange()
        self.assertEqual(len(flange.BendNumbers), 1)
        self.assertEqual(len(flange.WallNumbers), 1)
        flange.BendNumbers = self.Early.BendNumbers
        flange.WallNumbers = self.Early.WallNumbers
        flange.touch()
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        names = self.sheet(flange)
        self.assertEqual(len(set(names.values())), len(names))
        self.assertNotEqual(flange.BendNumbers, self.Early.BendNumbers)
        self.assertNotEqual(flange.WallNumbers, self.Early.WallNumbers)

    def testSavedAndReopened(self):
        """The numbers are the document's: reopened, and the flange before
        suppressed there, the names are as they were"""
        flange = self.rightFlange()
        early = set(self.sheet(self.Early)) - set(self.sheet(self.Plate))
        before = {k: v for k, v in self.sheet(flange).items() if k not in early}
        path = os.path.join(tempfile.mkdtemp(), "SheetMetalNames.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        flange = self.Doc.getObject("Flange")
        self.Early = self.Doc.getObject("Early")
        self.Early.Suppress = True
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        self.assertEqual(self.sheet(flange), before)
