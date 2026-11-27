"""A workbench whose Initialize() asks for one of its own commands, with the
command-to-workbench record (Preferences/Commands) naming that workbench.

What PartDesign does: its Initialize() imports SprocketFeature, which asks
Gui.Command.get("PartDesign_Sprocket") before it adds the command. On a profile
whose record says PartDesign_Sprocket = PartDesignWorkbench -- every profile
that ever ran PartDesign before the record was dropped -- the lookup followed
the record and initialised PartDesign again, from inside its own Initialize():

  - a C++ workbench is created only when it is activated, so the once-only
    guard's "fresh" exception held and Initialize() ran a second time, nested;
    there the module was half imported, the command not yet there, and the
    Design Generators group was made without the Sprocket;
  - the nested call, ending, cleared the name of the workbench initialising,
    so the commands the outer Initialize() added after it were recorded under
    no workbench: the Sprocket's record was erased, at every start.

Found under a clone of the reporter's profile (docs/SmokeTesting.md, step 1):
Design Generators held the gear and the shaft wizard only, and the clone's
user.cfg lost Commands/PartDesign_Sprocket in every run.

Measured here with a workbench of the test's own, made as PartDesign is (a
Python handler naming a C++ workbench class, its command added by a module
Initialize() imports, after asking for it): Initialize() runs once, sees its
command, and both records -- the command asked for and one added after it --
still name the workbench. Before the fix: two runs, the first without the
command, and the records gone.
"""
import os
import sys
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
RECORD = "User parameter:BaseApp/Preferences/Commands"
WB = "GtRecordProbeWorkbench"
ASKED = "GtRecordProbe_Asked"
AFTER = "GtRecordProbe_After"
MODULE = "gt_record_probe_commands"
state = {"done": False, "inits": 0, "seen": []}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


# The module Initialize() imports: it asks for its command before adding it,
# as SprocketFeature.py does
COMMANDS = '''
import FreeCADGui


class Probe:
    def GetResources(self):
        return {"MenuText": "Record probe"}

    def Activated(self):
        pass

    def IsActive(self):
        return True


if not FreeCADGui.Command.get("%s"):
    FreeCADGui.addCommand("%s", Probe())
# and one more after it, as SprocketFeature's own follows InvoluteGear's
if "%s" not in FreeCADGui.listCommands():
    FreeCADGui.addCommand("%s", Probe())
''' % (ASKED, ASKED, AFTER, AFTER)


class GtRecordProbeWorkbench(FreeCADGui.Workbench):
    MenuText = "Record probe"

    def Initialize(self):
        state["inits"] += 1
        import gt_record_probe_commands  # noqa: F401

        state["seen"].append(ASKED in FreeCADGui.listCommands())

    def GetClassName(self):
        # a C++ workbench class, as PartDesign's handler names
        # PartDesignGui::Workbench: made only when it is activated
        return "Gui::StdWorkbench"


def run():
    try:
        with open(os.path.join(OUT, MODULE + ".py"), "w") as f:
            f.write(COMMANDS)
        sys.path.insert(0, OUT)
        record = FreeCAD.ParamGet(RECORD)
        # the record a session before left: both commands under the workbench
        record.SetString(ASKED, WB)
        record.SetString(AFTER, WB)
        FreeCADGui.addWorkbench(GtRecordProbeWorkbench())
        FreeCADGui.activateWorkbench(WB)
        pump(0.5)
        check("Initialize() runs once", state["inits"] == 1, state["inits"])
        check("and sees the command its module adds", state["seen"] == [True], state["seen"])
        check("the command is there", ASKED in FreeCADGui.listCommands())
        check("its record still names the workbench", record.GetString(ASKED) == WB,
              repr(record.GetString(ASKED)))
        check("so does the record of a command added after it",
              record.GetString(AFTER) == WB, repr(record.GetString(AFTER)))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        record = FreeCAD.ParamGet(RECORD)
        record.RemString(ASKED)
        record.RemString(AFTER)
        FreeCADGui.removeWorkbench(WB)
    except Exception:
        note("cleanup: " + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
