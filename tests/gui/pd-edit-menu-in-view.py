"""The edit's menu and its button (entries 171 and 173).

docs/OnViewValueEditing.md sec 8: one menu for the right click on a handle,
Space and an always shown menu button at the left of the top-middle label;
it holds the value's own entries, "Show handles" (this view), the panel's
"Show preview", and a "Settings" sub-menu of the six drag settings as
widgets written to their preferences at once. A pad and a box, each in a
body of its own:
  - Pad in edit: the menu button is shown, as tall as a label, at the left
    of the view's middle (where the label "Length" stands);
  - a click on it opens the menu: "Show handles", "Show preview",
    "Settings"; the sub-menu has two check boxes, two combo boxes and two
    spin boxes;
  - in the shown sub-menu, a click on "Enable coarse snapping while
    dragging" flips the preference and the sub-menu stays open; the
    multiplier set to 7 and the default drag to "Fine" are the preferences
    at once;
  - "Show handles" off: no handle shown in the view; Tab still makes the
    length active, and its label is at the top; on again: shown;
  - "Show preview" from the menu: the panel's box follows, and so does
    PreviewOnEdit; back again;
  - Space with no value active opens the menu;
  - Box in edit (no menu of its own before): Space gives "Show handles" and
    "Settings".

Menus are answered from a timer. The test home is a throwaway one: the
preferences it writes are its own.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets
from PySide.QtCore import Qt

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "PDEditMenu"
GIZMOS = "User parameter:BaseApp/Preferences/Gui/Gizmos"

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.01)


def mw():
    return FreeCADGui.getMainWindow()


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def viewer_widget():
    return view().graphicsView()


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def text_labels():
    from pivy import coin

    search = coin.SoSearchAction()
    search.setName(coin.SbName("OnViewRoot"))
    search.setInterest(coin.SoSearchAction.FIRST)
    search.apply(view().getAuxSceneGraph())
    path = search.getPath()
    if not path:
        return []
    labels = coin.SoSearchAction()
    labels.setType(coin.SoType.fromName("SoDatumLabel"))
    labels.setInterest(coin.SoSearchAction.ALL)
    labels.setSearchingAll(True)
    labels.apply(path.getTail())
    return [field_text(p.getTail(), "string").strip().strip('"') for p in labels.getPaths()
            if field_text(p.getTail(), "datumtype") == "TEXT"]


def handles_shown():
    """How many handle containers in the view are switched on"""
    from pivy import coin

    root = view().getViewer().getSoRenderManager().getSceneGraph()
    shown = 0
    for kind in ("SoLinearDraggerContainer", "SoRotationDraggerContainer"):
        search = coin.SoSearchAction()
        search.setType(coin.SoType.fromName(kind))
        search.setInterest(coin.SoSearchAction.ALL)
        search.setSearchingAll(True)
        search.apply(root)
        for p in search.getPaths():
            if field_text(p.getTail(), "visible") in ("TRUE", "1"):
                shown += 1
    return shown


def real_key(widget, k):
    QtTest.QTest.keyPress(widget, k, Qt.NoModifier)
    pump(5)
    target = QtWidgets.QApplication.focusWidget() or widget
    QtTest.QTest.keyRelease(target, k, Qt.NoModifier)
    pump(10)


def key(k):
    real_key(viewer_widget(), k)


def no_active():
    """Nothing active: Tab makes one, Escape takes it back"""
    viewer_widget().setFocus()
    pump(5)
    key(Qt.Key_Tab)
    key(Qt.Key_Escape)


def shown_menu():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(w, QtWidgets.QMenu) and w.isVisible():
            return w
    return None


def with_menu(open_it, handler):
    """open_it() opens the menu; handler(menu) runs on it from a timer and
    closes it unless it returns False. Returns what handler returned."""
    seen = {"result": None, "done": False}

    def answer():
        w = shown_menu()
        if w is None:
            QtCore.QTimer.singleShot(100, answer)
            return
        try:
            seen["result"] = handler(w)
        except Exception:
            note("ABORT in menu " + traceback.format_exc().replace("\n", " | "))
        seen["done"] = True
        if w.isVisible():
            w.close()

    QtCore.QTimer.singleShot(300, answer)
    open_it()
    for _ in range(60):
        if seen["done"]:
            break
        pump(10)
    pump(40)
    return seen["result"]


def items(menu):
    return [a.text().replace("&", "") for a in menu.actions()
            if not a.isSeparator() and a.text()]


def action(menu, text):
    for a in menu.actions():
        if a.text().replace("&", "") == text:
            return a
    return None


def menu_button():
    for w in mw().findChildren(QtWidgets.QWidget, "OnViewMenuButton"):
        if w.isVisible():
            return w
    return None


def preview_box():
    for w in mw().findChildren(QtWidgets.QCheckBox):
        if w.isVisible() and w.text().replace("&", "") == "Show preview":
            return w
    return None


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        gizmos = FreeCAD.ParamGet(GIZMOS)
        gizmos.SetBool("EnableCoarseSnap", True)
        gizmos.SetInt("CoarseLinearSnapMultiplier", 5)
        gizmos.SetInt("DefaultCoarseDragBehavior", 0)
        mw().activateWindow()
        doc = FreeCAD.newDocument(DOC)

        b1 = doc.addObject("PartDesign::Body", "BodyPad")
        sk = b1.newObject("Sketcher::SketchObject", "Sketch")
        pts = [FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(10, 0, 0),
               FreeCAD.Vector(10, 10, 0), FreeCAD.Vector(0, 10, 0)]
        for i in range(4):
            sk.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % 4]), False)
        pad = b1.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sk
        pad.Length = 5
        b2 = doc.addObject("PartDesign::Body", "BodyBox")
        box = b2.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        box.Placement.Base = FreeCAD.Vector(30, 0, 0)
        doc.recompute()
        FreeCADGui.activeDocument().activeView().viewIsometric()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump()

        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(pad, 0)
        pump(120)
        check("the pad is in edit", gdoc.getInEdit() is not None)
        note("INFO labels %s" % text_labels())

        # The button
        btn = menu_button()
        if not check("the menu button is shown", btn is not None):
            return
        vw = btn.parentWidget()
        geo = btn.geometry()
        note("INFO button %s in a %dx%d view" % (geo, vw.width(), vw.height()))
        check("... as tall as a label (16 to 60 px), square",
              16 <= geo.height() <= 60 and geo.width() == geo.height(), geo)
        check("... left of the view's middle, near the top",
              geo.right() < vw.width() // 2 and geo.top() < 60, geo)
        # a picture of it beside its label, from the screen (a GL view's own
        # grab is empty)
        def picture(name):
            """The button rendering itself, on a dark grey like a view's top,
            four times up: a screen grab shows whatever window is in front on
            a desktop in use, and the GL view under it renders empty"""
            from PySide import QtGui
            img = QtGui.QImage(btn.size(), QtGui.QImage.Format_ARGB32)
            img.fill(QtGui.QColor(60, 63, 70))
            btn.render(img, QtCore.QPoint(), QtGui.QRegion(),
                       QtWidgets.QWidget.RenderFlag.DrawChildren)
            img.scaled(img.width() * 4, img.height() * 4).save(os.path.join(OUT, name))

        try:
            from PySide import QtGui
            picture("button.png")
            local = QtCore.QPointF(geo.width() / 2, geo.height() / 2)
            QtCore.QCoreApplication.sendEvent(
                btn, QtGui.QEnterEvent(local, local, QtCore.QPointF(btn.mapToGlobal(local.toPoint()))))
            pump(5)
            picture("button-hover.png")
            QtCore.QCoreApplication.sendEvent(btn, QtCore.QEvent(QtCore.QEvent.Leave))
            pump(5)
        except Exception as e:
            note("INFO no picture: %r" % e)

        # Its menu
        def read_menu(menu):
            names = items(menu)
            settings = action(menu, "Settings")
            kinds = {}
            if settings is not None and settings.menu() is not None:
                for a in settings.menu().actions():
                    w = a.defaultWidget() if isinstance(a, QtWidgets.QWidgetAction) else None
                    if w is None:
                        continue
                    for cls in (QtWidgets.QCheckBox, QtWidgets.QComboBox, QtWidgets.QSpinBox):
                        found = [w] if isinstance(w, cls) else w.findChildren(cls)
                        kinds[cls.__name__] = kinds.get(cls.__name__, 0) + len(found)
            return names, kinds

        # The settings, changed in the shown sub-menu. In the same opening as
        # the reading above: a widget of an earlier menu, deleted with it,
        # can leave PySide a stale wrapper at an address the next menu's
        # widget takes ("already deleted" on an object just found)
        def settings_work(menu):
            out = {"read": read_menu(menu)}
            sub = action(menu, "Settings").menu()
            # opened from its parent, as a user does (a popup() of its own
            # closes the parent, whose widgets go with it)
            menu.setActiveAction(action(menu, "Settings"))
            pump(5)
            QtTest.QTest.keyClick(menu, Qt.Key_Right)
            for _ in range(30):
                if sub.isVisible():
                    break
                pump(5)
            out["open"] = sub.isVisible()
            if not out["open"]:
                return out
            coarse = sub.findChild(QtWidgets.QCheckBox, "OnViewSettingEnableCoarseSnap")
            spin = sub.findChild(QtWidgets.QSpinBox, "OnViewSettingCoarseLinearSnapMultiplier")
            combo = sub.findChild(QtWidgets.QComboBox, "OnViewSettingDefaultCoarseDragBehavior")
            out["found"] = (coarse is not None, spin is not None, combo is not None)
            before = FreeCAD.ParamGet(GIZMOS).GetBool("EnableCoarseSnap", True)
            if coarse is not None:
                QtTest.QTest.mouseClick(coarse, Qt.LeftButton,
                                        Qt.NoModifier, QtCore.QPoint(8, coarse.height() // 2))
                pump(20)
            out["coarse"] = (before, FreeCAD.ParamGet(GIZMOS).GetBool("EnableCoarseSnap", True))
            out["still_open"] = sub.isVisible()
            if spin is not None:
                spin.setValue(7)
                pump(5)
            out["mult"] = FreeCAD.ParamGet(GIZMOS).GetInt("CoarseLinearSnapMultiplier", 5)
            if combo is not None:
                combo.setCurrentIndex(1)
                pump(5)
            out["fine"] = FreeCAD.ParamGet(GIZMOS).GetInt("DefaultCoarseDragBehavior", 0)
            sub.close()
            return out

        out = with_menu(lambda: QtTest.QTest.mouseClick(btn, Qt.LeftButton), settings_work) or {}
        names, kinds = out.get("read") or ([], {})
        note("INFO menu %s; settings widgets %s" % (names, kinds))
        check("a click on the button opens the menu with Show handles, Show preview, Settings",
              all(n in names for n in ("Show handles", "Show preview", "Settings")), names)
        check("... the Settings sub-menu: 2 check boxes, 2 combo boxes, 2 spin boxes",
              kinds.get("QCheckBox") == 2 and kinds.get("QComboBox") == 2
              and kinds.get("QSpinBox") == 2, kinds)
        note("INFO settings %s" % {k: v for k, v in out.items() if k != "read"})
        c = out.get("coarse", (None, None))
        check("a click on the coarse check box flips the preference at once",
              c[0] is not None and c[0] != c[1], c)
        check("... and the sub-menu stays open", out.get("still_open") is True, out)
        check("the multiplier set to 7 is the preference at once", out.get("mult") == 7, out)
        check("the default drag set to Fine is the preference at once", out.get("fine") == 1, out)

        # Show handles
        shown_before = handles_shown()
        with_menu(lambda: QtTest.QTest.mouseClick(btn, Qt.LeftButton),
                  lambda m: action(m, "Show handles").trigger())
        pump(40)
        hidden = handles_shown()
        note("INFO handles shown before %d, after Show handles off %d" % (shown_before, hidden))
        check("Show handles off: no handle shown in the view", shown_before > 0 and hidden == 0,
              (shown_before, hidden))
        viewer_widget().setFocus()
        key(Qt.Key_Tab)
        labels = text_labels()
        check("... Tab still makes the length active, its label at the top",
              any(t.startswith("Length") for t in labels), labels)
        key(Qt.Key_Escape)
        checked = with_menu(lambda: QtTest.QTest.mouseClick(btn, Qt.LeftButton),
                            lambda m: action(m, "Show handles").isChecked())
        check("... the menu shows it off", checked is False, checked)
        with_menu(lambda: QtTest.QTest.mouseClick(btn, Qt.LeftButton),
                  lambda m: action(m, "Show handles").trigger())
        pump(40)
        check("Show handles on again: shown", handles_shown() == shown_before, handles_shown())

        # Show preview: the panel's box
        pbox = preview_box()
        if check("the panel has its Show preview box", pbox is not None):
            was = pbox.isChecked()
            with_menu(lambda: QtTest.QTest.mouseClick(btn, Qt.LeftButton),
                      lambda m: action(m, "Show preview").trigger())
            pump(80)
            pref = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Part").GetBool(
                "PreviewOnEdit", True)
            check("Show preview from the menu: the panel's box follows, and PreviewOnEdit",
                  pbox.isChecked() != was and pref == pbox.isChecked(),
                  (was, pbox.isChecked(), pref))
            with_menu(lambda: QtTest.QTest.mouseClick(btn, Qt.LeftButton),
                      lambda m: action(m, "Show preview").trigger())
            pump(80)
            check("... and back", pbox.isChecked() == was, pbox.isChecked())

        # The button's menu below the button, even with a handle active (entry
        # 173, the reporter 14:09: it "popups near the handle far from the
        # menu button")
        viewer_widget().setFocus()
        key(Qt.Key_Tab)
        below = btn.mapToGlobal(QtCore.QPoint(0, btn.height()))
        at = with_menu(lambda: QtTest.QTest.mouseClick(btn, Qt.LeftButton),
                       lambda m: m.geometry().topLeft())
        note("INFO with a handle active, the button's menu at %s, the button's bottom left %s"
             % (at, below))
        check("with a handle active, the button's menu opens below the button",
              at is not None and abs(at.x() - below.x()) <= 40 and abs(at.y() - below.y()) <= 40,
              (at, below))
        key(Qt.Key_Escape)

        # Space with nothing active
        no_active()
        names = with_menu(lambda: key(Qt.Key_Space), items) or []
        check("Space with no value active opens the menu", "Settings" in names, names)

        gdoc.resetEdit()
        pump(80)
        check("out of the pad's edit: no menu button left", menu_button() is None)

        # The box: no menu of its own before
        gdoc.setEdit(box, 0)
        pump(120)
        check("the box is in edit", gdoc.getInEdit() is not None)
        viewer_widget().setFocus()
        key(Qt.Key_Tab)
        names = with_menu(lambda: key(Qt.Key_Space), items) or []
        check("the box's menu by Space: Show handles and Settings",
              "Show handles" in names and "Settings" in names, names)
        key(Qt.Key_Escape)
        gdoc.resetEdit()
        pump(80)
        check("still running, out of the edit", gdoc.getInEdit() is None)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.getDocument(DOC).resetEdit()
        pump()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
