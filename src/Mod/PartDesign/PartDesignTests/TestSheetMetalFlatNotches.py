# SPDX-License-Identifier: LGPL-2.1-or-later

# Notches at the bends (docs/SheetMetal.md sec 12, M4): the sheet's flat
# object cuts a small V or slot where each bend's line meets the blank's
# outer outline, for the press brake operator: Notch None (the default), V or
# Slot, NotchDepth along the line into the blank and NotchWidth across it at
# the outline. Only where the line ends on a straight piece of the outline
# near square to it: not at a hole or slot the line runs over, nor where it
# ends in a corner's miter or relief. The flat object's only, as its
# markings are: the folded part and every feature's flat know nothing of
# them. The bend's line stops at the notch; the DXF and SVG cut it.
#
# A plate 40 x 20, 1 thick, a flange 10 high on its far edge.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part
import PartDesign

from PartDesignTests.TestSheetMetalFlat import readDXF, readSVG

V = FreeCAD.Vector
FAR = ((0, 20, 1), (40, 20, 1))
RIGHT = ((40, 0, 1), (40, 20, 1))


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


def named(shape, prefix):
    """The edges of a shape whose names start with prefix, by name, a
    tangent line's left out"""
    res = {}
    for i in range(len(shape.Edges)):
        name = shape.getElementMappedName("Edge%d" % (i + 1))
        if name and name.startswith(prefix) and "Tangent" not in name:
            res[name.split(";")[0]] = shape.Edges[i]
    return res


