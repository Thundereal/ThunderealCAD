"""What every GUI test starts from, said before the test's own script runs.

scripts/gui-test.sh hands this to FreeCAD ahead of the test. A test runs on
a profile of its own, so it sees the defaults -- and a default is not a
thing a hundred tests should lean on without knowing: when one changes,
they all move.

  - View/AntiAliasing = 0. The default is MSAA 4x since 2026-10-09
    (docs/HandsOnQueue.md entry 51). The tests that read pixels were written
    against pictures without multisampling, and a multisampled view has no
    depth to read back: getRenderStats() answers geometryPixels -1 for it.
    A test about multisampling sets the value itself, which is after this.

  - Dialog/DontUseNativeColorDialog = true. A colour dialog the platform
    draws (macOS's NSColorPanel) is no QColorDialog a test can find and
    answer: its modal loop never returns (fillet-groups-and-preview-menu
    hung on the Mac mini, finding no dialog). Every colour dialog of the
    program takes this preference, so a test drives the same Qt dialog on
    every platform. It is also the setting's default; said here so a
    changed default does not move the tests.

  - FONTCONFIG_FILE is taken out of the environment. scripts/gui-test.sh
    states FreeCAD's font with it, and Qt has read it by now; a browser a test
    starts is not FreeCAD and keeps the box's own fonts.

  - Room for the 3D view. A view is never smaller than MDIView's minimum
    size hint, 400x300; in a central area smaller than that, the rest of it
    lies under the docks, where a click through the window lands on a dock
    instead (the Mac mini: a 1280x720 screen, the window opening at 640x519
    with a central area of 226x210). Only then: the window is maximized,
    and if the area is still short, the bottom docks are made as low as
    they go, and hidden as a last resort. A run whose area holds the view
    -- Windows' 860x594 view, Linux's 1280x1024 Xvfb -- is left exactly as
    it is; each run says what was found and done as a console message
    ("gui-test-profile: ..."), in the test's run.log and as a Msg line in
    FreeCAD.log -- not on stderr, which FreeCAD.log records as an error.

Only what is not stored yet is set. scripts/gui-test.sh starts every run
from a fresh profile, so that is what a test sees unless a --user-cfg is
handed over by some other means.
"""
import os

import FreeCAD

os.environ.pop("FONTCONFIG_FILE", None)

_view = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
if "AntiAliasing" not in _view.GetInts():
    _view.SetInt("AntiAliasing", 0)

_dialog = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Dialog")
if "DontUseNativeColorDialog" not in _dialog.GetBools():
    _dialog.SetBool("DontUseNativeColorDialog", True)


# the view's minimum, MDIView::minimumSizeHint()
VIEW_LEAST = (400, 300)


def _room_for_the_view():
    try:
        import FreeCADGui
        from PySide import QtCore, QtWidgets
    except ImportError:
        return
    mw = FreeCADGui.getMainWindow()
    if mw is None or mw.centralWidget() is None:
        return
    central = mw.centralWidget()

    def short():
        return central.width() < VIEW_LEAST[0] or central.height() < VIEW_LEAST[1]

    def settle(ms=400):
        loop = QtCore.QEventLoop()
        QtCore.QTimer.singleShot(ms, loop.quit)
        loop.exec()

    said = "central area %dx%d" % (central.width(), central.height())
    if not short():
        FreeCAD.Console.PrintMessage("gui-test-profile: %s holds the view: nothing done\n" % said)
        return
    done = []
    if not mw.isMaximized():
        mw.showMaximized()
        settle()
        done.append("maximized: %dx%d" % (central.width(), central.height()))
    bottom = [d for d in mw.findChildren(QtWidgets.QDockWidget)
              if d.isVisible() and not d.isFloating()
              and mw.dockWidgetArea(d) == QtCore.Qt.BottomDockWidgetArea]
    if short() and bottom:
        mw.resizeDocks(bottom, [1] * len(bottom), QtCore.Qt.Vertical)
        settle(200)
        done.append("bottom docks lowered: %dx%d" % (central.width(), central.height()))
    if short() and bottom:
        for dock in bottom:
            dock.hide()
        settle(200)
        done.append("bottom docks hidden (%s): %dx%d"
                    % (", ".join(d.objectName() for d in bottom),
                       central.width(), central.height()))
    FreeCAD.Console.PrintMessage(
        "gui-test-profile: %s, short of %dx%d; %s\n"
        % (said, VIEW_LEAST[0], VIEW_LEAST[1], "; ".join(done) or "nothing to do"))


_room_for_the_view()
