"""A VRML object keeps its textures: they are found, saved, restored under a
neutral name in the transient directory, and drawn from there
(docs/HandsOnQueue.md entry 212).

Since the blob store (959f96893c) the VRML file is read from
TransientDir/blobs, so Coin never found a texture beside it: Urls and
Resources stayed empty and a save dropped the texture. The restore was
broken longer: it registered each resource under a name that matched no
archive entry, and setting Urls rewrote the saved Resources to `<Obj>/`.
A restore writes a texture under the transient directory, which a close
deletes, so it goes as `<resource>.blob` (entry 206) and the view provider
points the scene at it.

  - a VRML file with an ImageTexture next to it, assigned from Python: the
    texture is listed as a resource, and its image is loaded;
  - saved, closed, reopened: the resource is written back under the
    transient directory with no guarded extension, the texture node points
    at it, and its image is loaded;
  - saved again: the archive holds the texture under the name Save always
    used, with the original bytes;
  - an OLD file (data/tests/VrmlTexturesByFork2025.FCStd, saved by the
    FreeCAD-Link-Tip 20251015 release): it opens with its texture drawn,
    and a save writes the texture back;
  - a close removes the transient directory.
"""
import os
import traceback
import zipfile

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui
from pivy import coin

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OLD = os.path.join(REPO, "data", "tests", "VrmlTexturesByFork2025.FCStd")
DOC = "VrmlNeutral"
GUARDED = {"png", "jpg", "jpeg", "gif", "bmp", "tif", "tiff", "svg", "pdf", "doc", "docx",
           "xls", "xlsx", "ppt", "pptx", "txt", "csv", "rtf", "zip"}
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name
         + (" | " + str(detail) if detail is not None and detail != "" else ""))
    return cond


def settle(seconds=0.3):
    import time
    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def guarded_files(root):
    found = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            ext = f.rsplit(".", 1)[-1].lower() if "." in f else ""
            if ext in GUARDED:
                found.append(os.path.relpath(os.path.join(dirpath, f), root))
    return found


def make_scene():
    src = os.path.join(OUT, "vrml")
    os.makedirs(src, exist_ok=True)
    img = QtGui.QImage(8, 8, QtGui.QImage.Format_RGB32)
    img.fill(QtGui.QColor(220, 20, 20))
    png = os.path.join(src, "tex.png")
    img.save(png, "PNG")
    wrl = os.path.join(src, "scene.wrl")
    with open(wrl, "w") as f:
        f.write("#VRML V2.0 utf8\n"
                "Shape {\n"
                "  appearance Appearance { texture ImageTexture { url \"tex.png\" } }\n"
                "  geometry Box { size 10 10 10 }\n"
                "}\n")
    return wrl, png


def texture_node(doc_name, obj_name):
    """The object's ImageTexture. A search's public path ends at the Shape:
    the texture hangs off its appearance field, not as a child."""
    vp = FreeCADGui.getDocument(doc_name).getObject(obj_name)
    sa = coin.SoSearchAction()
    sa.setType(coin.SoVRMLImageTexture.getClassTypeId())
    sa.setInterest(coin.SoSearchAction.FIRST)
    sa.setSearchingAll(True)
    sa.apply(vp.RootNode)
    path = sa.getPath()
    if path is None:
        return None
    shape = path.getTail()
    if shape.isOfType(coin.SoVRMLImageTexture.getClassTypeId()):
        return shape
    return shape.appearance.getValue().texture.getValue()


def texture_state(doc_name, obj_name):
    """(url, loaded size) of the object's texture. The image loads on the
    first draw that needs it, so the view is redrawn until it has."""
    tex = texture_node(doc_name, obj_name)
    if tex is None:
        return None, None
    url = tex.url.getValues()[0] if tex.url.getNum() else ""
    views = FreeCADGui.getDocument(doc_name).mdiViewsOfType("Gui::View3DInventor")
    size = None
    for _ in range(20):
        if views:
            views[0].redraw()
        settle(0.25)
        img = tex.getImage()
        if img is not None and img.hasData():
            size = list(img.getSize())[:2]
            break
    return url, size


