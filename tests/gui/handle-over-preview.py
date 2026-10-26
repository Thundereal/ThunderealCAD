"""A handle draws over a dress-up's on-top preview (entry 185).

The reporter 2026-10-19 15:49: "the handle is displayed after the preview
shape. It should draw above it, i.e. with higher show-on-top priority". A
10 mm box, one edge filleted 3 mm, in edit: the edge's arrow stands at the
fillet, where the preview's face is. The gizmo container's 3D annotation is
given its old priority (0) and then its new one (-1, over the preview's -2,
under the labels' 1) from the script, a render dump taken each time:
  - more of the arrow's own colour shows with -1 than with 0 (the preview no
    longer covers it);
  - the container's priority is -1 as built.
Pictures: priority0.png, priority-1.png.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "HandleOverPreview"

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def find_all(type_name):
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    root = view.getViewer().getSoRenderManager().getSceneGraph()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName(type_name))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    return [p.getTail() for p in search.getPaths()]


def colour_pixels(path, rgb):
    img = QtGui.QImage(path)
    r, g, b = rgb
    count = 0
    for y in range(0, img.height(), 2):
        for x in range(0, img.width(), 2):
            c = QtGui.QColor(img.pixel(x, y))
            if abs(c.red() - r) < 40 and abs(c.green() - g) < 40 and abs(c.blue() - b) < 40:
                count += 1
    return count


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1"])
        fillet.Radius = 3
        doc.recompute()
        view = FreeCADGui.getDocument(DOC).ActiveView
        view.viewIsometric()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(fillet, 0)
        pump(120)
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump(60)
        check("the fillet is in edit", gdoc.getInEdit() is not None)

        # registered under its full name (the editing root's path reads
        # "SoFCEditingRoot > So3DAnnotation"), unlike Coin's own types
        annotations = [n for n in find_all("So3DAnnotation")]
        note("INFO 3D annotations in the scene: %d, priorities %s"
             % (len(annotations), [field_text(n, "priority") for n in annotations]))
        if not check("the gizmo container's annotation is there", len(annotations) >= 1):
            return
        check("... its priority is -1 as built",
              any(field_text(n, "priority") == "-1" for n in annotations),
              [field_text(n, "priority") for n in annotations])
        arrows = find_all("SoLinearDraggerContainer")
        colours = [field_text(a, "color") for a in arrows if field_text(a, "visible") in ("TRUE", "1")]
        note("INFO arrow colours %s" % colours)
        if not check("an arrow is shown", bool(colours)):
            return
        rgb = tuple(int(float(v) * 255) for v in colours[0].split()[:3])

        counts = {}
        for priority in ("0", "-1"):
            for n in annotations:
                n.getField("priority").set(priority)
            view.redraw() if hasattr(view, "redraw") else None
            pump(60)
            path = os.path.join(OUT, "priority%s.png" % priority)
            view.saveRenderDump(path, metadata=False)
            counts[priority] = colour_pixels(path, rgb)
        note("INFO arrow-coloured pixels: priority 0 -> %d, -1 -> %d" % (counts["0"], counts["-1"]))
        check("over the preview: more of the arrow shows with -1 than with 0",
              counts["-1"] > counts["0"] + 5, counts)
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
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
