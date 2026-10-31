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


def touched(document):
    return [o.Name for o in document.Objects if "Touched" in o.State]


class DrawViewUndoTest(unittest.TestCase):
    """An undo of a change to a drawn shape leaves the drawing right, with nothing to recompute.

    A part view stores its projection in ProjectedGeometry, and the undo puts that back with
    the box's length and shape.  The view used to read it only when the file was opened, so
    it went on drawing the box it was last made from; and with the transaction log on, the
    undo put back the touched state the step started from -- clean -- so the drawing was
    stale and said nothing (docs/MultiViewEdit.md sec 12.10).  It draws what the undo put
    back now (docs/TechDrawStoredGeometry.md sec 6).
    """

    log = None   # the transaction log preference as the user has it

    def setUp(self):
        self.params = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
        self.tdParams = FreeCAD.ParamGet(
            "User parameter:BaseApp/Preferences/Mod/TechDraw/General")
        if self.log is not None:
            self.oldLog = self.params.GetInt("TransactionLog", -1)
            self.params.SetInt("TransactionLog", self.log)
        self.document = FreeCAD.newDocument("TDViewUndo")
        self.box = self.document.addObject("Part::Box", "Box")
        self.box.Length, self.box.Width, self.box.Height = 15, 10, 10
        page = createPageWithSVGTemplate(self.document)
        page.Scale = 1.0
        self.page = page
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
        self.tdParams.RemBool("StoreProjectedGeometry")
        FreeCAD.closeDocument(self.document.Name)
        if self.log is not None:
            if self.oldLog < 0:
                self.params.RemInt("TransactionLog")
            else:
                self.params.SetInt("TransactionLog", self.oldLog)

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

    def testUndoDrawsWhatItPutBack(self):
        self.lengthStep()
        self.document.undo()
        self.assertEqual(tallest(self.view), 15)
        self.assertNotIn("View", touched(self.document))
        self.document.redo()
        self.assertEqual(tallest(self.view), 20)
        self.assertNotIn("View", touched(self.document))

    def testUndoBelowAnotherStep(self):
        other = self.document.addObject("Part::Box", "Other")
        self.document.recompute()
        self.document.clearUndos()
        self.lengthStep()
        self.document.openTransaction("Other")
        other.Length = 30
        self.document.commitTransaction()
        self.document.recompute()
        self.document.undo(1)   # the length, under the newer step
        self.assertEqual(self.box.Length.Value, 15)
        self.assertEqual(other.Length.Value, 30)
        self.assertEqual(tallest(self.view), 15)
        self.assertNotIn("View", touched(self.document))

    def testUndoWithNothingStoredMarks(self):
        self.tdParams.SetBool("StoreProjectedGeometry", False)
        self.view.touch()
        self.document.recompute()   # stores nothing now
        self.document.clearUndos()
        self.lengthStep()
        self.document.undo()
        self.assertEqual(self.box.Length.Value, 15)
        self.assertRightOrMarked(15)

    def testRecomputeOnUndo(self):
        self.params.SetBool("RecomputeOnUndo", True)
        self.lengthStep()
        self.document.undo()
        self.assertEqual(tallest(self.view), 15)
        self.assertEqual([o.Name for o in self.document.Objects if "Touched" in o.State], [])
        self.document.redo()
        self.assertEqual(tallest(self.view), 20)
        self.assertEqual([o.Name for o in self.document.Objects if "Touched" in o.State], [])


class DrawViewUndoLogOffTest(DrawViewUndoTest):
    """The same with the transaction log off: undo writes from the in-memory steps alone"""

    log = 0


if __name__ == "__main__":
    unittest.main()
