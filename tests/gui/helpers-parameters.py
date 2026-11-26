"""The mesh helpers' parameters, read on the GUI thread (docs/WorkerProcess.md
sec 75).

A call to the helpers comes on any thread -- the pre-mesh's are the compute
pool's -- and a parameter group has no lock. The copies of the parameters a
helper is started with are made on the GUI thread: as the QApplication is
made, and again once the event loop turns after a change. So here:

  - a parameter set on the GUI thread, the loop let turn, and then the
    helpers' first call made from another thread: the copy they were
    started with holds it;
  - the call is a helper's (Worker counted), not one made here.
"""

import os
import threading
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
MARK = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/HelpersParametersTest")


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def run():
    try:
        before = Part.meshServiceInfo()
        check("no helper yet", before["Spawned"] == 0 and not before["Directory"], before)
        MARK.SetString("Mark", "set-on-the-gui-thread")
        pump(0.5)
        got = []

        def call():
            got.append(Part.meshShape(Part.makeBox(1, 1, 1), 0.1, 0.5, "worker"))

        thread = threading.Thread(target=call)
        thread.start()
        end = time.time() + 120
        while thread.is_alive() and time.time() < end:
            pump(0.05)
        after = Part.meshServiceInfo()
        check("the call from another thread is made by a helper",
              got == [True] and after["Worker"] == before["Worker"] + 1, (got, after))
        path = os.path.join(after["Directory"], "user.cfg")
        text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        check("the helpers' copy holds what was set before the call",
              "set-on-the-gui-thread" in text, (path, len(text)))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences").RemGroup("HelpersParametersTest")
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
