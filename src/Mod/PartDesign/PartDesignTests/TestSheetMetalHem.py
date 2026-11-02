# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's hem, PartDesign::SheetMetalHem (docs/SheetMetal.md sec 5.1,
# ruled 2026-11-02): a wall's end folded back over its skin. A closed hem is
# a 180 deg bend whose inner skins are the rule's Gap apart (0.1, so an
# inner radius of 0.05), an open one the same with a gap of its own; Length
# runs from the fold's outer face to the leg's end. Every volume below is
# worked by hand on a plate 40 x 20, 1 thick (z 0..1): a 180 deg bend of
# inner radius r is a half ring, (pi / 2) ((r + 1)^2 - r^2) in section.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector
# the closed hem's half ring at r = 0.05: (pi / 2) (1.05^2 - 0.05^2)
CLOSED = 0.55 * math.pi


class TestSheetMetalHem(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalHem")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def edge(self, feature, a, b):
        """The name of the edge of a feature's shape from a to b"""
        for i, e in enumerate(feature.Shape.Edges):
            ends = [v.Point for v in e.Vertexes]
            if len(ends) == 2 and (
                (ends[0].isEqual(V(*a), 1e-7) and ends[1].isEqual(V(*b), 1e-7))
                or (ends[0].isEqual(V(*b), 1e-7) and ends[1].isEqual(V(*a), 1e-7))
            ):
                return "Edge%d" % (i + 1)
        self.fail("no edge from %s to %s" % (a, b))

    def hem(self, edges, base=None, **props):
        base = base or self.Plate
        hem = self.Body.newObject("PartDesign::SheetMetalHem", "Hem")
        hem.Edges = (base, [self.edge(base, a, b) for a, b in edges])
        props.setdefault("Length", 6)
        for k, v in props.items():
            setattr(hem, k, v)
        self.Doc.recompute()
        return hem

    def far(self, **props):
        """A hem on the plate's far edge on its top skin"""
        return self.hem([((0, 20, 1), (40, 20, 1))], **props)

    def checkValid(self, feature):
        self.assertNotIn("Invalid", feature.State)
        self.assertTrue(feature.Shape.isValid())
        self.assertEqual(len(feature.Shape.Solids), 1)

    def owners(self, feature):
        return [feature.sheetElementOf("Face%d" % (i + 1)) for i in range(len(feature.Shape.Faces))]

    def checkFlat(self, feature, across, along, bends=1):
        """The flat lays out in one valid solid of the feature's volume
        (K 0.5), across x along"""
        flat = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "Flat")
        flat.Source = feature
        self.Doc.recompute()
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(len(flat.Shape.Solids), 1)
        solid = flat.Shape.Solids[0]
        self.assertTrue(solid.isValid())
        self.assertEqual(flat.BendCount, bends)
        self.assertAlmostEqual(solid.Volume, feature.Shape.Volume, places=6)
        box = solid.BoundBox
        lengths = sorted([box.XLength, box.YLength])
        self.assertAlmostEqual(lengths[0], min(across, along), places=6)
        self.assertAlmostEqual(lengths[1], max(across, along), places=6)
        return flat

    def testClosed(self):
        """The default, closed and Inside: the wall cut back 1.05 so the
        fold's outer face is where the wall ended, a half ring of inner
        radius 0.05, and the leg 6 - 1.05 long lying 0.1 over the skin; the
        flat 40 across, 18.95 + (0.05 + 0.5) pi + 4.95 along"""
        hem = self.far()
        self.assertEqual(hem.HemType, "Closed")
        self.assertEqual(hem.Position, "Inside")
        self.checkValid(hem)
        shape = hem.Shape
        self.assertAlmostEqual(shape.Volume, 40 * 20 - 40 * 1.05 + 40 * (CLOSED + 4.95), places=6)
        box = shape.BoundBox
        self.assertAlmostEqual(box.YMax, 20, places=6)
        self.assertAlmostEqual(box.ZMax, 2.1, places=6)
        self.assertAlmostEqual(box.ZMin, 0, places=6)
        self.assertTrue(shape.isInside(V(20, 15, 1.6), 1e-9, True))
        self.assertFalse(shape.isInside(V(20, 15, 1.05), 1e-9, True))
        self.assertFalse(shape.isInside(V(20, 13.9, 1.6), 1e-9, True))
        # in the fold, beside the gap's closed end
        self.assertTrue(shape.isInside(V(20, 19.9, 1.05), 1e-9, True))
        self.assertNotIn(None, self.owners(hem))
        self.checkFlat(hem, 40, 18.95 + 0.55 * math.pi + 4.95)

    def testOpen(self):
        """Open with a gap of 1: an inner radius of 0.5, the wall cut back
        1.5, the leg 4.5 long, 1 over the skin"""
        hem = self.far(HemType="Open", Gap=1)
        self.checkValid(hem)
        shape = hem.Shape
        self.assertAlmostEqual(
            shape.Volume, 40 * 20 - 40 * 1.5 + 40 * (math.pi + 4.5), places=6
        )
        self.assertAlmostEqual(shape.BoundBox.ZMax, 3, places=6)
        self.assertAlmostEqual(shape.BoundBox.YMax, 20, places=6)
        self.assertFalse(shape.isInside(V(20, 15, 1.5), 1e-9, True))
        self.assertTrue(shape.isInside(V(20, 15, 2.5), 1e-9, True))
        self.assertNotIn(None, self.owners(hem))
        self.checkFlat(hem, 40, 18.5 + math.pi + 4.5)

    def testGapOnlyWhenOpen(self):
        """A closed hem's gap is the rule's: Gap is read only then, and the
        rule's followed when it changes"""
        hem = self.far()
        self.assertIn("ReadOnly", hem.getPropertyStatus("Gap"))
        hem.HemType = "Open"
        self.assertNotIn("ReadOnly", hem.getPropertyStatus("Gap"))
        hem.HemType = "Closed"
        self.Doc.recompute()
        hem.Rule.Gap = 0.3
        self.Doc.recompute()
        self.checkValid(hem)
        # r = 0.15: (pi / 2) (1.15^2 - 0.15^2) = 0.65 pi, the leg 6 - 1.15
        self.assertAlmostEqual(
            hem.Shape.Volume, 40 * 20 - 40 * 1.15 + 40 * (0.65 * math.pi + 4.85), places=6
        )
        self.assertAlmostEqual(hem.Shape.BoundBox.ZMax, 2.3, places=6)

    def testOutside(self):
        """Outside: nothing cut back, the fold past the wall's end, its
        outer face at 21.05; the leg's end 6 back from there"""
        hem = self.far(Position="Outside")
        self.checkValid(hem)
        shape = hem.Shape
        self.assertAlmostEqual(shape.Volume, 40 * 20 + 40 * (CLOSED + 4.95), places=6)
        self.assertAlmostEqual(shape.BoundBox.YMax, 21.05, places=6)
        self.assertTrue(shape.isInside(V(20, 15.1, 1.6), 1e-9, True))
        self.assertFalse(shape.isInside(V(20, 15.0, 1.6), 1e-9, True))
        self.checkFlat(hem, 40, 20 + 0.55 * math.pi + 4.95)

    def testReversed(self):
        """Reversed: folded under the bottom skin"""
        hem = self.far(Reversed=True)
        self.checkValid(hem)
        box = hem.Shape.BoundBox
        self.assertAlmostEqual(box.ZMin, -1.1, places=6)
        self.assertAlmostEqual(box.ZMax, 1, places=6)
        self.assertAlmostEqual(
            hem.Shape.Volume, 40 * 20 - 40 * 1.05 + 40 * (CLOSED + 4.95), places=6
        )

    def testTwoEdges(self):
        """The plate's near and far edges, one hem each"""
        hem = self.hem([((0, 0, 1), (40, 0, 1)), ((0, 20, 1), (40, 20, 1))])
        self.checkValid(hem)
        self.assertAlmostEqual(
            hem.Shape.Volume, 40 * 20 - 2 * 40 * 1.05 + 2 * 40 * (CLOSED + 4.95), places=6
        )
        self.assertNotIn(None, self.owners(hem))

    def testCornerRefused(self):
        """Two edges meeting at a corner would fold into each other"""
        hem = self.hem([((0, 20, 1), (40, 20, 1)), ((40, 0, 1), (40, 20, 1))])
        self.assertIn("Invalid", hem.State)

    def testTooShort(self):
        """A length no longer than the fold leaves no leg"""
        hem = self.far(Length=1)
        self.assertIn("Invalid", hem.State)

    def testOnFlange(self):
        """On the end of a flange's leg (top at z 10, inner skin y 19),
        folded toward the plate: two bends in the flat"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Plate, [self.edge(self.Plate, (0, 20, 1), (40, 20, 1))])
        self.Doc.recompute()
        volume = flange.Shape.Volume
        hem = self.hem([((0, 19, 10), (40, 19, 10))], base=flange)
        self.checkValid(hem)
        self.assertAlmostEqual(
            hem.Shape.Volume, volume - 40 * 1.05 + 40 * (CLOSED + 4.95), places=6
        )
        self.assertAlmostEqual(hem.Shape.BoundBox.ZMax, 10, places=6)
        self.assertAlmostEqual(hem.Shape.BoundBox.YMin, 0, places=6)
        self.assertTrue(hem.Shape.isInside(V(20, 18.4, 7), 1e-9, True))
        self.assertNotIn(None, self.owners(hem))
        # the plate 18, the flange's bend (1 + 0.5) pi / 2 and leg 8 - 1.05,
        # the hem's bend (0.05 + 0.5) pi and leg 4.95
        self.checkFlat(
            hem, 40, 18 + 0.75 * math.pi + 6.95 + 0.55 * math.pi + 4.95, bends=2
        )

    def testSaved(self):
        """Saved and reopened, it recomputes to the same solid"""
        hem = self.far(HemType="Open", Gap=0.5)
        volume = hem.Shape.Volume
        path = os.path.join(tempfile.mkdtemp(), "hem.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        hem = self.Doc.getObject("Hem")
        self.assertEqual(hem.HemType, "Open")
        self.assertNotIn("ReadOnly", hem.getPropertyStatus("Gap"))
        hem.touch()
        self.Doc.recompute()
        self.checkValid(hem)
        self.assertAlmostEqual(hem.Shape.Volume, volume, places=6)
