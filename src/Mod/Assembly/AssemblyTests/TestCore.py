# SPDX-License-Identifier: LGPL-2.1-or-later
# /****************************************************************************
#                                                                           *
#    Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
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
# ***************************************************************************/

import math
import FreeCAD as App
import Part
import os
import tempfile
import unittest

import UtilsAssembly
import JointObject


def _msg(text, end="\n"):
    """Write messages to the console including the line ending."""
    App.Console.PrintMessage(text + end)


class AssemblyTestBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """setUpClass()...
        This method is called upon instantiation of this test class.  Add code and objects here
        that are needed for the duration of the test() methods in this class.  In other words,
        set up the 'global' test environment here; use the `setUp()` method to set up a 'local'
        test environment.
        This method does not have access to the class `self` reference, but it
        is able to call static methods within this same class.
        """
        pass

    @classmethod
    def tearDownClass(cls):
        """tearDownClass()...
        This method is called prior to destruction of this test class.  Add code and objects here
        that cleanup the test environment after the test() methods in this class have been executed.
        This method does not have access to the class `self` reference.  This method
        is able to call static methods within this same class.
        """
        pass

    # Setup and tear down methods called before and after each unit test
    def setUp(self):
        """setUp()...
        This method is called prior to each `test()` method.  Add code and objects here
        that are needed for multiple `test()` methods.
        """
        doc_name = self.__class__.__name__
        if App.ActiveDocument:
            if App.ActiveDocument.Name != doc_name:
                App.newDocument(doc_name)
        else:
            App.newDocument(doc_name)
        App.setActiveDocument(doc_name)
        self.doc = App.ActiveDocument

        self.assembly = App.ActiveDocument.addObject("Assembly::AssemblyObject", "Assembly")
        if self.assembly:
            self.jointgroup = self.assembly.newObject("Assembly::JointGroup", "Joints")

        _msg("  Temporary document '{}'".format(self.doc.Name))

    def tearDown(self):
        """tearDown()...
        This method is called after each test() method. Add cleanup instructions here.
        Such cleanup instructions will likely undo those in the setUp() method.
        """
        App.closeDocument(self.doc.Name)


