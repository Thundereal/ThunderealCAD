"""gui-test-guard.py: the script scripts/gui-test.cmd hands FreeCAD on Windows.

Runs the GUI test script named by GT_SCRIPT and ends the session whatever it does: a
script that raises before it arms its own timer, or that never writes DONE, would
otherwise leave a FreeCAD window open -- on the hidden desktop nobody would ever see it.

Environment: GT_SCRIPT, GT_RESULT, GT_TIMEOUT (seconds, 300), GT_PROFILE (the profile
the tests start from, scripts/gui-test-profile.py; GT_NO_PROFILE=1 skips it).
"""
import os
import sys
import threading
import traceback

import FreeCADGui
from PySide import QtCore

SCRIPT = os.environ["GT_SCRIPT"]
RESULT = os.environ["GT_RESULT"]
LIMIT = int(os.environ.get("GT_TIMEOUT", "300"))


def bail(why):
    try:
        with open(RESULT, "a") as f:
            f.write("FAIL guard: %s\nDONE\n" % why)
    finally:
        os._exit(3)


# the event loop may be held by the very thing that hangs: a thread as well as a timer
_thread = threading.Timer(LIMIT + 30, bail, ("no end after %d s (thread)" % (LIMIT + 30),))
_thread.daemon = True
_thread.start()
_timer = QtCore.QTimer()
_timer.setSingleShot(True)
_timer.timeout.connect(lambda: bail("no end after %d s" % LIMIT))
_timer.start(LIMIT * 1000)

# what scripts/gui-test.sh hands FreeCAD ahead of every test: the profile the tests start from
PROFILE = os.environ.get("GT_PROFILE", "")
if PROFILE and os.path.exists(PROFILE) and not os.environ.get("GT_NO_PROFILE"):
    with open(PROFILE, encoding="utf-8") as f:
        exec(compile(f.read(), PROFILE, "exec"), {"__name__": "__main__", "__file__": PROFILE})

# FreeCAD puts the directory of a script it is handed on the path; this one is handed to
# exec, so its helpers beside it (tests/gui/wsclient.py) would not be found
sys.path.insert(0, os.path.dirname(os.path.abspath(SCRIPT)))

try:
    with open(SCRIPT, encoding="utf-8") as f:
        code = compile(f.read(), SCRIPT, "exec")
    exec(code, {"__name__": "__main__", "__file__": SCRIPT})
except Exception:
    with open(RESULT, "a") as f:
        f.write(
            "FAIL guard: the script did not start | "
            + traceback.format_exc().replace("\n", " | ")
            + "\nDONE\n"
        )
    QtCore.QTimer.singleShot(0, FreeCADGui.getMainWindow().close)
