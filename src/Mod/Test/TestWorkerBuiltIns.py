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
left to be made here, or a job taken back for one, fails the test, named
with its document and what it used.

Not in the default list: it takes minutes. Run it by name:
    ThunderealCmd -t TestWorkerBuiltIns
WORKER_SWEEP_GLOB narrows it ("*/examples/*.FCStd"); WORKER_SWEEP_OUT names
a JSON file for the whole report.
"""

import fnmatch
import glob
import json
import os
import time
import unittest

import FreeCAD as App


def _documents():
    home = App.getHomePath()
    pattern = os.environ.get("WORKER_SWEEP_GLOB", "")
    found = []
    for ext in ("FCStd", "fcstd"):
        found += glob.glob(os.path.join(home, "**", "*." + ext), recursive=True)
    found = sorted(set(os.path.normpath(p) for p in found))
    if pattern:
        found = [p for p in found if fnmatch.fnmatch(p.replace("\\", "/"), pattern)]
    return found


class TestWorkerBuiltIns(unittest.TestCase):
    def setUp(self):
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        self.group = group
        for name, value in (("NoteGuiUp", True),):
            if name in group.GetBools():
                self.addCleanup(group.SetBool, name, group.GetBool(name))
            else:
                self.addCleanup(group.RemBool, name)
            group.SetBool(name, value)
        group.SetInt("Workers", 1)
        self.addCleanup(group.RemInt, "Workers")
        # This process imports as a worker does: a module that calls
        # FreeCADGui at import restores here too, so its objects are in
        # the pool's list of objects with a class
        from freecad import worker_gui

        worker_gui.install()

    def sweepOne(self, path):
        entry = {"file": path}
        try:
            doc = App.openDocument(path, True)
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
            if not pool.waitRecompute(600):
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

    def testEveryBuiltInObjectIsMadeByAWorker(self):
        report = [self.sweepOne(p) for p in _documents()]
        out = os.environ.get("WORKER_SWEEP_OUT")
        if out:
            with open(out, "w", encoding="utf-8") as f:
                json.dump(report, f, indent=1)
        bad = []
        for e in report:
            for f in e.get("faked", []):
                if f["BuiltIn"]:
                    bad.append("%s: %s (%s) used %s" % (os.path.basename(e["file"]),
                                                       f["Object"] or "the job", f["Class"],
                                                       f["Used"]))
        swept = sum(1 for e in report if "skipped" not in e)
        App.Console.PrintMessage("TestWorkerBuiltIns: %d documents swept, %d skipped, %d faults\n"
                                 % (swept, len(report) - swept, len(bad)))
        self.assertGreater(swept, 0, report)
        self.assertEqual(bad, [], "\n".join(bad))
