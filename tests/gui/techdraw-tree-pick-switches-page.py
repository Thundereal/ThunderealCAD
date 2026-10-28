"""A tree pick of a TechDraw object brings its page view to the front.

docs/HandsOnQueue.md entry 217 (the reporter, 2026-10-23): "when techdraw
dimension (or other object) is selected in the tree, it does not switch to
that page view even when Std_TreeSyncView is enabled. a related request,
when double click to edit techdraw object, switch to its own page view
regardless of Std_TreeSyncView"; then "if tree view sync is on, switch to
the page even it is closed. Note, this applies to select the page object
itself".

A box; two pages, each with a view of it; a distance dimension on the
first page's view. Both page views open, the second in front. Claims, with
real clicks on the tree's rows:
  - sync on: a click on the dimension brings the first page's view to the
    front;
  - sync on, the first page's view closed: a click on the dimension opens
    it, in front; a click on the page itself does the same;
  - sync off, the view closed: a click on the dimension opens nothing;
  - sync off: a double click on the dimension opens the first page's view,
    in front (and its edit).

Runs offscreen as well.
"""
import glob
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
TREE = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/TreeView")
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
DOC = "TreePickPage"

state = {"done": False, "sync": None, "area": None}


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
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.02)


def mw():
    return FreeCADGui.getMainWindow()


def tree():
    for w in mw().findChildren(QtWidgets.QTreeWidget):
        if w.metaObject().className() == "Gui::TreeWidget" and w.isVisible():
            return w
    return None


def row(label):
    w = tree()
    items = w.findItems(label, Qt.MatchExactly | Qt.MatchRecursive, 0)
    return items[0] if items else None


def click_row(label, double=False):
    w = tree()
    item = row(label)
    if item is None:
        return False
    # the tree makes a row's children anew when it is expanded: the row is
    # found again after
    chain = []
    p = item.parent()
    while p is not None:
        chain.append(p.text(0))
        p = p.parent()
    for text in reversed(chain):
        parent = row(text)
        if parent is not None and not parent.isExpanded():
            parent.setExpanded(True)
            settle(0.3)
    item = row(label)
    if item is None:
        return False
    w.scrollToItem(item)
    settle(0.3)
    at = w.visualItemRect(item).center()
    if double:
        QtTest.QTest.mouseDClick(w.viewport(), Qt.LeftButton, Qt.NoModifier, at)
    else:
        QtTest.QTest.mouseClick(w.viewport(), Qt.LeftButton, Qt.NoModifier, at)
    settle(1.5)
    return True


def page_views(gdoc):
    return gdoc.mdiViewsOfType("TechDrawGui::MDIViewPage")


def page_of(view):
    try:
        return view.getPage().Name
    except Exception:
        return None


def front():
    """The page shown by the view in front, or the kind of view it is"""
    v = FreeCADGui.activeView()
    if v is None:
        return None
    return page_of(v) or type(v).__name__


def open_pages(gdoc):
    return sorted(filter(None, (page_of(v) for v in page_views(gdoc))))


def close_page_view(gdoc, name):
    for v in page_views(gdoc):
        if page_of(v) == name:
            for sub in mw().findChildren(QtWidgets.QMdiSubWindow):
                if sub.widget() is not None and sub.widget().metaObject().className() == "TechDrawGui::MDIViewPage" \
                        and sub.windowTitle() and name in sub.windowTitle():
                    sub.close()
    FreeCADGui.Selection.clearSelection()
    settle(1.5)


def show_page(page):
    page.ViewObject.doubleClicked()
    settle(1.5)


def make_page(doc, name, box, template):
    page = doc.addObject("TechDraw::DrawPage", name)
    tmpl = doc.addObject("TechDraw::DrawSVGTemplate", name + "Template")
    if template:
        tmpl.Template = template
    page.Template = tmpl
    view = doc.addObject("TechDraw::DrawViewPart", name + "View")
    page.addView(view)
    view.Source = [box]
    return page, view


def run():
    try:
        state["sync"] = TREE.GetBool("SyncView", True)
        # Both page views open at once, each in a tab of its own: in a view
        # area a page view opened takes the cell of the one before (the
        # reuse step, ViewPlacement.cpp), and the first one's view would be
        # closed, not behind. Off before the document makes its 3D view.
        state["area"] = VIEW.GetBool("UseViewArea", True)
        VIEW.SetBool("UseViewArea", False)
        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        doc.recompute()
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"), recursive=True)
        template = templates[0] if templates else None
        page1, view1 = make_page(doc, "PageA", box, template)
        page2, view2 = make_page(doc, "PageB", box, template)
        doc.recompute()
        settle(5)
        dim = doc.addObject("TechDraw::DrawViewDimension", "Dimension")
        dim.Type = "Distance"
        dim.References2D = [(view1, "Edge0")]
        page1.addView(dim)
        doc.recompute()
        settle(3)
        gdoc = FreeCADGui.getDocument(DOC)
        show_page(page1)
        show_page(page2)
        note("INFO open page views %s, in front %s" % (open_pages(gdoc), front()))
        if not check("both page views open, PageB's in front",
                     open_pages(gdoc) == ["PageA", "PageB"] and front() == "PageB",
                     (open_pages(gdoc), front())):
            return

        TREE.SetBool("SyncView", True)
        click_row(dim.Label)
        check("sync on: a click on the dimension brings PageA's view to the front",
              front() == "PageA", front())

        show_page(page2)
        close_page_view(gdoc, "PageA")
        check("PageA's view closed", "PageA" not in open_pages(gdoc), open_pages(gdoc))
        click_row(dim.Label)
        check("sync on, PageA's view closed: a click on the dimension opens it, in front",
              "PageA" in open_pages(gdoc) and front() == "PageA", (open_pages(gdoc), front()))

        show_page(page2)
        close_page_view(gdoc, "PageA")
        click_row(page1.Label)
        check("sync on, closed: a click on the page itself opens its view, in front",
              "PageA" in open_pages(gdoc) and front() == "PageA", (open_pages(gdoc), front()))

        TREE.SetBool("SyncView", False)
        show_page(page2)
        close_page_view(gdoc, "PageA")
        click_row(dim.Label)
        check("sync off, closed: a click on the dimension opens nothing",
              "PageA" not in open_pages(gdoc) and front() == "PageB", (open_pages(gdoc), front()))

        click_row(dim.Label, double=True)
        settle(1.5)
        check("sync off: a double click on the dimension opens PageA's view, in front",
              "PageA" in open_pages(gdoc) and front() == "PageA", (open_pages(gdoc), front()))
        try:
            FreeCADGui.Control.closeDialog()
        except Exception:
            pass
        gdoc.resetEdit()
        settle(1)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    if state["sync"] is not None:
        TREE.SetBool("SyncView", state["sync"])
    if state["area"] is not None:
        VIEW.SetBool("UseViewArea", state["area"])
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
