# SPDX-License-Identifier: LGPL-2.1-or-later

# Cross break (docs/SheetMetal.md sec 12, M4): a feature of its own on picked
# walls, a face of each. The solid is not changed; the sheet's model gives
# each wall its break, Angle (3 deg by default) rising out of the face picked,
# or into it if Reversed. The flat draws the diagonals of each wall's region
# on the blank's top, named after the wall (Wall1CrossBreak1, ...2), Up or
# Down as the flat lies (the flat object's CrossBreaks); the DXF and SVG
# write them on the layer CROSS_BREAK with a note at the crossing. In 3D the
# diagonals lie on the face the break rises from (crossBreakLines()), and
# the wall's faces carry the break as a surface finish the feature implies:
# PartDesign.crossBreakFaces() lists them, for the Cross Break, every feature
# after it -- a Pocket too -- and the body; none for a feature before it, and
# none while it is suppressed.
#
# A plate 40 x 20, 1 thick, a flange 10 high on its far edge: the plate is
# Wall1, the flange's leg Wall2.

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
TOP = (20, 10, 1)
BOTTOM = (20, 10, 0)
LEG = (20, 20, 8)


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


def faceAt(feature, p):
    """The plane face of a feature's shape through p"""
    vertex = Part.Vertex(V(*p))
    for i, f in enumerate(feature.Shape.Faces):
        if isinstance(f.Surface, Part.Plane) and f.distToShape(vertex)[0] < 1e-7:
            return "Face%d" % (i + 1)
    raise ValueError("no face at %s" % (p,))


def named(shape, prefix):
    res = {}
    for i in range(len(shape.Edges)):
        name = shape.getElementMappedName("Edge%d" % (i + 1))
        if name and name.startswith(prefix):
            res[name.split(";")[0]] = shape.Edges[i]
    return res


