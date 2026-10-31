# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's rule object, PartDesign::SheetMetalRule (docs/SheetMetal.md
# sec 4.6): the values a sheet's features share, from its own properties or
# from a material card with the SheetMetal model.

import math
import os
import tempfile
import unittest

import FreeCAD


def card(gauges=(), method=None, table=None, name="Sheet test"):
    """A material with the SheetMetal model: gauges as (thickness, radius, K,
    minimum flange) rows, table as (thickness, radius, angle in deg, K,
    allowance, deduction) rows"""
    import Materials

    mat = Materials.Material()
    mat.Name = name
    mat.addPhysicalModel(Materials.UUIDs().SheetMetal)
    if gauges:
        array = Materials.Array2D()
        array.Columns = 4
        array.Rows = len(gauges)
        for r, (t, radius, k, flange) in enumerate(gauges):
            array.setValue(r, 0, "%g mm" % t)
            array.setValue(r, 1, "%g mm" % radius)
            array.setValue(r, 2, "%g" % k)
            array.setValue(r, 3, "%g mm" % flange)
        mat.setValue("Gauges", array)
    if method:
        mat.setPhysicalValue("AllowanceMethod", method)
    if table:
        array = Materials.Array2D()
        array.Columns = 6
        array.Rows = len(table)
        for r, (t, radius, angle, k, allowance, deduction) in enumerate(table):
            array.setValue(r, 0, "%g mm" % t)
            array.setValue(r, 1, "%g mm" % radius)
            array.setValue(r, 2, "%g deg" % angle)
            array.setValue(r, 3, "%g" % k)
            array.setValue(r, 4, "%g mm" % allowance)
            array.setValue(r, 5, "%g mm" % deduction)
        mat.setValue("BendTable", array)
    return mat


GAUGES = [(1, 1.5, 0.42, 6), (2, 2.5, 0.45, 10)]


