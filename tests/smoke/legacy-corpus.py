"""The legacy corpus loop, one document (docs/SmokeTesting.md sec 3, step 2).

Run by tests/smoke/legacy-corpus.sh through scripts/gui-test.sh, which gives
it an isolated profile under xvfb; GT_DOC names the document, never one a
script made in the same run. The document is copied first: the original is
not written.

  open      Gui.open, as File > Open does; the migration's question
            answered Yes, as a user would; its recompute -- handed to a
            worker where the GUI does that -- waited for
  worker    every object touched and recomputed by a worker
  here      every object touched and recomputed in this process
  save      saved as a copy under GT_OUT
  reopen    closed and opened again from the copy

Checks, each line a FINDING (docs/SmokeTesting.md sec 2.2):
  1 log      every Error and Warning the console carried, with its phase
  2 dialog   every dialog, with its text; only the migration's is expected
  6 twins    worker against here: each object's shapes, App properties,
             state and the visibility census (docs/WorkerProcess.md sec
             73); the live document against the reopened one: every
             property of the objects and their view providers
  8 the end  no document left open (the driver checks the exit code, the
             crash log and what is left in the transient folder)

INFO lines say what was done; a phase that could not be done (a worker
refused the document) is an INFO, not a finding. GT_SMOKE_SKIP_TWIN=1
leaves the worker twin out (a document too large for two recomputes).
"""
import faulthandler
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
SOURCE = os.environ.get("GT_DOC", "")  # none: an empty session, the baseline of check 8
WAIT = float(os.environ.get("GT_SMOKE_WAIT", "900"))

# A hang's Python stacks, every thread: on SIGUSR1 (kill -USR1 <pid>, as a
# debugger cannot attach where ptrace is confined to a parent), and once by
# itself shortly before the wait runs out
_stacks = open(os.path.join(OUT, "stacks.txt"), "w", encoding="ascii", errors="replace")
faulthandler.register(signal.SIGUSR1, file=_stacks, all_threads=True)
faulthandler.dump_traceback_later(max(WAIT - 30, 30), file=_stacks)

state = {"phase": "start", "dialogs": [], "log": [], "findings": 0}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg).replace("\n", " | ") + "\n")
        f.flush()


def finding(check, what, detail=""):
    state["findings"] += 1
    note("FINDING %s %s: %s%s" % (check, state["phase"], what,
                                  (" | " + str(detail)[:1500]) if detail else ""))


def phase(name):
    state["phase"] = name
    note("INFO phase %s at %.1f s" % (name, time.time() - state["t0"]))


# --- check 1: the log -------------------------------------------------------

def listen(notifier, message, level):
    if level in ("Warning", "Error", "Critical"):
        state["log"].append((state["phase"], level, message.strip()))


def flush_log():
    seen = {}
    for ph, level, text in state["log"]:
        key = (ph, level, re.sub(r"0x[0-9a-fA-F]+|\d+(\.\d+)?", "#", text))
        if key not in seen:
            seen[key] = [text, 0]
        seen[key][1] += 1
    for (ph, level, _), (text, count) in seen.items():
        state["phase"] = ph
        finding("log", level, ("%dx " % count if count > 1 else "") + text)
    state["log"] = []


# --- check 2: dialogs -------------------------------------------------------

MIGRATION = "Recompution required"


def sweep_dialogs():
    app = QtWidgets.QApplication.instance()
    boxes = []
    modal = app.activeModalWidget()
    if modal is not None:
        boxes.append(modal)
    for w in app.topLevelWidgets():
        if w.isVisible() and isinstance(w, QtWidgets.QMessageBox) and w not in boxes:
            boxes.append(w)
    for box in boxes:
        title = box.windowTitle()
        text = box.text() if isinstance(box, QtWidgets.QMessageBox) else ""
        state["dialogs"].append((state["phase"], type(box).__name__, title, text))
        if title == MIGRATION and isinstance(box, QtWidgets.QMessageBox):
            if state["phase"] == "reopen":
                # A copy this build just saved is no legacy file
                finding("dialog", "the migration asked again of the saved copy", text)
            else:
                note("INFO the migration asked in %s, answered Yes" % state["phase"])
            yes = box.button(QtWidgets.QMessageBox.Yes)
            if yes is not None:
                yes.click()
                continue
        finding("dialog", "%s %r" % (type(box).__name__, title), text)
        if isinstance(box, QtWidgets.QDialog):
            box.reject()
        else:
            box.close()


# --- waiting ----------------------------------------------------------------

