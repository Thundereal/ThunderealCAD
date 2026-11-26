# SPDX-License-Identifier: LGPL-2.1-or-later
"""Every built-in object is recomputed by a worker (docs/WorkerProcess.md
sec 73).

The reporter, 2026-11-26: "fix the code to make any built-in objects to
recompute in worker, the fallback is only for external addon code". A
worker leaves to the asking process what used FreeCADGui or a view object
it only stands in for (sec 72); for the install's own code that is a fault.

The sweep opens every document the install has (its data/examples and the
modules' test fixtures, FCStd files under App.getHomePath()), touches every
object, and has a worker recompute it with Workers/NoteGuiUp on -- a read
of FreeCAD.GuiUp inside an execute counts too, as an execute that skips
work without a GUI sends a result without it. A built-in object the worker
left to be made here, or a job taken back for one, is a fault, named with
its document, what it used and where.

The GUI's sweep is tests/gui/builtins-by-worker.py, which runs this one's
functions where a worker's copy has the views as the GUI saves them; this
module's test is the console's. Not in the default list: it takes
minutes. Run it by name:
    ThunderealCmd -t TestWorkerBuiltIns
WORKER_SWEEP_GLOB narrows it ("*/examples/*.FCStd"), WORKER_SWEEP_SKIP leaves
out what its patterns match (";" between them); WORKER_SWEEP_OUT names a
JSON file for the whole report.
"""

import fnmatch
import glob
import json
import os
import time
import unittest

import FreeCAD as App

WORKERS = "User parameter:BaseApp/Preferences/Workers"


def documents():
    """Every FCStd the install has, narrowed by WORKER_SWEEP_GLOB"""
    home = App.getHomePath()
    pattern = os.environ.get("WORKER_SWEEP_GLOB", "")
    found = []
    for ext in ("FCStd", "fcstd"):
        found += glob.glob(os.path.join(home, "**", "*." + ext), recursive=True)
    found = sorted(set(os.path.normpath(p) for p in found))
    if pattern:
        found = [p for p in found if fnmatch.fnmatch(p.replace("\\", "/"), pattern)]
    skip = [s for s in os.environ.get("WORKER_SWEEP_SKIP", "").split(";") if s]
    return [p for p in found
            if not any(fnmatch.fnmatch(p.replace("\\", "/"), s) for s in skip)]


def _wait(pool, seconds):
    return pool.waitRecompute(seconds)


def sweep_one(path, wait=_wait):
    """One document opened, everything touched, recomputed by a worker:
    what came of it"""
    entry = {"file": path}
    try:
        doc = App.openDocument(path)
    except Exception as e:  # pylint: disable=broad-except
        entry["skipped"] = "cannot open: %s" % e
        return entry
    try:
        for obj in doc.Objects:
            obj.touch()
        pool = App.workerPool(doc)
        began = time.time()
        if not pool.recompute():
            entry["skipped"] = pool.recomputeInfo()["Reason"]
            return entry
        if not wait(pool, 600):
            entry["skipped"] = "not done in 600 s"
            return entry
        info = pool.recomputeInfo()
        entry["seconds"] = round(time.time() - began, 2)
        entry["objects"] = len(doc.Objects)
        entry["applied"] = info["Applied"]
        entry["unfit"] = info["Unfit"]
        entry["failed"] = info["Failed"]
        entry["reason"] = info["Reason"]
        entry["here"] = info["ExecutedHereNames"]
        entry["faked"] = info["Faked"]
        pool.stop("")
    finally:
        App.closeDocument(doc.Name)
    return entry


def faults(report):
    """Each built-in object made here or job taken back, as a line"""
    bad = []
    for e in report:
        for f in e.get("faked", []):
            if f["BuiltIn"]:
                bad.append("%s: %s (%s) used %s" % (os.path.basename(e["file"]),
                                                   f["Object"] or "the job", f["Class"],
                                                   f["Used"]))
        if e.get("failed"):
            bad.append("%s: failed: %s" % (os.path.basename(e["file"]), e.get("reason")))
    return bad


def sweep(paths, wait=_wait, each=None):
    """Workers/NoteGuiUp on, one worker, every document of `paths`: the
    report, kept as WORKER_SWEEP_OUT says. each(path) is told of each first"""
    group = App.ParamGet(WORKERS)
    had = group.GetBool("NoteGuiUp", False) if "NoteGuiUp" in group.GetBools() else None
    workers = group.GetInt("Workers", -1) if "Workers" in group.GetInts() else None
    group.SetBool("NoteGuiUp", True)
    group.SetInt("Workers", 1)
    try:
        report = []
        for p in paths:
            if each is not None:
                each(p)
            report.append(sweep_one(p, wait))
    finally:
        if had is None:
            group.RemBool("NoteGuiUp")
        else:
            group.SetBool("NoteGuiUp", had)
        if workers is None:
            group.RemInt("Workers")
        else:
            group.SetInt("Workers", workers)
    out = os.environ.get("WORKER_SWEEP_OUT")
    if out:
        with open(out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
    return report


class TestWorkerBuiltIns(unittest.TestCase):
    def testEveryBuiltInObjectIsMadeByAWorker(self):
        # This process imports as a worker does: a module that calls
        # FreeCADGui at import restores here too, so its objects are in
        # the pool's list of objects with a class
        from freecad import worker_gui

        worker_gui.install()
        report = sweep(documents())
        bad = faults(report)
        swept = sum(1 for e in report if "skipped" not in e)
        App.Console.PrintMessage("TestWorkerBuiltIns: %d documents swept, %d skipped, %d faults\n"
                                 % (swept, len(report) - swept, len(bad)))
        self.assertGreater(swept, 0, report)
        self.assertEqual(bad, [], "\n".join(bad))