class TestSheetMetalRule(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalRule")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def rule(self, body=None):
        rule = (body or self.Body).newObject("PartDesign::SheetMetalRule", "Rule")
        self.Doc.recompute()
        return rule

    def testDefaults(self):
        """With no card, the rule's own values: 1 mm thick, 1 mm radius,
        K 0.5; an empty shape, in the body and never its tip"""
        box = self.Body.newObject("PartDesign::AdditiveBox", "Box")
        self.Doc.recompute()
        rule = self.rule()
        self.assertIn(rule, self.Body.Group)
        self.assertEqual(self.Body.Tip, box)
        self.assertNotIn("Invalid", rule.State)
        self.assertTrue(rule.Shape.isNull())
        self.assertAlmostEqual(rule.Thickness.Value, 1)
        self.assertAlmostEqual(rule.BendRadius.Value, 1)
        self.assertAlmostEqual(rule.KFactor, 0.5)
        self.assertEqual(rule.AllowanceMethod, "KFactor")
        self.assertEqual(rule.Overridden, [])
        for name in (
            "MinFlange",
            "Gap",
            "ReliefShape",
            "ReliefWidth",
            "ReliefDepth",
            "CornerReliefShape",
            "CornerReliefSize",
        ):
            self.assertIn(name, rule.PropertiesList)
        self.assertFalse(rule.hasGauge())
        self.assertAlmostEqual(box.Shape.Volume, 1000)

    def testGaugeRowFromCard(self):
        """The card's gauge row for the rule's thickness gives its radius, K
        and minimum flange; a thickness with no row keeps them"""
        rule = self.rule()
        rule.ShapeMaterial = card(GAUGES)
        self.assertTrue(rule.hasGauge())
        self.assertAlmostEqual(rule.BendRadius.Value, 1.5)
        self.assertAlmostEqual(rule.KFactor, 0.42)
        self.assertAlmostEqual(rule.MinFlange.Value, 6)
        rule.Thickness = 2
        self.assertAlmostEqual(rule.BendRadius.Value, 2.5)
        self.assertAlmostEqual(rule.KFactor, 0.45)
        self.assertAlmostEqual(rule.MinFlange.Value, 10)
        rule.Thickness = 3
        self.assertFalse(rule.hasGauge())
        self.assertAlmostEqual(rule.BendRadius.Value, 2.5)
        self.assertAlmostEqual(rule.KFactor, 0.45)
        # none of that was set by hand
        self.assertEqual(rule.Overridden, [])

    def testOverride(self):
        """A value set by hand stays through a change of thickness or card,
        until given back to the card"""
        rule = self.rule()
        rule.ShapeMaterial = card(GAUGES)
        rule.KFactor = 0.3
        self.assertEqual(rule.Overridden, ["KFactor"])
        rule.Thickness = 2
        self.assertAlmostEqual(rule.KFactor, 0.3)
        self.assertAlmostEqual(rule.BendRadius.Value, 2.5)
        rule.ShapeMaterial = card([(2, 3, 0.4, 12)], name="Other")
        self.assertAlmostEqual(rule.KFactor, 0.3)
        self.assertAlmostEqual(rule.BendRadius.Value, 3)
        rule.resetRuleValue("KFactor")
        self.assertEqual(rule.Overridden, [])
        self.assertAlmostEqual(rule.KFactor, 0.4)
        with self.assertRaises(Exception):
            rule.resetRuleValue("Thickness")

    def testMethodFromCard(self):
        """The card's allowance method, KFactor when it names none; set by
        hand it is overridden as any value"""
        rule = self.rule()
        rule.ShapeMaterial = card(GAUGES, method="KFactorDIN")
        self.assertEqual(rule.AllowanceMethod, "KFactorDIN")
        rule.ShapeMaterial = card(GAUGES, name="No method")
        self.assertEqual(rule.AllowanceMethod, "KFactor")
        rule.AllowanceMethod = "DIN6935"
        self.assertEqual(rule.Overridden, ["AllowanceMethod"])
        rule.ShapeMaterial = card(GAUGES, method="KFactorDIN", name="Again")
        self.assertEqual(rule.AllowanceMethod, "DIN6935")

    def testBendAllowance(self):
        """The rule's bend allowance at an angle: by K, and from the card's
        table, interpolated, for each table method"""
        rule = self.rule()
        self.assertAlmostEqual(rule.bendAllowance(90), math.pi / 2 * (1 + 0.5 * 1))
        rule.KFactor = 0.25
        self.assertAlmostEqual(rule.bendAllowance(90), math.pi / 2 * (1 + 0.25 * 1))
        rows = []
        for t in (1, 2):
            for radius in (1, 2):
                for angle in (45, 90):
                    k = 0.3 + 0.01 * t + 0.02 * radius + 0.001 * angle
                    allowance = 0.1 * t + radius + 0.01 * angle
                    rows.append((t, radius, angle, k, allowance, 2 * allowance))
        rule.ShapeMaterial = card([(1, 1, 0.5, 5)], method="KTable", table=rows)
        rule.resetRuleValue("KFactor")
        self.assertEqual(rule.AllowanceMethod, "KTable")
        # thickness 1, radius 1, half way between 45 and 90 deg
        k = 0.3 + 0.01 + 0.02 + 0.001 * 67.5
        angle = math.radians(67.5)
        self.assertAlmostEqual(rule.bendAllowance(67.5), angle * (1 + k * 1))
        rule.AllowanceMethod = "AllowanceTable"
        self.assertAlmostEqual(rule.bendAllowance(67.5), 0.1 + 1 + 0.675)
        # a table with a corner missing does not say what it means
        rule.ShapeMaterial = card([(1, 1, 0.5, 5)], method="KTable", table=rows[:-1], name="Short")
        rule.resetRuleValue("AllowanceMethod")
        with self.assertRaises(Exception):
            rule.bendAllowance(67.5)

    def testFollowsBodyMaterial(self):
        """The rule takes the body's material, as a feature does, until it
        is given another; one added later takes it too"""
        sheet = card(GAUGES)
        self.Body.ShapeMaterial = sheet
        rule = self.rule()
        self.assertEqual(rule.ShapeMaterial.UUID, sheet.UUID)
        self.assertAlmostEqual(rule.BendRadius.Value, 1.5)
        other = card([(1, 4, 0.4, 6)], name="Other")
        self.Body.ShapeMaterial = other
        self.assertEqual(rule.ShapeMaterial.UUID, other.UUID)
        self.assertAlmostEqual(rule.BendRadius.Value, 4)
        own = card([(1, 5, 0.4, 6)], name="Own")
        rule.ShapeMaterial = own
        self.Body.ShapeMaterial = sheet
        self.assertEqual(rule.ShapeMaterial.UUID, own.UUID)
        # and the rule's card is not the body's
        rule.ShapeMaterial = card([(1, 6, 0.4, 6)], name="Own again")
        self.assertEqual(self.Body.ShapeMaterial.UUID, sheet.UUID)

    def testExpressions(self):
        """A feature reads the rule by expression, and a spreadsheet drives
        it, both in one recompute"""
        rule = self.rule()
        rule.ShapeMaterial = card(GAUGES)
        box = self.Body.newObject("PartDesign::AdditiveBox", "Box")
        box.setExpression("Height", "Rule.Thickness")
        box.setExpression("Width", "Rule.BendRadius * 4")
        sheet = self.Doc.addObject("Spreadsheet::Sheet", "Sheet")
        sheet.set("A1", "2")
        sheet.setAlias("A1", "t")
        rule.setExpression("Thickness", "Sheet.t")
        self.Doc.recompute()
        self.assertAlmostEqual(rule.Thickness.Value, 2)
        self.assertAlmostEqual(box.Height.Value, 2)
        self.assertAlmostEqual(box.Width.Value, 10)
        sheet.set("A1", "1")
        self.Doc.recompute()
        self.assertAlmostEqual(box.Height.Value, 1)
        self.assertAlmostEqual(box.Width.Value, 6)
        self.assertNotIn("Touched", box.State)
        # driven by an expression is not set by hand
        self.assertEqual(rule.Overridden, [])

    def testUndo(self):
        """Undoing a value set by hand gives it back to the card"""
        self.Doc.UndoMode = 1
        rule = self.rule()
        rule.ShapeMaterial = card(GAUGES)
        self.Doc.openTransaction("K")
        rule.KFactor = 0.3
        self.Doc.commitTransaction()
        self.Doc.undo()
        self.assertAlmostEqual(rule.KFactor, 0.42)
        self.assertEqual(rule.Overridden, [])
        self.Doc.redo()
        self.assertAlmostEqual(rule.KFactor, 0.3)
        self.assertEqual(rule.Overridden, ["KFactor"])

    def testSaveAndRestore(self):
        """The values and what was overridden come back; a thickness changed
        after still follows the card"""
        rule = self.rule()
        rule.ShapeMaterial = card(GAUGES)
        rule.KFactor = 0.3
        self.Doc.recompute()
        path = os.path.join(tempfile.mkdtemp(), "rule.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        rule = self.Doc.getObject("Rule")
        self.assertEqual(rule.Overridden, ["KFactor"])
        self.assertAlmostEqual(rule.KFactor, 0.3)
        self.assertAlmostEqual(rule.BendRadius.Value, 1.5)
        self.assertTrue(rule.ShapeMaterial.hasPhysicalModel(__import__("Materials").UUIDs().SheetMetal))
        rule.Thickness = 2
        self.assertAlmostEqual(rule.BendRadius.Value, 2.5)
        self.assertAlmostEqual(rule.KFactor, 0.3)