def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def busy(doc):
    if doc.Restoring or doc.LiveImport or doc.Recomputing:
        return True
    try:
        return FreeCAD.workerPool(doc).recomputing()
    except Exception:  # pylint: disable=broad-except
        return False


def settle(doc, what):
    end = time.time() + WAIT
    while time.time() < end:
        pump(0.1)
        if not busy(doc):
            pump(0.5)
            if not busy(doc):
                return True
    finding("end", "%s not over after %d s" % (what, WAIT))
    return False


# --- snapshots --------------------------------------------------------------

def fingerprint(shape):
    try:
        if shape.isNull():
            return ("null",)
        bb = shape.BoundBox
        return (shape.ShapeType, len(shape.Solids), len(shape.Faces), len(shape.Edges),
                len(shape.Vertexes), num(shape.Volume), num(shape.Area), num(shape.Length),
                tuple(num(v) for v in (bb.XMin, bb.YMin, bb.ZMin, bb.XMax, bb.YMax, bb.ZMax)))
    except Exception as e:  # pylint: disable=broad-except
        return ("unreadable", str(e)[:80])


def num(v):
    if v != v:
        return v
    if abs(v) < 1e-9:  # a rounding's residue: -1.7e-14 and 0 are one coordinate
        return 0.0
    return float("%.6g" % v)


def norm(value, depth=0):
    """A value as plain data to compare: no addresses, rounded floats,
    objects by name, shapes by fingerprint"""
    if depth > 4:
        return "..."
    if isinstance(value, bool) or value is None or isinstance(value, int):
        return value
    if isinstance(value, float):
        return num(value)
    if isinstance(value, str):
        return value
    tid = getattr(value, "TypeId", None)
    if isinstance(value, FreeCAD.DocumentObject):
        return "obj:" + (value.Name if value.isAttachedToDocument() else "?")
    if hasattr(value, "ShapeType") and hasattr(value, "isNull"):
        return ("shape",) + fingerprint(value)
    if isinstance(value, FreeCAD.Placement):
        return ("placement",) + tuple(num(x) for x in value.Base) + tuple(
            num(x) for x in value.Rotation.Q)
    if isinstance(value, FreeCAD.Vector):
        return ("vector", num(value.x), num(value.y), num(value.z))
    if isinstance(value, FreeCAD.Rotation):
        return ("rotation",) + tuple(num(x) for x in value.Q)
    if isinstance(value, FreeCAD.Units.Quantity):
        return ("quantity", num(value.Value), str(value.Unit))
    if isinstance(value, (list, tuple)):
        if len(value) > 200:
            return ("long", len(value), digest(repr([norm(v, depth + 1) for v in value])))
        return [norm(v, depth + 1) for v in value]
    if isinstance(value, dict):
        return {str(k): norm(v, depth + 1) for k, v in sorted(value.items(), key=lambda kv: str(kv[0]))}
    text = re.sub(r" at 0x[0-9a-fA-F]+", "", repr(value))
    if tid:
        text = tid + ":" + text
    return text if len(text) < 300 else ("long", len(text), digest(text))


def digest(text):
    return hashlib.md5(text.encode("utf-8", "replace")).hexdigest()[:12]


SKIP_TYPES = ("App::PropertyPythonObject",)
# A cache, made again when wanted, not saved
SKIP_NAMES = {"_Part_ShapeCache"}


FILE_TYPES = ("App::PropertyFileIncluded", "App::PropertyFile", "App::PropertyPath")


def byte_colours(value):
    if isinstance(value, float):
        # as the save packs it: in single precision, half up (0.7 is 179)
        f32 = struct.unpack("f", struct.pack("f", value))[0]
        return int(struct.unpack("f", struct.pack("f", f32 * 255))[0] + 0.5) / 255.0
    if isinstance(value, (list, tuple)):
        return [byte_colours(v) for v in value]
    return value


def transient(holder, name):
    """Not saved by design, so neither a reopen nor a worker has to give it:
    an edit preview's shapes, a binder's copy, a legacy name's value"""
    try:
        if "Transient" in holder.getTypeOfProperty(name):
            return True
        # and Legacy: a compatibility name for what is kept elsewhere
        # (_ObjectUUID, added when an older file is read), never saved
        return any(s in ("Transient", "PropTransient", "Legacy", 19)
                   for s in holder.getPropertyStatus(name))
    except Exception:  # pylint: disable=broad-except
        return False


