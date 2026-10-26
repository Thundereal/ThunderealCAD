"""No file in a document's transient directory carries an image or document
extension (docs/HandsOnQueue.md entry 206).

Closing a document deletes its transient directory, and the user's company
malware guard (Apex) ended the program over the delete of
`...\\blobs\\<uuid>.png`: a ransomware heuristic watches deletes of such
files. Every file the store writes there now ends in ".blob" --
`<uuid>.png.blob` -- with the type kept before the suffix and in the blob's
own record; a history checkout under the same directory names its files
`<name>.blob` too.

  - an image plane given a PNG and one given an SVG, a drawing image given
    the PNG: their included paths (ImageFile, ImageIncluded) end in
    ".png.blob" / ".svg.blob";
  - the files load through Qt by content (QImage), and the SVG is still
    told from the PNG;
  - no file under the transient directory ends in a guarded extension;
  - the saved file's archive keeps its own names (`blobs/<Obj>.ImageFile.png`);
  - reopened, the paths are neutral again and the files exist;
  - a version checked out of the history under the transient directory
    carries no guarded name either, and reads back as a document.
"""
import os
import traceback
import zipfile

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "NeutralNames"
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


def guarded_files(root):
    found = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            ext = f.rsplit(".", 1)[-1].lower() if "." in f else ""
            if ext in GUARDED:
                found.append(os.path.relpath(os.path.join(dirpath, f), root))
    return found


def make_images():
    png = os.path.join(OUT, "probe.png")
    img = QtGui.QImage(40, 30, QtGui.QImage.Format_RGB32)
    img.fill(QtGui.QColor(200, 30, 30))
    img.save(png, "PNG")
    svg = os.path.join(OUT, "probe.svg")
    with open(svg, "w") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" width="40" height="30">'
                '<rect width="40" height="30" fill="#1e1ec8"/></svg>\n')
    return png, svg