class TestSheetMetalCrossBreak(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalCrossBreak")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Sketch = sketch
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.Flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        self.Flange.Edges = (self.Plate, [edgeOf(self.Plate, *FAR)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", self.Flange.State)

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def crossBreak(self, *points, **props):
        # faces of the tip, which the feature builds on
        base = self.Body.Tip
        feature = self.Body.newObject("PartDesign::SheetMetalCrossBreak", "CrossBreak")
        feature.Faces = (base, [faceAt(base, p) for p in points])
        for k, v in props.items():
            setattr(feature, k, v)
        self.Doc.recompute()
        return feature

    def flatObject(self):
        sheet = self.Body.getSolidBody(self.Flange)
        found = [
            o
            for o in self.Doc.Objects
            if o.isDerivedFrom("PartDesign::SheetMetalFlatPattern") and o.Sheet == sheet
        ]
        self.assertEqual(len(found), 1)
        found[0].Visibility = True
        self.Doc.recompute()
        self.assertEqual(found[0].Status, "")
        return found[0]

    def wall(self, feature, name):
        return [e for e in feature.SheetModel["Elements"] if e["Name"] == name][0]

    def checkDiagonals(self, edges, solid):
        """Two lines of one length, crossing at their middles, each from a
        vertex of the solid to another"""
        self.assertEqual(len(edges), 2)
        a, b = edges
        self.assertAlmostEqual(a.Length, b.Length, places=6)
        self.assertTrue(a.CenterOfMass.isEqual(b.CenterOfMass, 1e-6))
        for e in edges:
            for v in e.Vertexes:
                self.assertTrue(
                    any(v.Point.isEqual(w.Point, 1e-6) for w in solid.Vertexes), v.Point
                )

    def testSolidUnchanged(self):
        """Angle 3 deg, not Reversed; the solid as it was"""
        feature = self.crossBreak(TOP)
        self.assertNotIn("Invalid", feature.State)
        self.assertAlmostEqual(feature.Angle.Value, 3)
        self.assertFalse(feature.Reversed)
        self.assertAlmostEqual(feature.Shape.Volume, self.Flange.Shape.Volume, places=9)
        self.assertTrue(feature.Shape.BoundBox.isInside(self.Flange.Shape.BoundBox))
        self.assertEqual(len(feature.Shape.Faces), len(self.Flange.Shape.Faces))

    def testModel(self):
        """The wall's CrossBreak, deg, signed along its Normal: rising out
        of the face picked, into it Reversed"""
        feature = self.crossBreak(TOP)
        wall = self.wall(feature, "Wall1")
        self.assertAlmostEqual(wall["CrossBreak"], math.copysign(3, wall["Normal"].z))
        self.assertNotIn("CrossBreak", self.wall(feature, "Wall2"))
        self.assertNotIn("CrossBreak", self.wall(self.Flange, "Wall1"))
        feature.Reversed = True
        feature.Angle = 2
        self.Doc.recompute()
        wall = self.wall(feature, "Wall1")
        self.assertAlmostEqual(wall["CrossBreak"], -math.copysign(2, wall["Normal"].z))
        bottom = self.crossBreak(BOTTOM)
        wall = self.wall(bottom, "Wall1")
        self.assertAlmostEqual(wall["CrossBreak"], -math.copysign(3, wall["Normal"].z))

    def testLines(self):
        """In 3D the diagonals of the face picked"""
        feature = self.crossBreak(TOP)
        lines = feature.crossBreakLines()
        self.checkDiagonals(lines, feature.Shape)
        face = feature.Shape.getElement(faceAt(feature, TOP))
        for e in lines:
            self.assertLess(e.distToShape(face)[0], 1e-7)
            self.assertAlmostEqual(e.BoundBox.ZMin, 1, places=7)
            self.assertAlmostEqual(e.BoundBox.ZMax, 1, places=7)
        self.assertEqual(self.Flange.crossBreakLines(), [])

    def testFlat(self):
        """The flat object draws the wall's diagonals on the blank's top, Up
        where the break rises toward one looking down on it; the bend count
        is as it was"""
        flat = self.flatObject()
        bends = flat.BendCount
        self.crossBreak(TOP)
        flat = self.flatObject()
        lines = named(flat.Shape, "Wall1CrossBreak")
        self.assertEqual(sorted(lines), ["Wall1CrossBreak1", "Wall1CrossBreak2"])
        solid = flat.Shape.Solids[0]
        self.checkDiagonals(list(lines.values()), solid)
        top = max(v.Point.z for v in solid.Vertexes)
        for e in lines.values():
            self.assertAlmostEqual(e.BoundBox.ZMin, top, places=6)
            self.assertAlmostEqual(e.BoundBox.ZMax, top, places=6)
        self.assertEqual(flat.BendCount, bends)
        self.assertEqual(dict(flat.CrossBreaks), {"Wall1": "Up"})

    def testDirection(self):
        """Reversed, Down; the flat laid on its second skin turns it over"""
        feature = self.crossBreak(TOP, Reversed=True)
        flat = self.flatObject()
        self.assertEqual(dict(flat.CrossBreaks), {"Wall1": "Down"})
        feature.Reversed = False
        flat.SecondSkin = True
        self.Doc.recompute()
        self.assertEqual(dict(flat.CrossBreaks), {"Wall1": "Down"})

    def testFeatureFlat(self):
        """A feature's flat has its sheet's cross breaks too; one before it
        has none"""
        feature = self.crossBreak(TOP)
        self.assertEqual(len(named(feature.getSubObject("Flat"), "Wall1CrossBreak")), 2)
        self.assertEqual(named(self.Flange.getSubObject("Flat"), "Wall1CrossBreak"), {})

    def testTwoWalls(self):
        feature = self.crossBreak(TOP, LEG)
        self.assertEqual(len(feature.crossBreakLines()), 4)
        flat = self.flatObject()
        self.assertEqual(
            sorted(named(flat.Shape, "Wall")),
            ["Wall1CrossBreak1", "Wall1CrossBreak2", "Wall2CrossBreak1", "Wall2CrossBreak2"],
        )
        self.assertEqual(sorted(flat.CrossBreaks), ["Wall1", "Wall2"])

    def testFollowsEdit(self):
        """The plate made longer, the diagonals follow"""
        feature = self.crossBreak(TOP)
        before = feature.crossBreakLines()[0].Length
        self.Sketch.movePoint(1, 1, V(50, 0, 0))
        self.Sketch.movePoint(1, 2, V(50, 20, 0))
        self.Sketch.movePoint(0, 2, V(50, 0, 0))
        self.Sketch.movePoint(2, 1, V(50, 20, 0))
        self.Doc.recompute()
        self.assertNotIn("Invalid", feature.State)
        self.assertGreater(feature.crossBreakLines()[0].Length, before + 5)

    def testRefused(self):
        """Not on a bend; an angle above 0 and up to 45 deg"""
        bend = [
            "Face%d" % (i + 1)
            for i, f in enumerate(self.Flange.Shape.Faces)
            if isinstance(f.Surface, Part.Cylinder)
        ][0]
        feature = self.Body.newObject("PartDesign::SheetMetalCrossBreak", "CrossBreak")
        feature.Faces = (self.Flange, [bend])
        self.Doc.recompute()
        self.assertIn("Invalid", feature.State)
        feature.Faces = (self.Flange, [faceAt(self.Flange, TOP)])
        feature.Angle = 0
        self.Doc.recompute()
        self.assertIn("Invalid", feature.State)
        feature.Angle = 50
        self.Doc.recompute()
        self.assertIn("Invalid", feature.State)
        feature.Angle = 45
        self.Doc.recompute()
        self.assertNotIn("Invalid", feature.State)

    def testExport(self):
        """The DXF and SVG of the sheet: the diagonals on CROSS_BREAK, a note
        at their crossing"""
        self.crossBreak(TOP)
        with tempfile.TemporaryDirectory() as tmp:
            dxf = os.path.join(tmp, "flat.dxf")
            PartDesign.exportFlat(self.Body.getSolidBody(self.Flange), dxf)
            entities = readDXF(dxf)
            self.assertEqual([e[0] for e in entities if e[1] == "CROSS_BREAK"], ["LINE", "LINE"])
            notes = [e[2]["1"][0] for e in entities if e[1] == "LABELS" and e[0] == "TEXT"]
            self.assertIn("CROSS BREAK UP 3%%d", notes)
            svg = os.path.join(tmp, "flat.svg")
            PartDesign.exportFlat(self.Body.getSolidBody(self.Flange), svg)
            layers, _ = readSVG(svg)
            self.assertEqual([t for t, _, _ in layers["CROSS_BREAK"]], ["line", "line"])
            self.assertIn("CROSS BREAK UP 3\u00b0", [text for _, _, text in layers["LABELS"]])

    def checkFaces(self, obj, rises=True, angle=3):
        """Both skins of the plate, the one picked rising; the rectangle the
        wall's diagonals cross, of its long edge, in obj's shape"""
        faces = PartDesign.crossBreakFaces(obj)
        self.assertEqual(sorted(f["Wall"] for f in faces), ["Wall1", "Wall1"], faces)
        byZ = {round(obj.Shape.getElement(f["Face"]).CenterOfMass.z, 6): f for f in faces}
        self.assertEqual(sorted(byZ), [0, 1])
        self.assertEqual(byZ[1]["Rises"], rises)
        self.assertEqual(byZ[0]["Rises"], not rises)
        lines = self.Doc.getObject("CrossBreak").crossBreakLines()
        crossing = lines[0].CenterOfMass
        for f in faces:
            self.assertAlmostEqual(f["Angle"], angle)
            self.assertTrue(
                f["Direction"].isEqual(V(1, 0, 0), 1e-9)
                or f["Direction"].isEqual(V(-1, 0, 0), 1e-9),
                f,
            )
            self.assertTrue(f["Normal"].isEqual(V(0, 0, 1 if rises else -1), 1e-9), f)
            self.assertAlmostEqual(f["Length"], 40, places=6)
            self.assertAlmostEqual(math.hypot(f["Length"], f["Width"]), lines[0].Length, places=6)
            self.assertAlmostEqual(f["Center"].x, crossing.x, places=6)
            self.assertAlmostEqual(f["Center"].y, crossing.y, places=6)
        return faces

    def pocket(self):
        """A hole through the plate, after everything else"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "HoleSketch")
        sketch.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        sketch.addGeometry(Part.Circle(V(8, 5, 0), V(0, 0, 1), 2))
        pocket = self.Body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = sketch
        pocket.Type = "ThroughAll"
        self.Doc.recompute()
        self.assertNotIn("Invalid", pocket.State)
        self.assertLess(pocket.Shape.Volume, pocket.BaseFeature.Shape.Volume - 1)
        return pocket

    def testFaces(self):
        """The wall's two skins carry the break; Reversed, the other rises"""
        self.assertEqual(PartDesign.crossBreakFaces(self.Flange), [])
        feature = self.crossBreak(TOP)
        self.checkFaces(feature)
        feature.Reversed = True
        feature.Angle = 2
        self.Doc.recompute()
        self.checkFaces(feature, rises=False, angle=2)

    def testFacesPassOn(self):
        """A Pocket after it, the body, a flange after that: the faces left of
        the wall still carry it, the rectangle the wall's"""
        self.crossBreak(TOP)
        pocket = self.pocket()
        self.checkFaces(pocket)
        self.checkFaces(self.Body)
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange001")
        flange.Edges = (pocket, [edgeOf(pocket, (0, 0, 1), (40, 0, 1))])
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        faces = PartDesign.crossBreakFaces(flange)
        self.assertEqual(sorted(f["Rises"] for f in faces), [False, True], faces)
        self.assertEqual(PartDesign.crossBreakFaces(self.Plate), [])

    def testFacesPlaced(self):
        """In the shape's own coordinates, the body moved"""
        self.crossBreak(TOP)
        self.Body.Placement = FreeCAD.Placement(V(5, 7, 11), FreeCAD.Rotation(V(0, 0, 1), 90))
        self.Doc.recompute()
        faces = PartDesign.crossBreakFaces(self.Body)
        self.assertEqual(len(faces), 2)
        for f in faces:
            face = self.Body.Shape.getElement(f["Face"])
            self.assertLess(face.distToShape(Part.Vertex(f["Center"]))[0], 1e-6)
            self.assertTrue(
                f["Direction"].isEqual(V(0, 1, 0), 1e-9)
                or f["Direction"].isEqual(V(0, -1, 0), 1e-9),
                f,
            )

    def testSuppressed(self):
        """Suppressed, it breaks nothing: no faces, no lines, nothing in the
        flat"""
        feature = self.crossBreak(TOP)
        pocket = self.pocket()
        feature.Suppress = True
        self.Doc.recompute()
        self.assertEqual(PartDesign.crossBreakFaces(feature), [])
        self.assertEqual(PartDesign.crossBreakFaces(pocket), [])
        self.assertEqual(PartDesign.crossBreakFaces(self.Body), [])
        self.assertEqual(feature.crossBreakLines(), [])
        flat = self.flatObject()
        self.assertEqual(dict(flat.CrossBreaks), {})
        self.assertEqual(named(flat.Shape, "Wall1CrossBreak"), {})
        feature.Suppress = False
        self.Doc.recompute()
        self.assertEqual(len(PartDesign.crossBreakFaces(self.Body)), 2)
        self.assertEqual(dict(self.flatObject().CrossBreaks), {"Wall1": "Up"})

    def testSaveRestore(self):
        name = self.crossBreak(TOP, Angle=2.5).Name
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "crossbreak.FCStd")
            self.Doc.saveAs(path)
            FreeCAD.closeDocument(self.Doc.Name)
            self.Doc = FreeCAD.openDocument(path)
            feature = self.Doc.getObject(name)
            wall = [e for e in feature.SheetModel["Elements"] if e["Name"] == "Wall1"][0]
            self.assertAlmostEqual(abs(wall["CrossBreak"]), 2.5)
            self.Doc.recompute()
            self.assertNotIn("Invalid", feature.State)
            self.assertEqual(len(feature.crossBreakLines()), 2)


if __name__ == "__main__":
    unittest.main()
