import unittest

import FreeCAD

from .TechDrawTestUtilities import createPageWithSVGTemplate

DIMENSIONS = "User parameter:BaseApp/Preferences/Mod/TechDraw/Dimensions"
LABELS = "User parameter:BaseApp/Preferences/Mod/TechDraw/Labels"


class DefaultSizesTest(unittest.TestCase):
    """With nothing stored in the preferences, a new dimension's text is 2.5 mm
    and its arrows 1.5 mm, and a new text annotation's text 2.5 mm (the
    reporter, 2026-11-26: "I'd like the font size default to 2.5", "arrow
    1.5", "labels 2.5 too"). What is stored is kept: the test takes the
    three values out for its run and puts back whatever was there."""

    KEYS = ((DIMENSIONS, "FontSize"), (DIMENSIONS, "ArrowSize"), (LABELS, "LabelSize"))

    def setUp(self):
        self.stored = {}
        for path, key in self.KEYS:
            group = FreeCAD.ParamGet(path)
            if key in [f[0] if isinstance(f, tuple) else f for f in group.GetFloats()]:
                self.stored[(path, key)] = group.GetFloat(key)
                group.RemFloat(key)
        self.doc = FreeCAD.newDocument("TDDefaultSizes")
        FreeCAD.setActiveDocument(self.doc.Name)
        self.page = createPageWithSVGTemplate(self.doc)

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)
        for (path, key), value in self.stored.items():
            FreeCAD.ParamGet(path).SetFloat(key, value)

    def testANewDimensionIsTwoAndAHalfWithArrowsOfOneAndAHalf(self):
        box = self.doc.addObject("Part::Box", "Box")
        view = self.doc.addObject("TechDraw::DrawViewPart", "View")
        self.page.addView(view)
        view.Source = [box]
        dim = self.doc.addObject("TechDraw::DrawViewDimension", "Dim")
        dim.Type = "Distance"
        dim.References2D = [(view, "Edge0")]
        self.page.addView(dim)
        self.doc.recompute()
        self.assertAlmostEqual(dim.ViewObject.Fontsize.Value, 2.5)
        self.assertAlmostEqual(dim.ViewObject.Arrowsize.Value, 1.5)

    def testANewTextAnnotationIsTwoAndAHalf(self):
        anno = self.doc.addObject("TechDraw::DrawViewAnnotation", "Annotation")
        self.page.addView(anno)
        self.doc.recompute()
        self.assertAlmostEqual(float(getattr(anno.TextSize, "Value", anno.TextSize)), 2.5)

    def testAStoredSizeIsKept(self):
        FreeCAD.ParamGet(DIMENSIONS).SetFloat("FontSize", 4.0)
        try:
            box = self.doc.addObject("Part::Box", "Box")
            view = self.doc.addObject("TechDraw::DrawViewPart", "View")
            self.page.addView(view)
            view.Source = [box]
            dim = self.doc.addObject("TechDraw::DrawViewDimension", "Dim")
            dim.Type = "Distance"
            dim.References2D = [(view, "Edge0")]
            self.page.addView(dim)
            self.doc.recompute()
            self.assertAlmostEqual(dim.ViewObject.Fontsize.Value, 4.0)
        finally:
            FreeCAD.ParamGet(DIMENSIONS).RemFloat("FontSize")


if __name__ == "__main__":
    unittest.main()
