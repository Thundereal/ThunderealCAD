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

    def test_results_not_unique(self):
        """The Results box marks a redundant joint's reaction "(not unique)",
        and says why in its tool tip; a unique one is listed plain"""
        import CommandCreateSimulation
        from PySide import QtCore, QtWidgets

        class Results:
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
            pass

        panel = Panel()
        panel.assembly = Results()
        panel.resultCombo = QtWidgets.QComboBox()
        panel.resultsBox = QtWidgets.QWidget()
        panel.showResult = lambda: None
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
        leaves the group; it takes the law; an edit cancelled leaves it as it
        was; a new study ticks it"""
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
        task.boxes["Stiffness"].setValue(2e4)
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
