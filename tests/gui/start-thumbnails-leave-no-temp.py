"""The Start page reads a recent file's thumbnail without leaving a temporary
file behind.

DisplayedFilesModel's loadFCStdThumbnail() called
App::ProjectFile::extractInputFile("thumbnails/Thumbnail.png"), which writes
the entry to a new temporary file (Base::FileInfo::getTempFileName,
"fileXXXXXX" in the temporary path) and leaves it to the caller -- who read
it and never deleted it. One PNG per recent file each time the list was
loaded: at every start, and at every save or open (the list is rewritten
and the model reloads). dell's box held 253,820 of them in /tmp, 1.57 GB.
Now the entry is read into memory (ProjectFile::readInputFileDirect).

Three documents saved with thumbnails, the recent files list naming them,
the Start page open; the list rewritten twice so the model reloads. Claims:
the thumbnails are read (each row's image is a PNG), and the temporary
paths hold no more "file??????" entries after the reloads than before.
"""
import os
import re
import tempfile
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
NAME = re.compile(r"^file[A-Za-z0-9]{6}$")

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
        time.sleep(0.01)


def temp_roots():
    roots = []
    for root in (FreeCAD.getTempPath(), tempfile.gettempdir()):
        root = os.path.realpath(root)
        if root not in roots and os.path.isdir(root):
            roots.append(root)
    return roots


def temp_files():
    found = set()
    for root in temp_roots():
        for entry in os.listdir(root):
            if NAME.match(entry):
                found.add(os.path.join(root, entry))
    return found


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "SaveThumbnail", True)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        paths = []
        for i in range(3):
            doc = FreeCAD.newDocument("Thumb%d" % i)
            box = doc.addObject("Part::Box", "Box")
            box.Length = 10 + 5 * i
            doc.recompute()
            FreeCADGui.SendMsgToActiveView("ViewFit")
            pump(20)
            path = os.path.join(OUT, "thumb%d.FCStd" % i)
            doc.saveAs(path)
            paths.append(path)
            FreeCAD.closeDocument(doc.Name)
            pump(10)
        import zipfile
        with_thumbnail = [p for p in paths
                          if "thumbnails/Thumbnail.png" in zipfile.ZipFile(p).namelist()]
        check("the files carry thumbnails", len(with_thumbnail) == 3, with_thumbnail)
        recent = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/RecentFiles")
        recent.SetInt("RecentFiles", 0)
        for i, p in enumerate(paths):
            recent.SetString("MRU%d" % i, p)
        recent.SetInt("RecentFiles", len(paths))
        FreeCADGui.runCommand("Start_Start", 0)
        pump(100)
        before = temp_files()
        note("INFO the temporary roots %s; %d \"file??????\" entries before the reloads"
             % (temp_roots(), len(before)))
        for _ in range(2):
            # rewritten as a save or an open rewrites it: the model reloads
            recent.SetInt("RecentFiles", len(paths) - 1)
            pump(10)
            recent.SetInt("RecentFiles", len(paths))
            pump(60)
        after = temp_files()
        added = sorted(after - before)
        note("INFO after two reloads: %d entries, %d new" % (len(after), len(added)))
        # the image role (DisplayedFilesModelRoles::image, Qt::UserRole + 2):
        # the PNG's bytes as read
        image_role = int(QtCore.Qt.UserRole) + 2
        images = None
        for view in FreeCADGui.getMainWindow().findChildren(QtWidgets.QListView):
            model = view.model()
            if model is None or model.rowCount() < len(paths):
                continue
            images = []
            for row in range(model.rowCount()):
                data = model.data(model.index(row, 0), image_role)
                raw = bytes(data) if data is not None else b""
                images.append(raw[:8] == b"\x89PNG\r\n\x1a\n" and len(raw))
            break
        check("the Start page shows each recent file's thumbnail, a PNG read whole",
              images is not None and len(images) >= 3 and all(images), images)
        check("no temporary file left by the thumbnails' reading", not added,
              [os.path.basename(a) for a in added[:6]])
        for p in added:
            try:
                os.remove(p)
            except OSError:
                pass
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
