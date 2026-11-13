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
import sys
import tempfile
import time
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
            for module in (
                "CommandCreateSpring",
                "CommandCreateLoad",
                "CommandCreateBushing",
                "CommandCreateContact",
                "CommandCreateFlexibleBody",
            ):
                if module in sys.modules:
                    sys.modules[module].activeTask = None
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
        # A joint made in an assembly is the C++ one, its sides element bindings
        self.assertTrue(JointObject.isTyped(j))
        self.assertEqual(UtilsAssembly.getObject(JointObject.sideRef(j, 1)), self.cyl1)
        self.assertEqual(UtilsAssembly.getObject(JointObject.sideRef(j, 2)), self.cyl2)
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
        self.assertEqual(grounded[0].TypeId, "Assembly::GroundedJointPython")
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
        self.assertEqual(groups[0].TypeId, "Assembly::RigidGroupPython")
        self.assertEqual(set(groups[0].ObjectsToRigidGroup), {self.cyl1, self.cyl2})
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
        self.assertEqual(JointObject.sideRef(task.joint, 2), None)
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

        trees = Gui.getMainWindow().findChildren(QtWidgets.QTreeWidget)

        def settle():
            """The tree makes new items and icons in a status pass its timer
            defers until changes stop (for at most five of its timeouts):
            wait, the event loop turning, until no pass is pending"""
            timers = [
                t
                for tree in trees
                for t in tree.findChildren(QtCore.QTimer)
                if t.property("firstAsked") is not None
            ]
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                _process()
                if not any(t.isActive() for t in timers):
                    return
                time.sleep(0.02)

        def tree_icon():
            j.ViewObject.signalChangeIcon()
            # The tree makes a group's items when it is expanded, a level at
            # a time: the joint's is not there under a collapsed Joints group
            for _ in range(6):
                settle()
                for tree in trees:
                    found = tree.findItems(
                        j.Label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive
                    )
                    if found:
                        return found[0].icon(0).pixmap(QtCore.QSize(64, 32)).toImage()
                for tree in trees:
                    tree.expandAll()

            def walk(item, depth=1):
                out = ["  " * depth + item.text(0)]
                for i in range(item.childCount()):
                    out += walk(item.child(i), depth + 1)
                return out

            seen = []
            for tree in trees:
                seen.append("%s visible %s" % (tree.objectName(), tree.isVisible()))
                for i in range(tree.topLevelItemCount()):
                    seen += walk(tree.topLevelItem(i))
            self.fail("joint %r not in the tree:\n%s" % (j.Label, "\n".join(seen)))

        with_overlay = tree_icon()
        with mock.patch.object(type(j.ViewObject.Proxy), "getOverlayIcons", lambda s: {}):
            plain_a = tree_icon()
            plain_b = tree_icon()
        self.assertEqual(plain_a, plain_b, "control: the same icon twice must compare equal")
        self.assertNotEqual(with_overlay, plain_a, "the overlay did not reach the tree icon")
        self.assertClean()

    def test_tree_keeps_up_with_steady_changes(self):
        """The tree makes new items in a status pass that waits for changes
        to stop -- but at most five of its timeouts: changes that go on (a
        script, a playback) do not keep a new object out of it"""
        timeout = App.ParamGet("User parameter:BaseApp/Preferences/TreeView").GetInt(
            "StatusTimeout", 100
        )
        trees = [
            t for t in Gui.getMainWindow().findChildren(QtWidgets.QTreeWidget) if t.isVisible()
        ]
        self.doc.addObject("Part::Box", "Steady")
        start = time.monotonic()
        deadline = start + max(0.2, 12 * timeout / 1000.0)
        seen = None
        while time.monotonic() < deadline:
            # A change every turn of the event loop
            self.box.Length = self.box.Length.Value + 0.001
            _process()
            if any(
                t.findItems("Steady", QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)
                for t in trees
            ):
                seen = time.monotonic() - start
                break
            time.sleep(0.02)
        self.assertIsNotNone(seen, "the tree made no item while the changes went on")
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
        self.assertEqual(views[0].TypeId, "Assembly::ExplodedViewPython")
        self.assertClean()

    def test_create_snapshot(self):
        self.run_command("Assembly_CreateSnapshot")
        snaps = self.proxies("Snapshot")
        self.assertEqual(len(snaps), 1)
        self.assertEqual(snaps[0].TypeId, "Assembly::SnapshotPython")
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
        self.assertEqual(sims[0].TypeId, "Assembly::SimulationPython")
        self.assertClean()

    def test_results_to_spreadsheet(self):
        """The Results box's To Spreadsheet button puts the run's values in
        a new sheet; Export CSV... asks for a file"""
        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        self.assertTrue(panel.csvButton.isVisibleTo(panel.resultsBox))

        doc = self.doc

        class Results:
            Document = doc

            def getSimulationResults(self):
                return {
                    "Times": [0.0, 0.5],
                    "Channels": [
                        {
                            "Object": None,
                            "Name": "Motion",
                            "Quantity": "MotorTorque",
                            "Values": [1000.0, 2000.0],
                            "Unique": True,
                        }
                    ],
                }

        assembly = panel.assembly
        panel.assembly = Results()
        try:
            panel.sheetButton.click()
        finally:
            panel.assembly = assembly
        sheets = [o for o in self.doc.Objects if o.isDerivedFrom("Spreadsheet::Sheet")]
        self.assertEqual(len(sheets), 1)
        self.assertEqual(sheets[0].get("B1"), "Motion: Motor torque (N m)")
        self.assertAlmostEqual(sheets[0].get("B3"), 2e-3, places=12)
        self.accept_task()
        self.assertClean()

    def test_run_on_a_worker(self):
        """Generate runs the study on a worker thread: the panel shows its
        progress and a Cancel, then the player; cancelled, it says so and
        keeps no frames"""
        import time

        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.TimeEnd = 0.2
        self.doc.recompute()
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()

        def wait(until, limit=60.0):
            start = time.monotonic()
            while not until():
                self.assertLess(time.monotonic() - start, limit)
                _process(1)
                time.sleep(0.01)

        panel.form.RunKinematicsButton.click()
        wait(lambda: not panel.runPoll.isActive())
        self.assertFalse(panel.runMessage.isVisibleTo(panel.form), panel.runMessage.text())
        self.assertTrue(panel.form.groupBox_player.isVisibleTo(panel.form))
        self.assertGreater(self.assembly.numberOfFrames(), 1)
        self.assertTrue(panel.form.RunKinematicsButton.isEnabled())
        self.assertFalse(panel.runRow.isVisibleTo(panel.form))

        # the cancel half runs a dynamic study, which only Chrono solves
        import AssemblyApp

        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.accept_task()
            self.assertClean()
            return
        sim.AnalysisType = "Dynamic"
        sim.TimeEnd = 10000
        self.doc.recompute()
        panel.form.RunKinematicsButton.click()
        self.assertTrue(panel.runRow.isVisibleTo(panel.form))
        self.assertFalse(panel.form.RunKinematicsButton.isEnabled())
        wait(lambda: self.assembly.getSimulationProgress()["Frames"] >= 2)
        panel.runCancelButton.click()
        wait(lambda: not panel.runPoll.isActive(), limit=10.0)
        self.assertIn("cancelled", panel.runMessage.text())
        self.assertEqual(self.assembly.numberOfFrames(), 0)
        self.assertTrue(panel.form.RunKinematicsButton.isEnabled())
        self.accept_task()
        self.assertClean()

    def test_stale_results(self):
        """A study changed after its run: the panel says its results are
        stale and the tree's icon carries the recompute mark; run again,
        both go"""
        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.TimeEnd = 0.05
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertIsInstance(sim.ViewObject.Proxy.getIcon(), str)
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        self.assertFalse(panel.staleMessage.isVisibleTo(panel.form))
        sim.TimeEnd = 0.04
        self.doc.recompute()
        _process()
        self.assertTrue(sim.ResultsStale)
        self.assertTrue(panel.staleMessage.isVisibleTo(panel.form))
        self.assertIsInstance(sim.ViewObject.Proxy.getIcon(), QtGui.QIcon)
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        _process()
        self.assertFalse(sim.ResultsStale)
        self.assertFalse(panel.staleMessage.isVisibleTo(panel.form))
        self.accept_task()
        self.assertClean()

    def test_results_shown_again(self):
        """A study opened again after another's run shows the results it kept
        in the document: the player at the first frame, the run's frames on
        the slider; its Keep row shows what it keeps and sets it"""
        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        first, second = self.proxies("Simulation")[:2]
        first.TimeEnd = 0.05
        second.TimeEnd = 0.02
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(first), 0)
        self.assertEqual(self.assembly.generateSimulation(second), 0)
        self.assertTrue(first.Results)
        self.assertEqual(self.assembly.numberOfFrames(), 3)

        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(first)
        Gui.Control.showDialog(panel)
        _process()
        self.assertEqual(self.assembly.getSimulationResults()["Simulation"], first)
        self.assertEqual(self.assembly.numberOfFrames(), 6)
        self.assertTrue(panel.form.groupBox_player.isVisibleTo(panel.form))
        self.assertEqual(panel.form.frameSlider.maximum(), 5)
        self.assertEqual(panel.form.frameSlider.value(), 0)
        self.assertTrue(panel.runMessage.isHidden())
        self.assertEqual(panel.keepCombo.currentData(), "Motion")
        panel.keepCombo.setCurrentIndex(panel.keepCombo.findData("All"))
        self.assertEqual(first.KeepResults, "All")
        self.accept_task()
        self.assertClean()

    def test_gltf_from_the_panel(self):
        """Save Animation's glTF format, no suffix typed: a .glb of the run,
        a component coloured as its view shows it"""
        import json
        import os
        import shutil
        import struct
        import tempfile
        from unittest import mock

        import CommandCreateSimulation
        import SimulationGltf

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.TimeEnd = 0.05
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.cyl1.ViewObject.ShapeColor = (0.2, 0.4, 0.8)
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        folder = tempfile.mkdtemp(prefix="GltfPanel")
        try:
            chosen = (os.path.join(folder, "run"), "Animated glTF, binary (*.glb)")
            with mock.patch.object(
                CommandCreateSimulation.QFileDialog, "getSaveFileName", return_value=chosen
            ):
                panel.saveAnimation()
            with open(os.path.join(folder, "run.glb"), "rb") as f:
                data = f.read()
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        self.assertEqual(data[:4], b"glTF")
        (length,) = struct.unpack_from("<I", data, 12)
        gltf = json.loads(data[20 : 20 + length])
        self.assertEqual(gltf["asset"]["extras"]["study"], sim.Label)
        node = next(n for n in gltf["nodes"] if n.get("extras", {}).get("name") == self.cyl1.Name)
        (primitive,) = gltf["meshes"][node["mesh"]]["primitives"]
        color = gltf["materials"][primitive["material"]]["pbrMetallicRoughness"]["baseColorFactor"]
        want = [SimulationGltf.srgbToLinear(c) for c in (0.2, 0.4, 0.8)] + [1.0]
        for a, b in zip(color, want):
            self.assertAlmostEqual(a, b, places=6)
        self.accept_task()
        self.assertClean()

    def test_trace_button(self):
        """The panel's Trace button traces the selected vertex: a trace of
        its part, the point in the part's coordinates; with nothing
        selected it says what to select"""
        import CommandCreateSimulation
        import SimulationTrace

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        Gui.Selection.clearSelection()
        self.select(self.cyl1, "Vertex1")
        part, point = CommandCreateSimulation.selectedPoint(self.assembly)
        self.assertEqual(part, self.cyl1)
        want = self.cyl1.Placement.inverse().multVec(self.cyl1.Shape.Vertexes[0].Point)
        self.assertLess((point - want).Length, 1e-9)
        panel.addTraceButton.click()
        traces = [o for o in sim.Group if SimulationTrace.isTrace(o)]
        self.assertEqual(len(traces), 1)
        self.assertEqual(traces[0].Part, self.cyl1)
        self.assertLess((traces[0].Point - want).Length, 1e-9)
        listed = panel.form.motionList
        self.assertIn(traces[0].Label, [listed.item(i).text() for i in range(listed.count())])
        Gui.Selection.clearSelection()
        self.accept_task()
        self.assertClean()

    def test_kinematic_reactions_check(self):
        """The Reactions check shows for a Kinematic study, sets Reactions,
        and with it the study takes gravity and the elements as a Dynamic
        one does; a Dynamic study hides it"""
        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        self.assertFalse(sim.Reactions)
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        self.assertTrue(panel.reactionsCheck.isVisibleTo(panel.form))
        self.assertFalse(panel.gravityCheck.isEnabled())
        self.assertFalse(panel.elementsBox.isVisibleTo(panel.form))
        panel.reactionsCheck.setChecked(True)
        self.assertTrue(sim.Reactions)
        self.assertTrue(panel.gravityCheck.isEnabled())
        self.assertTrue(panel.elementsBox.isVisibleTo(panel.form))
        panel.analysisCombo.setCurrentIndex(panel.analysisCombo.findData("Dynamic"))
        self.assertFalse(panel.reactionsCheck.isVisibleTo(panel.form))
        self.assertTrue(panel.gravityCheck.isEnabled())
        self.accept_task()
        self.assertTrue(sim.Reactions)
        self.assertClean()

    def test_initial_velocity(self):
        """The initial velocity's dialog writes a joint's rate, and reopened
        on it, a component's velocity"""
        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        joint = self.joints("Revolute")[0]
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.AnalysisType = "Dynamic"

        dialog = CommandCreateSimulation.InitialConditionDialog(self.assembly)
        self.assertEqual(dialog.jointCombo.currentData(), joint)
        self.assertEqual(dialog.typeCombo.currentData(), "Angular")
        self.assertEqual(dialog.rateBox.suffix(), " deg/s")
        dialog.rateBox.setValue(45)
        obj = CommandCreateSimulation.newInitialCondition(sim)
        dialog.apply(obj)
        self.assertEqual(obj.Mode, "Joint")
        self.assertEqual(obj.Joint, joint)
        self.assertAlmostEqual(obj.Rate.getValueAs("deg/s").Value, 45, places=9)
        self.assertIn(obj, sim.Group)
        self.assertIn(joint.Label, obj.Label)

        # Reopened on it, as a double click in the panel's list does
        dialog = CommandCreateSimulation.InitialConditionDialog(self.assembly, obj)
        self.assertEqual(dialog.rateBox.value(), 45)
        dialog.modeCombo.setCurrentIndex(dialog.modeCombo.findData("Component"))
        self.assertFalse(dialog.rateBox.isVisibleTo(dialog.dialog))
        self.assertTrue(dialog.componentCombo.isVisibleTo(dialog.dialog))
        dialog.componentCombo.setCurrentIndex(dialog.componentCombo.findData(self.cyl1))
        dialog.linearBoxes[2].setValue(20)
        dialog.angularBoxes[0].setValue(30)
        dialog.apply(obj)
        self.assertEqual(obj.Mode, "Component")
        self.assertEqual(obj.Component, self.cyl1)
        self.assertEqual(obj.LinearVelocity, App.Vector(0, 0, 20))
        self.assertEqual(obj.AngularVelocity, App.Vector(30, 0, 0))
        self.assertClean()

    def test_sensor(self):
        """The sensor's dialog lists what reports values -- the joint, not
        its grounding -- and the quantities of the one picked, the component
        idle for a single value, the threshold in the unit the results show;
        it writes the sensor, and reopened on it shows it again"""
        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        joint = self.joints("Revolute")[0]
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.AnalysisType = "Dynamic"

        dialog = CommandCreateSimulation.SensorDialog(self.assembly)
        combo = dialog.sourceCombo
        self.assertEqual([combo.itemData(i) for i in range(combo.count())], [joint])
        combo = dialog.quantityCombo
        self.assertEqual(
            [combo.itemData(i) for i in range(combo.count())],
            ["Force", "Torque", "MotorTorque", "MotorForce"],
        )
        self.assertTrue(dialog.componentCombo.isEnabled())
        self.assertEqual(dialog.thresholdBox.suffix(), " N")
        combo.setCurrentIndex(combo.findData("MotorTorque"))
        self.assertFalse(dialog.componentCombo.isEnabled())
        self.assertEqual(dialog.thresholdBox.suffix(), " N m")
        dialog.thresholdBox.setValue(2.5)
        dialog.compareCombo.setCurrentIndex(dialog.compareCombo.findData("Below"))
        dialog.actionCombo.setCurrentIndex(dialog.actionCombo.findData("Mark"))
        obj = CommandCreateSimulation.newSensor(sim, joint, "Force", 0.0)
        dialog.apply(obj)
        self.assertEqual(obj.Source, joint)
        self.assertEqual(obj.Quantity, "MotorTorque")
        self.assertEqual(obj.Compare, "Below")
        self.assertEqual(obj.Action, "Mark")
        self.assertAlmostEqual(obj.Threshold.getValueAs("N*m").Value, 2.5, places=9)
        self.assertIn(obj, sim.Group)
        self.assertEqual(obj.Label, joint.Label + ": Motor torque below 2.5 N m")

        # Reopened on it, as a double click in the panel's list does
        dialog = CommandCreateSimulation.SensorDialog(self.assembly, obj)
        self.assertEqual(dialog.quantityCombo.currentData(), "MotorTorque")
        self.assertEqual(dialog.compareCombo.currentData(), "Below")
        self.assertEqual(dialog.actionCombo.currentData(), "Mark")
        self.assertAlmostEqual(dialog.thresholdBox.value(), 2.5, places=9)
        self.assertClean()

    def test_sensor_on_contact_events(self):
        """On a contact set the sensor's dialog offers its contacts made and
        let go after its values, by their speed: the component idle, the
        threshold in mm/s, the compare worded for a speed and back for a
        value; the label says which, and the speed when there is one"""
        import CommandCreateContact
        import CommandCreateSimulation

        contact = CommandCreateContact.newContactSet(self.assembly, [self.box], [self.cyl1])
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.AnalysisType = "Dynamic"

        dialog = CommandCreateSimulation.SensorDialog(self.assembly)
        combo = dialog.sourceCombo
        combo.setCurrentIndex(combo.findData(contact))
        combo = dialog.quantityCombo
        quantities = [combo.itemData(i) for i in range(combo.count())]
        self.assertEqual(quantities[-2:], ["ContactBegin", "ContactEnd"])
        compare = dialog.compareCombo
        self.assertEqual(compare.itemText(0), "Rises above")
        combo.setCurrentIndex(combo.findData("ContactEnd"))
        self.assertFalse(dialog.componentCombo.isEnabled())
        self.assertEqual(dialog.thresholdBox.suffix(), " mm/s")
        self.assertEqual([compare.itemText(i) for i in range(2)], ["Speed at least", "Speed below"])
        obj = CommandCreateSimulation.newSensor(sim, contact, "Force", 0.0)
        dialog.apply(obj)
        self.assertEqual(obj.Quantity, "ContactEnd")
        self.assertEqual(obj.Label, contact.Label + ": Contact let go")
        dialog.thresholdBox.setValue(50)
        dialog.compareCombo.setCurrentIndex(dialog.compareCombo.findData("Below"))
        dialog.apply(obj)
        self.assertAlmostEqual(obj.Threshold.getValueAs("mm/s").Value, 50.0, places=9)
        self.assertEqual(obj.Label, contact.Label + ": Contact let go below 50 mm/s")
        dialog.compareCombo.setCurrentIndex(dialog.compareCombo.findData("Above"))
        dialog.apply(obj)
        self.assertEqual(obj.Label, contact.Label + ": Contact let go at 50 mm/s or more")
        combo.setCurrentIndex(combo.findData("ContactForce"))
        self.assertEqual(compare.itemText(0), "Rises above")

        # Reopened on it
        dialog = CommandCreateSimulation.SensorDialog(self.assembly, obj)
        self.assertEqual(dialog.quantityCombo.currentData(), "ContactEnd")
        self.assertAlmostEqual(dialog.thresholdBox.value(), 50.0, places=9)
        self.assertEqual(dialog.compareCombo.itemText(0), "Speed at least")
        self.assertClean()

    def test_sensor_count_and_pair(self):
        """The sensor's dialog: a value can be crossed either way, an event
        cannot; the pair, idle for a value, lists the set's components for
        its events; the trip it acts on. The label says each, and the dialog
        reopened on the sensor shows them again"""
        import CommandCreateContact
        import CommandCreateSimulation

        contact = CommandCreateContact.newContactSet(self.assembly, [self.box], [self.cyl1])
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.AnalysisType = "Dynamic"

        dialog = CommandCreateSimulation.SensorDialog(self.assembly)
        dialog.sourceCombo.setCurrentIndex(dialog.sourceCombo.findData(contact))
        quantity = dialog.quantityCombo
        compare = dialog.compareCombo
        quantity.setCurrentIndex(quantity.findData("ContactForce"))
        self.assertGreaterEqual(compare.findData("Crosses"), 0)
        self.assertFalse(dialog.pairCombos[0].isEnabled())
        compare.setCurrentIndex(compare.findData("Crosses"))
        dialog.countBox.setValue(4)
        obj = CommandCreateSimulation.newSensor(sim, contact, "Force", 0.0)
        dialog.apply(obj)
        self.assertEqual(obj.Compare, "Crosses")
        self.assertEqual(obj.Count, 4)
        self.assertEqual(obj.Pair, [])
        self.assertEqual(obj.Label, contact.Label + ": Contact force crossing 0 N, at the 4th")

        quantity.setCurrentIndex(quantity.findData("ContactBegin"))
        self.assertEqual(compare.findData("Crosses"), -1)
        self.assertTrue(dialog.pairCombos[0].isEnabled())
        parts = [dialog.pairCombos[1].itemData(i) for i in range(dialog.pairCombos[1].count())]
        self.assertEqual(parts, [None, self.box, self.cyl1])
        dialog.pairCombos[0].setCurrentIndex(dialog.pairCombos[0].findData(self.cyl1))
        dialog.actionCombo.setCurrentIndex(dialog.actionCombo.findData("Mark"))
        dialog.countBox.setValue(2)
        dialog.apply(obj)
        self.assertEqual(obj.Pair, [self.cyl1])
        self.assertEqual(obj.Compare, "Above")
        self.assertEqual(
            obj.Label, contact.Label + ": Contact made by " + self.cyl1.Label + ", from the 2nd"
        )
        dialog.pairCombos[1].setCurrentIndex(dialog.pairCombos[1].findData(self.box))
        dialog.apply(obj)
        self.assertEqual(obj.Pair, [self.cyl1, self.box])
        self.assertEqual(
            obj.Label,
            contact.Label
            + ": Contact made by %s and %s, from the 2nd" % (self.cyl1.Label, self.box.Label),
        )

        # Reopened on it
        dialog = CommandCreateSimulation.SensorDialog(self.assembly, obj)
        self.assertEqual(dialog.pairCombos[0].currentData(), self.cyl1)
        self.assertEqual(dialog.pairCombos[1].currentData(), self.box)
        self.assertEqual(dialog.countBox.value(), 2)
        self.assertEqual([CommandCreateSimulation.ordinal(n) for n in (1, 2, 3, 11, 12, 21, 112)],
                         ["1st", "2nd", "3rd", "11th", "12th", "21st", "112th"])

        # Every such trip: a mark's, idle for a stop
        self.assertTrue(dialog.repeatCheck.isEnabled())
        self.assertFalse(dialog.repeatCheck.isChecked())
        dialog.repeatCheck.setChecked(True)
        dialog.countBox.setValue(3)
        dialog.apply(obj)
        self.assertTrue(obj.Repeat)
        self.assertEqual(
            obj.Label,
            contact.Label
            + ": Contact made by %s and %s, every 3rd" % (self.cyl1.Label, self.box.Label),
        )
        dialog = CommandCreateSimulation.SensorDialog(self.assembly, obj)
        self.assertTrue(dialog.repeatCheck.isChecked())
        dialog.actionCombo.setCurrentIndex(dialog.actionCombo.findData("Stop"))
        self.assertFalse(dialog.repeatCheck.isEnabled())
        dialog.apply(obj)
        self.assertFalse(obj.Repeat)
        self.assertTrue(obj.Label.endswith(", at the 3rd"), obj.Label)
        self.assertClean()

    def test_sensor_events_shown(self):
        """After a run the panel says which sensors tripped and when, by
        their labels; the one that stopped it says so. A contact made or let
        go names its pair, its speed and its set"""
        import CommandCreateSimulation
        from PySide import QtWidgets

        class Sensor:
            Label = "Spring length"
            Quantity = "SpringLength"

        class Contact:
            Label = "Landing"
            Quantity = "ContactForce"

        class Panel:
            pass

        holder = QtWidgets.QWidget()
        panel = Panel()
        panel.eventMessage = QtWidgets.QLabel(holder)
        class Ball:
            Label = "Ball"

        def contact(time, kind, speed):
            return {
                "Time": time,
                "Object": Contact(),
                "Name": "C",
                "Value": speed,
                "Stopped": False,
                "Kind": kind,
                "Bodies": (Ball(), None),
            }

        panel.results = {
            "Events": [
                {"Time": 0.0125, "Object": Sensor(), "Name": "S", "Value": 4.0, "Stopped": False},
                contact(0.0142809, "ContactBegin", 140.0),
                contact(0.0143335, "ContactEnd", 112.0),
                {"Time": 0.0258109, "Object": Contact(), "Name": "C", "Value": 2500.0, "Stopped": True},
            ]
        }
        CommandCreateSimulation.TaskAssemblyCreateSimulation.showEvents(panel)

        def speed(value):
            return App.Units.Quantity(value, App.Units.Velocity).UserString

        self.assertEqual(
            panel.eventMessage.text(),
            "Marked at 0.0125 s by Spring length\n"
            "Contact at 0.0142809 s: Ball and the ground, in at {} (Landing)\n"
            "Let go at 0.0143335 s: Ball and the ground, out at {} (Landing)\n"
            "Stopped at 0.0258109 s by Landing".format(speed(140.0), speed(112.0)),
        )
        self.assertFalse(panel.eventMessage.isHidden())
        # Past eight lines the sensors' stay, the contacts' fill the rest
        bounces = [
            contact(0.01 * (k + 1), "ContactEnd" if k % 2 else "ContactBegin", 100.0)
            for k in range(10)
        ]
        stop = {"Time": 0.2, "Object": Contact(), "Name": "C", "Value": 1.0, "Stopped": True}
        panel.results = {"Events": bounces + [stop] + bounces[:2]}
        CommandCreateSimulation.TaskAssemblyCreateSimulation.showEvents(panel)
        lines = panel.eventMessage.text().split("\n")
        self.assertEqual(len(lines), 9, lines)
        self.assertTrue(lines[0].startswith("Contact at 0.01 s: Ball and the ground"))
        self.assertTrue(lines[6].startswith("Contact at 0.07 s"))
        self.assertEqual(lines[7:], ["Stopped at 0.2 s by Landing", "and 5 more"])
        panel.results = {"Events": []}
        CommandCreateSimulation.TaskAssemblyCreateSimulation.showEvents(panel)
        self.assertTrue(panel.eventMessage.isHidden())
        holder.deleteLater()
        self.assertClean()

    def test_results_not_unique(self):
        """The Results box marks a redundant joint's reaction "(not unique)",
        and says why in its tool tip; a unique one is listed plain"""
        import CommandCreateSimulation
        from PySide import QtCore, QtWidgets

        panel = self.fake_results_panel()
        CommandCreateSimulation.TaskAssemblyCreateSimulation.fillResults(panel)
        combo = panel.resultCombo
        self.assertEqual(combo.count(), 2)
        self.assertTrue(combo.itemText(0).startswith("Revolute: "))
        self.assertTrue(combo.itemText(0).endswith(" (not unique)"))
        self.assertIn("redundant", combo.itemData(0, QtCore.Qt.ToolTipRole))
        self.assertNotIn("not unique", combo.itemText(1))
        self.assertIsNone(combo.itemData(1, QtCore.Qt.ToolTipRole))
        panel.resultCombo.deleteLater()
        panel.resultsBox.deleteLater()

    def fake_results_panel(self):
        """The Results box's filling on its own: a run of a redundant
        Revolute and a Slider, its keys made anew on every call, as the run's
        results are"""
        import CommandCreateSimulation
        from PySide import QtWidgets

        doc = self.doc

        class Results:
            Document = doc

            def getSimulationResults(self):
                force = {"Object": None, "Quantity": "Force", "Values": []}
                return {
                    "Times": [],
                    "Channels": [
                        dict(force, Name="Revolute", Unique=False),
                        dict(force, Name="Slider", Unique=True),
                    ],
                }

        class Panel:
            def resultIndex(self, key):
                return CommandCreateSimulation.TaskAssemblyCreateSimulation.resultIndex(self, key)

        panel = Panel()
        panel.assembly = Results()
        panel.resultCombo = QtWidgets.QComboBox()
        panel.resultsBox = QtWidgets.QWidget()
        panel.showResult = lambda: None
        return panel

    def test_results_keep_the_channel_chosen(self):
        """The Results box filled again, as a new run does, keeps the channel
        chosen: its key is a tuple made anew, which QComboBox.findData never
        matched (it compares a Python object by identity)"""
        import CommandCreateSimulation

        fill = CommandCreateSimulation.TaskAssemblyCreateSimulation.fillResults
        panel = self.fake_results_panel()
        fill(panel)
        panel.resultCombo.setCurrentIndex(1)
        fill(panel)
        self.assertEqual(panel.resultCombo.currentIndex(), 1)
        self.assertEqual(tuple(panel.resultCombo.currentData()), ("Slider", "Force"))
        self.assertEqual(panel.resultIndex(None), -1)
        self.assertEqual(panel.resultIndex(("Nothing", "Force")), -1)
        panel.resultCombo.deleteLater()
        panel.resultsBox.deleteLater()

    def test_results_on_parts(self):
        """A contact set's values on each of its parts (D4a): one row each
        in the Results box, named by the part, keyed by it, and a column
        each in the table the exports write"""
        import CommandCreateSimulation

        ball = self.doc.addObject("Part::Box", "Ball")
        plate = self.doc.addObject("Part::Box", "Plate")
        plate.Label = "Floor plate"
        panel = self.fake_results_panel()
        doc = self.doc

        class Results:
            Document = doc

            def getSimulationResults(self):
                on = {"Object": None, "Name": "ContactSet", "Quantity": "PartContactForce"}
                return {
                    "Times": [0.0],
                    "Channels": [
                        dict(on, On=ball, Values=[App.Vector(0, 0, 2000)]),
                        dict(on, On=plate, Values=[App.Vector(0, 0, -2000)]),
                    ],
                }

        panel.assembly = Results()
        CommandCreateSimulation.TaskAssemblyCreateSimulation.fillResults(panel)
        combo = panel.resultCombo
        self.assertEqual(
            [combo.itemText(k) for k in range(combo.count())],
            [
                "ContactSet (Ball): Contact force on the part",
                "ContactSet (Floor plate): Contact force on the part",
            ],
        )
        combo.setCurrentIndex(1)
        CommandCreateSimulation.TaskAssemblyCreateSimulation.fillResults(panel)
        self.assertEqual(tuple(combo.currentData()), ("ContactSet", "PartContactForce", "Plate"))
        header, rows = CommandCreateSimulation.resultsTable(panel.assembly)
        self.assertIn("ContactSet (Floor plate): Contact force on the part Z (N)", header)
        column = header.index("ContactSet (Ball): Contact force on the part Z (N)")
        self.assertEqual(rows[0][column], 2.0)
        combo.deleteLater()
        panel.resultsBox.deleteLater()

    def test_results_sampled(self):
        """A stress series with the part's own vibration has it sampled
        between the frames too: the Results box draws the samples, a peak
        between two frames to its height, each frame's static stress beside
        them, and reads both at the frame"""
        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        doc = self.doc
        samples = [0.0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3]
        values = [1.0, 2.0, 2.5, 9.0, 3.5, 1.0, 1.5]

        class Results:
            Document = doc

            def getSimulationResults(self):
                return {
                    "Times": [0.0, 0.1, 0.2, 0.3],
                    "Channels": [
                        {
                            "Object": None,
                            "Name": "Series",
                            "Quantity": "VonMises",
                            "Values": [1.0, 2.5, 3.5, 1.5],
                            "Unique": True,
                            "SampleTimes": samples,
                            "Samples": values,
                            "Static": [1.0, 2.4, 3.0, 1.4],
                        }
                    ],
                }

            def updateForFrame(self, frame):
                pass

        assembly = panel.assembly
        panel.assembly = Results()
        try:
            panel.fillResults()
            self.assertEqual(
                panel.resultCurves(),
                (
                    [
                        ("vibrating", [1.0, 2.5, 3.5, 1.5], (samples, values)),
                        ("as it stands", [1.0, 2.4, 3.0, 1.4]),
                    ],
                    "MPa",
                ),
            )
            panel.form.frameSlider.setMaximum(3)
            panel.form.frameSlider.setValue(2)
            panel.showResultValue()
            self.assertIn("vibrating 3.5 MPa", panel.resultValue.text())
            self.assertIn("as it stands 3 MPa", panel.resultValue.text())
            self.assertFalse(panel.resultPlot.grab().isNull())
            low, high = panel.resultPlot.valueRange
            self.assertGreaterEqual(high, 9.0)
        finally:
            panel.assembly = assembly
        self.accept_task()
        self.assertClean()

    def test_results_with_gaps(self):
        """A stress series' channel has a value only at the frames solved:
        the Results box plots the runs of values, and at a frame without
        one says so; the CSV leaves its cell empty"""
        import csv
        import os
        import tempfile

        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        doc = self.doc

        class Results:
            Document = doc

            def getSimulationResults(self):
                return {
                    "Times": [0.0, 0.1, 0.2, 0.3],
                    "Channels": [
                        {
                            "Object": None,
                            "Name": "Series",
                            "Quantity": "VonMises",
                            "Values": [None, 2.5, 3.5, None],
                            "Unique": True,
                        }
                    ],
                }

            def updateForFrame(self, frame):
                pass

        assembly = panel.assembly
        panel.assembly = Results()
        path = os.path.join(tempfile.mkdtemp(prefix="asm_gaps_"), "gaps.csv")
        try:
            panel.fillResults()
            self.assertEqual(panel.resultCurves(), ([("", [None, 2.5, 3.5, None])], "MPa"))
            panel.form.frameSlider.setMaximum(3)
            panel.form.frameSlider.setValue(0)
            panel.showResultValue()
            self.assertIn("not solved", panel.resultValue.text())
            panel.form.frameSlider.setValue(2)
            panel.showResultValue()
            self.assertIn("3.5 MPa", panel.resultValue.text())
            # Drawn, gaps and all
            self.assertFalse(panel.resultPlot.grab().isNull())
            self.assertTrue(CommandCreateSimulation.exportResultsCsv(panel.assembly, path))
            with open(path, newline="", encoding="utf-8") as f:
                rows = list(csv.reader(f))
        finally:
            panel.assembly = assembly
        self.assertEqual(rows[0][1], "Series: Largest von Mises stress (MPa)")
        self.assertEqual([r[1] for r in rows[1:]], ["", "2.5", "3.5", ""])
        self.accept_task()
        self.assertClean()

    @staticmethod
    def impact_times():
        """A run's frames: every 2.5 ms to 0.25 s, and 20 through an impact
        at 0.1 s, 3 us apart"""
        return sorted([k * 2.5e-3 for k in range(101)] + [0.1 + 3e-6 * (k + 1) for k in range(20)])

    def impact_run(self, times, shown):
        """An assembly's run at times for a panel to play: a spring's force
        through the impact, the times the parts were put at kept in shown"""
        doc = self.doc

        class Results:
            Document = doc

            def getSimulationResults(self):
                force = [1000.0 if 0.1 < t < 0.10006 else 0.0 for t in times]
                return {
                    "Times": times,
                    "Channels": [
                        {
                            "Object": None,
                            "Name": "Spring",
                            "Quantity": "SpringForce",
                            "Values": force,
                            "Unique": True,
                        }
                    ],
                }

            def updateForFrame(self, frame):
                shown.append(times[frame])

            def updateForTime(self, time):
                shown.append(time)

            def numberOfFrames(self):
                return len(times)

        return Results()

    def test_video_with_results_box(self):
        """Save Animation's menu: a video or a GIF with the Results box
        beside the 3D view as it plays and the time and speed over the view,
        the menu's choices kept in the preferences; both off, the view alone.
        The panel left at the frame it was at"""
        import os
        import shutil
        import tempfile
        from unittest import mock

        from PIL import Image

        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.TimeStepOutput = 2.5e-3
        sim.PlaybackSpeed = "Normal"
        params = App.ParamGet(CommandCreateSimulation.VideoParams)
        kept = {k: params.GetBool(k, True) for k in ("VideoResultsBox", "VideoTime")}
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        times, shown = self.impact_times(), []
        assembly = panel.assembly
        panel.assembly = self.impact_run(times, shown)
        folder = tempfile.mkdtemp(prefix="VideoBox")
        # The save's progress, left to run
        self.modals.expect.append(
            (lambda w: isinstance(w, QtWidgets.QProgressDialog), lambda w: None)
        )

        def save(name):
            chosen = (os.path.join(folder, name), "Animated GIF (*.gif)")
            with mock.patch.object(
                CommandCreateSimulation.QFileDialog, "getSaveFileName", return_value=chosen
            ):
                panel.saveAnimation()
            return Image.open(os.path.join(folder, name))

        try:
            panel.fillResults()
            panel.form.frameSlider.setMaximum(len(times) - 1)
            panel.fitPlayerLabels(len(times))
            panel.resultsBox.show()
            panel.form.frameSlider.setValue(7)
            _process()
            label = panel.form.FrameTimeLabel.text()
            self.assertTrue(panel.videoBoxAction.isChecked())
            self.assertTrue(panel.videoTimeAction.isChecked())
            self.assertIs(panel.form.SaveAnimationButton.menu().actions()[0], panel.videoBoxAction)
            width, height = Gui.ActiveDocument.ActiveView.getSize()
            width -= width % 2
            height -= height % 2
            # With both: the box beside the view, its frames each the run's
            # frame at its time, the time over the view on a light backing
            both = save("both.gif")
            self.assertEqual(both.n_frames, 101)
            self.assertGreater(both.width, width + 100)
            self.assertEqual(both.width % 2, 0)
            self.assertEqual(both.height, height)
            both.seek(50)
            frame = both.convert("RGB")
            pad = max(12, height // 32) // 3
            self.assertGreater(min(frame.getpixel((pad + 2, pad + 2))), 180)
            self.assertAlmostEqual(shown[-1], times[7], delta=1e-12)
            self.assertEqual(panel.form.FrameTimeLabel.text(), label)
            # Off, kept in the preferences: the view alone
            panel.videoBoxAction.setChecked(False)
            panel.videoTimeAction.setChecked(False)
            self.assertFalse(params.GetBool("VideoResultsBox", True))
            self.assertFalse(params.GetBool("VideoTime", True))
            alone = save("alone.gif")
            self.assertEqual(alone.size, (width, height))
            # The time alone: over the view, no box beside it
            panel.videoTimeAction.setChecked(True)
            self.assertEqual(save("time.gif").size, (width, height))
        finally:
            for k, v in kept.items():
                params.SetBool(k, v)
            panel.assembly = assembly
            shutil.rmtree(folder, ignore_errors=True)
        self.accept_task()
        self.assertClean()

    def test_video_background(self):
        """Save Animation's Background menu: the 3D view saved on white, on
        black or on a colour of one's own, asked for each time Custom is
        chosen and kept as it was if the dialog is cancelled, the choice and
        the colour kept in the preferences"""
        import os
        import shutil
        import tempfile
        from unittest import mock

        from PIL import Image

        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.TimeStepOutput = 2.5e-3
        sim.PlaybackSpeed = "Normal"
        params = App.ParamGet(CommandCreateSimulation.VideoParams)
        keptBools = {k: params.GetBool(k, True) for k in ("VideoResultsBox", "VideoTime")}
        keptStrings = {
            k: params.GetString(k, "") for k in ("VideoBackground", "VideoBackgroundColor")
        }
        params.SetString("VideoBackground", "Current")
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        times, shown = self.impact_times(), []
        assembly = panel.assembly
        panel.assembly = self.impact_run(times, shown)
        folder = tempfile.mkdtemp(prefix="VideoBack")
        self.modals.expect.append(
            (lambda w: isinstance(w, QtWidgets.QProgressDialog), lambda w: None)
        )
        getColor = CommandCreateSimulation.QtWidgets.QColorDialog

        def save(name):
            chosen = (os.path.join(folder, name), "Animated GIF (*.gif)")
            with mock.patch.object(
                CommandCreateSimulation.QFileDialog, "getSaveFileName", return_value=chosen
            ):
                panel.saveAnimation()
            image = Image.open(os.path.join(folder, name))
            # The corner, away from the parts and the view's overlays: the
            # first frame's, as the GIF folds frames that stay the same
            return image.convert("RGB").getpixel((2, 2))

        def choose(key, color=None):
            with mock.patch.object(getColor, "getColor", return_value=color) as asked:
                panel.videoBackgroundActions[key].trigger()
            return asked.called

        try:
            panel.fillResults()
            panel.form.frameSlider.setMaximum(len(times) - 1)
            panel.fitPlayerLabels(len(times))
            panel.videoBoxAction.setChecked(False)
            panel.videoTimeAction.setChecked(False)
            self.assertTrue(panel.videoBackgroundActions["Current"].isChecked())
            self.assertEqual(panel.videoBackground(), "Current")
            self.assertFalse(choose("White"))
            self.assertEqual(params.GetString("VideoBackground", ""), "White")
            self.assertGreater(min(save("white.gif")), 245)
            choose("Black")
            self.assertLess(max(save("black.gif")), 10)
            # Custom: its colour asked for, kept, a swatch on the action
            self.assertTrue(choose("Custom", QtGui.QColor(30, 120, 200)))
            self.assertEqual(params.GetString("VideoBackground", ""), "Custom")
            self.assertEqual(params.GetString("VideoBackgroundColor", ""), "#1e78c8")
            self.assertEqual(panel.videoBackground(), "#1e78c8")
            self.assertFalse(panel.videoBackgroundActions["Custom"].icon().isNull())
            for got, want in zip(save("custom.gif"), (30, 120, 200)):
                self.assertAlmostEqual(got, want, delta=12)
            # Cancelled from black: black still
            choose("Black")
            self.assertTrue(choose("Custom", QtGui.QColor()))
            self.assertTrue(panel.videoBackgroundActions["Black"].isChecked())
            self.assertEqual(params.GetString("VideoBackground", ""), "Black")
            self.assertEqual(params.GetString("VideoBackgroundColor", ""), "#1e78c8")
            # A choice the menu does not know: the view's own
            params.SetString("VideoBackground", "Bogus")
            self.assertEqual(panel.videoBackground(), "Current")
        finally:
            for k, v in keptBools.items():
                params.SetBool(k, v)
            for k, v in keptStrings.items():
                if v:
                    params.SetString(k, v)
                else:
                    params.RemString(k)
            panel.assembly = assembly
            shutil.rmtree(folder, ignore_errors=True)
        self.accept_task()
        self.assertClean()

    def test_playback_speed(self):
        """The player at the study's speed (docs/AssemblyDynamics.md sec
        8.6): Auto by default; through an impact's frames, microseconds
        apart, the parts at the run's time between them, the speed said
        against real time, the time to the microsecond and the plot about
        it, less of the run, the impact stretched; its + and - zooming about
        the time shown, a click on its timeline going to the time there;
        Normal, the whole run shown, an output step a video frame"""
        import CommandCreateSimulation

        self.make_joint("Assembly_CreateJointRevolute", "Revolute")
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.TimeStepOutput = 2.5e-3
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        self.assertEqual(sim.PlaybackSpeed, "Auto")
        self.assertEqual(panel.speedCombo.currentData(), "Auto")
        times, shown = self.impact_times(), []
        assembly = panel.assembly
        panel.assembly = self.impact_run(times, shown)
        try:
            panel.fillResults()
            panel.form.frameSlider.setMaximum(len(times) - 1)
            playback = panel.makePlayback()
            t = 0.10003
            panel.showPlayed(t, playback.rate(t))
            self.assertEqual(shown[-1], t)
            self.assertAlmostEqual(times[panel.form.frameSlider.value()], t, delta=1e-12)
            self.assertTrue(panel.speedLabel.text().startswith("1/"), panel.speedLabel.text())
            self.assertIn("0.100030", panel.form.FrameTimeLabel.text())
            plot = panel.resultPlot
            low, high = plot.runWindow()
            self.assertTrue(low <= t <= high)
            self.assertLess(high - low, 0.25)
            self.assertFalse(plot.grab().isNull())
            # Zoomed in about the time shown, half as much of the run
            plot.zoomIn.click()
            zoomed = plot.runWindow()
            self.assertTrue(zoomed[0] <= t <= zoomed[1])
            self.assertLess(zoomed[1] - zoomed[0], high - low)
            plot.zoomOut.click()
            self.assertEqual(plot.runWindow(), (low, high))
            # The timeline a movie player's: a click goes to the time there
            plot.grab()
            area = plot.area
            point = QtCore.QPointF(area.left() + 0.25 * area.width(), area.bottom() + 6)
            v0, v1 = plot.window
            wanted = plot.fromAxis(v0 + 0.25 * (v1 - v0))
            press = QtGui.QMouseEvent(
                QtCore.QEvent.MouseButtonPress,
                point,
                plot.mapToGlobal(point),
                QtCore.Qt.LeftButton,
                QtCore.Qt.LeftButton,
                QtCore.Qt.NoModifier,
            )
            QtWidgets.QApplication.sendEvent(plot, press)
            self.assertAlmostEqual(panel.shownTime, wanted, delta=1e-12)
            self.assertAlmostEqual(shown[-1], wanted, delta=1e-12)
            # Played: the video frames' times
            panel.animationTimerStartForward()
            self.assertTrue(panel.animationTimer.isActive())
            self.assertGreater(len(panel.playFrames), 121)
            panel.stopAnimation()
            # The player's labels as wide at every frame, as wide as their
            # widest, not their text then: as they were, the panel, as wide
            # as they are, widened and narrowed as it played
            labels = (panel.form.FrameLabel, panel.form.FrameTimeLabel, panel.speedLabel)

            def widths():
                seen = [set() for _ in labels]
                for t, rate in panel.playFrames:
                    panel.showPlayed(t, rate)
                    for k, label in enumerate(labels):
                        seen[k].add(max(label.sizeHint().width(), label.minimumWidth()))
                return [len(s) for s in seen]

            for label in labels:
                label.setMinimumWidth(0)
            self.assertEqual([n > 1 for n in widths()], [True, True, True])
            panel.fitPlayerLabels(len(times))
            self.assertEqual(widths(), [1, 1, 1])
            # Auto's slow motion, the study's
            self.assertFalse(panel.slowSpin.isHidden())
            self.assertEqual(sim.SlowMotion, 1.0)
            frames = len(panel.playback.frames())
            panel.slowSpin.setValue(3.0)
            self.assertEqual(sim.SlowMotion, 3.0)
            self.assertGreater(len(panel.playback.frames()), frames)
            # Normal: the whole run, an output step a video frame
            panel.speedCombo.setCurrentIndex(panel.speedCombo.findData("Normal"))
            self.assertEqual(sim.PlaybackSpeed, "Normal")
            self.assertTrue(panel.slowSpin.isHidden())
            self.assertEqual(len(panel.playback.frames()), 101)
            panel.showPlayed(t, panel.playback.rate(t))
            low, high = panel.resultPlot.runWindow()
            self.assertAlmostEqual(low, 0.0, delta=1e-12)
            self.assertAlmostEqual(high, 0.25, delta=1e-12)
        finally:
            panel.stopAnimation()
            panel.assembly = assembly
        self.accept_task()
        self.assertClean()

    # ------------------------------------------------------------ springs

    def springs(self):
        return [o for o in self.doc.Objects if o.isDerivedFrom("Assembly::SpringDamper")]

    def assertEnds(self, spring):
        """End 1 on the middle of the box's top face, end 2 on the centre of the
        first cylinder's top (Face3; Face2 is its bottom)"""
        self.assertIsNotNone(spring.Binding1)
        self.assertIsNotNone(spring.Binding2)
        self.assertEqual(spring.Component1, self.box)
        self.assertEqual(spring.Component2, self.cyl1)
        self.assertLess((spring.getEndPoint(1) - App.Vector(5, 5, 10)).Length, 1e-7)
        self.assertLess((spring.getEndPoint(2) - App.Vector(30, 0, 10)).Length, 1e-7)

    def test_spring_from_two_picked_faces(self):
        """Picked first, the command puts the ends on them and opens the
        panel; OK keeps the spring, free at the distance between them"""
        import CommandCreateSpring

        # Where the cursor was, as a click selects them
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face6", 5, 5, 10)
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Cyl1.Face3", 30, 0, 10)
        self.run_command("Assembly_CreateSpring")
        task = CommandCreateSpring.activeTask
        self.assertIsInstance(task, CommandCreateSpring.TaskSpring)
        self.accept_task()
        springs = self.springs()
        self.assertEqual(len(springs), 1)
        spring = springs[0]
        self.assertEnds(spring)
        length = (spring.getEndPoint(2) - spring.getEndPoint(1)).Length
        self.assertAlmostEqual(spring.FreeLength.Value, length, places=6)
        # Its element bindings under it in the tree
        self.assertEqual(
            set(spring.ViewObject.claimChildren()), {spring.Binding1, spring.Binding2}
        )
        self.assertClean()

    def pickInPanel(self):
        import CommandCreateSpring

        self.run_command("Assembly_CreateSpring")
        task = CommandCreateSpring.activeTask
        self.assertIsInstance(task, CommandCreateSpring.TaskSpring)
        self.assertEqual(task.armed, 1)
        # As the 3D view picks them: the cursor over each face's middle
        Gui.Selection.addSelection(
            self.doc.Name, self.assembly.Name, "Box.Face6", 5, 5, 10
        )
        _process()
        self.assertEqual(task.armed, 2)
        Gui.Selection.addSelection(
            self.doc.Name, self.assembly.Name, "Cyl1.Face3", 30, 0, 10
        )
        _process()
        self.assertIsNone(task.armed)
        self.assertEqual(Gui.Selection.getSelectionEx("", 0), [])
        return task

    def test_spring_picked_in_the_panel(self):
        task = self.pickInPanel()
        # The second end made free, then picked again
        task.onFree(2)
        self.assertIsNone(task.spring.Binding2)
        task.arm(2)
        Gui.Selection.addSelection(
            self.doc.Name, self.assembly.Name, "Cyl1.Face3", 30, 0, 10
        )
        _process()
        self.accept_task()
        springs = self.springs()
        self.assertEqual(len(springs), 1)
        self.assertEnds(springs[0])
        bindings = [o for o in self.doc.Objects if o.isDerivedFrom("Part::ElementBinding")]
        self.assertEqual(len(bindings), 2)
        self.assertClean()

    def test_spring_cancelled(self):
        """Cancel deletes the spring the command made, and its bindings"""
        task = self.pickInPanel()
        task.reject()
        _process()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertEqual(self.springs(), [])
        bindings = [o for o in self.doc.Objects if o.isDerivedFrom("Part::ElementBinding")]
        self.assertEqual(bindings, [])
        self.assertClean()

    def test_spring_edited_by_double_click(self):
        import CommandCreateSpring

        self.pickInPanel()
        self.accept_task()
        spring = self.springs()[0]
        self.assertTrue(spring.ViewObject.doubleClicked())
        _process()
        task = CommandCreateSpring.activeTask
        self.assertIsInstance(task, CommandCreateSpring.TaskSpring)
        self.assertIsNone(task.armed)
        task.onOffset(1, App.Units.Quantity("2 mm"))
        self.accept_task()
        self.assertLess((spring.getEndPoint(1) - App.Vector(5, 5, 12)).Length, 1e-7)
        self.assertClean()

    # ------------------------------------------------------------ loads

    def loads(self):
        return [o for o in self.doc.Objects if o.isDerivedFrom("Assembly::AppliedLoad")]

    def test_load_from_a_picked_face(self):
        """Picked first, the command puts the load on the face and opens the
        panel; OK keeps it, along the face's normal"""
        import CommandCreateLoad

        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face6", 5, 5, 10)
        self.run_command("Assembly_CreateLoad")
        task = CommandCreateLoad.activeTask
        self.assertIsInstance(task, CommandCreateLoad.TaskLoad)
        self.assertIsNone(task.armed)
        self.accept_task()
        loads = self.loads()
        self.assertEqual(len(loads), 1)
        load = loads[0]
        self.assertIsNotNone(load.Binding)
        self.assertEqual(load.Component, self.box)
        self.assertLess((load.getEndPlacement().Base - App.Vector(5, 5, 10)).Length, 1e-7)
        self.assertLess((load.getDirectionNow() - App.Vector(0, 0, 1)).Length, 1e-9)
        self.assertEqual(load.ViewObject.claimChildren(), [load.Binding])
        self.assertClean()

    def test_load_panel(self):
        """Picking in the panel: where it acts, then the reaction; a torque,
        a profile, the axes; then an edit cancelled leaves it as it was"""
        import CommandCreateLoad

        self.run_command("Assembly_CreateLoad")
        task = CommandCreateLoad.activeTask
        self.assertEqual(task.armed, "frame")
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face6", 5, 5, 10)
        _process()
        self.assertIsNone(task.armed)
        task.arm("other")
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Cyl1.Face1", 32, 0, 5)
        _process()
        self.assertIsNone(task.armed)
        task.kindCombo.setCurrentIndex(task.kindCombo.findData("Torque"))
        task.profileEdit.setText("sin(time)")
        task.axesCombo.setCurrentIndex(task.axesCombo.findData("Assembly"))
        task.onReverse()
        self.accept_task()
        load = self.loads()[0]
        self.assertEqual(load.Kind, "Torque")
        self.assertEqual(load.Reaction, self.cyl1)
        self.assertEqual(load.Profile, "sin(time)")
        self.assertEqual(load.DirectionAxes, "Assembly")
        self.assertLess((load.Direction - App.Vector(0, 0, -1)).Length, 1e-12)
        magnitude = load.Magnitude.getValueAs("N*m").Value
        # Edited and cancelled
        self.assertTrue(load.ViewObject.doubleClicked())
        _process()
        task = CommandCreateLoad.activeTask
        self.assertIsInstance(task, CommandCreateLoad.TaskLoad)
        task.onGround()
        load.Magnitude = "5 N*m"
        task.reject()
        _process()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertEqual(load.Reaction, self.cyl1)
        self.assertAlmostEqual(load.Magnitude.getValueAs("N*m").Value, magnitude, places=9)
        self.assertClean()

    def test_bushing_panel(self):
        """Picked first, the command puts the bushing on the face; the panel
        picks the part holding it and takes its values in their units; an
        edit cancelled leaves it as it was"""
        import CommandCreateBushing

        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face6", 5, 5, 10)
        self.run_command("Assembly_CreateBushing")
        task = CommandCreateBushing.activeTask
        self.assertIsInstance(task, CommandCreateBushing.TaskBushing)
        self.assertIsNone(task.armed)
        task.arm("other")
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Cyl1.Face1", 32, 0, 5)
        _process()
        self.assertIsNone(task.armed)
        # 2 N/mm along y, 3 N m/rad about z, a preload of 4 N along x
        box = task.valueBoxes[("TranslationalStiffness", 1)]
        self.assertEqual(box.value(), 1000)
        self.assertEqual(box.suffix(), " N/mm")
        box.setValue(2)
        task.valueBoxes[("RotationalStiffness", 2)].setValue(3)
        task.valueBoxes[("Preload", 0)].setValue(4)
        self.accept_task()
        bushings = [o for o in self.doc.Objects if o.isDerivedFrom("Assembly::Bushing")]
        self.assertEqual(len(bushings), 1)
        bushing = bushings[0]
        self.assertEqual(bushing.Component, self.box)
        self.assertEqual(bushing.Other, self.cyl1)
        self.assertLess((bushing.getEndPlacement().Base - App.Vector(5, 5, 10)).Length, 1e-7)
        self.assertEqual(bushing.ViewObject.claimChildren(), [bushing.Binding])
        self.assertAlmostEqual(bushing.TranslationalStiffness.y, 2e3, places=6)
        self.assertAlmostEqual(bushing.TranslationalStiffness.x, 1e6, places=6)
        self.assertAlmostEqual(bushing.RotationalStiffness.z, 3e6, places=6)
        self.assertAlmostEqual(bushing.Preload.x, 4e3, places=6)
        # Edited and cancelled
        self.assertTrue(bushing.ViewObject.doubleClicked())
        _process()
        task = CommandCreateBushing.activeTask
        self.assertIsInstance(task, CommandCreateBushing.TaskBushing)
        task.onGround()
        task.onFree()
        task.reject()
        _process()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertEqual(bushing.Other, self.cyl1)
        self.assertIsNotNone(bushing.Binding)
        self.assertClean()

    # ------------------------------------------------------------ contact sets

    def contactSets(self):
        return [o for o in self.doc.Objects if o.isDerivedFrom("Assembly::ContactSet")]

    def test_contact_set_panel(self):
        """Selected first, the components go in group A; the panel picks group
        B, a component picked into it leaves group A, and one picked again
        leaves the group; it takes the law -- from the materials, the stiffness
        and its slider idle, or a value, the radius idle -- and shows how a
        part rests on it; an edit cancelled leaves it as it was; a new study
        ticks it"""
        import CommandCreateContact

        self.select(self.box, "Face6")
        self.select(self.cyl1, "Face1")
        self.run_command("Assembly_CreateContact")
        task = CommandCreateContact.activeTask
        self.assertIsInstance(task, CommandCreateContact.TaskContact)
        self.assertIsNone(task.armed)
        contact = task.contact
        self.assertEqual(contact.GroupA, [self.box, self.cyl1])
        task.arm("B")
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Cyl2.Face1", 0, 32, 5)
        _process()
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Cyl1.Face1", 32, 0, 5)
        _process()
        self.assertEqual(contact.GroupA, [self.box])
        self.assertEqual(contact.GroupB, [self.cyl2, self.cyl1])
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Cyl1.Face1", 32, 0, 5)
        _process()
        self.assertEqual(contact.GroupB, [self.cyl2])
        self.assertEqual(task.armed, "B")
        self.assertEqual(task.groups["B"][0].count(), 1)
        self.assertEqual(Gui.Selection.getSelectionEx("", 0), [])
        self.assertEqual(task.boxes["Stiffness"].value(), 1e5)
        # From the materials: the stiffness is Hertz's, the radius chosen
        self.assertEqual(contact.StiffnessFrom, "Materials")
        self.assertFalse(task.boxes["Stiffness"].isEnabled())
        self.assertFalse(task.slider.isEnabled())
        self.assertTrue(task.boxes["RadiusFrom"].isEnabled())
        self.assertIn("N/mm^1.5", task.estimate.text())
        combo = task.boxes["RadiusFrom"]
        combo.setCurrentIndex(combo.findData("Part"))
        self.assertEqual(contact.RadiusFrom, "Part")
        # Damped by restitution: the damping's rows idle; by a value, the
        # restitution's
        self.assertEqual(contact.DampingFrom, "Restitution")
        self.assertTrue(task.boxes["Restitution"].isEnabled())
        self.assertFalse(task.boxes["MaxDamping"].isEnabled())
        task.boxes["Restitution"].setValue(0.8)
        self.assertAlmostEqual(contact.Restitution, 0.8, places=9)
        combo = task.boxes["DampingFrom"]
        combo.setCurrentIndex(combo.findData("Value"))
        self.assertEqual(contact.DampingFrom, "Value")
        self.assertFalse(task.boxes["Restitution"].isEnabled())
        self.assertTrue(task.boxes["MaxDamping"].isEnabled())
        self.assertTrue(task.boxes["Penetration"].isEnabled())
        # By an allowed penetration: the allowance and the exponent, the
        # stiffness theirs
        self.assertFalse(task.boxes["AllowedPenetration"].isEnabled())
        combo = task.boxes["StiffnessFrom"]
        combo.setCurrentIndex(combo.findData("Penetration"))
        self.assertEqual(contact.StiffnessFrom, "Penetration")
        self.assertTrue(task.boxes["AllowedPenetration"].isEnabled())
        self.assertTrue(task.boxes["Exponent"].isEnabled())
        self.assertFalse(task.boxes["Stiffness"].isEnabled())
        self.assertFalse(task.slider.isEnabled())
        self.assertFalse(task.boxes["RadiusFrom"].isEnabled())
        self.assertIn("sinks 0.01 mm", task.estimate.text())
        # A value: the stiffness and its slider, ten times the stiffness a
        # hundred ticks on
        combo = task.boxes["StiffnessFrom"]
        combo.setCurrentIndex(combo.findData("Value"))
        self.assertEqual(contact.StiffnessFrom, "Value")
        self.assertTrue(task.boxes["Stiffness"].isEnabled())
        self.assertTrue(task.slider.isEnabled())
        self.assertFalse(task.boxes["RadiusFrom"].isEnabled())
        self.assertEqual(task.slider.value(), 500)
        before = task.estimate.text()
        task.slider.setValue(600)
        self.assertAlmostEqual(contact.Stiffness, 1e6, delta=1.0)
        self.assertNotEqual(task.estimate.text(), before)
        task.boxes["Stiffness"].setValue(2e4)
        self.assertEqual(task.slider.value(), 430)
        task.boxes["StaticFriction"].setValue(0.5)
        self.accept_task()
        self.assertEqual(self.contactSets(), [contact])
        self.assertAlmostEqual(contact.Stiffness, 2e4, places=9)
        self.assertAlmostEqual(contact.StaticFriction, 0.5, places=9)
        self.assertEqual(contact.ViewObject.TypeId, "AssemblyGui::ViewProviderContactSet")
        # Edited and cancelled
        self.assertTrue(contact.ViewObject.doubleClicked())
        _process()
        task = CommandCreateContact.activeTask
        self.assertIsInstance(task, CommandCreateContact.TaskContact)
        task.groups["B"][0].selectAll()
        task.onRemove("B")
        self.assertEqual(contact.GroupB, [])
        task.boxes["Exponent"].setValue(2.0)
        task.reject()
        _process()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertEqual(contact.GroupB, [self.cyl2])
        self.assertAlmostEqual(contact.Exponent, 1.5, places=9)
        # A new study's elements
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sims = self.proxies("Simulation")
        self.assertEqual(len(sims), 1)
        self.assertIn(contact, sims[0].Elements)
        self.assertClean()

    def test_contact_set_needs_group_a(self):
        """With nothing selected the panel picks group A; OK waits for it;
        Cancel deletes the contact set"""
        import CommandCreateContact

        self.run_command("Assembly_CreateContact")
        task = CommandCreateContact.activeTask
        self.assertEqual(task.armed, "A")
        task.arm(None)
        self.assertFalse(task.accept())
        _process()
        self.assertTrue(Gui.Control.activeDialog())
        self.assertEqual(task.armed, "A")
        task.reject()
        _process()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertEqual(self.contactSets(), [])
        self.assertClean()

    def test_cam_contact(self):
        """Idle until a Cam joint is selected; then a cam contact on it,
        selected, its law by an allowed penetration with no friction, ticked
        in the Dynamic study and not the Kinematic one; one undo takes it out"""
        j = self.jointgroup.newObject("App::FeaturePython", "Cam")
        JointObject.Joint(j, JointObject.JointTypes.index("Cam"))
        JointObject.ViewProviderJoint(j.ViewObject)
        sims = self.assembly.getGroup("Simulations", True)
        dynamic = sims.newObject("Assembly::Simulation", "Dynamic")
        dynamic.AnalysisType = "Dynamic"
        kinematic = sims.newObject("Assembly::Simulation", "Kinematic")
        _process()
        Gui.Selection.clearSelection()
        _process()
        self.assertFalse(Gui.Command.get("Assembly_CreateCamContact").isActive())
        Gui.Selection.addSelection(j)
        self.run_command("Assembly_CreateCamContact")
        cams = [o for o in self.contactSets() if o.isDerivedFrom("Assembly::CamContact")]
        self.assertEqual(len(cams), 1)
        cam = cams[0]
        self.assertEqual(cam.Joint, j)
        self.assertEqual(Gui.Selection.getSelection(), [cam])
        self.assertEqual(cam.StiffnessFrom, "Penetration")
        self.assertEqual(cam.StaticFriction, 0.0)
        self.assertIn(cam, dynamic.Elements)
        self.assertNotIn(cam, kinematic.Elements)
        self.doc.undo()
        _process()
        self.assertEqual(self.contactSets(), [])
        self.assertEqual(dynamic.Elements, [])
        self.assertClean()

    def test_cam_sensor_quantities(self):
        """A sensor on a cam contact can watch its pressure angle, in
        degrees, its slip, in mm/s, and its drive force and side load, by
        component; a profile contact all of them, its B the follower; a
        plain contact set none of them"""
        import CommandCreateContact
        import CommandCreateSimulation

        j = self.jointgroup.newObject("App::FeaturePython", "Cam")
        JointObject.Joint(j, JointObject.JointTypes.index("Cam"))
        JointObject.ViewProviderJoint(j.ViewObject)
        j.Detach1 = True
        j.Detach2 = True
        j.Reference1 = (self.cyl1, ["Edge1"])
        j.Reference2 = (self.box, ["Vertex1"])
        cam = CommandCreateContact.newCamContact(self.assembly, j)
        plain =CommandCreateContact.newContactSet(self.assembly, [self.box], [self.cyl1])
        profile = CommandCreateContact.newProfileContact(
            self.assembly, self.box, ["Face6"], self.cyl1, ["Face2"]
        )
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.AnalysisType = "Dynamic"

        dialog = CommandCreateSimulation.SensorDialog(self.assembly)
        dialog.sourceCombo.setCurrentIndex(dialog.sourceCombo.findData(plain))
        quantity = dialog.quantityCombo
        offered = [quantity.itemData(i) for i in range(quantity.count())]
        self.assertNotIn("CamPressureAngle", offered)
        self.assertNotIn("CamSideForce", offered)
        dialog.sourceCombo.setCurrentIndex(dialog.sourceCombo.findData(profile))
        offered = [quantity.itemData(i) for i in range(quantity.count())]
        for name in ("CamPressureAngle", "CamSlip", "CamDriveForce", "CamSideForce"):
            self.assertIn(name, offered)
        dialog.sourceCombo.setCurrentIndex(dialog.sourceCombo.findData(cam))
        offered = [quantity.itemData(i) for i in range(quantity.count())]
        self.assertIn("CamPressureAngle", offered)
        self.assertIn("CamSlip", offered)
        quantity.setCurrentIndex(quantity.findData("CamPressureAngle"))
        self.assertEqual(dialog.thresholdBox.suffix(), " deg")
        dialog.thresholdBox.setValue(20.0)
        obj = CommandCreateSimulation.newSensor(sim, cam, "ContactForce", 0.0)
        dialog.apply(obj)
        self.assertEqual(obj.Quantity, "CamPressureAngle")
        self.assertAlmostEqual(obj.Threshold.getValueAs("deg").Value, 20.0, places=9)
        self.assertEqual(obj.Label, cam.Label + ": Pressure angle above 20 deg")
        quantity.setCurrentIndex(quantity.findData("CamSlip"))
        self.assertEqual(dialog.thresholdBox.suffix(), " mm/s")
        self.assertFalse(dialog.componentCombo.isEnabled())
        # The force's parts along the follower's motion and across it,
        # vectors: a component or the size
        self.assertIn("CamDriveForce", offered)
        self.assertIn("CamSideForce", offered)
        quantity.setCurrentIndex(quantity.findData("CamSideForce"))
        self.assertEqual(dialog.thresholdBox.suffix(), " N")
        self.assertTrue(dialog.componentCombo.isEnabled())
        dialog.componentCombo.setCurrentIndex(dialog.componentCombo.findData("X"))
        dialog.thresholdBox.setValue(2.0)
        dialog.apply(obj)
        self.assertEqual(obj.Quantity, "CamSideForce")
        self.assertEqual(obj.Component, "X")
        self.assertEqual(obj.Label, cam.Label + ": Cam side load X above 2 N")
        # Its events' pair: of its cam and its follower, the joint's sides,
        # not of every component
        quantity.setCurrentIndex(quantity.findData("ContactEnd"))
        self.assertTrue(dialog.pairCombos[0].isEnabled())
        for combo in dialog.pairCombos:
            parts = [combo.itemData(i) for i in range(combo.count())]
            self.assertEqual(parts, [None, self.cyl1, self.box])
        self.assertClean()

    def test_profile_contact(self):
        """Idle until planar faces of two components are selected -- not one
        component's, not a curved face; then a profile contact on them,
        selected, its law by an allowed penetration with no friction, ticked
        in the Dynamic study and not the Kinematic one; one undo takes it out"""
        a = self.assembly.newObject("Part::Box", "Wheel")
        b = self.assembly.newObject("Part::Cylinder", "Pin")
        b.Placement.Base = App.Vector(20, 0, 0)
        sims = self.assembly.getGroup("Simulations", True)
        dynamic = sims.newObject("Assembly::Simulation", "Dynamic")
        dynamic.AnalysisType = "Dynamic"
        kinematic = sims.newObject("Assembly::Simulation", "Kinematic")
        self.doc.recompute()
        _process()
        command = Gui.Command.get("Assembly_CreateProfileContact")

        def select(*subs):
            Gui.Selection.clearSelection()
            for sub in subs:
                Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, sub)
            _process()

        select()
        self.assertFalse(command.isActive())
        select("Wheel.Face6")
        self.assertFalse(command.isActive())
        select("Wheel.Face6", "Wheel.Face5")
        self.assertFalse(command.isActive())
        select("Wheel.Face6", "Pin.Face1")
        self.assertFalse(command.isActive())
        select("Wheel.Face6", "Pin.Face2")
        self.run_command("Assembly_CreateProfileContact")
        contacts = [o for o in self.contactSets() if o.isDerivedFrom("Assembly::ProfileContact")]
        self.assertEqual(len(contacts), 1)
        contact = contacts[0]
        self.assertEqual((contact.ProfileA[0], list(contact.ProfileA[1])), (a, ["Face6"]))
        self.assertEqual((contact.ProfileB[0], list(contact.ProfileB[1])), (b, ["Face2"]))
        self.assertEqual(Gui.Selection.getSelection(), [contact])
        self.assertEqual(contact.StiffnessFrom, "Penetration")
        self.assertEqual(contact.StaticFriction, 0.0)
        self.assertIn(contact, dynamic.Elements)
        self.assertNotIn(contact, kinematic.Elements)
        self.doc.undo()
        _process()
        self.assertEqual(self.contactSets(), [])
        self.assertEqual(dynamic.Elements, [])
        self.assertClean()

    def test_flexible_body(self):
        """Idle until one component is selected -- here by a face of it in the
        assembly, as the 3D view picks it; then a flexible body of it,
        selected, ticked in the Dynamic study and neither the Static nor the
        Kinematic one, its panel open; OK keeps it. One undo takes it out"""
        import CommandCreateFlexibleBody

        sims = self.assembly.getGroup("Simulations", True)
        dynamic = sims.newObject("Assembly::Simulation", "Dynamic")
        dynamic.AnalysisType = "Dynamic"
        static = sims.newObject("Assembly::Simulation", "Static")
        static.AnalysisType = "Static"
        kinematic = sims.newObject("Assembly::Simulation", "Kinematic")
        _process()
        Gui.Selection.clearSelection()
        _process()
        self.assertFalse(Gui.Command.get("Assembly_CreateFlexibleBody").isActive())
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face1")
        self.run_command("Assembly_CreateFlexibleBody")
        task = CommandCreateFlexibleBody.activeTask
        self.assertIsInstance(task, CommandCreateFlexibleBody.TaskFlexibleBody)
        self.accept_task()
        self.assertIsNone(CommandCreateFlexibleBody.activeTask)
        physics = self.assembly.getGroup("Physics")
        made = [o for o in physics.Group if o.TypeId == "Assembly::FlexibleBody"]
        self.assertEqual(len(made), 1)
        flexible = made[0]
        self.assertEqual(flexible.Component, self.box)
        self.assertEqual(Gui.Selection.getSelection(), [flexible])
        self.assertIn(flexible, dynamic.Elements)
        self.assertNotIn(flexible, static.Elements)
        self.assertNotIn(flexible, kinematic.Elements)
        self.assertEqual(flexible.Modes, 12)
        self.assertAlmostEqual(flexible.Damping, 0.02)
        self.doc.undo()
        _process()
        self.assertEqual([o for o in physics.Group if o.TypeId == "Assembly::FlexibleBody"], [])

    def test_flexible_body_cancelled(self):
        """The command's panel cancelled: no flexible body"""
        import CommandCreateFlexibleBody

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face1")
        self.run_command("Assembly_CreateFlexibleBody")
        self.assertIsNotNone(CommandCreateFlexibleBody.activeTask)
        Gui.Control.activeTaskDialog().reject()
        _process()
        physics = self.assembly.getGroup("Physics")
        group = physics.Group if physics else []
        self.assertEqual([o for o in group if o.TypeId == "Assembly::FlexibleBody"], [])
        self.assertFalse(Gui.Control.activeDialog())
        self.assertClean()

    def test_flexible_body_panel(self):
        """A flexible body's panel, as a double click opens it: its component
        and material; Modes, Damping and Mesh Size set the properties; Mesh
        Now meshes it, the hinge's interface listed; Show the Mesh draws its
        surface edges and the held nodes in the scene; a face of the part
        picked in the 3D view is its contact face, the mesh then of the part
        as it was, and a face of another part is not; Find Modes meshes it
        again and lists as many frequencies as modes, lowest first. Cancel
        undoes it all and takes the drawing out"""
        import CommandCreateFlexibleBody
        import FlexibleBodies

        base = self.box
        base.Length, base.Width, base.Height = 10, 10, 10
        rod = self.assembly.newObject("Part::Box", "Rod")
        rod.Length, rod.Width, rod.Height = 60, 10, 10
        rod.Placement.Base = App.Vector(10, 0, 0)
        ground = self.jointgroup.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        hinge = self.jointgroup.newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(hinge, JointObject.JointTypes.index("Revolute"))
        hinge.Detach1 = True
        hinge.Detach2 = True
        hinge.Reference1 = (base, ["Face2"])
        hinge.Reference2 = (rod, ["Face1"])
        turn = App.Rotation(App.Vector(1, 0, 0), -90)
        hinge.Placement1 = App.Placement(App.Vector(10, 5, 5), turn)
        hinge.Placement2 = App.Placement(App.Vector(0, 5, 5), turn)
        sims = self.assembly.getGroup("Simulations", True)
        dynamic = sims.newObject("Assembly::Simulation", "Dynamic")
        dynamic.AnalysisType = "Dynamic"
        self.doc.recompute()
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        self.doc.recompute()
        _process()
        self.assertTrue(flexible.ViewObject.doubleClicked())
        _process()
        task = CommandCreateFlexibleBody.activeTask
        self.assertIsInstance(task, CommandCreateFlexibleBody.TaskFlexibleBody)
        self.assertEqual(task.componentLabel.text(), rod.Label)
        self.assertIn("GPa", task.materialLabel.text())
        self.assertIn("Not meshed", task.stateLabel.text())
        self.assertTrue(task.findButton.isEnabled())
        task.modesBox.setValue(4)
        task.dampingBox.setValue(5.0)
        task.sizeBox.setProperty("rawValue", 5.0)
        _process()
        self.assertEqual(flexible.Modes, 4)
        self.assertAlmostEqual(flexible.Damping, 0.05)
        self.assertAlmostEqual(flexible.MeshSize.Value, 5.0)

        task.meshButton.click()
        _process()
        self.assertGreater(len(flexible.Nodes), 0)
        self.assertEqual(FlexibleBodies.meshState(flexible), "current")
        self.assertIn("%d nodes" % len(flexible.Nodes), task.stateLabel.text())
        self.assertIn(hinge.Label, task.interfacesLabel.text())

        # How its bending is drawn after a run: Automatic, or a scale. Not
        # the mesh's
        self.assertEqual(task.scaleBox.text(), "Automatic")
        task.scaleBox.setValue(1.0)
        _process()
        self.assertEqual(flexible.BendingScale, 1.0)
        self.assertEqual(task.scaleBox.text(), "x1.00")
        self.assertEqual(FlexibleBodies.meshState(flexible), "current")

        task.showMeshBox.setChecked(True)
        _process()
        scene = Gui.ActiveDocument.ActiveView.getSceneGraph()
        drawn = task.meshRoot
        self.assertIsNotNone(drawn)
        self.assertGreaterEqual(scene.findChild(drawn), 0)

        task.addButton.setChecked(True)
        _process()
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Rod.Face2")
        _process()
        self.assertEqual([(o, list(s)) for o, s in flexible.ContactFaces], [(rod, ["Face2"])])
        self.assertEqual(task.facesList.count(), 1)
        self.assertEqual(FlexibleBodies.meshState(flexible), "stale")
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face1")
        _process()
        self.assertEqual(task.facesList.count(), 1)
        task.addButton.setChecked(False)
        _process()

        task.findButton.click()
        _process()
        self.assertEqual(len(task.frequencies), 4, task.modesLabel.text())
        self.assertEqual(task.frequencies, sorted(task.frequencies))
        self.assertGreater(task.frequencies[0], 0.0)
        self.assertTrue(task.modesLabel.text().startswith("4 modes"))
        self.assertEqual(FlexibleBodies.meshState(flexible), "current")
        self.assertIn("Contact faces", task.interfacesLabel.text())

        Gui.Control.activeTaskDialog().reject()
        _process()
        self.assertIsNone(CommandCreateFlexibleBody.activeTask)
        self.assertEqual(scene.findChild(drawn), -1)
        self.assertEqual(flexible.Modes, 12)
        self.assertAlmostEqual(flexible.Damping, 0.02)
        self.assertEqual(flexible.MeshSize.Value, 0.0)
        self.assertEqual(len(flexible.ContactFaces), 0)
        self.assertEqual(len(flexible.Nodes), 0)

    def test_flexible_body_playback(self):
        """A run with a flexible body draws it bent as it plays: its mesh's
        surface in the scene, its component hidden, drawn at each frame the
        player shows, coloured by its stress, the scale's legend beside it
        and its largest stress in the Results box; the panel closed, the
        drawing and the legend go and the component shows again"""
        import CommandCreateSimulation
        import FlexibleBodies

        base = self.box
        base.Length, base.Width, base.Height = 10, 10, 10
        rod = self.assembly.newObject("Part::Box", "Rod")
        rod.Length, rod.Width, rod.Height = 60, 10, 10
        rod.Placement.Base = App.Vector(10, 0, 0)
        ground = self.jointgroup.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        hinge = self.jointgroup.newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(hinge, JointObject.JointTypes.index("Revolute"))
        hinge.Detach1 = True
        hinge.Detach2 = True
        hinge.Reference1 = (base, ["Face2"])
        hinge.Reference2 = (rod, ["Face1"])
        turn = App.Rotation(App.Vector(1, 0, 0), -90)
        hinge.Placement1 = App.Placement(App.Vector(10, 5, 5), turn)
        hinge.Placement2 = App.Placement(App.Vector(0, 5, 5), turn)
        self.doc.recompute()
        self.run_command("Assembly_CreateSimulation")
        self.accept_task()
        sim = self.proxies("Simulation")[0]
        sim.AnalysisType = "Dynamic"
        sim.TimeEnd = 0.01
        sim.TimeStepOutput = 1e-3
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.Modes = 4
        flexible.MeshSize = 5.0
        self.doc.recompute()
        _process()
        panel = CommandCreateSimulation.TaskAssemblyCreateSimulation(sim)
        Gui.Control.showDialog(panel)
        _process()
        panel.form.RunKinematicsButton.click()
        start = time.monotonic()
        while panel.runPoll.isActive():
            self.assertLess(time.monotonic() - start, 120.0)
            _process(1)
            time.sleep(0.01)
        _process()
        self.assertFalse(panel.runMessage.isVisibleTo(panel.form), panel.runMessage.text())
        self.assertGreater(len(flexible.Nodes), 0)
        self.assertEqual(len(panel.deformations), 1)
        shape = panel.deformations[0]
        self.assertIsNotNone(shape.root)
        self.assertFalse(rod.ViewObject.Visibility)
        scene = Gui.ActiveDocument.ActiveView.getSceneGraph()
        drawn = shape.root
        self.assertGreaterEqual(scene.findChild(drawn), 0)
        # At each frame shown, the surface there
        panel.form.frameSlider.setValue(0)
        _process()
        first = [tuple(p) for p in shape.coords.point.getValues()]
        panel.form.frameSlider.setValue(self.assembly.numberOfFrames() - 1)
        _process()
        last = [tuple(p) for p in shape.coords.point.getValues()]
        self.assertEqual(len(first), len(last))
        self.assertNotEqual(first, last)
        # Coloured by its stress, a colour a node, red at the largest over
        # the run; the legend's scale over the view's right side, and in a
        # saved video's frames at the same place
        colors = [tuple(c) for c in shape.material.diffuseColor.getValues()]
        self.assertEqual(len(colors), len(last))
        self.assertGreater(len(set(colors)), 10)
        peak = panel.deformations.peakStress()
        self.assertGreater(peak, 0.0)
        legend = panel.deformations.legend.widget
        viewer = Gui.ActiveDocument.ActiveView.graphicsView()
        self.assertTrue(legend.isVisibleTo(viewer))
        self.assertEqual(legend.parentWidget(), viewer)
        place = FlexibleBodies.Legend.place(viewer.width(), viewer.height())
        self.assertEqual(legend.geometry().getRect(), place)
        self.assertEqual(panel.deformations.legendOf(), (peak, shape.scale))
        words = FlexibleBodies.Legend.lines(peak, shape.scale)
        self.assertEqual(words[2], "%.4g" % peak)
        self.assertIn("x%.4g" % shape.scale, words[1])
        width, height = Gui.ActiveDocument.ActiveView.getSize()
        layout = panel.videoLayout(width, height)
        self.assertEqual(layout["legend"], (peak, shape.scale))
        # Painted: the bar's top red, its bottom blue
        image = QtGui.QImage(width, height, QtGui.QImage.Format_RGB32)
        image.fill(QtGui.QColor(255, 255, 255))
        painter = QtGui.QPainter(image)
        at = FlexibleBodies.Legend.place(width, height)
        FlexibleBodies.Legend.paint(painter, at, peak, shape.scale)
        painter.end()
        top = image.pixelColor(at[0] + 21, at[1] + 54)
        bottom = image.pixelColor(at[0] + 21, at[1] + at[3] - 16)
        self.assertGreater(top.red(), 200)
        self.assertLess(top.blue(), 60)
        self.assertGreater(bottom.blue(), 200)
        self.assertLess(bottom.red(), 60)
        # In Save Picture's picture (saveImage(overlays=True)) where it is on
        # the view, and twice the view's size twice as large; not without
        view = Gui.ActiveDocument.ActiveView
        left, up, _, tall = legend.geometry().getRect()

        def picture(name, scale, **kwds):
            path = os.path.join(self.tmpdir, name)
            view.saveImage(path, viewer.width() * scale, viewer.height() * scale, "White", **kwds)
            saved = QtGui.QImage(path)
            self.assertFalse(saved.isNull(), path)
            self.assertEqual(saved.width(), viewer.width() * scale)
            return saved

        def bar(saved, scale):
            # The bar's top and bottom, at their place on the view
            x = (left + 21) * scale
            top = saved.pixelColor(int(x), int((up + 54) * scale))
            bottom = saved.pixelColor(int(x), int((up + tall - 16) * scale))
            return top, bottom

        for scale in (1, 2):
            top, bottom = bar(picture("legend%d.png" % scale, scale, overlays=True), scale)
            self.assertGreater(top.red(), 200, scale)
            self.assertLess(top.blue(), 60, scale)
            self.assertGreater(bottom.blue(), 200, scale)
            self.assertLess(bottom.red(), 60, scale)
        top, bottom = bar(picture("bare.png", 1), 1)
        self.assertFalse(top.red() > 200 and top.blue() < 60)
        self.assertFalse(bottom.blue() > 200 and bottom.red() < 60)
        # The Results box: its largest at each frame
        at = panel.resultIndex((flexible.Name, "FlexibleVonMises"))
        self.assertGreaterEqual(at, 0)
        self.assertIn("Largest von Mises stress", panel.resultCombo.itemText(at))
        self.accept_task()
        _process()
        self.assertEqual(scene.findChild(drawn), -1)
        self.assertIsNone(panel.deformations.legend.widget)
        _process()
        self.assertEqual(viewer.findChildren(QtWidgets.QWidget, "FlexibleStressLegend"), [])
        self.assertTrue(rod.ViewObject.Visibility)

    def test_joint_friction(self):
        """Idle until a Slider, Cylindrical or Revolute joint is selected; then
        a friction on it, selected, ticked in the Dynamic study and neither the
        Static nor the Kinematic one, showing a slide's preload force and not
        a turn's radius and torque for a slider. One undo takes it out"""
        j = self.jointgroup.newObject("App::FeaturePython", "Slider")
        JointObject.Joint(j, JointObject.JointTypes.index("Slider"))
        JointObject.ViewProviderJoint(j.ViewObject)
        sims = self.assembly.getGroup("Simulations", True)
        dynamic = sims.newObject("Assembly::Simulation", "Dynamic")
        dynamic.AnalysisType = "Dynamic"
        static = sims.newObject("Assembly::Simulation", "Static")
        static.AnalysisType = "Static"
        kinematic = sims.newObject("Assembly::Simulation", "Kinematic")
        _process()
        Gui.Selection.clearSelection()
        _process()
        self.assertFalse(Gui.Command.get("Assembly_CreateJointFriction").isActive())
        Gui.Selection.addSelection(j)
        self.run_command("Assembly_CreateJointFriction")
        physics = self.assembly.getGroup("Physics")
        made = [o for o in physics.Group if o.isDerivedFrom("Assembly::JointFriction")]
        self.assertEqual(len(made), 1)
        friction = made[0]
        self.assertEqual(friction.Joint, j)
        self.assertEqual(Gui.Selection.getSelection(), [friction])
        self.assertIn(friction, dynamic.Elements)
        self.assertNotIn(friction, static.Elements)
        self.assertNotIn(friction, kinematic.Elements)

        def hidden(name):
            return "Hidden" in friction.getPropertyStatus(name)

        self.assertFalse(hidden("PreloadForce"))
        self.assertTrue(hidden("PinRadius"))
        self.assertTrue(hidden("PreloadTorque"))
        self.doc.undo()
        _process()
        self.assertEqual(
            [o for o in physics.Group if o.isDerivedFrom("Assembly::JointFriction")], []
        )
        self.assertEqual(dynamic.Elements, [])
        self.assertClean()

    def test_joint_stop(self):
        """Idle until a Slider, Cylindrical or Revolute joint is selected; then
        a penalty stop on it, selected, ticked in the Dynamic study and not the
        Kinematic one, showing only a slide's stiffness and damping for a
        slider; Rigid shows the restitution alone. One undo takes it out"""
        j = self.jointgroup.newObject("App::FeaturePython", "Slider")
        JointObject.Joint(j, JointObject.JointTypes.index("Slider"))
        JointObject.ViewProviderJoint(j.ViewObject)
        sims = self.assembly.getGroup("Simulations", True)
        dynamic = sims.newObject("Assembly::Simulation", "Dynamic")
        dynamic.AnalysisType = "Dynamic"
        kinematic = sims.newObject("Assembly::Simulation", "Kinematic")
        _process()
        Gui.Selection.clearSelection()
        _process()
        self.assertFalse(Gui.Command.get("Assembly_CreateJointStop").isActive())
        Gui.Selection.addSelection(j)
        self.run_command("Assembly_CreateJointStop")
        physics = self.assembly.getGroup("Physics")
        stops = [o for o in physics.Group if o.isDerivedFrom("Assembly::JointStop")]
        self.assertEqual(len(stops), 1)
        stop = stops[0]
        self.assertEqual(stop.Joint, j)
        self.assertEqual(Gui.Selection.getSelection(), [stop])
        self.assertEqual(stop.Mode, "Penalty")
        self.assertIn(stop, dynamic.Elements)
        self.assertNotIn(stop, kinematic.Elements)

        def hidden(name):
            return "Hidden" in stop.getPropertyStatus(name)

        self.assertFalse(hidden("Stiffness"))
        self.assertFalse(hidden("Damping"))
        self.assertTrue(hidden("TorsionalStiffness"))
        self.assertTrue(hidden("Restitution"))
        stop.Mode = "Rigid"
        self.assertFalse(hidden("Restitution"))
        self.assertTrue(hidden("Stiffness"))
        self.doc.undo()
        _process()
        self.assertEqual(
            [o for o in physics.Group if o.isDerivedFrom("Assembly::JointStop")], []
        )
        self.assertEqual(dynamic.Elements, [])
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
