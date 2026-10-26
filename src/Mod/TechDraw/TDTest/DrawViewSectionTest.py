import FreeCAD
import unittest
from .TechDrawTestUtilities import createPageWithSVGTemplate
from PySide import QtCore

class DrawViewSectionTest(unittest.TestCase):
    def setUp(self):
        """Creates a page and a view"""
        FreeCAD.newDocument("TDSection")
        FreeCAD.setActiveDocument("TDSection")
        FreeCAD.ActiveDocument = FreeCAD.getDocument("TDSection")

        self.box = FreeCAD.ActiveDocument.addObject("Part::Box", "Box")

        self.page = createPageWithSVGTemplate()
        self.page.Scale = 5.0
        # page.ViewObject.show()    # unit tests run in console mode
        print("DrawViewSection test: page created")

        self.view = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(self.view)
        self.view.Source = [self.box]
        self.view.Direction = (0.0, 0.0, 1.0)
        self.view.Rotation = 0.0
        self.view.X = 30.0
        self.view.Y = 150.0
        FreeCAD.ActiveDocument.recompute()

        #wait for threads to complete before checking result
        loop = QtCore.QEventLoop()

        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)

        timer.start(2000)   #2 second delay
        loop.exec_()

        print("DrawViewSection test: view created")

    def tearDown(self):
        print("DrawViewSection test: finished")
        FreeCAD.closeDocument("TDSection")

    def testMakeDrawViewSection(self):
        """Tests if a DrawViewSection can be added to page"""
        section = FreeCAD.ActiveDocument.addObject(
            "TechDraw::DrawViewSection", "Section"
        )
        self.page.addView(section)
        section.Source = [self.box]
        section.BaseView = self.view
        section.Direction = (0.0, 1.0, 0.0)
        section.SectionNormal = (0.0, 1.0, 0.0)
        section.SectionOrigin = (5.0, 5.0, 5.0)
        print("DrawViewSection test: section created")
        FreeCAD.ActiveDocument.recompute()

        #wait for threads to complete before checking result
        loop = QtCore.QEventLoop()

        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)

        timer.start(2000)   #2 second delay
        loop.exec_()

        edges = section.getVisibleEdges()
        self.assertEqual(len(edges), 4, "DrawViewSection has wrong number of edges")
        self.assertTrue("Up-to-date" in section.State)

    def testASectionWithNoBaseViewSaysWhy(self):
        """A section with no base view says so: it said "BaseView object not
        found" (docs/TechDrawErrors.md 3.2)"""
        section = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewSection", "Section")
        self.page.addView(section)
        section.Source = [self.box]
        FreeCAD.ActiveDocument.recompute()
        self.assertTrue("Invalid" in section.State, section.State)
        self.assertIn("no base view", section.getStatusString())

    def testASectionThatCutsNothingSaysWhy(self):
        """A section plane just before the part: the cutting tool, a prism
        along the normal as deep as the shape is big, takes all of it and
        leaves nothing to draw. In error, and says why; it used to be a
        console line from the cut's worker and an empty view
        (docs/TechDrawErrors.md 3.2)"""
        section = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewSection", "Section")
        self.page.addView(section)
        section.Source = [self.box]
        section.BaseView = self.view
        section.Direction = (0.0, 1.0, 0.0)
        section.SectionNormal = (0.0, 1.0, 0.0)
        section.SectionOrigin = (5.0, -2.0, 5.0)
        FreeCAD.ActiveDocument.recompute()

        loop = QtCore.QEventLoop()
        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)
        timer.start(2000)
        loop.exec_()

        self.assertTrue("Invalid" in section.State, section.State)
        self.assertIn("does not meet the shape", section.getStatusString())


if __name__ == "__main__":
    unittest.main()
