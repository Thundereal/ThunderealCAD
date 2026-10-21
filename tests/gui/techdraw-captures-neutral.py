"""TechDraw's two 3D captures write no file with an image extension that is
later deleted (docs/HandsOnQueue.md entry 206, follow-up).

A ransomware guard watches the delete of a file with an image extension.
  - a part view's shaded underlay is captured to a temporary file, included,
    and the temporary deleted at once: the temporary ends in ".png.blob",
    and none named "..._ShadedUnderlay.png" is left or made;
  - the Active View command saves its capture in the document's transient
    directory, and the store moves it into blobs/: it ends in ".png.blob"
    from the start, so no image file is renamed or deleted, and the
    included copy still reads back as a PNG;
  - the saved file names both "...png" in its archive.
"""
import glob
import os
import traceback
import zipfile

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "CapturesNeutral"
GUARDED = {"png", "jpg", "jpeg", "gif", "bmp", "tif", "tiff", "svg", "pdf", "doc", "docx",
           "xls", "xlsx", "ppt", "pptx", "txt", "csv", "rtf", "zip"}
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds=0.3):
    import time
    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def wait_for(cond, seconds):
    import time
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        settle(0.2)
        if cond():
            return True
    return cond()


def guarded_files(root):
    found = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            ext = f.rsplit(".", 1)[-1].lower() if "." in f else ""
            if ext in GUARDED:
                found.append(os.path.relpath(os.path.join(dirpath, f), root))
    return found


def activate_3d_view(doc_name):
    gdoc = FreeCADGui.getDocument(doc_name)
    views = gdoc.mdiViewsOfType("Gui::View3DInventor")
    if not views:
        return False
    mdi = FreeCADGui.getMainWindow().findChild(QtGui.QMdiArea)
    for sub in mdi.subWindowList():
        if "3D" in sub.windowTitle() or sub.widget().metaObject().className() == "Gui::View3DInventor":
            mdi.setActiveSubWindow(sub)
            settle(0.5)
            return True
    return False


def click_ok():
    mw = FreeCADGui.getMainWindow()
    for box in mw.findChildren(QtGui.QDialogButtonBox):
        if not box.isVisible():
            continue
        ok = box.button(QtGui.QDialogButtonBox.Ok)
        if ok is not None and ok.isEnabled():
            ok.click()
            return True
    return False


def run():
    try:
        tmp = QtCore.QDir.tempPath()
        before = set(glob.glob(os.path.join(tmp, "*_ShadedUnderlay.png*")))
        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        page = doc.addObject("TechDraw::DrawPage", "Page")
        template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        template.Template = os.path.join(
            FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates", "A4_Landscape_blank.svg")
        page.Template = template
        view = doc.addObject("TechDraw::DrawViewPart", "View")
        page.addView(view)
        view.Source = [box]
        view.Direction = FreeCAD.Vector(1, 1, 1)
        doc.recompute()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        settle(1.0)
        page.ViewObject.doubleClicked()
        settle(2.0)
        view.Shaded = True
        doc.recompute()
        got = wait_for(lambda: bool(view.UnderlayImage), 20)
        note("INFO UnderlayImage " + repr(view.UnderlayImage))
        if check("the shaded underlay was captured", got, view.UnderlayImage):
            check("its included file ends in .png.blob",
                  view.UnderlayImage.lower().endswith(".png.blob"), view.UnderlayImage)
        made = set(glob.glob(os.path.join(tmp, "*_ShadedUnderlay.png*"))) - before
        check("no temporary underlay capture is left behind", not made, sorted(made))
        check("none named ..._ShadedUnderlay.png",
              not os.path.exists(os.path.join(tmp, "View_ShadedUnderlay.png")))

        # The Active View command, from the 3D view onto the page
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, page.Name)
        activated = activate_3d_view(doc.Name)
        note("INFO 3D view active: %s" % activated)
        names = set(o.Name for o in doc.Objects)
        FreeCADGui.runCommand("TechDraw_ActiveView", 0)
        settle(1.5)
        clicked = click_ok()
        note("INFO OK clicked: %s" % clicked)
        wait_for(lambda: any(o.TypeId == "TechDraw::DrawViewImage" for o in doc.Objects), 10)
        images = [o for o in doc.Objects
                  if o.TypeId == "TechDraw::DrawViewImage" and o.Name not in names]
        if check("the Active View command made an image", len(images) == 1,
                 [o.Name for o in images]):
            img = images[0]
            note("INFO ImageFile %s, ImageIncluded %s" % (img.ImageFile, img.ImageIncluded))
            check("its capture ends in .png.blob", img.ImageFile.lower().endswith(".png.blob"),
                  img.ImageFile)
            check("its included copy ends in .png.blob",
                  img.ImageIncluded.lower().endswith(".png.blob"), img.ImageIncluded)
            # The store moved the capture into blobs/ (it was in the transient
            # directory): it is the included copy that reads back
            note("INFO the capture itself still exists: %s" % os.path.exists(img.ImageFile))
            q = QtGui.QImage()
            check("the included copy reads back as a PNG",
                  q.load(img.ImageIncluded, "PNG") and not q.isNull(), img.ImageIncluded)
        check("no file under the transient directory carries a guarded extension",
              not guarded_files(doc.TransientDir), guarded_files(doc.TransientDir))

        path = os.path.join(OUT, DOC + ".FCStd")
        doc.saveAs(path)
        settle(0.5)
        with zipfile.ZipFile(path) as z:
            blobs = [n for n in z.namelist() if n.startswith("blobs/")]
        note("INFO archive blobs " + repr(blobs))
        check("the archive names the underlay a .png",
              not view.UnderlayImage or any(n.endswith("View.UnderlayImage.png") for n in blobs),
              blobs)
        check("and the active view's image a .png",
              not images or any(n.endswith(images[0].Name + ".ImageIncluded.png") for n in blobs),
              blobs)
        check("no archive name ends in .blob", not any(n.endswith(".blob") for n in blobs), blobs)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
