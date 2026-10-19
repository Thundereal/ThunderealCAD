#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# test script for DrawViewDetail
# creates a page, a view and a detail view


import FreeCAD
import unittest
from .TechDrawTestUtilities import createPageWithSVGTemplate
from PySide import QtCore

class DrawViewDetailTest(unittest.TestCase):
    def setUp(self):
        """Creates a page"""
        FreeCAD.newDocument("TDPart")
        FreeCAD.setActiveDocument("TDPart")
        FreeCAD.ActiveDocument = FreeCAD.getDocument("TDPart")

        FreeCAD.ActiveDocument.addObject("Part::Box", "Box")

        self.page = createPageWithSVGTemplate()
        self.page.Scale = 5.0
        # page.ViewObject.show()    # unit tests run in console mode
        print("DrawViewDetail test: page created")

        self.view = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(self.view)
        FreeCAD.ActiveDocument.View.Source = [FreeCAD.ActiveDocument.Box]
        FreeCAD.ActiveDocument.recompute()

        #wait for threads to complete before checking result
        loop = QtCore.QEventLoop()

        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)

        timer.start(2000)   #2 second delay
        loop.exec_()
        print("DrawViewDetail test: view created")

    def tearDown(self):
        print("DrawViewDetail test finished")
        FreeCAD.closeDocument("TDPart")

    def testMakeDrawViewPart(self):
        """Tests if a view can be added to page"""
        print("testing DrawViewDetail")

        detail = FreeCAD.ActiveDocument.addObject(
            "TechDraw::DrawViewDetail", "Detail"
        )
        detail.BaseView = self.view
        detail.Direction = self.view.Direction
        detail.XDirection = self.view.XDirection
        self.page.addView(detail)
        FreeCAD.ActiveDocument.recompute()
        print("DrawViewDetail test: Detail created")

        #wait for threads to complete before checking result
        loop = QtCore.QEventLoop()

        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)

        timer.start(2000)   #2 second delay
        loop.exec_()

        edges = detail.getVisibleEdges()

        self.assertEqual(len(edges), 4, "DrawViewDetail has wrong number of edges")
        self.assertTrue("Up-to-date" in detail.State, "DrawViewDetail is not Up-to-date")

    def testFailedDetailSaysWhy(self):
        """A detail whose cut fails in its worker thread is in error, and says
        why: the cut runs after execute() has returned, so the failure used to
        be a console line and an empty view (docs/TechDrawErrors.md 3.1).
        A radius of 0 makes the cutting tool fail."""
        detail = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewDetail", "Detail001")
        detail.BaseView = self.view
        detail.Direction = self.view.Direction
        detail.XDirection = self.view.XDirection
        detail.Radius = 0
        self.page.addView(detail)
        FreeCAD.ActiveDocument.recompute()

        loop = QtCore.QEventLoop()
        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)
        timer.start(3000)
        loop.exec_()

        self.assertTrue("Invalid" in detail.State, detail.State)
        why = detail.getStatusString()
        self.assertIn("detail could not be made", why)

if __name__ == "__main__":
    unittest.main()
