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


# The main window no bigger than scripts/gui-test.sh's xvfb screen, 1280x1024: maximized
# on a 5120x1440 monitor a 3D view is 4700x930, and tests that pick, zoom or count pixels
# were written against the other shape (a sketch marker 12x17 where it is 7, a pick that
# misses). A maximum size, not a restored window: Windows keeps a maximized window within
# it, so a test that maximizes still has a maximized window, and its restore rect is its
# own. A test that sizes the window past it asks for that size (X lets a window be bigger
# than the screen; mw.resize(1400, 900)), so the bound gives way to it.
# GT_SCREEN=WxH another size, GT_SCREEN=full the whole monitor.
_screen = os.environ.get("GT_SCREEN", "1280x1024").lower()
if _screen != "full":
    _sw, _sh = (int(v) for v in _screen.split("x"))
    _mw = FreeCADGui.getMainWindow()
    _maximized = _mw.isMaximized()
    if _maximized:
        _mw.showNormal()
    _mw.setMaximumSize(_sw, _sh)
    if _maximized:
        _mw.showMaximized()
    else:
        # the window FreeCAD restores is the monitor's size, unmaximized: on the screen
        _avail = _mw.screen().availableGeometry()
        _frame = _mw.frameGeometry()
        _mw.move(_mw.x() + _avail.x() - _frame.x(), _mw.y() + _avail.y() - _frame.y())

    def _lifted(call):
        def sized(*args):
            r = QtCore.QRect(*args) if len(args) == 4 else None
            size = r.size() if r else (QtCore.QSize(*args) if len(args) == 2 else args[-1])
            if hasattr(size, "size"):
                size = size.size()
            if size.width() > _sw or size.height() > _sh:
                _mw.setMaximumSize(16777215, 16777215)
            return call(*args)

        return sized

    _mw.resize = _lifted(_mw.resize)
    _mw.setGeometry = _lifted(_mw.setGeometry)

# On the user's screen (gui-test.cmd's GT_DESKTOP=screen) the main window over every other
# window, so that what a test reads off the screen is its own: Windows does not let a
# process that is not in the foreground take it (raise_() and activateWindow() asked, and
# a terminal stayed in front), but a topmost window is above the others without that.
# Only the z-order: the window is not activated and not re-created.
if sys.platform == "win32" and os.environ.get("GT_DESKTOP", "").lower() == "screen":
    import ctypes

    _HWND_TOPMOST, _SWP_NOSIZE, _SWP_NOMOVE, _SWP_NOACTIVATE = -1, 0x1, 0x2, 0x10
    ctypes.windll.user32.SetWindowPos(
        ctypes.c_void_p(int(FreeCADGui.getMainWindow().winId())), ctypes.c_void_p(_HWND_TOPMOST),
        0, 0, 0, 0, _SWP_NOSIZE | _SWP_NOMOVE | _SWP_NOACTIVATE)

# GT_CHECKS (scripts/gui-test.cmd): what has to hold whatever the test does, recorded for
# scripts/gui-test-checks.py -- every message box shown, with its text, one left up for
# 5 s closed so that the run goes on; and at quit, the documents still open.
if os.environ.get("GT_CHECKS"):
    from PySide import QtWidgets

    _OUTDIR = os.path.dirname(RESULT)
    _boxes = {}

    def _checks_write(name, line):
        with open(os.path.join(_OUTDIR, name), "a", encoding="utf-8") as f:
            f.write(line.replace("\n", " ") + "\n")

    def _box_sweep():
        # Not an event filter: one on the application would run Python for every
        # event and slow the tests it watches. The tests' own sweepers close a box
        # at 100 to 200 ms, after this has seen it.
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMessageBox) and w.isVisible() and id(w) not in _boxes:
                _boxes[id(w)] = [w, 0]
                _checks_write("checks-dialogs.txt", "%s | %s | %s" % (
                    w.windowTitle(), w.text(), w.informativeText()))
        for key, entry in list(_boxes.items()):
            box = entry[0]
            try:
                visible = box.isVisible()
            except RuntimeError:
                visible = False
            if not visible:
                del _boxes[key]
                continue
            entry[1] += 1
            if entry[1] >= 100:
                _checks_write("checks-dialogs.txt", "LEFT UP 5 s, closed | %s" % box.text())
                box.close()
                del _boxes[key]

    _box_timer = QtCore.QTimer()
    _box_timer.timeout.connect(_box_sweep)
    _box_timer.start(50)

    def _at_quit():
        import FreeCAD

        _checks_write("checks-end.txt", "documents open at quit: %s"
                      % sorted(FreeCAD.listDocuments().keys()))

    QtWidgets.QApplication.instance().aboutToQuit.connect(_at_quit)

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
