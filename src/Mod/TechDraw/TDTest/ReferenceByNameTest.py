import unittest

import FreeCAD
import Part
from PySide import QtCore

from .TechDrawTestUtilities import createPageWithSVGTemplate


class ReferenceByNameTest(unittest.TestCase):
    """A hatch's faces and a centre line's elements are found again by name
    only where the element carrying the name is also what was saved of it --
    its area and centre, its ends -- or by that geometry alone. A name match
    whose geometry is other is not taken in a recompute: it is left, said,
    and the user's edit takes it (repointByName()). The reporter, 2026-11-26:
    "the auto reference fix should only be done if geometry search found the
    match instead of pure naming match ... cover hatches and centre lines
    too".

    Two boxes seen from above: two faces, of 100 and of 200."""

    NOWHERE = "F 1 0 0 999"

    def setUp(self):
        self.doc = FreeCAD.newDocument("TDRefByName")
        FreeCAD.setActiveDocument(self.doc.Name)
        self.page = createPageWithSVGTemplate(self.doc)
        a = Part.makeBox(10, 10, 10)
        b = Part.makeBox(20, 10, 10, FreeCAD.Vector(30, 0, 0))
        src = self.doc.addObject("Part::Feature", "Shape")
        src.Shape = Part.Compound([a, b])
        self.view = self.doc.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(self.view)
        self.view.Source = [src]
        self.view.Direction = (0.0, 0.0, 1.0)
        self.doc.recompute()
        self.wait()

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def wait(self, ms=2000):
        """the projection and its faces are made in worker threads"""
        loop = QtCore.QEventLoop()
        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)
        timer.start(ms)
        loop.exec_()

    def again(self):
        self.view.touch()
        self.doc.recompute()
        self.wait()

    def hatch(self, kind="TechDraw::DrawHatch"):
        hatch = self.doc.addObject(kind, "Hatch")
        hatch.Source = (self.view, ["Face0"])
        self.doc.recompute()
        self.wait()
        self.assertEqual(len(self.view.getFaceNames()), 2)
        names, geometry = list(hatch.SavedNames), list(hatch.SavedGeometry)
        self.assertTrue(names[0], names)
        self.assertTrue(geometry[0].startswith("F "), geometry)
        return hatch, names, geometry

    def misplace(self, hatch, names, geometry):
        """on the other face, what was saved of it set after, as a file brings it"""
        hatch.Source = (self.view, ["Face1"])
        hatch.SavedNames = names
        hatch.SavedGeometry = geometry
        self.again()

    def testAHatchFaceWithItsGeometryIsTakenByName(self):
        hatch, names, geometry = self.hatch()
        self.misplace(hatch, names, geometry)
        self.assertEqual(list(hatch.Source[1]), ["Face0"])
        self.assertNotIn("Invalid", hatch.State)

    def testAHatchFaceIsFoundByItsGeometryAlone(self):
        hatch, names, geometry = self.hatch()
        self.misplace(hatch, [""], geometry)
        self.assertEqual(list(hatch.Source[1]), ["Face0"])

    def testAHatchFaceByNameAloneIsLeftAndSaid(self):
        hatch, names, geometry = self.hatch()
        self.misplace(hatch, names, [self.NOWHERE])
        self.assertEqual(list(hatch.Source[1]), ["Face1"])
        self.assertIn("Invalid", hatch.State)
        self.assertEqual(hatch.findByName(), [(0, "Face1", "Face0", False)])
        # what was saved of it kept for the edit, not recorded over
        self.assertEqual(list(hatch.SavedNames), names)
        self.assertEqual(list(hatch.SavedGeometry), [self.NOWHERE])
        # the edit's
        self.assertEqual(hatch.repointByName(), [(0, "Face1", "Face0", False)])
        self.assertEqual(list(hatch.Source[1]), ["Face0"])
        self.assertEqual(hatch.findByName(), [])

    def testAGeometricHatchFaceByNameAloneIsLeft(self):
        hatch, names, geometry = self.hatch("TechDraw::DrawGeomHatch")
        self.misplace(hatch, names, [self.NOWHERE])
        self.assertEqual(list(hatch.Source[1]), ["Face1"])
        self.assertEqual(hatch.findByName(), [(0, "Face1", "Face0", False)])
        self.assertEqual(hatch.repointByName(), [(0, "Face1", "Face0", False)])
        self.assertEqual(list(hatch.Source[1]), ["Face0"])

    def centreLine(self):
        tag = self.view.makeCenterLine(["Face0"], 0)
        self.again()
        cl = self.view.getCenterLine(tag)
        names, geometry = list(cl.FaceNames), list(cl.FaceGeometry)
        self.assertTrue(names[0], names)
        self.assertTrue(geometry[0].startswith("F "), geometry)
        return tag, names, geometry

    def testACentreLineFaceWithItsGeometryIsTakenByName(self):
        tag, names, geometry = self.centreLine()
        cl = self.view.getCenterLine(tag)
        cl.Faces = ["Face1"]
        cl.FaceGeometry = geometry
        self.again()
        cl = self.view.getCenterLine(tag)
        self.assertEqual(list(cl.Faces), ["Face0"])

    def testACentreLineFaceByNameAloneIsLeft(self):
        tag, names, geometry = self.centreLine()
        cl = self.view.getCenterLine(tag)
        cl.Faces = ["Face1"]
        cl.FaceGeometry = [self.NOWHERE]
        self.again()
        cl = self.view.getCenterLine(tag)
        self.assertEqual(list(cl.Faces), ["Face1"])
        self.assertEqual(list(cl.FaceNames), names)
        self.assertEqual(cl.findByName(self.view), [(0, "Face1", "Face0", False)])
        self.assertEqual(cl.repointByName(self.view), [(0, "Face1", "Face0", False)])
        self.assertEqual(list(cl.Faces), ["Face0"])
        self.assertEqual(cl.findByName(self.view), [])

    def testAFaceSignatureDoesNotFollowTheViewScale(self):
        """what is saved of a face is in the view's unscaled frame: scaling
        the view moves nothing by name"""
        hatch, names, geometry = self.hatch()
        self.view.Scale = 2.0
        self.again()
        self.assertEqual(list(hatch.Source[1]), ["Face0"])
        self.assertEqual(list(hatch.SavedGeometry)[0].split()[0], "F")
        self.assertEqual(hatch.findByName(), [])


if __name__ == "__main__":
    unittest.main()
