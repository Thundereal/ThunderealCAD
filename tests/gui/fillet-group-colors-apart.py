"""A fillet's groups' automatic colours stand apart from each other.

docs/HandsOnQueue.md entry 209, the user 2026-10-21: "the group color auto
selection shall pick distintct colors. right now the group 2 and 3 has very
similar color. is it picking random color or a fixed list of color? Make it
fix if possible". Each group's hue was stepped from its id alone, clear of
the handle's own colours but not of the other groups'.

A box with a fillet on eight of its edges, each with a radius of its own:
groups 1 to 8. In edit. Claims:
  - eight group colours on the arrows, besides the plain one;
  - every two of them at least 0.04 apart in hue, and each at least that
    from the plain, selected, pointer and dragged colours;
  - the same colours after the edit is left and opened again (fixed).

Runs with QT_QPA_PLATFORM=offscreen.
"""
import colorsys
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "FilletGroupColorsApart"
APART = 0.04


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


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def arrow_colours():
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    root = view.getViewer().getSoRenderManager().getSceneGraph()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName("SoLinearDraggerContainer"))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    colours = []
    for p in search.getPaths():
        container = p.getTail()
        if field_text(container, "visible") not in ("TRUE", "1"):
            continue
        dragger = container.getPart("dragger", False)
        if dragger is not None:
            c = dragger.getField("color").getValue()
            colours.append(tuple(round(v, 3) for v in (c[0], c[1], c[2])))
    return colours


def packed(name, default):
    value = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").GetUnsigned(name, default)
    return ((value >> 24) & 0xff) / 255.0, ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0


def hue_gap(a, b):
    d = abs(colorsys.rgb_to_hsv(*a)[0] - colorsys.rgb_to_hsv(*b)[0])
    return min(d, 1.0 - d)


def group_colours():
    plain = (1.0, 0.0, 0.0)
    return sorted(set(c for c in arrow_colours() if c != plain))


def run():
    try:
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 20
        doc.recompute()
        edges = ["Edge%d" % i for i in range(1, 9)]
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, edges)
        fillet.Radius = 1
        fillet.Segments = [(e, [(0.0, 0.5 + 0.1 * i)]) for i, e in enumerate(edges)]
        doc.recompute()
        check("the fillet is valid", fillet.isValid(), fillet.getStatusString())
        FreeCADGui.SendMsgToActiveView("ViewFit")
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(fillet, 0)
        pump(80)
        own = group_colours()
        note("INFO group colours %s" % own)
        check("eight group colours besides the plain one", len(own) == 8, own)
        pairs = [(a, b, round(hue_gap(a, b), 3)) for i, a in enumerate(own) for b in own[i + 1:]]
        close = [p for p in pairs if p[2] < APART]
        check("every two group colours at least %.2f apart in hue" % APART, not close, close)
        taken = {"plain": (1.0, 0.0, 0.0), "selected": packed("SelectionColor", 0x1cad1cff),
                 "pointer": packed("HighlightColor", 0xe1e114ff), "dragged": (1.0, 1.0, 0.0)}
        near = [(c, k, round(hue_gap(c, t), 3)) for c in own for k, t in taken.items()
                if hue_gap(c, t) < APART]
        check("each at least %.2f from the handle's own colours" % APART, not near, near)
        gdoc.resetEdit()
        pump(40)
        gdoc.setEdit(fillet, 0)
        pump(80)
        again = group_colours()
        check("the same colours when opened again", again == own, (own, again))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        try:
            FreeCADGui.getDocument(DOC).resetEdit()
            pump()
        except Exception:
            pass
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
