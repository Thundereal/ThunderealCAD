# SPDX-License-Identifier: LGPL-2.1-or-later

# Starter shapes (docs/SheetMetal.md sec 5.1, M5's first item, ruled
# 2026-11-06): PartDesign.makeStarterShape() makes an L, U, tub, hat or box as
# ordinary features of a body -- a rectangle sketch with its Length and Width
# constraints named, a base wall, a flange on the edges the shape has, and for
# a hat or a box a second flange, the lip, on the legs' top ends -- all of
# them edited afterwards as any are. The sizes are the outside ones: Length
# along X, Width along Y, Height up; a hat's lip reaches FlangeWidth out past
# the legs, a box's in from their outsides. Centred on the origin, or from it.

import unittest

import FreeCAD
import Part
import PartDesign

V = FreeCAD.Vector
L, W, H, F = 40.0, 30.0, 15.0, 5.0
KINDS = {
    # kind: (bends, flange features)
    "L": (1, 1),
    "U": (2, 1),
    "Tub": (4, 1),
    "Hat": (8, 2),
    "Box": (8, 2),
}


class TestSheetMetalStarter(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalStarter")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def fresh(self):
        """Another document, for the next shape"""
        FreeCAD.closeDocument(self.Doc.Name)
        self.setUp()

    def make(self, kind, **kw):
        made = PartDesign.makeStarterShape(self.Body, kind, L, W, H, **kw)
        self.Doc.recompute()
        for o in made:
            self.assertNotIn("Invalid", o.State, (o.Name, o.State))
        return made

    def checkBox(self, shape, x0, x1, y0, y1, z0, z1):
        # the optimal box, not of a mesh: the plain one is loose about a bend
        box = shape.optimalBoundingBox(False)
        for got, want in zip(
            (box.XMin, box.XMax, box.YMin, box.YMax, box.ZMin, box.ZMax), (x0, x1, y0, y1, z0, z1)
        ):
            self.assertAlmostEqual(got, want, places=6, msg=str(box))

    def testKinds(self):
        """Each kind: a sketch, a base wall and its flanges, the last the tip,
        one solid of the outside sizes asked for"""
        for kind, (bends, flanges) in KINDS.items():
            with self.subTest(kind=kind):
                made = self.make(kind)
                types = [o.TypeId for o in made]
                self.assertEqual(
                    types,
                    ["Sketcher::SketchObject", "PartDesign::SheetMetalBaseWall"]
                    + ["PartDesign::SheetMetalFlange"] * flanges,
                )
                tip = made[-1]
                self.assertEqual(self.Body.Tip, tip)
                self.assertEqual(len(tip.Shape.Solids), 1)
                self.assertTrue(tip.Shape.isValid())
                out = F if kind == "Hat" else 0
                self.checkBox(tip.Shape, -L / 2 - out, L / 2 + out, -W / 2 - out, W / 2 + out, 0, H)
                bendCount = len([e for e in tip.SheetModel["Elements"] if e["Kind"] == "Bend"])
                self.assertEqual(bendCount, bends)
                self.fresh()

    def testOneSide(self):
        """An L's leg on the far long edge, a U's on both"""
        tip = self.make("L")[-1]
        half = 1.0 / 2
        self.assertTrue(tip.Shape.isInside(V(0, W / 2 - half, H - 1), 1e-7, True))
        self.assertFalse(tip.Shape.isInside(V(0, -W / 2 + half, H - 1), 1e-7, True))
        self.fresh()
        tip = self.make("U")[-1]
        self.assertTrue(tip.Shape.isInside(V(0, W / 2 - half, H - 1), 1e-7, True))
        self.assertTrue(tip.Shape.isInside(V(0, -W / 2 + half, H - 1), 1e-7, True))
        self.assertFalse(tip.Shape.isInside(V(L / 2 - half, 0, H - 1), 1e-7, True))

    def testLip(self):
        """A box's lip reaches FlangeWidth in from the legs' outsides, a hat's
        out past them, both on top; a box's stop short of its corners, where
        they would meet bending in"""
        tip = self.make("Box")[-1]
        T = 1.0
        z = H - T / 2
        self.assertTrue(tip.Shape.isInside(V(-L / 2 + F - 0.1, 0, z), 1e-7, True))
        self.assertFalse(tip.Shape.isInside(V(-L / 2 + F + 0.1, 0, z), 1e-7, True))
        self.assertTrue(tip.Shape.isInside(V(0, W / 2 - F + 0.1, z), 1e-7, True))
        self.assertFalse(tip.Shape.isInside(V(0, W / 2 - F - 0.1, z), 1e-7, True))
        corner = V(-L / 2 + F / 2 + 0.5, -W / 2 + F / 2 + 0.5, z)
        self.assertFalse(tip.Shape.isInside(corner, 1e-7, True))
        self.fresh()
        tip = self.make("Hat", FlangeWidth=8)[-1]
        self.checkBox(tip.Shape, -L / 2 - 8, L / 2 + 8, -W / 2 - 8, W / 2 + 8, 0, H)
        self.assertTrue(tip.Shape.isInside(V(-L / 2 - 7.9, 0, z), 1e-7, True))
        self.assertFalse(tip.Shape.isInside(V(-L / 2 + 0.5, 0, H - 0.5), 1e-7, True))

    def testFlat(self):
        """Each lies flat with nothing wrong; a lip as wide as the legs' miter
        planes meet across the shape too (40 by 30, the +Y legs' meet at y
        -5, where a 10 mm lip on the -Y legs ends: its end face was taken
        for the +Y lip's and the flat did not close, found 2026-11-06)"""
        cases = [(kind, {}) for kind in KINDS] + [(k, {"FlangeWidth": 10}) for k in ("Hat", "Box")]
        for kind, kw in cases:
            with self.subTest(kind=kind, **kw):
                tip = self.make(kind, **kw)[-1]
                problems = PartDesign.checkSheet(tip)["Problems"]
                self.assertEqual([p for p in problems if p["Kind"] == "NoFlat"], [], problems)
                self.assertIsNotNone(tip.getSubObject("Flat"))
                self.fresh()

    def testEdited(self):
        """Ordinary features: the sketch's Length and Width, the flange's
        Length, edited afterwards"""
        sketch, wall, flange = self.make("Tub")
        names = [c.Name for c in sketch.Constraints if c.Name]
        self.assertEqual(sorted(names), ["Length", "Width"])
        self.assertEqual(wall.Profile[0], sketch)
        sketch.setDatum("Length", FreeCAD.Units.Quantity("60 mm"))
        sketch.setDatum("Width", FreeCAD.Units.Quantity("50 mm"))
        flange.Length = 25
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        self.checkBox(flange.Shape, -30, 30, -25, 25, 0, 25)

    def testFromOrigin(self):
        """Not centred: from the origin along X and Y"""
        tip = self.make("U", Centered=False)[-1]
        self.checkBox(tip.Shape, 0, L, 0, W, 0, H)

    def testThickness(self):
        """The outside sizes whatever the rule's thickness and radius"""
        made = self.make("L")
        rule = made[1].Rule
        rule.Thickness = 2
        rule.BendRadius = 3
        self.Doc.recompute()
        self.checkBox(made[-1].Shape, -L / 2, L / 2, -W / 2, W / 2, 0, H)

    def testRefused(self):
        """An unknown kind, sizes too small for the sheet's bends, no body"""
        with self.assertRaises(ValueError):
            PartDesign.makeStarterShape(self.Body, "Z", L, W, H)
        with self.assertRaises(ValueError):
            PartDesign.makeStarterShape(self.Body, "L", L, W, 1.5)
        with self.assertRaises(ValueError):
            PartDesign.makeStarterShape(self.Body, "Hat", L, W, 3)
        with self.assertRaises(ValueError):
            PartDesign.makeStarterShape(self.Body, "Box", L, W, H, FlangeWidth=1)
        with self.assertRaises(ValueError):
            PartDesign.makeStarterShape(self.Body, "Tub", 3, W, H)
        with self.assertRaises((TypeError, ValueError)):
            PartDesign.makeStarterShape(self.Doc.addObject("App::FeaturePython", "X"), "L", L, W, H)
        self.assertEqual(
            [o.TypeId for o in self.Doc.Objects if o.TypeId.startswith("PartDesign::SheetMetal")],
            [],
        )


if __name__ == "__main__":
    unittest.main()
