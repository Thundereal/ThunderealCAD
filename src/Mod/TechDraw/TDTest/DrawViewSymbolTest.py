
import FreeCAD
import os
import unittest
from .TechDrawTestUtilities import createPageWithSVGTemplate


class DrawViewSymbolTest(unittest.TestCase):
    def setUp(self):
        """Creates a page"""
        FreeCAD.newDocument("TDAnno")
        FreeCAD.setActiveDocument("TDAnno")
        FreeCAD.ActiveDocument = FreeCAD.getDocument("TDAnno")
        self.page = createPageWithSVGTemplate()

    def tearDown(self):
        FreeCAD.closeDocument("TDAnno")

    def testMakeSymbol(self):
        """Tests if an symbol can be added to page"""
        sym = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewSymbol", "TestSymbol")
        path = os.path.dirname(os.path.abspath(__file__))
        symbolFileSpec = path + "/TestSymbol.svg"
        f = open(symbolFileSpec, "r")
        svg = f.read()
        f.close()
        sym.Symbol = svg
        self.page.addView(sym)
        sym.X = 220.0
        sym.Y = 150.0

        FreeCAD.ActiveDocument.recompute()

        self.assertTrue("Up-to-date" in sym.State)

    def testASymbolWithBrokenSvgSaysWhere(self):
        """A symbol whose SVG does not parse draws nothing: it is in error and
        says where it broke (docs/TechDrawErrors.md 3.2)"""
        sym = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewSymbol", "Broken")
        sym.Symbol = "<svg>\n<g>\n</svg>"
        self.page.addView(sym)
        FreeCAD.ActiveDocument.recompute()
        self.assertTrue("Invalid" in sym.State, sym.State)
        self.assertIn("line 3", sym.getStatusString())

    def testASpreadsheetViewWithABadRangeSaysWhich(self):
        """A spreadsheet view whose range is wrong is in error, naming the
        cell; it logged a line and drew nothing (docs/TechDrawErrors.md 3.2)"""
        sheet = FreeCAD.ActiveDocument.addObject("Spreadsheet::Sheet", "Sheet")
        sheet.set("A1", "1")
        view = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewSpreadsheet", "SheetView")
        view.Source = sheet
        self.page.addView(view)
        view.CellStart = "A1"
        view.CellEnd = "1B"
        FreeCAD.ActiveDocument.recompute()
        self.assertTrue("Invalid" in view.State, view.State)
        self.assertIn("1B", view.getStatusString())
        view.CellStart = "C3"
        view.CellEnd = "A1"
        FreeCAD.ActiveDocument.recompute()
        self.assertTrue("Invalid" in view.State, view.State)
        self.assertIn("reversed", view.getStatusString())
        view.CellStart = "A1"
        view.CellEnd = "B2"
        FreeCAD.ActiveDocument.recompute()
        self.assertTrue("Up-to-date" in view.State, view.getStatusString())

    def testAnArchViewOfNoSectionPlaneSaysWhy(self):
        """An Arch view of something that is not a section plane is in error,
        naming it (docs/TechDrawErrors.md 3.2)"""
        box = FreeCAD.ActiveDocument.addObject("Part::Box", "NotAPlane")
        view = FreeCAD.ActiveDocument.addObject("TechDraw::DrawViewArch", "ArchView")
        view.Source = box
        self.page.addView(view)
        FreeCAD.ActiveDocument.recompute()
        self.assertTrue("Invalid" in view.State, view.State)
        self.assertIn("NotAPlane is not a section plane", view.getStatusString())

    def testAParametricTemplateWithNoScriptSaysWhich(self):
        """A parametric template whose script cannot be read is in error,
        naming it; it succeeded with nothing drawn (docs/TechDrawErrors.md 3.2)"""
        tpl = FreeCAD.ActiveDocument.addObject("TechDraw::DrawParametricTemplate", "Param")
        tpl.Template = "no_such_template_script.py"
        FreeCAD.ActiveDocument.recompute()
        self.assertTrue("Invalid" in tpl.State, tpl.State)
        self.assertIn("no_such_template_script.py", tpl.getStatusString())


if __name__ == "__main__":
    unittest.main()
