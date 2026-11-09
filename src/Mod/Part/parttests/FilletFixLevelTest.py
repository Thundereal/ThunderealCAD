# The OCCT fork's fillet fix level (ChFi3d_SetFixLevel) on the features that
# fillet and chamfer. A correction of the fork's that gives a different shape
# than before for some input is made only at the level that brings it: an
# object made from now on recomputes at the latest level FreeCAD knows, one
# saved before at 0, as it did (its _FilletFixLevel, which a file from before
# has not got). What was refused before needs no level: below the latest, a
# fillet the level does not make is computed again at the latest.
# Level 1 moves a chamfer's guide only near the end it corrects: on #631's arm
# a chamfer of 0.8 ran from 0.52 to 1.37 wide along its edge, and is exact
# from level 1; at 0.3 it was refused, and is made at every level (the fork's
# tests/fork/fillet/README.md, "A chamfer's guide bent at its other end").
# That the level is put back after a recompute is
# tests/src/Mod/Part/App/FilletFixLevel.cpp.

import os
import unittest

import FreeCAD as App
import Part

from parttests.occt_fork import needsOCCTFunction

# realthunder/FreeCAD#631's arm (the fork's
# tests/fork/fillet/models/issue631_fillet002_base.brep); Edge12 is the
# ellipse where the arm's underside cuts the cylinder of radius 49 at y +18
ARM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "issue631_arm.brep")
EDGE = "Edge12"
# the chamfers of Edge12, by size: the one made before and the exact one
BEFORE_08 = 41511.45232
EXACT_08 = 41511.954615
EXACT_03 = 41535.183701


def armShape():
    shape = Part.Shape()
    shape.importBrep(ARM)
    return shape


@needsOCCTFunction("TKFillet", "ChFi3d_SetFixLevel")
class FilletFixLevelTest(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("FilletFixLevel")
        self.arm = self.doc.addObject("Part::Feature", "Arm")
        self.arm.Shape = armShape()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def chamfer(self, size):
        c = self.doc.addObject("Part::Chamfer", "Chamfer")
        c.Base = self.arm
        c.Edges = [(12, size, size)]
        return c

    def bodyChamfer(self, size):
        body = self.doc.addObject("PartDesign::Body", "Body")
        body.BaseFeature = self.arm
        c = body.newObject("PartDesign::Chamfer", "Chamfer")
        c.Base = (body.BaseFeature, [EDGE])
        c.Size = size
        return c

    def assertMade(self, obj, volume):
        self.doc.recompute()
        self.assertNotIn("Invalid", obj.State)
        self.assertTrue(obj.Shape.isValid())
        self.assertAlmostEqual(obj.Shape.Volume, volume, delta=1e-4)

    def test_new_chamfer_is_made_at_the_latest_level(self):
        """Part's chamfer of 0.8 made now: at the latest level, exact"""
        c = self.chamfer(0.8)
        self.assertGreaterEqual(c._FilletFixLevel, 1)
        self.assertMade(c, EXACT_08)

    def test_chamfer_from_before_as_it_was(self):
        """A chamfer saved before the level, at 0: as it was made then"""
        c = self.chamfer(0.8)
        c._FilletFixLevel = 0
        self.assertMade(c, BEFORE_08)

    def test_chamfer_refused_before_is_made_at_0(self):
        """At 0.3 the chamfer was refused: at 0 it is made, the latest's"""
        c = self.chamfer(0.3)
        c._FilletFixLevel = 0
        self.assertMade(c, EXACT_03)

    def test_partdesign_chamfer(self):
        """PartDesign's chamfer the same, at either level"""
        c = self.bodyChamfer(0.8)
        self.assertGreaterEqual(c._FilletFixLevel, 1)
        self.assertMade(c, EXACT_08)
        c._FilletFixLevel = 0
        c.touch()
        self.assertMade(c, BEFORE_08)

    def test_chamfer_without_a_feature(self):
        """Outside any feature, at the thread's level 0: as before"""
        shape = armShape()
        r = shape.makeChamfer(0.8, [shape.Edges[11]])
        self.assertTrue(r.isValid())
        self.assertAlmostEqual(r.Volume, BEFORE_08, delta=1e-4)

    def test_level_is_saved(self):
        """A file keeps each object's level: one made at 0 reopens at 0"""
        c0 = self.chamfer(0.8)
        c0._FilletFixLevel = 0
        c1 = self.chamfer(0.8)
        names = c0.Name, c1.Name
        path = os.path.join(App.getTempPath(), "FilletFixLevel.FCStd")
        self.doc.saveAs(path)
        # where the document went: this FreeCAD may save in a format of its own
        path = self.doc.FileName
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        try:
            self.assertEqual(self.doc.getObject(names[0])._FilletFixLevel, 0)
            self.assertGreaterEqual(self.doc.getObject(names[1])._FilletFixLevel, 1)
        finally:
            os.remove(path)

    def test_other_features_take_the_level(self):
        """The fillets, Part's and PartDesign's: made at the latest level"""
        body = self.doc.addObject("PartDesign::Body", "Body")
        objs = [
            self.doc.addObject("Part::Fillet", "Fillet"),
            body.newObject("PartDesign::Fillet", "Fillet"),
        ]
        for obj in objs:
            self.assertGreaterEqual(obj._FilletFixLevel, 1, obj.TypeId)
