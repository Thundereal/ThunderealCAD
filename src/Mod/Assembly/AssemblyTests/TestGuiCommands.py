# SPDX-License-Identifier: LGPL-2.1-or-later
# /****************************************************************************
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

"""GUI smoke test: every Assembly command, run on a real document.

Each test builds a small assembly, sets up the selection the command needs,
runs it with Gui.runCommand(), drives or accepts the task panel it opens, and
fails if anything reached the console as an error. A call into a Python API
this fork lacks shows up as an AttributeError there, so it fails here instead
of in front of a user.

Modal dialogs are answered by a timer: the ones a test expects are accepted
and counted, any other is rejected and fails the test.
"""

import ast
import os
import tempfile
import unittest
from unittest import mock

import FreeCAD as App

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtCore, QtGui, QtWidgets

    import JointObject
    import UtilsAssembly


def _process(rounds=3):
    for _ in range(rounds):
        QtWidgets.QApplication.processEvents()


class _ConsoleErrors:
    """Collects console errors, and anything that names an exception."""

    MARKERS = ("Traceback", "AttributeError", "Exception", "Error:")

    def __init__(self):
        self.messages = []

    def __call__(self, notifier, msg, level):
        if level in ("Error", "Critical") or any(m in msg for m in self.MARKERS):
            self.messages.append("%s: %s" % (level, msg.strip()))


class _ModalAnswerer:
    """Answers modal dialogs from a timer that also runs in their nested loop.

    'expect' maps a predicate on the widget to an action ('accept' or a
    callable); a modal no predicate claims is rejected and recorded.
    """

    def __init__(self):
        self.expect = []
        self.answered = []
        self.unexpected = []
        self.timer = QtCore.QTimer()
        self.timer.setInterval(100)
        self.timer.timeout.connect(self.tick)
        self.timer.start()

    def stop(self):
        self.timer.stop()

    def tick(self):
        w = QtWidgets.QApplication.activeModalWidget()
        if w is None:
            return
        name = type(w).__name__ + ":" + (w.windowTitle() or w.objectName())
        if w.objectName() == "DocumentRecovery" or w.windowTitle() == "Document Recovery":
            # Left by an earlier run of the application, not by the test
            w.reject()
            return
        for pred, action in self.expect:
            if pred(w):
                self.answered.append(name)
                if action == "accept":
                    w.accept()
                else:
                    action(w)
                return
        text = w.text() if isinstance(w, QtWidgets.QMessageBox) else ""
        self.unexpected.append("%s %r" % (name, text))
        if isinstance(w, QtWidgets.QDialog):
            w.reject()
        else:
            w.close()


