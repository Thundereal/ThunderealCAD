# vrml-textures-file-make.py : a document holding a VRML object whose VRML
# file names a texture, inserted the way File > Open inserts one (through
# Gui.insert, which puts the file's directory on Coin's search path), and
# saved. What data/tests/VrmlTexturesByFork2025.FCStd was written by, in the
# FreeCAD-Link-Tip 20251015 release (before the blob store, 959f96893c), and
# what tests/gui/vrml-resources-neutral.py opens as an old file
# (docs/HandsOnQueue.md entry 212). A GUI run, given this script at startup:
#
#   V_FILE=C:\tmp\old_vrml.FCStd FreeCADLink.exe vrml-textures-file-make.py
#
#   V_FILE    the file to save; the scene and its texture are written beside
#             it, in <dir>/src, and what the build listed in <file>.made.txt
#
# The saved file holds `scene.wrl` and the texture as `scene.scene/tex.png`,
# an 8x8 PNG of (220, 20, 20), its Resources property `scene/tex.png`.
import os
import traceback
import zipfile

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui

PATH = os.environ["V_FILE"]
SRC = os.path.join(os.path.dirname(PATH), "src")
LOG = PATH + ".made.txt"


def note(msg):
    with open(LOG, "a") as f:
        f.write(str(msg) + "\n")


def settle(seconds):
    import time
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtCore.QCoreApplication.processEvents()
        time.sleep(0.01)


def run():
    try:
        os.makedirs(SRC, exist_ok=True)
        img = QtGui.QImage(8, 8, QtGui.QImage.Format_RGB32)
        img.fill(QtGui.QColor(220, 20, 20))
        img.save(os.path.join(SRC, "tex.png"), "PNG")
        wrl = os.path.join(SRC, "scene.wrl")
        with open(wrl, "w") as f:
            f.write("#VRML V2.0 utf8\n"
                    "Shape {\n"
                    "  appearance Appearance { texture ImageTexture { url \"tex.png\" } }\n"
                    "  geometry Box { size 10 10 10 }\n"
                    "}\n")
        note("version " + repr(FreeCAD.Version()[:4]))
        doc = FreeCAD.newDocument("OldVrml")
        FreeCADGui.insert(wrl, doc.Name)
        doc.recompute()
        settle(2.0)
        obj = doc.Objects[0]
        note("object " + obj.Name)
        note("Urls " + repr(list(obj.Urls)))
        note("Resources " + repr(list(obj.Resources)))
        doc.saveAs(PATH)
        with zipfile.ZipFile(PATH) as z:
            note("archive " + repr(z.namelist()))
        FreeCAD.closeDocument(doc.Name)
    except Exception:
        note("ABORT " + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(2000, run)