class TestCore(AssemblyTestBase):
    def test_component_count_for_link_array(self):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("App::Link", "Array")
        array.LinkedObject = source
        array.ElementCount = 3
        array.ShowElement = False
        self.doc.recompute()
        self.assertEqual(len(array.ElementList), 0)
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 3)

        array.ShowElement = True
        self.doc.recompute()
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 3)
        array.setElementVisible("1", False)
        self.doc.recompute()
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 2)
        array.setElementVisible("1", True)
        self.doc.recompute()
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 3)

    def test_generated_array_is_one_component(self):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("Part::LinkArrayLinear", "Array")
        array.LinkedObject = source
        array.Occurrences = 3
        self.doc.recompute()
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 1)
        self.assertEqual(UtilsAssembly.getSubMovingParts(array, False), [array])
        self.assertEqual(UtilsAssembly.getObject((array, ["1.Face1"])), array)

    def test_suppressed_link_elements_are_not_movable(self):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("App::Link", "Array")
        array.LinkedObject = source
        array.ElementCount = 3
        self.doc.recompute()
        element = array.ElementList[1]
        array.setElementVisible("1", False)
        self.doc.recompute()
        self.assertNotIn(element, UtilsAssembly.getMovablePartsWithin(array))
        array.setElementVisible("1", True)
        self.doc.recompute()
        self.assertIn(element, UtilsAssembly.getMovablePartsWithin(array))

    def test_assembly_link_synchronizes_element_suppression(self):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("App::Link", "Array")
        array.LinkedObject = source
        array.ElementCount = 3
        array.setElementVisible("1", False)
        parent = self.doc.addObject("Assembly::AssemblyObject", "ParentAssembly")
        instance = parent.newObject("Assembly::AssemblyLink", "Instance")
        instance.LinkedObject = self.assembly
        self.doc.recompute()
        local_array = next(obj for obj in instance.Group if obj.TypeId == "App::Link")
        self.assertEqual(local_array.isElementVisible("1"), 0)
        array.setElementVisible("1", True)
        instance.touch()
        self.doc.recompute()
        self.assertEqual(local_array.isElementVisible("1"), 1)

    def test_assembly_link_maps_generated_array_joint(self):
        self._check_generated_array_joint("Face1")

    def test_assembly_link_maps_generated_array_whole_element_joint(self):
        self._check_generated_array_joint("")

    def _check_generated_array_joint(self, sub):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("Part::LinkArrayLinear", "Array")
        array.LinkedObject = source
        array.Occurrences = 3
        array.ShowElement = True
        self.doc.recompute()
        joint = self.jointgroup.newObject("App::FeaturePython", "Joint")
        JointObject.Joint(joint, 0)
        joint.Reference1 = (array.ElementList[1], [sub])
        joint.Reference2 = (array.ElementList[2], ["Face1"])
        parent = self.doc.addObject("Assembly::AssemblyObject", "ParentAssembly")
        instance = parent.newObject("Assembly::AssemblyLink", "Instance")
        instance.LinkedObject = self.assembly
        instance.Rigid = False
        self.doc.recompute()
        local_array = next(obj for obj in instance.Group if obj.TypeId == "App::Link")
        local_group = next(obj for obj in instance.Group if obj.TypeId == "Assembly::JointGroup")
        local_joint = local_group.Group[0]
        self.assertEqual(local_joint.Reference1, (local_array, ["1." + sub]))
        self.assertIsNotNone(local_array.getSubObject("1." + sub))

    def test_create_assembly(self):
        """Create an assembly."""
        operation = "Create Assembly Object"
        _msg("  Test '{}'".format(operation))
        self.assertTrue(self.assembly, "'{}' failed".format(operation))

    def test_create_jointGroup(self):
        """Create a joint group in an assembly."""
        operation = "Create JointGroup Object"
        _msg("  Test '{}'".format(operation))
        self.assertTrue(self.jointgroup, "'{}' failed".format(operation))

    def test_create_joint(self):
        """Create a joint in an assembly."""
        operation = "Create Joint Object"
        _msg("  Test '{}'".format(operation))

        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        self.assertTrue(joint, "'{}' failed (FeaturePython creation failed)".format(operation))
        JointObject.Joint(joint, 0)

        self.assertTrue(hasattr(joint, "JointType"), "'{}' failed".format(operation))

    def test_create_grounded_joint(self):
        """Create a grounded joint in an assembly."""
        operation = "Create Grounded Joint Object"
        _msg("  Test '{}'".format(operation))

        groundedjoint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        self.assertTrue(
            groundedjoint, "'{}' failed (FeaturePython creation failed)".format(operation)
        )

        box = self.assembly.newObject("Part::Box", "Box")

        JointObject.GroundedJoint(groundedjoint, box)

        self.assertTrue(
            hasattr(groundedjoint, "ObjectToGround"),
            "'{}' failed: No attribute 'ObjectToGround'".format(operation),
        )
        self.assertTrue(
            groundedjoint.ObjectToGround == box,
            "'{}' failed: ObjectToGround not set correctly.".format(operation),
        )

    def test_toggle_grounded_joint(self):
        """test grounding and ungrounding a part, added because of github.com/freecad/freecad/issues/28440"""
        operation = "Toggle Grounded Joint"
        _msg("  Test '{}'".format(operation))

        box = self.assembly.newObject("Part::Box", "Box")

        # ground the part
        groundedjoint = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(groundedjoint, box)
        self.doc.recompute()

        # verify grounded
        self.assertTrue(
            hasattr(groundedjoint, "ObjectToGround"),
            "'{}' failed: No attribute 'ObjectToGround'".format(operation),
        )
        self.assertEqual(
            groundedjoint.ObjectToGround,
            box,
            "'{}' failed: ObjectToGround not set correctly".format(operation),
        )

        # unground the part
        self.doc.removeObject(groundedjoint.Name)
        self.doc.recompute()

        # verify no grounded joints remain in this part
        for joint in self.jointgroup.Group:
            if hasattr(joint, "ObjectToGround"):
                self.assertNotEqual(
                    joint.ObjectToGround,
                    box,
                    "'{}' failed: part still grounded after toggle".format(operation),
                )

    def test_find_placement(self):
        """Test find placement of joint."""
        operation = "Find placement"
        _msg("  Test '{}'".format(operation))

        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        JointObject.Joint(joint, 0)

        L = 2
        W = 3
        H = 7
        box = self.assembly.newObject("Part::Box", "Box")
        box.Length = L
        box.Width = W
        box.Height = H
        box.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(15, 25, 35))

        # Step 0 : box with placement. No element selected
        ref = [self.assembly, [box.Name + ".", box.Name + "."]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(), App.Rotation())
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 0".format(operation))

        # Step 1 : box with placement. Face + Vertex
        ref = [self.assembly, [box.Name + ".Face6", box.Name + ".Vertex7"]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(L, W, H), App.Rotation())
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 1".format(operation))

        # Step 2 : box with placement. Edge + Vertex
        ref = [self.assembly, [box.Name + ".Edge8", box.Name + ".Vertex8"]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(L, W, 0), App.Rotation(0, -90, 270))
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 2".format(operation))

        # Step 3 : box with placement. Vertex
        ref = [self.assembly, [box.Name + ".Vertex3", box.Name + ".Vertex3"]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(0, W, H), App.Rotation())
        _msg("  plc '{}'".format(plc))
        _msg("  targetPlc '{}'".format(targetPlc))
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 3".format(operation))

        # Step 4 : box with placement. Face
        ref = [self.assembly, [box.Name + ".Face2", box.Name + ".Face2"]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(L, W / 2, H / 2), App.Rotation(0, -90, 180))
        _msg("  plc '{}'".format(plc))
        _msg("  targetPlc '{}'".format(targetPlc))
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 4".format(operation))

    def test_solve_assembly(self):
        """Test solving an assembly."""
        operation = "Solve assembly"
        _msg("  Test '{}'".format(operation))

        box = self.assembly.newObject("Part::Box", "Box")
        box.Length = 10
        box.Width = 10
        box.Height = 10
        box.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(15, 25, 35))

        box2 = self.assembly.newObject("Part::Box", "Box")
        box2.Length = 10
        box2.Width = 10
        box2.Height = 10
        box2.Placement = App.Placement(App.Vector(40, 50, 60), App.Rotation(45, 55, 65))

        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, box2)

        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        JointObject.Joint(joint, 0)

        refs = [
            [box2, ["Face6", "Vertex7"]],
            [box, ["Face6", "Vertex7"]],
        ]

        joint.Proxy.setJointConnectors(joint, refs)

        self.assertTrue(box.Placement.isSame(box2.Placement, 1e-6), "'{}'".format(operation))

    def test_rack_pinion_with_slider_offset(self):
        """Rack and pinion joint whose rack slider has a yaw offset, see
        github.com/freecad/freecad/issues/17563"""
        operation = "Rack and pinion with slider offset"
        _msg("  Test '{}'".format(operation))

        ground = self.assembly.newObject("Part::Box", "Ground")
        ground.Length = 200
        ground.Width = 200
        ground.Height = 10

        rack = self.assembly.newObject("Part::Box", "Rack")
        rack.Length = 10
        rack.Width = 100
        rack.Height = 10
        rack.Placement.Base = App.Vector(50, 0, 10)

        pinion = self.assembly.newObject("Part::Cylinder", "Pinion")
        pinion.Radius = 10
        pinion.Height = 10
        pinion.Placement.Base = App.Vector(30, 50, 10)
        self.doc.recompute()

        # Attach to real faces, but specify the JCS explicitly so the test does
        # not depend on OpenCASCADE's orientation of those faces.
        along_rack = App.Rotation(App.Vector(1, 0, 0), -90)
        yaw_offset = App.Rotation(App.Vector(0, 0, 1), -90)

        slider = self.jointgroup.newObject("App::FeaturePython", "Slider")
        JointObject.Joint(slider, JointObject.JointTypes.index("Slider"))
        slider.Detach1 = True
        slider.Detach2 = True
        slider.Reference1 = (ground, ["Face1"])
        slider.Reference2 = (rack, ["Face1"])
        slider.Placement1 = App.Placement(App.Vector(), along_rack)
        slider.Placement2 = App.Placement(App.Vector(), along_rack * yaw_offset)

        rackPinion = self.jointgroup.newObject("App::FeaturePython", "RackPinion")
        JointObject.Joint(rackPinion, JointObject.JointTypes.index("RackPinion"))
        rackPinion.Detach1 = True
        rackPinion.Reference1 = (rack, ["Face1"])
        rackPinion.Reference2 = (pinion, ["Face1"])
        rackPinion.Placement1 = App.Placement(App.Vector(), along_rack)
        rackPinion.Distance = 10

        self.doc.recompute()

        slider_axis = UtilsAssembly.getJcsGlobalPlc(
            slider.Placement2, slider.Reference2
        ).Rotation.multVec(App.Vector(0, 0, 1))
        rack_axis = UtilsAssembly.getJcsGlobalPlc(
            rackPinion.Placement1, rackPinion.Reference1
        ).Rotation.multVec(App.Vector(0, 0, 1))
        self.assertLess(slider_axis.cross(rack_axis).Length, 1e-7)

        # The rack and pinion joint must reach the solver: it is silently dropped when
        # the rack cannot be identified from its slider. Look for its pitchRadius field
        # rather than its type name, which the solver derives from the compiler-specific
        # typeid name and is only readable with MSVC.
        with tempfile.TemporaryDirectory() as temp_dir:
            fileName = os.path.join(temp_dir, "rackPinion.asmt")
            self.assembly.exportAsASMT(fileName)
            with open(fileName) as asmt:
                content = asmt.read()
        self.assertIn(
            "pitchRadius",
            content,
            "'{}' failed - joint not exported; slider state: {}, rack-pinion state: {}; "
            "exported assembly: {}".format(
                operation, slider.State, rackPinion.State, content[-2500:]
            ),
        )


