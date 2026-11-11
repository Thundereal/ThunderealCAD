# SPDX-License-Identifier: LGPL-2.1-or-later

# Forms (docs/SheetMetal.md sec 4.8, M5, ruled 2026-11-06): a form is pressed
# into a wall at each point of a sketch, an element of its own owned by that
# wall and carried rigidly into the flat. PartDesign::SheetMetalForm, Kind
# "Tool" for a user's tool -- any shape and a face of it that sits on the
# wall's skin, taken as the form's outside and hollowed to the thickness, the
# hollow opened down through the wall -- or a built-in one ("Dimple"). The
# flat object shows forms Formed, as an Outline or as a Centre mark
# (FormDisplay, Outline at first), the outline and the mark on the FORMS
# layer of the DXF.
#
# A plate 40 x 30, 1 thick; a cylinder 4 in radius and 3 high for the tool,
# pressed up out of the plate's top at (10, 10) and (28, 18).

import math
import os
import tempfile
import unittest

import FreeCAD
import Part
import PartDesign

from PartDesignTests.TestSheetMetalFlat import readDXF

from . import SheetMetalNames

V = FreeCAD.Vector
POINTS = [(10, 10), (28, 18)]
T = 1.0
PLATE = 40 * 30 * T
# a cylinder r 4, h 3 on the top, hollowed to r 3 and 2 high, opened through
# the plate: 48 pi - 18 pi - 9 pi each
TOOL_FORM = 21 * math.pi


def faceAt(feature, p, normal=None):
    """The plane face of a feature's shape through p, facing normal"""
    vertex = Part.Vertex(V(*p))
    for i, f in enumerate(feature.Shape.Faces):
        if not isinstance(f.Surface, Part.Plane) or f.distToShape(vertex)[0] > 1e-7:
            continue
        if normal is not None and not f.normalAt(0, 0).isEqual(V(*normal), 1e-7):
            continue
        return "Face%d" % (i + 1)
    raise ValueError("no face at %s" % (p,))


def named(shape, prefix):
    res = {}
    for i in range(len(shape.Edges)):
        name = shape.getElementMappedName("Edge%d" % (i + 1))
        if name and name.startswith(prefix):
            res[name.split(";")[0]] = shape.Edges[i]
    return res


