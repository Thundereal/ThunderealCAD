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

import FreeCAD as App

import CommandCreateView
import UtilsAssembly

from AssemblyTests.TestJoints import JointTestBase, Z, plc

TOL = 1e-9


def turn(center, angle):
    """A turn of `angle` degrees about Z through `center`, as the panel's
    dragger makes it"""
    c = App.Vector(*center)
    return App.Placement(c, App.Rotation(Z, angle)) * App.Placement(-c, App.Rotation())


class TestExplodedView(JointTestBase):
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

    def assertPlc(self, a, b, msg=None):
        self.assertTrue(a.isSame(b, TOL), f"{msg or ''}: {a} != {b}")

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