def props(holder):
    out = {}
    for name in holder.PropertiesList:
        try:
            if name in SKIP_NAMES or holder.getTypeIdOfProperty(name) in SKIP_TYPES:
                continue
            if transient(holder, name):
                continue
            kind = holder.getTypeIdOfProperty(name)
            value = holder.getPropertyByName(name)
            if "Color" in kind:
                # As a save keeps it: a channel in 8 bits (0.7 is 0.701961 after it)
                value = byte_colours(value)
            elif kind in FILE_TYPES and isinstance(value, str) and os.path.isfile(value):
                # A file the document carries: its path is in this session's
                # transient folder, a new one each open, so its content
                with open(value, "rb") as f:
                    value = "file:%s" % hashlib.md5(f.read()).hexdigest()[:12]
            out[name] = norm(value)
        except Exception as e:  # pylint: disable=broad-except
            out[name] = ("unreadable", str(e)[:80])
    return out


def snapshot(doc, views=True):
    gdoc = FreeCADGui.getDocument(doc.Name)
    view = None
    try:
        view = gdoc.activeView() if gdoc else None
    except Exception:  # pylint: disable=broad-except
        view = None
    rows = {}
    for o in doc.Objects:
        row = {"type": o.TypeId,
               "state": sorted(s for s in o.State if s in ("Touched", "Invalid", "Error")),
               "props": props(o)}
        vp = o.ViewObject if gdoc else None
        if vp is not None:
            census = [bool(o.Visibility), bool(vp.Visibility)]
            try:
                census.append(bool(vp.isVisible()))
            except Exception:  # pylint: disable=broad-except
                census.append("?")
            census.append(getattr(vp, "DisplayMode", None))
            try:
                census.append(view.getObjectVisibility(o) if view else None)
            except Exception:  # pylint: disable=broad-except
                census.append("?")
            row["census"] = census
            if views:
                row["view"] = props(vp)
        rows[o.Name] = row
    return rows


# Properties a recompute or a save is to change, or which name a process
VOLATILE = {"MadeBy"}
# Differences named a compare; the rest are counted
LIMIT = 150


def compare(check, a, b, what_a, what_b, keys=("type", "state", "props", "census", "view")):
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    if only_a:
        finding(check, "objects only %s" % what_a, only_a[:20])
    if only_b:
        finding(check, "objects only %s" % what_b, only_b[:20])
    differ = 0
    for name in sorted(set(a) & set(b)):
        ra, rb = a[name], b[name]
        for key in keys:
            va, vb = ra.get(key), rb.get(key)
            if key in ("props", "view") and va is not None and vb is not None:
                for p in sorted(set(va) | set(vb)):
                    if p in VOLATILE:
                        continue
                    if va.get(p, "<none>") != vb.get(p, "<none>"):
                        differ += 1
                        if differ <= LIMIT:
                            finding(check, "%s.%s%s differs" % (name, "ViewObject." if key == "view" else "", p),
                                    "%s %s ; %s %s" % (what_a, short(va.get(p, "<none>")),
                                                       what_b, short(vb.get(p, "<none>"))))
            elif va != vb:
                differ += 1
                if differ <= LIMIT:
                    finding(check, "%s %s differs" % (name, key),
                            "%s %s ; %s %s" % (what_a, short(va), what_b, short(vb)))
    if differ > LIMIT:
        finding(check, "and %d more differences" % (differ - LIMIT))
    return differ


def short(v):
    text = json.dumps(v, default=str)
    return text if len(text) < 400 else text[:400] + "..."


def touch_all(doc):
    for o in doc.Objects:
        try:
            o.touch()
        except Exception:  # pylint: disable=broad-except
            pass


# --- the loop ---------------------------------------------------------------

