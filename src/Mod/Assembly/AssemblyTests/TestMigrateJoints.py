# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of FreeCAD.                                         *
# *                                                                         *
# *   FreeCAD is free software: you can redistribute it and/or modify it    *
# *   under the terms of the GNU Lesser General Public License as           *
# *   published by the Free Software Foundation, either version 2.1 of the  *
# *   License, or (at your option) any later version.                       *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful, but        *
# *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
# *   Lesser General Public License for more details.                       *
# *                                                                         *
# *   You should have received a copy of the GNU Lesser General Public      *
# *   License along with FreeCAD. If not, see                               *
# *   <https://www.gnu.org/licenses/>.                                      *
# *                                                                         *
# ***************************************************************************

"""Upstream's joints converted to C++ joints when a file is opened.

See AssemblyMigrate.py and docs/AssemblyObjectModel.md sec 8. Each test
builds upstream's Python joints, saves, and opens the file again: the
joints must be C++ joints with the same frames, and the parts must not move
on a re-solve.
"""

import os
import tempfile

import FreeCAD as App

import JointObject
import UtilsAssembly

from AssemblyTests.TestJoints import JointTestBase, Z, plc

FRAME_TOL = 1e-7


class TestMigrateJoints(JointTestBase):
    def setUp(self):
        super().setUp()
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._convert = self.params.GetBool("ConvertJointsOnOpen", True)
        self.params.SetBool("ConvertJointsOnOpen", True)

    def tearDown(self):
        self.params.SetBool("ConvertJointsOnOpen", self._convert)
        super().tearDown()

    # ------------------------------------------------------------ helpers

    def pythonJoint(self, jtype, ref1, ref2, **props):
        """Upstream's joint, its frames from the picks as its task makes them"""
        j = self.jointgroup.newObject("App::FeaturePython", jtype)
        JointObject.Joint(j, JointObject.JointTypes.index(jtype))
        for k, v in props.items():
            setattr(j, k, v)
        j.Proxy.setJointConnectors(j, [ref1, ref2])
        return j

    def topVertex(self, box):
        for i, v in enumerate(box.Shape.Vertexes):
            if abs(v.Point.z - box.Height.Value) < 1e-9 and v.Point.x < 1e-9 and v.Point.y < 1e-9:
                return "Vertex%d" % (i + 1)
        self.fail("no top vertex")

    def reopen(self):
        path = os.path.join(tempfile.mkdtemp(), "Migrate.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.assembly = self.doc.getObject("Assembly")
        self.jointgroup = UtilsAssembly.getJointGroup(self.assembly)

    def joints(self):
        return [o for o in self.jointgroup.Group if o.isDerivedFrom("Assembly::Joint")]

    def frames(self, joint):
        return JointObject.sideJcsGlobal(joint, 1), JointObject.sideJcsGlobal(joint, 2)

    def assertSameFrame(self, a, b, msg=""):
        self.assertTrue(a.isSame(b, FRAME_TOL), "%s: %s != %s" % (msg, a, b))

    def placements(self, *objs):
        return {o.Name: o.Placement for o in objs}

    def assertNotMoved(self, before):
        for name, p in before.items():
            self.assertTrue(
                self.doc.getObject(name).Placement.isSame(p, 1e-6),
                "%s moved: %s -> %s" % (name, p, self.doc.getObject(name).Placement),
            )

    # ------------------------------------------------------------ tests

    def testRevoluteWithVertexAndOffset(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 2, Z, 20))
        self.ground(a)
        self.doc.recompute()
        j = self.pythonJoint(
            "Revolute", [a, ["Face6", self.topVertex(a)]], [b, ["Face5", "Face5"]]
        )
        j.Offset2 = plc(0, 0, 2, Z, 15)
        j.Label = "Hinge"
        self.solve()
        before = self.frames(j)
        poses = self.placements(a, b)

        self.reopen()
        joints = self.joints()
        self.assertEqual(len(joints), 1)
        new = joints[0]
        self.assertEqual(new.Label, "Hinge")
        self.assertEqual(new.JointType, "Revolute")
        after = self.frames(new)
        self.assertSameFrame(after[0], before[0], "side 1")
        self.assertSameFrame(after[1], before[1], "side 2")
        # The vertex is kept live, the offset whole
        self.assertIsNotNone(new.ElementLink1.Vertex)
        self.assertIsNone(new.ElementLink2.Vertex)
        self.solve()
        self.assertNotMoved(poses)

    def testVertexStaysLive(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 2, Z, 20))
        self.ground(a)
        self.doc.recompute()
        vertex = self.topVertex(a)
        self.pythonJoint("Fixed", [a, ["Face6", vertex]], [b, ["Face5", "Face5"]])
        self.solve()
        self.reopen()
        new = self.joints()[0]
        a = self.doc.getObject("A")
        a.Length = 20
        self.doc.recompute()
        self.assertTrue(
            JointObject.sideJcsGlobal(new, 1).Base.isEqual(a.getSubObject(vertex).Point, 1e-7)
        )

    def testDetachedSideKeepsItsFrame(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 2))
        self.ground(a)
        self.doc.recompute()
        j = self.joint("Fixed", [a, ["Face6", "Face6"]], plc(1, 2, 10, Z, 30),
                       [b, ["Face5", "Face5"]], plc(3, 4, 0))
        self.solve()
        before = self.frames(j)
        self.reopen()
        new = self.joints()[0]
        after = self.frames(new)
        self.assertSameFrame(after[0], before[0], "side 1")
        self.assertSameFrame(after[1], before[1], "side 2")
        self.assertIsNone(new.ElementLink1.Vertex)

    def testDistanceKeepsItsValueAndExpression(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 20))
        self.ground(a)
        self.doc.recompute()
        j = self.pythonJoint(
            "Distance", [a, ["Face6", self.topVertex(a)]], [b, ["Face5", "Face5"]], Distance=4
        )
        j.setExpression("Distance", "2 * 3 mm")
        j.setExpression("Offset2.Base.z", "1 mm")
        self.solve()
        before = self.frames(j)
        self.reopen()
        new = self.joints()[0]
        self.assertAlmostEqual(new.Distance.Value, 6.0)
        self.assertIn("Distance", [p for p, _ in new.ExpressionEngine])
        # A Distance joint does not take its frame from the vertex
        self.assertIsNone(new.ElementLink1.Vertex)
        after = self.frames(new)
        self.assertSameFrame(after[0], before[0], "side 1")
        self.assertSameFrame(after[1], before[1], "side 2")

    def testWhatLinkedTheJointFollowsIt(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        self.ground(a)
        self.doc.recompute()
        j = self.pythonJoint("Revolute", [a, ["Face6", "Face6"]], [b, ["Face5", "Face5"]])
        motion = self.doc.addObject("App::FeaturePython", "Motion")
        motion.addProperty("App::PropertyXLinkSubHidden", "Joint", "Motion")
        motion.Joint = [j, ""]
        self.solve()
        self.reopen()
        new = self.joints()[0]
        self.assertEqual(self.doc.getObject("Motion").Joint[0], new)

    def testPreferenceOff(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        self.ground(a)
        self.doc.recompute()
        self.pythonJoint("Revolute", [a, ["Face6", "Face6"]], [b, ["Face5", "Face5"]])
        self.params.SetBool("ConvertJointsOnOpen", False)
        self.reopen()
        self.assertEqual(self.joints(), [])
        import AssemblyMigrate

        self.assertTrue(any(AssemblyMigrate.isPythonJoint(o) for o in self.jointgroup.Group))
