"""Every save names who asked for it (docs/HandsOnQueue.md entry 167).

scanner.FCStd was saved by "someone (not me)", and nothing in the file or
the console said what called the save. Now the history's record of a
save carries a trigger (doc.getTransactionSave()['trigger']), every save
writes a console Log line "Saved <file> (schema N) by <trigger>", and
the MCP console logs each block it runs with its client.

  - a save from Python: "python:<this file>:<line>";
  - Std_Save with the 3D view active: "Std_Save/Save@Gui::View3DInventor";
  - Std_Save with a drawing page active -- the likely caller of entry
    167: "Std_Save/Save@TechDrawGui::MDIViewPage";
  - a block run by the MCP console for a client: the block logged with
    the client, and the save it makes "mcp:<client>" (the block's own
    line is in that log, not in the trigger);
  - each trigger also in the console log line the MCP log captures;
  - a record read back as a dict with id, version, seq and trigger, and
    a save id the history does not hold gives None.

The MCP block runs through the server module's own executor in process,
not over HTTP: the client's name is what the transport hands the tool,
and this exercises everything after that.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SaveTrigger"
MCP_LOG = os.path.join(OUT, "mcp_console.log")
state = {"done": False, "observer": False}


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


def trigger_of(doc):
    rec = doc.getTransactionSave()
    return rec["trigger"] if rec else None


def mcp_log():
    try:
        with open(MCP_LOG, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def saved_line(text, trigger):
    return any("Saved " in line and "by " + trigger in line for line in text.splitlines())


def run():
    try:
        from freecad.mcp_console import server

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        # The MCP console's log, with its console observer: what a host's
        # log would carry of these saves
        server._attach_log_observer(MCP_LOG)
        state["observer"] = True

        doc = FreeCAD.newDocument(DOC)
        doc.addObject("Part::Box", "Box")
        doc.recompute()
        settle(0.5)

        # -- Python ----------------------------------------------------
        path = os.path.join(OUT, DOC + ".FCStd")
        doc.saveAs(path)
        settle(0.5)
        t = trigger_of(doc)
        check("a save from Python names this file and the line",
              t is not None and t.startswith("python:") and "save-trigger.py" in t
              and t.rsplit(":", 1)[-1].isdigit(), t)
        check("and the console line says so", saved_line(mcp_log(), t or "?"), t)
        rec = doc.getTransactionSave()
        check("the record has id, version, seq and trigger",
              rec is not None and set(rec) == {"id", "version", "seq", "trigger"}
              and rec["version"] >= 1 and rec["seq"] >= 1, rec)
        check("a save id the history does not hold gives None",
              doc.getTransactionSave("no-such-save") is None)

        # -- Std_Save, 3D view active ----------------------------------
        doc.getObject("Box").Length = 20
        doc.recompute()
        FreeCADGui.getDocument(DOC).mdiViewsOfType("Gui::View3DInventor")[0].graphicsView().setFocus()
        settle(0.3)
        FreeCADGui.runCommand("Std_Save")
        settle(0.8)
        t = trigger_of(doc)
        check("Std_Save with the 3D view active names both",
              t == "Std_Save/Save@Gui::View3DInventor", t)
        check("and the console line says so", saved_line(mcp_log(), t or "?"), t)

        # -- Std_Save, drawing page active -----------------------------
        page = doc.addObject("TechDraw::DrawPage", "Page")
        template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        template.Template = os.path.join(
            FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates", "A4_Landscape_blank.svg")
        page.Template = template
        doc.recompute()
        page.ViewObject.doubleClicked()
        settle(1.5)
        pages = FreeCADGui.getDocument(DOC).mdiViewsOfType("TechDrawGui::MDIViewPage")
        check("the page's view is the active one",
              len(pages) == 1 and mw.getActiveWindow() is not None
              and mw.getActiveWindow().ID == pages[0].ID,
              (len(pages), getattr(mw.getActiveWindow(), "ID", None)))
        FreeCADGui.runCommand("Std_Save")
        settle(0.8)
        t = trigger_of(doc)
        check("Std_Save with a drawing page active names the page view",
              t == "Std_Save/Save@TechDrawGui::MDIViewPage", t)
        check("and the console line says so", saved_line(mcp_log(), t or "?"), t)

        # -- the MCP console -------------------------------------------
        doc.getObject("Box").Width = 20
        doc.recompute()
        code = "App.getDocument('%s').save()\n" % DOC
        server._seed_namespace()  # App and Gui, as start() seeds them
        res = server._exec_code(code, client="probe-client")
        settle(0.5)
        check("the block ran", res.get("exception") is None, res.get("exception"))
        t = trigger_of(doc)
        check("a save by a block the MCP console ran names the client",
              t == "mcp:probe-client", t)
        log = mcp_log()
        exec_lines = [l for l in log.splitlines() if " Exec " in l]
        check("the MCP log has the block: client, length, first line",
              any("client=probe-client" in l and "len=%d" % len(code) in l
                  and "first=App.getDocument('%s').save()" % DOC in l for l in exec_lines),
              exec_lines)
        check("and the save's console line after it", saved_line(log, t or "?"), t)
        check("the log holds the block's first line, not the code",
              not any("\n" in l for l in exec_lines) and all(len(l) < 300 for l in exec_lines))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    if state["observer"]:
        try:
            from freecad.mcp_console import server
            server._detach_log_observer()
        except Exception:
            pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
