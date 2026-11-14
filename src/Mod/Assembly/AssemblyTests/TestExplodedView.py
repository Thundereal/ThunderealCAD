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
