"""An older file whose looks are taken from its view providers opens in
about the time the same file saved now does.

The smoke corpus, 2026-11-27 (finding B2): MiSTer_binary.FCStd (18199
objects, 0.22) did not finish opening in 18 minutes on the 2026-11-26
stage. Sampled, the main thread was in
ViewProviderPartExt::finishAppearance: for each object whose looks were
taken from its view provider, the whole recursive in-list was gathered and
dependency-sorted to make again what was made from it -- inside the read,
one object at a time, every object's in-list again. The pass made once
the document is read (settleAdopted(), 1bcd48431b) makes them all, what
the others are made from first; that stays.

Scene: a chain of N Part features, each linking the one before, saved
and then stripped of ElementAppearance -- what a file from before the
looks were the object's is -- with the first one's colour set in its view
provider. Opening a chain this long costs its time whatever the file
(the same chain saved by this build is the measure); what taking the
looks adds was quadratic: at 1500 objects the stage opened the older
file in 26.5 s against 15.0 s for the newer, 16.2 s now. Claims: the
older file opens within 1.2 times the newer one's, plus 1 s, and the
colour set is the object's look once it has (what 39c692c6c5 was for).
GT_OLDER_LOOKS_N sets the chain's length (1500).
"""
import os
import re
import time
import traceback
import zipfile

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
N = int(os.environ.get("GT_OLDER_LOOKS_N", "1500"))
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)
RED = (1.0, 0.0, 0.0)


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


def reading(doc):
    # isBuildingVisuals() counts a progressive load's view providers still
    # being made, after the App has read the document
    return doc.Restoring or doc.LiveImport or FreeCADGui.isBuildingVisuals()


def strip(block):
    """A <Properties> block without ElementAppearance, its count one less"""
    count, body = int(block.group(1)), block.group(2)
    body, n = re.subn(r'<Property name="ElementAppearance".*?</Property>\s*', "", body,
                      flags=re.S)
    return '<Properties Count="%d"%s</Properties>' % (count - n, body)


def drop(block, names):
    """`block` without the properties `names`, its Properties count less as many"""
    n = 0
    for name in names:
        block, k = re.subn(r'<Property name="%s".*?</Property>\s*' % name, "", block, flags=re.S)
        n += k
    count = re.search(r'<Properties Count="(\d+)"', block)
    return block.replace(count.group(0), '<Properties Count="%d"' % (int(count.group(1)) - n), 1)


def red(block):
    """F0's view provider as a file of 0.20 keeps its looks: a ShapeColor,
    red, and none of what came after it"""
    block = drop(block, ("ShapeAppearance", "DiffuseColor", "ShapeMaterial"))
    color = r'\g<1>%d' % 0xFF0000FF
    block, n = re.subn(r'(<Property name="ShapeColor"[^>]*>\s*<PropertyColor value=")\d+',
                       color, block)
    if n:
        return block
    count = re.search(r'<Properties Count="(\d+)"', block)
    block = block.replace(count.group(0), '<Properties Count="%d"' % (int(count.group(1)) + 1), 1)
    at = block.rindex("</Properties>")
    return (block[:at] + '<Property name="ShapeColor" type="App::PropertyColor">\n'
            '<PropertyColor value="%d"/>\n</Property>\n' % 0xFF0000FF + block[at:])


def make(path, older=True):
    doc = FreeCAD.newDocument("OlderLooks")
    doc.BlobCompression = "Deflate"  # what Python's zipfile reads
    # the standard format: each object's properties its own, as a file of
    # 0.22 has them (5 puts the defaults in one block)
    doc.SaveSchemaVersion = 4
    box = Part.makeBox(1, 1, 1)
    prev = doc.addObject("Part::Feature", "F0")
    prev.Shape = box
    for i in range(1, N):
        obj = doc.addObject("Part::Feature", "F%d" % i)
        obj.Shape = box
        obj.addProperty("App::PropertyLink", "Dep")
        obj.Dep = prev
        prev = obj
    pump(0.5)
    tmp = path[:-6] + "-new.FCStd"
    doc.saveAs(tmp)
    FreeCAD.closeDocument(doc.Name)
    pump(0.5)
    if not older:
        os.replace(tmp, path)
        return
    # An older file: no ElementAppearance, the first one's colour in its
    # view provider only
    with zipfile.ZipFile(tmp) as zin, zipfile.ZipFile(path, "w") as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "Document.xml":
                text = data.decode("utf-8")
                text = re.sub(r'<Properties Count="(\d+)"(.*?)</Properties>', strip, text,
                              flags=re.S)
                # nor the object's own material, which a file of 0.20 had not
                start = text.index('<Object name="F0"', text.index("<ObjectData"))
                end = text.index("</Object>", start)
                text = text[:start] + drop(text[start:end], ("ShapeMaterial",)) + text[end:]
                data = text.encode("utf-8")
            elif item.filename == "GuiDocument.xml":
                text = data.decode("utf-8")
                start = text.index('<ViewProvider name="F0"')
                end = text.index("</ViewProvider>", start)
                text = text[:start] + red(text[start:end]) + text[end:]
                data = text.encode("utf-8")
            zout.writestr(item, data)
    os.remove(tmp)


def opened(path):
    """The document at `path` opened, and how long until it is all made"""
    began = time.time()
    FreeCADGui.open(path)
    doc = FreeCAD.ActiveDocument
    while reading(doc) and time.time() - began < 600:
        pump(0.1)
    pump(0.5)
    return doc, time.time() - began


def run():
    # The same chain saved by this build: what opening it costs without
    # any looks to take, the measure of the older file's
    newer = os.path.join(OUT, "newer-looks.FCStd")
    make(newer, older=False)
    doc, base = opened(newer)
    FreeCAD.closeDocument(doc.Name)
    pump(0.5)
    path = os.path.join(OUT, "older-looks.FCStd")
    make(path)
    doc, took = opened(path)
    note("INFO %d objects: the older file opened in %.1f s, the same saved now in %.1f s"
         % (len(doc.Objects), took, base))
    # The stage at 1500: 1.7 times the newer file's (1.6 at 1000), quadratic
    # in the objects taken
    check("taking an older file's looks costs no more than a fraction of its open",
          took <= base * 1.2 + 1.0, "%.1f s against %.1f s" % (took, base))
    look = doc.F0.ShapeAppearance[0].DiffuseColor[:3]
    check("the view provider's colour is the object's look", all(
        abs(a - b) < 1e-3 for a, b in zip(look, RED)), look)


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