@unittest.skipIf(not App.GuiUp, "GUI tests require FreeCAD GUI mode")
class TestGuiCommands(unittest.TestCase):
    """Runs each Assembly command through Gui.runCommand on a live document."""

    maxDiff = None

    DOC = "AsmGuiSmoke"

    # ------------------------------------------------------------ fixture

    def setUp(self):
        self.prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._groundFirst = self.prefs.GetInt("GroundFirstPart", 0)
        self._partInNewFile = self.prefs.GetBool("PartInNewFile", True)
        self._showOnlyParts = self.prefs.GetBool("InsertShowOnlyParts", False)
        self.prefs.SetBool("InsertShowOnlyParts", False)

        Gui.activateWorkbench("AssemblyWorkbench")
        self.errors = _ConsoleErrors()
        App.Console.AttachObserver(self.errors)
        self.modals = _ModalAnswerer()
        self.tmpdir = tempfile.mkdtemp(prefix="asmgui")

        self.doc = App.newDocument(self.DOC)
        App.setActiveDocument(self.doc.Name)
        _process(5)

        self.run_command("Assembly_CreateAssembly")
        self.assembly = UtilsAssembly.activeAssembly()
        self.assertIsNotNone(self.assembly, "Assembly_CreateAssembly left no active assembly")
        self.jointgroup = UtilsAssembly.getJointGroup(self.assembly)

        self.box = self.assembly.newObject("Part::Box", "Box")
        self.cyl1 = self.assembly.newObject("Part::Cylinder", "Cyl1")
        self.cyl1.Placement.Base = App.Vector(30, 0, 0)
        self.cyl2 = self.assembly.newObject("Part::Cylinder", "Cyl2")
        self.cyl2.Placement.Base = App.Vector(0, 30, 0)
        self.doc.recompute()
        _process()

    def tearDown(self):
        try:
            if Gui.Control.activeDialog():
                dlg = Gui.Control.activeTaskDialog()
                if dlg is not None:
                    dlg.reject()
                _process()
                if Gui.Control.activeDialog():
                    Gui.Control.closeDialog()
            JointObject.activeTask = None
            Gui.Selection.clearSelection()
            gdoc = Gui.getDocument(self.doc.Name)
            if gdoc.getInEdit():
                gdoc.resetEdit()
            _process()
        finally:
            self.modals.stop()
            App.Console.DetachObserver(self.errors)
            for name in list(App.listDocuments().keys()):
                App.closeDocument(name)
            _process()
            self.prefs.SetInt("GroundFirstPart", self._groundFirst)
            self.prefs.SetBool("PartInNewFile", self._partInNewFile)
            self.prefs.SetBool("InsertShowOnlyParts", self._showOnlyParts)

    # ------------------------------------------------------------ helpers

    def run_command(self, name):
        cmd = Gui.Command.get(name)
        self.assertIsNotNone(cmd, "%s is not registered" % name)
        _process()
        self.assertTrue(cmd.isActive(), "%s is not active" % name)
        Gui.runCommand(name)
        _process()

    def select(self, obj, sub=""):
        """Select obj (a part inside the assembly) as the 3D view would."""
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, obj.Name + "." + sub)

    def accept_task(self):
        self.assertTrue(Gui.Control.activeDialog(), "no task panel to accept")
        Gui.Control.activeTaskDialog().accept()
        _process()
        self.assertFalse(Gui.Control.activeDialog(), "the task panel did not close on OK")

    def assertClean(self):
        _process()
        self.assertEqual(self.modals.unexpected, [], "unexpected modal dialogs")
        self.assertEqual(self.errors.messages, [], "console errors")

    def proxies(self, cls):
        """The document's objects whose Python proxy is of class 'cls'."""
        return [o for o in self.doc.Objects if type(getattr(o, "Proxy", None)).__name__ == cls]

    def joints(self, jtype=None):
        return [
            j
            for j in self.jointgroup.Group
            if hasattr(j, "JointType") and (jtype is None or j.JointType == jtype)
        ]

    def make_joint(self, command, jtype, with_offset=False):
        """Create a joint through its command between the two cylinders' top circles."""
        self.select(self.cyl1, "Edge1")
        self.select(self.cyl2, "Edge1")
        self.run_command(command)
        task = JointObject.activeTask
        self.assertIsNotNone(task, "%s opened no joint task" % command)
        self.assertEqual(len(task.refs), 2, "the initial selection was not taken")
        if with_offset:
            # The offset buttons open the placement dialog modally
            # (Gui.TaskPlacement); accept it.
            self.modals.expect.append(
                (lambda w: w.findChild(QtWidgets.QPushButton, "selectedVertex"), "accept")
            )
            task.onOffset1Clicked()
            task.onOffset2Clicked()
            self.assertEqual(len(self.modals.answered), 2, "the placement dialog did not open")
        self.accept_task()
        made = self.joints(jtype)
        self.assertEqual(len(made), 1, "no %s joint was made" % jtype)
        j = made[0]
        self.assertEqual(UtilsAssembly.getObject(j.Reference1), self.cyl1)
        self.assertEqual(UtilsAssembly.getObject(j.Reference2), self.cyl2)
        return j

    # ------------------------------------------------------------ assembly

    def test_create_assembly(self):
        # setUp ran it; check what it made
        self.assertEqual(self.assembly.Type, "Assembly")
        self.assertTrue(self.assembly.ViewObject.isInEditMode())
        self.assertIsNotNone(self.jointgroup)
        self.assertClean()

    def test_activate_assembly(self):
        Gui.getDocument(self.doc.Name).resetEdit()
        _process()
        self.assertIsNone(UtilsAssembly.activeAssembly())
        self.run_command("Assembly_ActivateAssembly")
        self.assertEqual(UtilsAssembly.activeAssembly(), self.assembly)
        self.assertClean()

    def test_solve_assembly(self):
        self.run_command("Assembly_SolveAssembly")
        self.assertClean()

    # ------------------------------------------------------------ joints

    def test_toggle_grounded(self):
        self.select(self.box, "Face1")
        self.run_command("Assembly_ToggleGrounded")
        grounded = [j for j in self.jointgroup.Group if hasattr(j, "ObjectToGround")]
        self.assertEqual(len(grounded), 1)
        self.assertEqual(grounded[0].ObjectToGround, self.box)
        # and back
        self.select(self.box, "Face1")
        self.run_command("Assembly_ToggleGrounded")
        grounded = [j for j in self.jointgroup.Group if hasattr(j, "ObjectToGround")]
        self.assertEqual(grounded, [])
        self.assertClean()

    def test_rigid_group(self):
        self.select(self.cyl1, "Face1")
        self.select(self.cyl2, "Face1")
        self.run_command("Assembly_CreateJointRigidGroup")
        groups = [j for j in self.jointgroup.Group if j.Name.startswith("RigidGroupJoint")]
        self.assertEqual(len(groups), 1)
        self.assertClean()

    def test_joint_fixed(self):
        # also the offset buttons, which use Gui.TaskPlacement
        j = self.make_joint("Assembly_CreateJointFixed", "Fixed", with_offset=True)
        self.assertEqual(j.JointType, "Fixed")
        self.assertClean()

    def test_joint_revolute(self):
        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.assertClean()

    def test_joint_cylindrical(self):
        self.make_joint("Assembly_CreateJointCylindrical", "Cylindrical")
        self.assertClean()

    def test_joint_slider(self):
        self.make_joint("Assembly_CreateJointSlider", "Slider")
        self.assertClean()

    def test_joint_ball(self):
        self.make_joint("Assembly_CreateJointBall", "Ball")
        self.assertClean()

    def test_joint_distance(self):
        self.make_joint("Assembly_CreateJointDistance", "Distance")
        self.assertClean()

    def test_joint_parallel(self):
        self.make_joint("Assembly_CreateJointParallel", "Parallel")
        self.assertClean()

    def test_joint_perpendicular(self):
        self.make_joint("Assembly_CreateJointPerpendicular", "Perpendicular")
        self.assertClean()

    def test_joint_angle(self):
        self.make_joint("Assembly_CreateJointAngle", "Angle")
        self.assertClean()

    def test_joint_rack_pinion(self):
        self.make_joint("Assembly_CreateJointRackPinion", "RackPinion")
        self.assertClean()

    def test_joint_screw(self):
        self.make_joint("Assembly_CreateJointScrew", "Screw")
        self.assertClean()

    def test_joint_gears(self):
        self.make_joint("Assembly_CreateJointGears", "Gears")
        self.assertClean()

    def test_joint_belt(self):
        self.make_joint("Assembly_CreateJointBelt", "Belt")
        self.assertClean()

    def test_joint_gear_belt_group(self):
        # The group command runs its first member, Gears
        self.make_joint("Assembly_CreateJointGearBelt", "Gears")
        self.assertClean()

    def test_joint_edit_and_cancel(self):
        j = self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        # Double-click in the tree: edit the joint, then cancel
        self.assertTrue(j.ViewObject.Proxy.doubleClicked(j.ViewObject))
        _process()
        self.assertTrue(Gui.Control.activeDialog(), "editing the joint opened no task")
        Gui.Control.activeTaskDialog().reject()
        _process()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertEqual(len(self.joints("Revolute")), 1)
        self.assertClean()

    # ------------------------------------------------------------ refused Distance

    def freeform(self):
        """A part whose one face is freeform: a plane and it make no Distance (sec 6.11)."""
        from AssemblyTests.TestJoints import freeform

        where = App.Placement(App.Vector(-30, 0, 0), App.Rotation())
        free = freeform(self.assembly, placement=where)["free"]
        self.doc.recompute()
        _process()
        return free

    # What the panel says: the element, and what it is.
    REFUSAL = (
        "the face Face1 of Free is not planar, cylindrical, conical, spherical or "
        "toroidal: it is a B-spline surface"
    )

    def assertRefusal(self, task, reason=REFUSAL):
        self.assertFalse(task.refusalLabel.isHidden(), "the refusal is not shown")
        self.assertIn(reason, task.refusalLabel.text())

    def test_joint_distance_refused_initial_selection(self):
        """Decision 7 and sec 6.11: a plane and a freeform face make no Distance joint."""
        free = self.freeform()
        self.select(self.box, "Face6")
        self.select(free, "Face1")
        self.run_command("Assembly_CreateJointDistance")
        task = JointObject.activeTask
        self.assertIsNotNone(task)
        self.assertEqual(task.refs, [], "the refused picks were taken")
        self.assertRefusal(task)
        self.assertFalse(task.accept())
        self.assertClean()

    def test_joint_distance_refused_pick(self):
        """The refused second pick is dropped; a mapped one is then taken."""
        free = self.freeform()
        self.run_command("Assembly_CreateJointDistance")
        task = JointObject.activeTask
        self.select(self.box, "Face6")
        _process()
        self.assertEqual(len(task.refs), 1)
        self.select(free, "Face1")
        _process()
        self.assertEqual(len(task.refs), 1, "the refused pick was taken")
        self.assertRefusal(task)
        self.assertEqual(task.joint.Reference2, None)
        self.select(self.cyl1, "Face1")  # PlaneCylinder
        _process()
        self.assertEqual(len(task.refs), 2)
        self.assertTrue(task.refusalLabel.isHidden())
        self.accept_task()
        self.assertEqual(len(self.joints("Distance")), 1)
        self.assertClean()

    def test_joint_distance_refused_by_type_change(self):
        """Picks taken for another joint type are refused when it becomes a Distance joint."""
        free = self.freeform()
        self.select(self.box, "Face6")
        self.select(free, "Face1")
        self.run_command("Assembly_CreateJointRevolute")
        task = JointObject.activeTask
        self.assertEqual(len(task.refs), 2)
        task.jForm.jointType.setCurrentIndex(JointObject.JointTypes.index("Distance"))
        _process()
        self.assertRefusal(task)
        self.assertFalse(task.accept())
        self.assertClean()

    def test_joint_distance_refused_kind_in_document_stays_editable(self):
        """A joint of a refused kind already in the document opens and accepts as it is."""
        free = self.freeform()
        j = self.jointgroup.newObject("App::FeaturePython", "Distance")
        JointObject.Joint(j, JointObject.JointTypes.index("Distance"))
        JointObject.ViewProviderJoint(j.ViewObject)
        j.Proxy.setJointConnectors(
            j, [[self.box, ["Face6", "Face6"]], [free, ["Face1", "Face1"]]]
        )
        _process()
        self.assertTrue(j.ViewObject.Proxy.doubleClicked(j.ViewObject))
        _process()
        task = JointObject.activeTask
        self.assertIsNotNone(task)
        self.assertTrue(task.refusalLabel.isHidden())
        self.accept_task()
        picked = {UtilsAssembly.getObject(j.Reference1), UtilsAssembly.getObject(j.Reference2)}
        self.assertEqual(picked, {self.box, free})
        self.assertClean()

    def test_joint_overlay_icon(self):
        """A joint to a part not connected to ground shows Part_Detached."""
        j = self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.assertTrue(hasattr(Gui, "IconPosition"))
        overlays = j.ViewObject.Proxy.getOverlayIcons()
        self.assertEqual(overlays, {Gui.IconPosition.BottomLeft: "Part_Detached"})

        def tree_icon():
            j.ViewObject.signalChangeIcon()
            _process()
            for tree in Gui.getMainWindow().findChildren(QtWidgets.QTreeWidget):
                found = tree.findItems(j.Label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)
                if found:
                    return found[0].icon(0).pixmap(QtCore.QSize(64, 32)).toImage()
            self.fail("joint not in the tree")

        with_overlay = tree_icon()
        with mock.patch.object(type(j.ViewObject.Proxy), "getOverlayIcons", lambda s: {}):
            plain_a = tree_icon()
            plain_b = tree_icon()
        self.assertEqual(plain_a, plain_b, "control: the same icon twice must compare equal")
        self.assertNotEqual(with_overlay, plain_a, "the overlay did not reach the tree icon")
        self.assertClean()

    # ------------------------------------------------------------ the rest

    def test_insert_link(self):
        self.prefs.SetInt("GroundFirstPart", 1)  # no question asked
        source = self.doc.addObject("Part::Box", "Source")
        self.doc.recompute()
        self.run_command("Assembly_InsertLink")
        # A closed panel's widgets may linger until deleted; take the shown one
        lists = [
            w
            for w in Gui.getMainWindow().findChildren(QtWidgets.QTreeWidget, "partList")
            if w.isVisible()
        ]
        self.assertEqual(len(lists), 1)
        partList = lists[0]
        found = partList.findItems(
            source.Label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive
        )
        listed = []

        def walk(item, depth):
            listed.append("  " * depth + item.text(0))
            for i in range(item.childCount()):
                walk(item.child(i), depth + 1)

        for i in range(partList.topLevelItemCount()):
            walk(partList.topLevelItem(i), 0)
        self.assertEqual(len(found), 1, "the source part is not listed: %r" % listed)
        partList.itemClicked.emit(found[0], 0)
        _process()
        self.accept_task()
        links = [o for o in self.assembly.Group if getattr(o, "LinkedObject", None) == source]
        self.assertEqual(len(links), 1)
        grounded = [j for j in self.jointgroup.Group if hasattr(j, "ObjectToGround")]
        self.assertEqual(len(grounded), 1, "the first inserted part was not grounded")
        self.assertClean()

    def test_insert_group(self):
        # The group command runs its first member, Insert Link
        self.run_command("Assembly_Insert")
        self.assertTrue(Gui.Control.activeDialog())
        Gui.Control.activeTaskDialog().reject()
        _process()
        self.assertClean()

    def test_insert_new_part(self):
        self.prefs.SetBool("PartInNewFile", False)
        self.doc.saveAs(os.path.join(self.tmpdir, "smoke.FCStd"))
        self.run_command("Assembly_InsertNewPart")
        self.assertIsNotNone(JointObject.activeTask)
        self.accept_task()
        parts = [
            o
            for o in self.doc.Objects
            if o.isDerivedFrom("App::Part") and not o.isDerivedFrom("Assembly::AssemblyObject")
        ]
        self.assertEqual(len(parts), 1, "no new part was made")
        self.assertClean()

    def test_create_view(self):
        self.run_command("Assembly_CreateView")
        self.accept_task()
        views = self.proxies("ExplodedView")
        self.assertEqual(len(views), 1)
        self.assertClean()

    def test_create_snapshot(self):
        self.run_command("Assembly_CreateSnapshot")
        snaps = self.proxies("Snapshot")
        self.assertEqual(len(snaps), 1)
        self.assertClean()

    def test_create_bom(self):
        self.run_command("Assembly_CreateBom")
        self.accept_task()
        boms = [o for o in self.doc.Objects if o.isDerivedFrom("Spreadsheet::Sheet")]
        self.assertEqual(len(boms), 1)
        self.assertClean()

    def test_create_simulation(self):
        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sims = self.proxies("Simulation")
        self.assertEqual(len(sims), 1)
        self.assertClean()

    def test_export_asmt(self):
        import CommandExportASMT

        # Forward slashes, as Qt's file dialog returns them
        path = os.path.join(self.tmpdir, "smoke.asmt").replace("\\", "/")
        with mock.patch.object(
            CommandExportASMT.QFileDialog, "getSaveFileName", return_value=(path, "")
        ):
            self.run_command("Assembly_ExportASMT")
        self.assertTrue(os.path.exists(path), "no ASMT file written")
        self.assertClean()

    # ------------------------------------------------------------ C++ commands

    def test_select_constraints_and_dofs(self):
        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        for name in (
            "Assembly_SelectConflictingConstraints",
            "Assembly_SelectRedundantConstraints",
            "Assembly_SelectMalformedConstraints",
            "Assembly_SelectComponentsWithDoFs",
        ):
            self.run_command(name)
        self.assertClean()

    def test_select_joints_of_component(self):
        j = self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.select(self.cyl1)
        self.run_command("Assembly_SelectJointsOfComponent")
        selected = [s.Object for s in Gui.Selection.getSelectionEx("", 1)]
        self.assertIn(j, selected)
        self.assertClean()

    def test_link_select_linked(self):
        sub = self.doc.addObject("Assembly::AssemblyObject", "SubAssembly")
        sub.newObject("Assembly::JointGroup", "Joints")
        sub.newObject("Part::Box", "SubBox")
        link = self.assembly.newObject("Assembly::AssemblyLink", "SubLink")
        link.LinkedObject = sub
        self.doc.recompute()
        _process()
        Gui.Selection.clearSelection()
        self.select(link)
        self.run_command("Assembly_LinkSelectLinked")
        selected = [s.Object for s in Gui.Selection.getSelectionEx("", 1)]
        self.assertEqual(selected, [sub])
        self.assertClean()

    # ------------------------------------------------------------ members

    def test_members_named_by_the_workbench(self):
        """The view, view provider and document members the workbench calls exist.

        Collects every name the workbench reads off a receiver whose type is
        known from its name (view, vobj, ...), and checks it on a live object.
        """
        j = self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        gdoc = Gui.getDocument(self.doc.Name)
        live = {
            "view": gdoc.activeView(),
            "Gui.Selection": Gui.Selection,
            "Gui.Control": Gui.Control,
            "gui_doc": gdoc,
            "Gui.ActiveDocument": gdoc,
            "assembly.ViewObject": self.assembly.ViewObject,
            "joint.ViewObject": j.ViewObject,
        }
        receivers = {
            "view": "view",
            "self.view": "view",
            "Gui.Selection": "Gui.Selection",
            "Gui.Control": "Gui.Control",
            "self.gui_doc": "gui_doc",
            "Gui.ActiveDocument": "Gui.ActiveDocument",
            "self.assembly.ViewObject": "assembly.ViewObject",
            "assembly.ViewObject": "assembly.ViewObject",
            "assembly_vobj": "assembly.ViewObject",
            "self.joint.ViewObject": "joint.ViewObject",
            "joint.ViewObject": "joint.ViewObject",
        }
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        missing = set()
        checked = set()
        for fname in sorted(os.listdir(here)):
            if not fname.endswith(".py"):
                continue
            tree = ast.parse(open(os.path.join(here, fname), encoding="utf-8").read())
            for node in ast.walk(tree):
                if not isinstance(node, ast.Attribute):
                    continue
                try:
                    recv = ast.unparse(node.value)
                except Exception:
                    continue
                key = receivers.get(recv)
                if key is None:
                    continue
                if isinstance(node.ctx, ast.Store):
                    continue  # setting a Python attribute is always allowed
                checked.add((key, node.attr))
                if not hasattr(live[key], node.attr):
                    missing.add("%s.%s (%s)" % (key, node.attr, fname))
        # 42 when written; a much smaller count means the scan broke
        self.assertGreater(len(checked), 30)
        self.assertEqual(sorted(missing), [])
        self.assertClean()
