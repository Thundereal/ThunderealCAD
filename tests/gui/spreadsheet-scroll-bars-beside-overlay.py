"""A spreadsheet's scroll bars stay usable beside overlay panels: laid over the cells, out of the docks' way, thin and fading.

docs/HandsOnQueue.md entry 222 (the reporter, 2026-10-23): "spreadsheet has
the same problem, but it does need scroll bar for easy scrolling. Suggest a
way to coexist with overlay panels"; then "222, A and B". (A) the bars stand
in the area the overlaid docks leave free; (B) they are thin and
see-through, shown while scrolling or with the pointer near, fading after.

A sheet with a cell far out (Z300), its view open. Claims:
  - the view's own scroll bars are off; the laid-over ones follow their
    ranges;
  - a scroll shows the bars, and after a moment they are gone again;
  - a bar moved scrolls the sheet;
  - every dock overlaid, the report view shown at the bottom: after a
    scroll, neither bar stands under a shown overlaid dock.

Runs offscreen as well.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))

state = {"done": False, "overlay": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def mw():
    return FreeCADGui.getMainWindow()


def table():
    for w in mw().findChildren(QtWidgets.QTableView):
        if w.isVisible() and w.metaObject().className() == "SpreadsheetGui::SheetTableView":
            return w
    return None


def ours(view, name):
    for b in view.findChildren(QtWidgets.QScrollBar):
        if b.objectName() == name:
            return b
    return None


def global_rect(w):
    return QtCore.QRect(w.mapToGlobal(QtCore.QPoint(0, 0)), w.size())


def overlaid_docks():
    out = []
    for w in mw().findChildren(QtWidgets.QWidget):
        if w.metaObject().className() == "Gui::OverlayTabWidget" and w.isVisible() and w.count():
            out.append(w)
    return out


def run():
    try:
        doc = FreeCAD.newDocument("SheetScrollBars")
        sheet = doc.addObject("Spreadsheet::Sheet", "Sheet")
        sheet.set("A1", "1")
        sheet.set("Z300", "2")
        doc.recompute()
        sheet.ViewObject.doubleClicked()
        settle(2)
        view = table()
        if not check("the sheet's view is open", view is not None):
            return
        v, h = ours(view, "OverlayScrollBarV"), ours(view, "OverlayScrollBarH")
        own_v, own_h = view.verticalScrollBar(), view.horizontalScrollBar()
        check("the view's own scroll bars are off",
              view.verticalScrollBarPolicy() == QtCore.Qt.ScrollBarAlwaysOff
              and view.horizontalScrollBarPolicy() == QtCore.Qt.ScrollBarAlwaysOff
              and not own_v.isVisible() and not own_h.isVisible())
        check("the laid-over bars follow their ranges",
              v is not None and h is not None and v.maximum() == own_v.maximum() > 0
              and h.maximum() == own_h.maximum() > 0,
              (v.maximum() if v else None, own_v.maximum(), h.maximum() if h else None,
               own_h.maximum()))

        QtWidgets.QApplication.instance().processEvents()
        own_v.setValue(own_v.maximum() // 3)
        settle(0.4)
        check("a scroll shows the bars", v.isVisible() and h.isVisible(),
              (v.isVisible(), h.isVisible()))
        settle(2.5)
        check("... and after a moment they are gone", not v.isVisible() and not h.isVisible(),
              (v.isVisible(), h.isVisible()))

        h.setValue(h.maximum() // 2)
        settle(0.3)
        check("a laid-over bar moved scrolls the sheet", own_h.value() == h.value(),
              (h.value(), own_h.value()))

        FreeCADGui.runCommand("Std_DockOverlayAll")
        state["overlay"] = True
        settle(2)
        report = mw().findChild(QtWidgets.QDockWidget, "Report view")
        if report is not None:
            report.show()
        settle(2)
        docks = overlaid_docks()
        note("INFO overlaid docks shown: %s" % [global_rect(d).getRect() for d in docks])
        own_v.setValue(own_v.maximum() // 2)
        settle(0.5)
        under = []
        for bar in (v, h):
            if not bar.isVisible():
                continue
            r = global_rect(bar)
            for d in docks:
                if r.intersects(global_rect(d)):
                    under.append((bar.objectName(), r.getRect(), global_rect(d).getRect()))
        note("INFO bars at %s, %s" % (global_rect(v).getRect(), global_rect(h).getRect()))
        check("every dock overlaid: there are docks over the view", bool(docks), len(docks))
        check("... and neither bar stands under one", not under, under)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    if state["overlay"]:
        try:
            FreeCADGui.runCommand("Std_DockOverlayAll")
        except Exception:
            pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