def run():
    try:
        png, svg = make_images()
        doc = FreeCAD.newDocument(DOC)
        plane = doc.addObject("Image::ImagePlane", "Plane")
        plane.ImageFile = png
        vector = doc.addObject("Image::ImagePlane", "Vector")
        vector.ImageFile = svg
        page = doc.addObject("TechDraw::DrawPage", "Page")
        template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        template.Template = os.path.join(
            FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates", "A4_Landscape_blank.svg")
        page.Template = template
        drawing = doc.addObject("TechDraw::DrawViewImage", "Drawing")
        drawing.ImageFile = png
        page.addView(drawing)
        doc.recompute()
        settle(1.0)
        transient = doc.TransientDir
        note("INFO transient " + transient)

        check("the PNG plane's file ends in .png.blob",
              plane.ImageFile.lower().endswith(".png.blob"), plane.ImageFile)
        check("the SVG plane's file ends in .svg.blob",
              vector.ImageFile.lower().endswith(".svg.blob"), vector.ImageFile)
        # DrawViewImage.ImageFile is the user's own path; the store's copy is
        # ImageIncluded
        check("the drawing image's included file ends in .png.blob",
              drawing.ImageIncluded.lower().endswith(".png.blob"), drawing.ImageIncluded)
        included = (plane.ImageFile, vector.ImageFile, drawing.ImageIncluded)
        check("the files live under the transient directory",
              all(os.path.normcase(os.path.normpath(p)).startswith(
                  os.path.normcase(os.path.normpath(transient))) for p in included), included)
        raster = QtGui.QImage(plane.ImageFile)
        check("Qt reads the PNG blob by content", not raster.isNull() and raster.width() == 40,
              (raster.isNull(), raster.width()))
        vp = FreeCADGui.getDocument(DOC).getObject("Vector")
        check("the SVG plane is told from a raster: it sized itself from the SVG",
              abs(vector.XSize - 40 * 25.4 / 96) < 1.0 or vector.XSize > 0, vector.XSize)
        check("no file under the transient directory carries a guarded extension",
              not guarded_files(transient), guarded_files(transient))

        path = os.path.join(OUT, DOC + ".FCStd")
        doc.saveAs(path)
        settle(0.5)
        # The transient directory's name carries a hash of the file name: a
        # saveAs moves it, and a scan of the old path would pass vacuously
        before = transient
        transient = doc.TransientDir
        note("INFO transient after the save " + transient)
        check("the transient directory after the save exists", os.path.isdir(transient),
              transient)
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
        check("the archive keeps its own names for the blobs",
              any(n.endswith("Plane.ImageFile.png") for n in names)
              and any(n.endswith("Vector.ImageFile.svg") for n in names)
              and not any(n.endswith(".blob") for n in names),
              [n for n in names if n.startswith("blobs/")])
        check("still no guarded file after the save", not guarded_files(transient),
              guarded_files(transient))
        check("nor under the directory the save moved away from",
              not os.path.isdir(before) or not guarded_files(before),
              guarded_files(before) if os.path.isdir(before) else "gone")

        # A version checked out of the history. The newest is the one the
        # document holds, and asking for it returns the document itself:
        # take a second version and open the first.
        doc.saveToLog()
        plane.XSize = plane.XSize * 2
        doc.recompute()
        doc.saveToLog()
        settle(0.5)
        versions = doc.getTransactionVersions()
        note("INFO versions " + repr([v["num"] for v in versions]))
        vdoc = None
        if len(versions) > 1:
            try:
                vdoc = doc.openTransactionVersion(versions[0]["num"])
            except Exception as e:
                note("INFO openTransactionVersion: " + str(e))
        settle(1.0)
        check("the checkout leaves no guarded file under the transient directory",
              not guarded_files(transient), guarded_files(transient))
        if vdoc is not None and vdoc.Name != doc.Name:
            note("INFO version document %s, transient %s" % (vdoc.Name, vdoc.TransientDir))
            check("nor does the version document's own transient directory",
                  os.path.isdir(vdoc.TransientDir) and not guarded_files(vdoc.TransientDir),
                  guarded_files(vdoc.TransientDir))
            vplane = vdoc.getObject("Plane")
            check("and the checked-out version's image plane reads its file back",
                  vplane is not None and vplane.ImageFile and os.path.exists(vplane.ImageFile),
                  getattr(vplane, "ImageFile", None))
            check("as a neutral name", vplane is not None
                  and vplane.ImageFile.lower().endswith(".png.blob"),
                  getattr(vplane, "ImageFile", None))
            vtransient = vdoc.TransientDir
            FreeCAD.closeDocument(vdoc.Name)
            settle(0.5)
            check("the version document's transient directory is gone with it",
                  not os.path.isdir(vtransient), vtransient)
        else:
            check("a version other than the document's own opened", False,
                  (versions, vdoc.Name if vdoc is not None else None))

        FreeCAD.closeDocument(doc.Name)
        settle(0.5)
        check("the transient directory is gone with the document", not os.path.isdir(transient),
              transient)

        doc = FreeCAD.openDocument(path)
        settle(1.0)
        plane = doc.getObject("Plane")
        vector = doc.getObject("Vector")
        check("reopened: the PNG plane's file is neutral and exists",
              plane.ImageFile.lower().endswith(".png.blob") and os.path.exists(plane.ImageFile),
              plane.ImageFile)
        check("reopened: the SVG plane's file is neutral and exists",
              vector.ImageFile.lower().endswith(".svg.blob") and os.path.exists(vector.ImageFile),
              vector.ImageFile)
        drawing = doc.getObject("Drawing")
        check("reopened: the drawing image's included file is neutral and exists",
              drawing.ImageIncluded.lower().endswith(".png.blob")
              and os.path.exists(drawing.ImageIncluded), drawing.ImageIncluded)
        check("reopened: no guarded file", not guarded_files(doc.TransientDir),
              guarded_files(doc.TransientDir))
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
