import math
import unittest

import FreeCAD
import Part

from .TechDrawTestUtilities import createPageWithSVGTemplate


class DimensionReferenceTest(unittest.TestCase):
    """A dimension's references are found again when its view is made anew:
    by the name each element carries, then by the geometry saved with the
    dimension. A file written with OCCT 7.7 and made again with 8 numbers the
    projection otherwise, runs some edges the other way and writes some
    lines as B-splines (docs/WorkerProcess.md sec 46). Each case here puts a
    dimension in that state by hand: references on the wrong elements, the
    saved geometry what they meant."""

    def setUp(self):
        self.doc = FreeCAD.newDocument("TDDimRefs")
        FreeCAD.setActiveDocument(self.doc.Name)
        self.page = createPageWithSVGTemplate(self.doc)

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def view(self, shape=None):
        if shape is None:
            source = self.doc.addObject("Part::Box", "Box")
        else:
            source = self.doc.addObject("Part::Feature", "Shape")
            source.Shape = shape
        view = self.doc.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(view)
        view.Source = [source]
        self.doc.recompute()
        return view

    def dimension(self, view, kind, subs):
        dim = self.doc.addObject("TechDraw::DrawViewDimension", "Dim")
        dim.Type = kind
        dim.References2D = [(view, subs)]
        self.page.addView(dim)
        self.doc.recompute()
        return dim

    def misplace(self, dim, view, subs, geometry, names):
        """References moved to `subs`, the saved geometry and names set after,
        as a file brings them: a change of the references saves its own"""
        dim.References2D = [(view, subs)]
        dim.SavedGeometry = geometry
        dim.SavedNames = names
        dim.touch()
        self.doc.recompute()

    def testAReversedEdgeIsFoundAgain(self):
        """The edge a dimension measures, run the other way in the new
        projection, is the same edge"""
        view = self.view()
        dim = self.dimension(view, "DistanceX", ("Vertex0", "Edge1"))
        self.assertAlmostEqual(dim.getRawValue(), 10.0)
        vertex, edge = dim.SavedGeometry
        a, b = edge.Vertexes[0].Point, edge.Vertexes[-1].Point
        reversed_ = Part.Edge(Part.LineSegment(b, a))
        self.misplace(dim, view, ("Vertex0", "Edge0"), [vertex, reversed_], ["", ""])
        self.assertEqual(dim.References2D[0][1], ("Vertex0", "Edge1"))
        self.assertAlmostEqual(dim.getRawValue(), 10.0)

    def testALineSavedAsABSplineIsFoundAgain(self):
        """A line saved as a trimmed piece of a longer B-spline is the line:
        the piece's ends and its points say so, its poles do not"""
        view = self.view()
        dim = self.dimension(view, "DistanceX", ("Vertex0", "Edge1"))
        vertex, edge = dim.SavedGeometry
        a, b = edge.Vertexes[0].Point, edge.Vertexes[-1].Point
        longer = Part.LineSegment(a + (a - b) * 0.5, b + (b - a) * 0.5).toBSpline()
        piece = Part.Edge(longer, longer.parameter(a), longer.parameter(b))
        self.assertEqual(piece.Curve.__class__.__name__, "BSplineCurve")
        self.misplace(dim, view, ("Vertex0", "Edge0"), [vertex, piece], ["", ""])
        self.assertEqual(dim.References2D[0][1], ("Vertex0", "Edge1"))
        self.assertAlmostEqual(dim.getRawValue(), 10.0)

    def testAReferenceFoundByNameKeepsTheOthersSaved(self):
        """The edge found again by its name, the vertex not: the vertex's
        saved geometry is kept as it was, and finds it. It was saved again
        from the element its stale number names, and then passed"""
        view = self.view()
        dim = self.dimension(view, "DistanceX", ("Vertex0", "Edge1"))
        geometry = list(dim.SavedGeometry)
        names = list(dim.SavedNames)
        self.assertTrue(names[1], names)
        self.misplace(dim, view, ("Vertex2", "Edge0"), geometry, ["", names[1]])
        self.assertEqual(dim.References2D[0][1], ("Vertex0", "Edge1"))
        self.assertAlmostEqual(dim.getRawValue(), 10.0)

    def lineAndSpline(self):
        """A view of a line and a B-spline beside it, facing the view: the
        line's edge and the spline's, by index"""
        a, b, c = FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(10, 0, 0), FreeCAD.Vector(20, 0, 0)
        spline = Part.BSplineCurve()
        spline.interpolate([b, FreeCAD.Vector(15, 0, 5), c])
        view = self.view(Part.Compound([Part.Edge(Part.LineSegment(a, b)), spline.toShape()]))
        line = curve = None
        n = 0
        while True:
            try:
                e = view.getEdgeByIndex(n)
            except Exception:
                break
            kind = e.Curve.__class__.__name__
            if kind == "Line" and line is None:
                line = "Edge%d" % n
            elif kind == "BSplineCurve" and curve is None:
                curve = "Edge%d" % n
            n += 1
        self.assertTrue(line and curve, (line, curve))
        return view, line, curve

    def testANameOnAnotherKindOfEdgeIsNotTakenInARecompute(self):
        """The reporter's (2026-11-26, scanner.FCStd): a distance on a line
        whose saved name a B-spline carried after the view was made again
        was repointed onto it by the name alone, and failed: "Edge21 is a
        BSpline, not a straight line". In a recompute a name is taken only
        where the geometry agrees; otherwise the dimension is left as it was,
        in error, saying so, and the name's element is the user's to take on
        edit (repointByName())"""
        view, line, curve = self.lineAndSpline()
        dim = self.dimension(view, "Distance", (line,))
        self.assertAlmostEqual(dim.getRawValue(), 10.0)
        splineName = view.getGeometryName(curve)
        self.assertTrue(splineName)
        # the line it was made on is elsewhere now: its saved geometry is
        # nothing in the view, and its name is the spline's
        moved = Part.Edge(Part.LineSegment(FreeCAD.Vector(0, 0, 40), FreeCAD.Vector(10, 0, 40)))
        self.misplace(dim, view, (line,), [moved], [splineName])
        self.assertEqual(dim.References2D[0][1], (line,))
        self.assertIn("Invalid", dim.State)
        self.assertEqual(dim.findByName(), [(0, line, curve, False)])
        # the name kept for the user's edit, not recorded over
        self.assertEqual(list(dim.SavedNames), [splineName])
        # the edit's: taken, and said what it moved
        self.doc.UndoMode = 1
        self.doc.openTransaction("take by name")
        self.assertEqual(dim.repointByName(), [(0, line, curve, False)])
        self.doc.commitTransaction()
        self.assertEqual(dim.References2D[0][1], (curve,))
        # taken back, as the edit's Cancel does: what was saved comes back
        # with the reference, not saved again from the element it is on
        self.doc.undo()
        self.assertEqual(dim.References2D[0][1], (line,))
        self.assertEqual(list(dim.SavedNames), [splineName])
        self.assertEqual(dim.findByName(), [(0, line, curve, False)])

    def testANameWithItsGeometryIsTakenInARecompute(self):
        """The element that carries the saved name and has the saved
        geometry is the reference's, taken in a recompute"""
        view, line, curve = self.lineAndSpline()
        dim = self.dimension(view, "Distance", (line,))
        geometry = list(dim.SavedGeometry)
        names = list(dim.SavedNames)
        # on the spline, the line's name and geometry saved
        dim.References2D = [(view, (curve,))]
        dim.SavedGeometry = geometry
        dim.SavedNames = names
        self.assertEqual(dim.findByName(), [(0, curve, line, True)])
        dim.touch()
        self.doc.recompute()
        self.assertEqual(dim.References2D[0][1], (line,))
        self.assertIn("Up-to-date", dim.State)
        self.assertAlmostEqual(dim.getRawValue(), 10.0)

    def testAnEdgeChangedInTheModelKeepsItsDimension(self):
        """An edge whose name it still carries, changed by the model: no
        geometry matches any more, and nothing else has the name -- the
        dimension measures it as it is, not in error"""
        view = self.view()
        box = self.doc.getObject("Box")
        dim = None
        n = 0
        while dim is None:
            e = view.getEdgeByIndex(n)
            a, b = e.Vertexes[0].Point, e.Vertexes[-1].Point
            # along the box's length, as the front view draws it
            if abs(b.x - a.x) > 1e-6 and abs(b.y - a.y) < 1e-6:
                dim = self.dimension(view, "Distance", ("Edge%d" % n,))
            n += 1
        self.assertAlmostEqual(dim.getRawValue(), 10.0)
        box.Length = 30
        self.doc.recompute()
        self.assertIn("Up-to-date", dim.State)
        self.assertEqual(dim.findByName(), [])
        self.assertAlmostEqual(dim.getRawValue(), 30.0)

    def testTheHalvesOfACircleAreToldApart(self):
        """Each half of a circle has the other's ends, the other way round:
        the half a radius was on is found, not the other"""
        # facing the view, which looks along y
        circle = Part.Circle(FreeCAD.Vector(), FreeCAD.Vector(0, 1, 0), 5)
        halves = [Part.Edge(circle, 0, math.pi), Part.Edge(circle, math.pi, 2 * math.pi)]
        view = self.view(Part.Wire(halves))
        sides = []
        n = 0
        while True:
            try:
                e = view.getEdgeByIndex(n)
            except Exception:
                break
            sides.append(e.valueAt((e.FirstParameter + e.LastParameter) / 2).x)
            n += 1
        self.assertEqual(n, 2)
        # the later of the two: an earlier other half would be found first
        dim = self.dimension(view, "Radius", ("Edge1",))
        geometry = list(dim.SavedGeometry)
        self.misplace(dim, view, ("Edge0",), geometry, [""])
        self.assertEqual(dim.References2D[0][1], ("Edge1",))
        self.assertNotAlmostEqual(sides[0], sides[1])

    def testADimensionMadeWithItsViewSavesWhatItMeasures(self):
        """A dimension made before its view has drawn -- both in the same
        recompute, as a script makes them -- saved null geometry for its
        references, and nothing filled it: every recompute said "no exact
        match" and its references were never found again by their geometry.
        The entries are filled from the elements once the view has drawn
        them (docs/WorkerProcess.md sec 54)"""
        source = self.doc.addObject("Part::Box", "Box")
        view = self.doc.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(view)
        view.Source = [source]
        dim = self.doc.addObject("TechDraw::DrawViewDimension", "Dim")
        dim.Type = "DistanceX"
        dim.References2D = [(view, ("Vertex0", "Edge1"))]
        self.page.addView(dim)
        # the state it is about: saved before the view drew anything
        self.assertEqual(len(dim.SavedGeometry), 2)
        self.assertTrue(all(g.isNull() for g in dim.SavedGeometry))
        self.doc.recompute()
        self.assertFalse(any(g.isNull() for g in dim.SavedGeometry))
        # what a dimension made after its view saves for the same elements
        other = self.dimension(view, "DistanceX", ("Vertex0", "Edge1"))
        points = lambda d: [[tuple(round(c, 9) for c in v.Point) for v in g.Vertexes]
                            for g in d.SavedGeometry]
        self.assertEqual(points(dim), points(other))
        self.assertAlmostEqual(dim.getRawValue(), other.getRawValue(), 9)

    def testSavedGeometryWithANullEntryIsCopied(self):
        """An entry with no geometry -- saved for a reference before its view
        drew anything -- is a value of the list: a transaction copies the
        list before a change, and the copy threw Standard_NullObject over it
        (docs/WorkerProcess.md sec 46)"""
        view = self.view()
        dim = self.dimension(view, "DistanceX", ("Vertex0", "Edge1"))
        vertex, edge = dim.SavedGeometry
        self.doc.UndoMode = 1
        self.doc.openTransaction("null entry")
        dim.SavedGeometry = [Part.Shape(), edge]
        dim.SavedGeometry = [vertex, edge]
        self.doc.commitTransaction()
        self.assertFalse(any(g.isNull() for g in dim.SavedGeometry))
        # back to before the transaction, through the copies it took
        self.doc.undo()
        self.assertFalse(any(g.isNull() for g in dim.SavedGeometry))

    def testSavedGeometryInFilesNamedByContentIsRead(self):
        """A version of the document, which a worker reads, names each file
        of a property by its content too: `<object>.<property>.<index>.<hash>
        .brp`. Each entry is the one its file was named for in the XML, not
        a number read from the name -- it was read from the hash, out of
        range, and the entry restored null (docs/WorkerProcess.md sec 46)"""
        import os
        import re
        import shutil
        import tempfile

        import ArchiveMembers

        view = self.view()
        dim = self.dimension(view, "DistanceX", ("Vertex0", "Edge1"))
        saved = [[tuple(v.Point) for v in g.Vertexes] for g in dim.SavedGeometry]
        folder = tempfile.mkdtemp(prefix="td-named-")
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "named.FCStd")
        self.doc.saveAs(path)
        unpacked = os.path.join(folder, "unpacked")
        for item, data in ArchiveMembers.members(path):
            target = os.path.join(unpacked, item.filename)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(data)
        xml = os.path.join(unpacked, "Document.xml")
        with open(xml, encoding="utf-8") as f:
            text = f.read()
        # hashes that read as numbers out of range, or as the other entry
        hashes = {"0": "4ae0069a9881", "1": "0dffd3dcf921"}
        renamed = 0
        for name in os.listdir(unpacked):
            m = re.fullmatch(r"(Dim\.SavedGeometry)\.(\d)\.(brp|bin)", name)
            if not m:
                continue
            new = "%s.%s.%s.%s" % (m.group(1), m.group(2), hashes[m.group(2)], m.group(3))
            os.rename(os.path.join(unpacked, name), os.path.join(unpacked, new))
            text = text.replace('"%s"' % name, '"%s"' % new)
            renamed += 1
        self.assertEqual(renamed, 2)
        with open(xml, "w", encoding="utf-8") as f:
            f.write(text)
        doc = FreeCAD.openDocument(xml)
        try:
            again = doc.getObject("Dim")
            self.assertEqual(
                [[tuple(v.Point) for v in g.Vertexes] for g in again.SavedGeometry], saved)
        finally:
            FreeCAD.closeDocument(doc.Name)


if __name__ == "__main__":
    unittest.main()