def run():
    state["t0"] = time.time()
    sweeper = QtCore.QTimer()
    sweeper.timeout.connect(sweep_dialogs)
    sweeper.start(100)
    FreeCAD.Console.AttachObserver(listen)
    doc = None
    try:
        if not SOURCE:
            note("INFO no document: the empty session")
            return
        work = os.path.join(OUT, "work")
        os.makedirs(work, exist_ok=True)
        path = os.path.join(work, os.path.basename(SOURCE))
        shutil.copyfile(SOURCE, path)
        # The documents beside it, which it may link to: a link to a file
        # the copy left behind is not the document's fault
        here_dir = os.path.dirname(SOURCE)
        siblings = [f for f in os.listdir(here_dir)
                    if f.lower().endswith(".fcstd") and f != os.path.basename(SOURCE)]
        total = sum(os.path.getsize(os.path.join(here_dir, f)) for f in siblings)
        if siblings and total < 200 * 1024 * 1024:
            for f in siblings:
                shutil.copyfile(os.path.join(here_dir, f), os.path.join(work, f))
        elif siblings:
            note("INFO %d documents beside it not copied (%d MB)" % (len(siblings), total >> 20))
        note("INFO document %s (%d bytes)" % (SOURCE, os.path.getsize(SOURCE)))

        phase("open")
        before = set(FreeCAD.listDocuments())
        FreeCADGui.open(path)
        opened = [d for n, d in FreeCAD.listDocuments().items() if n not in before]
        mine = [d for d in opened if os.path.normpath(d.FileName) == os.path.normpath(path)]
        if not mine:
            finding("open", "the document did not open", sorted(FreeCAD.listDocuments()))
            return
        doc = mine[0]
        if len(opened) > 1:
            note("INFO it opened others too: %s" % sorted(d.Name for d in opened if d is not doc))
        settle(doc, "the open")
        pump(1.0)
        note("INFO %d objects, schema %s" % (len(doc.Objects), doc.getProgramVersion()
                                             if hasattr(doc, "getProgramVersion") else "?"))
        broken = sorted(o.Name for o in doc.Objects if "Invalid" in o.State or "Error" in o.State)
        if broken:
            finding("log", "objects in error after the open", broken[:30])

        if os.environ.get("GT_SMOKE_SKIP_TWIN") != "1":
            phase("worker")
            touch_all(doc)
            pool = FreeCAD.workerPool(doc)
            if pool.recompute():
                settle(doc, "the worker's recompute")
                info = pool.recomputeInfo()
                note("INFO worker: applied %s, made here %s, failed %s, unfit %s, %.1f s"
                     % (info["Applied"], info["ExecutedHere"], info["Failed"], info["Unfit"],
                        info["Times"].get("Total", 0)))
                if info["Failed"] or info["Unfit"]:
                    finding("twin", "the worker's recompute failed or was unfit", info["Reason"])
                for f in info["Faked"]:
                    finding("twin", "%s made here: %s used %s" % (f["Object"], f["Class"], f["Used"]),
                            "built-in" if f["BuiltIn"] else "an addon's")
                # A worker that put nothing in has no twin: what it would be
                # compared with is the document as opened, not recomputed
                # (604 differences on a document whose worker was unfit)
                twin = not (info["Failed"] or info["Unfit"]) and info["Applied"] > 0
                by_worker = snapshot(doc, views=False) if twin else None

                phase("here")
                touch_all(doc)
                doc.recompute()
                settle(doc, "the recompute here")
                if twin:
                    here = snapshot(doc, views=False)
                    n = compare("twin", by_worker, here, "worker", "here",
                                keys=("type", "state", "props", "census"))
                    note("INFO worker against here: %d differences" % n)
                else:
                    note("INFO worker against here: not compared, the worker put nothing in")
            else:
                note("INFO not recomputed by a worker: %s" % pool.recomputeInfo()["Reason"])
                phase("here")
                touch_all(doc)
                doc.recompute()
                settle(doc, "the recompute here")
        else:
            phase("here")
            touch_all(doc)
            doc.recompute()
            settle(doc, "the recompute here")

        phase("save")
        saved = os.path.join(work, "saved-" + os.path.basename(path))
        doc.saveAs(saved)
        pump(0.5)
        live = snapshot(doc)
        name = doc.Name
        FreeCAD.closeDocument(name)
        doc = None
        pump(0.5)

        phase("reopen")
        before = set(FreeCAD.listDocuments())
        FreeCADGui.open(saved)
        again = [d for n, d in FreeCAD.listDocuments().items() if n not in before
                 and os.path.normpath(d.FileName) == os.path.normpath(saved)]
        if not again:
            finding("open", "the saved copy did not open")
            return
        doc = again[0]
        settle(doc, "the reopen")
        pump(1.0)
        reopened = snapshot(doc)
        n = compare("reopen", live, reopened, "live", "reopened")
        note("INFO live against reopened: %d differences" % n)
    except Exception:  # pylint: disable=broad-except
        note("ABORT " + traceback.format_exc())
    finally:
        phase("close")
        for n in list(FreeCAD.listDocuments().keys()):
            try:
                FreeCAD.closeDocument(n)
            except Exception as e:  # pylint: disable=broad-except
                finding("end", "closing %s raised" % n, e)
        pump(0.5)
        left = sorted(FreeCAD.listDocuments())
        if left:
            finding("end", "documents left open", left)
        sweeper.stop()
        FreeCAD.Console.DetachObserver(listen)
        flush_log()
        note("INFO %d findings" % state["findings"])
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
