# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# **************************************************************************/

"""The exploded view in C++ (Assembly::ExplodedView): where its steps put
the parts at a progress through them, and the trail each part leaves
(docs/AssemblyObjectModel.md sec 7.2).
"""

import math
import unittest
import os
import tempfile

import FreeCAD as App

import CommandCreateView
import UtilsAssembly

from AssemblyTests.TestJoints import JointTestBase, Z, plc

TOL = 1e-9


def saveInGui(doc):
    """Saved as the command saves: an App save leaves the Gui document's
    Modified flag alone"""
    import FreeCADGui as Gui

    doc.saveAs(os.path.join(tempfile.mkdtemp(), doc.Name + ".FCStd"))
    Gui.setActiveDocument(doc.Name)
    Gui.runCommand("Std_Save")


class PlacementAsserts:
    def assertPlc(self, a, b, msg=None):
        self.assertTrue(a.isSame(b, TOL), f"{msg or ''}: {a} != {b}")


def turn(center, angle):
    """A turn of `angle` degrees about Z through `center`, as the panel's
    dragger makes it"""
    c = App.Vector(*center)
    return App.Placement(c, App.Rotation(Z, angle)) * App.Placement(-c, App.Rotation())


class TestExplodedView(PlacementAsserts, JointTestBase):
    # ------------------------------------------------------------ helpers

    def explodedView(self, steps):
        """An exploded view and its steps, as the panel makes them"""
        group = UtilsAssembly.getViewGroup(self.assembly)
        view = group.newObject("Assembly::ExplodedViewPython", "Exploded View")
        CommandCreateView.ExplodedView(view)
        for subs, move, typeIndex in steps:
            step = self.assembly.newObject("Assembly::ExplodedViewStepPython", "Move")
            CommandCreateView.ExplodedViewStep(step, typeIndex)
            step.MovementTransform = move
            step.References = [self.assembly, subs]
            view.Group = view.Group + [step]
        self.doc.recompute()
        return view

    def placements(self, view, progress=None):
        moves, _trails = view.explodedAt() if progress is None else view.explodedAt(progress)
        return {o.Name: p for o, p in moves}

    def trails(self, view, progress=None):
        _moves, trails = view.explodedAt() if progress is None else view.explodedAt(progress)
        return trails

    # ------------------------------------------------------------ the end state

    def testTranslation(self):
        b = self.box("B", plc(30, 0, 0))
        view = self.explodedView([(["B."], plc(0, 0, 20), 0)])
        self.assertPlc(self.placements(view)["B"], plc(30, 0, 20))
        # Asking writes nothing
        self.assertPlc(b.Placement, plc(30, 0, 0))
        ((step, start, end),) = self.trails(view)
        self.assertEqual(step, 0)
        self.assertTrue(start.isEqual(App.Vector(35, 5, 5), TOL), start)
        self.assertTrue(end.isEqual(App.Vector(35, 5, 25), TOL), end)

    def testTurnAboutACenter(self):
        self.box("B", plc(30, 0, 0))
        view = self.explodedView([(["B."], turn((30, 0, 0), 90), 0)])
        self.assertPlc(self.placements(view)["B"], plc(30, 0, 0, Z, 90))
        # Halfway is half the turn about the same center, not a chord
        self.assertPlc(self.placements(view, 0.5)["B"], plc(30, 0, 0, Z, 45))

    def testTurnTrailFollowsTheArc(self):
        """A turn's trail is its arc, sampled, not the chord between its ends"""
        self.box("B", plc(30, 0, 0))
        view = self.explodedView([(["B."], turn((30, 0, 0), 90), 0)])
        ((step, start, end, points),) = view.explodedAt(None, True)[1]
        self.assertTrue(start.isEqual(App.Vector(35, 5, 5), TOL), start)
        self.assertTrue(end.isEqual(App.Vector(25, 5, 5), TOL), end)
        self.assertGreaterEqual(len(points), 10)
        self.assertTrue(points[0].isEqual(start, TOL) and points[-1].isEqual(end, TOL))
        # Every point at the same distance from the axis, the arc's radius
        for p in points:
            r = math.hypot(p.x - 30, p.y)
            self.assertAlmostEqual(r, math.sqrt(50), 9)
            self.assertAlmostEqual(p.z, 5, 9)
        # Halfway through the move, the arc only as far as it went
        ((_s, _a, half, points),) = view.explodedAt(0.5, True)[1]
        self.assertTrue(points[-1].isEqual(half, TOL))
        self.assertAlmostEqual(math.degrees(math.atan2(half.y, half.x - 30)), 90, 9)
        # The step draws it as segments
        view.pose()
        trail = view.Group[0].Trail
        n = len(view.explodedAt(None, True)[1][0][3])
        self.assertEqual(len(trail), 2 * (n - 1))
        view.unpose()

    def testStepDrawsAnArcAsOneLine(self):
        """The 3D view draws the segments that meet as one line, so a turn's
        arc is dashed end to end"""
        self.box("B", plc(30, 0, 0))
        self.box("C", plc(0, 30, 0))
        view = self.explodedView([(["B."], turn((30, 0, 0), 90), 0), (["C."], plc(0, 0, 20), 0)])
        view.pose()
        points = list(view.Group[0].Trail)
        lines = CommandCreateView.trailPolylines(list(zip(points[0::2], points[1::2])))
        self.assertEqual(len(lines), 1)
        self.assertEqual(len(lines[0]), len(points) // 2 + 1)
        points = list(view.Group[1].Trail)
        self.assertEqual(
            len(CommandCreateView.trailPolylines(list(zip(points[0::2], points[1::2])))), 1
        )
        view.unpose()

    def testSlideTrailIsAStraightLine(self):
        self.box("B", plc(30, 0, 0))
        view = self.explodedView([(["B."], plc(0, 0, 20), 0)])
        ((_s, start, end, points),) = view.explodedAt(None, True)[1]
        self.assertEqual(len(points), 2)
        view.pose()
        self.assertEqual(len(view.Group[0].Trail), 2)
        view.unpose()
        moves, trails = view.Group[0].explodeFrom(App.Vector(), 100, 1.0, True)
        self.assertEqual(len(trails[0]), 3)

    def testStepsCompose(self):
        self.box("C", plc(0, 30, 0))
        first, second = plc(0, 0, 20), plc(15, 0, 0, Z, 30)
        view = self.explodedView([(["C."], first, 0), (["C."], second, 0)])
        self.assertPlc(self.placements(view)["C"], second * first * plc(0, 30, 0))
        # The second trail starts where the first ended
        (s0, a0, b0), (s1, a1, b1) = self.trails(view)
        self.assertEqual((s0, s1), (0, 1))
        self.assertTrue(a1.isEqual(b0, TOL), (a1, b0))
        self.assertTrue(b0.isEqual(App.Vector(5, 35, 25), TOL), b0)
        expected = (second * first * plc(0, 30, 0)).multVec(App.Vector(5, 5, 5))
        self.assertTrue(b1.isEqual(expected, TOL), (b1, expected))

    def testOneStepMovesSeveralParts(self):
        self.box("B", plc(30, 0, 0))
        self.box("C", plc(0, 30, 0))
        view = self.explodedView([(["B.", "C."], plc(0, 0, 20), 0)])
        got = self.placements(view)
        self.assertPlc(got["B"], plc(30, 0, 20))
        self.assertPlc(got["C"], plc(0, 30, 20))
        self.assertEqual([s for s, _a, _b in self.trails(view)], [0, 0])

    def testMatchesUpstreamOnNormalMoves(self):
        """The calculation upstream's Python made, for the moves it made the
        same way (a radial one read the view's bounding box, which a console
        has not)"""
        self.mechanismOf3()
        view = self.explodedView(
            [
                (["B.", "C."], plc(0, 0, 20), 0),
                (["C."], plc(15, 0, 0, Z, 30), 0),
            ]
        )
        got = self.placements(view)
        p = {"B": self.doc.B.Placement, "C": self.doc.C.Placement}
        p["B"] = plc(0, 0, 20) * p["B"]
        p["C"] = plc(15, 0, 0, Z, 30) * plc(0, 0, 20) * p["C"]
        self.assertEqual(set(got), {"B", "C"})
        for name in p:
            self.assertPlc(got[name], p[name], name)

    def mechanismOf3(self):
        a = self.box("A")
        self.box("B", plc(30, 0, 0))
        self.box("C", plc(0, 30, 0))
        self.ground(a)
        self.doc.recompute()

    # ------------------------------------------------------------ radial

    def testRadial(self):
        self.box("A")
        self.box("B", plc(30, 0, 0))
        view = self.explodedView([(["B."], plc(10, 0, 0), 1)])
        center, size = view.getCenterAndSize()
        # The parts' bounding box, [0, 40] x [0, 10] x [0, 10]
        self.assertTrue(center.isEqual(App.Vector(20, 5, 5), TOL), center)
        self.assertAlmostEqual(size, math.sqrt(40**2 + 10**2 + 10**2))
        # Away from the center by four times the move's length over the size
        factor = 4 * 10 / size
        expected = plc(30 + 15 * factor, 0, 0)
        self.assertPlc(self.placements(view)["B"], expected)
        self.assertPlc(self.placements(view, 0.5)["B"], plc(30 + 7.5 * factor, 0, 0))

    # ------------------------------------------------------------ progress

    def testProgress(self):
        self.box("B", plc(30, 0, 0))
        view = self.explodedView(
            [
                (["B."], plc(0, 0, 20), 0),
                (["B."], plc(20, 0, 0), 0),
            ]
        )
        # At the start nothing has moved
        self.assertEqual(view.explodedAt(0), ([], []))
        self.assertPlc(self.placements(view, 0.5)["B"], plc(30, 0, 10))
        self.assertPlc(self.placements(view, 1)["B"], plc(30, 0, 20))
        self.assertPlc(self.placements(view, 1.5)["B"], plc(40, 0, 20))
        self.assertPlc(self.placements(view, 2)["B"], plc(50, 0, 20))
        # Past the end is the end; before the start is the start
        self.assertPlc(self.placements(view, 7)["B"], plc(50, 0, 20))
        self.assertEqual(view.explodedAt(-0.5), ([], []))
        self.assertPlc(self.placements(view)["B"], plc(50, 0, 20))
        # A trail is drawn as far as the part has gone
        trails = self.trails(view, 1.5)
        self.assertEqual([s for s, _a, _b in trails], [0, 1])
        self.assertTrue(trails[1][2].isEqual(App.Vector(45, 5, 25), TOL), trails[1][2])

    # ------------------------------------------------------------ frames

    def testAssemblyMoved(self):
        """A part's Placement is in its parent's frame; a trail is in the
        document's, where the panel draws it"""
        self.box("B", plc(30, 0, 0))
        self.assembly.Placement = plc(0, 100, 0, Z, 90)
        view = self.explodedView([(["B."], plc(0, 0, 20), 0)])
        self.assertPlc(self.placements(view)["B"], plc(30, 0, 20))
        ((_s, start, end),) = self.trails(view)
        g = self.assembly.Placement
        self.assertTrue(start.isEqual(g.multVec(App.Vector(35, 5, 5)), TOL), start)
        self.assertTrue(end.isEqual(g.multVec(App.Vector(35, 5, 25)), TOL), end)

    def testPartInAPart(self):
        part = self.assembly.newObject("App::Part", "Part")
        part.Placement = plc(0, 0, 50)
        b = part.newObject("Part::Box", "B")
        b.Placement = plc(30, 0, 0)
        view = self.explodedView([(["Part.B."], plc(0, 20, 0), 0)])
        self.assertPlc(self.placements(view)["B"], plc(30, 20, 0))
        ((_s, start, end),) = self.trails(view)
        self.assertTrue(start.isEqual(App.Vector(35, 5, 55), TOL), start)
        self.assertTrue(end.isEqual(App.Vector(35, 25, 55), TOL), end)

    # ------------------------------------------------------------ what is skipped

    def testMissingPartIsSkipped(self):
        self.box("B", plc(30, 0, 0))
        view = self.explodedView([(["B.", "Gone."], plc(0, 0, 20), 0)])
        self.assertEqual(set(self.placements(view)), {"B"})
        self.assertEqual(len(self.trails(view)), 1)

    def testEmptyView(self):
        self.box("B", plc(30, 0, 0))
        view = self.explodedView([])
        self.assertEqual(view.explodedAt(), ([], []))

    # ------------------------------------------------------------ the panel's path

    def testPanelAppliesTheSameNumbers(self):
        """The panel's applyMoves and applyStep write what explodedAt says"""
        b = self.box("B", plc(30, 0, 0))
        view = self.explodedView(
            [
                (["B."], plc(0, 0, 20), 0),
                (["B."], turn((35, 5, 25), 90), 0),
            ]
        )
        expected = self.placements(view)["B"]
        trails = self.trails(view)
        positions = view.Proxy.applyMoves(view)
        self.assertPlc(b.Placement, expected)
        self.assertEqual(len(positions), 2)
        for (a, e), (_s, ta, te) in zip(positions, trails):
            self.assertTrue(a.isEqual(ta, TOL) and e.isEqual(te, TOL), (a, e, ta, te))
        # Each step keeps its trail for its view provider to draw
        first, second = view.Group
        self.assertEqual(len(first.Trail), 2)
        self.assertTrue(first.Trail[1].isEqual(trails[0][2], TOL))
        # One step on where the parts are now
        b.Placement = plc(30, 0, 0)
        first.Proxy.applyStep(first)
        self.assertPlc(b.Placement, plc(30, 0, 20))

    def testStepExplodeFrom(self):
        b = self.box("B", plc(30, 0, 0))
        view = self.explodedView([(["B."], plc(0, 0, 20), 0)])
        center, size = view.getCenterAndSize()
        step = view.Group[0]
        moves, trails = step.explodeFrom(center, size)
        self.assertEqual([o.Name for o, _p in moves], ["B"])
        self.assertPlc(moves[0][1], plc(30, 0, 20))
        self.assertEqual(len(trails), 1)
        moves, _trails = step.explodeFrom(center, size, 0.25)
        self.assertPlc(moves[0][1], plc(30, 0, 5))
        self.assertPlc(b.Placement, plc(30, 0, 0))


class TestExplodedViewShown(PlacementAsserts, JointTestBase):
    """Showing an exploded view is not an edit (docs/AssemblyExplode.md
    sec 3): no undo step, nothing touched, the file as it was"""

    def setUp(self):
        super().setUp()
        self.doc.UndoMode = 1

    def explodedView(self, steps):
        return TestExplodedView.explodedView(self, steps)

    def touched(self):
        return [o.Name for o in self.doc.Objects if "Touched" in o.State]

    def assertNotAnEdit(self, undo):
        self.assertEqual(self.doc.UndoCount, undo, "an undo step")
        self.assertFalse(self.doc.HasPendingTransaction, "a transaction open")
        self.assertEqual(self.touched(), [], "touched")
        if App.GuiUp:
            import FreeCADGui as Gui

            self.assertFalse(Gui.getDocument(self.doc.Name).Modified, "modified")

    def settle(self):
        """A recomputed document with nothing pending: what a user has
        before selecting the view"""
        self.doc.recompute()
        if self.doc.HasPendingTransaction:
            self.doc.commitTransaction()
        if App.GuiUp:
            saveInGui(self.doc)
        return self.doc.UndoCount

    def twoSteps(self):
        b = self.box("B", plc(30, 0, 0))
        view = self.explodedView(
            [
                (["B."], plc(0, 0, 20), 0),
                (["B."], plc(20, 0, 0), 0),
            ]
        )
        return b, view

    # ------------------------------------------------------------ the tree's explode

    def testSelectionExplodeIsNotAnEdit(self):
        """What the tree does on selecting the view and on leaving it"""
        b, view = self.twoSteps()
        undo = self.settle()
        view.Proxy.explodeTemporarily(view)
        self.assertPlc(b.Placement, plc(50, 0, 20))
        self.assertNotAnEdit(undo)
        view.Proxy.restoreAssembly(view)
        self.assertPlc(b.Placement, plc(30, 0, 0))
        self.assertNotAnEdit(undo)

    # ------------------------------------------------------------ pose

    def testPose(self):
        b, view = self.twoSteps()
        undo = self.settle()
        self.assertIsNone(view.posedAt())
        view.pose(1)
        self.assertEqual(view.posedAt(), 1)
        self.assertPlc(b.Placement, plc(30, 0, 20))
        # Again from rest, not from the pose on show
        view.pose(0.5)
        self.assertPlc(b.Placement, plc(30, 0, 10))
        view.pose()
        self.assertEqual(view.posedAt(), 2)
        self.assertPlc(b.Placement, plc(50, 0, 20))
        view.pose(0)
        self.assertPlc(b.Placement, plc(30, 0, 0))
        self.assertNotAnEdit(undo)
        view.pose(2)
        view.unpose()
        self.assertIsNone(view.posedAt())
        self.assertPlc(b.Placement, plc(30, 0, 0))
        self.assertNotAnEdit(undo)

    def testPoseShowsTheTrails(self):
        b, view = self.twoSteps()
        first, second = view.Group
        first.Visibility = second.Visibility = False
        self.settle()
        view.pose(1.5)
        self.assertEqual(len(first.Trail), 2)
        self.assertEqual(len(second.Trail), 2)
        self.assertTrue(second.Trail[1].isEqual(App.Vector(45, 5, 25), TOL), second.Trail[1])
        self.assertTrue(first.Visibility and second.Visibility)
        view.pose(0.5)
        self.assertEqual(len(second.Trail), 0)
        view.unpose()
        self.assertEqual((len(first.Trail), len(second.Trail)), (0, 0))
        self.assertFalse(first.Visibility or second.Visibility)

    def testExplodedAtWhilePosed(self):
        """The calculation is from rest whatever is on show"""
        _b, view = self.twoSteps()
        before = view.explodedAt(1.5)
        view.pose()
        during = view.explodedAt(1.5)
        view.unpose()
        for (o1, p1), (o2, p2) in zip(before[0], during[0]):
            self.assertEqual(o1, o2)
            self.assertPlc(p1, p2)
        self.assertEqual(len(before[1]), len(during[1]))

    def testPartInAPartPosed(self):
        """A step moving a part and a later one moving a part in it: the
        second is placed through the first's rest frame"""
        part = self.assembly.newObject("App::Part", "Part")
        b = part.newObject("Part::Box", "B")
        b.Placement = plc(30, 0, 0)
        view = self.explodedView(
            [
                (["Part."], plc(0, 0, 50), 0),
                (["Part.B."], plc(0, 20, 0), 0),
            ]
        )
        expected = view.explodedAt()
        view.pose()
        self.assertPlc(part.Placement, plc(0, 0, 50))
        self.assertPlc(b.Placement, plc(30, 20, 0))
        again = view.explodedAt()
        for (_s1, a1, b1), (_s2, a2, b2) in zip(expected[1], again[1]):
            self.assertTrue(a1.isEqual(a2, TOL) and b1.isEqual(b2, TOL), (a1, a2, b1, b2))
        view.unpose()
        self.assertPlc(part.Placement, plc())
        self.assertPlc(b.Placement, plc(30, 0, 0))

    # ------------------------------------------------------------ the design while posed

    def testSaveWhilePosedWritesTheDesign(self):
        b, view = self.twoSteps()
        self.settle()
        view.pose()
        path = os.path.join(tempfile.mkdtemp(), "Posed.FCStd")
        self.doc.saveAs(path)
        # Still on show after the save
        self.assertPlc(b.Placement, plc(50, 0, 20))
        self.assertEqual(view.posedAt(), 2)
        name = self.doc.Name
        App.closeDocument(name)
        self.doc = App.openDocument(path)
        self.assembly = self.doc.getObject("Assembly")
        self.assertPlc(self.doc.getObject("B").Placement, plc(30, 0, 0))

    def testRecomputeWhilePosedSolvesTheDesign(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        self.ground(a)
        self.joint(
            "Revolute",
            (a, ["Face6", "Face6"]),
            plc(5, 5, 10),
            (b, ["Face5", "Face5"]),
            plc(5, 5, 0),
        )
        self.doc.recompute()
        rest = App.Placement(b.Placement)
        view = self.explodedView([(["B."], plc(0, 0, 20), 0)])
        self.settle()
        view.pose()
        posed = App.Placement(b.Placement)
        self.assertFalse(posed.isSame(rest, 1e-6))
        self.assembly.touch()
        self.doc.recompute()
        # The solve ran on the design and the pose is back on top of it
        self.assertPlc(b.Placement, posed)
        view.unpose()
        self.assertPlc(b.Placement, rest)

    def testEditWhilePosedIsTheDesign(self):
        b, view = self.twoSteps()
        self.settle()
        view.pose()
        self.doc.openTransaction("move B")
        b.Placement = plc(0, 0, 100)
        self.doc.commitTransaction()
        view.unpose()
        self.assertPlc(b.Placement, plc(0, 0, 100))
        # And the next pose is from there
        view.pose(1)
        self.assertPlc(b.Placement, plc(0, 0, 120))
        view.unpose()

    def testEditingAStepWhilePosedShowsIt(self):
        """A step's move, parts or the steps themselves changed while the
        view is shown: shown again, from rest"""
        b, view = self.twoSteps()
        c = self.box("C", plc(0, 30, 0))
        self.settle()
        view.pose()
        first, second = view.Group
        first.MovementTransform = plc(0, 0, 30)
        self.assertPlc(b.Placement, plc(50, 0, 30))
        first.References = [self.assembly, ["B.", "C."]]
        self.assertPlc(c.Placement, plc(0, 30, 30))
        view.Group = [second]
        self.assertPlc(b.Placement, plc(50, 0, 0))
        self.assertPlc(c.Placement, plc(0, 30, 0))
        self.assertEqual(view.posedAt(), 2)
        view.unpose()
        self.assertPlc(b.Placement, plc(30, 0, 0))
        self.assertPlc(c.Placement, plc(0, 30, 0))

    def testUndoOfAnEditWhilePosedGoesBackToTheDesign(self):
        """An edit of a part while the view is shown records the design's
        placement as the value before, not the pose (docs/Animation.md
        sec 5.2): its undo puts the design back"""
        b, view = self.twoSteps()
        self.settle()
        view.pose()
        self.doc.openTransaction("move B")
        b.Placement = plc(0, 0, 100)
        self.doc.commitTransaction()
        view.unpose()
        self.assertPlc(b.Placement, plc(0, 0, 100))
        self.doc.undo()
        self.assertPlc(b.Placement, plc(30, 0, 0))
        self.doc.redo()
        self.assertPlc(b.Placement, plc(0, 0, 100))

    def testUndoWhilePosedShowsTheDesignExploded(self):
        """An undo while the view is shown: the design it puts back, shown
        exploded again"""
        b, view = self.twoSteps()
        self.settle()
        self.doc.openTransaction("move B")
        b.Placement = plc(0, 0, 100)
        self.doc.commitTransaction()
        view.pose()
        self.assertPlc(b.Placement, plc(20, 0, 120))
        self.doc.undo()
        self.assertPlc(b.Placement, plc(50, 0, 20))
        view.unpose()
        self.assertPlc(b.Placement, plc(30, 0, 0))

    def testTheLogKeepsTheDesignNotThePoseOfAnEditPosedInItsTransaction(self):
        """An edit, then the view shown, in one transaction: the transaction
        log's row says what was typed, not the pose on show at the commit
        (docs/Animation.md sec 5.2)"""
        b, view = self.twoSteps()
        self.settle()
        self.doc.openTransaction("move B")
        b.Placement = plc(0, 0, 100)
        view.pose()
        self.assertPlc(b.Placement, plc(20, 0, 120))
        self.doc.commitTransaction()
        self.doc.resolveTransactionLog()
        ref = self.doc.getTransactionValueAt(b, "Placement")
        if not ref:
            self.skipTest("the transaction log is off")
        logged = self.doc.getTransactionValue(ref)[0]
        self.assertIn('Px="0', logged)
        self.assertIn('Pz="100', logged)
        # And the pose stays a pose
        self.assertPlc(b.Placement, plc(20, 0, 120))
        view.unpose()
        self.assertPlc(b.Placement, plc(0, 0, 100))

    def testPartHoldingAMovedPartKeepsItsRestBox(self):
        """A Part a step moves, holding a part an earlier step moved: its
        trail starts at its box at rest whatever is on show"""
        part = self.assembly.newObject("App::Part", "Part")
        b = part.newObject("Part::Box", "B")
        b.Placement = plc(30, 0, 0)
        view = self.explodedView(
            [
                (["Part.B."], plc(0, 0, 50), 0),
                (["Part."], plc(0, 40, 0), 0),
            ]
        )
        before = view.explodedAt()[1]
        view.pose()
        during = view.explodedAt()[1]
        view.unpose()
        for (_s1, a1, b1), (_s2, a2, b2) in zip(before, during):
            self.assertTrue(a1.isEqual(a2, TOL) and b1.isEqual(b2, TOL), (a1, a2, b1, b2))

    def testDeletingThePosedViewPutsThePartsBack(self):
        b, view = self.twoSteps()
        self.settle()
        view.pose()
        steps = [s.Name for s in view.Group]
        self.doc.removeObject(view.Name)
        for name in steps:
            self.doc.removeObject(name)
        self.assertPlc(b.Placement, plc(30, 0, 0))


@unittest.skipIf(not App.GuiUp, "the tree's explode needs the GUI")
class TestExplodedViewInTree(PlacementAsserts, JointTestBase):
    """Selecting the view in the tree shows it; leaving it puts the parts
    back (ViewProviderAssembly::explodeTemporarily)"""

    def testSelectAndLeave(self):
        import FreeCADGui as Gui

        b = self.box("B", plc(30, 0, 0))
        view = TestExplodedView.explodedView(self, [(["B."], plc(0, 0, 20), 0)])
        CommandCreateView.ViewProviderExplodedView(view.ViewObject)
        step = view.Group[0]
        CommandCreateView.ViewProviderExplodedViewStep(step.ViewObject)
        self.doc.recompute()
        if self.doc.HasPendingTransaction:
            self.doc.commitTransaction()
        saveInGui(self.doc)
        gdoc = Gui.getDocument(self.doc.Name)
        self.assertFalse(gdoc.Modified)
        undo = self.doc.UndoCount
        # The active assembly is the one watching the selection
        gdoc.setEdit(self.assembly.Name)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(view)
        try:
            self.assertEqual(view.posedAt(), 1)
            self.assertPlc(b.Placement, plc(30, 0, 20))
            # The step's view provider draws its trail
            self.assertEqual(step.ViewObject.Proxy.lineSetGroup.getNumChildren(), 1)
        finally:
            Gui.Selection.clearSelection()
            gdoc.resetEdit()
        self.assertIsNone(view.posedAt())
        self.assertPlc(b.Placement, plc(30, 0, 0))
        self.assertEqual(step.ViewObject.Proxy.lineSetGroup.getNumChildren(), 0)
        self.assertEqual(self.doc.UndoCount, undo, "an undo step")
        self.assertFalse(self.doc.HasPendingTransaction, "a transaction open")
        self.assertFalse(gdoc.Modified, "modified")


@unittest.skipIf(not App.GuiUp, "the context menu needs the GUI")
class TestExplodedViewAnimate(PlacementAsserts, JointTestBase):
    def testAnimate(self):
        """The view's Animate: one undo step making an Animation of it"""
        import FreeCADGui as Gui

        b = self.box("B", plc(30, 0, 0))
        view = TestExplodedView.explodedView(self, [(["B."], plc(0, 0, 20), 0)])
        CommandCreateView.ViewProviderExplodedView(view.ViewObject)
        self.doc.recompute()
        if self.doc.HasPendingTransaction:
            self.doc.commitTransaction()
        Gui.setActiveDocument(self.doc.Name)
        undo = self.doc.UndoCount
        view.ViewObject.Proxy._animate(view)
        try:
            anims = [o for o in self.doc.Objects if o.isDerivedFrom("App::Animation")]
            self.assertEqual(len(anims), 1)
            anim = anims[0]
            self.assertEqual(self.doc.UndoCount, undo + 1)
            self.assertEqual(anim.Label, view.Label + " animation")
            anim.Time = 0.5
            self.assertPlc(b.Placement, plc(30, 0, 10))
            anim.stop()
            self.assertPlc(b.Placement, plc(30, 0, 0))
        finally:
            Gui.ActiveDocument.resetEdit()


class TestExplodeTrack(PlacementAsserts, JointTestBase):
    """An exploded view played by an Animation: a track of kind Explode
    whose one number is the progress through the steps
    (docs/AssemblyExplode.md sec 4)"""

    def setUp(self):
        super().setUp()
        self.doc.UndoMode = 1
        self.b = self.box("B", plc(30, 0, 0))
        self.view = TestExplodedView.explodedView(
            self,
            [
                (["B."], plc(0, 0, 20), 0),
                (["B."], plc(20, 0, 0), 0),
            ],
        )
        self.path = "Explode:" + self.view.Name

    def settle(self):
        self.doc.recompute()
        if self.doc.HasPendingTransaction:
            self.doc.commitTransaction()
        return self.doc.UndoCount

    def assertNotAnEdit(self, undo):
        self.assertEqual(self.doc.UndoCount, undo, "an undo step")
        self.assertFalse(self.doc.HasPendingTransaction, "a transaction open")
        # Nothing the explode moves; an Animation's own Time is its business
        touched = [
            o.Name
            for o in self.doc.Objects
            if "Touched" in o.State and not o.isDerivedFrom("App::Animation")
        ]
        self.assertEqual(touched, [])

    def testPlay(self):
        anim = self.doc.addObject("App::Animation", "Anim")
        anim.addKey(self.path, 0, 0, interp="Linear")
        anim.addKey(self.path, 4, 2, interp="Linear")
        self.assertEqual(anim.state(self.path), "Ok")
        self.assertEqual(anim.track(self.path)["Path"], self.path)
        undo = self.settle()
        anim.Time = 1
        self.assertAlmostEqual(self.view.posedAt(), 0.5)
        self.assertPlc(self.b.Placement, plc(30, 0, 10))
        anim.Time = 3
        self.assertPlc(self.b.Placement, plc(40, 0, 20))
        anim.Time = 4
        self.assertPlc(self.b.Placement, plc(50, 0, 20))
        self.assertNotAnEdit(undo)
        anim.stop()
        self.assertIsNone(self.view.posedAt())
        self.assertPlc(self.b.Placement, plc(30, 0, 0))
        self.assertNotAnEdit(undo)

    def testKeyByLabelAndNow(self):
        anim = self.doc.addObject("App::Animation", "Anim")
        self.view.Label = "Apart"
        anim.addKey("Explode:Apart", 0, [0.0])
        self.assertEqual(anim.track(self.path)["Path"], self.path)
        # The value now: the progress on show
        anim.Time = 2
        self.view.pose(1.5)
        anim.keyNow(self.path)
        self.assertEqual([list(k[1]) for k in anim.keys(self.path)], [[0.0], [1.5]])
        anim.stop()

    def testReverseStripAssembles(self):
        explode = CommandCreateView.makeExplodeAnimation(self.view, 1.0)
        self.assertEqual(explode.End, 0)  # the last key's time
        self.assertEqual(explode.frames()[-1], 2.0)
        show = self.doc.addObject("App::Animation", "Show")
        show.addStrip(explode)
        show.addStrip(explode, reverse=True)
        undo = self.settle()
        show.Time = 1.5
        self.assertPlc(self.b.Placement, plc(40, 0, 20))
        show.Time = 2.5  # assembling: half of step 2 undone
        self.assertPlc(self.b.Placement, plc(40, 0, 20))
        show.Time = 3.5
        self.assertPlc(self.b.Placement, plc(30, 0, 10))
        show.Time = 4
        self.assertPlc(self.b.Placement, plc(30, 0, 0))
        self.assertNotAnEdit(undo)
        show.stop()
        self.assertIsNone(self.view.posedAt())

    def testDeletedViewIsSkipped(self):
        anim = CommandCreateView.makeExplodeAnimation(self.view)
        for step in self.view.Group:
            self.doc.removeObject(step.Name)
        self.doc.removeObject(self.view.Name)
        anim.Time = 1
        anim.stop()
        self.assertPlc(self.b.Placement, plc(30, 0, 0))


def hasEventLoop():
    """A Qt application to run an event loop in: FreeCADCmd has none"""
    from PySide import QtCore

    return QtCore.QCoreApplication.instance() is not None


def waitForWorkers(ms=2000):
    """TechDraw projects in worker threads; give them the event loop"""
    from PySide import QtCore

    loop = QtCore.QEventLoop()
    timer = QtCore.QTimer()
    timer.setSingleShot(True)
    timer.timeout.connect(loop.quit)
    timer.start(ms)
    loop.exec_()


def trailNames(shape):
    return sorted(n for n in shape.ElementMap if n.split(";")[0].startswith("Trail"))


class TestExplodedShape(PlacementAsserts, JointTestBase):
    """The exploded view's shape: the parts where its steps put them, and its
    trails as edges named Trail<n>, one each (docs/AssemblyExplode.md sec 5)"""

    def build(self, steps):
        self.box("A")
        self.b = self.box("B", plc(30, 0, 0))
        return TestExplodedView.explodedView(self, steps)

    def testShapeIsTheExplodedParts(self):
        import Part

        view = self.build([(["B."], plc(0, 0, 20), 0)])
        shape = view.getExplodedShape()
        self.assertEqual(len(shape.Solids), 2)
        zmins = sorted(round(s.BoundBox.ZMin, 9) for s in shape.Solids)
        self.assertEqual(zmins, [0, 20])
        self.assertEqual([n.split(";")[0] for n in trailNames(shape)], ["Trail1"])
        trail = shape.getElement(shape.ElementMap[trailNames(shape)[0]])
        self.assertTrue(trail.Vertexes[0].Point.isEqual(App.Vector(35, 5, 5), TOL))
        self.assertTrue(trail.Vertexes[1].Point.isEqual(App.Vector(35, 5, 25), TOL))
        # As any consumer of shapes sees it, and the design untouched
        same = Part.getShape(view)
        self.assertEqual(len(same.Solids), 2)
        self.assertAlmostEqual(same.BoundBox.ZMax, 30)
        self.assertPlc(self.b.Placement, plc(30, 0, 0))
        # At a progress
        half = view.getExplodedShape(0.5)
        self.assertAlmostEqual(half.BoundBox.ZMax, 20)

    def testTurnTrailIsItsArc(self):
        """A turn's trail is one curved edge along its arc, not pieces: a
        drawing dashes it end to end"""
        view = self.build([(["B."], turn((30, 0, 0), 90), 0)])
        shape = view.getExplodedShape()
        names = trailNames(shape)
        self.assertEqual([n.split(";")[0] for n in names], ["Trail1"])
        edge = shape.getElement(shape.ElementMap[names[0]])
        # The quarter circle of radius sqrt(50), not its chord of 10
        self.assertAlmostEqual(edge.Length, math.sqrt(50) * math.pi / 2, 3)
        mid = edge.valueAt((edge.FirstParameter + edge.LastParameter) / 2)
        self.assertAlmostEqual(math.hypot(mid.x - 30, mid.y), math.sqrt(50), 3)

    def testAssemblyShapeLeavesTheViewOut(self):
        import Part

        view = self.build([(["B."], plc(0, 0, 20), 0)])
        self.assertEqual(len(Part.getShape(self.assembly).Solids), 2)
        center, size = view.getCenterAndSize()
        self.assertTrue(center.isEqual(App.Vector(20, 5, 5), TOL), center)

    def testPosedViewDrawsTheSame(self):
        view = self.build([(["B."], plc(0, 0, 20), 0)])
        before = view.getExplodedShape()
        view.pose(0.5)
        during = view.getExplodedShape()
        view.unpose()
        self.assertTrue(before.BoundBox.isInside(during.BoundBox.Center))
        self.assertAlmostEqual(during.BoundBox.ZMax, before.BoundBox.ZMax)

    def testTechDrawView(self):
        """A TechDraw view of the exploded view: its parts and its trails,
        each projected edge of a trail named after it"""
        try:
            import TechDraw  # noqa: F401
            from TDTest.TechDrawTestUtilities import createPageWithSVGTemplate
        except ImportError:
            self.skipTest("no TechDraw")
        if not hasEventLoop():
            self.skipTest("TechDraw projects in threads, which need an event loop")
        view = self.build([(["B."], plc(0, 0, 20), 0)])
        page = createPageWithSVGTemplate(self.doc)
        drawing = self.doc.addObject("TechDraw::DrawViewPart", "Drawing")
        page.addView(drawing)
        drawing.Source = [view]
        drawing.Direction = App.Vector(0, -1, 0)
        self.doc.recompute()
        waitForWorkers()
        sources = [source.split(";")[0] for _n, source, _i in drawing.getEdgeNames()]
        self.assertIn("Trail1", sources)
        # Both boxes: B drawn where the step put it, 20 up
        shape = drawing.getSourceShape()
        self.assertEqual(len(shape.Solids), 2)
        self.assertAlmostEqual(shape.BoundBox.ZMax, 30)


@unittest.skipIf(not App.GuiUp, "drawing a page needs the GUI")
class TestExplodedViewDrawn(PlacementAsserts, JointTestBase):
    # The steps' line colour for the test: a green nothing else on the page
    # has. Put back after (the GUI tests write the real user.cfg).
    GREEN = 0x00C80000

    def setUp(self):
        super().setUp()
        self.prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self.hadColor = "StepLineColor" in self.prefs.GetUnsigneds()
        self.oldColor = self.prefs.GetUnsigned("StepLineColor", 0xCC333300)
        self.prefs.SetUnsigned("StepLineColor", self.GREEN)

    def tearDown(self):
        if self.hadColor:
            self.prefs.SetUnsigned("StepLineColor", self.oldColor)
        else:
            self.prefs.RemUnsigned("StepLineColor")
        super().tearDown()

    def testTrailsAreDashed(self):
        """On the page, in its SVG and on the vg 2D engine's page, a trail
        is dashed and in the steps' colour: the exploded view's view
        provider styles its trail edges"""
        try:
            import TechDraw  # noqa: F401
            import TechDrawGui
            from TDTest.TechDrawTestUtilities import createPageWithSVGTemplate
        except ImportError:
            self.skipTest("no TechDraw")
        self.box("A")
        self.box("B", plc(30, 0, 0))
        view = TestExplodedView.explodedView(self, [(["B."], plc(0, 0, 20), 0)])
        CommandCreateView.ViewProviderExplodedView(view.ViewObject)
        self.assertEqual(view.ViewObject.TypeId, "AssemblyGui::ViewProviderExplodedViewPython")
        page = createPageWithSVGTemplate(self.doc)
        drawing = self.doc.addObject("TechDraw::DrawViewPart", "Drawing")
        page.addView(drawing)
        drawing.Source = [view]
        drawing.Direction = App.Vector(0, -1, 0)
        self.doc.recompute()
        waitForWorkers()
        page.ViewObject.show()
        waitForWorkers(1000)
        trail = self.greenStrokes(TechDrawGui, page)
        greenPixels = self.greenPixels(TechDrawGui, page)
        self.assertTrue(trail, "no stroke in the steps' colour")
        for g in trail:
            self.assertIn("stroke-dasharray", g)
            self.assertIn('stroke-opacity="1"', g)
        if greenPixels is not None:
            self.assertGreater(greenPixels, 20, "the vg page draws no trail")
        # Without the trail, none
        view.Group[0].MovementTransform = plc()
        self.doc.recompute()
        drawing.touch()
        self.doc.recompute()
        waitForWorkers()
        self.assertEqual(self.greenStrokes(TechDrawGui, page), [])
        if greenPixels is not None:
            self.assertEqual(self.greenPixels(TechDrawGui, page), 0)

    def greenStrokes(self, TechDrawGui, page):
        """The groups of the page's SVG that stroke in the steps' green"""
        import re

        path = os.path.join(tempfile.mkdtemp(), "exploded.svg")
        TechDrawGui.exportPageAsSvg(page, path)
        svg = open(path, encoding="utf-8").read()
        return [g for g in re.findall(r"<g [^>]*>", svg) if 'stroke="#00c800"' in g]

    def greenPixels(self, TechDrawGui, page):
        """The pixels of the steps' green on the page the vg 2D engine draws,
        or None where it cannot draw"""
        from PIL import Image

        path = os.path.join(tempfile.mkdtemp(), "exploded.png")
        try:
            TechDrawGui.renderPageVg(page, path, 1400, 1000)
        except Exception:
            return None
        img = Image.open(path).convert("RGB")
        return sum(1 for r, g, b in img.getdata() if g > 120 and r < 90 and b < 90)


class TestExplodedViewAtSteps(PlacementAsserts, JointTestBase):
    """An exploded view at a progress, and a drawing of each step
    (docs/AssemblyExplode.md sec 8)"""

    def twoSteps(self):
        self.box("A")
        self.b = self.box("B", plc(30, 0, 0))
        view = TestExplodedView.explodedView(
            self,
            [
                (["B."], plc(0, 0, 20), 0),
                (["B."], plc(20, 0, 0), 0),
            ],
        )
        view.Group[0].Label = "Lift"
        view.Group[1].Label = "Slide"
        return view

    def testShapeAtAProgress(self):
        import Part

        view = self.twoSteps()
        at = self.doc.addObject("Assembly::ExplodedViewAt", "At")
        at.View = view
        at.Progress = 1
        self.assertAlmostEqual(Part.getShape(at).BoundBox.ZMax, 30)
        self.assertAlmostEqual(Part.getShape(at).BoundBox.XMax, 40)
        at.Progress = -1
        self.assertAlmostEqual(Part.getShape(at).BoundBox.XMax, 60)
        # A step edited: drawn again at the recompute
        view.Group[1].MovementTransform = plc(40, 0, 0)
        self.doc.recompute()
        self.assertAlmostEqual(Part.getShape(at).BoundBox.XMax, 80)
        # Recomputed without an error: outside the assembly, its link to the
        # view inside it is allowed
        self.assertNotIn("Invalid", at.State)
        # Nothing written
        self.assertPlc(self.b.Placement, plc(30, 0, 0))

    def testStepDrawings(self):
        """A page with a view of each step, captioned, at one scale; GUI
        only, as TechDraw projects in threads that need an event loop"""
        if not hasEventLoop():
            self.skipTest("TechDraw projects in threads, which need an event loop")
        try:
            import TechDraw  # noqa: F401
        except ImportError:
            self.skipTest("no TechDraw")
        view = self.twoSteps()
        page = CommandCreateView.makeStepDrawings(view)
        waitForWorkers(3000)
        parts = [v for v in page.Views if v.isDerivedFrom("TechDraw::DrawViewPart")]
        captions = [v for v in page.Views if v.isDerivedFrom("TechDraw::DrawViewAnnotation")]
        self.assertEqual(len(parts), 2)
        self.assertEqual([c.Text for c in captions], [["1. Lift"], ["2. Slide"]])
        self.assertEqual([v.Source[0].Progress for v in parts], [1, 2])
        self.assertEqual(len({v.Scale for v in parts}), 1)
        # Step 1 draws one trail, step 2 two
        trails = [
            len({s.split(";")[0] for _n, s, _i in v.getEdgeNames() if s.startswith("Trail")})
            for v in parts
        ]
        self.assertEqual(trails, [1, 2])
        # Inside the page
        for v in parts:
            self.assertTrue(0 < v.X < page.PageWidth and 0 < v.Y < page.PageHeight)
        # A step edited: its views project it again
        before = max(e.BoundBox.XMax for e in parts[1].getVisibleEdges())
        view.Group[1].MovementTransform = plc(60, 0, 0)
        self.doc.recompute()
        waitForWorkers(3000)
        after = max(e.BoundBox.XMax for e in parts[1].getVisibleEdges())
        self.assertGreater(after, before + 1)


class TestExplodedViewRedrawn(PlacementAsserts, JointTestBase):
    """A drawing of an exploded view, or of a step of one, is drawn again
    when a part moves that no step moves: by hand or by the solve
    (docs/AssemblyExplode.md sec 8)"""

    def drawn(self, source):
        if not hasEventLoop():
            self.skipTest("TechDraw projects in threads, which need an event loop")
        try:
            import TechDraw  # noqa: F401
            from TDTest.TechDrawTestUtilities import createPageWithSVGTemplate
        except ImportError:
            self.skipTest("no TechDraw")
        page = getattr(self, "page", None) or createPageWithSVGTemplate(self.doc)
        self.page = page
        drawing = self.doc.addObject("TechDraw::DrawViewPart", "Drawing")
        page.addView(drawing)
        drawing.Source = [source]
        drawing.Direction = App.Vector(0, -1, 0)
        return drawing

    def height(self, drawing):
        import Part

        waitForWorkers()
        return Part.Compound(drawing.getVisibleEdges()).BoundBox.YLength

    def views(self):
        self.box("B", plc(30, 0, 0))
        view = TestExplodedView.explodedView(self, [(["B."], plc(0, 0, 20), 0)])
        at = self.doc.addObject("Assembly::ExplodedViewAt", "At")
        at.View = view
        return [self.drawn(view), self.drawn(at)]

    def testAPartNoStepMovesByHand(self):
        self.box("A")
        c = self.box("C", plc(60, 0, 0))
        drawings = self.views()
        self.doc.recompute()
        before = [self.height(d) for d in drawings]
        c.Placement = plc(60, 0, 100)
        self.doc.recompute()
        for d, h in zip(drawings, before):
            self.assertAlmostEqual(self.height(d), h + 80, 3, d.Source[0].Name)
            self.assertNotIn("Touched", d.State)
        # And nothing left touched: the assembly is not recomputed again
        self.assertEqual([o.Name for o in self.doc.Objects if "Touched" in o.State], [])

    def testAPartTheSolveMoves(self):
        a = self.box("A")
        c = self.box("C", plc(60, 0, 0))
        self.ground(a)
        joint = self.joint(
            "Fixed",
            (a, ["Face6", "Face6"]),
            plc(5, 5, 10),
            (c, ["Face5", "Face5"]),
            plc(5, 5, 0),
        )
        drawings = self.views()
        self.doc.recompute()
        self.assertAlmostEqual(c.Placement.Base.z, 10, 6)
        before = [self.height(d) for d in drawings]
        joint.Placement1 = plc(5, 5, 110)
        self.doc.recompute()
        self.assertAlmostEqual(c.Placement.Base.z, 110, 6)
        for d, h in zip(drawings, before):
            self.assertAlmostEqual(self.height(d), h + 90, 3, d.Source[0].Name)
            self.assertNotIn("Touched", d.State)
        # And nothing left touched: the assembly is not recomputed again
        self.assertEqual([o.Name for o in self.doc.Objects if "Touched" in o.State], [])


class TestExplodedViewBalloons(PlacementAsserts, JointTestBase):
    """Balloons with the parts' numbers in the BOM on a drawing of an
    exploded view (docs/AssemblyExplode.md sec 9)"""

    def build(self):
        self.a = self.box("A")
        self.b = self.box("B", plc(30, 0, 0))
        self.link = self.assembly.newObject("App::Link", "LinkA")
        self.link.LinkedObject = self.a
        self.link.Placement = plc(0, 30, 0)
        view = TestExplodedView.explodedView(self, [(["B."], plc(0, 0, 20), 0)])
        self.bom = CommandCreateView.assemblyBom(self.assembly)
        self.bom.recompute()
        return view

    def testBomRows(self):
        self.build()
        rows = dict((o.Name, i) for i, o in self.bom.getRows())
        self.assertIn("A", rows)
        self.assertIn("B", rows)
        # A link is numbered as the object it links, as the BOM files it
        self.assertEqual(self.bom.getIndexOf(self.link), rows["A"])
        self.assertEqual(self.bom.getIndexOf(self.assembly), "")
        # A mirrored link is a row of its own; a plain one stays in its part's
        mirrored = self.assembly.newObject("App::Link", "Mirrored")
        mirrored.LinkedObject = self.a
        mirrored.Scale = -1
        self.doc.recompute()
        self.bom.recompute()
        self.assertEqual(self.bom.getIndexOf(self.link), self.bom.getIndexOf(self.a))
        self.assertNotIn(self.bom.getIndexOf(mirrored), ("", self.bom.getIndexOf(self.a)))

    @unittest.skipIf(not App.GuiUp, "a view provider needs the GUI")
    def testEdgeInfoGivesPartNumbers(self):
        view = self.build()
        CommandCreateView.ViewProviderExplodedView(view.ViewObject)
        shape = view.getExplodedShape()
        byTag = {}
        for name, index in shape.ElementMap.items():
            if index.startswith("Edge"):
                history = shape.getElementHistory(name)
                byTag.setdefault(history[0] if history else None, name)
        # Without a BOM, a part's edge is the drawing's
        self.assertIsNone(view.ViewObject.getEdgeInfo(byTag[self.b.ID]))
        view.Bom = self.bom
        info = view.ViewObject.getEdgeInfo(byTag[self.b.ID])
        self.assertEqual(info["Label"], self.bom.getIndexOf(self.b))
        self.assertFalse(info["Styled"])
        self.assertTrue(info["OncePerLabel"])
        self.assertEqual(info["Bubble"], "Circular")
        self.assertEqual(
            view.ViewObject.getEdgeInfo(byTag[self.link.ID])["Label"], self.bom.getIndexOf(self.a)
        )
        trail = view.ViewObject.getEdgeInfo(trailNames(shape)[0])
        self.assertEqual((trail["Label"], trail["Style"], trail["Styled"]), ("", 2, True))

    @unittest.skipIf(not App.GuiUp, "a view provider needs the GUI")
    def testABodysEdgeIsTheBodys(self):
        """A Body's edges carry its features' tags: numbered as the Body"""
        body = self.assembly.newObject("PartDesign::Body", "Body")
        body.newObject("PartDesign::AdditiveBox", "Box")
        body.Placement = plc(0, 60, 0)
        view = self.build()
        CommandCreateView.ViewProviderExplodedView(view.ViewObject)
        view.Bom = self.bom
        self.bom.recompute()
        number = self.bom.getIndexOf(body)
        self.assertTrue(number)
        shape = view.getExplodedShape()
        labels = set()
        for name, index in shape.ElementMap.items():
            if index.startswith("Edge"):
                info = view.ViewObject.getEdgeInfo(name)
                if info and info["Label"]:  # not the trail
                    labels.add(info["Label"])
        # The Body's, A's and B's numbers, each its own
        self.assertEqual(labels, {number, self.bom.getIndexOf(self.a), self.bom.getIndexOf(self.b)})

    @unittest.skipIf(not App.GuiUp, "balloons are made by a TechDraw command")
    def testBalloons(self):
        try:
            import TechDraw  # noqa: F401
            from TDTest.TechDrawTestUtilities import createPageWithSVGTemplate
        except ImportError:
            self.skipTest("no TechDraw")
        view = self.build()
        CommandCreateView.ViewProviderExplodedView(view.ViewObject)
        page = createPageWithSVGTemplate(self.doc)
        drawing = self.doc.addObject("TechDraw::DrawViewPart", "Drawing")
        page.addView(drawing)
        drawing.Source = [view]
        drawing.Direction = App.Vector(1, -1, 1)
        self.doc.recompute()
        waitForWorkers()
        balloons = CommandCreateView.addBomBalloons(drawing)
        self.assertEqual(view.Bom, self.bom)
        # A and its link share A's number: one balloon for it, one for B
        texts = sorted(b.Text for b in balloons)
        self.assertEqual(texts, sorted({self.bom.getIndexOf(self.a), self.bom.getIndexOf(self.b)}))
        for b in balloons:
            self.assertEqual(b.BubbleShape, "Circular")
            self.assertTrue(b.SourceEdge)
            # A straight leader, as the ring lays them: a kink's shoulder
            # could cross a neighbour's
            self.assertEqual(b.KinkLength.Value, 0)
        # On the ring round the view, a balloon (14 mm) and a gap apart on
        # the paper
        k = drawing.Scale
        for i, p in enumerate(balloons):
            for q in balloons[i + 1 :]:
                apart = math.hypot((p.X.Value - q.X.Value) * k, (p.Y.Value - q.Y.Value) * k)
                self.assertGreaterEqual(apart, 17 - 1e-3)
        # Again: the same balloons, brought up to date
        names = sorted(b.Name for b in balloons)
        again = CommandCreateView.addBomBalloons(drawing)
        self.assertEqual(sorted(b.Name for b in again), names)

    def drawingWithBalloons(self, more=()):
        """A drawing of the exploded view of build(), with `more` boxes (name,
        placement) in the assembly, and its balloons"""
        try:
            import TechDraw  # noqa: F401
            from TDTest.TechDrawTestUtilities import createPageWithSVGTemplate
        except ImportError:
            self.skipTest("no TechDraw")
        boxes = [self.box(name, at) for name, at in more]
        view = self.build()
        CommandCreateView.ViewProviderExplodedView(view.ViewObject)
        page = createPageWithSVGTemplate(self.doc)
        drawing = self.doc.addObject("TechDraw::DrawViewPart", "Drawing")
        page.addView(drawing)
        drawing.Source = [view]
        drawing.Direction = App.Vector(1, -1, 1)
        self.doc.recompute()
        waitForWorkers()
        return drawing, boxes, CommandCreateView.addBomBalloons(drawing)

    def assertLaidOut(self, drawing, balloons, kept=()):
        """No balloon on the drawing, no two meet (14 mm and a gap on the
        paper) and no two leaders cross; `kept` (dragged) excepted"""
        import Part

        k = drawing.Scale
        box = Part.Compound(drawing.getVisibleEdges()).BoundBox
        balloons = [b for b in balloons if b not in kept]
        for b in balloons:
            x, y = b.X.Value * k, b.Y.Value * k
            inside = box.XMin < x < box.XMax and box.YMin < y < box.YMax
            self.assertFalse(inside, "%s at (%.1f, %.1f) on the drawing %s" % (b.Text, x, y, box))

        def leader(b):
            return (b.OriginX.Value, b.OriginY.Value), (b.X.Value, b.Y.Value)

        def cross(s, t):
            def side(a, b, c):
                return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

            (p, q), (r, u) = s, t
            return (side(p, q, r) * side(p, q, u) < -1e-9) and (
                side(r, u, p) * side(r, u, q) < -1e-9
            )

        for i, p in enumerate(balloons):
            for q in balloons[i + 1 :]:
                apart = math.hypot((p.X.Value - q.X.Value) * k, (p.Y.Value - q.Y.Value) * k)
                self.assertGreaterEqual(apart, 17 - 1e-3, (p.Text, q.Text))
                self.assertFalse(cross(leader(p), leader(q)), (p.Text, q.Text))

    @unittest.skipIf(not App.GuiUp, "balloons are made by a TechDraw command")
    def testBalloonsAreLaidAgainAfterAMove(self):
        """Run again after the parts moved: each balloon not dragged is laid
        on the ring again, not kept at its offset from its part"""
        drawing, _boxes, balloons = self.drawingWithBalloons([("C", plc(60, 0, 0))])
        self.assertLaidOut(drawing, balloons)
        names = sorted(b.Name for b in balloons)
        # C goes far up: the view is another shape, the balloons at their
        # offsets would stand on it
        self.doc.getObject("C").Placement = plc(0, 0, 150)
        self.doc.recompute()
        waitForWorkers()
        again = CommandCreateView.addBomBalloons(drawing)
        self.assertEqual(sorted(b.Name for b in again), names)
        self.assertLaidOut(drawing, again)

    @unittest.skipIf(not App.GuiUp, "balloons are made by a TechDraw command")
    def testLeadersDoNotCross(self):
        """Parts crowded round the view's middle, near and far from it: the
        balloons spread along the ring would cross their leaders"""
        more = [("P%d" % i, plc(12 * i - 40, 60, 0)) for i in range(10)]
        drawing, _boxes, balloons = self.drawingWithBalloons(more)
        self.assertEqual(len(balloons), 12)
        self.assertLaidOut(drawing, balloons)

    @unittest.skipIf(not App.GuiUp, "balloons are made by a TechDraw command")
    def testADraggedBalloonStays(self):
        drawing, _boxes, balloons = self.drawingWithBalloons([("C", plc(60, 0, 0))])
        dragged = balloons[0]
        dragged.X = dragged.X.Value + 40
        at = (dragged.X.Value - dragged.OriginX.Value, dragged.Y.Value - dragged.OriginY.Value)
        self.doc.getObject("C").Placement = plc(0, 0, 150)
        self.doc.recompute()
        waitForWorkers()
        again = CommandCreateView.addBomBalloons(drawing)
        # Where the user put it against its part; the others laid again
        self.assertAlmostEqual(dragged.X.Value - dragged.OriginX.Value, at[0], 6)
        self.assertAlmostEqual(dragged.Y.Value - dragged.OriginY.Value, at[1], 6)
        self.assertLaidOut(drawing, again, kept=[dragged])


class TestAutoExplode(PlacementAsserts, JointTestBase):
    """Steps made from the joints (docs/AssemblyExplode.md sec 10)"""

    def stack(self):
        """A grounded; B on A's top, C on B's; D on A's +X side"""
        self.a = self.box("A")
        self.b = self.box("B", plc(0, 0, 10))
        self.c = self.box("C", plc(0, 0, 20))
        self.d = self.box("D", plc(10, 0, 0))
        self.ground(self.a)
        self.assembly.makeJoint("Fixed", (self.a, "Face6"), (self.b, "Face5"))
        self.assembly.makeJoint("Fixed", (self.b, "Face6"), (self.c, "Face5"))
        self.assembly.makeJoint("Fixed", (self.a, "Face2"), (self.d, "Face1"))
        self.doc.recompute()

    def steps(self, view):
        return [(sorted(step.References[1]), step.MovementTransform) for step in view.Group]

    def testOuterPartsFirstEachTakingThoseOutside(self):
        self.stack()
        view = CommandCreateView.makeAutoExplode(self.assembly)
        self.doc.recompute()
        steps = self.steps(view)
        self.assertEqual([s for s, _m in steps], [["C."], ["B.", "C."], ["D."]])
        # Each by half again its size, along its joint, away from the inner part
        for (_s, move), want in zip(steps, (plc(0, 0, 15), plc(0, 0, 15), plc(15, 0, 0))):
            self.assertPlc(move, want)
        placed = TestExplodedView.placements(self, view)
        self.assertPlc(placed["C"], plc(0, 0, 50))
        self.assertPlc(placed["B"], plc(0, 0, 25))
        self.assertPlc(placed["D"], plc(25, 0, 0))
        self.assertNotIn("A", placed)
        # Nothing written
        self.assertPlc(self.c.Placement, plc(0, 0, 20))
        self.assertEqual(view.Group[2].Label, "Move D")

    def testSameWayAtOneDepthIsOneStep(self):
        a = self.box("A", size=30)
        b = self.box("B", plc(0, 0, 30))
        c = self.box("C", plc(20, 0, 30))
        self.ground(a)
        self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        self.assembly.makeJoint("Fixed", (a, "Face6"), (c, "Face5"))
        self.doc.recompute()
        view = CommandCreateView.makeAutoExplode(self.assembly)
        self.assertEqual([s for s, _m in self.steps(view)], [["B.", "C."]])
        self.assertEqual(view.Group[0].Label, "Move B, C")

    def testWithNoGroundTheLargestStays(self):
        a = self.box("A", size=20)
        b = self.box("B", plc(0, 0, 20))
        self.assembly.makeJoint("Fixed", (b, "Face5"), (a, "Face6"))
        self.doc.recompute()
        view = CommandCreateView.makeAutoExplode(self.assembly)
        ((subs, move),) = self.steps(view)
        self.assertEqual(subs, ["B."])
        self.assertPlc(move, plc(0, 0, 15))

    def testAPartNoJointReachesStays(self):
        self.stack()
        self.box("E", plc(50, 50, 0))
        view = CommandCreateView.makeAutoExplode(self.assembly)
        self.assertFalse(any("E." in s for s, _m in self.steps(view)))

    def testFactorAndAfterTheViewsSteps(self):
        self.stack()
        view = TestExplodedView.explodedView(self, [(["D."], plc(0, 5, 0), 0)])
        CommandCreateView.makeAutoExplode(self.assembly, view, factor=2.0)
        steps = self.steps(view)
        self.assertEqual(len(steps), 4)
        self.assertPlc(steps[0][1], plc(0, 5, 0))
        self.assertPlc(steps[1][1], plc(0, 0, 20))

    def testABoltThroughALidLeavesItOutwards(self):
        """Its middle below the lid's: the joint's side on the lid says out"""
        lid = self.box("Lid", size=10)
        self.ground(lid)
        bolt = self.cylinder("Bolt", 1, 16, plc(5, 5, -4))
        self.assembly.makeJoint("Fixed", (lid, "Face6"), (bolt, "Face3"))
        bolt.Placement = plc(5, 5, -4)  # where it was, through the lid
        self.doc.recompute()
        self.assertLess(bolt.Shape.BoundBox.Center.z, lid.Shape.BoundBox.Center.z)
        view = CommandCreateView.makeAutoExplode(self.assembly)
        ((subs, move),) = self.steps(view)
        self.assertEqual(subs, ["Bolt."])
        self.assertPlc(move, plc(0, 0, 24))

    def testTheAssemblysFrame(self):
        """A move is in the assembly's frame, as a part's Placement is"""
        self.assembly.Placement = plc(0, 100, 0, Z, 90)
        self.stack()
        view = CommandCreateView.makeAutoExplode(self.assembly)
        self.assertPlc(self.steps(view)[2][1], plc(15, 0, 0))