class TestSheetMetalForm(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalForm")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 30), (0, 30)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Sketch = sketch
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.Tool = self.Doc.addObject("Part::Cylinder", "Tool")
        self.Tool.Radius = 4
        self.Tool.Height = 3
        self.Doc.recompute()

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def points(self, pts=POINTS, z=T):
        sketch = self.Body.newObject("Sketcher::SketchObject", "Points")
        sketch.Placement = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
        for x, y in pts:
            sketch.addGeometry(Part.Point(V(x, y, 0)))
        self.Doc.recompute()
        return sketch

    def toolBase(self):
        return faceAt(self.Tool, (0, 0, 0), (0, 0, -1))

    def form(self, base=None, face=None, **props):
        base = base or self.Body.Tip
        positions = props.pop("Positions", None) or self.points()
        feature = self.Body.newObject("PartDesign::SheetMetalForm", "Form")
        feature.Face = (base, [face or faceAt(base, (20, 15, T), (0, 0, 1))])
        feature.Positions = positions
        if props.get("Kind", "Tool") == "Tool" and "Tool" not in props:
            feature.Tool = (self.Tool, [self.toolBase()])
        for k, v in props.items():
            setattr(feature, k, v)
        self.Doc.recompute()
        return feature

    def flatObject(self):
        sheet = self.Body.getSolidBody(self.Plate)
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

    def testTool(self):
        """A user's tool at each point: the formed outside on the top, the
        hollow opened through the plate; one valid solid"""
        feature = self.form()
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.Volume, PLATE + 2 * TOOL_FORM, places=6)
        box = shape.optimalBoundingBox(False)
        self.assertAlmostEqual(box.ZMax, T + 3, places=6)
        for x, y in POINTS:
            # the form's top, its hollow, the opening through the plate
            self.assertTrue(shape.isInside(V(x, y, T + 2.5), 1e-7, True))
            self.assertFalse(shape.isInside(V(x, y, T + 1.5), 1e-7, True))
            self.assertFalse(shape.isInside(V(x, y, T / 2), 1e-7, True))
            self.assertTrue(shape.isInside(V(x + 3.5, y, T + 1.5), 1e-7, True))
        self.assertTrue(shape.isInside(V(20, 28, T / 2), 1e-7, True))

    def testModel(self):
        """A form an element of its own a point, owned by its wall: its faces
        are the form's"""
        feature = self.form()
        forms = feature.SheetModel["Forms"]
        self.assertEqual([f["Name"] for f in forms], ["Form1", "Form2"])
        self.assertEqual({f["Wall"] for f in forms}, {"Wall1"})
        for f, (x, y) in zip(forms, POINTS):
            self.assertTrue(f["Center"].isEqual(V(x, y, T), 1e-7), f)
            self.assertTrue(f["Normal"].isEqual(V(0, 0, 1), 1e-7), f)
        top = faceAt(feature, (10, 10, T + 3))
        self.assertEqual(feature.sheetElementOf(top), "Form1")
        self.assertEqual(feature.sheetElementOf(faceAt(feature, (20, 28, T))), "Wall1")
        self.assertNotIn("Forms", self.Plate.SheetModel)

    def testFlatOutline(self):
        """Outline, as it starts: the flat is the plate, the forms' outlines on
        its top, and on FORMS in the DXF"""
        feature = self.form()
        flat = self.flatObject()
        self.assertEqual(flat.FormDisplay, "Outline")
        solid = flat.Shape.Solids[0]
        self.assertAlmostEqual(solid.Volume, PLATE, places=6)
        outlines = named(flat.Shape, "Form")
        self.assertEqual(sorted({n.split("Outline")[0] for n in outlines}), ["Form1", "Form2"])
        top = solid.BoundBox.ZMax
        centres = []
        for name, e in outlines.items():
            self.assertIn("Outline", name)
            self.assertAlmostEqual(e.BoundBox.ZMin, top, places=6)
            self.assertAlmostEqual(e.BoundBox.ZMax, top, places=6)
            centres.append(e.BoundBox.Center)
        # the tool's base: a circle 4 in radius about each point, as laid
        lengths = sorted(
            round(sum(e.Length for n, e in outlines.items() if n.startswith(f)), 6)
            for f in ("Form1", "Form2")
        )
        self.assertAlmostEqual(lengths[0], 8 * math.pi, places=5)
        self.assertAlmostEqual(lengths[1], 8 * math.pi, places=5)
        with tempfile.TemporaryDirectory() as tmp:
            dxf = os.path.join(tmp, "flat.dxf")
            PartDesign.exportFlat(self.Body.getSolidBody(self.Plate), dxf)
            layers = {e[1] for e in readDXF(dxf)}
            self.assertIn("FORMS", layers)

    def testFlatFormed(self):
        """Formed: the forms kept, moved with their wall"""
        feature = self.form()
        flat = self.flatObject()
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        solid = flat.Shape.Solids[0]
        self.assertAlmostEqual(solid.Volume, feature.Shape.Volume, places=6)
        self.assertEqual(named(flat.Shape, "Form"), {})
        self.assertAlmostEqual(solid.BoundBox.ZLength, T + 3, places=6)

    def testFlatCentreMark(self):
        """Centre mark: the plate, and a cross at each form's middle"""
        self.form()
        flat = self.flatObject()
        flat.FormDisplay = "CentreMark"
        self.Doc.recompute()
        solid = flat.Shape.Solids[0]
        self.assertAlmostEqual(solid.Volume, PLATE, places=6)
        marks = named(flat.Shape, "Form")
        self.assertEqual(sorted(marks), ["Form1Mark1", "Form1Mark2", "Form2Mark1", "Form2Mark2"])
        for f in ("Form1", "Form2"):
            a, b = marks[f + "Mark1"], marks[f + "Mark2"]
            self.assertTrue(a.CenterOfMass.isEqual(b.CenterOfMass, 1e-6))
            self.assertAlmostEqual(abs(a.Curve.Direction.dot(b.Curve.Direction)), 0, places=7)

    def testReversed(self):
        """Reversed: pressed into the face, down through the plate"""
        feature = self.form(Reversed=True)
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertAlmostEqual(shape.Volume, PLATE + 2 * TOOL_FORM, places=6)
        box = shape.optimalBoundingBox(False)
        self.assertAlmostEqual(box.ZMin, -3, places=6)
        self.assertAlmostEqual(box.ZMax, T, places=6)

    def testAngle(self):
        """Turned about the face's normal by Angle: a box tool turned 90"""
        self.Tool = self.Doc.addObject("Part::Box", "BoxTool")
        self.Tool.Length, self.Tool.Width, self.Tool.Height = 8, 4, 2
        self.Tool.Placement.Base = V(-4, -2, 0)
        self.Doc.recompute()
        feature = self.form(Positions=self.points([(20, 15)]), Angle=90)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isInside(V(20, 18.5, T + 1.5), 1e-7, True))
        self.assertFalse(feature.Shape.isInside(V(23.5, 15, T + 1.5), 1e-7, True))

    def testOnLeg(self):
        """A form on a flange's leg, carried into the flat with it"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        edges = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(self.Plate.Shape.Edges)
            if all(abs(v.Point.y - 30) < 1e-7 and abs(v.Point.z - T) < 1e-7 for v in e.Vertexes)
        ]
        flange.Edges = (self.Plate, edges[:1])
        flange.Length = 20
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        # the leg's outer skin, y 30, facing +Y; a point on it
        leg = faceAt(flange, (20, 30, 12), (0, 1, 0))
        sketch = self.Body.newObject("Sketcher::SketchObject", "LegPoints")
        sketch.Placement = FreeCAD.Placement(V(0, 30, 0), FreeCAD.Rotation(V(1, 0, 0), 90))
        sketch.addGeometry(Part.Point(V(20, 12, 0)))
        self.Doc.recompute()
        feature = self.form(base=flange, face=leg, Positions=sketch)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertAlmostEqual(feature.Shape.optimalBoundingBox(False).YMax, 33, places=6)
        self.assertEqual({f["Wall"] for f in feature.SheetModel["Forms"]}, {"Wall2"})
        flat = self.flatObject()
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, feature.Shape.Volume, places=5)

    def testDimple(self):
        """A built-in dimple: its top Depth above the face, the sheet's
        thickness all through it, opened beneath"""
        feature = self.form(Kind="Dimple", Diameter=12, Depth=2, Radius=1)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        box = shape.optimalBoundingBox(False)
        self.assertAlmostEqual(box.ZMax, T + 2, places=6)
        for x, y in POINTS:
            self.assertTrue(shape.isInside(V(x, y, T + 1.5), 1e-7, True))
            self.assertFalse(shape.isInside(V(x, y, T + 0.5), 1e-7, True))
            self.assertFalse(shape.isInside(V(x, y, T / 2), 1e-7, True))
            # past its rim the plate as it was
            self.assertTrue(shape.isInside(V(x + 6.5, y, T / 2), 1e-7, True))
        # a sheet's volume, the area it covers about the same: its slant
        # walls a little more than the plate they replace
        self.assertGreater(shape.Volume, PLATE)
        self.assertLess(shape.Volume, PLATE + 2 * math.pi * 6 * 6 * T)
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)

    def louver(self, **props):
        """A louver at the plate's middle, (20, 15): 20 long along X, 6 from
        its hinge to its opening, which faces +Y, its lip 3 above the face"""
        sizes = dict(Length=20, Width=6, Depth=3, Radius=0.5, FlankAngle=45)
        sizes.update(props)
        return self.form(Kind="Louver", Positions=self.points([(20, 15)]), **sizes)

    def testLouver(self):
        """A built-in louver: the sheet cut along one long side, the strip
        risen as a hood, a ramp from its hinge to the opening Depth above the
        face, its ends drawn down to the face at FlankAngle; the thickness all
        through it, the plate beneath it open"""
        feature = self.louver()
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 3, places=6)
        # the ramp, 3 over 6, so its top 2.75 above the face 2.5 in from the
        # middle toward the opening, the thickness square to it 1.118 deep
        self.assertTrue(shape.isInside(V(20, 17.5, T + 2.25), 1e-7, True))
        self.assertFalse(shape.isInside(V(20, 17.5, T + 0.5), 1e-7, True))
        self.assertFalse(shape.isInside(V(20, 17.5, T / 2), 1e-7, True))
        # open beneath the lip, the plate in front of it as it was
        self.assertFalse(shape.isInside(V(20, 17.9, T + 1), 1e-7, True))
        self.assertTrue(shape.isInside(V(20, 18.5, T / 2), 1e-7, True))
        self.assertFalse(shape.isInside(V(20, 18.5, T + 0.1), 1e-7, True))
        # an end, drawn: closed, sloping to the face at 45 deg
        self.assertTrue(shape.isInside(V(29.6, 17, T + 0.2), 1e-7, True))
        self.assertFalse(shape.isInside(V(29.6, 17, T + 0.6), 1e-7, True))
        # behind the hinge, the plate as it was
        self.assertTrue(shape.isInside(V(20, 11.5, T / 2), 1e-7, True))
        forms = feature.SheetModel["Forms"]
        self.assertEqual([(f["Name"], f["Wall"]) for f in forms], [("Form1", "Wall1")])
        self.assertEqual(feature.sheetElementOf(faceAt(feature, (20, 18, T + 3))), "Form1")
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testLouverTurned(self):
        """Angle 90: the louver along Y, opening toward -X"""
        feature = self.louver(Angle=90)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isInside(V(17.5, 15, T + 2.25), 1e-7, True))
        self.assertFalse(feature.Shape.isInside(V(17.9, 15, T + 1), 1e-7, True))
        self.assertTrue(feature.Shape.isInside(V(11.5, 15, T / 2), 1e-7, True))

    def testLouverSharp(self):
        """Radius 0: its bends sharp"""
        feature = self.louver(Radius=0)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())

    def testLouverRefused(self):
        """Ends meeting before the lip's height, no room inside under the
        lip for the thickness, a size of 0"""
        for props in (
            dict(Length=4),
            dict(Depth=1),
            dict(Width=0),
            dict(Length=0),
            dict(Depth=0),
        ):
            with self.subTest(**props):
                feature = self.louver(**props)
                self.assertIn("Invalid", feature.State)
                self.Doc.removeObject(feature.Name)

    def bead(self, **props):
        """A bead at the plate's middle, (20, 15): 24 long end to end along
        X where it leaves the face, 6 across, 2 high, so straight for 18
        between its round ends"""
        sizes = dict(Length=24, Diameter=6, Depth=2, Radius=0.5, FlankAngle=45)
        sizes.update(props)
        return self.form(Kind="Bead", Positions=self.points([(20, 15)]), **sizes)

    def testBead(self):
        """A built-in bead: a dimple drawn out along the form's X, its sides
        and round ends at FlankAngle, its bends rounded, the thickness all
        through it, opened beneath"""
        feature = self.bead()
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 2, places=6)
        # its top, 1 across at 45 deg; its hollow; open beneath
        for x in (20, 20 + 8, 20 + 9.5):
            self.assertTrue(shape.isInside(V(x, 15, T + 1.5), 1e-7, True), x)
            self.assertFalse(shape.isInside(V(x, 15, T + 0.5), 1e-7, True), x)
            self.assertFalse(shape.isInside(V(x, 15, T / 2), 1e-7, True), x)
        # past its end and its side, the plate as it was
        self.assertTrue(shape.isInside(V(20 + 12.5, 15, T / 2), 1e-7, True))
        self.assertTrue(shape.isInside(V(20, 15 + 3.5, T / 2), 1e-7, True))
        self.assertFalse(shape.isInside(V(20 + 12.5, 15, T + 0.1), 1e-7, True))
        forms = feature.SheetModel["Forms"]
        self.assertEqual([(f["Name"], f["Wall"]) for f in forms], [("Form1", "Wall1")])
        self.assertEqual(feature.sheetElementOf(faceAt(feature, (20, 15, T + 2))), "Form1")
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testBeadTurned(self):
        """Angle 90: along Y"""
        feature = self.bead(Angle=90)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isInside(V(20, 15 + 9.5, T + 1.5), 1e-7, True))
        self.assertTrue(feature.Shape.isInside(V(20 + 9.5, 15, T / 2), 1e-7, True))

    def testBeadRefused(self):
        """No longer than it is wide, closing to a point first, a size of 0"""
        for props in (dict(Length=6), dict(Depth=4), dict(Diameter=0), dict(Depth=0)):
            with self.subTest(**props):
                feature = self.bead(**props)
                self.assertIn("Invalid", feature.State)
                self.Doc.removeObject(feature.Name)

    # A bead along a line or an arc of the sketch, in place of straight at a
    # point (built 2026-11-11): each open chain of the sketch is a bead's
    # path, the bead reaching Diameter / 2 past its ends; Length and Angle
    # are the path's.

    def drawing(self, edges, z=T, name="Drawing"):
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        sketch.Placement = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
        for edge in edges:
            sketch.addGeometry(edge)
        self.Doc.recompute()
        return sketch

    def volumeOf(self, feature):
        """A built-in form's volume, the form taken out again"""
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        volume = feature.Shape.Volume
        self.Body.removeObject(feature)
        self.Doc.removeObject(feature.Name)
        self.Doc.recompute()
        return volume

    def alongPath(self, edges, **props):
        sizes = dict(Diameter=6, Depth=2, Radius=0.5, FlankAngle=45)
        sizes.update(props)
        return self.form(Kind="Bead", Positions=self.drawing(edges), **sizes)

    def testBeadAlongLine(self):
        """A line 18 long through the plate's middle: the bead of testBead,
        24 end to end"""
        volume = self.volumeOf(self.bead())
        feature = self.alongPath([Part.LineSegment(V(11, 15, 0), V(29, 15, 0))])
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.Volume, volume, places=6)
        for x in (20, 20 + 8, 20 + 9.5):
            self.assertTrue(shape.isInside(V(x, 15, T + 1.5), 1e-7, True), x)
            self.assertFalse(shape.isInside(V(x, 15, T + 0.5), 1e-7, True), x)
            self.assertFalse(shape.isInside(V(x, 15, T / 2), 1e-7, True), x)
        self.assertTrue(shape.isInside(V(20 + 12.5, 15, T / 2), 1e-7, True))
        # the one form, numbered past the one made and taken out before it
        forms = feature.SheetModel["Forms"]
        self.assertEqual([(f["Name"], f["Wall"]) for f in forms], [("Form2", "Wall1")])
        self.assertEqual(feature.sheetElementOf(faceAt(feature, (20, 15, T + 2))), "Form2")
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testBeadAlongSlantedLine(self):
        """A line at 30 deg: the bead of that Angle"""
        volume = self.volumeOf(self.bead(Angle=30))
        c, s = 9 * math.cos(math.radians(30)), 9 * math.sin(math.radians(30))
        feature = self.alongPath([Part.LineSegment(V(20 - c, 15 - s, 0), V(20 + c, 15 + s, 0))])
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())
        self.assertAlmostEqual(feature.Shape.Volume, volume, places=6)

    def testBeadAlongArc(self):
        """An arc of radius 12 about (20, 5), 120 deg: its section the
        bead's all along, so (Pappus: the section is the same either side of
        the arc) the volume of a straight bead as long as the arc"""
        length = 12 * math.radians(120)
        volume = self.volumeOf(self.bead(Length=length + 6))
        circle = Part.Circle(V(20, 5, 0), V(0, 0, 1), 12)
        feature = self.alongPath(
            [Part.ArcOfCircle(circle, math.radians(30), math.radians(150))]
        )
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 2, places=6)
        self.assertAlmostEqual(shape.Volume, volume, places=5)
        # on the arc: its top, its hollow, open beneath
        for a in (35, 90, 140):
            x = 20 + 12 * math.cos(math.radians(a))
            y = 5 + 12 * math.sin(math.radians(a))
            self.assertTrue(shape.isInside(V(x, y, T + 1.5), 1e-7, True), a)
            self.assertFalse(shape.isInside(V(x, y, T + 0.5), 1e-7, True), a)
            self.assertFalse(shape.isInside(V(x, y, T / 2), 1e-7, True), a)
        # inside the arc and outside it, the plate as it was
        for y in (5 + 12 - 3.6, 5 + 12 + 3.6):
            self.assertTrue(shape.isInside(V(20, y, T / 2), 1e-7, True), y)
            self.assertFalse(shape.isInside(V(20, y, T + 0.1), 1e-7, True), y)
        # numbered past the bead made and taken out before it
        self.assertEqual(feature.sheetElementOf(faceAt(feature, (20, 17, T + 2))), "Form2")
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testBeadAlongChain(self):
        """A line running into an arc, tangent: one bead"""
        circle = Part.Circle(V(24, 9, 0), V(0, 0, 1), 9)
        feature = self.alongPath(
            [
                Part.LineSegment(V(10, 18, 0), V(24, 18, 0)),
                Part.ArcOfCircle(circle, math.radians(30), math.radians(90)),
            ]
        )
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertEqual(len(feature.SheetModel["Forms"]), 1)
        straight = self.volumeOf(feature)
        length = 14 + 9 * math.radians(60)
        self.assertAlmostEqual(straight, self.volumeOf(self.bead(Length=length + 6)), places=5)

    def testBeadAlongPathRefused(self):
        """A path with a corner, an arc rounder than the bead is wide, a
        path off the wall"""
        tight = Part.Circle(V(20, 15, 0), V(0, 0, 1), 2)
        for edges in (
            [Part.LineSegment(V(10, 10, 0), V(20, 10, 0)), Part.LineSegment(V(20, 10, 0), V(20, 20, 0))],
            [Part.ArcOfCircle(tight, 0, math.radians(180))],
            [Part.LineSegment(V(20, 15, 0), V(39, 15, 0))],
        ):
            with self.subTest(edges=len(edges)):
                feature = self.alongPath(edges)
                self.assertIn("Invalid", feature.State)
                self.Doc.removeObject(feature.Name)

    def lance(self, **props):
        """A lance at the plate's middle, (20, 15): 20 long along X, its tab
        6 from its hinge to its end, which faces +Y and rises 3 above the
        face"""
        sizes = dict(Length=20, Width=6, Depth=3, Radius=0.5)
        sizes.update(props)
        return self.form(Kind="Lance", Positions=self.points([(20, 15)]), **sizes)

    def testLance(self):
        """A built-in lance: the sheet cut on three sides, the tab between
        bent up along the fourth, its hinge, to its end Depth above the face;
        free at its sides, the thickness all through it, the plate beneath
        it open"""
        feature = self.lance()
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 3, places=6)
        # the tab as the louver's ramp, 3 over 6; open beneath
        self.assertTrue(shape.isInside(V(20, 17.5, T + 2.25), 1e-7, True))
        self.assertFalse(shape.isInside(V(20, 17.5, T + 0.5), 1e-7, True))
        self.assertFalse(shape.isInside(V(20, 17.5, T / 2), 1e-7, True))
        # its sides cut free, not drawn: the tab to x 30, a slit the rule's
        # gap wide beside it, the plate past that
        self.assertTrue(shape.isInside(V(29.9, 17, T + 2), 1e-7, True))
        self.assertFalse(shape.isInside(V(30.05, 17, T + 2), 1e-7, True))
        self.assertFalse(shape.isInside(V(29.6, 17, T + 0.2), 1e-7, True))
        self.assertFalse(shape.isInside(V(30.05, 17, T / 2), 1e-7, True))
        self.assertTrue(shape.isInside(V(30.5, 17, T / 2), 1e-7, True))
        # behind the hinge, the plate as it was
        self.assertTrue(shape.isInside(V(20, 11.5, T / 2), 1e-7, True))
        forms = feature.SheetModel["Forms"]
        self.assertEqual([(f["Name"], f["Wall"]) for f in forms], [("Form1", "Wall1")])
        self.assertEqual(feature.sheetElementOf(faceAt(feature, (20, 18, T + 3))), "Form1")
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testLanceSharp(self):
        """Radius 0: its hinge sharp"""
        feature = self.lance(Radius=0)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())

    def testLanceRefused(self):
        """A tab too shallow to clear the sheet, a size of 0"""
        for props in (dict(Depth=1), dict(Width=0), dict(Length=0), dict(Depth=0)):
            with self.subTest(**props):
                feature = self.lance(**props)
                self.assertIn("Invalid", feature.State)
                self.Doc.removeObject(feature.Name)

    def drawn(self, kind, **props):
        sizes = dict(Diameter=12, Length=12, Depth=3, Radius=0.5, FlankAngle=60)
        sizes.update(props)
        return self.form(Kind=kind, Positions=self.points([(20, 15)]), **sizes)

    def testDrawnCutout(self):
        """A built-in drawn cutout: a dimple open at its top, its slant at
        FlankAngle, its rim's bend rounded, the thickness all through it;
        round as long as it is wide"""
        feature = self.drawn("DrawnCutout")
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 3, places=6)
        # open all through at its middle
        for z in (T / 2, T + 1.5, T + 2.9):
            self.assertFalse(shape.isInside(V(20, 15, z), 1e-7, True), z)
        # its wall 1.5 up: outside 6 - 1.5 / tan 60, the thickness / sin 60
        # less inside, 5.134 and 3.979 from its axis
        self.assertTrue(shape.isInside(V(24.5, 15, T + 1.5), 1e-7, True))
        self.assertFalse(shape.isInside(V(23.5, 15, T + 1.5), 1e-7, True))
        self.assertFalse(shape.isInside(V(25.5, 15, T + 1.5), 1e-7, True))
        forms = feature.SheetModel["Forms"]
        self.assertEqual([(f["Name"], f["Wall"]) for f in forms], [("Form1", "Wall1")])
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testDrawnCutoutLong(self):
        """Longer than it is wide: drawn out along the form's X as a bead"""
        feature = self.drawn("DrawnCutout", Length=24)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertFalse(feature.Shape.isInside(V(26, 15, T + 1.5), 1e-7, True))
        self.assertTrue(feature.Shape.isInside(V(20, 19.5, T + 1.5), 1e-7, True))
        self.assertTrue(feature.Shape.isInside(V(30.5, 15, T + 1.5), 1e-7, True))

    def testFlangedHole(self):
        """A built-in flanged hole: a hole Diameter across, its rim drawn up
        into a straight collar Depth above the face, its bend rounded"""
        feature = self.drawn("FlangedHole", Diameter=8)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 3, places=6)
        for z in (T / 2, T + 1.5):
            self.assertFalse(shape.isInside(V(20, 15, z), 1e-7, True), z)
        # its collar from 4 to 5 off its axis, straight
        for z in (T + 1.5, T + 2.9):
            self.assertTrue(shape.isInside(V(24.5, 15, z), 1e-7, True), z)
            self.assertFalse(shape.isInside(V(23.9, 15, z), 1e-7, True), z)
            self.assertFalse(shape.isInside(V(25.1, 15, z), 1e-7, True), z)
        # past its bend, the plate as it was
        self.assertTrue(shape.isInside(V(25.8, 15, T / 2), 1e-7, True))
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)

    def testDrawnRefused(self):
        """A drawn cutout closing at its top, one shorter than it is wide, a
        flanged hole's collar no higher than its bend, a size of 0"""
        cases = [
            ("DrawnCutout", dict(Diameter=6, Length=6, FlankAngle=45)),
            ("DrawnCutout", dict(Length=8)),
            ("DrawnCutout", dict(Depth=0)),
            ("FlangedHole", dict(Depth=0.5)),
            ("FlangedHole", dict(Diameter=0)),
        ]
        for kind, props in cases:
            with self.subTest(kind=kind, **props):
                feature = self.drawn(kind, **props)
                self.assertIn("Invalid", feature.State)
                self.Doc.removeObject(feature.Name)

    def emboss(self, **props):
        """An emboss at the plate's middle, (20, 15): 20 by 12 where it
        leaves the face, its corners 3 round there, 2 high at 60 deg"""
        sizes = dict(Length=20, Width=12, CornerRadius=3, Depth=2, Radius=0.5, FlankAngle=60)
        sizes.update(props)
        return self.form(Kind="Emboss", Positions=self.points([(20, 15)]), **sizes)

    def testEmboss(self):
        """A built-in emboss: a flat-topped rounded rectangle raised Depth
        above the face, its sides at FlankAngle, its bends rounded, the
        thickness all through it, opened beneath"""
        feature = self.emboss()
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 2, places=6)
        # its top; open beneath
        for x, y in ((20, 15), (26, 19)):
            self.assertTrue(shape.isInside(V(x, y, T + 1.5), 1e-7, True))
            self.assertFalse(shape.isInside(V(x, y, T + 0.5), 1e-7, True))
            self.assertFalse(shape.isInside(V(x, y, T / 2), 1e-7, True))
        # its end 0.6 up, clear of its bends: outside 10 - 0.6 / tan 60, the
        # thickness / sin 60 less inside, 9.654 and 8.499 from its middle
        self.assertTrue(shape.isInside(V(29, 15, T + 0.6), 1e-7, True))
        self.assertFalse(shape.isInside(V(28.2, 15, T + 0.6), 1e-7, True))
        self.assertFalse(shape.isInside(V(29.9, 15, T + 0.6), 1e-7, True))
        # past it, the plate as it was
        self.assertTrue(shape.isInside(V(30.8, 15, T / 2), 1e-7, True))
        self.assertTrue(shape.isInside(V(20, 21.8, T / 2), 1e-7, True))
        forms = feature.SheetModel["Forms"]
        self.assertEqual([(f["Name"], f["Wall"]) for f in forms], [("Form1", "Wall1")])
        self.assertEqual(feature.sheetElementOf(faceAt(feature, (20, 15, T + 2))), "Form1")
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testEmbossSharp(self):
        """Radius 0: its bends sharp"""
        feature = self.emboss(Radius=0)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())

    def testEmbossRefused(self):
        """Corners rounder than it is wide, corners closing at its slant
        before its top, no room inside, a size of 0"""
        for props in (
            dict(CornerRadius=7),
            dict(CornerRadius=1),
            dict(Width=4, CornerRadius=1.5),
            dict(Depth=0),
        ):
            with self.subTest(**props):
                feature = self.emboss(**props)
                self.assertIn("Invalid", feature.State)
                self.Doc.removeObject(feature.Name)

    # An emboss of a sketch's profile, in place of a rounded rectangle at a
    # point (built 2026-11-11): each closed chain of the sketch is where an
    # emboss leaves the face; its corners drawn sharp are rounded by
    # CornerRadius.

    def rounded(self, x0, y0, x1, y1, r):
        """A rectangle with corners r round, as a sketch's edges"""
        up = V(0, 0, 1)
        q = math.pi / 2
        edges = [
            Part.LineSegment(V(x0 + r, y0, 0), V(x1 - r, y0, 0)),
            Part.LineSegment(V(x1, y0 + r, 0), V(x1, y1 - r, 0)),
            Part.LineSegment(V(x1 - r, y1, 0), V(x0 + r, y1, 0)),
            Part.LineSegment(V(x0, y1 - r, 0), V(x0, y0 + r, 0)),
        ]
        for (cx, cy), a in (
            ((x1 - r, y0 + r), -q),
            ((x1 - r, y1 - r), 0),
            ((x0 + r, y1 - r), q),
            ((x0 + r, y0 + r), 2 * q),
        ):
            edges.append(Part.ArcOfCircle(Part.Circle(V(cx, cy, 0), up, r), a, a + q))
        return edges

    def polygon(self, pts):
        return [
            Part.LineSegment(V(*pts[i], 0), V(*pts[(i + 1) % len(pts)], 0)) for i in range(len(pts))
        ]

    def profiled(self, edges, **props):
        sizes = dict(CornerRadius=3, Depth=2, Radius=0.5, FlankAngle=60)
        sizes.update(props)
        return self.form(Kind="Emboss", Positions=self.drawing(edges), **sizes)

    def testEmbossProfile(self):
        """The rounded rectangle of testEmboss drawn: the same emboss"""
        volume = self.volumeOf(self.emboss())
        feature = self.profiled(self.rounded(10, 9, 30, 21, 3), CornerRadius=0)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.Volume, volume, places=6)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 2, places=6)
        self.assertTrue(shape.isInside(V(29, 15, T + 0.6), 1e-7, True))
        self.assertFalse(shape.isInside(V(28.2, 15, T + 0.6), 1e-7, True))
        self.assertFalse(shape.isInside(V(29.9, 15, T + 0.6), 1e-7, True))
        # the one form, numbered past the one made and taken out before it
        forms = feature.SheetModel["Forms"]
        self.assertEqual([(f["Name"], f["Wall"]) for f in forms], [("Form2", "Wall1")])
        self.assertEqual(feature.sheetElementOf(faceAt(feature, (20, 15, T + 2))), "Form2")
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testEmbossProfileSharp(self):
        """The rectangle drawn with sharp corners, CornerRadius 3: the same"""
        volume = self.volumeOf(self.emboss())
        feature = self.profiled(self.polygon([(10, 9), (30, 9), (30, 21), (10, 21)]))
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())
        self.assertAlmostEqual(feature.Shape.Volume, volume, places=6)

    def testEmbossProfileL(self):
        """An L, its inside corner rounded as the others: valid, the
        thickness through it, the plate as it was in the L's notch"""
        feature = self.profiled(
            self.polygon([(6, 5), (34, 5), (34, 15), (20, 15), (20, 25), (6, 25)]),
            FlankAngle=75,
        )
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.optimalBoundingBox(False).ZMax, T + 2, places=6)
        for x, y in ((12, 10), (28, 10), (12, 20)):
            self.assertTrue(shape.isInside(V(x, y, T + 1.5), 1e-7, True), (x, y))
            self.assertFalse(shape.isInside(V(x, y, T + 0.5), 1e-7, True), (x, y))
            self.assertFalse(shape.isInside(V(x, y, T / 2), 1e-7, True), (x, y))
        self.assertTrue(shape.isInside(V(27, 20, T / 2), 1e-7, True))
        self.assertFalse(shape.isInside(V(27, 20, T + 0.1), 1e-7, True))
        self.assertEqual(len(feature.SheetModel["Forms"]), 1)
        flat = self.flatObject()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, PLATE, places=6)
        flat.FormDisplay = "Formed"
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, shape.Volume, places=6)

    def testEmbossProfileRefused(self):
        """Sharp corners and no CornerRadius, corners closing at the slant
        before the top, a profile off the wall"""
        box = [(10, 9), (30, 9), (30, 21), (10, 21)]
        for edges, props in (
            (self.polygon(box), dict(CornerRadius=0)),
            (self.polygon(box), dict(CornerRadius=1)),
            (self.polygon([(20, 9), (41, 9), (41, 21), (20, 21)]), dict()),
        ):
            with self.subTest(**props):
                feature = self.profiled(edges, **props)
                self.assertIn("Invalid", feature.State)
                self.Doc.removeObject(feature.Name)

    def bracket(self):
        """The plate with a leg 20 up on its far edge, y 30: its bend's
        inner skins the plate's top, z 1, and the leg's inside, y 29, their
        virtual sharp the line y 29, z 1"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        edges = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(self.Plate.Shape.Edges)
            if all(abs(v.Point.y - 30) < 1e-7 and abs(v.Point.z - T) < 1e-7 for v in e.Vertexes)
        ]
        flange.Edges = (self.Plate, edges[:1])
        flange.Length = 20
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        return flange

    def gusset(self, flange, **props):
        """Gussets across the bracket's bend at x 10 and 30: each reaching 6
        along both walls from their sharp, 3 wide at its face"""
        bend = [
            "Face%d" % (i + 1)
            for i, f in enumerate(flange.Shape.Faces)
            if isinstance(f.Surface, Part.Cylinder) and abs(f.Surface.Radius - 1) < 1e-7
        ][0]
        positions = props.pop("Positions", None) or self.points([(10, 5), (30, 5)])
        sizes = dict(Length=6, Width=3, Radius=0.5, FlankAngle=60)
        sizes.update(props)
        return self.form(base=flange, face=bend, Kind="Gusset", Positions=positions, **sizes)

    def testGusset(self):
        """A built-in gusset: a rib pressed in across a bend from its
        outside, its face a flat across the inside corner, Length along each
        wall from their sharp, Width wide there, its ends sloping back at
        FlankAngle; the thickness all through it, open behind. The only form
        across a bend: owned by the bend"""
        flange = self.bracket()
        flat = self.flatObject()
        plain = flat.Shape.Solids[0].Volume
        feature = self.gusset(flange)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        shape = feature.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        # its face 6 sin 45 = 4.243 from the sharp toward the inside, the
        # thickness behind it; open behind, where the bend's corner was
        u = V(0, -1, 1).normalize()
        sharp = V(10, 29, T)
        self.assertTrue(shape.isInside(sharp + u * 3.74, 1e-7, True))
        self.assertFalse(shape.isInside(sharp + u * 4.74, 1e-7, True))
        self.assertFalse(shape.isInside(sharp, 1e-7, True))
        self.assertFalse(shape.isInside(V(10, 27, T / 2), 1e-7, True))
        # between the two, the bend as it was
        self.assertTrue(shape.isInside(V(20, 29, T), 1e-7, True))
        self.assertTrue(shape.isInside(V(20, 27, T / 2), 1e-7, True))
        forms = feature.SheetModel["Forms"]
        self.assertEqual(
            [(f["Name"], f["Wall"]) for f in forms], [("Form1", "Bend1"), ("Form2", "Bend1")]
        )
        # its face is the form's
        face = faceAt(feature, sharp + u * (6 * math.sin(math.pi / 4)))
        self.assertEqual(feature.sheetElementOf(face), "Form1")
        # the flat: the bend's flat as it was, the gussets' outlines on it;
        # Formed cannot lay a gusset flat, and shows its outline too
        for mode in ("Outline", "Formed"):
            with self.subTest(mode=mode):
                flat.FormDisplay = mode
                self.Doc.recompute()
                self.assertEqual(flat.Status, "")
                self.assertEqual(len(flat.Shape.Solids), 1)
                self.assertAlmostEqual(flat.Shape.Solids[0].Volume, plain, places=5)
                outlines = named(flat.Shape, "Form")
                self.assertEqual(
                    sorted({n.split("Outline")[0] for n in outlines}), ["Form1", "Form2"]
                )
        flat.FormDisplay = "CentreMark"
        self.Doc.recompute()
        self.assertEqual(flat.Status, "")
        self.assertEqual(len(named(flat.Shape, "Form")), 4)

    def testGussetSharpBehindTheBend(self):
        """A bend whose sharp is behind its outer skin -- R 3 at 90 deg, and
        135 deg -- valid, rounded, and flat (found 2026-11-06: the outside's
        corner stood out past the bend there, and its ends left slivers
        behind it that would not round)"""
        for angle, radius, length in ((90, 3, 8), (135, 1, 8)):
            with self.subTest(angle=angle, radius=radius):
                self.Plate.Rule.BendRadius = radius
                flange = self.bracket()
                flange.Angle = angle
                self.Doc.recompute()
                self.assertNotIn("Invalid", flange.State)
                bend = [
                    "Face%d" % (i + 1)
                    for i, f in enumerate(flange.Shape.Faces)
                    if isinstance(f.Surface, Part.Cylinder)
                    and abs(f.Surface.Radius - radius) < 1e-7
                ][0]
                feature = self.form(
                    base=flange,
                    face=bend,
                    Kind="Gusset",
                    Positions=self.points([(20, 5)]),
                    Length=length,
                    Width=3,
                    Radius=0.3,
                    FlankAngle=60,
                )
                self.assertNotIn("Invalid", feature.State, feature.getStatusString())
                self.assertTrue(feature.Shape.isValid())
                self.assertEqual(len(feature.Shape.Solids), 1)
                flat = self.flatObject()
                self.assertEqual(flat.Status, "")
                self.assertEqual(len(flat.Shape.Solids), 1)
                self.tearDown()
                self.setUp()

    def testGussetSaveRestore(self):
        """Saved and opened again, its form still the bend's (found
        2026-11-06: the model's restore refused a form on no wall)"""
        name = self.gusset(self.bracket()).Name
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "gusset.FCStd")
            self.Doc.saveAs(path)
            FreeCAD.closeDocument(self.Doc.Name)
            self.Doc = FreeCAD.openDocument(path)
            feature = self.Doc.getObject(name)
            self.assertEqual(
                [(f["Name"], f["Wall"]) for f in feature.SheetModel["Forms"]],
                [("Form1", "Bend1"), ("Form2", "Bend1")],
            )
            self.Doc.recompute()
            self.assertNotIn("Invalid", feature.State, feature.getStatusString())

    def testGussetSharp(self):
        """Radius 0: its bends sharp"""
        feature = self.gusset(self.bracket(), Radius=0)
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())

    def testGussetRefused(self):
        """On a wall's face, not a bend's; no further along a wall than the
        bend's inside; past a wall's end; off the bend's length; a size of 0"""
        flange = self.bracket()
        feature = self.gusset(flange, Length=1)
        self.assertIn("Invalid", feature.State)
        self.Doc.removeObject(feature.Name)
        for props in (dict(Length=25), dict(Width=0), dict(Length=0)):
            with self.subTest(**props):
                feature = self.gusset(flange, **props)
                self.assertIn("Invalid", feature.State)
                self.Doc.removeObject(feature.Name)
        feature = self.gusset(flange, Positions=self.points([(45, 5)]))
        self.assertIn("Invalid", feature.State)
        self.Doc.removeObject(feature.Name)
        feature = self.form(base=flange, Kind="Gusset", Positions=self.points([(10, 5)]))
        self.assertIn("Invalid", feature.State)

    def testSizesFromThickness(self):
        """A built-in form's sizes start from the sheet's thickness: the
        rule's 2 mm doubles them; set before, they are kept"""
        self.Plate.Rule.Thickness = 2
        self.Doc.recompute()
        feature = self.Body.newObject("PartDesign::SheetMetalForm", "Form")
        sizes = ("Diameter", "Depth", "Radius", "Length", "Width", "CornerRadius")
        self.assertEqual(
            [getattr(feature, p).Value for p in sizes], [20.0, 4.0, 2.0, 40.0, 12.0, 6.0]
        )
        self.assertEqual(feature.FlankAngle.Value, 45)
        kept = self.Doc.addObject("PartDesign::SheetMetalForm", "Kept")
        kept.Diameter = 7
        self.Body.addObject(kept)
        self.assertEqual(kept.Diameter.Value, 7)
        self.assertEqual(kept.Depth.Value, 4)

    def testFollowsEdit(self):
        """A point moved, the form moves"""
        positions = self.points()
        feature = self.form(Positions=positions)
        positions.movePoint(0, 1, V(15, 12, 0))
        self.Doc.recompute()
        self.assertNotIn("Invalid", feature.State)
        self.assertTrue(feature.Shape.isInside(V(15, 12, T + 2.5), 1e-7, True))
        self.assertFalse(feature.Shape.isInside(V(10, 10, T + 2.5), 1e-7, True))

    def testRefused(self):
        """A bend's face, no point, no tool, a point off the face"""
        feature = self.form(Positions=self.points([(20, 15)]))
        self.assertNotIn("Invalid", feature.State)
        feature.Tool = None
        self.Doc.recompute()
        self.assertIn("Invalid", feature.State)
        feature.Tool = (self.Tool, [self.toolBase()])
        feature.Positions = self.points([(80, 80)])
        self.Doc.recompute()
        self.assertIn("Invalid", feature.State)
        feature.Positions = self.Body.newObject("Sketcher::SketchObject", "Empty")
        self.Doc.recompute()
        self.assertIn("Invalid", feature.State)

    def testNamesHold(self):
        """A form keeps its name with the forms before it suppressed, and
        back; one made after takes the body's next number, not one a form
        deleted had (2026-11-11, as a wall's and a bend's)"""

        def names(feature):
            return [f["Name"] for f in feature.SheetModel["Forms"]]

        first = self.form(Kind="Dimple")
        self.assertNotIn("Invalid", first.State, first.getStatusString())
        second = self.form(Kind="Dimple", Positions=self.points([(32, 6)]))
        self.assertNotIn("Invalid", second.State, second.getStatusString())
        self.assertEqual(names(second), ["Form1", "Form2", "Form3"])
        self.assertEqual(list(second.FormNumbers), [3])
        first.Suppress = True
        self.Doc.recompute()
        self.assertNotIn("Invalid", second.State, second.getStatusString())
        self.assertEqual(names(second), ["Form3"])
        self.assertEqual(second.sheetElementOf(faceAt(second, (32, 6, T + 2))), "Form3")
        first.Suppress = False
        self.Doc.recompute()
        self.assertEqual(names(second), ["Form1", "Form2", "Form3"])
        self.Body.removeObject(second)
        self.Doc.removeObject(second.Name)
        self.Doc.recompute()
        third = self.form(Kind="Dimple", Positions=self.points([(32, 6)]))
        self.assertNotIn("Invalid", third.State, third.getStatusString())
        self.assertEqual(names(third), ["Form1", "Form2", "Form4"])

    def testSaveRestore(self):
        name = self.form().Name
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "form.FCStd")
            self.Doc.saveAs(path)
            FreeCAD.closeDocument(self.Doc.Name)
            self.Doc = FreeCAD.openDocument(path)
            feature = self.Doc.getObject(name)
            self.assertEqual([f["Name"] for f in feature.SheetModel["Forms"]], ["Form1", "Form2"])
            self.Doc.recompute()
            self.assertNotIn("Invalid", feature.State)
            self.assertAlmostEqual(feature.Shape.Volume, PLATE + 2 * TOOL_FORM, places=6)


if __name__ == "__main__":
    unittest.main()
