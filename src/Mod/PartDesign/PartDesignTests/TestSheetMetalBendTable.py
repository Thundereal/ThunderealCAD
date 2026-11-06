# SPDX-License-Identifier: LGPL-2.1-or-later

# The bend table of a drawing (docs/SheetMetal.md sec 5.6, ruled 2026-11-05):
# PartDesign::SheetMetalBendTable, a spreadsheet of its own, filled from the
# sheet's bends (PartDesign.sheetBends()). AutoSync fills it at each
# recompute; off, PartDesign.syncBendTable() does when asked. It is edited as
# any spreadsheet: a cell the user changed stays theirs, a cell the table
# never wrote is the user's, and a forced sync takes every cell of its own
# back.
#
# A plate 40 x 20, 1 thick, a flange 10 high on its far edge and one on its
# near edge: two bends, 90 deg, the rule's radius 1 and K 0.5, each 2.356
# (pi / 2 * 1.5) long flat.

import math
import unittest

import FreeCAD
import Part
import PartDesign

V = FreeCAD.Vector
FAR = ((0, 20, 1), (40, 20, 1))
NEAR = ((0, 0, 1), (40, 0, 1))
BA = math.pi / 2 * 1.5


class TestSheetMetalBendTable(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalBendTable")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.First = self.flange(FAR, self.Plate)
        self.Second = self.flange(NEAR, self.First)

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def edge(self, feature, a, b):
        for i, e in enumerate(feature.Shape.Edges):
            ends = [v.Point for v in e.Vertexes]
            if len(ends) == 2 and (
                (ends[0].isEqual(V(*a), 1e-7) and ends[1].isEqual(V(*b), 1e-7))
                or (ends[0].isEqual(V(*b), 1e-7) and ends[1].isEqual(V(*a), 1e-7))
            ):
                return "Edge%d" % (i + 1)
        self.fail("no edge from %s to %s" % (a, b))

    def flange(self, edge, base):
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (base, [self.edge(base, *edge)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        return flange

    def table(self, **props):
        table = self.Doc.addObject("PartDesign::SheetMetalBendTable", "BendTable")
        table.Source = self.Body.getSolidBody(self.Second)
        for k, v in props.items():
            setattr(table, k, v)
        self.Doc.recompute()
        return table

    def cell(self, table, address):
        try:
            return table.get(address)
        except Exception:
            return None

    def testSheetBends(self):
        rows = PartDesign.sheetBends(self.Second)
        self.assertEqual([r["Name"] for r in rows], ["Bend1", "Bend2"])
        self.assertEqual(rows[0]["Feature"], self.First.Name)
        self.assertAlmostEqual(rows[0]["Allowance"], BA, places=6)
        self.assertIn(rows[0]["Direction"], ("Up", "Down"))
        self.assertNotIn("Elements", rows[0])

    def testFilled(self):
        table = self.table()
        self.assertNotIn("Invalid", table.State, table.getStatusString())
        self.assertTrue(table.AutoSync)
        self.assertEqual(self.cell(table, "A1"), "Bend")
        self.assertEqual(self.cell(table, "A2"), "Bend1")
        self.assertEqual(self.cell(table, "A3"), "Bend2")
        self.assertEqual(self.cell(table, "B2"), self.First.Label)
        self.assertAlmostEqual(float(self.cell(table, "C2")), 90, places=6)
        self.assertAlmostEqual(float(self.cell(table, "D2")), 1, places=6)
        self.assertAlmostEqual(float(self.cell(table, "E2")), 0.5, places=6)
        self.assertAlmostEqual(float(self.cell(table, "F2")), BA, places=3)
        self.assertIn(self.cell(table, "G2"), ("Up", "Down"))
        self.assertIsNone(self.cell(table, "A4"))

    def testAutoSync(self):
        table = self.table()
        self.First.Rule.BendRadius = 2
        self.Doc.recompute()
        self.assertAlmostEqual(float(self.cell(table, "D2")), 2, places=6)
        self.assertAlmostEqual(float(self.cell(table, "D3")), 2, places=6)

    def testManualSync(self):
        table = self.table(AutoSync=False)
        self.assertEqual(self.cell(table, "A2"), "Bend1")
        self.First.Rule.BendRadius = 2
        self.Doc.recompute()
        self.assertAlmostEqual(float(self.cell(table, "D2")), 1, places=6)
        PartDesign.syncBendTable(table)
        self.Doc.recompute()
        self.assertAlmostEqual(float(self.cell(table, "D2")), 2, places=6)

    def testEditsKept(self):
        """A cell the user changed stays theirs, and one the table never
        wrote; the rest follow the sheet. A forced sync takes its own back"""
        table = self.table()
        table.set("G2", "check")
        table.set("H2", "deburr")
        self.Doc.recompute()
        self.First.Rule.BendRadius = 2
        self.Doc.recompute()
        self.assertEqual(self.cell(table, "G2"), "check")
        self.assertEqual(self.cell(table, "H2"), "deburr")
        self.assertAlmostEqual(float(self.cell(table, "D2")), 2, places=6)
        PartDesign.syncBendTable(table, True)
        self.Doc.recompute()
        self.assertIn(self.cell(table, "G2"), ("Up", "Down"))
        self.assertEqual(self.cell(table, "H2"), "deburr")

    def testFewerBends(self):
        """A bend gone takes its row, but for what the user wrote there"""
        table = self.table()
        table.set("C3", "92")
        self.Doc.recompute()
        self.Body.removeObject(self.Second)
        self.Doc.removeObject(self.Second.Name)
        table.Source = self.Body.getSolidBody(self.First)
        self.Doc.recompute()
        self.assertEqual(self.cell(table, "A2"), "Bend1")
        self.assertIsNone(self.cell(table, "A3"))
        self.assertAlmostEqual(float(self.cell(table, "C3")), 92, places=6)

    def testBendOwnK(self):
        table = self.table()
        self.Second.setBendKFactor("Bend2", 0.3)
        self.Doc.recompute()
        self.assertAlmostEqual(float(self.cell(table, "E3")), 0.3, places=6)

    def testNotSheet(self):
        box = self.Doc.addObject("Part::Box", "Box")
        table = self.Doc.addObject("PartDesign::SheetMetalBendTable", "BendTable")
        table.Source = box
        self.Doc.recompute()
        self.assertIn("Invalid", table.State)
