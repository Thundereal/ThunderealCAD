"""Nested links read from an older file open without listing every path
through them.

The smoke corpus, 2026-11-27 (finding B1): MonsterKossel/Assembly.FCStd,
an Assembly3 assembly of 187 KB linking thirty documents, spun for twenty
minutes in its open on the 2026-11-26 stage. Sampled, twice: a view
provider's Selectable read from the file called setSelectable(), whose
SoSearchAction over the root lists every path through it -- and a link's
root holds what it links, shared, so the paths multiply with each level.
A link whose record names an archive entry (an older file's MaterialList)
is restored during the load and its record replayed once the scene is
built (Gui::Document's drain, phase three), when the whole linked tree is
there to be searched, once per link. Reading true changes nothing --
SoFCSelection's defaults are setSelectable(true)'s -- so a restore that
reads true does not search.

Scene: a box and DEPTH levels of link arrays of FAN elements each, the
level above linking the one below, saved in schema 4 (an older file's
form, whose link records carry archive entries); one link saved with
Selectable false. Claims: the reopen ends in under BOUND s (the stage
took 44.4 s at depth 4, and did not end at depth 5; 0.3 s now), and each
link's Selectable is as it was saved.
"""
import os
import time
import traceback
import zipfile

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DEPTH = int(os.environ.get("GT_LINK_DEPTH", "4"))
FAN = 6
BOUND = 10.0
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtCore.QCoreApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)
        time.sleep(0.01)


def make(path):
    doc = FreeCAD.newDocument("NestedLinks")
    doc.SaveSchemaVersion = 4
    prev = doc.addObject("Part::Box", "Box")
    doc.recompute()
    for level in range(DEPTH):
        link = doc.addObject("App::Link", "L%d" % level)
        link.LinkedObject = prev
        link.ElementCount = FAN
        prev = link
    doc.recompute()
    for obj in doc.Objects:
        obj.Visibility = False
    # the top one shown: it builds the whole linked tree
    prev.Visibility = True
    doc.L1.ViewObject.Selectable = False
    pump(0.5)
    doc.saveAs(path)
    FreeCAD.closeDocument(doc.Name)
    pump(0.5)


def run():
    path = os.path.join(OUT, "nested-links.FCStd")
    make(path)
    gui = zipfile.ZipFile(path).read("GuiDocument.xml").decode("utf-8")
    start = gui.index('<ViewProvider name="L0"')
    check("an older file's link record names an archive entry",
          ' file="' in gui[start : gui.index("</ViewProvider>", start)])
    began = time.time()
    FreeCADGui.open(path)
    doc = FreeCAD.ActiveDocument
    while (doc.Restoring or doc.LiveImport or FreeCADGui.isBuildingVisuals()) \
            and time.time() - began < BOUND * 30:
        pump(0.1)
    took = time.time() - began
    note("INFO %d levels of %d links opened in %.1f s" % (DEPTH, FAN, took))
    check("nested links open in under %d s" % BOUND, took < BOUND, "%.1f s" % took)
    got = {"L%d" % k: doc.getObject("L%d" % k).ViewObject.Selectable for k in range(DEPTH)}
    want = {name: name != "L1" for name in got}
    check("each link is selectable as it was saved", got == want, got)


def main():
    try:
        run()
    except Exception:  # pylint: disable=broad-except
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(0, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1000, main)
