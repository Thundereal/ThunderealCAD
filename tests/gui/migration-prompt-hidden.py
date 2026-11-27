"""A document opened hidden by code is not asked about its migration.

The question "Some document(s) require recomputation for migration
purpose ... Do you want to recompute now?" (Gui::Application, on
signalFinishOpenDocument) was put for EVERY document the open left marked
for a migration. CAM opens each tool-bit shape it reads as a hidden
document, from a temporary copy, and closes it again
(Path/Tool/shape/doc.py). Its shapes were saved by another version of
the element names, so creating a Job raised the question about a
document the user never saw. Under a test nobody answers it:
SandboxPanelMirror.test_cam_op_panel sat in it on Linux with
Preferences/Document/WarnRecomputeOnRestore at its default.

Now the question is put only about what the user can reach: a document
with a view, and the documents such a one depends on.

What is done, with WarnRecomputeOnRestore on, and a timer answering any
such question "No" so that nothing waits:
  1. CAM's endmill.fcstd, copied, opened hidden: the open marks it for
     the migration (the console says "Recomputation required for document
     ..."), and no question is put.
  2. Another copy opened with a view: the question is put, once.
  3. A document with a view that links into a third copy, saved and
     opened again: the copy comes in as its dependency, with no view of
     its own, and the question is put about it, once -- what the user's
     document depends on is reachable.

Scored against the tree before the change: step 1 put the question.
"""
import os
import shutil
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
SHAPE = os.path.join(FreeCAD.getHomePath(), "Mod", "CAM", "Tools", "Shape", "endmill.fcstd")

state = {"asked": [], "marked": []}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(n=10):
    for _ in range(n):
        QtCore.QCoreApplication.processEvents()


class Marked:
    """The console's word that an open marked a document for migration"""

    def __init__(self):
        self.lines = []

    def __call__(self, notifier, msg, level):
        if "Recomputation required for document" in msg:
            self.lines.append(msg.strip())


def answer():
    """Answers the migration question No, wherever it is put, and says so"""
    w = QtWidgets.QApplication.activeModalWidget()
    if isinstance(w, QtWidgets.QMessageBox) and "migration" in (w.text() or ""):
        state["asked"].append(w.text()[:60])
        no = w.button(QtWidgets.QMessageBox.No)
        if no is not None:
            no.click()
        else:
            w.reject()


def marked_for_migration(doc):
    """Whether the open left an object of `doc` marked for the migration
    (its State's MigrationPending): read off the document itself, not
    off a console line said once"""
    return any("MigrationPending" in o.State for o in doc.Objects)


def copy(name):
    path = os.path.join(OUT, name + ".FCStd")
    shutil.copyfile(SHAPE, path)
    return path


def run():
    marked = Marked()
    timer = QtCore.QTimer()
    timer.timeout.connect(answer)
    timer.start(50)
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "WarnRecomputeOnRestore", True)
        FreeCAD.Console.AttachObserver(marked)
        check("the fixture is there", os.path.isfile(SHAPE), SHAPE)

        # 1. hidden, as CAM opens it
        before = len(marked.lines)
        doc = FreeCAD.openDocument(copy("hidden"), hidden=True)
        settle()
        check("1: the case arose: the hidden open marked it for migration",
              marked_for_migration(doc), marked.lines[before:])
        check("1: no view was made for it",
              not FreeCADGui.getDocument(doc.Name).mdiViewsOfType("Gui::View3DInventor"))
        check("1: no question about the hidden document", not state["asked"], state["asked"])
        FreeCAD.closeDocument(doc.Name)
        settle()

        # 2. with a view, as the user opens it
        before = len(marked.lines)
        doc = FreeCAD.openDocument(copy("shown"))
        settle()
        check("2: the case arose: the open marked it for migration",
              marked_for_migration(doc), marked.lines[before:])
        check("2: the question was put, once", len(state["asked"]) == 1, state["asked"])
        FreeCAD.closeDocument(doc.Name)
        settle()

        # 3. a dependency of a document with a view
        part = FreeCAD.openDocument(copy("part"))
        settle()
        asm = FreeCAD.newDocument("asm")
        link = asm.addObject("App::Link", "Link")
        link.LinkedObject = part.getObject("Body")
        asm.recompute()
        asm_path = os.path.join(OUT, "asm.FCStd")
        asm.saveAs(asm_path)
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        settle()
        asked = len(state["asked"])
        before = len(marked.lines)
        asm = FreeCAD.openDocument(asm_path)
        settle()
        loaded = sorted(FreeCAD.listDocuments())
        check("3: the case arose: the part came in with the assembly, marked",
              "part" in loaded and marked_for_migration(FreeCAD.getDocument("part")),
              (loaded, marked.lines[before:]))
        check("3: the part has no view of its own",
              "part" in loaded
              and not FreeCADGui.getDocument("part").mdiViewsOfType("Gui::View3DInventor"))
        check("3: the question was put about the dependency, once",
              len(state["asked"]) == asked + 1, state["asked"][asked:])
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        timer.stop()
        try:
            FreeCAD.Console.DetachObserver(marked)
        except Exception:
            pass
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(0, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1000, run)
