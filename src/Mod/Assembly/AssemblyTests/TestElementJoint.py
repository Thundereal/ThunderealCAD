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

"""The C++ joint (Assembly::Joint): its sides are element bindings.

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
        self.assertTrue(joint.isDerivedFrom("Assembly::Joint"))
        self.assertEqual(joint.JointType, "Fixed")
        elements = self.assembly.Elements
        self.assertIsNotNone(elements)
        labels = sorted(e.Label for e in elements.Group)
        self.assertEqual(labels, ["_Element", "_Element001"])
        link1, link2 = joint.Binding1, joint.Binding2
        self.assertEqual(link1.TypeId, "Part::ElementBinding")
        # The element bindings are the assembly's children, the joint holds them
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
        self.assertEqual(joint.Binding1.Element[0], part)
        self.assertEqual(joint.Binding1.Element[1], ["$$Elements.$%s." % element.Label])

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

    def testFrameThroughAComponentThatDeclaresElements(self):
        # The side names the element the part declares ("$$Elements.$X."),
        # no face: still framed as the face is, not as a whole object
        part = self.assembly.newObject("App::Part", "Sub")
        part.Placement = plc(1, 2, 3)
        part.newObject("Part::Box", "SubBox")
        body = self.assembly.newObject("App::Part", "BodyPart")
        body.Placement = plc(30, 0, 0)
        pdbody = body.newObject("PartDesign::Body", "Body")
        pdbody.newObject("PartDesign::AdditiveBox", "PDBox")
        self.doc.recompute()
        joint = self.assembly.makeJoint("Fixed", (part, "SubBox.Face6"), (body, "Body.Face5"))
        self.assertEqual(joint.Binding1.Element[0], part)
        self.assertEqual(joint.Binding2.Element[0], body)
        p1, p2 = self.frames(joint, part, body)
        self.assertVecClose(p1.Base, App.Vector(6, 7, 13))
        self.assertVecClose(zaxis(p1), Z)
        self.assertVecClose(p2.Base, App.Vector(35, 5, 0))
        # As a convenience element of the assembly frames the same faces
        joint.setSide(1, part, "SubBox.Face6", "", True)
        joint.setSide(2, body, "Body.Face5", "", True)
        self.assertNotEqual(joint.Binding1.Element[0], part)
        q1, q2 = self.frames(joint, part, body)
        self.assertTrue(p1.isSame(q1, 1e-9), "%r != %r" % (p1, q1))
        self.assertTrue(p2.isSame(q2, 1e-9), "%r != %r" % (p2, q2))

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

    def testWholeObjectWithoutAShape(self):
        # An empty link, as Insert New Part picks it before it links the
        # part: a whole object is its own frame, shape or not
        link = self.assembly.newObject("App::Link", "Link")
        link.Placement = plc(3, 4, 5, Z, 30)
        b = self.box("B", plc(30, 0, 0))
        self.doc.recompute()
        joint = self.assembly.makeJoint("Fixed", (link, ""), (b, "Face5"))
        self.doc.recompute()
        self.assertNotIn("Invalid", joint.State)
        self.assertTrue(joint.getFrame(1).isSame(link.Placement, 1e-9))

    def testAnAxisIsTheFrameAlongIt(self):
        # A whole axis of a coordinate system: Z along the line, as
        # upstream's joint takes it (UtilsAssembly.findPlacement)
        part = self.assembly.newObject("App::Part", "Holder")
        part.Placement = plc(3, 4, 5, Z, 30)
        b = self.box("B", plc(30, 0, 0))
        self.doc.recompute()
        for role, axis in (("X_Axis", App.Vector(1, 0, 0)), ("Y_Axis", App.Vector(0, 1, 0)),
                           ("Z_Axis", App.Vector(0, 0, 1))):
            line = next(f for f in part.Origin.OriginFeatures if f.Role == role)
            joint = self.assembly.makeJoint(
                "Revolute", (part, "%s.%s." % (part.Origin.Name, line.Name)), (b, "Face5")
            )
            self.doc.recompute()
            self.assertNotIn("Invalid", joint.State, role)
            frame = joint.getFrame(1)
            self.assertVecClose(frame.Base, part.Placement.Base)
            along = part.Placement.Rotation.multVec(axis)
            self.assertVecClose(zaxis(frame), along)

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
        # The element keeps the vertex, and its Offset follows it; the
        # element binding refers to the element only
        link = joint.Binding1
        element = link.Element[0].getSubObject(link.Element[1][0], retType=1)
        self.assertEqual(element.Vertex[0], a)
        self.assertEqual(element.Vertex[1], [top])
        self.assertFalse(element.Offset.isIdentity())
        self.assertTrue(link.Offset.isIdentity())
        self.assertFalse(hasattr(link, "Vertex"))
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

    def testElementBindingsAreNoComponents(self):
        # A joint's element bindings are children of the assembly and shapes,
        # but no parts: counted, each added six freedoms to the DoF
        a = self.box("A")
        b = self.box("B", plc(30, 5, 2, Z, 20))
        self.ground(a)
        self.assembly.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        status = self.solve()
        self.assertEqual(status["DoF"], 1)

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
        old = joint.Binding2.Name
        link = joint.setSide(2, c, "Face5")
        self.assertEqual(joint.Binding2, link)
        self.assertEqual(joint.getComponent(2), c)
        self.assertIsNone(self.doc.getObject(old))
        joint.clearSide(1)
        self.assertIsNone(joint.Binding1)
        self.assertIsNone(joint.getComponent(1))
        with self.assertRaises(ValueError):
            joint.getFrame(3)

    # ------------------------------------------------------------ lifetime

    def testDeletingTheJointDeletesItsElementBindings(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        joint = self.assembly.makeJoint("Fixed", (a, "Face6"), (b, "Face5"))
        names = [joint.Binding1.Name, joint.Binding2.Name]
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

    def testElementBindingOffsetIsTheSidesOffset(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        self.doc.recompute()
        joint = self.assembly.makeJoint("Fixed", (a, "Face6", "Vertex2"), (b, "Face5"))
        self.assertFalse(hasattr(joint, "Offset1"))
        # The user's offset composes with the picked vertex's, in the frame
        link = joint.Binding1
        vertex = joint.getFrame(1).Base
        link.Offset = link.Offset.multiply(plc(0, 0, 3))
        self.assertVecClose(joint.getFrame(1).Base, vertex + App.Vector(0, 0, 3))

    def testRestoresTheNamesOfBefore(self):
        """A document of before the rename names Part::ElementLink and the
        joint's ElementLink1/2: it restores as Part::ElementBinding and
        Binding1/2, the joint on the same frames. The document is this one
        saved as a directory (an archive's blobs are zstd, which Python's
        zipfile does not read) with the old names written back into its XML
        (Document.xml, and each object's file: SplitXML), as the build
        before 2026-10-28 wrote them."""
        part = self.assembly.newObject("App::Part", "Part")
        part.newObject("Part::Box", "SubBox")
        b = self.box("B", plc(0, 0, 10))
        self.doc.recompute()
        joint = self.assembly.makeJoint("Fixed", (part, "SubBox.Face6"), (b, "Face5"))
        joint.Binding2.Offset = plc(1, 2, 3)
        self.doc.recompute()
        frames = [joint.getFrame(1), joint.getFrame(2)]
        name = joint.Name
        old = os.path.join(tempfile.mkdtemp(), "Old")
        os.makedirs(old)
        self.doc.saveAs(old)
        App.closeDocument(self.doc.Name)
        renamed = {"type": 0, "Binding1": 0, "Binding2": 0}
        for fileName in os.listdir(old):
            if not fileName.endswith(".xml"):
                continue
            xml = os.path.join(old, fileName)
            with open(xml, encoding="utf-8") as f:
                text = f.read()
            renamed["type"] += text.count('type="Part::ElementBinding"')
            text = text.replace('type="Part::ElementBinding"', 'type="Part::ElementLink"')
            for i in (1, 2):
                renamed["Binding%d" % i] += text.count('<Property name="Binding%d"' % i)
                text = text.replace(
                    '<Property name="Binding%d"' % i, '<Property name="ElementLink%d"' % i
                )
            with open(xml, "w", encoding="utf-8") as f:
                f.write(text)
        self.assertEqual(renamed, {"type": 2, "Binding1": 1, "Binding2": 1})
        self.doc = App.openDocument(old)
        self.assembly = self.doc.getObject("Assembly")
        joint = self.doc.getObject(name)
        for i in (1, 2):
            binding = getattr(joint, "Binding%d" % i)
            self.assertIsNotNone(binding, i)
            self.assertEqual(binding.TypeId, "Part::ElementBinding")
            # The old name still answers, for the macros of before
            self.assertTrue(binding.isDerivedFrom("Part::ElementLink"))
        self.assertFalse(hasattr(joint, "ElementLink1"))
        self.assertTrue(joint.Binding2.Offset.isSame(plc(1, 2, 3), 1e-12))
        self.doc.recompute()
        for i in (1, 2):
            self.assertTrue(joint.getFrame(i).isSame(frames[i - 1], 1e-9), i)
        # And the old Python name makes the new type
        made = Part.makeElementLink(self.assembly, self.assembly.Elements, "$_Element.")
        self.assertEqual(made.TypeId, "Part::ElementBinding")

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
        self.assertTrue(joint.isDerivedFrom("Assembly::Joint"))
        a = self.doc.getObject("A")
        b = self.doc.getObject("B")
        b.Placement = plc(30, 5, 2, Z, 20)
        self.solve()
        self.assertCoincide(joint, a, b)