def entry_bytes(path, suffix):
    with zipfile.ZipFile(path) as z:
        names = [n for n in z.namelist() if n.endswith(suffix)]
        return names, (z.read(names[0]) if len(names) == 1 else b"")


def check_reopened(doc, obj_name, tag, original):
    obj = doc.getObject(obj_name)
    transient = doc.TransientDir
    note("INFO %s Urls %r Resources %r" % (tag, list(obj.Urls), list(obj.Resources)))
    urls = list(obj.Urls)
    check(tag + ": the resource is written back under the transient directory",
          len(urls) == 1 and os.path.exists(urls[0])
          and os.path.normcase(os.path.normpath(urls[0])).startswith(
              os.path.normcase(os.path.normpath(transient))), urls)
    check(tag + ": under a neutral name", len(urls) == 1 and urls[0].endswith(".png.blob"), urls)
    stored = b""
    if len(urls) == 1 and os.path.exists(urls[0]):
        with open(urls[0], "rb") as f:
            stored = f.read()
    check(tag + ": with the texture's bytes", stored == original, len(stored))
    check(tag + ": the saved Resources stand",
          list(obj.Resources) == [obj_name + "/tex.png"], list(obj.Resources))
    check(tag + ": no guarded file under the transient directory",
          not guarded_files(transient), guarded_files(transient))
    url, size = texture_state(doc.Name, obj_name)
    check(tag + ": the texture node points at the stored file",
          url is not None and url.endswith(".png.blob") and os.path.exists(url), url)
    check(tag + ": and has its 8x8 image loaded", size == [8, 8], size)


def run():
    try:
        wrl, png = make_scene()
        with open(png, "rb") as f:
            original = f.read()

        # A fresh import, from Python
        doc = FreeCAD.newDocument(DOC)
        obj = doc.addObject("App::VRMLObject", "Vrml")
        obj.VrmlFile = wrl
        doc.recompute()
        settle(1.5)
        note("INFO fresh Urls %r Resources %r" % (list(obj.Urls), list(obj.Resources)))
        check("fresh: the texture is listed as a resource",
              list(obj.Resources) == ["Vrml/tex.png"], list(obj.Resources))
        url, size = texture_state(doc.Name, "Vrml")
        check("fresh: the texture is read from the user's file, by its name",
              url == "tex.png", url)
        check("fresh: and has its 8x8 image loaded", size == [8, 8], size)

        path = os.path.join(OUT, DOC + ".FCStd")
        doc.saveAs(path)
        settle(0.5)
        names, data = entry_bytes(path, "tex.png")
        check("saved: the archive holds the texture as Vrml.Vrml/tex.png, with its bytes",
              names == ["Vrml.Vrml/tex.png"] and data == original, names)
        FreeCAD.closeDocument(doc.Name)
        settle(0.5)

        doc = FreeCAD.openDocument(path)
        settle(1.5)
        check_reopened(doc, "Vrml", "reopened", original)
        again = os.path.join(OUT, DOC + "2.FCStd")
        doc.saveAs(again)
        settle(0.5)
        names, data = entry_bytes(again, "tex.png")
        check("saved again: the texture under the same name, with its bytes",
              names == ["Vrml.Vrml/tex.png"] and data == original, names)
        transient = doc.TransientDir
        FreeCAD.closeDocument(doc.Name)
        settle(0.5)
        check("a close removes the transient directory", not os.path.isdir(transient), transient)

        # The old file
        check("the old file is at hand", os.path.exists(OLD), OLD)
        _names, old_bytes = entry_bytes(OLD, "tex.png")
        doc = FreeCAD.openDocument(OLD)
        settle(1.5)
        check_reopened(doc, "scene", "old file", old_bytes)
        resaved = os.path.join(OUT, "old_resaved.FCStd")
        doc.saveAs(resaved)
        settle(0.5)
        names, data = entry_bytes(resaved, "tex.png")
        check("old file saved: the texture written back, with its bytes",
              names == ["scene.scene/tex.png"] and data == old_bytes, names)
        FreeCAD.closeDocument(doc.Name)
        settle(0.5)
        doc = FreeCAD.openDocument(resaved)
        settle(1.5)
        check_reopened(doc, "scene", "old file resaved, reopened", old_bytes)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
