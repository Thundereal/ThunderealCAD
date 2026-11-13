# SPDX-License-Identifier: LGPL-2.1-or-later

"""An element the kernel tells nothing of keeps its name over a change made
elsewhere in the model (docs/TopoNamingEnhance.md sec 9).

A draft gives history for the faces it turns and none for the edges between
them. Such an edge was named from one face and its place among that face's
unnamed edges, so a hole added to the sketch, which gives the top and bottom
faces an edge more each, moved the names along: on this very model four
references to the draft's edges then found another edge, and no error said so
(scripts/name-stability, case draft_fillet, before the change). An edge is now
named from all the faces it lies between.
"""

import unittest

import FreeCAD
import Part

V = FreeCAD.Vector


def signature(element):
    """What an element is known by over a change: where it is"""
    if element.ShapeType == "Vertex":
        return ("V",) + tuple(round(c, 6) for c in element.Point)
    box = element.BoundBox
    size = element.Length if element.ShapeType == "Edge" else element.Area
    return (element.ShapeType[0], round(size, 6)) + tuple(
        round(c, 6) for c in (box.XMin, box.YMin, box.ZMin, box.XMax, box.YMax, box.ZMax)
    )


def names(obj):
    """signature -> mapped name, of every edge and vertex of the object's shape"""
    shape = obj.Shape
    result = {}
    for kind, elements in (("Edge", shape.Edges), ("Vertex", shape.Vertexes)):
        for i, element in enumerate(elements):
            sig = signature(element)
            # two elements at one place are not told apart by it
            result[sig] = (
                None if sig in result else shape.getElementMappedName("%s%d" % (kind, i + 1))
            )
    return {sig: name for sig, name in result.items() if name}


class TestNameStability(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestNameStability")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def feature(self, kind, name, **props):
        obj = self.Body.newObject("PartDesign::" + kind, name)
        for prop, value in props.items():
            setattr(obj, prop, value)
        self.Doc.recompute()
        self.assertTrue(obj.isValid(), name)
        return obj

    def makeDraftedBlock(self):
        """A block 50 x 30 x 20, its two long sides drafted, the top rim rounded"""
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        sketch.AttachmentSupport = (self.Body.Origin.OriginFeatures[3], [""])
        sketch.MapMode = "FlatFace"
        corners = [V(0, 0, 0), V(50, 0, 0), V(50, 30, 0), V(0, 30, 0)]
        for i in range(4):
            sketch.addGeometry(Part.LineSegment(corners[i], corners[(i + 1) % 4]))
        pad = self.feature("Pad", "Pad", Profile=sketch, Length=20)
        faces = pad.Shape.Faces
        sides = [
            "Face%d" % (i + 1)
            for i, f in enumerate(faces)
            if abs(f.BoundBox.YMax - f.BoundBox.YMin) < 1e-6
        ]
        bottom = ["Face%d" % (i + 1) for i, f in enumerate(faces) if abs(f.BoundBox.ZMax) < 1e-6]
        draft = self.feature(
            "Draft", "Draft", Base=(pad, sides), NeutralPlane=(pad, bottom), Angle=5
        )
        rim = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(draft.Shape.Edges)
            if abs(e.BoundBox.ZMin - 20) < 1e-6 and len(e.Vertexes) == 2
        ]
        fillet = self.feature("Fillet", "Fillet", Base=(draft, rim), Radius=2)
        return sketch, draft, fillet

    def testNamesStayWhenAHoleIsAdded(self):
        # Arrange
        sketch, draft, fillet = self.makeDraftedBlock()
        before = {obj.Name: names(obj) for obj in (draft, fillet)}

        # Act: a hole through the block, far from every edge there was
        sketch.addGeometry(Part.Circle(V(20, 15, 0), V(0, 0, 1), 4))
        self.Doc.recompute()

        # Assert: every edge and vertex that is where it was has the name it had
        for obj in (draft, fillet):
            self.assertTrue(obj.isValid(), obj.Name)
            after = names(obj)
            kept = [sig for sig in before[obj.Name] if sig in after]
            # the block's own twelve edges and eight vertices at the least
            self.assertGreaterEqual(len(kept), 20, obj.Name)
            for sig in kept:
                self.assertEqual(after[sig], before[obj.Name][sig], "%s %s" % (obj.Name, sig))

    def testAnEdgeIsNamedFromBothItsFaces(self):
        """No name of the model holds the older step, one face and a number"""
        _sketch, draft, fillet = self.makeDraftedBlock()
        for obj in (draft, fillet):
            shape = obj.Shape
            for i in range(len(shape.Edges)):
                name = shape.getElementMappedName("Edge%d" % (i + 1))
                self.assertTrue(name, "%s Edge%d has no name" % (obj.Name, i + 1))
                self.assertNotIn(";:U", name)
