import FreeCAD
import unittest
from .TechDrawTestUtilities import createPageWithSVGTemplate
from PySide import QtCore

class DrawViewDimensionTest(unittest.TestCase):
    def setUp(self):
        """Creates a page and 2 views"""
        print("DVDTest.setUp()")
        FreeCAD.newDocument("TDDimTest")
        FreeCAD.setActiveDocument("TDDimTest")
        FreeCAD.ActiveDocument = FreeCAD.getDocument("TDDimTest")
        self.document = FreeCAD.ActiveDocument

        # make source feature
        self.document.addObject("Part::Box", "Box")
        self.document.addObject("Part::Sphere", "Sphere")

        # make a page
        self.page = createPageWithSVGTemplate(self.document)
        self.page.Scale = 5.0

        # make Views
        self.view = self.document.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(self.view)
        self.view.Source = [self.document.Box]
        self.view.X = 30
        self.view.Y = 150

        self.view1 = self.document.addObject("TechDraw::DrawViewPart", "View001")
        self.page.addView(self.view1)
        self.view1.Source = [self.document.Sphere]
        self.view1.X = 220
        self.view1.Y = 150

        self.document.recompute()

        #wait for threads to complete before checking result
        loop = QtCore.QEventLoop()

        timer = QtCore.QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)

        timer.start(5000)   #5 second delay
        loop.exec_()

    def tearDown(self):
        print("DVDTest.tearDown()")
        FreeCAD.closeDocument("TDDimTest")

    def testLengthDimension(self):
        """Tests if a length dimension can be added to view"""
        # make length dimension
        print("making length dimension")

        dimension = self.document.addObject("TechDraw::DrawViewDimension", "Dimension")
        self.page.addView(dimension)
        dimension.Type = "Distance"
        dimension.References2D = [(self.view, "Edge1")]
        print("finished length dimension")
        self.document.recompute()
        self.assertTrue("Up-to-date" in dimension.State)

    def testRadiusDimension(self):
        """Tests if a radius dimension can be added to view1"""
        print("making radius dimension")
        dimension = self.document.addObject("TechDraw::DrawViewDimension", "Dimension001")
        self.page.addView(dimension)
        dimension.Type = "Radius"
        dimension.MeasureType = "Projected"
        dimension.References2D = [(self.view1, "Edge0")]
        self.document.recompute()
        self.assertTrue("Up-to-date" in dimension.State)

    def testAngleOnParallelEdgesSaysWhy(self):
        """An angle dimension on two parallel edges fails, and its recompute
        error says so: the kind, the view, the edges and that they are
        parallel (docs/HandsOnLog.md entry 178: it said only "Invalid
        selection - Det = 0")"""
        edges = []
        for i in range(12):
            try:
                edge = self.view.getEdgeByIndex(i)
            except Exception:
                break
            if edge is None or len(edge.Vertexes) < 2:
                continue
            edges.append((i, edge.Vertexes[-1].Point - edge.Vertexes[0].Point))
        pair = None
        for a in range(len(edges)):
            for b in range(a + 1, len(edges)):
                d0, d1 = edges[a][1], edges[b][1]
                if d0.Length > 1e-6 and d1.Length > 1e-6 and \
                        abs(d0.x * d1.y - d0.y * d1.x) < 1e-6 * d0.Length * d1.Length:
                    pair = (edges[a][0], edges[b][0])
                    break
            if pair:
                break
        self.assertIsNotNone(pair, "the box's view has two parallel edges")
        dimension = self.document.addObject("TechDraw::DrawViewDimension", "Dimension002")
        self.page.addView(dimension)
        dimension.Type = "Angle"
        dimension.MeasureType = "Projected"
        dimension.References2D = [(self.view, ("Edge%d" % pair[0], "Edge%d" % pair[1]))]
        self.document.recompute()
        self.assertTrue("Invalid" in dimension.State, dimension.State)
        why = dimension.getStatusString()
        self.assertIn("parallel", why)
        self.assertIn("Angle dimension", why)
        self.assertIn("Edge%d" % pair[0], why)
        self.assertNotIn("Det = 0", why)


if __name__ == "__main__":
    unittest.main()
