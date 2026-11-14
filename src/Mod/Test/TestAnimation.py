# SPDX-License-Identifier: LGPL-2.1-or-later
# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
# *                                                                         *
# *   This file is part of FreeCAD.                                         *
# *                                                                         *
# *   FreeCAD is free software: you can redistribute it and/or modify it    *
# *   under the terms of the GNU Lesser General Public License as           *
# *   published by the Free Software Foundation, either version 2.1 of the  *
# *   License, or (at your option) any later version.                       *
# *                                                                         *
# ***************************************************************************

"""App::Animation, headless (docs/Animation.md sections 3 to 5, phase 2)."""

import math
import os
import tempfile
import unittest

import FreeCAD as App


class TestAnimation(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestAnimation")
        self.doc.UndoMode = 1
        obj = self.doc.addObject("App::FeaturePython", "Obj")
        obj.addProperty("App::PropertyFloat", "F")
        obj.addProperty("App::PropertyLength", "L")
        obj.addProperty("App::PropertyBool", "B")
        obj.addProperty("App::PropertyInteger", "I")
        obj.addProperty("App::PropertyPlacement", "P")
        obj.addProperty("App::PropertyFloat", "G")
        obj.F = 1.0
        obj.L = 4.0
        obj.I = 3
        self.obj = obj
        self.anim = self.doc.addObject("App::Animation", "Anim")
        self.doc.recompute()
        self.files = []

    def tearDown(self):
        App.closeDocument(self.doc.Name)
        for f in self.files:
            try:
                os.remove(f)
            except OSError:
                pass

    def tempFile(self):
        fd, name = tempfile.mkstemp(suffix=".FCStd")
        os.close(fd)
        self.files.append(name)
        return name

    def reopen(self):
        name = self.tempFile()
        self.doc.saveAs(name)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(name)
        self.obj = self.doc.getObject("Obj")
        self.anim = self.doc.getObject("Anim")

    # -- evaluation --------------------------------------------------------

    def testLinearFloat(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 2.0, 10.0)
        self.anim.Time = 1.0
        self.assertTrue(self.anim.Active)
        self.assertAlmostEqual(self.obj.F, 5.0)
        self.anim.Time = 3.0
        self.assertAlmostEqual(self.obj.F, 10.0)  # Constant after the last key
        self.anim.stop()
        self.assertFalse(self.anim.Active)
        self.assertAlmostEqual(self.obj.F, 1.0)  # the rest value

    def testKeyTheCurrentValue(self):
        self.anim.addKey("Obj.L", 0.0)
        self.obj.L = 8.0
        self.anim.addKey("Obj.L", 1.0)
        self.assertEqual([k[0] for k in self.anim.keys("Obj.L")], [0.0, 1.0])
        self.assertAlmostEqual(self.anim.value("Obj.L", 0.5).Value, 6.0)
        self.anim.addKey("Obj.L", 0.5, "1 cm")
        self.assertAlmostEqual(self.anim.value("Obj.L", 0.5).Value, 10.0)

    def testValueWritesNothing(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 2.0)
        self.assertAlmostEqual(self.anim.value("Obj.F", 0.5), 1.0)
        self.assertAlmostEqual(self.obj.F, 1.0)
        self.assertFalse(self.anim.Active)

    def testIntegerRounds(self):
        self.anim.addKey("Obj.I", 0.0, 0)
        self.anim.addKey("Obj.I", 1.0, 3)
        self.anim.Time = 0.5
        self.assertEqual(self.obj.I, 2)

    def testEasing(self):
        self.anim.addKey("Obj.F", 0.0, 0.0, easing=("Quad", "In"))
        self.anim.addKey("Obj.F", 1.0, 1.0)
        self.assertEqual(self.anim.track("Obj.F")["Keys"][0]["Interp"], "Easing")
        self.assertAlmostEqual(self.anim.value("Obj.F", 0.5), 0.25)
        self.anim.setInterp("Obj.F", 0.0, "Constant")
        self.assertAlmostEqual(self.anim.value("Obj.F", 0.5), 0.0)

    def testStepped(self):
        self.anim.addKey("Obj.B", 0.0, True)
        self.anim.addKey("Obj.B", 1.0, False)
        self.assertTrue(self.anim.track("Obj.B")["Stepped"])
        self.anim.Time = 1.5
        self.assertFalse(self.obj.B)
        self.anim.Time = 0.5
        self.assertTrue(self.obj.B)
        self.anim.stop()
        self.assertFalse(self.obj.B)

    def testExtrapolation(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 1.0)
        self.anim.setTrack("Obj.F", extrapolation=("Constant", "Cycle"))
        self.assertAlmostEqual(self.anim.value("Obj.F", 1.25), 0.25)
        self.anim.setTrack("Obj.F", extrapolation="Linear")
        self.assertAlmostEqual(self.anim.value("Obj.F", 3.0), 3.0)
        self.assertAlmostEqual(self.anim.value("Obj.F", -1.0), -1.0)

    def testPlacementWholeAndPart(self):
        rot = App.Rotation(App.Vector(0, 0, 1), 90)
        self.anim.addKey("Obj.P", 0.0, App.Placement(App.Vector(0, 0, 0), App.Rotation()))
        self.anim.addKey("Obj.P", 2.0, App.Placement(App.Vector(10, 0, 0), rot))
        self.anim.Time = 1.0
        self.assertAlmostEqual(self.obj.P.Base.x, 5.0)
        self.assertAlmostEqual(self.obj.P.Rotation.Angle, math.radians(45))
        # A part applied after the whole
        self.anim.addKey("Obj.P.Base.z", 0.0, 0.0)
        self.anim.addKey("Obj.P.Base.z", 2.0, 20.0)
        self.anim.Time = 1.0
        self.assertAlmostEqual(self.obj.P.Base.x, 5.0)
        self.assertAlmostEqual(self.obj.P.Base.z, 10.0)
        self.anim.stop()
        self.assertTrue(self.obj.P.isSame(App.Placement()))

    def testAnglePart(self):
        self.anim.addKey("Obj.P.Rotation.Angle", 0.0, 0.0)
        self.anim.addKey("Obj.P.Rotation.Angle", 1.0, 90.0)
        self.anim.Time = 0.5
        self.assertAlmostEqual(math.degrees(self.obj.P.Rotation.Angle), 45.0)

    def testTwoTurns(self):
        # An angle part is a scalar: 720 deg is two turns
        self.anim.addKey("Obj.P.Rotation.Angle", 0.0, 0.0)
        self.anim.addKey("Obj.P.Rotation.Angle", 1.0, 720.0)
        self.anim.Time = 0.25
        self.assertAlmostEqual(math.degrees(self.obj.P.Rotation.Angle), 180.0, places=5)

    def testRotationModeConverts(self):
        rot = App.Rotation(App.Vector(0, 0, 1), 60)
        self.anim.addKey("Obj.P", 0.0, App.Placement(App.Vector(), App.Rotation()))
        self.anim.addKey("Obj.P", 1.0, App.Placement(App.Vector(), rot))
        self.anim.setTrack("Obj.P", rotation="Euler")
        self.assertEqual(len(self.anim.track("Obj.P")["Keys"][1]["Numbers"]), 6)
        self.assertTrue(self.anim.value("Obj.P", 1.0).Rotation.isSame(rot, 1e-9))
        self.assertAlmostEqual(math.degrees(self.anim.value("Obj.P", 0.5).Rotation.Angle), 30.0)

    def testFrames(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 1.0)
        self.anim.FrameRate = 4
        self.assertEqual(self.anim.frames(), [0.0, 0.25, 0.5, 0.75, 1.0])

    # -- not an edit -------------------------------------------------------

    def testPlayingMakesNoUndoStep(self):
        self.doc.openTransaction("keys")
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 2.0)
        self.doc.commitTransaction()
        count = self.doc.UndoCount
        for t in (0.1, 0.5, 0.9):
            self.anim.Time = t
        self.anim.stop()
        self.assertEqual(self.doc.UndoCount, count)

    def testUndoKeyEdit(self):
        self.doc.openTransaction("key 1")
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.doc.commitTransaction()
        self.doc.openTransaction("key 2")
        self.anim.addKey("Obj.F", 1.0, 2.0)
        self.doc.commitTransaction()
        self.assertEqual(len(self.anim.keys("Obj.F")), 2)
        self.doc.undo()
        self.assertEqual(len(self.anim.keys("Obj.F")), 1)
        self.doc.redo()
        self.assertEqual(len(self.anim.keys("Obj.F")), 2)

    def testEditWhileActiveIsTheDesign(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 4.0)
        self.anim.Time = 0.5
        self.doc.openTransaction("edit")
        self.obj.F = 7.0
        self.doc.commitTransaction()
        self.anim.Time = 0.75
        self.assertAlmostEqual(self.obj.F, 3.0)
        self.anim.stop()
        self.assertAlmostEqual(self.obj.F, 7.0)  # the edit, not the pose

    def testUndoWhileActiveActsOnTheDesign(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 4.0)
        self.anim.Time = 0.5
        self.assertAlmostEqual(self.obj.F, 2.0)
        self.doc.openTransaction("edit")
        self.obj.F = 7.0
        self.doc.commitTransaction()
        self.doc.undo()
        # The value before the edit is the rest value, not the pose
        self.anim.stop()
        self.assertAlmostEqual(self.obj.F, 1.0)
        self.doc.redo()
        self.assertAlmostEqual(self.obj.F, 7.0)

    def testSaveWritesTheRestValues(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 4.0)
        self.anim.Recompute = "Never"
        self.anim.Time = 0.5
        name = self.tempFile()
        self.doc.saveAs(name)
        self.assertAlmostEqual(self.obj.F, 2.0)  # the pose is back after the save
        self.anim.stop()
        self.anim.Active = True  # saved active: evaluates once when opened
        self.doc.save()
        App.closeDocument(self.doc.Name)
        doc = App.openDocument(name)
        try:
            anim = doc.getObject("Anim")
            obj = doc.getObject("Obj")
            self.assertTrue(anim.Active)
            self.assertAlmostEqual(obj.F, 2.0)  # the pose at Time
            anim.stop()
            self.assertAlmostEqual(obj.F, 1.0)  # its rest value: the original 1.0
            anim.Time = 0.0
            self.assertAlmostEqual(obj.F, 0.0)
            anim.stop()
            self.assertAlmostEqual(obj.F, 1.0)
        finally:
            App.closeDocument(doc.Name)
            self.doc = App.newDocument("TestAnimation")

    def testTracksRoundTrip(self):
        self.anim.addKey("Obj.F", 0.0, 0.0, interp="Bezier")
        self.anim.addKey("Obj.F", 1.0, 2.0, easing=("Elastic", "OutIn", 1.5, 0.4, 2.0))
        self.anim.addKey("Obj.F", 3.0, -1.0)
        self.anim.setTrack("Obj.F", label="the float", mute=False,
                           extrapolation=("PingPong", "CycleOffset"))
        self.anim.addKey("Obj.B", 0.5, True)
        self.anim.addKey("Obj.P", 0.0, App.Placement(App.Vector(1, 2, 3), App.Rotation(10, 20, 30)))
        self.anim.setTrack("Obj.P", rotation="Euler")
        self.anim.addKey("Obj.P.Base.z", 2.0, 5.0)
        before = {p: self.anim.track(p) for p in ("Obj.F", "Obj.B", "Obj.P", "Obj.P.Base.z")}
        values = [self.anim.value("Obj.F", t / 10.0) for t in range(-10, 50)]
        self.reopen()
        after = {p: self.anim.track(p) for p in before}
        self.assertEqual(before, after)
        self.assertEqual(values, [self.anim.value("Obj.F", t / 10.0) for t in range(-10, 50)])

    # -- the document ------------------------------------------------------

    def testOneActive(self):
        other = self.doc.addObject("App::Animation", "Anim2")
        self.anim.addKey("Obj.F", 0.0, 5.0)
        other.addKey("Obj.L", 0.0, 9.0)
        # Time is 0 already: an equal write changes nothing, so activate()
        self.anim.activate()
        self.assertTrue(self.anim.Active)
        self.assertAlmostEqual(self.obj.F, 5.0)
        other.activate()
        self.assertFalse(self.anim.Active)
        self.assertAlmostEqual(self.obj.F, 1.0)  # put back by the switch
        self.assertAlmostEqual(self.obj.L.Value, 9.0)

    def testDriver(self):
        self.obj.setExpression("G", "Anim.Time * 2")
        self.doc.recompute()
        self.anim.Time = 3.0
        self.assertAlmostEqual(self.obj.G, 6.0)
        self.anim.Time = 1.5
        self.assertAlmostEqual(self.obj.G, 3.0)

    def testDrivenIsWrittenAndWarned(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.assertEqual(self.anim.state("Obj.F"), "Ok")
        self.obj.setExpression("F", "4")
        self.assertEqual(self.anim.state("Obj.F"), "Driven")

    def testDeletedTargetIsUnresolved(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 4.0)
        self.anim.Time = 0.5
        self.assertAlmostEqual(self.obj.F, 2.0)
        self.doc.openTransaction("delete")
        self.doc.removeObject("Obj")
        self.doc.commitTransaction()
        self.assertEqual(self.anim.state("Obj.F"), "Unresolved")
        self.anim.Time = 0.75  # skipped, not an error
        self.doc.undo()
        obj = self.doc.getObject("Obj")
        self.assertEqual(self.anim.state("Obj.F"), "Ok")
        self.anim.stop()
        self.assertAlmostEqual(obj.F, 1.0)

    def testRemovedPropertyIsUnresolved(self):
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.Time = 0.0
        self.obj.removeProperty("F")
        self.assertEqual(self.anim.state("Obj.F"), "Unresolved")
        self.anim.Time = 0.5
        self.anim.stop()

    def testDeletingTheAnimationPutsTheDesignBack(self):
        self.anim.addKey("Obj.F", 0.0, 3.0)
        self.anim.activate()
        self.assertAlmostEqual(self.obj.F, 3.0)
        self.doc.removeObject("Anim")
        self.assertAlmostEqual(self.obj.F, 1.0)

    # -- making keys -------------------------------------------------------

    def testKeyStateAndKeyNow(self):
        self.assertEqual(self.anim.keyState("Obj.F"), "None")
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 2.0, 4.0)
        self.anim.Time = 1.0
        self.assertEqual(self.anim.keyState("Obj.F"), "Animated")
        self.obj.F = 9.0  # Record off: the design, shown until the next evaluation
        self.assertEqual(self.anim.keyState("Obj.F"), "Changed")
        self.anim.keyNow("Obj.F")
        self.assertEqual(self.anim.keyState("Obj.F"), "Keyed")
        self.assertAlmostEqual(self.anim.value("Obj.F", 1.0), 9.0)
        self.assertEqual(self.anim.nextKeyTime(), 2.0)
        self.assertEqual(self.anim.nextKeyTime(False), 0.0)

    def testKeyNowKeysMovedParts(self):
        self.anim.addKey("Obj.P", 0.0, App.Placement())
        self.anim.addKey("Obj.P.Base.z", 0.0, 0.0)
        self.anim.addKey("Obj.P.Base.z", 2.0, 10.0)
        self.anim.Time = 1.0
        self.assertAlmostEqual(self.obj.P.Base.z, 5.0)
        self.obj.P = App.Placement(App.Vector(3, 0, 20), App.Rotation())
        self.anim.keyNow("Obj.P")
        self.anim.Time = 0.5
        self.anim.Time = 1.0
        # The part keyed with its piece: the pose stays as posed
        self.assertAlmostEqual(self.obj.P.Base.x, 3.0)
        self.assertAlmostEqual(self.obj.P.Base.z, 20.0)

    def testRecord(self):
        self.anim.Recording = True
        self.assertTrue(self.anim.Active)
        self.anim.Time = 0.5  # nothing animated yet
        self.doc.openTransaction("pose 1")
        self.obj.F = 3.0
        self.doc.commitTransaction()
        self.anim.Time = 1.5
        self.doc.openTransaction("pose 2")
        self.obj.F = 6.0
        self.obj.F = 7.0  # a drag: one key, the last value
        self.doc.commitTransaction()
        self.assertEqual([k[0] for k in self.anim.keys("Obj.F")], [0.5, 1.5])
        self.assertAlmostEqual(self.anim.value("Obj.F", 1.5), 7.0)
        self.anim.Time = 1.0
        self.assertAlmostEqual(self.obj.F, 5.0)
        self.anim.Recording = False
        self.anim.stop()
        self.assertAlmostEqual(self.obj.F, 1.0)  # posing while recording is not the design
        # Skipped: what Record never keys
        self.anim.Exclude = ["Obj.L"]
        self.anim.Recording = True
        self.obj.L = 6.0
        self.obj.Label = "Renamed"
        self.anim.Recording = False
        paths = [t["Path"] for t in self.anim.Tracks]
        self.assertEqual(paths, ["Obj.F"])

    def testRecordUndo(self):
        self.anim.Recording = True
        self.anim.Time = 1.0
        self.doc.openTransaction("pose")
        self.obj.F = 3.0
        self.doc.commitTransaction()
        self.assertEqual(len(self.anim.keys("Obj.F")), 1)
        self.doc.undo()  # the track Record made, its key and the pose: one step
        self.assertEqual(len(self.anim.Tracks), 0)
        self.assertAlmostEqual(self.obj.F, 1.0)

    # -- strips -------------------------------------------------------------

    def strips(self):
        a1 = self.doc.addObject("App::Animation", "A1")
        a1.addKey("Obj.F", 0.0, 0.0)
        a1.addKey("Obj.F", 1.0, 10.0)
        a2 = self.doc.addObject("App::Animation", "A2")
        a2.addKey("Obj.F", 0.0, 10.0)
        a2.addKey("Obj.F", 2.0, 20.0)
        return a1, a2

    def testStripsInSequence(self):
        a1, a2 = self.strips()
        self.anim.addStrip(a1)
        self.anim.addStrip(a2)  # After: when the first ends
        spans = [(s["Begin"], s["End"]) for s in self.anim.strips()]
        self.assertEqual(spans, [(0.0, 1.0), (1.0, 3.0)])
        self.assertEqual(self.anim.frames()[-1], 3.0)
        self.anim.Time = 0.5
        self.assertAlmostEqual(self.obj.F, 5.0)
        self.anim.Time = 1.5
        self.assertAlmostEqual(self.obj.F, 12.5)
        self.anim.Time = 4.0
        self.assertAlmostEqual(self.obj.F, 20.0)  # the last pose held
        self.anim.stop()
        self.assertAlmostEqual(self.obj.F, 1.0)

    def testStripWith(self):
        a1, a2 = self.strips()
        self.anim.addStrip(a1)
        self.anim.addStrip(a2, start="With", offset=0.5)
        self.assertEqual([s["Begin"] for s in self.anim.strips()], [0.0, 0.5])
        self.anim.Time = 0.25
        self.assertAlmostEqual(self.obj.F, 2.5)  # the second not started
        self.anim.Time = 1.0
        self.assertAlmostEqual(self.obj.F, 12.5)  # the second over the first

    def testStripTiming(self):
        a1, _ = self.strips()
        self.anim.addStrip(a1, reverse=True)
        self.anim.Time = 0.25
        self.assertAlmostEqual(self.obj.F, 7.5)
        self.anim.setStrip(a1, reverse=False, scale=2.0)
        self.assertEqual(self.anim.strips()[0]["End"], 2.0)
        self.anim.Time = 0.5
        self.assertAlmostEqual(self.obj.F, 2.5)
        self.anim.setStrip(a1, scale=1.0, repeat=3)
        self.assertEqual(self.anim.strips()[0]["End"], 3.0)
        self.anim.Time = 2.25
        self.assertAlmostEqual(self.obj.F, 2.5)

    def testStripAdd(self):
        # This one's own: the part slides 100 along x in 2 s
        self.anim.addKey("Obj.P", 0.0, App.Placement())
        self.anim.addKey("Obj.P", 2.0, App.Placement(App.Vector(100, 0, 0), App.Rotation()))
        # A strip added on top: a hop of 5 up and back, and a quarter turn
        hop = self.doc.addObject("App::Animation", "Hop")
        hop.addKey("Obj.P", 0.0, App.Placement())
        hop.addKey("Obj.P", 0.5, App.Placement(App.Vector(0, 0, 5),
                                               App.Rotation(App.Vector(0, 0, 1), 90)))
        hop.addKey("Obj.P", 1.0, App.Placement())
        self.anim.addStrip(hop, start="At", blend="Add")
        self.anim.Time = 0.5
        self.assertAlmostEqual(self.obj.P.Base.x, 25.0)
        self.assertAlmostEqual(self.obj.P.Base.z, 5.0)
        self.assertAlmostEqual(math.degrees(self.obj.P.Rotation.Angle), 90.0, places=5)
        self.anim.Time = 1.5
        self.assertAlmostEqual(self.obj.P.Base.x, 75.0)
        self.assertAlmostEqual(self.obj.P.Base.z, 0.0)
        # Alone, an Add strip adds to the rest value
        self.anim.removeTrack("Obj.P")
        self.obj.P = App.Placement(App.Vector(1, 2, 3), App.Rotation())
        self.anim.Time = 0.5
        self.assertTrue(self.obj.P.Base.isEqual(App.Vector(1, 2, 8), 1e-9), self.obj.P.Base)
        self.anim.Time = 0.25
        self.anim.Time = 0.5  # and does not pile up frame after frame
        self.assertTrue(self.obj.P.Base.isEqual(App.Vector(1, 2, 8), 1e-9), self.obj.P.Base)

    def testClickPointsAndSave(self):
        a1, a2 = self.strips()
        self.anim.addStrip(a1)
        self.anim.addStrip(a2, start="Click", offset=0.5, repeat=2, blend="Replace")
        self.assertEqual(self.anim.clickPoints(), [1.5])
        before = self.anim.strips()
        self.reopen()
        self.assertEqual(self.anim.strips(), before)
        self.assertEqual(self.anim.state("Strip:A2"), "Ok")

    def testStripCycleRefused(self):
        a1, a2 = self.strips()
        a1.addStrip(a2)
        with self.assertRaises(Exception):
            a2.addStrip(a1)
        with self.assertRaises(Exception):
            a1.addStrip(a1)

    # -- export ------------------------------------------------------------

    def gltfScene(self):
        try:
            import Part  # noqa: F401
        except ImportError:
            self.skipTest("the glTF export meshes shapes with Part")
        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        still = self.doc.addObject("Part::Sphere", "Still")
        self.doc.recompute()
        self.anim.FrameRate = 10
        self.anim.addKey("Box.Placement", 0.0)
        self.anim.addKey(
            "Box.Placement", 1.0, App.Placement(App.Vector(0, 0, 50), App.Rotation())
        )
        self.anim.addKey("Cyl.Visibility", 0.0, True)
        self.anim.addKey("Cyl.Visibility", 0.5, False)
        return box, cyl, still

    def readGltf(self, name):
        import json
        import struct

        with open(name, "rb") as f:
            gltf = json.load(f)
        with open(os.path.splitext(name)[0] + ".bin", "rb") as f:
            blob = f.read()

        def values(index):
            acc = gltf["accessors"][index]
            view = gltf["bufferViews"][acc["bufferView"]]
            width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[acc["type"]]
            flat = struct.unpack_from(
                "<%df" % (acc["count"] * width), blob, view.get("byteOffset", 0)
            )
            return [flat[i : i + width] for i in range(0, len(flat), width)]

        return gltf, values

    def testGltfExport(self):
        from freecad import animationgltf

        box, cyl, _still = self.gltfScene()
        folder = tempfile.mkdtemp(prefix="AnimGltf")
        name = os.path.join(folder, "anim.gltf")
        self.files += [name, os.path.join(folder, "anim.bin")]
        done = animationgltf.exportGltf(self.anim, name)
        self.assertEqual(done["Frames"], 11)
        self.assertEqual(done["Animated"], 2)
        self.assertEqual(done["Keyed"], 1)
        self.assertEqual(done["Meshes"], 3)
        self.assertEqual(done["Skipped"], [])
        # The design is as it was, the animation stopped
        self.assertFalse(self.anim.Active)
        self.assertAlmostEqual(box.Placement.Base.z, 0.0)
        self.assertTrue(cyl.Visibility)

        gltf, values = self.readGltf(name)
        nodes = {n["name"]: i for i, n in enumerate(gltf["nodes"])}
        self.assertEqual(set(nodes), {self.doc.Name, "Box", "Cyl", "Still"})
        anim = gltf["animations"][0]
        by = {}
        for ch in anim["channels"]:
            sampler = anim["samplers"][ch["sampler"]]
            by[(ch["target"]["node"], ch["target"]["path"])] = sampler
        self.assertEqual(
            set(by),
            {
                (nodes["Box"], "translation"),
                (nodes["Box"], "rotation"),
                (nodes["Cyl"], "scale"),
            },
        )
        # Two Linear keys: the channel is the keys
        move = by[(nodes["Box"], "translation")]
        self.assertEqual(move["interpolation"], "LINEAR")
        times = [v[0] for v in values(move["input"])]
        self.assertEqual(len(times), 2)
        self.assertAlmostEqual(times[0], 0.0)
        self.assertAlmostEqual(times[-1], 1.0, places=5)
        # Metres
        z = [v[2] for v in values(move["output"])]
        self.assertAlmostEqual(z[0], 0.0)
        self.assertAlmostEqual(z[-1], 0.05, places=5)
        # The visibility where it switches
        hide = by[(nodes["Cyl"], "scale")]
        self.assertEqual(hide["interpolation"], "STEP")
        self.assertEqual([round(v[0], 5) for v in values(hide["input"])], [0.0, 0.5])
        shown = [v[0] for v in values(hide["output"])]
        self.assertEqual(shown, [1.0, 0.0])

    def gltfMove(self, name):
        """The Box's translation and rotation samplers of an export, and
        the accessor reader"""
        from freecad import animationgltf

        folder = tempfile.mkdtemp(prefix="AnimGltf")
        path = os.path.join(folder, name + ".gltf")
        self.files += [path, os.path.join(folder, name + ".bin")]
        done = animationgltf.exportGltf(self.anim, path)
        gltf, values = self.readGltf(path)
        box = [i for i, n in enumerate(gltf["nodes"]) if n["name"] == "Box"][0]
        anim = gltf["animations"][0]
        by = {}
        for ch in anim["channels"]:
            if ch["target"]["node"] == box:
                by[ch["target"]["path"]] = anim["samplers"][ch["sampler"]]
        return done, by["translation"], by["rotation"], values

    def boxAt(self, t):
        """The Box's translation (m) and rotation at t, from the document"""
        self.anim.Time = t
        p = self.doc.getObject("Box").Placement
        b, q = p.Base, p.Rotation.Q
        out = [b.x * 1e-3, b.y * 1e-3, b.z * 1e-3], list(q)
        return out

    def testGltfConstantKeysAreAStepOnTheKeys(self):
        self.gltfScene()
        self.anim.setInterp("Box.Placement", 0.0, "Constant")
        done, move, _turn, values = self.gltfMove("step")
        self.assertEqual(done["Keyed"], 1)
        self.assertEqual(move["interpolation"], "STEP")
        self.assertEqual(len(values(move["input"])), 2)

    def testGltfBezierKeysAreACubicSplineThatGivesBackEveryFrame(self):
        self.gltfScene()
        self.anim.addKey(
            "Box.Placement",
            0.5,
            App.Placement(App.Vector(20, 0, 10), App.Rotation(App.Vector(0, 0, 1), 30)),
        )
        for t in (0.0, 0.5):
            self.anim.setInterp("Box.Placement", t, "Bezier")
        done, move, turn, values = self.gltfMove("bezier")
        self.assertEqual(move["interpolation"], "CUBICSPLINE", done)
        self.assertEqual(turn["interpolation"], "CUBICSPLINE")
        keys = [v[0] for v in values(move["input"])]
        self.assertEqual(len(keys), 3)
        out = values(move["output"])
        self.assertEqual(len(out), 9)
        # Between the keys, off the frames too, the Hermite curve glTF draws
        # is where the document puts the box
        for t in (0.13, 0.37, 0.71, 0.94):
            k = 0 if t < keys[1] else 1
            t0, t1 = keys[k], keys[k + 1]
            u, span = (t - t0) / (t1 - t0), t1 - t0
            h = (
                2 * u**3 - 3 * u**2 + 1,
                span * (u**3 - 2 * u**2 + u),
                -2 * u**3 + 3 * u**2,
                span * (u**3 - u**2),
            )
            got = [
                h[0] * out[3 * k + 1][i]
                + h[1] * out[3 * k + 2][i]
                + h[2] * out[3 * k + 4][i]
                + h[3] * out[3 * k + 3][i]
                for i in range(3)
            ]
            want, _q = self.boxAt(t)
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, delta=2e-5, msg="at %s" % t)

    def testGltfTheFirstFrameIsTheAnimationsNotTheRest(self):
        from freecad import animationgltf

        box, _cyl, _still = self.gltfScene()
        # Keyed away from where it rests, the Time already at the start
        self.anim.addKey("Box.Placement", 0.0, App.Placement(App.Vector(0, 0, 20), App.Rotation()))
        self.assertAlmostEqual(self.anim.Time.Value, 0.0)
        self.assertFalse(self.anim.Active)
        folder = tempfile.mkdtemp(prefix="AnimGltf")
        name = os.path.join(folder, "first.gltf")
        self.files += [name, os.path.join(folder, "first.bin")]
        animationgltf.exportGltf(self.anim, name)
        gltf, values = self.readGltf(name)
        node = [n for n in gltf["nodes"] if n["name"] == "Box"][0]
        self.assertAlmostEqual(node["translation"][2], 0.02, places=6)
        anim = gltf["animations"][0]
        index = gltf["nodes"].index(node)
        for ch in anim["channels"]:
            if ch["target"] == {"node": index, "path": "translation"}:
                out = values(anim["samplers"][ch["sampler"]]["output"])
                self.assertAlmostEqual(out[0][2], 0.02, places=6)
        self.assertAlmostEqual(box.Placement.Base.z, 0.0)

    def testGltfALargeBezierTurnIsSampledAndItsMoveKeyed(self):
        # The user manual's lid: open 110 deg by 1.5 s, held, closed at 5 s
        self.gltfScene()
        self.anim.removeKey("Box.Placement", 1.0)
        self.anim.FrameRate = 30
        self.anim.End = 5.0
        closed = App.Placement(App.Vector(0, 40, 30), App.Rotation())
        opened = App.Placement(App.Vector(0, 40, 30), App.Rotation(App.Vector(1, 0, 0), -110))
        self.anim.addKey("Box.Placement", 0.0, closed, interp="Bezier")
        self.anim.addKey("Box.Placement", 1.5, opened, interp="Bezier")
        self.anim.addKey("Box.Placement", 4.0, opened, interp="Bezier")
        self.anim.addKey("Box.Placement", 5.0, closed)
        done, move, turn, values = self.gltfMove("bigturn")
        self.assertEqual(done["Keyed"], 0)
        self.assertEqual(done["PartlyKeyed"], 1)
        self.assertEqual(move["interpolation"], "CUBICSPLINE")
        self.assertEqual(turn["interpolation"], "LINEAR")
        self.assertEqual(len(values(turn["input"])), 151)

    def testGltfEasedKeysAreSampledAtEveryFrame(self):
        self.gltfScene()
        self.anim.setInterp("Box.Placement", 0.0, "Easing", easing=("Bounce", "Out"))
        done, move, _turn, values = self.gltfMove("eased")
        self.assertEqual(done["Keyed"], 0)
        self.assertEqual(move["interpolation"], "LINEAR")
        self.assertEqual(len(values(move["input"])), 11)

    def testGltfAPartTrackBesideTheKeysIsSampled(self):
        self.gltfScene()
        self.anim.addKey("Box.Placement.Base.x", 0.0, 0.0)
        self.anim.addKey("Box.Placement.Base.x", 0.7, 30.0)
        done, move, _turn, values = self.gltfMove("part")
        self.assertEqual(done["Keyed"], 0)
        self.assertEqual(len(values(move["input"])), 11)
        xs = [v[0] for v in values(move["output"])]
        self.assertAlmostEqual(xs[7], 0.03, places=5)

    def testGltfATurnOnLinearKeysIsKeyedWhereItTurnsAsGltfDoes(self):
        self.gltfScene()
        self.anim.addKey(
            "Box.Placement",
            1.0,
            App.Placement(App.Vector(0, 0, 50), App.Rotation(App.Vector(0, 0, 1), 90)),
        )
        done, move, turn, values = self.gltfMove("turn")
        # Whichever it is written as, every frame is where the box is
        times = [v[0] for v in values(turn["input"])]
        quats = values(turn["output"])
        for i in range(11):
            t = i / 10.0
            _m, want = self.boxAt(t)
            k = max(j for j in range(len(times)) if times[j] <= t + 1e-9)
            if k + 1 < len(times):
                u = (t - times[k]) / (times[k + 1] - times[k])
                a, b = quats[k], quats[k + 1]
                import math

                d = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
                ang = math.acos(d)
                if ang > 1e-6:
                    got = [
                        (math.sin((1 - u) * ang) * x + math.sin(u * ang) * y) / math.sin(ang)
                        for x, y in zip(a, b)
                    ]
                else:
                    got = list(a)
            else:
                got = list(quats[k])
            dot = abs(sum(x * y for x, y in zip(got, want)))
            self.assertAlmostEqual(dot, 1.0, places=6, msg="at %s" % t)
        self.assertEqual(done["Keyed"], 1 if len(times) == 2 else 0)

    def testGltfExportSaysWhatItLeavesOut(self):
        from freecad import animationgltf

        box, _cyl, _still = self.gltfScene()
        self.anim.addKey("Box.Height", 0.0, 10)
        self.anim.addKey("Box.Height", 1.0, 30)
        folder = tempfile.mkdtemp(prefix="AnimGltf")
        name = os.path.join(folder, "anim.glb")
        self.files.append(name)
        done = animationgltf.exportGltf(self.anim, name)
        self.assertEqual(len(done["Skipped"]), 1, done["Skipped"])
        self.assertIn("Box", done["Skipped"][0])
        self.assertIn("shape", done["Skipped"][0])
        with open(name, "rb") as f:
            self.assertEqual(f.read(4), b"glTF")
        self.assertAlmostEqual(box.Height.Value, 10.0)

    # -- the frame cache ---------------------------------------------------

    def cacheScene(self, slow=0.0):
        """Obj.G is twice Obj.F, by an execute that counts itself"""
        import time

        class Twice:
            def __init__(self):
                self.count = 0

            def execute(self, obj):
                self.count += 1
                if slow:
                    time.sleep(slow)
                obj.G = obj.F * 2

        self.obj.Proxy = Twice()
        self.doc.recompute()
        self.anim.FrameRate = 10
        self.anim.addKey("Obj.F", 0.0, 0.0)
        self.anim.addKey("Obj.F", 1.0, 10.0)
        return self.obj.Proxy

    def testCacheReplaysWhatAFrameRecomputedTo(self):
        proxy = self.cacheScene()
        self.anim.Cache = True
        self.anim.activate()
        times = self.anim.frames()
        self.assertEqual(len(times), 11)
        for t in times:
            self.anim.Time = t
            self.assertAlmostEqual(self.obj.G, 2 * self.obj.F)
        info = self.anim.cacheInfo()
        self.assertEqual(len(info["Times"]), 11)
        self.assertEqual(info["Stored"], 11)
        self.assertEqual(info["Replayed"], 0)
        self.assertGreater(info["Bytes"], 0)
        executed = proxy.count
        undo = self.doc.UndoCount
        for t in reversed(times):
            self.anim.Time = t
            self.assertAlmostEqual(self.obj.F, 10 * t)
            self.assertAlmostEqual(self.obj.G, 20 * t)
            self.assertNotIn("Touched", self.obj.State)
        info = self.anim.cacheInfo()
        self.assertEqual(info["Replayed"], 10)  # the last was on show already
        self.assertEqual(info["Stored"], 11)
        self.assertEqual(proxy.count, executed)  # nothing was recomputed
        self.assertEqual(self.doc.UndoCount, undo)
        # Stopping is a recompute of the design, as ever
        self.anim.stop()
        self.assertAlmostEqual(self.obj.F, 1.0)
        self.assertAlmostEqual(self.obj.G, 2.0)
        self.assertEqual(len(self.anim.cacheInfo()["Times"]), 11)

    def testCacheKeepsFramesOnly(self):
        proxy = self.cacheScene()
        self.anim.Cache = True
        self.anim.activate()
        self.anim.Time = 0.5
        self.anim.Time = 0.55  # between two frames: recomputed, not kept
        self.assertAlmostEqual(self.obj.G, 11.0)
        self.assertEqual(self.anim.cacheInfo()["Times"], [0.0, 0.5])
        self.anim.Time = 0.5  # put back
        executed = proxy.count
        self.anim.Time = 0.55  # recomputed again
        self.assertGreater(proxy.count, executed)
        self.assertEqual(self.anim.cacheInfo()["Replayed"], 1)

    def testCacheOffKeepsNothing(self):
        proxy = self.cacheScene()
        for t in self.anim.frames():
            self.anim.Time = t
        executed = proxy.count
        self.anim.Time = 0.5
        self.assertGreater(proxy.count, executed)
        self.assertEqual(self.anim.cacheInfo()["Times"], [])
        self.anim.Cache = True
        self.anim.activate()
        self.anim.Recompute = "Never"  # nothing is recomputed: nothing to keep
        self.anim.Time = 0.4
        self.assertEqual(self.anim.cacheInfo()["Times"], [])

    def testCacheDroppedByAnEditOfTheDesign(self):
        proxy = self.cacheScene()
        self.anim.Cache = True
        self.anim.activate()
        for t in self.anim.frames():
            self.anim.Time = t
        self.assertEqual(len(self.anim.cacheInfo()["Times"]), 11)
        self.obj.L = 5.0  # an edit: nothing kept is known to stand
        self.assertEqual(self.anim.cacheInfo()["Times"], [])
        executed = proxy.count
        self.anim.Time = 0.3
        self.assertGreater(proxy.count, executed)
        self.assertEqual(self.anim.cacheInfo()["Times"], [0.3])
        # A key moved is another animation
        self.anim.addKey("Obj.F", 1.0, 20.0)
        self.assertEqual(len(self.anim.cacheInfo()["Times"]), 1)  # evaluated again, at 0.3
        self.assertAlmostEqual(self.obj.G, 12.0)
        self.anim.Time = 0.3
        self.assertAlmostEqual(self.obj.G, 12.0)
        self.anim.clearCache()
        self.assertEqual(self.anim.cacheInfo()["Times"], [])
        # A new object, an undo
        self.anim.Time = 0.3
        self.doc.addObject("App::FeaturePython", "Other")
        self.assertEqual(self.anim.cacheInfo()["Times"], [])

    def testCacheFrameReachedFromElsewhere(self):
        # A frame kept when only one object changed on the way to it is not
        # the whole answer when two differ: it is recomputed then
        proxy = self.cacheScene()

        class Thrice:
            def execute(self, obj):
                obj.G = obj.F * 3

        other = self.doc.addObject("App::FeaturePython", "Other")
        other.addProperty("App::PropertyFloat", "F")
        other.addProperty("App::PropertyFloat", "G")
        other.Proxy = Thrice()
        self.doc.recompute()
        self.anim.addKey("Other.F", 0.0, 0.0)
        self.anim.addKey("Other.F", 0.5, 5.0)  # and held from there
        self.anim.Cache = True
        self.anim.activate()
        for t in self.anim.frames():
            self.anim.Time = t
        for t in (0.1, 0.9, 0.2, 1.0, 0.0, 0.7, 0.4, 0.8, 0.3):
            self.anim.Time = t
            self.assertAlmostEqual(self.obj.G, 20 * t)
            self.assertAlmostEqual(other.G, 3 * min(5.0, 10 * t))
        self.assertGreater(self.anim.cacheInfo()["Replayed"], 0)
        self.assertGreater(proxy.count, 0)

    def testCacheKeepsAShapeWithItsMesh(self):
        try:
            import Part  # noqa: F401
        except ImportError:
            self.skipTest("shapes are Part's")
        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = cyl
        self.doc.recompute()
        rest = cut.Shape.Volume
        self.anim.FrameRate = 5
        self.anim.addKey("Box.Height", 0.0, 10.0)
        self.anim.addKey("Box.Height", 1.0, 20.0)
        self.anim.Cache = True
        self.anim.activate()
        kept = []
        for t in self.anim.frames():
            self.anim.Time = t
            cut.Shape.tessellate(0.1)  # as a view would
            kept.append((cut.Shape, cut.Shape.Volume))
        self.assertGreater(kept[-1][1], kept[0][1] * 1.5)
        for k, ((shape, volume), t) in enumerate(zip(kept, self.anim.frames())):
            self.anim.Time = t
            self.assertAlmostEqual(cut.Shape.Volume, volume, 6)
            self.assertAlmostEqual(box.Height.Value, 10 + 10 * t)
            # The very shape, so the mesh it was shown with comes with it.
            # Not the first: nothing was recomputed for it when the design
            # was already there, so coming back to it is its first recompute
            if k:
                self.assertTrue(cut.Shape.isPartner(shape))
        self.assertEqual(self.anim.cacheInfo()["Replayed"], len(kept) - 1)
        self.anim.Time = 0.0
        first = cut.Shape
        self.anim.Time = self.anim.frames()[-1]
        self.anim.Time = 0.0
        self.assertTrue(cut.Shape.isPartner(first))
        self.anim.stop()
        self.assertAlmostEqual(cut.Shape.Volume, rest, 6)

    def cacheLimits(self, memory=None, disk=None, always=False):
        group = App.ParamGet("User parameter:BaseApp/Preferences/Animation")
        if always:
            group.SetBool("CacheDiskAlways", True)
        else:
            group.RemBool("CacheDiskAlways")
        # By each property's own count while a test sets limits in bytes:
        # the heap's growth is the truth, and not the same twice
        if memory is None:
            group.RemBool("CacheMeasure")
        else:
            group.SetBool("CacheMeasure", False)
        for name, value in (("CacheLimitBytes", memory), ("CacheDiskLimitBytes", disk)):
            if value is None:
                group.RemInt(name)
            else:
                group.SetInt(name, value)

    def testCacheGoesToDiskOverItsMemory(self):
        # Slow enough to recompute that a frame is worth a file: one that
        # is quicker made again than read back is dropped instead
        proxy = self.cacheScene(slow=0.01)
        self.addCleanup(self.cacheLimits)
        self.cacheLimits(memory=1 << 30)
        self.anim.Cache = True
        self.anim.activate()
        one = self.anim.cacheInfo()["Bytes"]
        self.assertGreater(one, 0)
        self.cacheLimits(memory=3 * one + one // 2)  # three frames' worth
        times = self.anim.frames()
        for t in times:
            self.anim.Time = t
        info = self.anim.cacheInfo()
        self.assertEqual(len(info["Times"]), 11)  # none lost
        self.assertEqual(len(info["OnDisk"]), 8)
        self.assertLessEqual(info["Bytes"], 3 * one + one // 2)
        self.assertGreater(info["DiskBytes"], 0)
        self.assertEqual(info["Spilled"], 8)
        self.assertEqual(info["OnDisk"], times[:8])  # shown longest ago
        files = os.path.join(self.doc.TransientDir, "AnimationCache", "Anim")
        self.assertEqual(len(os.listdir(files)), 8)
        executed = proxy.count
        for t in times:
            self.anim.Time = t
            self.assertAlmostEqual(self.obj.F, 10 * t)
            self.assertAlmostEqual(self.obj.G, 20 * t)
            self.assertNotIn("Touched", self.obj.State)
        info = self.anim.cacheInfo()
        self.assertEqual(proxy.count, executed)  # read back, not recomputed
        self.assertGreaterEqual(info["Loaded"], 8)
        self.assertEqual(len(info["Times"]), 11)
        self.assertLessEqual(info["Bytes"], 3 * one + one // 2)
        # The disk has a budget as well: over it the oldest are dropped
        self.cacheLimits(memory=3 * one + one // 2, disk=info["DiskBytes"] // 3)
        self.anim.Time = times[5]
        info = self.anim.cacheInfo()
        self.assertLess(len(info["Times"]), 11)
        self.assertLessEqual(info["DiskBytes"], info["DiskBytes"] // 3 * 3 + 1)
        for t in times:
            self.anim.Time = t
            self.assertAlmostEqual(self.obj.G, 20 * t)
        # No disk at all: what does not fit is dropped
        self.anim.clearCache()
        self.assertEqual(os.listdir(files), [])
        self.cacheLimits(memory=3 * one + one // 2, disk=0)
        for t in times:
            self.anim.Time = t
        info = self.anim.cacheInfo()
        self.assertEqual(len(info["Times"]), 3)
        self.assertEqual(info["OnDisk"], [])
        self.assertEqual(os.listdir(files), [])
        # An edit takes the files with the frames
        self.cacheLimits(memory=3 * one + one // 2)
        for t in times:
            self.anim.Time = t
        self.assertGreater(len(os.listdir(files)), 0)
        self.obj.L = 6.0
        self.assertEqual(os.listdir(files), [])
        self.assertEqual(self.anim.cacheInfo()["DiskBytes"], 0)

    def testCacheShapeBackFromDisk(self):
        try:
            import Part  # noqa: F401
        except ImportError:
            self.skipTest("shapes are Part's")
        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = cyl
        self.doc.recompute()
        rest = cut.Shape.Volume
        self.anim.FrameRate = 5
        self.anim.addKey("Box.Height", 0.0, 10.0)
        self.anim.addKey("Box.Height", 1.0, 20.0)
        self.addCleanup(self.cacheLimits)
        # The frame on show, and no more; to disk however quick the cut is
        self.cacheLimits(memory=1, always=True)
        self.anim.Cache = True
        self.anim.activate()
        kept = []
        for t in self.anim.frames():
            self.anim.Time = t
            kept.append((cut.Shape.Volume, len(cut.Shape.Faces),
                         cut.Shape.getElementName("Face1")))
        info = self.anim.cacheInfo()
        self.assertEqual(len(info["OnDisk"]), len(kept) - 1)
        for (volume, faces, name), t in zip(kept, self.anim.frames()):
            self.anim.Time = t
            self.assertAlmostEqual(cut.Shape.Volume, volume, 6)
            self.assertEqual(len(cut.Shape.Faces), faces)
            self.assertEqual(cut.Shape.getElementName("Face1"), name)
            self.assertTrue(cut.Shape.isValid())
            self.assertAlmostEqual(box.Height.Value, 10 + 10 * t)
            self.assertNotIn("Touched", cut.State)
        info = self.anim.cacheInfo()
        self.assertEqual(info["Stored"], len(kept) + 1)  # the first, come back to
        self.assertGreaterEqual(info["Loaded"], len(kept) - 2)
        self.anim.stop()
        self.assertAlmostEqual(cut.Shape.Volume, rest, 6)

    def testCacheCountsWhatAFrameReallyHolds(self):
        try:
            import Part  # noqa: F401
        except ImportError:
            self.skipTest("shapes are Part's")
        box = self.doc.addObject("Part::Box", "Box")
        tools = []
        for i in range(9):
            cyl = self.doc.addObject("Part::Cylinder", "Cyl")
            cyl.Radius = 0.4
            cyl.Placement.Base = App.Vector(1 + 3 * (i % 3), 1 + 3 * (i // 3), -1)
            tools.append(cyl)
        tool = self.doc.addObject("Part::Compound", "Tool")
        tool.Links = tools
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = tool
        self.doc.recompute()
        self.anim.FrameRate = 10
        self.anim.addKey("Box.Height", 0.0, 10.0)
        self.anim.addKey("Box.Height", 1.0, 20.0)
        own = cut.Shape.MemSize + box.Shape.MemSize  # what the properties say
        self.anim.Cache = True
        self.anim.activate()
        for t in self.anim.frames():
            self.anim.Time = t
        info = self.anim.cacheInfo()
        if info["Bytes"] <= 10 * own:
            self.skipTest("the heap is not measured on this platform")
        # A shape is its curves, its pcurves and its names as well: several
        # times what its property counts, and the cache counts that
        self.assertGreater(info["Bytes"], 10 * 5 * own)
        self.assertLess(info["Bytes"], 10 * 2000 * own)

    def testVideoFormats(self):
        from freecad import videoexport

        known = videoexport.formats()
        self.assertEqual(known[-1], ("PNG image sequence", ".png"))
        self.assertEqual(videoexport.frameName("/tmp/out.png", 7), "/tmp/out_00007.png")
        with self.assertRaises(ValueError):
            videoexport.encode("/tmp/out.xyz", ["a.png"], 10)
        with self.assertRaises(ValueError):
            videoexport.encode("/tmp/out.gif", [], 10)

    def testErrors(self):
        with self.assertRaises(Exception):
            self.anim.addKey("Nothing.F", 0.0)
        with self.assertRaises(Exception):
            self.anim.addKey("Obj.F", 0.0, interp="Wobbly")
        with self.assertRaises(Exception):
            self.anim.addKey("Anim.Speed", 0.0)
        with self.assertRaises(Exception):
            self.anim.removeKey("Obj.F", 0.0)
        with self.assertRaises(TypeError):
            self.anim.Tracks = []


if __name__ == "__main__":
    unittest.main()
