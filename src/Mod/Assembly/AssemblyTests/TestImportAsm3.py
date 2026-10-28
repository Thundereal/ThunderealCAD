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

"""Assembly3 assemblies converted to Assembly objects (AssemblyImportAsm3).

Each test builds an Assembly3 assembly with the bundled Assembly3 code, as
its commands do but from explicit selections, in a pose its constraints
hold, converts it, and checks what it became and that a solve moves
nothing. testSample converts the essais_asm3 sample of
docs/AssemblyFromAsm3.md B.6 when ASM3_SAMPLE_DIR names its folder.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import FreeCAD as App

import AssemblyImportAsm3

Z = App.Vector(0, 0, 1)


def plc(x=0.0, y=0.0, z=0.0, axis=None, angle=0.0):
    return App.Placement(App.Vector(x, y, z), App.Rotation(axis or Z, angle))


class TestImportAsm3(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("ImportAsm3")
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        for name in list(App.listDocuments()):
            if name.startswith("ImportAsm3"):
                App.closeDocument(name)

    # ------------------------------------------------------------ building

    def asm3(self, name="Assembly"):
        """An Assembly3 assembly, as Assembly.make makes one without the GUI"""
        from freecad.asm3.assembly import Assembly

        return self.doc.addObject("Part::FeaturePython", name, Assembly(), None, True)

    def part(self, a, typ, name, placement=None, **props):
        obj = self.doc.addObject(typ, name)
        for k, v in props.items():
            setattr(obj, k, v)
        if placement is not None:
            obj.Placement = placement
        # Its shape, which an element needs when it is made
        obj.recompute()
        group = a.Proxy.getPartGroup()
        group.Group = group.Group + [obj]
        return obj

    def constraint(self, a, typeid, *subs, **props):
        """An Assembly3 constraint of the elements, paths in the part group"""
        from freecad.asm3.assembly import AsmConstraint

        group = a.Proxy.getPartGroup()
        sel = AsmConstraint.Selection(None, None, a, None, [(group, s) for s in subs])
        cstr = AsmConstraint.make(typeid, sel, undo=False)
        for k, v in props.items():
            setattr(cstr, k, v)
        return cstr

    def convert(self):
        self.doc.recompute()
        self.poses = self.placements()
        self.report = AssemblyImportAsm3.convert(self.doc)
        self.assemblies = [
            o for o in self.doc.Objects if o.isDerivedFrom("Assembly::AssemblyObject")
        ]
        return self.report

    def placements(self):
        return {
            o.Name: o.Placement
            for o in self.doc.Objects
            if o.isDerivedFrom("Part::Feature")
            and not o.isDerivedFrom("Part::ElementBinding")
            and not o.isDerivedFrom("Part::Element")
            and not o.isDerivedFrom("Part::FeaturePython")
        }

    def joints(self, assembly=None):
        assembly = assembly or self.assemblies[-1]
        return list(assembly.getGroup("Joints").Group)

    def kinds(self, assembly=None):
        return sorted(getattr(j, "JointType", j.TypeId) for j in self.joints(assembly))

    def assertNotMoved(self, tol=1e-6):
        for name, p in self.poses.items():
            obj = self.doc.getObject(name)
            self.assertTrue(
                obj.Placement.isSame(p, tol), "%s moved: %s -> %s" % (name, p, obj.Placement)
            )

    def solveAll(self):
        for a in self.assemblies:
            self.assertEqual(a.solve(), 0, a.Label)

    # ------------------------------------------------------------ tests

    def testNothingToConvert(self):
        self.doc.addObject("Part::Box", "Box")
        self.assertFalse(AssemblyImportAsm3.isAsm3Document(self.doc))
        self.assertEqual(str(AssemblyImportAsm3.convert(self.doc)), "")

    def testPlaneCoincidentIsARevolute(self):
        a = self.asm3()
        self.part(a, "Part::Box", "A")
        b = self.part(a, "Part::Box", "B", plc(0, 0, 10, Z, 30))
        self.constraint(a, 35, "A.Face6", "B.Face5")
        self.constraint(a, 0, "A.Face1")
        self.convert()
        self.assertEqual(len(self.assemblies), 1)
        new = self.assemblies[0]
        self.assertEqual(new.Label, "Assembly")
        self.assertEqual(self.kinds(), ["Assembly::GroundedJointPython", "Revolute"])
        self.assertEqual(sorted(j.Label for j in self.joints()), ["Locked", "PlaneCoincident"])
        # Assembly3's objects are gone, the parts in the new assembly
        self.assertEqual(AssemblyImportAsm3.findAssemblies(self.doc), [])
        self.assertIn(b, new.Group)
        self.solveAll()
        self.assertNotMoved()
        # And it holds B on A's top face
        b.Placement = plc(3, 4, 15, Z, 30)
        self.solveAll()
        self.assertAlmostEqual(b.Placement.Base.z, 10, 6)

    def testPlaneCoincidentOffsetAndLockAngle(self):
        a = self.asm3()
        self.part(a, "Part::Box", "A")
        self.part(a, "Part::Box", "B", plc(2, 0, 13, Z, 20))
        self.constraint(a, 35, "A.Face6", "B.Face5", Offset=3, OffsetX=2, LockAngle=True, Angle=20)
        self.constraint(a, 0, "A.Face1")
        self.convert()
        self.assertEqual(self.kinds(), ["Assembly::GroundedJointPython", "Fixed"])
        self.solveAll()
        self.assertNotMoved()

    def testAxialAlignmentAndColinearAreCylindrical(self):
        a = self.asm3()
        self.part(a, "Part::Cylinder", "Shaft", Radius=2, Height=40)
        self.part(a, "Part::Cylinder", "Bush", plc(0, 0, 10, Z, 40), Radius=2, Height=5)
        self.part(a, "Part::Cylinder", "Ring", plc(0, 0, 25), Radius=2, Height=3)
        self.constraint(a, 36, "Shaft.Face1", "Bush.Face1")
        self.constraint(a, 39, "Shaft.Face1", "Ring.Face1")
        self.constraint(a, 0, "Shaft.Face2")
        self.convert()
        self.assertEqual(
            self.kinds(), ["Assembly::GroundedJointPython", "Cylindrical", "Cylindrical"]
        )
        self.solveAll()
        self.assertNotMoved()

    def testPointPlaneDistance(self):
        a = self.asm3()
        self.part(a, "Part::Box", "A")
        b = self.part(a, "Part::Box", "B", plc(20, 0, 2.5))
        # B's origin vertex is 2.5 above A's top face
        vertex = None
        for i, v in enumerate(b.Shape.Vertexes):
            if v.Point.isEqual(App.Vector(20, 0, 2.5), 1e-9):
                vertex = "Vertex%d" % (i + 1)
        self.assertIsNotNone(vertex)
        self.constraint(a, 7, "B." + vertex, "A.Face6", Distance=7.5)
        self.constraint(a, 0, "A.Face1")
        self.convert()
        joints = [j for j in self.joints() if getattr(j, "JointType", "") == "Distance"]
        self.assertEqual(len(joints), 1)
        self.solveAll()
        self.assertNotMoved()

    def testMultiAndCascade(self):
        for cascade in (False, True):
            self.tearDown()
            self.setUp()
            a = self.asm3()
            self.part(a, "Part::Box", "A")
            self.part(a, "Part::Box", "B", plc(0, 0, 10))
            self.part(a, "Part::Box", "C", plc(0, 0, 20))
            self.constraint(a, 35, "A.Face6", "B.Face5", "C.Face5", Cascade=cascade, LockAngle=True)
            self.constraint(a, 0, "A.Face1")
            self.convert()
            pairs = sorted(
                tuple(sorted(j.getComponent(i).Name for i in (1, 2)))
                for j in self.joints()
                if getattr(j, "JointType", "") == "Fixed"
            )
            if cascade:
                self.assertEqual(pairs, [("A", "B"), ("B", "C")])
            else:
                self.assertEqual(pairs, [("A", "B"), ("A", "C")])

    def testLocks(self):
        a = self.asm3()
        self.part(a, "Part::Box", "A")
        b = self.part(a, "Part::Box", "B", plc(30, 0, 0))
        self.constraint(a, 0, "A.Face1")
        self.constraint(a, 0, "B.Vertex1")
        self.convert()
        self.assertEqual(
            self.kinds(),
            ["Assembly::GroundedJointPython", "Assembly::GroundedJointPython", "Ball"],
        )
        datums = [o for o in self.assemblies[0].Group if o.Label.endswith("_Datum")]
        self.assertEqual(len(datums), 1)
        self.solveAll()
        self.assertNotMoved()

    def testNoLockGroundsTheFirstPart(self):
        a = self.asm3()
        self.part(a, "Part::Box", "A")
        self.part(a, "Part::Box", "B", plc(0, 0, 10))
        self.constraint(a, 35, "A.Face6", "B.Face5")
        self.convert()
        grounds = [j for j in self.joints() if hasattr(j, "ObjectToGround")]
        self.assertEqual([g.ObjectToGround.Name for g in grounds], ["A"])
        self.assertIn("no lock", str(self.report))

    def testDisabledIsSuppressed(self):
        a = self.asm3()
        self.part(a, "Part::Box", "A")
        self.part(a, "Part::Box", "B", plc(0, 0, 10))
        self.constraint(a, 35, "A.Face6", "B.Face5", Disabled=True)
        self.constraint(a, 0, "A.Face1")
        self.convert()
        joint = [j for j in self.joints() if hasattr(j, "JointType")][0]
        self.assertTrue(joint.Suppressed)

    def testUnconvertedTypesAreReported(self):
        a = self.asm3()
        self.part(a, "Part::Box", "A")
        self.part(a, "Part::Box", "B", plc(0, 0, 10))
        self.constraint(a, 0, "A.Face1")
        self.constraint(a, 2, "A.Face6", "B.Face5")  # SameOrientation
        self.convert()
        self.assertIn("approximate", str(self.report))

    def testNestedAssemblyDirectlyAndThroughALink(self):
        child = self.asm3("Child")
        self.part(child, "Part::Box", "Base")
        self.part(child, "Part::Box", "Top", plc(0, 0, 10))
        self.constraint(child, 35, "Base.Face6", "Top.Face5", LockAngle=True)
        self.constraint(child, 0, "Base.Face1")
        parent = self.asm3("Parent")
        self.part(parent, "Part::Box", "Floor", plc(0, 0, -10))
        group = parent.Proxy.getPartGroup()
        group.Group = group.Group + [child]
        link = self.doc.addObject("App::Link", "Second")
        link.LinkedObject = child
        link.Placement = plc(30, 0, 0)
        group.Group = group.Group + [link]
        self.doc.recompute()
        self.constraint(parent, 0, "Floor.Face1")
        # The child's base on the floor, directly and through the link
        self.constraint(parent, 35, "Floor.Face6", "Child.2.Base.Face5", LockAngle=True)
        self.constraint(parent, 35, "Floor.Face6", "Second.2.Base.Face5", LockAngle=True)
        self.convert()
        self.assertEqual(sorted(a.Label for a in self.assemblies), ["Child", "Parent"])
        newChild = [a for a in self.assemblies if a.Label == "Child"][0]
        newParent = [a for a in self.assemblies if a.Label == "Parent"][0]
        self.assertIn(newChild, newParent.Group)
        # Each assembly's group by its own label, the same in both
        self.assertEqual([a.getGroup("Joints").Label for a in self.assemblies], ["Joints"] * 2)
        self.assertIs(self.doc.getObject("Second").LinkedObject, newChild)
        self.assertEqual(self.kinds(newParent).count("Fixed"), 2)
        # A side through the link names the child's element by its label
        paths = [
            l.Element[1][0]
            for j in self.joints(newParent)
            if hasattr(j, "JointType")
            for l in (j.Binding1, j.Binding2)
        ]
        self.assertTrue(all(p.startswith("$") for p in paths), paths)
        targets = [e.Target for e in newParent.Elements.Group]
        self.assertTrue(any(t[1][0].startswith("$$Elements.") for t in targets), targets)
        self.solveAll()
        self.assertNotMoved()

    def pinsAndPlate(self, a):
        """A plate with four holes and a link array of four pins, each in its
        hole: Assembly3 moves the array's elements one by one"""
        import Part

        holes = ((10, 10), (50, 10), (10, 50), (50, 50))
        shape = Part.makeBox(60, 60, 5)
        for x, y in holes:
            shape = shape.cut(Part.makeCylinder(2, 5, App.Vector(x, y, 0)))
        plate = self.part(a, "Part::Feature", "Plate", Shape=shape)
        pin = self.doc.addObject("Part::Cylinder", "Pin")
        pin.Radius = 2
        pin.Height = 15
        pin.recompute()
        pins = self.doc.addObject("App::Link", "Pins")
        pins.LinkedObject = pin
        pins.ElementCount = 4
        pins.ShowElement = True
        pins.recompute()
        for element, (x, y) in zip(pins.ElementList, holes):
            element.Placement = plc(x, y, 5)
        group = a.Proxy.getPartGroup()
        group.Group = group.Group + [pins]

        def circle(shape, x, y, z):
            for i, e in enumerate(shape.Edges):
                if e.Curve.TypeId == "Part::GeomCircle" and e.Curve.Center.isEqual(
                    App.Vector(x, y, z), 1e-6
                ):
                    return "Edge%d" % (i + 1)

        self.holeEdge = lambda x, y: circle(plate.Shape, x, y, 5)
        self.pinEdge = circle(pin.Shape, 0, 0, 0)
        self.holes = holes
        return plate, pins

    def testAnArrayElement(self):
        a = self.asm3()
        plate, pins = self.pinsAndPlate(a)
        self.constraint(a, 0, "Plate.Face1")
        self.constraint(a, 35, "Pins.1.%s" % self.pinEdge, "Plate.%s" % self.holeEdge(50, 10))
        self.convert()
        joint = [j for j in self.joints() if getattr(j, "JointType", "") == "Revolute"][0]
        self.assertEqual(joint.getComponent(1), pins.ElementList[1])
        self.solveAll()
        self.assertNotMoved()
        # It holds that element, not the others
        pins.ElementList[1].Placement = plc(7, 7, 30)
        pins.ElementList[2].Placement = plc(7, 7, 30)
        self.solveAll()
        self.assertTrue(pins.ElementList[1].Placement.Base.isEqual(App.Vector(50, 10, 5), 1e-6))
        self.assertTrue(pins.ElementList[2].Placement.Base.isEqual(App.Vector(7, 7, 30), 1e-6))

    def testMultipliedIsOneMultipliedJoint(self):
        a = self.asm3()
        plate, pins = self.pinsAndPlate(a)
        self.constraint(a, 0, "Plate.Face1")
        # The hole picked is not the one the first pin is in: Assembly3
        # pairs them by position
        cstr = self.constraint(
            a, 35, "Pins.0.%s" % self.pinEdge, "Plate.%s" % self.holeEdge(50, 10)
        )
        # As Assembly3's multiply command leaves the first element binding
        first = cstr.Group[0]
        first.addProperty("App::PropertyInteger", "Count", "", "")
        first.addProperty("App::PropertyPlacementList", "PlacementList", "", "")
        first.Count = 4
        cstr.Multiply = True
        # The target made again multiplied, so that Assembly3 gathers the
        # holes like it into its element, as its command does
        group = a.Proxy.getPartGroup()
        cstr.Group[1].Proxy.setLink(group, "Plate.%s" % self.holeEdge(50, 10), multiply=True)
        self.doc.recompute()
        # Solved, as a saved file is: each pin in its hole
        for element, (x, y) in zip(pins.ElementList, self.holes):
            element.Placement = plc(x, y, 5)
        self.convert()
        joints = [j for j in self.joints() if getattr(j, "JointType", "") == "Revolute"]
        self.assertEqual(len(joints), 1)
        joint = joints[0]
        self.assertTrue(joint.Multiply)
        self.assertEqual(len(joint._Instances), 4)
        # Each pin with the hole it is in, though the hole picked is another
        pairs = sorted((i._Instance, i._Target) for i in joint._Instances)
        self.assertEqual(pairs, [(i, self.holeEdge(x, y)) for i, (x, y) in enumerate(self.holes)])
        self.solveAll()
        self.assertNotMoved()
        # Each pin back in its own hole
        for i, element in enumerate(pins.ElementList):
            element.Placement = plc(100 + 20 * i, 0, 40)
        self.solveAll()
        for element, (x, y) in zip(pins.ElementList, self.holes):
            self.assertTrue(element.Placement.Base.isEqual(App.Vector(x, y, 5), 1e-6), element.Name)

    def testMultipliedOverTargetsPickedByHand(self):
        """NoExpand: the holes picked are the joint's Targets"""
        a = self.asm3()
        plate, pins = self.pinsAndPlate(a)
        pins.ElementCount = 2
        pins.recompute()
        self.constraint(a, 0, "Plate.Face1")
        picked = (self.holeEdge(50, 10), self.holeEdge(10, 50))
        cstr = self.constraint(
            a, 35, "Pins.0.%s" % self.pinEdge, "Plate.%s" % picked[0], "Plate.%s" % picked[1]
        )
        first = cstr.Group[0]
        first.addProperty("App::PropertyInteger", "Count", "", "")
        first.addProperty("App::PropertyPlacementList", "PlacementList", "", "")
        first.Count = 2
        cstr.Multiply = True
        group = a.Proxy.getPartGroup()
        cstr.Group[1].Proxy.setLink(group, "Plate.%s" % picked[0], multiply=True)
        cstr.Group[1].NoExpand = True
        cstr.Group[2].Proxy.setLink(group, "Plate.%s" % picked[1], multiply=True)
        self.assertTrue(cstr.Group[2].NoExpand)
        self.doc.recompute()
        for element, (x, y) in zip(pins.ElementList, ((50, 10), (10, 50))):
            element.Placement = plc(x, y, 5)
        self.convert()
        joints = [j for j in self.joints() if getattr(j, "JointType", "") == "Revolute"]
        self.assertEqual(len(joints), 1)
        joint = joints[0]
        self.assertEqual(list(joint.Targets), list(picked))
        pairs = sorted((i._Instance, i._Target) for i in joint._Instances)
        self.assertEqual(pairs, [(0, picked[0]), (1, picked[1])])
        self.solveAll()
        self.assertNotMoved()

    def testAttachmentOffset(self):
        """A Fixed joint keeping the saved relative placement; its Update
        Offset keeps a new one"""
        import JointObject

        a = self.asm3()
        self.part(a, "Part::Box", "A")
        b = self.part(a, "Part::Box", "B", plc(3, 4, 12, App.Vector(1, 1, 0), 25))
        self.constraint(a, 0, "A.Face1")
        self.constraint(a, 46, "A.Face6", "B.Face5")
        self.convert()
        joint = [j for j in self.joints() if getattr(j, "JointType", "") == "Fixed"][0]
        self.solveAll()
        self.assertNotMoved()
        moved = plc(-5, 8, 20, Z, 70)
        b.Placement = moved
        self.solveAll()
        self.assertFalse(b.Placement.isSame(moved, 1e-6))
        b.Placement = moved
        self.assertTrue(JointObject.updateOffset(joint))
        self.solveAll()
        self.assertTrue(b.Placement.isSame(moved, 1e-6), b.Placement)

    def testRoundTrip(self):
        """Saved and opened again in a fresh process: no Assembly3 code is
        needed, and a solve moves nothing"""
        a = self.asm3()
        self.part(a, "Part::Box", "A")
        self.part(a, "Part::Box", "B", plc(0, 0, 10, Z, 30))
        self.constraint(a, 35, "A.Face6", "B.Face5")
        self.constraint(a, 0, "A.Face1")
        self.convert()
        path = os.path.join(tempfile.mkdtemp(), "ImportAsm3.FCStd")
        self.doc.saveAs(path)
        exe = sys.executable
        if "cmd" not in os.path.basename(exe).lower():
            self.skipTest("needs the console executable to start a fresh session")
        script = path + ".py"
        with open(script, "w") as f:
            f.write(
                "import sys\n"
                "import FreeCAD as App\n"
                "doc = App.openDocument(%r)\n"
                "asm = [o for o in doc.Objects if o.isDerivedFrom('Assembly::AssemblyObject')][0]\n"
                "b = doc.getObject('B')\n"
                "before = b.Placement\n"
                "print('SOLVE=%%d' %% asm.solve())\n"
                "print('MOVED=%%d' %% (not b.Placement.isSame(before, 1e-6)))\n"
                "print('ASM3=%%d' %% len([m for m in sys.modules if 'asm3' in m]))\n" % path
            )
        res = subprocess.run([exe, script], capture_output=True, text=True, timeout=300).stdout
        for line in ("SOLVE=0", "MOVED=0", "ASM3=0"):
            self.assertIn(line, res)

    @unittest.skipUnless(os.environ.get("ASM3_SAMPLE_DIR"), "the essais_asm3 sample is not given")
    def testSample(self):
        src = os.environ["ASM3_SAMPLE_DIR"]
        folder = os.path.join(tempfile.mkdtemp(), "essais_asm3")
        shutil.copytree(src, folder)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(os.path.join(folder, "asm3_essai_methodo.FCStd"))
        report = self.convert()
        self.assertNotIn("broken", str(report))
        self.assertEqual(
            sorted(a.Label for a in self.assemblies), ["Assembly", "sous-Ass_masselotte"]
        )
        parent = [a for a in self.assemblies if a.Label == "Assembly"][0]
        self.assertEqual(len(self.joints(parent)), 9)
        self.assertEqual(self.kinds(parent).count("Cylindrical"), 2)  # the two Colinear
        self.assertNotIn("solve moves", str(report))
        self.solveAll()
        self.assertNotMoved()
