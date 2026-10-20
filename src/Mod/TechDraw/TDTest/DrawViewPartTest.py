#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# basic test script for TechDraw module
# creates a page and 1 view


import FreeCAD
import unittest
from .TechDrawTestUtilities import createPageWithSVGTemplate
from PySide import QtCore

class DrawViewPartTest(unittest.TestCase):
    def setUp(self):
        """Creates a page"""
        FreeCAD.newDocument("TDPart")
        FreeCAD.setActiveDocument("TDPart")
        FreeCAD.ActiveDocument = FreeCAD.getDocument("TDPart")

        FreeCAD.ActiveDocument.addObject("Part::Box", "Box")

        self.page = createPageWithSVGTemplate()
        self.page.Scale = 5.0
        # page.ViewObject.show()    # unit tests run in console mode
        print("DrawViewPart test: page created")

    def tearDown(self):
        print("DrawViewPart test finished")
        FreeCAD.closeDocument("TDPart")

    def testMakeDrawViewPart(self):
        """Tests if a view can be added to page"""
        print("testing DrawViewPart")
        view = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(view)
        FreeCAD.ActiveDocument.View.Source = [FreeCAD.ActiveDocument.Box]
        FreeCAD.ActiveDocument.recompute()

        #wait for threads to complete before checking result
        loop = QtCore.QEventLoop()

        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)

        timer.start(2000)   #2 second delay
        loop.exec_()

        edges = view.getVisibleEdges()
        self.assertEqual(len(edges), 4, "DrawViewPart has wrong number of edges")
        self.assertTrue("Up-to-date" in view.State, "DrawViewPart is not Up-to-date")

    @staticmethod
    def _waitForThreads(ms=2000):
        loop = QtCore.QEventLoop()
        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)
        timer.start(ms)
        loop.exec_()

    def testAViewWithNoSourceSaysWhy(self):
        """A view with nothing to draw is in error, and says why: it used to
        log a console line and stay valid and empty (docs/TechDrawErrors.md
        3.2)"""
        view = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(view)
        FreeCAD.ActiveDocument.recompute()
        self.assertTrue("Invalid" in view.State, view.State)
        self.assertIn("no source objects", view.getStatusString())

    def testAHatchOfAFaceNotInTheViewSaysWhy(self):
        """A hatch of a face the view does not have is in error, and says
        which: its faces are checked once the view has found its own, after
        the hatch's recompute (docs/TechDrawErrors.md 3.2)"""
        view = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(view)
        view.Source = [FreeCAD.ActiveDocument.Box]
        FreeCAD.ActiveDocument.recompute()
        self._waitForThreads()
        hatch = FreeCAD.ActiveDocument.addObject("TechDraw::DrawHatch", "Hatch")
        hatch.Source = (view, ["Face99"])
        view.touch()
        FreeCAD.ActiveDocument.recompute()
        self._waitForThreads()
        self.assertTrue("Invalid" in hatch.State, hatch.State)
        self.assertIn("Face99", hatch.getStatusString())

if __name__ == "__main__":
    unittest.main()