class TestSolverBackend(AssemblyTestBase):
    """Which solver backend an assembly is solved with (docs/AssemblySolverInterface.md sec 4.8)."""

    def make_pair(self):
        box = self.assembly.newObject("Part::Box", "Box")
        box.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(15, 25, 35))
        box2 = self.assembly.newObject("Part::Box", "Box")
        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, box2)
        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        JointObject.Joint(joint, 0)
        joint.Proxy.setJointConnectors(joint, [[box2, ["Face6", "Vertex7"]], [box, ["Face6", "Vertex7"]]])
        return box, box2

    def test_available_solvers(self):
        import AssemblyApp

        names = AssemblyApp.getAvailableSolvers()
        self.assertIsInstance(names, list)
        self.assertIn("Ondsel", names)

    def test_property_empty_by_default(self):
        self.assertIn("SolverBackend", self.assembly.PropertiesList)
        self.assertEqual(self.assembly.SolverBackend, "")

    def test_unknown_backend_falls_back(self):
        """A name nobody registered solves with the default backend, as an empty one does."""
        box, box2 = self.make_pair()
        start = App.Placement(App.Vector(1, 2, 3), App.Rotation())
        results = []
        for name in ("", "NoSuchSolver"):
            self.assembly.SolverBackend = name
            box.Placement = start
            self.assertEqual(self.assembly.solve(), 0)
            results.append((App.Placement(box.Placement), self.assembly.getSolveStatus()))
        self.assertTrue(results[0][0].isSame(results[1][0], 1e-12))
        self.assertEqual(results[0][1], results[1][1])
        self.assertEqual(results[1][1]["SolverStatus"], 0)

    def test_same_start_same_solution_bit_for_bit(self):
        """OndselSolver's answer is a function of the start alone.

        A chain of boxes, Fixed and Revolute joints alternating (free
        rotations, so the free parts' poses depend on the solve), solved from
        the same start twice: every placement equal bit for bit. Its sparse
        LU orders the columns by the matrix's pattern only.
        """
        boxes = []
        for i in range(12):
            box = self.assembly.newObject("Part::Box", "Box")
            boxes.append(box)
        self.doc.recompute()  # the shapes the references name
        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, boxes[0])
        for i in range(1, len(boxes)):
            joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
            JointObject.Joint(joint, i % 2)  # Fixed, Revolute
            joint.Proxy.setJointConnectors(
                joint, [[boxes[i - 1], ["Face2", "Face2"]], [boxes[i], ["Face1", "Face1"]]]
            )

        def start():
            for i, box in enumerate(boxes):
                box.Placement = App.Placement(
                    App.Vector(12.0 * i + i % 3, 1.5 * (i % 4), 0.7 * (i % 5)),
                    App.Rotation(App.Vector(0, 0, 1), (7 * i) % 20),
                )

        def solved():
            self.assertEqual(self.assembly.solve(), 0)
            values = []
            for box in boxes:
                plm = box.Placement
                values += [plm.Base.x, plm.Base.y, plm.Base.z] + list(plm.Rotation.Q)
            return [float(v).hex() for v in values]

        start()
        first = solved()
        start()
        self.assertEqual(solved(), first)
        revolutes = sum(i % 2 for i in range(1, len(boxes)))
        self.assertEqual(self.assembly.getSolveStatus()["DoF"], revolutes)

    def test_rigid_groups(self):
        """Bodies the joints hold rigidly leave the solve (docs/AssemblySolverInterface.md sec 6.21, 6.23).

        A box Fixed to the grounded one is placed by its joint's frames,
        exactly; a box held by a Fixed and a Revolute joint is not taken out,
        and the redundancy is reported as the solver finds it. A box Fixed to
        one a Revolute holds, off its joint at the start, is solved with the
        rest; once its joint holds (the next solve) it moves with that box,
        one body that weighs two.
        """
        g, a, b, c, d = [self.assembly.newObject("Part::Box", "Box") for i in range(5)]
        self.doc.recompute()  # the shapes the references name
        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, g)

        def joint(kind, one, two):
            j = self.jointgroup.newObject("App::FeaturePython", "testJoint")
            JointObject.Joint(j, kind)
            j.Proxy.setJointConnectors(j, [[one, ["Face2", "Face2"]], [two, ["Face1", "Face1"]]])
            return j

        hinge = joint(1, g, a)  # Revolute
        arm = joint(0, a, b)  # Fixed
        placed = joint(0, g, c)  # Fixed
        held = joint(0, c, d)  # Fixed
        extra = joint(1, c, d)  # Revolute, redundant with it
        for i, box in enumerate((a, b, c, d)):
            box.Placement = App.Placement(
                App.Vector(12.0 * (i + 1), 3.0 * i, -2.0), App.Rotation(5.0 * i, 10.0, -7.0)
            )
        self.assertEqual(self.assembly.solve(), 0)

        residuals = self.assembly.getSolverResiduals()
        for j in (hinge, arm, placed):
            self.assertLess(residuals[j.Name], 1e-9, j.Name)
        status = self.assembly.getSolveStatus()
        self.assertEqual(status["DoF"], 1)
        self.assertTrue(set(status["Redundant"]) & {held.Name, extra.Name}, status)
        for j in (hinge, arm, placed):
            self.assertNotIn(j.Name, status["Redundant"])

        metric = self.assembly.getSolverMetric()
        self.assertNotEqual(metric[a.Name][2], metric[b.Name][2], "b off its joint: alone")
        self.assertEqual(self.assembly.solve(), 0)
        metric = self.assembly.getSolverMetric()
        self.assertEqual(metric[a.Name][2], metric[b.Name][2], "a and b one body")
        self.assertEqual(metric[a.Name][3], 2.0, "the group weighs two bodies")
        self.assertNotEqual(metric[a.Name][2], metric[c.Name][2])
        self.assertNotEqual(metric[c.Name][2], metric[d.Name][2], "d not taken out")

    def test_rigid_pair(self):
        """Two bodies rigid only together are placed together (docs/AssemblySolverInterface.md sec 6.22).

        Two boxes on Revolute joints to the grounded one, about crossed axes,
        and a Distance and an Angle between them at their values: neither box
        is held alone, the two are. Both are placed before the solve (a
        placed body's metric is a unit inertia), every joint satisfied, DoF 0.
        The joints are made without a solve each (SolveInJointCreation off),
        so that the values set are those of the turned boxes.
        """
        params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        saved = params.GetBool("SolveInJointCreation", True)
        params.SetBool("SolveInJointCreation", False)
        self.addCleanup(params.SetBool, "SolveInJointCreation", saved)
        g, a, b = [self.assembly.newObject("Part::Box", "Box") for i in range(3)]
        self.doc.recompute()  # the shapes the references name
        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, g)

        def joint(kind, one, sub1, two, sub2):
            j = self.jointgroup.newObject("App::FeaturePython", "testJoint")
            JointObject.Joint(j, JointObject.JointTypes.index(kind))
            j.Proxy.setJointConnectors(j, [[one, [sub1, sub1]], [two, [sub2, sub2]]])
            return j

        joints = [joint("Revolute", g, "Face2", a, "Face1"), joint("Revolute", g, "Face4", b, "Face3")]
        self.assertEqual(self.assembly.solve(), 0)
        turn = App.Rotation(App.Vector(1, 0, 0), 20)
        a.Placement = App.Placement(App.Vector(), turn, App.Vector(10, 5, 5)).multiply(a.Placement)
        turn = App.Rotation(App.Vector(0, 1, 0), 35)
        b.Placement = App.Placement(App.Vector(), turn, App.Vector(5, 10, 5)).multiply(b.Placement)
        self.doc.recompute()
        distance = joint("Distance", a, "Vertex8", b, "Vertex8")
        distance.Distance = (a.Shape.Vertexes[7].Point - b.Shape.Vertexes[7].Point).Length
        angle = joint("Angle", a, "Face6", b, "Face6")
        normals = a.Shape.Faces[5].normalAt(0, 0), b.Shape.Faces[5].normalAt(0, 0)
        angle.Angle = math.degrees(normals[0].getAngle(normals[1]))
        joints += [distance, angle]
        self.doc.recompute()

        self.assertEqual(self.assembly.solve(), 0)
        status = self.assembly.getSolveStatus()
        self.assertEqual(status["DoF"], 0)
        self.assertEqual(status["Redundant"], [])
        residuals = self.assembly.getSolverResiduals()
        for j in joints:
            self.assertLess(residuals[j.Name], 1e-9, j.Name)
        metric = self.assembly.getSolverMetric()
        for box in (a, b):
            self.assertEqual(metric[box.Name][1], 1.0, "%s placed" % box.Name)

    def test_reference_backend(self):
        """The Reference backend, chosen by the property, solves as OndselSolver does.

        A revolute leaves one rotation free, so the two placements need not
        be the same; each satisfies the joint by the neutral checker, and the
        statuses are equal (docs/AssemblySolverInterface.md sec 6.13).
        """
        import AssemblyApp

        self.assertIn("Reference", AssemblyApp.getAvailableSolvers())
        box = self.assembly.newObject("Part::Box", "Box")
        box2 = self.assembly.newObject("Part::Box", "Box")
        self.doc.recompute()  # the shapes the references name
        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, box2)
        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        JointObject.Joint(joint, 1)  # Revolute
        joint.Proxy.setJointConnectors(joint, [[box2, ["Face6", "Vertex7"]], [box, ["Face6", "Vertex7"]]])
        start = App.Placement(App.Vector(1, 2, 3), App.Rotation(10, 20, 30))
        statuses = {}
        for name in ("Ondsel", "Reference"):
            self.assembly.SolverBackend = name
            box.Placement = start
            self.assertEqual(self.assembly.solve(), 0, name)
            residuals = self.assembly.getSolverResiduals()
            self.assertEqual(sorted(residuals), [joint.Name], name)
            self.assertLess(residuals[joint.Name], 1e-9, name)
            self.assertFalse(box.Placement.isSame(start, 1e-3), name)
            statuses[name] = self.assembly.getSolveStatus()
        self.assertEqual(statuses["Ondsel"], statuses["Reference"])
        self.assertEqual(statuses["Reference"]["DoF"], 1)

    def test_chrono_backend(self):
        """The Chrono backend, when built, chosen by the property or the preference (sec 6.15).

        A revolute solves as OndselSolver does (statuses equal, the joint
        satisfied by the neutral checker); an Angle of 30 degrees, which
        Chrono has no link for, is held by the neutral equations (sec 6.17)
        and solves as OndselSolver does too, its link named in the export.
        """
        import AssemblyApp

        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.skipTest("built without BUILD_ASSEMBLY_CHRONO")
        box = self.assembly.newObject("Part::Box", "Box")
        box2 = self.assembly.newObject("Part::Box", "Box")
        self.doc.recompute()  # the shapes the references name
        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, box2)
        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        JointObject.Joint(joint, 1)  # Revolute
        joint.Proxy.setJointConnectors(joint, [[box2, ["Face6", "Vertex7"]], [box, ["Face6", "Vertex7"]]])
        start = App.Placement(App.Vector(1, 2, 3), App.Rotation(10, 20, 30))
        statuses = {}
        for name in ("Ondsel", "Chrono"):
            self.assembly.SolverBackend = name
            box.Placement = start
            self.assertEqual(self.assembly.solve(), 0, name)
            residuals = self.assembly.getSolverResiduals()
            self.assertEqual(sorted(residuals), [joint.Name], name)
            self.assertLess(residuals[joint.Name], 1e-9, name)
            self.assertFalse(box.Placement.isSame(start, 1e-3), name)
            statuses[name] = self.assembly.getSolveStatus()
        self.assertEqual(statuses["Ondsel"], statuses["Chrono"])
        self.assertEqual(statuses["Chrono"]["DoF"], 1)

        # The preference, when the property is empty: the export is then
        # Chrono's own JSON archive of its system.
        params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        saved = params.GetString("SolverBackend", "")
        tmp = tempfile.mkdtemp(prefix="fc_solver_chrono_")
        path = os.path.join(tmp, "chrono.json")
        try:
            params.SetString("SolverBackend", "Chrono")
            self.assembly.SolverBackend = ""
            box.Placement = start
            self.assertEqual(self.assembly.solve(), 0)
            self.assembly.exportAsASMT(path)
            with open(path) as f:
                text = f.read()
            self.assertTrue(text.lstrip().startswith("{"), text[:80])
            self.assertIn("ChLinkMateRevolute", text)
        finally:
            if saved:
                params.SetString("SolverBackend", saved)
            else:
                params.RemString("SolverBackend")
            if os.path.exists(path):
                os.remove(path)
            os.rmdir(tmp)

        # A kind Chrono has no link for: the neutral equations hold it.
        joint.JointType = "Angle"
        joint.Angle = 30.0
        statuses = {}
        for name in ("Ondsel", "Chrono"):
            self.assembly.SolverBackend = name
            box.Placement = start
            self.assertEqual(self.assembly.solve(), 0, name)
            residuals = self.assembly.getSolverResiduals()
            self.assertLess(residuals[joint.Name], 1e-9, name)
            statuses[name] = self.assembly.getSolveStatus()
        self.assertEqual(statuses["Ondsel"], statuses["Chrono"])
        tmp = tempfile.mkdtemp(prefix="fc_solver_chrono_")
        path = os.path.join(tmp, "chrono.json")
        try:
            self.assembly.exportAsASMT(path)
            with open(path) as f:
                self.assertIn("AssemblyNeutralLink", f.read())
        finally:
            if os.path.exists(path):
                os.remove(path)
            os.rmdir(tmp)

    def test_property_is_saved(self):
        self.make_pair()
        self.assembly.SolverBackend = "Ondsel"
        path = os.path.join(tempfile.mkdtemp(prefix="fc_solver_backend_"), "backend.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        try:
            self.assertEqual(self.doc.getObject("Assembly").SolverBackend, "Ondsel")
        finally:
            os.remove(path)
            os.rmdir(os.path.dirname(path))


class TestSubNames(AssemblyTestBase):
    """Subname parsing, which has to cope with this fork's mapped element names.

    A mapped (topological-name-proof) element name is prefixed with ';' and is
    followed by the indexed name it currently resolves to, so a subname carrying
    one has an extra dot separated segment that is not an object name.
    """

    def test01_splitSubName(self):
        operation = "Split subname"
        _msg("  Test '{}'".format(operation))

        for sub, expected in [
            ("Assembly1.Box.Edge16", ("Assembly1.Box.", "Edge16")),
            ("Assembly1.Box.", ("Assembly1.Box.", "")),
            ("Edge16", ("", "Edge16")),
            ("", ("", "")),
            ("Box.;Face3;:H1,F.Face6", ("Box.", ";Face3;:H1,F.Face6")),
            (";Face3;:H1,F.Face6", ("", ";Face3;:H1,F.Face6")),
        ]:
            self.assertEqual(UtilsAssembly.splitSubName(sub), expected, "'{}'".format(operation))

    def test02_getObjsNamesAndElement(self):
        operation = "Get object names and element"
        _msg("  Test '{}'".format(operation))

        for sub, expected in [
            ("A1.A2.Box.Edge16", (["Assembly", "A1", "A2", "Box"], "Edge16")),
            ("A1.Box.", (["Assembly", "A1", "Box"], "")),
            ("Edge16", (["Assembly"], "Edge16")),
            # The mapped name belongs to the element, not to the object path.
            ("A1.Box.;Face3;:H1,F.Face6", (["Assembly", "A1", "Box"], "Face6")),
        ]:
            self.assertEqual(
                UtilsAssembly.getObjsNamesAndElement("Assembly", sub),
                expected,
                "'{}'".format(operation),
            )

    def test03_getElementName(self):
        operation = "Get element name"
        _msg("  Test '{}'".format(operation))

        for sub, expected in [
            ("A.Box.Edge16", "Edge16"),
            ("A.Box.;Face3;:H1,F.Face6", "Face6"),
            ("A.Box.", ""),
            ("A.LCS.X", ""),  # datums have no element name
        ]:
            self.assertEqual(
                UtilsAssembly.getElementName(sub), expected, "'{}'".format(operation)
            )

    def test04_swapElNameInSubname(self):
        operation = "Swap element name in subname"
        _msg("  Test '{}'".format(operation))

        for sub, expected in [
            ("assembly.box.Edge1", "assembly.box.Vertex2"),
            # The stale mapped name must go with the element it named.
            ("assembly.box.;Face3;:H1,F.Face6", "assembly.box.Vertex2"),
            ("Edge1", "Vertex2"),
        ]:
            self.assertEqual(
                UtilsAssembly.swapElNameInSubname(sub, "Vertex2"),
                expected,
                "'{}'".format(operation),
            )
