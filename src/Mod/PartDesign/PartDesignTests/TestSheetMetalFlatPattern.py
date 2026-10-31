# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's flat pattern, PartDesign::SheetMetalFlatPattern
# (docs/SheetMetal.md sec 4.7): every face of the folded part sent through
# its element's map, a root wall laid on XY. The flat lengths below are the
# hand sums of the straight lengths and of each bend's allowance
# A (R + K T), at 1e-6.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector


def ba(angle=90, R=1.0, T=1.0, K=0.5):
    return math.radians(angle) * (R + K * T)


class TestSheetMetalFlatPattern(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalFlatPattern")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def sketch(self, points, closed=False, name="Sketch"):
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        n = len(points) if closed else len(points) - 1
        for i in range(n):
            a, b = points[i], points[(i + 1) % len(points)]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        return sketch

    def wall(self, points, closed=False, **props):
        wall = self.Body.newObject("PartDesign::SheetMetalBaseWall", "BaseWall")
        wall.Profile = self.sketch(points, closed)
        for k, v in props.items():
            setattr(wall, k, v)
        self.Doc.recompute()
        return wall

    def flat(self, source, root=None):
        flat = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "FlatPattern")
        flat.Source = source
        if root:
            flat.RootFace = (source, [root])
        self.Doc.recompute()
        return flat

    def solid(self, flat):
        self.assertNotIn("Invalid", flat.State)
        solids = flat.Shape.Solids
        self.assertEqual(len(solids), 1)
        self.assertTrue(solids[0].isValid())
        return solids[0]

    def lines(self, flat):
        return flat.Shape.SubShapes[1].Edges

    def extents(self, shape):
        box = shape.BoundBox
        return sorted([box.XLength, box.YLength, box.ZLength])

    def checkFlat(self, flat, length, width, T=1.0):
        solid = self.solid(flat)
        box = solid.BoundBox
        # on XY, the material above
        self.assertAlmostEqual(box.ZMin, 0, places=6)
        self.assertAlmostEqual(box.ZMax, T, places=6)
        flat = sorted([box.XLength, box.YLength])
        for a, b in zip(flat, sorted([length, width])):
            self.assertAlmostEqual(a, b, places=6)
        return solid

    def testPlate(self):
        """A plate lies flat as it is"""
        wall = self.wall([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True)
        flat = self.flat(wall)
        solid = self.checkFlat(flat, 20, 10)
        self.assertAlmostEqual(solid.Volume, 200, places=6)
        self.assertEqual(len(self.lines(flat)), 0)

    def testL(self):
        """An L of inner legs 19 and 9, 15 wide: 19 + BA + 9 long; at K 0.5
        the flat keeps the folded volume"""
        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        flat = self.flat(wall)
        solid = self.checkFlat(flat, 19 + ba() + 9, 15)
        self.assertAlmostEqual(solid.Volume, wall.Shape.Volume, places=6)
        lines = self.lines(flat)
        self.assertEqual(len(lines), 1)
        self.assertAlmostEqual(lines[0].Length, 15, places=6)
        # on the flat's top or bottom
        z = lines[0].Vertexes[0].Point.z
        self.assertTrue(abs(z) < 1e-6 or abs(z - 1) < 1e-6, z)

    def testU(self):
        wall = self.wall([(0, 10), (0, 0), (30, 0), (30, 10)], Length=8)
        flat = self.flat(wall)
        # the sketch on the inner skin: legs 9, 28, 9
        self.checkFlat(flat, 9 + ba() + 28 + ba() + 9, 8)
        self.assertEqual(len(self.lines(flat)), 2)

    def testKFactor(self):
        """The rule's K is the flat's"""
        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        wall.Rule.KFactor = 0.3
        self.Doc.recompute()
        flat = self.flat(wall)
        self.checkFlat(flat, 19 + ba(K=0.3) + 9, 15)

    def testFlange(self):
        """A plate and a flange: 18 + BA + 8"""
        plate = self.wall([(0, 0), (40, 0), (40, 20), (0, 20)], closed=True)
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        edge = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(plate.Shape.Edges)
            if all(abs(v.Point.y - 20) < 1e-7 and abs(v.Point.z - 1) < 1e-7 for v in e.Vertexes)
        ]
        flange.Edges = (plate, edge)
        self.Doc.recompute()
        flat = self.flat(flange)
        solid = self.checkFlat(flat, 18 + ba() + 8, 40)
        self.assertAlmostEqual(solid.Volume, flange.Shape.Volume, places=6)

    def testRootFace(self):
        """Laid from another wall, the same flat moved"""
        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        first = self.extents(self.solid(self.flat(wall)))
        # the second leg's outer skin, x = 21
        root = [
            "Face%d" % (i + 1)
            for i, f in enumerate(wall.Shape.Faces)
            if abs(f.BoundBox.XMin - 21) < 1e-7 and abs(f.BoundBox.XMax - 21) < 1e-7
        ]
        self.assertEqual(len(root), 1)
        flat = self.flat(wall, root[0])
        second = self.solid(flat)
        for a, b in zip(first, self.extents(second)):
            self.assertAlmostEqual(a, b, places=6)
        self.assertAlmostEqual(second.BoundBox.ZMin, 0, places=6)

    def testHoleCarried(self):
        """A pocket through a wall is no sheet metal feature: its faces are
        placed by where they lie, and the hole is in the flat"""
        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        sketch = self.Body.newObject("Sketcher::SketchObject", "HoleSketch")
        sketch.Placement = FreeCAD.Placement(V(0, 0, 7.5), FreeCAD.Rotation(V(1, 0, 0), 90))
        sketch.addGeometry(Part.Circle(V(8, 0, 0), V(0, 0, 1), 2))
        pocket = self.Body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = sketch
        pocket.Type = "ThroughAll"
        pocket.Midplane = True
        self.Doc.recompute()
        self.assertNotIn("Invalid", pocket.State)
        self.assertLess(pocket.Shape.Volume, wall.Shape.Volume - 1)
        flat = self.flat(pocket)
        solid = self.checkFlat(flat, 19 + ba() + 9, 15)
        self.assertAlmostEqual(solid.Volume, pocket.Shape.Volume, places=6)

    def testLive(self):
        """The flat recomputes with the part"""
        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        flat = self.flat(wall)
        wall.Length = 25
        self.Doc.recompute()
        self.assertNotIn("Touched", flat.State)
        self.checkFlat(flat, 19 + ba() + 9, 25)
        self.assertEqual(self.Body.Tip, wall)

    def testFaceNotOwned(self):
        """A face no wall or bend owns says which"""
        wall = self.wall([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True)
        box = self.Body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 4
        self.Doc.recompute()
        flat = self.flat(box)
        self.assertIn("Invalid", flat.State)

    def testManufacturing(self):
        """What a bill of materials takes (sec 5.4): the flat's size and area,
        thickness, material, bend count and largest angle"""
        import Materials

        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        card = Materials.Material()
        card.Name = "Sheet test"
        wall.Rule.ShapeMaterial = card
        flat = self.flat(wall)
        self.assertAlmostEqual(flat.FlatLength.Value, 19 + ba() + 9, places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, 15, places=6)
        self.assertAlmostEqual(flat.FlatArea.Value, (19 + ba() + 9) * 15, places=6)
        self.assertAlmostEqual(flat.Thickness.Value, 1)
        self.assertEqual(flat.MaterialName, "Sheet test")
        self.assertEqual(flat.BendCount, 1)
        self.assertAlmostEqual(flat.MaxBendAngle.Value, 90)
        for name in ("FlatLength", "FlatWidth", "FlatArea", "BendCount", "MaxBendAngle"):
            self.assertIn("ReadOnly", flat.getEditorMode(name))

    def testBendDirections(self):
        """Up where the material comes up toward one looking down on the
        flat; turned over, every bend the other way"""
        wall = self.wall([(0, 0), (20, 0), (20, 10), (40, 10)], Length=15)
        flat = self.flat(wall)
        self.assertEqual(flat.BendDirections, {"Bend1": "Up", "Bend2": "Down"})
        # the first wall's other skin, y = 0, laid down
        root = [
            "Face%d" % (i + 1)
            for i, f in enumerate(wall.Shape.Faces)
            if abs(f.BoundBox.YMin) < 1e-7 and abs(f.BoundBox.YMax) < 1e-7
        ]
        self.assertEqual(len(root), 1)
        flat.RootFace = (wall, root)
        self.Doc.recompute()
        self.assertEqual(flat.BendDirections, {"Bend1": "Down", "Bend2": "Up"})

    def readDXF(self, path):
        """The entities of a DXF file: (type, layer, {code: value})"""
        with open(path) as f:
            lines = [line.strip() for line in f]
        pairs = list(zip(lines[0::2], lines[1::2]))
        entities, current, inside = [], None, False
        for code, value in pairs:
            if code == "2" and value == "ENTITIES":
                inside = True
                continue
            if not inside:
                continue
            if code == "0":
                if current:
                    entities.append(current)
                current = None if value in ("ENDSEC", "EOF", "SEQEND") else [value, "", {}]
                if value == "ENDSEC":
                    inside = False
            elif current is not None:
                if code == "8":
                    current[1] = value
                else:
                    current[2].setdefault(code, []).append(value)
        return entities

    def exportDXF(self, flat):
        path = os.path.join(tempfile.mkdtemp(), "flat.dxf")
        flat.exportDXF(path)
        return self.readDXF(path)

    def testDXF(self):
        """The outline, the bend lines up and down on layers of their own,
        each bend labelled"""
        wall = self.wall([(0, 0), (20, 0), (20, 10), (40, 10)], Length=15)
        flat = self.flat(wall)
        ents = self.exportDXF(flat)
        outline = [e for e in ents if e[1] == "OUTLINE"]
        self.assertEqual({e[0] for e in outline}, {"LINE"})
        self.assertEqual(len(outline), 4)
        xs = [float(x) for e in outline for x in e[2]["10"] + e[2]["11"]]
        ys = [float(y) for e in outline for y in e[2]["20"] + e[2]["21"]]
        size = sorted([max(xs) - min(xs), max(ys) - min(ys)])
        self.assertAlmostEqual(size[1], flat.FlatLength.Value, places=6)
        self.assertAlmostEqual(size[0], flat.FlatWidth.Value, places=6)
        self.assertEqual(len([e for e in ents if e[1] == "BEND_UP" and e[0] == "LINE"]), 1)
        self.assertEqual(len([e for e in ents if e[1] == "BEND_DOWN" and e[0] == "LINE"]), 1)
        labels = sorted(e[2]["1"][0] for e in ents if e[1] == "LABELS" and e[0] == "TEXT")
        self.assertEqual(labels, ["Bend1", "Bend2"])
        self.assertEqual([e for e in ents if e[1] == "INNER"], [])

    def testDXFHole(self):
        """A round hole is a circle on the inner layer"""
        wall = self.wall([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True)
        sketch = self.Body.newObject("Sketcher::SketchObject", "HoleSketch")
        sketch.addGeometry(Part.Circle(V(8, 5, 0), V(0, 0, 1), 2))
        pocket = self.Body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = sketch
        pocket.Type = "ThroughAll"
        pocket.Midplane = True
        self.Doc.recompute()
        flat = self.flat(pocket)
        inner = [e for e in self.exportDXF(flat) if e[1] == "INNER"]
        self.assertEqual([e[0] for e in inner], ["CIRCLE"])
        self.assertAlmostEqual(float(inner[0][2]["40"][0]), 2, places=6)

    def testSaveAndRestore(self):
        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        flat = self.flat(wall)
        volume = self.solid(flat).Volume
        path = os.path.join(tempfile.mkdtemp(), "flat.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        flat = self.Doc.getObject("FlatPattern")
        self.assertAlmostEqual(self.solid(flat).Volume, volume, places=6)
        flat.touch()
        self.Doc.recompute()
        self.assertAlmostEqual(self.solid(flat).Volume, volume, places=6)
