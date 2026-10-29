# The OCCT fork's offset fix level (BRepOffset_SetFixLevel) on the features
# that offset. A correction of the fork's that gives a different shape than
# before for some input is made only at the level that brings it: an object
# made from now on recomputes at the latest level FreeCAD knows, one saved
# before at 0, as it did (its _OffsetFixLevel, which a file from before has
# not got).

import os
import unittest

import FreeCAD as App
import Part

from parttests.occt_fork import needsOCCTFunction

# A box under a B-spline dome, the dome's four edges filleted r3; Face4 is its
# bottom. The OCCT fork's tests/fork/boolean/models/dome_fillet_r3.brep: with
# the Intersection join it offsets outward from level 1 only (the fork's
# Thickness.md sec 31).
DOME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dome_fillet_r3.brep")


def domeShape():
    shape = Part.Shape()
    shape.importBrep(DOME)
    return shape


@needsOCCTFunction("TKOffset", "BRepOffset_SetFixLevel")
class OffsetFixLevelTest(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("OffsetFixLevel")
        self.dome = self.doc.addObject("Part::Feature", "Dome")
        self.dome.Shape = domeShape()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def thickness(self):
        t = self.doc.addObject("Part::Thickness", "Thickness")
        t.Faces = (self.dome, ["Face4"])
        t.Value = 1.0
        t.Join = "Intersection"
        return t

    def offset(self):
        o = self.doc.addObject("Part::Offset", "Offset")
        o.Source = self.dome
        o.Value = 1.0
        o.Join = "Intersection"
        return o

    def test_new_thickness_is_made_at_the_latest_level(self):
        """The dome outward with the Intersection join: made at level 1"""
        t = self.thickness()
        self.assertGreaterEqual(t._OffsetFixLevel, 1)
        self.doc.recompute()
        self.assertNotIn("Invalid", t.State)
        self.assertTrue(t.Shape.isValid())
        # the fork's suite: dome_join_out
        self.assertAlmostEqual(t.Shape.Volume, 11009.9518, delta=11.0)

    def test_thickness_from_before_recomputes_as_it_did(self):
        """A thickness saved before the level: refused, as it was"""
        t = self.thickness()
        t._OffsetFixLevel = 0
        self.doc.recompute()
        self.assertIn("Invalid", t.State)

    def test_level_is_put_back_after_a_recompute(self):
        """The level holds for the feature's recompute only: an offset made
        on the thread afterwards, outside any feature, is at level 0"""
        t = self.thickness()
        self.doc.recompute()
        self.assertTrue(t.Shape.isValid())
        shape = domeShape()
        with self.assertRaises(Exception):
            shape.makeThickness([shape.Faces[3]], 1.0, 1e-7, False, False, 0, 2)

    def test_new_offset_is_made_at_the_latest_level(self):
        """The whole dome offset outward with the Intersection join: a null
        shape at level 0"""
        o = self.offset()
        self.assertGreaterEqual(o._OffsetFixLevel, 1)
        self.doc.recompute()
        self.assertNotIn("Invalid", o.State)
        self.assertTrue(o.Shape.isValid())
        # the Arc join's 130072.93, and the wedges and corners the sharp
        # edges gain, d^2 (tan(phi/2) - phi/2) along each, about 77.4
        self.assertAlmostEqual(o.Shape.Volume, 130150.5282, delta=13.0)

    def test_offset_from_before_recomputes_as_it_did(self):
        o = self.offset()
        o._OffsetFixLevel = 0
        self.doc.recompute()
        self.assertIn("Invalid", o.State)

    def test_level_is_saved(self):
        """A file keeps each object's level: one made at 0 reopens at 0"""
        t = self.thickness()
        t._OffsetFixLevel = 0
        o = self.offset()
        names = t.Name, o.Name
        path = os.path.join(App.getTempPath(), "OffsetFixLevel.FCStd")
        self.doc.saveAs(path)
        # where the document went: this FreeCAD may save in a format of its own
        path = self.doc.FileName
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        try:
            self.assertEqual(self.doc.getObject(names[0])._OffsetFixLevel, 0)
            self.assertGreaterEqual(self.doc.getObject(names[1])._OffsetFixLevel, 1)
        finally:
            os.remove(path)

    def test_other_features_that_offset_take_the_level(self):
        """The features whose shapes come from an offset or thickness: made
        at the latest level"""
        for type_name in ("Part::SubShapeBinder",):
            obj = self.doc.addObject(type_name, "Obj")
            self.assertGreaterEqual(obj._OffsetFixLevel, 1, type_name)
