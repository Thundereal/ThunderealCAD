# GUI check of docs/TransactionLogJournal.md sec 38.14: a save of a document kept
# as a folder leaves the view-provider files of the objects whose view
# nothing changed as the folder has them, as it does their objects' files
# (sec 38.12) -- after a save of it, and after an open of it.
# One GUI run in a fresh user home, given this script at startup:
#
#   cd build/conda-relwithdebinfo-801
#   QT_QPA_PLATFORM=offscreen FREECAD_USER_HOME=/tmp/fchome-fo \
#     FOLDERCHECK_OUT=/tmp/fo/out.txt ~/works/sw/fcad/.conda/run.sh ./bin/FreeCAD \
#     ~/works/sw/fcad/scripts/transaction-log-folder-check.py
#
# It writes PASS/FAIL lines to $FOLDERCHECK_OUT and exits. The setting it
# changes is put back before it exits.
import os, shutil, tempfile, traceback
import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets

OUT = os.environ["FOLDERCHECK_OUT"]
lines = []


def check(what, ok):
    lines.append(("PASS " if ok else "FAIL ") + what)


def settle(ms=0):
    waited = QtCore.QElapsedTimer()
    waited.start()
    for _ in range(5):
        QtWidgets.QApplication.processEvents()
    while waited.elapsed() < ms:
        QtWidgets.QApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)


def stamp(path):
    st = os.stat(path)
    return (st.st_ino, st.st_mtime_ns, st.st_size)


def viewFile(path, name):
    """The view-provider file of object `name` in folder `path`."""
    f = os.path.join(path, name + ".Gui.xml")
    return f if os.path.exists(f) else None


def edit(doc, name, change):
    doc.openTransaction(name)
    change(doc)
    doc.recompute()
    doc.commitTransaction()
    settle()


def last(doc):
    info = doc.getLastFolderSave()
    return (info["written"], info["kept"], info["viewsWritten"], info["viewsKept"])


def run():
    p = App.ParamGet("User parameter:BaseApp/Preferences/Document")
    mode = p.GetInt("TransactionLog", 0)
    checking = os.environ.pop("FC_FOLDER_SAVE_CHECK", None)
    where = tempfile.mkdtemp(prefix="fc-folder-check-")
    try:
        p.SetInt("TransactionLog", 2)
        path = os.path.join(where, "Views")
        os.makedirs(path)
        doc = App.newDocument("Views")
        doc.UndoMode = 1
        edit(doc, "create", lambda d: [d.addObject("Part::Box", "Box%d" % i) for i in range(4)])
        doc.saveAs(path)
        settle(300)
        check("the first save writes every view-provider file (%r)" % (last(doc),),
              last(doc)[2] == 4 and viewFile(path, "Box1") is not None)

        doc.save()
        settle()
        check("saved again with nothing changed, every one is kept (%r)" % (last(doc),),
              last(doc) == (0, 4, 0, 4))

        box1 = stamp(viewFile(path, "Box1"))
        # A property of the view provider's own (ShapeColor is the object's
        # appearance, sec 34, and its change the object's file).
        edit(doc, "wide", lambda d: setattr(d.Box0.ViewObject, "LineWidth", 4.0))
        doc.save()
        settle()
        check("a line width changed writes that view provider's file and keeps the others "
              "(%r)" % (last(doc),), last(doc) == (0, 4, 1, 3))
        check("and leaves the others' files untouched", stamp(viewFile(path, "Box1")) == box1)

        # A property's status is recorded by no transaction.
        Gui.getDocument(doc.Name).getObject("Box2").setEditorMode("PointSize", 2)
        doc.save()
        settle()
        check("a property's status changed writes that file too (%r)" % (last(doc),),
              last(doc)[2:] == (1, 3))

        # -- After an open ---------------------------------------------------
        App.closeDocument(doc.Name)
        settle(300)
        doc = App.openDocument(path)
        settle(500)
        check("opened, the line width is there (%r)" % (doc.Box0.ViewObject.LineWidth,),
              doc.Box0.ViewObject.LineWidth == 4.0)
        doc.save()
        settle()
        check("saved after an open with nothing changed, everything is kept (%r)"
              % (last(doc),), last(doc) == (0, 4, 0, 4))
        edit(doc, "wider", lambda d: setattr(d.Box3.ViewObject, "LineWidth", 5.0))
        edit(doc, "longer", lambda d: setattr(d.Box1, "Length", 20))
        doc.save()
        settle()
        check("then one view and one object changed: each file written alone (%r)"
              % (last(doc),), last(doc) == (1, 3, 1, 3))
        App.closeDocument(doc.Name)
        settle(300)
        doc = App.openDocument(path)
        settle(500)
        check("opened again, all of it is there (%r, %r, %r)"
              % (doc.Box0.ViewObject.LineWidth, doc.Box3.ViewObject.LineWidth,
                 doc.Box1.Length.Value),
              doc.Box0.ViewObject.LineWidth == 4.0 and doc.Box3.ViewObject.LineWidth == 5.0
              and doc.Box1.Length.Value == 20
              and Gui.getDocument(doc.Name).getObject("Box2").getEditorMode("PointSize")
              == ["Hidden"])

        # The object's appearance: its file, not the view provider's.
        edit(doc, "red", lambda d: setattr(d.Box0.ViewObject, "ShapeColor", (1.0, 0.0, 0.0)))
        doc.save()
        settle()
        check("a colour changed writes the object's file (%r)" % (last(doc),),
              last(doc)[:2] == (1, 3))
        App.closeDocument(doc.Name)
        settle(300)
        doc = App.openDocument(path)
        settle(500)
        check("and is there when opened (%r)" % (doc.Box0.ViewObject.ShapeColor,),
              doc.Box0.ViewObject.ShapeColor[:3] == (1.0, 0.0, 0.0))

        # -- The check -------------------------------------------------------
        os.environ["FC_FOLDER_SAVE_CHECK"] = "1"
        edit(doc, "thin", lambda d: setattr(d.Box2.ViewObject, "LineWidth", 1.0))
        doc.save()
        settle()
        info = doc.getLastFolderSave()
        check("under the check, what would have been kept came out as the folder has it "
              "(%r)" % (info,), info["checked"] and info["differed"] == []
              and info["viewsKept"] == 0)
        del os.environ["FC_FOLDER_SAVE_CHECK"]
        App.closeDocument(doc.Name)
    except Exception:
        lines.append("FAIL exception\n" + traceback.format_exc())
    finally:
        if checking is not None:
            os.environ["FC_FOLDER_SAVE_CHECK"] = checking
        p.SetInt("TransactionLog", mode)
        App.saveParameter()
        shutil.rmtree(where, ignore_errors=True)
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    os._exit(0)


QtCore.QTimer.singleShot(0, run)
