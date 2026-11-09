"""A view in a tab closed of itself, not through its tab (MDIView::closeEvent):
a script's close() of the view widget, an object's view going with the object.

Claims:
  - the tab goes with the view: none is left standing empty;
  - what follows -- other tabs shown and maximized, which asks every 3D
    view whether it is behind another (MDIView::isBackgroundView) -- does
    not read the view that is gone (it was a segmentation fault);
  - the sheet opens again in a tab of its own.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
OPEN_VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/OpenView")
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()
    for _ in range(5):
        QtCore.QCoreApplication.processEvents()


def tabs():
    area = FreeCADGui.getMainWindow().findChild(QtWidgets.QMdiArea)
    return [(sw.windowTitle(),
             None if sw.widget() is None else sw.widget().metaObject().className())
            for sw in area.subWindowList()]


R = {}


def run():
    try:
        # A tab of its own, not a cell of the document's area
        OPEN_VIEW.SetString("DocViewTarget", "Tab")
        doc = R["doc"] = FreeCAD.newDocument("ViewCloseDirect")
        doc.addObject("Part::Box", "Box")
        sheet = R["sheet"] = doc.addObject("Spreadsheet::Sheet", "Sheet")
        doc.recompute()
        sheet.ViewObject.doubleClicked()
        settle(1000)
        check("the sheet is a tab of its own",
              ("Sheet[*]", "SpreadsheetGui::SheetView") in tabs()
              or ("Sheet", "SpreadsheetGui::SheetView") in tabs(), tabs())
        for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QMainWindow):
            if w.metaObject().className() == "SpreadsheetGui::SheetView":
                w.close()
        settle(500)
        # On from the main loop: the view's deferred delete runs there
        QtCore.QTimer.singleShot(500, after)
        return
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def after():
    try:
        doc, sheet = R["doc"], R["sheet"]
        check("no tab is left without its view",
              all(cls is not None for _, cls in tabs()), tabs())
        check("the sheet's tab went with it",
              not any(cls == "SpreadsheetGui::SheetView" for _, cls in tabs()), tabs())
        # Window state changes: every tab shown maximized in turn
        mw = FreeCADGui.getMainWindow()
        area = mw.findChild(QtWidgets.QMdiArea)
        for sw in area.subWindowList():
            sw.showNormal()
            settle(100)
            sw.showMaximized()
            settle(100)
        check("window state changes after it", True)
        sheet.ViewObject.doubleClicked()
        settle(1000)
        gdoc = FreeCADGui.getDocument(doc.Name)
        check("the sheet opens again",
              len(gdoc.mdiViewsOfType("SpreadsheetGui::SheetView")) == 1,
              tabs())
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    OPEN_VIEW.RemString("DocViewTarget")
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
