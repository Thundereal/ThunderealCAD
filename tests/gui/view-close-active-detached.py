"""A closed view does not stay the active view with no document.

docs/HandsOnQueue.md entry 132: TechDraw pages exported to PDF, and the
session crashed in the menus' update -- MainWindow::_updateActions asked the
active view whether it could redo, and MDIViewPage::onHasMsg read its document
through a null pointer. d9c3193856 guarded that read; this is why there was
no document.

A view whose document has another view is detached at its close
(MDIView::closeEvent): out of the document's list, its document pointer
cleared. The main window's active view is a QPointer, and let go of it only
when the view was deleted -- a deferred delete, 110-180 ms later on the box it
was measured on (scanner.FCStd, a probe polling every 2 ms). Nothing moved
the active view in between unless Qt activated another sub window, which it
need not. The same after a document closed: its 3D view stayed the active
view with no document.

A poller here reads, every 2 ms, the main window's active view against every
view each open document lists. Claims:
  - a page's view, the active one, closed with the document's 3D view open:
    no moment where the active view is in no document, and the 3D view is
    the active one after;
  - a page exported to PDF (which opens its view) and its view closed at
    once, as a script exporting pages does: the same;
  - a second document, its page view active, closed: the same, and the first
    document's 3D view is the active one after;
  - the last document closed: never a stale one, and what is active after
    is none, or a view no document held (the Start page).

Runs with QT_QPA_PLATFORM=offscreen as well as on a screen; the pointer is not
moved.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
STEPS = []
S = {"stale": [], "step": "start", "polls": 0, "done": False}


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


def mw():
    return FreeCADGui.getMainWindow()


def listed_ids():
    ids = set()
    for name in FreeCAD.listDocuments():
        gdoc = FreeCADGui.getDocument(name)
        if gdoc is not None:
            ids.update(v.ID for v in gdoc.mdiViewsOfType("Gui::MDIView"))
    return ids


def active_id():
    act = mw().getActiveWindow()
    return act.ID if act is not None else None


def poll():
    """The state 132 needs: an active view that was a document's and that no
    document lists now. A view no document ever held (the Start page) has
    none to lose."""
    if S["done"]:
        return
    S["polls"] += 1
    try:
        vid = active_id()
        listed = listed_ids()
        S.setdefault("held", set()).update(listed)
        if vid is not None and vid in S["held"] and vid not in listed:
            if not S["stale"] or S["stale"][-1] != (S["step"], vid):
                S["stale"].append((S["step"], vid))
    except Exception:
        note("poll error: " + traceback.format_exc()[-300:])
    QtCore.QTimer.singleShot(2, poll)


def stale_in(step):
    return [s for s in S["stale"] if s[0] == step]


def view3d_id(doc):
    return FreeCADGui.getDocument(doc.Name).mdiViewsOfType("Gui::View3DInventor")[0].ID


def page_views(doc):
    return FreeCADGui.getDocument(doc.Name).mdiViewsOfType("TechDrawGui::MDIViewPage")


def make_doc(name):
    doc = FreeCAD.newDocument(name)
    box = doc.addObject("Part::Box", "Box")
    page = doc.addObject("TechDraw::DrawPage", "Page")
    template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    template.Template = os.path.join(
        FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates", "A4_Landscape_blank.svg")
    page.Template = template
    view = doc.addObject("TechDraw::DrawViewPart", "View")
    page.addView(view)
    view.Source = [box]
    doc.recompute()
    return doc


def first():
    S["step"] = "close"
    doc = make_doc("CloseActiveA")
    S["a"] = doc
    S["a3d"] = view3d_id(doc)
    doc.getObject("Page").ViewObject.doubleClicked()
    settle(1500)
    pages = page_views(doc)
    check("a page's view is open and the active view",
          len(pages) == 1 and active_id() == pages[0].ID,
          (len(pages), active_id(), [p.ID for p in pages]))
    pages[0].close()
    settle(800)
    check("closed: never the active view with no document", not stale_in("close"),
          stale_in("close"))
    check("... and the document's 3D view is the active one after",
          active_id() == S["a3d"], (active_id(), S["a3d"]))


def second():
    import TechDrawGui

    S["step"] = "export"
    doc = S["a"]
    TechDrawGui.exportPageAsPdf(doc.getObject("Page"), os.path.join(OUT, "page.pdf"))
    opened = len(page_views(doc))
    for v in page_views(doc):
        v.close()
    settle(800)
    check("a page exported to PDF opens its view", opened == 1, opened)
    check("its view closed at once: never the active view with no document",
          not stale_in("export"), stale_in("export"))
    check("... and the 3D view is the active one after", active_id() == S["a3d"],
          (active_id(), S["a3d"]))


def third():
    S["step"] = "closedoc"
    doc = make_doc("CloseActiveB")
    doc.getObject("Page").ViewObject.doubleClicked()
    settle(1500)
    pages = page_views(doc)
    check("a second document's page view is the active view",
          len(pages) == 1 and active_id() == pages[0].ID, (active_id(), len(pages)))
    FreeCAD.closeDocument(doc.Name)
    settle(800)
    check("that document closed: never an active view with no document",
          not stale_in("closedoc"), stale_in("closedoc"))
    check("... and the first document's 3D view is the active one after",
          active_id() == S["a3d"], (active_id(), S["a3d"]))


def fourth():
    S["step"] = "closelast"
    FreeCAD.closeDocument(S["a"].Name)
    settle(800)
    check("the last document closed: never an active view with no document",
          not stale_in("closelast"), stale_in("closelast"))
    check("... and the active view after is none, or one no document held",
          active_id() is None or active_id() not in S["held"],
          (active_id(), repr(mw().getActiveWindow())))


def finish():
    S["done"] = True
    note("INFO polls %d" % S["polls"])
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


def advance():
    if not STEPS:
        finish()
        return
    step = STEPS.pop(0)
    try:
        step()
    except Exception:
        note("ABORT in %s: %s" % (step.__name__, traceback.format_exc().replace("\n", " | ")))
        STEPS[:] = []
    QtCore.QTimer.singleShot(500, advance)


STEPS[:] = [first, second, third, fourth]
QtCore.QTimer.singleShot(2, poll)
QtCore.QTimer.singleShot(2500, advance)
