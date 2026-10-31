import FreeCAD
import unittest
from .TechDrawTestUtilities import createPageWithSVGTemplate


def tallest(view):
    """The longest vertical edge the view draws: the box's length, as shown"""
    best = 0.0
    for i in range(64):
        try:
            edge = view.getEdgeByIndex(i)
        except Exception:
            break
        a, b = edge.Vertexes[0].Point, edge.Vertexes[-1].Point
        if abs(a.x - b.x) < 1e-6:
            best = max(best, abs(a.y - b.y))
    return round(best, 3)


class DrawViewUndoTest(unittest.TestCase):
    """An undo of a change to a drawn shape leaves the drawing right or marked to recompute.

    A part view keeps its projection in the view, not in a property, so no undo puts it
    back: the undo writes the box's length and shape back, and the view shows the box it
    was last made from.  With the transaction log on, the undo also put back the touched
    state the step started from -- clean -- so the drawing was stale and said nothing
    (docs/MultiViewEdit.md sec 12.10).
    """

    def setUp(self):
        self.params = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
        self.document = FreeCAD.newDocument("TDViewUndo")
        self.box = self.document.addObject("Part::Box", "Box")
        self.box.Length, self.box.Width, self.box.Height = 15, 10, 10
        page = createPageWithSVGTemplate(self.document)
        page.Scale = 1.0
        self.view = self.document.addObject("TechDraw::DrawViewPart", "View")
        page.addView(self.view)
        self.view.Source = [self.box]
        self.view.Direction = FreeCAD.Vector(0, -1, 0)
        self.view.XDirection = FreeCAD.Vector(0, 0, 1)   # the length shows upright
        self.view.CoarseView = True   # projected on this thread
        self.view.ScaleType = "Custom"
        self.view.Scale = 1.0
        self.document.recompute()
        self.document.UndoMode = 1
        self.document.clearUndos()
        self.assertEqual(tallest(self.view), 15)

    def tearDown(self):
        self.params.RemBool("RecomputeOnUndo")
        FreeCAD.closeDocument(self.document.Name)

    def lengthStep(self):
        self.document.openTransaction("Length")
        self.box.Length = 20
        self.document.commitTransaction()
        self.document.recompute()
        self.assertEqual(tallest(self.view), 20)

    def assertRightOrMarked(self, length):
        if tallest(self.view) != length:
            self.assertIn("Touched", self.view.State,
                          "the drawing shows %s for a box of %s and is not marked"
                          % (tallest(self.view), length))

    def testUndoMarksTheDrawing(self):
        self.lengthStep()
        self.document.undo()
        self.assertEqual(self.box.Length.Value, 15)
        self.assertRightOrMarked(15)
        self.document.recompute()
        self.assertEqual(tallest(self.view), 15)
        self.document.redo()
        self.assertEqual(self.box.Length.Value, 20)
        self.assertRightOrMarked(20)

    def testRecomputeOnUndo(self):
        self.params.SetBool("RecomputeOnUndo", True)
        self.lengthStep()
        self.document.undo()
        self.assertEqual(tallest(self.view), 15)
        self.assertEqual([o.Name for o in self.document.Objects if "Touched" in o.State], [])
        self.document.redo()
        self.assertEqual(tallest(self.view), 20)
        self.assertEqual([o.Name for o in self.document.Objects if "Touched" in o.State], [])


if __name__ == "__main__":
    unittest.main()