class TestSheetMetalFlatNotches(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalFlatNotches")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.Tip = self.Plate
        self.Flange = self.flange("Flange", FAR)
        sheet = self.Body.getSolidBody(self.Flange)
        found = [
            o
            for o in self.Doc.Objects
            if o.isDerivedFrom("PartDesign::SheetMetalFlatPattern") and o.Sheet == sheet
        ]
        self.assertEqual(len(found), 1)
        self.Flat = found[0]
        self.Flat.Visibility = True
        self.Doc.recompute()

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def flange(self, name, edge):
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", name)
        flange.Edges = (self.Tip, [edgeOf(self.Tip, *edge)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        self.Tip = flange
        return flange

    def solid(self):
        self.assertEqual(self.Flat.Status, "")
        self.assertEqual(len(self.Flat.Shape.Solids), 1)
        solid = self.Flat.Shape.Solids[0]
        self.assertTrue(solid.isValid())
        return solid

    def line(self, name="Bend1"):
        """A bend's line in the flat: its ends, the one with the smaller x
        (then y) first"""
        edge = named(self.Flat.Shape, name)[name]
        ends = sorted([v.Point for v in edge.Vertexes], key=lambda p: (round(p.x, 6), p.y))
        return ends[0], ends[1]

    def notch(self, kind, depth=None, width=None):
        self.Flat.Notch = kind
        if depth is not None:
            self.Flat.NotchDepth = depth
        if width is not None:
            self.Flat.NotchWidth = width
        self.Doc.recompute()

    def mid(self, solid):
        zs = [v.Point.z for v in solid.Vertexes]
        return (min(zs) + max(zs)) / 2

    def inside(self, solid, p):
        return solid.isInside(V(p.x, p.y, self.mid(solid)), 1e-9, True)

    def testDefaults(self):
        """None, 1 deep, 1 wide: the blank as it was"""
        self.assertEqual(self.Flat.Notch, "None")
        self.assertEqual(self.Flat.getEnumerationsOfProperty("Notch"), ["None", "V", "Slot"])
        self.assertAlmostEqual(self.Flat.NotchDepth.Value, 1)
        self.assertAlmostEqual(self.Flat.NotchWidth.Value, 1)
        self.assertAlmostEqual(
            self.solid().Volume, self.Flange.getSubObject("Flat").Solids[0].Volume, places=9
        )

    def testV(self):
        """A V at each end of the line, its tip 1 in along the line, 1 wide at
        the outline: half of 1 x 1 out of the blank, through it, each"""
        volume = self.solid().Volume
        a, b = self.line()
        thickness = self.Flat.Thickness.Value
        self.notch("V")
        solid = self.solid()
        self.assertAlmostEqual(solid.Volume, volume - 2 * 0.5 * thickness, places=6)
        for end, other in ((a, b), (b, a)):
            u = (other - end).normalize()
            n = V(-u.y, u.x, 0)
            self.assertFalse(self.inside(solid, end + u * 0.5))
            self.assertTrue(self.inside(solid, end + u * 0.5 + n * 0.4))
            self.assertTrue(self.inside(solid, end + u * 0.5 - n * 0.4))
            self.assertTrue(self.inside(solid, end + u * 1.2))
            # its tip a vertex of the blank
            tip = end + u * 1
            self.assertTrue(
                any(math.hypot(v.Point.x - tip.x, v.Point.y - tip.y) < 1e-6 for v in solid.Vertexes)
            )

    def testSlot(self):
        """A slot 1 wide, 1 deep at each end"""
        volume = self.solid().Volume
        a, b = self.line()
        thickness = self.Flat.Thickness.Value
        self.notch("Slot")
        solid = self.solid()
        self.assertAlmostEqual(solid.Volume, volume - 2 * thickness, places=6)
        for end, other in ((a, b), (b, a)):
            u = (other - end).normalize()
            n = V(-u.y, u.x, 0)
            self.assertFalse(self.inside(solid, end + u * 0.5 + n * 0.4))
            self.assertTrue(self.inside(solid, end + u * 0.5 + n * 0.6))
            self.assertTrue(self.inside(solid, end + u * 1.2))

    def testSize(self):
        """NotchDepth and NotchWidth, changed, cut again"""
        volume = self.solid().Volume
        thickness = self.Flat.Thickness.Value
        self.notch("V")
        self.notch("V", depth=2, width=1.5)
        self.assertAlmostEqual(self.solid().Volume, volume - 2 * 1.5 * thickness, places=6)
        self.notch("Slot")
        self.assertAlmostEqual(self.solid().Volume, volume - 2 * 3 * thickness, places=6)
        self.notch("None")
        self.assertAlmostEqual(self.solid().Volume, volume, places=6)

    def testLineStops(self):
        """The bend's line runs from notch to notch"""
        a, b = self.line()
        self.notch("V", depth=2)
        c, d = self.line()
        u = (b - a).normalize()
        self.assertTrue(c.isEqual(a + u * 2, 1e-6))
        self.assertTrue(d.isEqual(b - u * 2, 1e-6))

    def testFlatOnly(self):
        """The folded part and every feature's flat know nothing of them;
        the flat object's area does"""
        volume = self.Flange.Shape.Volume
        flat = self.Flange.getSubObject("Flat").Solids[0].Volume
        area = self.Flat.FlatArea.Value
        self.notch("Slot")
        self.assertAlmostEqual(self.Flange.Shape.Volume, volume, places=9)
        self.assertAlmostEqual(self.Flange.getSubObject("Flat").Solids[0].Volume, flat, places=9)
        self.assertAlmostEqual(self.Flat.FlatArea.Value, area - 2, places=6)

    def testNotAtHole(self):
        """A cutout over the bend's middle parts its line in two; the line's
        ends at the hole are not notched"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "Profile")
        sketch.MapMode = "Deactivated"
        # from outside the far flange, across its leg's foot and its bend
        placement = FreeCAD.Placement(V(0, 30, 0), FreeCAD.Rotation(V(1, 0, 0), -90))
        sketch.Placement = placement
        inv = placement.inverse()
        pts = [inv.multVec(V(*p)) for p in [(10, 30, -1), (20, 30, -1), (20, 30, 6), (10, 30, 6)]]
        for i in range(4):
            sketch.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % 4]))
        cutout = self.Body.newObject("PartDesign::SheetMetalCutout", "Cutout")
        cutout.Profile = sketch
        self.Doc.recompute()
        self.assertNotIn("Invalid", cutout.State)
        self.Doc.recompute()
        volume = self.solid().Volume
        self.assertEqual(sorted(named(self.Flat.Shape, "Bend1")), ["Bend1", "Bend1Part2"])
        self.notch("V")
        thickness = self.Flat.Thickness.Value
        self.assertAlmostEqual(self.solid().Volume, volume - 2 * 0.5 * thickness, places=6)

    def testNotAtMiter(self):
        """Two flanges meeting at a corner: each line's end in the corner's
        miter has none, its other end has one"""
        self.flange("Right", RIGHT)
        self.Doc.recompute()
        volume = self.solid().Volume
        thickness = self.Flat.Thickness.Value
        self.notch("V")
        # the miter's faces are B-splines, measured to 1e-5
        self.assertAlmostEqual(self.solid().Volume, volume - 2 * 0.5 * thickness, places=4)

    def testRefused(self):
        """A notch of no depth or width is refused, the flat's Status saying
        so and the last flat kept"""
        self.notch("V")
        volume = self.solid().Volume
        self.notch("V", depth=0)
        self.assertIn("notch", self.Flat.Status)
        self.assertAlmostEqual(self.Flat.Shape.Solids[0].Volume, volume, places=9)
        self.notch("V", depth=1, width=0)
        self.assertIn("notch", self.Flat.Status)
        self.notch("V", width=1)
        self.assertEqual(self.Flat.Status, "")

    def testTangentLines(self):
        """A tangent line, wider of the bend's line than the notch, runs on"""
        self.Flat.TangentLines = True
        self.Doc.recompute()
        self.notch("V")
        tangents = [
            e
            for i, e in enumerate(self.Flat.Shape.Edges)
            if "Tangent" in (self.Flat.Shape.getElementMappedName("Edge%d" % (i + 1)) or "")
        ]
        self.assertEqual(len(tangents), 2)
        for e in tangents:
            self.assertAlmostEqual(e.Length, 40, places=6)

    def testExport(self):
        """The sheet's DXF and SVG outline the notches; a Plate's has none"""
        a, b = self.line()
        u = (b - a).normalize()
        tip = a + u * 1
        self.notch("V")

        def touches(entity):
            d = entity[2]
            for x, y in (("10", "20"), ("11", "21")):
                if x in d and math.hypot(float(d[x][0]) - tip.x, float(d[y][0]) - tip.y) < 1e-6:
                    return True
            return False

        with tempfile.TemporaryDirectory() as tmp:
            dxf = os.path.join(tmp, "flat.dxf")
            PartDesign.exportFlat(self.Body.getSolidBody(self.Flange), dxf)
            outline = [e for e in readDXF(dxf) if e[1] == "OUTLINE" and e[0] == "LINE"]
            self.assertEqual(len([e for e in outline if touches(e)]), 2)
            svg = os.path.join(tmp, "flat.svg")
            PartDesign.exportFlat(self.Flat, svg)
            layers, _ = readSVG(svg)
            self.assertTrue(layers["OUTLINE"])
            # hidden, the sheet's file still has them
            self.Flat.Visibility = False
            self.Doc.recompute()
            hidden = os.path.join(tmp, "hidden.dxf")
            PartDesign.exportFlat(self.Flange, hidden)
            outline = [e for e in readDXF(hidden) if e[1] == "OUTLINE" and e[0] == "LINE"]
            self.assertEqual(len([e for e in outline if touches(e)]), 2)


if __name__ == "__main__":
    unittest.main()
