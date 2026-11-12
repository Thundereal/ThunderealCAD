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
