#***************************************************************************
#*   Copyright (c) 2011 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
#*                                                                         *
#*   This program is free software; you can redistribute it and/or modify  *
#*   it under the terms of the GNU Lesser General Public License (LGPL)    *
#*   as published by the Free Software Foundation; either version 2 of     *
#*   the License, or (at your option) any later version.                   *
#*   for detail see the LICENCE text file.                                 *
#*                                                                         *
#*   This program is distributed in the hope that it will be useful,       *
#*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
#*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
#*   GNU Library General Public License for more details.                  *
#*                                                                         *
#*   You should have received a copy of the GNU Library General Public     *
#*   License along with this program; if not, write to the Free Software   *
#*   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
#*   USA                                                                   *
#*                                                                         *
#***************************************************************************

import unittest

import FreeCAD
import TestSketcherApp

class TestMirrored(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestMirrored")

    def testMirroredSketchCase(self):
        """
        Creates a unit cube cornered at the origin and mirrors it about the Y axis.
        This operation should create a rectangular prism with volume 2.0.
        """
        self.Body = self.Doc.addObject('PartDesign::Body','Body')
        self.Rect = self.Doc.addObject('Sketcher::SketchObject','Rect')
        self.Body.addObject(self.Rect)
        TestSketcherApp.CreateRectangleSketch(self.Rect, (0, 0), (1, 1))
        self.Doc.recompute()
        self.Pad = self.Doc.addObject("PartDesign::Pad","Pad")
        self.Pad.Profile = self.Rect
        self.Pad.Length = 1
        self.Body.addObject(self.Pad)
        self.Doc.recompute()
        self.Mirrored = self.Doc.addObject("PartDesign::Mirrored","Mirrored")
        self.Mirrored.Originals = [self.Pad]
        self.Mirrored.MirrorPlane = (self.Rect, ["V_Axis"])
        self.Body.addObject(self.Mirrored)
        self.Doc.recompute()
        self.assertAlmostEqual(self.Mirrored.Shape.Volume, 2.0)

    def testMirroredPrimitiveCase(self):
        """
        Tests the same mirroring scenario as in the sketch case,
        but is designed to ensure the same end result occurs with
        a different base object.
        """
        self.Body = self.Doc.addObject('PartDesign::Body','Body')
        self.Box = self.Doc.addObject('PartDesign::AdditiveBox','Box')
        self.Box.Length=1
        self.Box.Width=1
        self.Box.Height=1
        self.Body.addObject(self.Box)
        self.Doc.recompute()
        self.Mirrored = self.Doc.addObject("PartDesign::Mirrored", "Mirrored")
        self.Mirrored.Originals = [self.Box]
        self.Mirrored.MirrorPlane = (self.Doc.XY_Plane, [""])
        self.Body.addObject(self.Mirrored)
        self.Doc.recompute()
        self.assertAlmostEqual(self.Mirrored.Shape.Volume, 2.0)

    def testMirroredOffsetCase(self):
        self.Body = self.Doc.addObject('PartDesign::Body','Body')
        self.Body.SingleSolid = True
        self.Rect = self.Doc.addObject('Sketcher::SketchObject','Rect')
        self.Body.addObject(self.Rect)
        TestSketcherApp.CreateRectangleSketch(self.Rect, (0, 1), (1, 1))
        self.Doc.recompute()
        self.Pad = self.Doc.addObject("PartDesign::Pad","Pad")
        self.Pad.Profile = self.Rect
        self.Pad.Length = 1
        self.Body.addObject(self.Pad)
        self.Doc.recompute()
        self.Mirrored = self.Doc.addObject("PartDesign::Mirrored","Mirrored")
        self.Mirrored.Originals = [self.Pad]
        self.Mirrored.MirrorPlane = (self.Rect, ["H_Axis"])
        self.Body.addObject(self.Mirrored)
        self.Doc.recompute()
        self.assertIn("Up-to-date", self.Mirrored.State)

    def testPlaneWithoutSubname(self):
        """A plane set in the property editor has no subname at all
        (upstream 7d10f5ed73): it was refused as no plane."""
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.Box = self.Body.newObject("PartDesign::AdditiveBox", "Box")
        self.Box.Length = self.Box.Width = self.Box.Height = 10
        self.Box.Placement.Base = FreeCAD.Vector(0, 5, 0)
        xz = [f for f in self.Body.Origin.OriginFeatures if f.Role == "XZ_Plane"][0]
        self.Doc.recompute()
        mirrored = self.Body.newObject("PartDesign::Mirrored", "Mirrored")
        mirrored.Originals = [self.Box]
        mirrored.MirrorPlane = (xz, [])
        self.Doc.recompute()
        self.assertIn("Up-to-date", mirrored.State)
        self.assertAlmostEqual(mirrored.Shape.BoundBox.YMin, -15)

    def testMovedLCSPlane(self):
        """A plane of a moved or turned coordinate system mirrors where the
        plane is, not across its global counterpart."""
        for placement, check in (
                (FreeCAD.Placement(FreeCAD.Vector(0, 0, 20), FreeCAD.Rotation()),
                 lambda box: self.assertAlmostEqual(box.ZMax, 40)),
                (FreeCAD.Placement(FreeCAD.Vector(0, 0, 20),
                                   FreeCAD.Rotation(FreeCAD.Vector(1, 0, 0), 90)),
                 lambda box: self.assertAlmostEqual(box.YMin, -10))):
            doc = FreeCAD.newDocument("PartDesignTestMirroredLCS")
            try:
                body = doc.addObject("PartDesign::Body", "Body")
                box = body.newObject("PartDesign::AdditiveBox", "Box")
                box.Length = box.Width = box.Height = 10
                lcs = doc.addObject("App::LocalCoordinateSystem", "LCS")
                lcs.Placement = placement
                doc.recompute()
                xy = [f for f in lcs.OriginFeatures if f.Role == "XY_Plane"][0]
                mirrored = body.newObject("PartDesign::Mirrored", "Mirrored")
                mirrored.Originals = [box]
                mirrored.MirrorPlane = (xy, [""])
                doc.recompute()
                self.assertIn("Up-to-date", mirrored.State)
                check(mirrored.Shape.BoundBox)
            finally:
                FreeCAD.closeDocument(doc.Name)

    def _filletedPadMirrored(self, version=None, offset=None):
        """A pad, a fillet round its top, and the pad mirrored, in a
        document of its own: the volume on the original's side of the
        plane, on the copy's, and the fillet's."""
        doc = FreeCAD.newDocument("PartDesignTestMirroredFillet")
        try:
            return self._filletedPadMirroredIn(doc, version, offset)
        finally:
            FreeCAD.closeDocument(doc.Name)

    def _filletedPadMirroredIn(self, doc, version, offset):
        import Part
        V = FreeCAD.Vector
        body = doc.addObject("PartDesign::Body", "FilletedBody")
        sketch = body.newObject("Sketcher::SketchObject", "FilletedSketch")
        sketch.AttachmentSupport = (doc.getObject("XY_Plane"), [""])
        sketch.MapMode = "FlatFace"
        corners = [(5, 5), (25, 5), (25, 15), (5, 15)]
        for (x0, y0), (x1, y1) in zip(corners, corners[1:] + corners[:1]):
            sketch.addGeometry(Part.LineSegment(V(x0, y0, 0), V(x1, y1, 0)))
        pad = body.newObject("PartDesign::Pad", "FilletedPad")
        pad.Profile = sketch
        pad.Length = 10
        doc.recompute()
        top = [
            "Edge%d" % (i + 1)
            for i, edge in enumerate(pad.Shape.Edges)
            if all(abs(vertex.Point.z - 10) < 1e-9 for vertex in edge.Vertexes)
        ]
        fillet = body.newObject("PartDesign::Fillet", "PadFillet")
        fillet.Base = (pad, top)
        fillet.Radius = 2
        doc.recompute()
        mirrored = body.newObject("PartDesign::Mirrored", "PadMirrored")
        mirrored.Originals = [pad]
        mirrored.MirrorPlane = (doc.getObject("YZ_Plane"), [""])
        if version is not None:
            mirrored._Version = version
        if offset is not None:
            mirrored.TransformOffset = FreeCAD.Placement(offset, FreeCAD.Rotation())
        doc.recompute()
        self.assertIn("Up-to-date", mirrored.State)
        shape = mirrored.Shape
        originalSide = shape.common(Part.makeBox(100, 200, 100, V(0, -100, -50))).Volume
        return originalSide, shape.Volume - originalSide, fillet.Shape.Volume

    def testOriginalKeepsWhatCameAfterIt(self):
        """The first instance of a pattern is the original, which the
        support already holds as the features after it left it: made over
        again, a pad mirrored after it was filleted lost its fillet."""
        original, copy, filleted = self._filletedPadMirrored()
        self.assertAlmostEqual(filleted, 1951.5634, 3)
        self.assertAlmostEqual(original, filleted, 6)
        self.assertAlmostEqual(copy, 2000, 6)

    def testOffsetThatCannotMoveTheOriginal(self):
        """An offset moves the copies. The original moves with them where
        the history can be written again without it, which a fillet after
        it rules out: it then stays, and is not added a second time 3 mm
        further on."""
        original, copy, filleted = self._filletedPadMirrored(offset=FreeCAD.Vector(0, 3, 0))
        self.assertAlmostEqual(original, filleted, 6)
        self.assertAlmostEqual(copy, 2000, 6)

    def testEarlierPatternsStayAsMade(self):
        """_Version says how a pattern was made. 3 and before left the
        first instance out, as 5 does; 4 made it, and a file of those two
        years keeps what it has."""
        self.assertEqual(self.Doc.addObject("PartDesign::Mirrored", "Fresh")._Version, 5)
        for version in (0, 2, 3):
            original, copy, filleted = self._filletedPadMirrored(version)
            self.assertAlmostEqual(original, filleted, 6, "version %d" % version)
            self.assertAlmostEqual(copy, 2000, 6, "version %d" % version)
        original, copy, filleted = self._filletedPadMirrored(4)
        self.assertAlmostEqual(original, 2000, 6)
        self.assertAlmostEqual(copy, 2000, 6)

    def tearDown(self):
        #closing doc
        FreeCAD.closeDocument("PartDesignTestMirrored")
        #print ("omit closing document for debugging")
