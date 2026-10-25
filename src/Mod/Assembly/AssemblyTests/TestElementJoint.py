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

"""The C++ joint (Assembly::Joint): its sides are element links.

See docs/AssemblyObjectModel.md sec 7.1. Each side is made from a pick, a
component and a subname as a selection gives it; the joint coordinate
system is the element's frame, and the solver builder reads it through the
same helpers that read a Python joint's references.
"""

import os
import tempfile

import FreeCAD as App
import Part

from AssemblyTests.TestJoints import JointTestBase, X, Z, zaxis, xaxis, plc, TOL


class TestElementJoint(JointTestBase):
    def setUp(self):
        super().setUp()
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Part")

    def frames(self, joint, part1, part2):
        """The joint's frames in the assembly's coordinate system: Placement1/2
        are relative to the sides' components"""
        self.doc.recompute()
        return (
            part1.Placement.multiply(joint.Placement1),
            part2.Placement.multiply(joint.Placement2),
        )

    def assertCoincide(self, joint, part1, part2, flipped=None):
        p1, p2 = self.frames(joint, part1, part2)
        self.assertVecClose(p1.Base, p2.Base)
        dot = zaxis(p1).dot(zaxis(p2))
        self.assertLess(abs(abs(dot) - 1), 1e-6, "z axes not parallel: %r" % dot)
        if flipped is not None:
            self.assertEqual(dot < 0, flipped)

    # ------------------------------------------------------------ sides

    def testConvenienceElementsForPlainParts(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 0))
        joint = self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        self.assertEqual(joint.TypeId, "Assembly::Joint")
        self.assertEqual(joint.JointType, "Fixed")
        elements = self.assembly.Elements
        self.assertIsNotNone(elements)
        labels = sorted(e.Label for e in elements.Group)
        self.assertEqual(labels, ["_Element", "_Element001"])
        link1, link2 = joint.ElementLink1, joint.ElementLink2
        self.assertEqual(link1.TypeId, "Part::ElementLink")
        # The element links are the assembly's children, the joint holds them
        self.assertIn(link1, self.assembly.Group)
        self.assertIn(link2, self.assembly.Group)
        self.assertEqual(link1.Element[0], elements)

    def testElementInAComponentThatDeclaresElements(self):
        part = self.assembly.newObject("App::Part", "Sub")
        box = part.newObject("Part::Box", "SubBox")
        other = self.box("Other", plc(40, 0, 0))
        joint = self.assembly.makeJoint("Fixed", (part, "SubBox.Face6"), (other, "Face5"))
        self.assertIsNotNone(part.Elements)
        element = part.Elements.Group[0]
        self.assertEqual(element.Target[0], box)
        self.assertEqual(element.Target[1], ["Face6"])
        self.assertEqual(joint.ElementLink1.Element[0], part)
        self.assertEqual(joint.ElementLink1.Element[1], ["$$Elements.$%s." % element.Label])

    def testElementIsReused(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        c = self.box("C", plc(60, 0, 0))
        self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        self.assembly.makeJoint("Fixed", (a, "Face6"), (c, "Face5"))
        targets = [(e.Target[0].Name, e.Target[1]) for e in self.assembly.Elements.Group]
        self.assertEqual(targets.count(("A", ["Face6"])), 1)

    def testFrameOfAFace(self):
        a = self.box("A", plc(1, 2, 3))
        b = self.box("B", plc(30, 0, 0))
        joint = self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        p1, _ = self.frames(joint, a, b)
        # The top face of a 10 mm box, at its centre, normal up
        self.assertVecClose(p1.Base, App.Vector(6, 7, 13))
        self.assertVecClose(zaxis(p1), Z)

    def testFrameOfACircleAndACylinder(self):
        cyl = self.cylinder("Cyl", 3, 10, plc(5, 0, 0))
        self.doc.recompute()
        top = next(
            "Edge%d" % (i + 1)
            for i, e in enumerate(cyl.Shape.Edges)
            if e.Curve.TypeId == "Part::GeomCircle" and abs(e.Curve.Center.z - 10) < 1e-9
        )
        lateral = next(
            "Face%d" % (i + 1)
            for i, f in enumerate(cyl.Shape.Faces)
            if f.Surface.TypeId == "Part::GeomCylinder"
        )
        joint = self.assembly.makeJoint("Revolute", (cyl, top), (cyl, lateral))
        p1, p2 = self.frames(joint, cyl, cyl)
        # The top circle's centre, and the axis point level with the face's
        # centre of gravity
        self.assertVecClose(p1.Base, App.Vector(5, 0, 10))
        self.assertVecClose(p2.Base, App.Vector(5, 0, 5))
        self.assertVecClose(zaxis(p1), Z)
        self.assertLess(abs(abs(zaxis(p2).dot(Z)) - 1), 1e-9)

    def testVertexBecomesTheOffset(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        # A pick is made on shapes that are there
        self.doc.recompute()
        top = next(
            "Vertex%d" % (i + 1)
            for i, v in enumerate(a.Shape.Vertexes)
            if abs(v.Point.z - 10) < 1e-9 and v.Point.x < 1e-9 and v.Point.y < 1e-9
        )
        joint = self.assembly.makeJoint("Fixed", (a, "Face6", top), (b, "Face5"))
        p1, _ = self.frames(joint, a, b)
        self.assertVecClose(p1.Base, a.getSubObject(top).Point)
        self.assertVecClose(zaxis(p1), Z)
        self.assertFalse(joint.ElementLink1.Offset.isIdentity())
        # The element link keeps the vertex, and its Offset follows it
        self.assertEqual(joint.ElementLink1.Vertex[0], a)
        self.assertEqual(joint.ElementLink1.Vertex[1], [top])
        a.Length = 20
        p1, _ = self.frames(joint, a, b)
        self.assertVecClose(p1.Base, a.getSubObject(top).Point)

    # ------------------------------------------------------------ solving

    def testFixedJointSolves(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 2, Z, 20))
        self.ground(a)
        joint = self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        self.solve()
        self.assertCoincide(joint, a, b)

    def testRevoluteJointSolvesAndMatchesThePythonJoint(self):
        a = self.box("A")
        start = plc(30, 5, 2, Z, 20)
        b = self.box("B", start)
        self.ground(a)
        joint = self.assembly.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        self.solve()
        self.assertCoincide(joint, a, b)
        solved = b.Placement

        # The same joint the Python way, from the same start
        joint.Suppressed = True
        b.Placement = start
        self.doc.recompute()
        self.joint(
            "Revolute",
            [a, ["Face6", "Face6"]],
            joint.Placement1,
            [b, ["Face5", "Face5"]],
            joint.Placement2,
        )
        self.solve()
        self.assertVecClose(b.Placement.Base, solved.Base, 1e-6)
        self.assertTrue(b.Placement.Rotation.isSame(solved.Rotation, 1e-6))

    def testDistanceJointKeepsItsSubType(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 20))
        self.ground(a)
        joint = self.assembly.makeJoint("Distance", (a, "Face6"), (b, "Face5"))
        joint.Distance = 7
        self.solve()
        p1, p2 = self.frames(joint, a, b)
        self.assertClose((p2.Base - p1.Base).dot(zaxis(p1)), 7.0)

    def testMalformedWhenBothSidesAreOnePart(self):
        a = self.box("A")
        self.ground(a)
        self.assembly.makeJoint("Fixed", (a, "Face6"), (a, "Face5"))
        self.doc.recompute()
        self.assembly.solve()
        self.assertEqual(len(self.assembly.getSolveStatus()["Malformed"]), 1)

    def testSidesFromPython(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        c = self.box("C", plc(60, 0, 0))
        self.doc.recompute()
        joint = self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        self.assertEqual(joint.getComponent(1), a)
        self.assertEqual(joint.getComponent(2), b)
        self.assertVecClose(joint.getFrame(1).Base, App.Vector(5, 5, 10))
        old = joint.ElementLink2.Name
        link = joint.setSide(2, c, "Face5")
        self.assertEqual(joint.ElementLink2, link)
        self.assertEqual(joint.getComponent(2), c)
        self.assertIsNone(self.doc.getObject(old))
        joint.clearSide(1)
        self.assertIsNone(joint.ElementLink1)
        self.assertIsNone(joint.getComponent(1))
        with self.assertRaises(ValueError):
            joint.getFrame(3)

    # ------------------------------------------------------------ lifetime

    def testDeletingTheJointDeletesItsElementLinks(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        joint = self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        names = [joint.ElementLink1.Name, joint.ElementLink2.Name]
        self.doc.removeObject(joint.Name)
        for name in names:
            self.assertIsNone(self.doc.getObject(name))

    def testUnusedConvenienceElementGoes(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        c = self.box("C", plc(60, 0, 0))
        j1 = self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        self.assembly.makeJoint("Fixed", (a, "Face6"), (c, "Face5"))
        before = len(self.assembly.Elements.Group)
        self.doc.removeObject(j1.Name)
        # B's element is gone, A's is kept for the other joint
        targets = [e.Target[0].Name for e in self.assembly.Elements.Group]
        self.assertEqual(len(targets), before - 1)
        self.assertIn("A", targets)
        self.assertNotIn("B", targets)

    def testElementLinkOffsetIsTheSidesOffset(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        self.doc.recompute()
        joint = self.assembly.makeJoint("Fixed", (a, "Face6", "Vertex2"), (b, "Face5"))
        self.assertFalse(hasattr(joint, "Offset1"))
        # The user's offset composes with the picked vertex's, in the frame
        link = joint.ElementLink1
        vertex = joint.getFrame(1).Base
        link.Offset = link.Offset.multiply(plc(0, 0, 3))
        self.assertVecClose(joint.getFrame(1).Base, vertex + App.Vector(0, 0, 3))

    def testSaveAndRestore(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 2, Z, 20))
        self.ground(a)
        joint = self.assembly.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        self.solve()
        name = joint.Name
        path = os.path.join(tempfile.mkdtemp(), "ElementJoint.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.assembly = self.doc.getObject("Assembly")
        joint = self.doc.getObject(name)
        self.assertEqual(joint.TypeId, "Assembly::Joint")
        a = self.doc.getObject("A")
        b = self.doc.getObject("B")
        b.Placement = plc(30, 5, 2, Z, 20)
        self.solve()
        self.assertCoincide(joint, a, b)
